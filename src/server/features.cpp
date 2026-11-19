// =============================================================================
//  src/server/features.cpp —— index -> LSP JSON 的唯一实现
// =============================================================================
//  见 features.h 的分层说明。本文件**不持有状态、不碰传输、不读磁盘**。
// =============================================================================

#include "server/features.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "platform/path.h"
#include "hdl/primitives.h"
#include "hdl/primitives.h"

namespace fpgalsp::server::features {
namespace {

using hdl::DocSymbol;
using hdl::HdlIndex;
using hdl::Instance;
using hdl::Location;
using hdl::Module;
using hdl::Port;
using hdl::Span;
using hdl::SymbolAtResult;
using hdl::WorkspaceSymbol;

Json MakeRange(int start_line, int start_character, int end_line, int end_character) {
    Json range = Json::object();
    Json start = Json::object();
    start["line"] = start_line;
    start["character"] = start_character;
    Json end = Json::object();
    end["line"] = end_line;
    end["character"] = end_character;
    range["start"] = std::move(start);
    range["end"] = std::move(end);
    return range;
}

Json MakeSpan(const Span& span) {
    return MakeRange(span.line, span.start_character, span.line, span.end_character);
}

Json MakeLocation(const std::string& path, int line, int start_character, int end_character) {
    Json location = Json::object();
    location["uri"] = platform::path_to_uri(path);
    location["range"] = MakeRange(line, start_character, line, end_character);
    return location;
}

Json DocSymbolToJson(const DocSymbol& symbol) {
    Json out = Json::object();
    out["name"] = symbol.name;
    out["detail"] = symbol.detail;
    out["kind"] = symbol.kind;
    out["range"] = MakeRange(symbol.range_start_line, symbol.range_start_character,
                             symbol.range_end_line, symbol.range_end_character);
    out["selectionRange"] = MakeSpan(symbol.selection);
    Json children = Json::array();
    for (const DocSymbol& child : symbol.children) children.push_back(DocSymbolToJson(child));
    out["children"] = std::move(children);
    return out;
}

Json WorkspaceSymbolToJson(const WorkspaceSymbol& symbol) {
    Json out = Json::object();
    out["name"] = symbol.name;
    out["kind"] = symbol.kind;
    out["location"] = MakeLocation(symbol.path, symbol.span.line,
                                   symbol.span.start_character, symbol.span.end_character);
    if (!symbol.container.empty()) out["containerName"] = symbol.container;
    return out;
}

// -----------------------------------------------------------------------------
// completion 的行首判断（对应 server.py 的"行首只有空白/正在输入的标识符"）
// -----------------------------------------------------------------------------

bool IsIdentifierChar(char c) {
    const unsigned char uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) != 0 || c == '_' || c == '$';
}

std::string PrefixIdentifier(std::string_view line_text, int character) {
    const std::size_t limit = std::min<std::size_t>(
        line_text.size(), static_cast<std::size_t>(std::max(0, character)));
    std::size_t begin = limit;
    while (begin > 0 && IsIdentifierChar(line_text[begin - 1])) --begin;
    return std::string(line_text.substr(begin, limit - begin));
}

bool OnlyWhitespaceBefore(std::string_view line_text, int limit) {
    const std::size_t bounded = std::min<std::size_t>(
        line_text.size(), static_cast<std::size_t>(std::max(0, limit)));
    for (std::size_t i = 0; i < bounded; ++i) {
        if (line_text[i] != ' ' && line_text[i] != '\t') return false;
    }
    return true;
}

std::string IndentOf(std::string_view line_text, int character) {
    const std::size_t limit = std::min<std::size_t>(
        line_text.size(), static_cast<std::size_t>(std::max(0, character)));
    std::string indent;
    for (std::size_t i = 0; i < limit; ++i) {
        if (line_text[i] == ' ' || line_text[i] == '\t') {
            indent.push_back(line_text[i]);
        } else {
            break;
        }
    }
    return indent;
}

std::string InstantiationSnippet(const Module& module, const std::string& indent,
                                 const std::string& instance) {
    std::string out;
    out += indent + module.name + " " + instance + " (\n";
    int index = 1;
    for (const Port& port : module.ports) {
        out += indent + "  ." + port.name + "(" + "${" + std::to_string(index) + ":" +
               port.name + "}),\n";
        ++index;
    }
    if (!module.ports.empty()) {
        const std::string trailing = "),\n";
        if (out.size() >= trailing.size() &&
            out.compare(out.size() - trailing.size(), trailing.size(), trailing) == 0) {
            out.resize(out.size() - 2);  // 去掉最后的 ",\n"（保留 ')'）
            out += "\n";
        }
    }
    out += indent + ");";
    return out;
}

std::string UniqueInstanceName(const HdlIndex& index, const std::string& path,
                               const std::string& module_name) {
    const std::string base = "u_" + module_name;
    std::set<std::string> used;
    for (const Instance* instance : index.InstancesOfFile(path)) {
        if (!instance->name.empty()) used.insert(instance->name);
    }
    if (used.count(base) == 0) return base;
    for (int i = 1; i < 1000; ++i) {
        const std::string candidate = base + "_" + std::to_string(i);
        if (used.count(candidate) == 0) return candidate;
    }
    return base;
}

// -----------------------------------------------------------------------------
// moduleTree（对应 server.py on_fpgaLsp_moduleTree）
// -----------------------------------------------------------------------------

Json MakeUsedAt(const Instance& instance) {
    Json used = Json::object();
    used["uri"] = platform::path_to_uri(instance.file);
    used["range"] = MakeRange(instance.line, instance.character, instance.line,
                              instance.character + static_cast<int>(instance.name.size()));
    return used;
}

struct TreeBudget {
    int left = 5000;
    bool truncated = false;
};

Json BuildNode(const HdlIndex& index, const std::string& module_name,
               const Instance* instance, std::vector<std::string> stack, int depth,
               bool include_primitives, TreeBudget& budget) {
    if (budget.left <= 0) {
        budget.truncated = true;
        return Json(nullptr);
    }
    --budget.left;

    const Module* definition = index.ModuleByName(module_name);
    if (definition == nullptr) {
        Json node = Json::object();
        node["module"] = module_name;
        node["instance"] = (instance != nullptr && !instance->name.empty()) ? instance->name
                                                                            : module_name;
        node["file"] = instance != nullptr ? instance->file : std::string();
        const int line = instance != nullptr ? instance->line : 0;
        const int character = instance != nullptr ? instance->character : 0;
        node["range"] =
            MakeRange(line, character, line, character + static_cast<int>(module_name.size()));
        node["defined"] = false;
        node["blackbox"] = true;
        node["recursive"] = false;
        node["portCount"] = 0;
        node["usedAt"] = instance != nullptr ? MakeUsedAt(*instance) : Json(nullptr);
        node["children"] = Json::array();
        return node;
    }

    Json node = Json::object();
    node["module"] = definition->name;
    node["instance"] = instance != nullptr ? instance->name : std::string();
    node["file"] = definition->file;
    node["range"] = MakeRange(definition->line, definition->character, definition->line,
                              definition->character +
                                  static_cast<int>(definition->name.size()));
    node["defined"] = true;
    node["blackbox"] = false;
    node["recursive"] = false;
    node["portCount"] = definition->ports.size();
    node["usedAt"] = instance != nullptr ? MakeUsedAt(*instance) : Json(nullptr);

    std::string lowered = definition->name;
    for (char& c : lowered) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (std::find(stack.begin(), stack.end(), lowered) != stack.end() || depth >= 24) {
        node["recursive"] = true;
        node["children"] = Json::array();
        return node;
    }

    Json children = Json::array();
    stack.push_back(lowered);
    for (const Instance* child : index.InstancesOfFile(definition->file)) {
        if (child->module.empty()) continue;
        if (child->line < definition->line || child->line > definition->end_line) continue;
        if (!include_primitives && index.ModuleByName(child->module) == nullptr &&
            (hdl::IsPrimitiveGate(child->module) || hdl::IsDevicePrimitive(child->module))) {
            continue;
        }
        Json built = BuildNode(index, child->module, child, stack, depth + 1,
                               include_primitives, budget);
        if (built.is_null()) break;
        children.push_back(std::move(built));
        if (children.size() >= 500) {
            node["truncated"] = true;
            break;
        }
    }
    node["children"] = std::move(children);
    return node;
}

// 取第 line 行的文本（不含换行）
std::string_view LineOf(std::string_view text, int line) {
    std::size_t begin = 0;
    int current = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            if (current == line) {
                std::string_view slice = text.substr(begin, i - begin);
                if (!slice.empty() && slice.back() == '\r') slice.remove_suffix(1);
                return slice;
            }
            begin = i + 1;
            ++current;
        }
    }
    return {};
}

}  // namespace

