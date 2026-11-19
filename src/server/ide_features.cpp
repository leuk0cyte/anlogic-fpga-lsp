// =============================================================================
//  src/server/ide_features.cpp —— IDE 类命令的纯逻辑实现（唯一实现）
// =============================================================================
//  对应 Python：
//    * Y:\fpga-lsp\fpgalsp\ide.py            （symbol_path / duplicate_modules /
//                                              search_hierarchy / add_source_file /
//                                              set_project_name）
//    * Y:\fpga-lsp\fpgalsp\server.py         （_instantiation_text / modules_in_file /
//                                              on_workspace_executeCommand 的 fpgalsp.* 分支）
//
//  ⚠ 契约（raw.json commands[6..14] 是唯一权威）：
//      instantiate       -> {ok, module, instanceName, text, portCount}   module 为空时 {ok, modules}
//      copyInstantiation -> {ok, module, text, portCount}                 （没有 instanceName！）
//      symbolPath        -> {ok, name, path, instancePath, module}
//      duplicateModules  -> {ok, duplicates:[{name, definitions:[{file,line,end_line}]}]}
//      searchHierarchy   -> {ok, matches:[{kind,name,file,line,module,instance[,parent]}]}
//      addSourceFile     -> {ok, added, rel, compileOrder}
//      setProjectName    -> {ok, changed, name}
//    不要"顺手"加字段：字段级契约不一致会被 probe_lsp 判成回归。
//
//  与 Python 的两处**有意差异**（都只在失败/字节层面，字段不受影响）：
//    1. 写 .al 走"临时文件 + 替换 + 回滚"：Python 直接覆盖，磁盘满/被杀会留下半截文件。
//    2. 写 .al 保留原文件行尾：Python 的 `open(..., newline="")` 会把 CRLF 全文改成 LF
//       （对用户工程是看不见的整文件改写）。C++ 只插入新块，并沿用文件现有行尾。
// =============================================================================

#include "server/ide_features.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <re2/re2.h>

#include "platform/encoding.h"
#include "platform/path.h"

namespace fpgalsp::server::ide {
namespace {

namespace fs = std::filesystem;

using platform::normalize_path;
using platform::path_dirname;
using platform::path_join;

std::string LowerAscii(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::string TrimText(std::string_view text) {
    const auto is_space = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
    };
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && is_space(text[begin])) ++begin;
    while (end > begin && is_space(text[end - 1])) --end;
    return std::string(text.substr(begin, end - begin));
}

std::string NormaliseSlashes(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c == '\\') c = '/';
    }
    return out;
}

std::string ToUtf8(const fs::path& path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

// UTF-8 字节串 -> fs::path。不用 fs::u8path（C++20 起被标记 deprecated，会触发 C4996；
// fs::path(std::string) 在 Windows 上按 ANSI 解释，中文路径会坏）。
fs::path ToPath(std::string_view utf8) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

// 把 "\n" 换成文件里实际使用的行尾（.al 在 Windows 上是 CRLF）
std::string ApplyLineEnding(std::string block, std::string_view text) {
    if (text.find("\r\n") == std::string_view::npos) return block;
    std::string out;
    out.reserve(block.size() + block.size() / 8);
    for (const char c : block) {
        if (c == '\n') out += "\r\n";
        else out.push_back(c);
    }
    return out;
}

Json Failure(std::string message) {
    Json out = Json::object();
    out["ok"] = false;
    out["error"] = std::move(message);
    return out;
}

// Python ide.module_at + index.modules 的遍历顺序：
//   modules 是 dict{name: [module...]}，插入顺序 = 模块名首次出现的顺序。
//   C++ 的 HdlIndex 用 modules_by_key_ 去重，ModuleNames() 正是这个"首次出现"顺序，
//   所以按名字分组、再按文件顺序取同名模块，才能和 Python 的遍历顺序一致。
std::vector<const hdl::Module*> ModulesInIndexOrder(const hdl::HdlIndex& index) {
    const std::vector<std::string> paths = index.Paths();
    std::vector<const hdl::Module*> out;
    for (const std::string& name : index.ModuleNames()) {
        const std::string lowered = LowerAscii(name);
        for (const std::string& path : paths) {
            const hdl::ParsedFile* file = index.File(path);
            if (file == nullptr) continue;
            for (const hdl::ModuleInfo& info : file->modules) {
                if (LowerAscii(info.module.name) == lowered) out.push_back(&info.module);
            }
        }
    }
    return out;
}

// Python ide.module_parents：模块名 -> [(父模块, 实例名)]
std::map<std::string, std::vector<std::pair<const hdl::ModuleInfo*, std::string>>> ModuleParents(
    const hdl::HdlIndex& index) {
    std::map<std::string, std::vector<std::pair<const hdl::ModuleInfo*, std::string>>> parents;
    for (const hdl::Instance* instance : index.AllInstances()) {
        const hdl::ParsedFile* file = index.File(instance->file);
        if (file == nullptr) continue;
        const hdl::ModuleInfo* info = hdl::HdlIndex::ModuleAt(*file, instance->line);
        if (info == nullptr) continue;
        parents[instance->module].emplace_back(info, instance->name);
    }
    return parents;
}

}  // namespace

