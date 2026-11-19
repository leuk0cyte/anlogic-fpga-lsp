// =============================================================================
//  src/server/features_call_hierarchy.cpp —— call hierarchy + 区间格式化
// =============================================================================
//  OWNER: task B4 (parity-lsp)。B1 只放 stub；本文件是 LSP-06/07/08/09 四项 S0 的实现。
//
//  对应参考实现：
//    Y:\fpga-lsp\fpgalsp\nav.py:141-187   call_hierarchy / call_hierarchy_for_module
//    Y:\fpga-lsp\fpgalsp\nav.py:118-138   instance_parent_map
//    Y:\fpga-lsp\fpgalsp\server.py:1702-1785  on_*CallHierarchy*
//    Y:\fpga-lsp\fpgalsp\server.py:1646-1667  on_textDocument_rangeFormatting
//
//  契约（逐条对齐 Python）：
//    * CallHierarchyItem 恒为 {name, kind:2, uri, range, selectionRange, data{module, defined}}
//      - defined=true  -> uri/range 取模块定义（模块名 token 的位置）
//      - defined=false -> uri = .al 路径，range = 0:0-0:1（黑盒/未定义模块）
//    * outgoing：同文件、落在该模块 [line, end_line] 区间内的实例，**按目标模块名分组**，
//      分组顺序 = 目标名首次出现的顺序（Python dict 语义）
//    * incoming：`instance.module == 模块名` 的实例，按"所在模块"分组（最内层），
//      找不到父模块的实例被丢弃（Python: `if not parent: continue`）
//    * rangeFormatting：全文格式化 -> 行数不同则回落到全文档编辑；行数相同只出区间内的单行编辑
//
//  ⚠ 与 Python 的一处**已知**差异：`ifdef 死分支 / 非 BMP 列（见 B2-REPORT §4），
//    本文件沿用 hdl::HdlIndex 的既有语义（只索引激活分支、UTF-16 列）。
// =============================================================================

#include <algorithm>
#include <cstddef>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hdl/hdl_index.h"
#include "jsonrpc/json_util.h"
#include "platform/path.h"
#include "platform/utf16.h"
#include "server/features_range_format.h"
#include "server/lsp_server.h"

#if defined(FPGA_LSP_HAVE_VERIBLE_BRIDGE)
#include "verible_bridge/format_text.h"
#endif

namespace fpgalsp::server {
namespace {

// -----------------------------------------------------------------------------
//  JSON 小工具
// -----------------------------------------------------------------------------

Json Position(int line, int character) {
    Json position = Json::object();
    position["line"] = line;
    position["character"] = character;
    return position;
}

Json MakeRange(int start_line, int start_character, int end_line, int end_character) {
    Json range = Json::object();
    range["start"] = Position(start_line, start_character);
    range["end"] = Position(end_line, end_character);
    return range;
}

int RangeLine(const Json& range, const char* edge, int fallback) {
    if (!range.is_object()) return fallback;
    const auto it = range.find(edge);
    if (it == range.end() || !it->is_object()) return fallback;
    return jsonrpc::GetInt(*it, "line", fallback);
}

int CallItemModuleName(const Json& item, std::string* module_name) {
    if (!item.is_object()) return 0;
    const Json data = jsonrpc::GetObject(item, "data");
    *module_name = jsonrpc::GetString(data, "module");
    if (module_name->empty()) *module_name = jsonrpc::GetString(item, "module");
    return module_name->empty() ? 0 : 1;
}

// -----------------------------------------------------------------------------
//  CallHierarchyItem（对应 Python server.py:_call_item）
// -----------------------------------------------------------------------------

Json MakeCallItem(const hdl::HdlIndex& index, const std::string& module_name,
                  const project::Project& project) {
    Json item = Json::object();
    item["name"] = module_name;
    item["kind"] = 2;  // SymbolKind.Module

    const hdl::Module* module = index.ModuleByName(module_name);
    if (module == nullptr) {
        // 黑盒 / 未定义：指向 .al，range 取 Python 的 0:0-0:1 占位
        item["uri"] = platform::path_to_uri(project.al_path);
        item["range"] = MakeRange(0, 0, 0, 1);
        item["selectionRange"] = MakeRange(0, 0, 0, 1);
        Json data = Json::object();
        data["module"] = module_name;
        data["defined"] = false;
        item["data"] = std::move(data);
        return item;
    }

    item["uri"] = platform::path_to_uri(module->file);
    const std::string& name = module->name.empty() ? module_name : module->name;
    const int line = module->line;
    const int character = module->character;
    const int end_character = character + static_cast<int>(platform::utf16_len(name));
    item["range"] = MakeRange(line, character, line, end_character);
    item["selectionRange"] = MakeRange(line, character, line, end_character);
    Json data = Json::object();
    data["module"] = module_name;
    data["defined"] = true;
    item["data"] = std::move(data);
    return item;
}

// -----------------------------------------------------------------------------
//  实例 <<->> 模块 的隶属关系
// -----------------------------------------------------------------------------

bool InstanceInsideModule(const hdl::Instance& instance, const hdl::Module& module) {
    if (instance.file != module.file) return false;
    const int end = module.end_line > 0 ? module.end_line : std::numeric_limits<int>::max();
    return instance.line >= module.line && instance.line <= end;
}

// 对应 Python nav.instance_parent_map：同文件里**最内层**（start 最大）的包含模块
const hdl::Module* ParentModuleOf(const hdl::HdlIndex& index, const hdl::Instance& instance) {
    const hdl::Module* best = nullptr;
    for (const hdl::ModuleInfo* info : index.ModulesOfFile(instance.file)) {
        const hdl::Module& module = info->module;
        if (!InstanceInsideModule(instance, module)) continue;
        if (best == nullptr || module.line > best->line) best = &module;
    }
    return best;
}

// 按"首次出现顺序"分组（Python dict 的插入顺序）
class FirstSeenGroups {
public:
    void Add(const std::string& key, const hdl::Instance* instance) {
        const auto it = positions_.find(key);
        if (it == positions_.end()) {
            positions_.emplace(key, groups_.size());
            groups_.push_back({key, {instance}});
            return;
        }
        groups_[it->second].second.push_back(instance);
    }