// =============================================================================
//  公开查询
// =============================================================================

Json DocumentSymbols(const HdlIndex& index, std::string_view path) {
    Json out = Json::array();
    for (const DocSymbol& symbol : index.DocumentSymbols(path)) {
        out.push_back(DocSymbolToJson(symbol));
    }
    return out;
}

Json WorkspaceSymbols(const HdlIndex& index, std::string_view query) {
    Json out = Json::array();
    for (const WorkspaceSymbol& symbol : index.SearchSymbols(query)) {
        out.push_back(WorkspaceSymbolToJson(symbol));
    }
    return out;
}

Json Definition(const HdlIndex& index, std::string_view path, int line, int character) {
    const std::optional<Location> location = index.DefinitionOf(path, line, character);
    if (!location.has_value()) return Json(nullptr);
    return MakeLocation(location->path, location->line, location->character,
                        location->end_character);
}

Json Hover(const HdlIndex& index, std::string_view path, int line, int character) {
    const std::optional<SymbolAtResult> symbol = index.SymbolAt(path, line, character);
    if (!symbol.has_value()) return Json(nullptr);

    std::string markdown = "**" + symbol->name + "** — `" + symbol->kind + "`";
    if (!symbol->detail.empty()) markdown += "\n\n" + symbol->detail;
    if (symbol->width.has_value() && *symbol->width > 1) {
        markdown += "\n\nwidth: " + std::to_string(*symbol->width);
    }
    if (!symbol->module.empty() && symbol->kind != "module") {
        markdown += "\n\nmodule: `" + symbol->module + "`";
    }
    markdown += "\n\n```\n" + symbol->file + ":" + std::to_string(symbol->span.line + 1) +
                ":" + std::to_string(symbol->span.start_character + 1) + "\n```";

    Json contents = Json::object();
    contents["kind"] = "markdown";
    contents["value"] = std::move(markdown);

    Json result = Json::object();
    result["contents"] = std::move(contents);
    result["range"] = MakeSpan(symbol->span);
    return result;
}