// =============================================================================
//  Python server.py::_instantiation_text
// =============================================================================

InstantiationText BuildInstantiationText(const hdl::Module& module, std::string_view indent) {
    const std::string pad(indent);
    const std::string name = module.name.empty() ? "module" : module.name;

    InstantiationText out;
    out.instance = "u_" + name;

    std::string body;
    if (!module.parameters.empty()) {
        body += name + " #(\n";
        for (std::size_t i = 0; i < module.parameters.size(); ++i) {
            const std::string& parameter = module.parameters[i];
            const bool last = (i + 1 == module.parameters.size());
            body += pad + "    ." + parameter + " (" + parameter + ")" + (last ? "" : ",") + "\n";
        }
        body += pad + ") " + out.instance + " (\n";
    } else {
        body += name + " " + out.instance + " (\n";
    }
    for (std::size_t i = 0; i < module.ports.size(); ++i) {
        const std::string& port_name = module.ports[i].name;
        const bool last = (i + 1 == module.ports.size());
        body += pad + "    ." + port_name + " (" + port_name + ")" + (last ? "" : ",") + "\n";
    }
    body += pad + ");\n";

    // Python: indent + "\n".join(lines) + "\n"
    out.text = pad + body;
    return out;
}

Json Instantiation(const hdl::HdlIndex& index, std::string_view module_name,
                   std::string_view indent) {
    if (module_name.empty()) {
        std::vector<std::string> names = index.ModuleNames();
        std::sort(names.begin(), names.end());
        Json out = Json::object();
        out["ok"] = true;
        out["modules"] = std::move(names);
        return out;
    }

    const hdl::Module* module = index.ModuleByName(module_name);
    if (module == nullptr) {
        return Failure("Module not found: " + std::string(module_name));
    }

    const InstantiationText built = BuildInstantiationText(*module, indent);
    Json out = Json::object();
    out["ok"] = true;
    out["module"] = std::string(module_name);  // Python 回的是 options 里的名字
    out["instanceName"] = built.instance;
    out["text"] = built.text;
    out["portCount"] = static_cast<int>(module->ports.size());
    return out;
}

// =============================================================================
//  Python ide.modules_in_file + on_workspace_executeCommand::copyInstantiation
// =============================================================================

Json CopyInstantiation(const hdl::HdlIndex& index, std::string_view path) {
    const hdl::ParsedFile* file = index.File(path);
    const hdl::Module* first = nullptr;
    if (file != nullptr) {
        for (const hdl::ModuleInfo& info : file->modules) {
            if (first == nullptr || info.module.line < first->line) first = &info.module;
        }
    }
    if (first == nullptr) {
        return Failure("The current file has no module definition");
    }

    const InstantiationText built = BuildInstantiationText(*first, "    ");
    Json out = Json::object();
    out["ok"] = true;
    out["module"] = first->name;
    out["text"] = built.text;
    out["portCount"] = static_cast<int>(first->ports.size());
    return out;
}

// =============================================================================
//  Python ide.symbol_path
// =============================================================================