    const std::vector<std::pair<std::string, std::vector<const hdl::Instance*>>>& groups() const {
        return groups_;
    }

private:
    std::map<std::string, std::size_t> positions_;
    std::vector<std::pair<std::string, std::vector<const hdl::Instance*>>> groups_;
};

Json FromRanges(const std::vector<const hdl::Instance*>& instances) {
    Json ranges = Json::array();
    for (const hdl::Instance* instance : instances) {
        const int length = static_cast<int>(platform::utf16_len(instance->name));
        ranges.push_back(MakeRange(instance->line, instance->character, instance->line,
                                   instance->character + length));
    }
    return ranges;
}

// -----------------------------------------------------------------------------
//  行切分（保留行尾的 '\r'，与 Python str.split("\n") 对齐）
// -----------------------------------------------------------------------------

std::vector<std::string_view> SplitLines(std::string_view text) {
    std::vector<std::string_view> rows;
    std::size_t position = 0;
    for (;;) {
        const std::size_t newline = text.find('\n', position);
        if (newline == std::string_view::npos) {
            rows.push_back(text.substr(position));
            return rows;
        }
        rows.push_back(text.substr(position, newline - position));
        position = newline + 1;
    }
}

int RowWidth(std::string_view row) {
    if (!row.empty() && row.back() == '\r') row.remove_suffix(1);
    return static_cast<int>(platform::utf16_len(row));
}

bool IsBlank(std::string_view row) {
    for (const char ch : row) {
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n' && ch != '\f' && ch != '\v') {
            return false;
        }
    }
    return true;
}

// Python on_textDocument_formatting 的那条编辑：覆盖全文
Json FullDocumentEdit(std::string_view text, std::string_view formatted) {
    const std::vector<std::string_view> rows = SplitLines(text);
    const int last_line = static_cast<int>(rows.size()) - 1;
    Json edit = Json::object();
    edit["range"] = MakeRange(0, 0, last_line, RowWidth(rows.back()));
    edit["newText"] = std::string(formatted);
    Json edits = Json::array();
    edits.push_back(std::move(edit));
    return edits;
}

}  // namespace

// =============================================================================
//  textDocument/prepareCallHierarchy
// =============================================================================

Json LspServer::FeatureCallHierarchyPrepare(std::string_view path, int line, int character) {
    Json result = Json::array();
    const std::string path_string(path);
    if (path_string.empty()) return result;

    const std::optional<project::Project> project = ProjectForPath(path);
    if (!project.has_value()) return result;
    const std::shared_ptr<const hdl::HdlIndex> index = IndexForProject(*project);
    if (!index) return result;

    const std::optional<hdl::SymbolAtResult> symbol = index->SymbolAt(path, line, character);
    if (!symbol.has_value()) return result;

    std::string module_name;
    if (symbol->kind == "module") {
        module_name = symbol->name;
    } else if (symbol->kind == "instance") {
        module_name = symbol->module;  // 光标在实例名上 -> 跳到他例化的模块
    }
    if (module_name.empty()) return result;
    if (index->ModuleByName(module_name) == nullptr) return result;  // Python: module 未定义 -> []

    result.push_back(MakeCallItem(*index, module_name, *project));
    return result;
}

// =============================================================================
//  callHierarchy/outgoingCalls
// =============================================================================