Json References(const HdlIndex& index, std::string_view path, int line, int character,
                bool include_declaration) {
    Json out = Json::array();
    const std::optional<SymbolAtResult> symbol = index.SymbolAt(path, line, character);
    for (const Location& location : index.References(path, line, character)) {
        if (!include_declaration && symbol.has_value() && location.path == symbol->file &&
            location.line == symbol->span.line &&
            location.character == symbol->span.start_character) {
            continue;
        }
        out.push_back(MakeLocation(location.path, location.line, location.character,
                                   location.end_character));
    }
    return out;
}

Json InlayHints(const HdlIndex& index, std::string_view path) {
    Json out = Json::array();
    for (const Instance* instance : index.InstancesOfFile(path)) {
        if (instance->name.empty()) continue;
        Json hint = Json::object();
        Json position = Json::object();
        position["line"] = instance->line;
        position["character"] = instance->character +
                                static_cast<int>(instance->name.size());
        hint["position"] = std::move(position);
        hint["label"] = ": " + instance->module;
        hint["kind"] = 1;  // Type
        hint["paddingLeft"] = false;
        out.push_back(std::move(hint));
    }
    return out;
}

Json Completion(const HdlIndex& index, std::string_view path, std::string_view text, int line,
                int character) {
    Json items = Json::array();
    const std::string path_string(path);

    const std::string_view line_text = LineOf(text, line);
    const std::string prefix = PrefixIdentifier(line_text, character);
    const bool at_line_start =
        OnlyWhitespaceBefore(line_text, character - static_cast<int>(prefix.size()));
    // Phase 2 / B2：用户已经键入前缀时只回匹配项（814 → 几项），
    // 前缀为空时保持 Phase 1 的全量候选（探针/旧客户端契约不变）。
    const auto matches = [&prefix](std::string_view label) {
        if (prefix.empty()) return true;
        if (label.size() < prefix.size()) return false;
        for (std::size_t i = 0; i < prefix.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(label[i])) !=
                std::tolower(static_cast<unsigned char>(prefix[i]))) {
                return false;
            }
        }
        return true;
    };

    // Python: `for name in sorted(names)` —— 索引的 ModuleNames() 是**文件/插入顺序**，
    // 与 Python 的排序口径不同。这里排一次，保证「按 limit 截断」时 LSP 与 MCP
    // 截到的窗口一致（MCP 默认 limit=50，顺序不一致会让窗口内容漂移）。
    std::vector<std::string> names = index.ModuleNames();
    std::sort(names.begin(), names.end());

    for (const std::string& name : names) {
        if (!matches(name)) continue;
        const Module* module = index.ModuleByName(name);
        Json item = Json::object();
        item["label"] = name;
        item["kind"] = 9;  // Module
        if (module != nullptr) {
            item["detail"] =
                "module (" + std::to_string(module->ports.size()) + " ports)";
        }
        items.push_back(std::move(item));

        if (!at_line_start || module == nullptr) continue;
        const std::string indent = IndentOf(line_text, character);
        const std::string instance = UniqueInstanceName(index, path_string, module->name);
        Json snippet = Json::object();
        snippet["label"] = module->name + " (instantiate)";
        snippet["kind"] = 15;  // Snippet
        snippet["detail"] = std::to_string(module->ports.size()) + " ports";
        snippet["insertTextFormat"] = 2;
        snippet["insertText"] = InstantiationSnippet(*module, indent, instance);
        snippet["filterText"] = module->name;
        items.push_back(std::move(snippet));
    }

    Json result = Json::object();
    result["isIncomplete"] = !prefix.empty();
    // 器件原语（745，kind 3）+ Verilog 关键字（48，kind 14）
    //   Python `_complete_verilog` 的尾部两个循环，**紧跟在模块候选之后**、顺序一致。
    //   放在 features 层（而不是某个 handler 里）是必须的：LSP 的
    //   textDocument/completion 与 MCP 的 fpga_completion 都调这一个函数，谁都不能少。
    for (const std::string& primitive : hdl::DevicePrimitives()) {
        if (!matches(primitive)) continue;
        Json item = Json::object();
        item["label"] = primitive;
        item["kind"] = 3;  // Method/Function
        item["detail"] = "Device primitive";
        items.push_back(std::move(item));
    }
    for (const std::string& keyword : hdl::VerilogKeywords()) {
        if (!matches(keyword)) continue;
        Json item = Json::object();
        item["label"] = keyword;
        item["kind"] = 14;  // Keyword
        items.push_back(std::move(item));
    }
    result["items"] = std::move(items);
    return result;
}