Json SymbolPath(const hdl::HdlIndex& index, const project::Project& project,
                std::string_view path, int line, int character) {
    std::string name;
    if (const std::optional<hdl::SymbolAtResult> symbol = index.SymbolAt(path, line, character)) {
        name = symbol->name;
    }
    if (name.empty()) {
        // 对应 Python refs.token_span 的兜底：光标处 token 的文本
        if (const hdl::ParsedFile* file = index.File(path)) {
            if (const hdl::Token* token = hdl::HdlIndex::TokenAt(*file, line, character)) {
                name = token->text;
            }
        }
    }
    if (name.empty()) {
        return Failure("No copyable symbol under the cursor");
    }

    const hdl::ParsedFile* file = index.File(path);
    const hdl::ModuleInfo* module = file != nullptr ? hdl::HdlIndex::ModuleAt(*file, line) : nullptr;
    if (module == nullptr) {
        Json out = Json::object();
        out["ok"] = true;
        out["name"] = name;
        out["path"] = name;
        out["instancePath"] = "";
        out["module"] = "";
        return out;
    }

    const auto parents = ModuleParents(index);
    std::vector<std::string> chain;
    const hdl::ModuleInfo* current = module;
    std::set<std::string> visited;
    // Python: while current is not None and current.get("name") != (project.top or "")
    while (current != nullptr && (project.top.empty() || current->module.name != project.top)) {
        const std::string current_name = current->module.name;
        if (current_name.empty()) break;
        if (!visited.insert(current_name).second) break;
        const auto it = parents.find(current_name);
        if (it == parents.end()) break;
        const std::pair<const hdl::ModuleInfo*, std::string>* chosen = nullptr;
        for (const auto& candidate : it->second) {
            const std::string& parent_name = candidate.first->module.name;
            if (!parent_name.empty() && parent_name != current_name) {
                chosen = &candidate;
                break;
            }
        }
        if (chosen == nullptr) break;
        chain.insert(chain.begin(), chosen->second);
        current = chosen->first;
    }

    std::string instance_path;
    for (const std::string& segment : chain) {
        if (!instance_path.empty()) instance_path += "/";
        instance_path += segment;
    }
    const std::string full_path = instance_path.empty() ? name : instance_path + "/" + name;

    Json out = Json::object();
    out["ok"] = true;
    out["name"] = name;
    out["path"] = full_path;
    out["instancePath"] = instance_path;
    out["module"] = module->module.name;
    return out;
}

// =============================================================================
//  Python ide.duplicate_modules
// =============================================================================

Json DuplicateModules(const hdl::HdlIndex& index) {
    // 按名字分组（std::map -> 已按名字排序，等价 Python 的 sorted(groups.items())）
    std::map<std::string, std::vector<const hdl::Module*>> groups;
    for (const hdl::Module* module : ModulesInIndexOrder(index)) {
        groups[module->name].push_back(module);
    }

    Json duplicates = Json::array();
    for (const auto& [name, modules] : groups) {
        if (modules.size() < 2) continue;
        Json definitions = Json::array();
        for (const hdl::Module* module : modules) {
            Json item = Json::object();
            item["file"] = module->file;
            // ⚠ 0-based：Python 原样透传，不要改成 1-based
            item["line"] = module->line;
            item["end_line"] = module->end_line;
            definitions.push_back(std::move(item));
        }
        Json entry = Json::object();
        entry["name"] = name;
        entry["definitions"] = std::move(definitions);
        duplicates.push_back(std::move(entry));
    }

    Json out = Json::object();
    out["ok"] = true;
    out["duplicates"] = std::move(duplicates);
    return out;
}

// =============================================================================
//  Python ide.search_hierarchy
// =============================================================================