Json LspServer::FeatureCallHierarchyOutgoing(const Json& item) {
    Json result = Json::array();
    std::string module_name;
    if (CallItemModuleName(item, &module_name) == 0) return result;

    // 工程来源：item.uri 所在工程，回落到当前工程（Python 同样先看 doc.path 再看 root）
    std::optional<project::Project> project;
    const std::string uri = jsonrpc::GetString(item, "uri");
    if (!uri.empty()) project = ProjectForPath(platform::uri_to_path(uri));
    if (!project.has_value()) project = CurrentProject();
    if (!project.has_value()) return result;

    const std::shared_ptr<const hdl::HdlIndex> index = IndexForProject(*project);
    if (!index) return result;
    const hdl::Module* module = index->ModuleByName(module_name);
    if (module == nullptr) return result;

    // 只取"同一个定义文件 + 落在该模块行区间内"的实例
    FirstSeenGroups groups;
    for (const hdl::Instance* instance : index->AllInstances()) {
        if (!InstanceInsideModule(*instance, *module)) continue;
        if (instance->module.empty()) continue;  // Python: target 为空则跳过
        groups.Add(instance->module, instance);
    }

    const Json from = MakeCallItem(*index, module_name, *project);
    for (const auto& [target, instances] : groups.groups()) {
        Json call = Json::object();
        call["from"] = from;
        call["fromRanges"] = FromRanges(instances);
        call["to"] = MakeCallItem(*index, target, *project);
        result.push_back(std::move(call));
    }
    return result;
}

// =============================================================================
//  callHierarchy/incomingCalls
// =============================================================================

Json LspServer::FeatureCallHierarchyIncoming(const Json& item) {
    Json result = Json::array();
    std::string module_name;
    if (CallItemModuleName(item, &module_name) == 0) return result;

    std::optional<project::Project> project;
    const std::string uri = jsonrpc::GetString(item, "uri");
    if (!uri.empty()) project = ProjectForPath(platform::uri_to_path(uri));
    if (!project.has_value()) project = CurrentProject();
    if (!project.has_value()) return result;

    const std::shared_ptr<const hdl::HdlIndex> index = IndexForProject(*project);
    if (!index) return result;
    if (index->ModuleByName(module_name) == nullptr) return result;

    // 谁例化了这个模块：按"所在模块（最内层）"分组，找不到父模块的丢弃
    FirstSeenGroups groups;
    for (const hdl::Instance* instance : index->AllInstances()) {
        if (instance->module != module_name) continue;
        const hdl::Module* parent = ParentModuleOf(*index, *instance);
        if (parent == nullptr || parent->name.empty()) continue;
        groups.Add(parent->name, instance);
    }

    const Json to = MakeCallItem(*index, module_name, *project);
    for (const auto& [parent, instances] : groups.groups()) {
        Json call = Json::object();
        call["from"] = MakeCallItem(*index, parent, *project);
        call["fromRanges"] = FromRanges(instances);
        call["to"] = to;
        result.push_back(std::move(call));
    }
    return result;
}

// =============================================================================
//  区间格式化
// =============================================================================

Json RangeFormattingEdits(std::string_view text, std::string_view formatted, int first_line,
                          int last_line) {
    const std::vector<std::string_view> old_rows = SplitLines(text);
    const std::vector<std::string_view> new_rows = SplitLines(formatted);

    // 行结构变了：只有"整篇替换"能表达（Python 直接回落到 on_textDocument_formatting）
    if (old_rows.size() != new_rows.size()) return FullDocumentEdit(text, formatted);

    Json edits = Json::array();
    const int last = static_cast<int>(old_rows.size()) - 1;
    const int begin = std::max(0, first_line);
    const int end = std::min(last_line < first_line ? first_line : last_line, last);
    for (int line = begin; line <= end && line <= last; ++line) {
        const std::string_view old_row = old_rows[static_cast<std::size_t>(line)];
        const std::string_view new_row = new_rows[static_cast<std::size_t>(line)];
        if (old_row == new_row) continue;
        if (IsBlank(new_row)) continue;
        Json edit = Json::object();
        edit["range"] = MakeRange(line, 0, line, RowWidth(old_row));
        edit["newText"] = std::string(new_row);
        edits.push_back(std::move(edit));
    }
    return edits;
}

Json LspServer::FeatureRangeFormatting(std::string_view path, const Json& range,
                                       const Json& options) {
    (void)options;  // Python 只把 options 用于"无 Verible 时的内置重排"，C++ 不做兜底
#if !defined(FPGA_LSP_HAVE_VERIBLE_BRIDGE)
    (void)path;
    (void)range;
    return Json::array();  // 没链接 formatter 桥：合法空结果
#else
    const std::string path_string(path);
    if (path_string.empty()) return Json::array();
    // Python: 只有 .v/.sv/.vh/.svh 才有格式化
    if (!hdl::IsVerilogPath(path_string)) return Json::array();

    const std::optional<std::string> text = LoadText(path_string);
    if (!text.has_value()) return Json::array();

    const verible_bridge::FormatResult formatted = verible_bridge::FormatText(*text, path_string);
    if (!formatted.ok || formatted.text.empty()) return Json::array();

    const int first_line = RangeLine(range, "start", 0);
    const int last_line = RangeLine(range, "end", first_line);
    return RangeFormattingEdits(*text, formatted.text, first_line, last_line);
#endif
}

}  // namespace fpgalsp::server