// -----------------------------------------------------------------------------
//  B-B2：时序端点 -> 源码
// -----------------------------------------------------------------------------
namespace {

bool ResolveOneEndpoint(const HdlIndex& index, const project::Project& project, Json& endpoint) {
    if (!endpoint.is_object()) return false;
    const std::string signal = endpoint.value("signal", std::string());
    if (signal.empty()) return false;

    std::vector<std::string> chain;
    if (const auto it = endpoint.find("hierarchy");
        it != endpoint.end() && it->is_array()) {
        for (const Json& part : *it) {
            if (part.is_string()) chain.push_back(part.get<std::string>());
        }
    }

    std::string module = project.top;
    std::size_t consumed = 0;
    for (; consumed < chain.size(); ++consumed) {
        const hdl::Module* current = index.ModuleByName(module);
        if (current == nullptr) break;
        const std::string& step = chain[consumed];
        const hdl::Instance* hit = nullptr;
        for (const hdl::Instance* instance : index.InstancesOfFile(current->file)) {
            if (instance->line < current->line || instance->line > current->end_line) continue;
            if (instance->name == step) {
                hit = instance;
                break;
            }
        }
        if (hit == nullptr) {
            // 报告里有时写的是模块名而不是实例名
            if (index.ModuleByName(step) != nullptr) {
                module = step;
                continue;
            }
            break;
        }
        if (hit->module.empty()) break;
        module = hit->module;
    }

    const hdl::Module* definition = index.ModuleByName(module);
    if (definition == nullptr) {
        endpoint["resolved"] = false;
        endpoint["reason"] = "module-not-in-index";
        return false;
    }

    for (const hdl::ModuleInfo* info : index.ModulesOfFile(definition->file)) {
        if (info->module.name != definition->name) continue;
        for (const hdl::Signal& sig : info->signals) {
            if (sig.name != signal) continue;
            endpoint["file"] = platform::path_to_uri(sig.file.empty() ? definition->file : sig.file);
            endpoint["line"] = sig.line;
            endpoint["character"] = sig.character;
            endpoint["module"] = module;
            endpoint["resolved"] = true;
            endpoint["reason"] = nullptr;
            return true;
        }
        for (const hdl::Port& port : definition->ports) {
            if (port.name != signal) continue;
            endpoint["file"] =
                platform::path_to_uri(port.file.empty() ? definition->file : port.file);
            endpoint["line"] = port.line;
            endpoint["character"] = port.character;
            endpoint["module"] = module;
            endpoint["resolved"] = true;
            endpoint["reason"] = nullptr;
            return true;
        }
        break;
    }

    endpoint["resolved"] = false;
    endpoint["reason"] = consumed < chain.size() ? "hierarchy-not-in-index" : "signal-not-in-module";
    return false;
}

void ResolveTimingSection(const HdlIndex& index, const project::Project& project, Json& section) {
    if (!section.is_object()) return;
    const auto paths_it = section.find("paths");
    if (paths_it == section.end() || !paths_it->is_array()) return;
    int total = 0;
    int resolved = 0;
    for (Json& path : *paths_it) {
        if (!path.is_object()) continue;
        for (const char* key : {"begin_at", "end_at"}) {
            const auto it = path.find(key);
            if (it == path.end() || !it->is_object()) continue;
            ++total;
            if (ResolveOneEndpoint(index, project, *it)) ++resolved;
        }
    }
    if (total > 0) {
        Json counts = Json::object();
        counts["total"] = total;
        counts["resolved"] = resolved;
        section["begin_end_resolved"] = std::move(counts);
    }
}

}  // namespace