Json SearchHierarchy(const hdl::HdlIndex& index, std::string_view query, int limit) {
    Json out = Json::object();
    out["ok"] = true;

    Json matches = Json::array();
    const std::string needle = LowerAscii(query);
    if (needle.empty()) {
        out["matches"] = std::move(matches);
        return out;
    }

    const int bounded = std::clamp(limit, 1, 2000);
    std::set<std::string> seen;
    const auto add = [&](Json entry) {
        const std::string key = entry.value("kind", std::string()) + '\x1f' +
                                entry.value("name", std::string()) + '\x1f' +
                                entry.value("file", std::string()) + '\x1f' +
                                std::to_string(entry.value("line", 0));
        if (!seen.insert(key).second) return;
        matches.push_back(std::move(entry));
    };

    // 模块名命中（Python 不在这里截断，最后统一 results[:limit]）
    for (const hdl::Module* module : ModulesInIndexOrder(index)) {
        if (LowerAscii(module->name).find(needle) == std::string::npos) continue;
        Json item = Json::object();
        item["kind"] = "module";
        item["name"] = module->name;
        item["file"] = module->file;
        item["line"] = module->line;  // 0-based，同 Python
        item["module"] = module->name;
        item["instance"] = "";
        add(std::move(item));
    }

    // 实例名 / 被例化模块名命中
    for (const hdl::Instance* instance : index.AllInstances()) {
        if (LowerAscii(instance->name).find(needle) == std::string::npos &&
            LowerAscii(instance->module).find(needle) == std::string::npos) {
            continue;
        }
        std::string parent;
        if (const hdl::ParsedFile* file = index.File(instance->file)) {
            if (const hdl::ModuleInfo* info = hdl::HdlIndex::ModuleAt(*file, instance->line)) {
                parent = info->module.name;
            }
        }
        Json item = Json::object();
        item["kind"] = "instance";
        item["name"] = instance->name;
        item["file"] = instance->file;
        item["line"] = instance->line;  // 0-based，同 Python
        item["module"] = instance->module;
        item["instance"] = instance->name;
        item["parent"] = parent;
        add(std::move(item));
        if (static_cast<int>(matches.size()) >= bounded) break;
    }

    if (static_cast<int>(matches.size()) > bounded) {
        Json trimmed = Json::array();
        int count = 0;
        for (Json& item : matches) {
            if (count++ >= bounded) break;
            trimmed.push_back(std::move(item));
        }
        matches = std::move(trimmed);
    }

    out["matches"] = std::move(matches);
    return out;
}

// =============================================================================
//  原子写（失败保持原文件不变）
// =============================================================================

bool WriteTextFileAtomic(const std::string& path, std::string_view content) {
    const std::string temp = path + ".fpgalsp-tmp";
    if (!platform::write_file_utf8(temp, content)) return false;

    std::error_code ec;
    fs::rename(ToPath(temp), ToPath(path), ec);
    if (!ec) return true;

    // Windows 上"rename 到已存在的目标"可能直接失败：先把原文件挪开再替换，
    // 第二次 rename 也失败时把原文件放回去（保证"失败 = 原文件仍可用"）。
    const std::string backup = path + ".fpgalsp-bak";
    std::error_code ignored;
    fs::remove(ToPath(backup), ignored);
    fs::rename(ToPath(path), ToPath(backup), ec);
    if (ec) {
        fs::remove(ToPath(temp), ignored);
        return false;
    }
    fs::rename(ToPath(temp), ToPath(path), ec);
    if (ec) {
        std::error_code restore_ec;
        fs::rename(ToPath(backup), ToPath(path), restore_ec);
        fs::remove(ToPath(temp), ignored);
        return false;
    }
    fs::remove(ToPath(backup), ignored);
    return true;
}

std::string ProjectRelativePath(const std::string& path, const std::string& project_dir) {
    std::error_code ec;
    const fs::path relative =
        fs::relative(ToPath(NormaliseSlashes(path)), ToPath(NormaliseSlashes(project_dir)),
                     ec);
    if (ec || relative.empty()) return NormaliseSlashes(normalize_path(path));
    return NormaliseSlashes(ToUtf8(relative));
}

// =============================================================================
//  Python ide.add_source_file（写 <Source_Files>）
// =============================================================================