namespace {

// B-B7：area.modules[] 的每一项 -> {file,line,character,resolved,reason}
void ResolveAreaSection(const HdlIndex& index, Json& section) {
    if (!section.is_object()) return;
    const auto modules_it = section.find("modules");
    if (modules_it == section.end() || !modules_it->is_array()) return;
    for (Json& row : *modules_it) {
        if (!row.is_object()) continue;
        std::string module_name = row.value("module", std::string());
        if (module_name.empty()) module_name = row.value("name", std::string());
        if (module_name.empty()) continue;
        const hdl::Module* definition = index.ModuleByName(module_name);
        if (definition == nullptr) {
            row["resolved"] = false;
            row["reason"] = "module-not-in-index";
            row["file"] = nullptr;
            row["line"] = nullptr;
            continue;
        }
        row["resolved"] = true;
        row["reason"] = nullptr;
        row["file"] = platform::path_to_uri(definition->file);
        row["line"] = definition->line;
        row["character"] = definition->character;
    }
}

}  // namespace

void ResolveAreaModules(const HdlIndex& index, const project::Project& project, Json& data) {
    (void)project;
    if (!data.is_object()) return;
    if (auto it = data.find("area"); it != data.end()) ResolveAreaSection(index, *it);
    for (const char* stage : {"syn", "phy"}) {
        if (auto stage_it = data.find(stage); stage_it != data.end() && stage_it->is_object()) {
            if (auto area = stage_it->find("area"); area != stage_it->end()) {
                ResolveAreaSection(index, *area);
            }
        }
    }
}

void ResolveTimingEndpoints(const HdlIndex& index, const project::Project& project, Json& data) {
    if (project.top.empty() || !data.is_object()) return;
    if (auto it = data.find("timing"); it != data.end()) ResolveTimingSection(index, project, *it);
    for (const char* stage : {"syn", "phy"}) {
        if (auto stage_it = data.find(stage); stage_it != data.end() && stage_it->is_object()) {
            if (auto timing = stage_it->find("timing"); timing != stage_it->end()) {
                ResolveTimingSection(index, project, *timing);
            }
        }
    }
}