Json AddSourceFile(std::string_view al_path, std::string_view source_path) {
    const std::string al = NormaliseSlashes(normalize_path(al_path));
    const std::string project_dir = path_dirname(al);

    std::string source = NormaliseSlashes(normalize_path(source_path));
    if (source.empty()) {
        return Failure("the file path is empty");
    }
    if (!platform::is_absolute_path(source)) source = path_join(project_dir, source);

    const std::string rel = ProjectRelativePath(source, project_dir);
    const std::string rel_lower = LowerAscii(rel);

    const platform::ReadFileResult read = platform::read_file_utf8(al);
    if (read.content.empty()) return Failure("Cannot read " + al);
    const std::string original = read.content;

    // 定位 <Source_Files> ... </Source_Files>（Python: re.S 的等价写法）
    const std::string open_tag = "<Source_Files>";
    const std::string close_tag = "</Source_Files>";
    const std::string::size_type open_at = original.find(open_tag);
    const std::string::size_type close_at =
        open_at == std::string::npos ? std::string::npos
                                     : original.find(close_tag, open_at + open_tag.size());
    if (open_at == std::string::npos || close_at == std::string::npos) {
        return Failure("No <Source_Files> section in the .al");
    }
    const std::string body =
        original.substr(open_at + open_tag.size(), close_at - open_at - open_tag.size());

    // 已存在（大小写不敏感、斜杠归一）-> added:false
    {
        static const re2::RE2 file_path(R"re(<File\s+Path="([^"]+)")re");
        absl::string_view rest(body);
        re2::StringPiece captured;
        while (!rest.empty()) {
            re2::RE2::Arg arg(&captured);
            const re2::RE2::Arg* args[1] = {&arg};
            const std::size_t before = rest.size();
            if (!re2::RE2::FindAndConsumeN(&rest, file_path, args, 1)) break;
            if (rest.size() >= before) break;
            const std::string existing(captured.data(), captured.size());
            if (LowerAscii(NormaliseSlashes(existing)) == rel_lower) {
                Json out = Json::object();
                out["ok"] = true;
                out["added"] = false;
                out["rel"] = rel;
                out["compileOrder"] = Json(nullptr);
                return out;
            }
        }
    }

    // 下一个 CompileOrder = max + 1（Python: max(orders)+1 if orders else 1）
    int compile_order = 1;
    {
        static const re2::RE2 order_re(R"re(Name="CompileOrder"\s+Val="([0-9]+)")re");
        absl::string_view rest(body);
        re2::StringPiece captured;
        while (!rest.empty()) {
            re2::RE2::Arg arg(&captured);
            const re2::RE2::Arg* args[1] = {&arg};
            const std::size_t before = rest.size();
            if (!re2::RE2::FindAndConsumeN(&rest, order_re, args, 1)) break;
            if (rest.size() >= before) break;
            try {
                compile_order = std::max(
                    compile_order, std::stoi(std::string(captured.data(), captured.size())) + 1);
            } catch (...) {
                continue;
            }
        }
    }

    const std::string lowered_rel = LowerAscii(rel);
    const bool vhdl =
        (lowered_rel.size() >= 4 && lowered_rel.compare(lowered_rel.size() - 4, 4, ".vhd") == 0) ||
        (lowered_rel.size() >= 5 && lowered_rel.compare(lowered_rel.size() - 5, 5, ".vhdl") == 0);
    const std::string tag = vhdl ? "VHDL" : "Verilog";

    // 缩进：沿用同段里第一个 <Verilog>/<VHDL>/<ADC_FILE>/<SDC_FILE>/<IP> 的缩进，缺省 8 空格
    std::string indent = "        ";
    {
        static const re2::RE2 indent_re(R"(\n([ \t]+)<(?:Verilog|VHDL|ADC_FILE|SDC_FILE|IP)>)");
        std::string captured;
        if (re2::RE2::PartialMatch(body, indent_re, &captured)) indent = captured;
    }
    const std::string inner = indent + "    ";

    std::string block;
    block += indent + "<" + tag + ">\n";
    block += inner + "<File Path=\"" + rel + "\">\n";
    block += inner + "    <FileInfo>\n";
    block += inner + "        <Attr Name=\"UsedInSyn\" Val=\"true\"/>\n";
    block += inner + "        <Attr Name=\"UsedInP&R\" Val=\"true\"/>\n";
    block += inner + "        <Attr Name=\"BelongTo\" Val=\"design_1\"/>\n";
    block += inner + "        <Attr Name=\"CompileOrder\" Val=\"" + std::to_string(compile_order) +
             "\"/>\n";
    block += inner + "    </FileInfo>\n";
    block += inner + "</File>\n";
    block += indent + "</" + tag + ">\n";
    block = ApplyLineEnding(std::move(block), original);

    // 插入点：</Source_Files> 之前；若该行只有空白，则插到行首（保持闭合标签缩进）
    std::string::size_type insert_at = close_at;
    const std::string::size_type line_start =
        insert_at == 0 ? std::string::npos : original.rfind('\n', insert_at - 1);
    const std::string::size_type begin = (line_start == std::string::npos) ? 0 : line_start + 1;
    if (insert_at > begin && TrimText(original.substr(begin, insert_at - begin)).empty()) {
        insert_at = begin;
    }
    const std::string updated =
        original.substr(0, insert_at) + block + original.substr(insert_at);

    // "算-写-验-回滚"：写之前的解析结果作为基线（Python 没有验证，失败会留半改状态）
    const project::Project before = project::ParseAl(al);
    if (!WriteTextFileAtomic(al, updated)) return Failure("Cannot write " + al);

    const project::Project after = project::ParseAl(al);
    const std::string source_lower = LowerAscii(NormaliseSlashes(source));
    bool found = false;
    for (const project::AlFile& item : after.files) {
        if (LowerAscii(NormaliseSlashes(item.rel)) == rel_lower ||
            LowerAscii(NormaliseSlashes(item.path)) == source_lower) {
            found = true;
        }
    }
    if (!found || after.files.size() != before.files.size() + 1) {
        (void)WriteTextFileAtomic(al, original);  // 回滚
        return Failure("Verification failed: the new source is not in the .al (rolled back)");
    }

    Json out = Json::object();
    out["ok"] = true;
    out["added"] = true;
    out["rel"] = rel;
    out["compileOrder"] = compile_order;
    return out;
}

// =============================================================================
//  Python ide.set_project_name（写 <Name>）
// =============================================================================

Json SetProjectName(std::string_view al_path, std::string_view name_value) {
    const std::string name = TrimText(name_value);
    if (name.empty()) return Failure("Project name cannot be empty");
    if (name.find_first_of("<>\"&") != std::string::npos) {
        return Failure("Project name cannot contain < > \" &");
    }

    const std::string al = NormaliseSlashes(normalize_path(al_path));
    const platform::ReadFileResult read = platform::read_file_utf8(al);
    if (read.content.empty()) return Failure("Cannot read " + al);
    const std::string original = read.content;

    std::string updated;
    const std::string::size_type position = original.find("<Name>");
    bool replaced = false;
    if (position != std::string::npos) {
        const std::string::size_type close = original.find("</Name>", position);
        if (close != std::string::npos) {
            updated = original.substr(0, position) + "<Name>" + name + "</Name>" +
                      original.substr(close + 7);
            replaced = true;
        }
    }
    if (!replaced) {
        // Python: re.search(r"<Project\b[^>]*>", text) —— 手写等价物：
        // 找 "<Project" 后面紧跟"非单词字符"的那一个，再取它到最近的 '>'。
        // （不用 RE2 的 \b，避免与 vendored RE2 的边界语义纠缠）
        std::string::size_type tag_at = std::string::npos;
        std::string::size_type tag_end = std::string::npos;
        std::string::size_type search_from = 0;
        while (true) {
            const std::string::size_type at = original.find("<Project", search_from);
            if (at == std::string::npos) break;
            const std::string::size_type after = at + 8;
            const bool boundary =
                after >= original.size() ||
                !(std::isalnum(static_cast<unsigned char>(original[after])) ||
                  original[after] == '_');
            if (boundary) {
                const std::string::size_type close = original.find('>', after);
                if (close != std::string::npos) {
                    tag_at = at;
                    tag_end = close + 1;
                    break;
                }
            }
            search_from = at + 8;
        }
        if (tag_at == std::string::npos || tag_end == std::string::npos) {
            return Failure("No <Project> tag in the .al");
        }
        const std::string insert = ApplyLineEnding("\n    <Name>" + name + "</Name>", original);
        updated = original.substr(0, tag_end) + insert + original.substr(tag_end);
    }

    if (updated == original) {
        Json out = Json::object();
        out["ok"] = true;
        out["changed"] = false;
        out["name"] = name;
        return out;
    }

    const project::Project before = project::ParseAl(al);
    if (!WriteTextFileAtomic(al, updated)) return Failure("Cannot write " + al);

    // 验证：重新解析，只允许项目名变化（失败回滚，不留半改状态）
    const project::Project after = project::ParseAl(al);
    if (after.name != name || after.files.size() != before.files.size()) {
        (void)WriteTextFileAtomic(al, original);  // 回滚
        return Failure("Verification failed: the new project name is not in the .al (rolled back)");
    }

    Json out = Json::object();
    out["ok"] = true;
    out["changed"] = true;
    out["name"] = name;
    return out;
}

}  // namespace fpgalsp::server::ide