// P-C2：懒加载子节点。只构建**直接子节点**（每个都不带 children），
// 并给出 hasChildren 让客户端知道还能继续展开。
// 这样 1000 节点工程的首屏只传根的一层，而不是整棵树（服务端 500 子节点上限
// 会静默截断，客户端也只能整棵重建）。
namespace {

bool HasChildInstances(const HdlIndex& index, const Module& definition,
                       bool include_primitives) {
    for (const Instance* child : index.InstancesOfFile(definition.file)) {
        if (child->module.empty()) continue;
        if (child->line < definition.line || child->line > definition.end_line) continue;
        if (!include_primitives && index.ModuleByName(child->module) == nullptr &&
            (hdl::IsPrimitiveGate(child->module) || hdl::IsDevicePrimitive(child->module))) {
            continue;
        }
        return true;
    }
    return false;
}

// 与 BuildNode 的节点头**逐字段一致**，只是不带 children。
Json BuildShallowNode(const HdlIndex& index, const std::string& module_name,
                      const Instance* instance, bool include_primitives) {
    const Module* definition = index.ModuleByName(module_name);
    Json node = Json::object();
    if (definition == nullptr) {
        node["module"] = module_name;
        node["instance"] = (instance != nullptr && !instance->name.empty()) ? instance->name
                                                                            : module_name;
        node["file"] = instance != nullptr ? instance->file : std::string();
        const int line = instance != nullptr ? instance->line : 0;
        const int character = instance != nullptr ? instance->character : 0;
        node["range"] =
            MakeRange(line, character, line, character + static_cast<int>(module_name.size()));
        node["defined"] = false;
        node["blackbox"] = true;
        node["recursive"] = false;
        node["portCount"] = 0;
        node["usedAt"] = instance != nullptr ? MakeUsedAt(*instance) : Json(nullptr);
        node["hasChildren"] = false;
        return node;
    }
    node["module"] = definition->name;
    node["instance"] = instance != nullptr ? instance->name : std::string();
    node["file"] = definition->file;
    node["range"] = MakeRange(definition->line, definition->character, definition->line,
                              definition->character +
                                  static_cast<int>(definition->name.size()));
    node["defined"] = true;
    node["blackbox"] = false;
    node["recursive"] = false;
    node["portCount"] = definition->ports.size();
    node["usedAt"] = instance != nullptr ? MakeUsedAt(*instance) : Json(nullptr);
    node["hasChildren"] = HasChildInstances(index, *definition, include_primitives);
    return node;
}

}  // namespace

Json ModuleChildren(const HdlIndex& index, const project::Project& project,
                    std::string_view module, int offset, int limit, bool include_primitives) {
    std::string parent = std::string(module);
    if (parent.empty()) parent = project.top;
    if (parent.empty()) {
        Json out = Json::object();
        out["ok"] = false;
        out["error"] = "No HDL index or top module set";
        return out;
    }
    const Module* definition = index.ModuleByName(parent);
    if (definition == nullptr) {
        Json out = Json::object();
        out["ok"] = false;
        out["error"] = "Unknown module: " + parent;
        return out;
    }

    std::vector<const Instance*> children;
    for (const Instance* child : index.InstancesOfFile(definition->file)) {
        if (child->module.empty()) continue;
        if (child->line < definition->line || child->line > definition->end_line) continue;
        if (!include_primitives && index.ModuleByName(child->module) == nullptr &&
            (hdl::IsPrimitiveGate(child->module) || hdl::IsDevicePrimitive(child->module))) {
            continue;
        }
        children.push_back(child);
    }

    const int total = static_cast<int>(children.size());
    const int begin = std::clamp(offset, 0, total);
    const int take = std::clamp(limit <= 0 ? 200 : limit, 1, 500);
    Json items = Json::array();
    for (int i = begin; i < total && static_cast<int>(items.size()) < take; ++i) {
        items.push_back(BuildShallowNode(index, children[i]->module, children[i],
                                         include_primitives));
    }

    Json out = Json::object();
    out["ok"] = true;
    out["project"] = project.name;
    out["al"] = project.al_path;
    out["parent"] = definition->name;
    out["children"] = std::move(items);
    out["total"] = total;
    out["nextOffset"] = begin + static_cast<int>(out["children"].size());
    out["hasMore"] = begin + static_cast<int>(out["children"].size()) < total;
    return out;
}

Json ModuleTree(const HdlIndex& index, const project::Project& project,
                bool include_primitives) {
    if (project.top.empty()) {
        Json out = Json::object();
        out["ok"] = false;
        out["error"] = "No HDL index or top module set";
        return out;
    }

    TreeBudget budget;
    Json root = BuildNode(index, project.top, nullptr, {}, 0, include_primitives, budget);
    if (root.is_null()) {
        Json out = Json::object();
        out["ok"] = false;
        out["error"] = "Module tree too large";
        return out;
    }

    Json result = Json::object();
    result["ok"] = true;
    result["project"] = project.name;
    result["al"] = project.al_path;
    result["root"] = std::move(root);
    result["modulesSeen"] = index.ModuleCount();
    result["truncated"] = budget.truncated;
    return result;
}

}  // namespace fpgalsp::server::features