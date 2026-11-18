// =============================================================================
//  src/mcp/bridge.cpp —— MCP 工具实现
// =============================================================================
//  逐条移植 Y:\fpga-lsp\fpgalsp\mcp_bridge.py 的对应方法（注释里标了 Python 方法名）。
//  未移植的模块一律显式占位（见 bridge.h 的分阶段说明）。
// =============================================================================

#include "mcp/bridge.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <re2/re2.h>

#include "constraints/adc.h"
#include "flow/flow_runner.h"
#include "flow/programmer.h"
#include "flow/strategy.h"
#include "hdl/hdl_index.h"
#include "jsonrpc/dispatcher.h"
#include "server/features.h"
#include "constraints/sdc.h"
#include "platform/encoding.h"
#include "platform/log.h"
#include "platform/path.h"
#include "project/al_project.h"
#include "reports/reports_aggregate.h"
#include "td/td_device.h"
#include "td/td_diagnostics.h"
#include "td/td_session.h"

namespace fpgalsp::mcp {
namespace {

namespace fs = std::filesystem;
using platform::normalize_path;
using platform::path_dirname;
using platform::path_filename;
using platform::path_join;

// Python mcp_bridge.SEVERITY
const char* SeverityName(int severity) {
    switch (severity) {
        case 1: return "error";
        case 2: return "warning";
        case 3: return "information";
        case 4: return "hint";
        default: return "warning";
    }
}

// std::filesystem 的 u8string() 在 C++20 返回 std::u8string；项目内部统一用 UTF-8 字节串
std::string ToUtf8(const fs::path& path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

bool IsFile(const std::string& path) {
    std::error_code ec;
    return fs::is_regular_file(fs::u8path(path), ec);
}

bool IsDirectory(const std::string& path) {
    std::error_code ec;
    return fs::is_directory(fs::u8path(path), ec);
}

bool PathExists(const std::string& path) {
    std::error_code ec;
    return fs::exists(fs::u8path(path), ec);
}

std::string ReadTextFile(const std::string& path) {
    const platform::ReadFileResult result = platform::read_file_utf8(path);
    return result.content;
}

// Python project.rel(path)：相对工程目录、正斜杠
std::string RelativeTo(const std::string& path, const std::string& base) {
    std::error_code ec;
    const fs::path relative = fs::relative(fs::u8path(path), fs::u8path(base), ec);
    if (ec || relative.empty()) return std::string();
    std::string text = relative.generic_string();
    if (text == ".") return std::string();
    // Python os.path.relpath 会给出 ../ 形式；fs::relative 同语义
    return text;
}

std::string BaseName(const std::string& path) { return path_filename(path); }

// Python os.path.isdir(join(dir, "<name>_Runs"))
std::vector<std::string> ListSubdirectories(const std::string& dir) {
    std::vector<std::string> names;
    std::error_code ec;
    fs::directory_iterator it(fs::u8path(dir), ec);
    if (ec) return names;
    for (const fs::directory_entry& entry : it) {
        if (!entry.is_directory(ec) || ec) continue;
        names.push_back(ToUtf8(entry.path().filename()));
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::string> ListFiles(const std::string& dir) {
    std::vector<std::string> names;
    std::error_code ec;
    fs::directory_iterator it(fs::u8path(dir), ec);
    if (ec) return names;
    for (const fs::directory_entry& entry : it) {
        if (!entry.is_regular_file(ec) || ec) continue;
        names.push_back(ToUtf8(entry.path().filename()));
    }
    std::sort(names.begin(), names.end());
    return names;
}

int CountOccurrences(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return 0;
    int count = 0;
    size_t position = 0;
    while ((position = haystack.find(needle, position)) != std::string_view::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

std::string LowerAscii(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::string Trim(std::string_view text) {
    size_t begin = 0;
    size_t end = text.size();
    const auto is_space = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
    };
    while (begin < end && is_space(text[begin])) ++begin;
    while (end > begin && is_space(text[end - 1])) --end;
    return std::string(text.substr(begin, end - begin));
}

std::vector<std::string> SplitLines(std::string_view text) {
    std::vector<std::string> lines;
    std::string current;
    for (const char c : text) {
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
        } else if (c != '\r') {
            current.push_back(c);
        }
    }
    lines.push_back(current);
    return lines;
}

// Python fnmatch.fnmatch 的子集（* 与 ?）。MCP 的 glob 参数只用到这个程度。
bool GlobMatch(std::string_view pattern, std::string_view text) {
    size_t p = 0;
    size_t t = 0;
    size_t star = std::string_view::npos;
    size_t mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

// 文本扫描端口时的词法：标识符字符集（与 Python re 的 \w 基本一致，ASCII 版）
bool IsIdentStart(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
bool IsIdentChar(char c) {
    return IsIdentStart(c) || (c >= '0' && c <= '9') || c == '$';
}

// 1-based 位置（对应 Python _one_based_range 的 start）
Json DiagnosticJson(const lsp::Diagnostic& diagnostic, const std::string& path,
                    const project::Project& proj) {
    Json item = Json::object();
    item["file"] = path;
    item["line"] = diagnostic.range.start.line + 1;
    item["column"] = diagnostic.range.start.character + 1;
    item["endLine"] = diagnostic.range.end.line + 1;
    item["endColumn"] = diagnostic.range.end.character + 1;
    item["severity"] = SeverityName(diagnostic.severity);
    item["code"] = diagnostic.code;
    item["source"] = diagnostic.source;
    item["message"] = diagnostic.message;
    if (!path.empty()) item["relative"] = RelativeTo(path, proj.dir);
    return item;
}

// 把 JSON 数组截到前 limit 项（Python 的 list[:limit]）
Json TrimListTo(const Json& value, int limit) {
    Json out = Json::array();
    if (!value.is_array()) return out;
    int count = 0;
    for (const Json& item : value) {
        if (count++ >= limit) break;
        out.push_back(item);
    }
    return out;
}

// 前置声明：定义在文件后半段
std::vector<ScannedPort> ScanPortHeader(std::string_view header, size_t header_offset);

// 把 // 与 /* */ 注释替换成空格（保留换行与字节偏移，便于 span 仍指向原文）。
// 为什么需要：top.v 的端口列表里有 `input SUPSTS, // driver enable output signal`
// 这样的行尾注释；不先清空注释，扫描会把注释里的 "output signal" 当成一个端口。
std::string BlankComments(std::string_view text) {
    std::string out(text);
    size_t index = 0;
    while (index < out.size()) {
        if (out[index] == '/' && index + 1 < out.size() && out[index + 1] == '/') {
            while (index < out.size() && out[index] != '\n') {
                out[index] = ' ';
                ++index;
            }
            continue;
        }
        if (out[index] == '/' && index + 1 < out.size() && out[index + 1] == '*') {
            out[index] = ' ';
            out[index + 1] = ' ';
            index += 2;
            while (index + 1 < out.size() && !(out[index] == '*' && out[index + 1] == '/')) {
                if (out[index] != '\n') out[index] = ' ';
                ++index;
            }
            if (index + 1 < out.size()) {
                out[index] = ' ';
                out[index + 1] = ' ';
                index += 2;
            }
            continue;
        }
        ++index;
    }
    return out;
}
Json SummarizeReports(const Json& data, const std::string& project_name, std::string stage,
                      std::string section, int limit, bool include_raw);

Json Placeholder(const std::string& what) {
    Json out = Json::object();
    out["ok"] = false;
    out["error"] = "not implemented: " + what;
    return out;
}

}  // namespace

// =============================================================================
//  构造 / 工程解析
// =============================================================================

Bridge::Bridge(BridgeOptions options) : options_(std::move(options)) {
    root_ = normalize_path(options_.root.empty() ? std::string(".") : options_.root);
    if (!IsDirectory(root_)) {
        throw ToolError("Root directory does not exist: " + root_);
    }
    if (!options_.project.empty()) {
        active_al_ = ResolveAl(options_.project);
    } else {
        try {
            active_al_ = DiscoverDefaultAl();
        } catch (const ToolError&) {
            active_al_.clear();  // 没有工程也能起服务：工程类工具按需报错
        }
    }
}

Bridge::~Bridge() = default;

void Bridge::Shutdown() {
    if (session_) {
        session_->Close();
        session_.reset();
    }
    td_choice_.reset();
    devices_.clear();
}

std::string Bridge::DiscoverDefaultAl() const {
    const std::vector<std::string> candidates = project::DiscoverProjects(root_, 6);
    if (candidates.empty()) throw ToolError("No Anlogic .al project found under " + root_);
    const std::string root_lower = LowerAscii(root_);
    for (const std::string& candidate : candidates) {
        if (LowerAscii(path_dirname(candidate)) == root_lower) return candidate;
    }
    return candidates.front();
}

std::string Bridge::ResolveAl(const std::string& value) const {
    const std::string trimmed = Trim(value);
    if (trimmed.empty()) return std::string();

    const std::string candidate =
        normalize_path(platform::is_absolute_path(trimmed) ? trimmed : path_join(root_, trimmed));
    if (IsFile(candidate) && platform::path_extension(candidate) == ".al") return candidate;

    if (IsDirectory(candidate)) {
        const std::string dot_al = path_join(candidate, ".al");
        if (IsFile(dot_al)) return normalize_path(dot_al);
        const std::vector<std::string> found = project::DiscoverProjects(candidate, 4);
        if (!found.empty()) return found.front();
        throw ToolError("No .al project found in " + candidate);
    }

    const std::string lowered = LowerAscii(trimmed);
    for (const std::string& al : project::DiscoverProjects(root_, 6)) {
        const project::Project parsed = project::ParseAl(al);
        if (LowerAscii(parsed.name) == lowered ||
            LowerAscii(BaseName(al)) == lowered) {
            return al;
        }
    }
    throw ToolError("Project not found: " + trimmed);
}

const project::Project& Bridge::ActiveProject() {
    if (!active_al_.empty()) {
        const auto it = projects_.find(active_al_);
        if (it != projects_.end()) return *it->second;
        auto parsed = std::make_unique<project::Project>(project::ParseAl(active_al_));
        auto& slot = projects_[active_al_];
        slot = std::move(parsed);
        return *slot;
    }
    active_al_ = DiscoverDefaultAl();
    return ActiveProject();
}

const project::Project& Bridge::ProjectFor(const Json& arguments) {
    const auto it = arguments.find("project");
    if (it != arguments.end() && it->is_string() && !it->get<std::string>().empty()) {
        const std::string al = ResolveAl(it->get<std::string>());
        const auto cached = projects_.find(al);
        if (cached != projects_.end()) return *cached->second;
        auto& slot = projects_[al];
        slot = std::make_unique<project::Project>(project::ParseAl(al));
        active_al_ = al;
        return *slot;
    }
    return ActiveProject();
}

std::string Bridge::ResolveFile(const std::string& value, const project::Project& proj) const {
    if (value.empty()) throw ToolError("file is required");
    std::string candidate = value;
    if (!platform::is_absolute_path(candidate)) {
        const std::string in_project = normalize_path(path_join(proj.dir, candidate));
        if (IsFile(in_project)) {
            candidate = in_project;
        } else {
            const std::string in_root = normalize_path(path_join(root_, candidate));
            if (IsFile(in_root)) candidate = in_root;
        }
    }
    candidate = normalize_path(candidate);
    if (!IsFile(candidate)) throw ToolError("File not found: " + value);
    return candidate;
}

bool Bridge::IsWithin(const std::string& path, const std::string& root) {
    const std::string target = LowerAscii(normalize_path(path));
    const std::string base = LowerAscii(normalize_path(root));
    if (base.empty()) return false;
    if (target == base) return true;
    if (target.size() <= base.size()) return false;
    if (target.compare(0, base.size(), base) != 0) return false;
    const char separator = target[base.size()];
    return separator == '/' || separator == '\\';
}

std::string Bridge::SafeProjectFile(const std::string& value,
                                    const project::Project& proj) const {
    const std::string path = ResolveFile(value, proj);
    if (!IsWithin(path, proj.dir)) throw ToolError("File is outside the project directory: " + path);
    return path;
}

std::string Bridge::SafeProjectPath(const std::string& value,
                                    const project::Project& proj) const {
    const std::string trimmed = Trim(value);
    if (trimmed.empty()) throw ToolError("path is required");
    std::string candidate = trimmed;
    if (!platform::is_absolute_path(candidate)) {
        const std::string in_project = normalize_path(path_join(proj.dir, candidate));
        const std::string in_root = normalize_path(path_join(root_, candidate));
        if (PathExists(in_project)) {
            candidate = in_project;
        } else if (PathExists(in_root)) {
            candidate = in_root;
        } else {
            candidate = in_project;
        }
    }
    candidate = normalize_path(candidate);
    if (!IsWithin(candidate, proj.dir)) {
        throw ToolError("Path is outside the project directory: " + candidate);
    }
    if (!PathExists(candidate)) throw ToolError("Path not found: " + candidate);
    return candidate;
}

// =============================================================================
//  TD 会话 / 器件
// =============================================================================

Json Bridge::TdSummary() const {
    const std::optional<td::TdChoice> choice = td::FindTd(options_.td_path);
    Json out = Json::object();
    if (!choice.has_value()) {
        out["ok"] = false;
        out["error"] = "No TangDynasty installation found";
        return out;
    }
    out["ok"] = true;
    out["root"] = choice->root;
    out["version"] = choice->version;
    out["source"] = choice->source;
    Json alternatives = Json::array();
    for (const td::TdInstall& install : choice->alternatives) {
        Json item = Json::object();
        item["root"] = install.root;
        item["version"] = install.version;
        alternatives.push_back(std::move(item));
    }
    out["alternatives"] = std::move(alternatives);
    return out;
}

td::TdSession& Bridge::EnsureSession() {
    if (session_) return *session_;
    std::optional<td::TdChoice> choice = td::FindTd(options_.td_path);
    if (!choice.has_value()) throw ToolError("No TangDynasty installation found");
    td_choice_ = *choice;
    td::TdSessionOptions session_options;
    session_ = std::make_unique<td::TdSession>(*td_choice_, session_options);
    return *session_;
}

const td::DeviceInfo& Bridge::EnsureDeviceInfo(const project::Project& proj, bool refresh) {
    const std::string key =
        proj.device + "|" + proj.family + "|" + proj.speed.value_or("") + "|" + root_;
    if (!refresh) {
        const auto cached = devices_.find(key);
        if (cached != devices_.end()) return *cached->second;
    }
    if (proj.device.empty()) throw ToolError("Project has no device in the .al");

    td::TdSession& session = EnsureSession();
    const td::EnsureDeviceResult ensured = td::EnsureDevice(proj, session, td_choice_->root);
    if (!ensured.ok) {
        throw ToolError("No device library supports " + proj.device +
                        (ensured.error.empty() ? std::string() : (": " + ensured.error)));
    }
    // 对应 Python load_device_info 的四次 query_current_package
    const std::string fio = session.Run("query_current_package -fio");
    const std::string clock = session.Run("query_current_package -clock_io");
    const std::string lvds = session.Run("query_current_package -lvds");
    const std::string cfg = session.Run("query_current_package -cfg_io");

    auto info = std::make_unique<td::DeviceInfo>(td::BuildDeviceInfo(fio, clock, lvds, cfg));
    info->device = proj.device;
    info->family = proj.family;
    if (info->db.empty()) info->db = ensured.db;
    if (info->package.empty()) info->package = LowerAscii(proj.device);
    if (info->pins.empty()) {
        throw ToolError("query_current_package -fio returned no pins (device " + proj.device + ")");
    }
    auto& slot = devices_[key];
    slot = std::move(info);
    return *slot;
}

// =============================================================================
//  顶层端口：文本扫描（HDL 索引就绪前的降级）
// =============================================================================

namespace {

// 在文本里找 `module <top>`，返回模块头（到第一个 ';' 或 ')' 结束）的起始偏移与文本
bool FindModuleHeader(const std::string& text, const std::string& top, size_t* header_begin,
                      size_t* header_end) {
    // 两个捕获组 + 两个参数：RE2 的变参只对应捕获组 1..n（见 text_util.h 的说明），
    // 传的参数多于捕获组数量时 FindAndConsumeN 直接返回 false。
    const re2::RE2& pattern = []() -> const re2::RE2& {
        static const re2::RE2 re(R"(\b(module\s+([A-Za-z_][A-Za-z0-9_$]*)))");
        return re;
    }();
    absl::string_view rest(text.data(), text.size());
    re2::StringPiece statement;  // 组 1：`module <name>`（含关键字）
    re2::StringPiece name;       // 组 2：模块名
    while (true) {
        re2::RE2::Arg arg_statement(&statement);
        re2::RE2::Arg arg_name(&name);
        const re2::RE2::Arg* args[2] = {&arg_statement, &arg_name};
        if (!re2::RE2::FindAndConsumeN(&rest, pattern, args, 2)) return false;
        if (statement.size() == 0) return false;
        const std::string found(name.data(), name.size());
        if (found != top) continue;
        const size_t match_end = text.size() - rest.size();
        const size_t match_begin = match_end - statement.size();
        size_t cursor = match_end;
        while (cursor < text.size() && text[cursor] != ';' && text[cursor] != ')') ++cursor;
        *header_begin = match_begin;
        *header_end = std::min(text.size(), cursor + 1);
        return true;
    }
}

}  // namespace

std::vector<ScannedPort> Bridge::ScanTopPorts(const project::Project& proj) const {
    std::vector<ScannedPort> ports;
    if (proj.top.empty()) return ports;

    std::string text;
    size_t header_begin = 0;
    size_t header_end = 0;
    for (const project::AlFile* file : proj.HdlFiles()) {
        const std::string content = ReadTextFile(file->path);
        if (content.empty()) continue;
        if (FindModuleHeader(content, proj.top, &header_begin, &header_end)) {
            text = content;
            break;
        }
    }
    if (text.empty()) return ports;
    return ScanPortHeader(text.substr(header_begin, header_end - header_begin), header_begin);
}
// =============================================================================
//  符号层（阶段二 A 批，11 个工具）
// =============================================================================
//  数据来源分两类，刻意不混用：
//
//  1) **复用 provider::LspServer 的公开 handler**（document_symbols / workspace_symbols /
//     definition / references / hover / project_structure）。
//     这些 handler 已经与 Python 版逐字段对齐过；在 MCP 里重写一遍只会引入第二个偏差源。
//     features 是**无状态纯函数**（index -> LSP JSON），不持有 transport，
//     所以不存在"LSP 通知写进 MCP stdout"的风险 —— 这是选它而不是 LspServer 的原因。
//
//  2) **直接用 hdl::HdlIndex**（module_info / duplicate_modules / search_hierarchy /
//     symbol_path / call_hierarchy）。
//     这 5 个在 C++ 里**没有**现成的 feature API（Python 的 ide.py / nav.py 尚未移植），
//     而 LspServer 也没有对应 handler。这里按 Python 的算法逐行移植；
//     将来它们应该上移到 feature 层（hdl 或新的 ide 模块），MCP 只保留 JSON 转换。
//
//  两种来源的**位置约定不同**，转换必须逐处确认：
//     LspServer / hdl  都输出 0-based (line, character)
//     MCP 输出          1-based (line, column)  —— 与 Python bridge 一致
//   所以凡是把位置交给 agent 的字段，都过 one_based_range()；而 Python 原样透传的字段
//   （duplicate_modules / search_hierarchy 的 line）必须保持 0-based，不能"顺手改成 1-based"。
// =============================================================================

namespace {

// LSP DocumentSymbolKind -> Python mcp_bridge.SYMBOL_KIND 的字符串
const char* SymbolKindName(int kind) {
    switch (kind) {
        case 1: return "file";
        case 2: return "module";
        case 3: return "namespace";
        case 4: return "package";
        case 5: return "class";
        case 6: return "method";
        case 7: return "property";
        case 8: return "field";
        case 9: return "constructor";
        case 10: return "enum";
        case 11: return "interface";
        case 12: return "function";
        case 13: return "variable";
        case 14: return "constant";
        case 15: return "string";
        case 16: return "number";
        case 17: return "boolean";
        case 18: return "array";
        case 19: return "object";
        case 20: return "key";
        case 21: return "null";
        case 22: return "enumMember";
        case 23: return "struct";
        case 24: return "event";
        case 25: return "operator";
        case 26: return "typeParameter";
        default: return "unknown";
    }
}

// 对应 Python _one_based_range：LSP 的 0-based range -> MCP 的 1-based {start,end}
Json OneBasedRange(const Json& range) {
    Json out = Json::object();
    const Json start = range.is_object() && range.contains("start") ? range["start"] : Json::object();
    const Json end = range.is_object() && range.contains("end") ? range["end"] : Json::object();
    Json start_out = Json::object();
    start_out["line"] = (start.is_object() ? start.value("line", 0) : 0) + 1;
    start_out["column"] = (start.is_object() ? start.value("character", 0) : 0) + 1;
    Json end_out = Json::object();
    end_out["line"] = (end.is_object() ? end.value("line", 0) : 0) + 1;
    end_out["column"] = (end.is_object() ? end.value("character", 0) : 0) + 1;
    out["start"] = std::move(start_out);
    out["end"] = std::move(end_out);
    return out;
}

// 对应 Python _file_of：{file, relative}
Json FileOf(const std::string& path, const project::Project& proj) {
    Json out = Json::object();
    out["file"] = path;
    if (!path.empty()) out["relative"] = RelativeTo(path, proj.dir);
    return out;
}

// 对应 Python _location：{file, relative, range(1-based), line, column}
Json LocationJson(const Json& location, const project::Project& proj) {
    if (!location.is_object()) return Json(nullptr);
    const std::string uri = location.value("uri", std::string());
    Json out = FileOf(platform::uri_to_path(uri), proj);
    const Json range = OneBasedRange(location.value("range", Json::object()));
    out["range"] = range;
    out["line"] = range["start"]["line"];
    out["column"] = range["start"]["column"];
    return out;
}

// 对应 Python _symbol：LSP DocumentSymbol -> MCP 形状
Json SymbolJson(const Json& symbol, const std::string& path, const project::Project& proj,
                int depth) {
    if (!symbol.is_object()) return Json(nullptr);
    Json out = Json::object();
    out["name"] = symbol.value("name", std::string());
    const int kind = symbol.value("kind", 0);
    out["kind"] = SymbolKindName(kind);
    out["kindId"] = symbol.contains("kind") ? symbol["kind"] : Json(nullptr);
    out["detail"] = symbol.value("detail", std::string());
    out["range"] = OneBasedRange(symbol.value("range", Json::object()));
    if (symbol.contains("selectionRange") && symbol["selectionRange"].is_object()) {
        out["selectionRange"] = OneBasedRange(symbol["selectionRange"]);
    }
    if (depth < 8 && symbol.contains("children") && symbol["children"].is_array()) {
        Json children = Json::array();
        int count = 0;
        for (const Json& child : symbol["children"]) {
            if (count++ >= 1000) break;
            children.push_back(SymbolJson(child, path, proj, depth + 1));
        }
        out["children"] = std::move(children);
    }
    const Json file = FileOf(path, proj);
    out["file"] = file["file"];
    if (file.contains("relative")) out["relative"] = file["relative"];
    return out;
}

// 对应 Python _hover_text：把 LSP hover 的 contents 归一成一个 markdown 字符串
std::string HoverText(const Json& hover) {
    if (hover.is_null() || !hover.is_object()) return std::string();
    const auto it = hover.find("contents");
    if (it == hover.end()) return std::string();
    if (it->is_string()) return it->get<std::string>();
    if (it->is_object()) return it->value("value", std::string());
    if (it->is_array()) {
        std::string joined;
        for (const Json& item : *it) {
            std::string value;
            if (item.is_string()) {
                value = item.get<std::string>();
            } else if (item.is_object()) {
                value = item.value("value", std::string());
            }
            if (value.empty()) continue;
            if (!joined.empty()) joined += "\n\n";
            joined += value;
        }
        return joined;
    }
    return std::string();
}

// Python ide.module_parents：模块名 -> [(父模块, 实例名)]
std::map<std::string, std::vector<std::pair<const hdl::Module*, std::string>>> ModuleParents(
    const hdl::HdlIndex& index) {
    std::map<std::string, std::vector<std::pair<const hdl::Module*, std::string>>> parents;
    for (const hdl::Instance* instance : index.AllInstances()) {
        const hdl::ParsedFile* file = index.File(instance->file);
        if (file == nullptr) continue;
        const hdl::ModuleInfo* info = hdl::HdlIndex::ModuleAt(*file, instance->line);
        if (info == nullptr) continue;
        parents[instance->module].emplace_back(&info->module, instance->name);
    }
    return parents;
}

// Python ide.modules_in_file 的排序语义：按 line 升序
std::vector<const hdl::Module*> ModulesSortedByName(const hdl::HdlIndex& index) {
    std::vector<const hdl::Module*> modules;
    for (const std::string& path : index.Paths()) {
        const hdl::ParsedFile* file = index.File(path);
        if (file == nullptr) continue;
        for (const hdl::ModuleInfo& info : file->modules) modules.push_back(&info.module);
    }
    std::sort(modules.begin(), modules.end(),
              [](const hdl::Module* a, const hdl::Module* b) { return a->name < b->name; });
    return modules;
}

// call_hierarchy 的"被例化项"输出（Python 的 instances() 内嵌函数）
Json InstanceEntries(const std::vector<const hdl::Instance*>& instances) {
    Json out = Json::array();
    int count = 0;
    for (const hdl::Instance* instance : instances) {
        if (count++ >= 500) break;
        Json item = Json::object();
        item["module"] = instance->module;
        item["instance"] = instance->name;
        item["file"] = instance->file;
        item["line"] = instance->line + 1;
        item["column"] = instance->character + 1;
        out.push_back(std::move(item));
    }
    return out;
}

// Python nav.py：模块的 outgoing（同文件、定义行范围内的例化）/ incoming（module==name）
struct CallHierarchyData {
    const hdl::Module* definition = nullptr;
    std::vector<const hdl::Instance*> outgoing;
    std::vector<const hdl::Instance*> incoming;
};

std::optional<CallHierarchyData> CallHierarchyForModule(const hdl::HdlIndex& index,
                                                        const std::string& module_name) {
    if (module_name.empty()) return std::nullopt;
    const hdl::Module* module = index.ModuleByName(module_name);
    if (module == nullptr) return std::nullopt;
    CallHierarchyData data;
    data.definition = module;
    const long long kUnbounded = 1LL << 30;
    const long long end_line = module->end_line > 0 ? module->end_line : kUnbounded;
    for (const hdl::Instance* instance : index.AllInstances()) {
        if (instance->file == module->file && instance->line >= module->line &&
            static_cast<long long>(instance->line) <= end_line) {
            data.outgoing.push_back(instance);
        }
        if (instance->module == module_name) data.incoming.push_back(instance);
    }
    return data;
}

}  // namespace

// -----------------------------------------------------------------------------
//  LspServer / HDL 索引的惰性装配
// -----------------------------------------------------------------------------

// 说明：符号类的 JSON 形状由 `server::features`（LspServer 与 MCP 共用的无状态层）
// 产出，这里**不需要**构造 LspServer —— 也就不需要担心它的通知写进 MCP 的 stdout。
// 这是比"复用 LspServer handler"更干净的分层：features 不持有状态、不碰传输。
const hdl::HdlIndex& Bridge::EnsureHdlIndex(const project::Project& proj) {
    if (hdl_index_ && hdl_project_ == proj.al_path) return *hdl_index_;
    if (!hdl_cache_) hdl_cache_ = std::make_unique<hdl::ParseCache>();
    hdl::BuildOptions options;
    options.cache = hdl_cache_.get();
    // MCP 没有"未保存缓冲区"的概念：一律读磁盘（对应 Python 的 doc fallback）
    const hdl::TextLoader loader = [](const std::string& path) -> std::optional<std::string> {
        return platform::try_read_file_utf8(path);
    };
    hdl::BuildResult built = hdl::BuildIndex(proj, loader, options);
    hdl_index_ = std::make_unique<hdl::HdlIndex>(std::move(built.index));
    hdl_project_ = proj.al_path;
    return *hdl_index_;
}

std::vector<constraints::Port> Bridge::TopPorts(const project::Project& proj,
                                                std::string* source) {
    std::vector<constraints::Port> ports;
    if (source != nullptr) *source = "text-scan";

    // 优先 HDL 索引：它是 server/符号工具同一个数据源，不会与 hover 的端口信息打架
    if (!proj.top.empty()) {
        const hdl::HdlIndex& index = EnsureHdlIndex(proj);
        const hdl::Module* module = index.ModuleByName(proj.top);
        if (module != nullptr && !module->ports.empty()) {
            for (const hdl::Port& scanned : module->ports) {
                constraints::Port port;
                port.name = scanned.name;
                port.direction = scanned.direction;
                port.width = scanned.width;
                port.msb = scanned.msb;
                port.lsb = scanned.lsb;
                port.line = scanned.line;
                port.character = scanned.character;
                port.file = scanned.file;
                ports.push_back(std::move(port));
            }
            if (source != nullptr) *source = "hdl-index";
            return ports;
        }
    }

    // 回落：文本扫描（索引里没有 top 模块 / 端口为空时）
    for (const ScannedPort& scanned : ScanTopPorts(proj)) {
        constraints::Port port;
        port.name = scanned.name;
        port.direction = scanned.direction;
        port.width = scanned.width;
        if (scanned.width > 1) {
            port.msb = scanned.msb;
            port.lsb = scanned.lsb;
        }
        port.line = scanned.line;
        port.character = scanned.character;
        port.file = proj.top;
        ports.push_back(std::move(port));
    }
    return ports;
}

// -----------------------------------------------------------------------------
//  1) project_structure —— server::features::ModuleTree（含环路保护 / 节点预算）
// -----------------------------------------------------------------------------
Json Bridge::ProjectStructure(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);

    bool include_ports = false;
    bool include_primitives = false;
    int max_depth = 24;
    if (const auto it = arguments.find("include_ports");
        it != arguments.end() && it->is_boolean()) {
        include_ports = it->get<bool>();
    }
    if (const auto it = arguments.find("include_primitives");
        it != arguments.end() && it->is_boolean()) {
        include_primitives = it->get<bool>();
    }
    if (const auto it = arguments.find("max_depth");
        it != arguments.end() && it->is_number_integer()) {
        max_depth = std::clamp(it->get<int>(), 1, 64);
    }
    if (arguments.contains("max_nodes")) {
        // server 侧有自己的 TreeBudget；这里只校验取值范围（与 schema 的 1..50000 一致）
        const int max_nodes = arguments.value("max_nodes", 5000);
        if (max_nodes < 1 || max_nodes > 50000) {
            throw ToolError("max_nodes must be between 1 and 50000");
        }
    }

    const hdl::HdlIndex& index = EnsureHdlIndex(proj);
    const Json tree = server::features::ModuleTree(index, proj, include_primitives);
    if (!tree.is_object() || !tree.value("ok", false)) {
        throw ToolError(tree.is_object() ? tree.value("error", std::string("no project hierarchy"))
                                         : std::string("no project hierarchy"));
    }

    // 对应 Python 的 convert()：MCP 用 1-based 位置，字段名保持 moduleTree 的 camelCase
    std::function<Json(const Json&, int)> convert = [&](const Json& node, int depth) -> Json {
        if (!node.is_object()) return Json(nullptr);
        const Json range = OneBasedRange(node.value("range", Json::object()));
        Json out = Json::object();
        out["module"] = node.value("module", std::string());
        out["instance"] = node.value("instance", std::string());
        out["file"] = node.value("file", std::string());
        out["line"] = range["start"]["line"];
        out["column"] = range["start"]["column"];
        out["defined"] = node.value("defined", false);
        out["blackbox"] = node.value("blackbox", false);
        out["recursive"] = node.value("recursive", false);
        out["portCount"] = node.value("portCount", 0);
        if (node.contains("usedAt") && node["usedAt"].is_object()) {
            out["usedAt"] = LocationJson(node["usedAt"], proj);
        }
        if (include_ports) {
            const hdl::Module* definition = index.ModuleByName(node.value("module", std::string()));
            if (definition != nullptr) {
                Json ports = Json::array();
                int count = 0;
                for (const hdl::Port& port : definition->ports) {
                    if (count++ >= 500) break;
                    Json item = Json::object();
                    item["name"] = port.name;
                    item["direction"] = port.direction;
                    item["width"] = port.width;
                    ports.push_back(std::move(item));
                }
                out["ports"] = std::move(ports);
                Json parameters = Json::array();
                for (const std::string& name : definition->parameters) parameters.push_back(name);
                out["parameters"] = std::move(parameters);
            }
        }
        if (depth < max_depth && node.contains("children") && node["children"].is_array()) {
            Json children = Json::array();
            for (const Json& child : node["children"]) children.push_back(convert(child, depth + 1));
            out["children"] = std::move(children);
        }
        return out;
    };

    Json out = Json::object();
    out["ok"] = true;
    out["project"] = proj.name;
    out["root"] = convert(tree.value("root", Json(nullptr)), 0);
    out["modulesSeen"] = tree.value("modulesSeen", 0);
    out["truncated"] = tree.value("truncated", false);
    return out;
}

// -----------------------------------------------------------------------------
//  2) document_symbols / workspace_symbols / definition / references / hover
//     —— 全部走 server::features（LspServer 与 MCP 共用的无状态转换层）
//     features 只吃 (index, path, line, character) 并返回 LSP 形状的 JSON，
//     不持有状态、不碰传输，所以这里**不需要**构造 LspServer。
// -----------------------------------------------------------------------------
Json Bridge::DocumentSymbolsTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);
    const std::string path = ResolveFile(arguments.value("file", std::string()), proj);

    const hdl::HdlIndex& index = EnsureHdlIndex(proj);
    const Json result = server::features::DocumentSymbols(index, path);

    Json symbols = Json::array();
    int count = 0;
    for (const Json& symbol : result) {
        if (count++ >= 500) break;
        symbols.push_back(SymbolJson(symbol, path, proj, 0));
    }
    Json out = Json::object();
    out["ok"] = true;
    out["file"] = path;
    out["symbols"] = std::move(symbols);
    return out;
}

Json Bridge::WorkspaceSymbolsTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);
    const std::string query = arguments.value("query", std::string());
    int limit = 200;
    if (const auto it = arguments.find("limit"); it != arguments.end() && it->is_number_integer()) {
        limit = std::clamp(it->get<int>(), 1, 1000);
    }

    const hdl::HdlIndex& index = EnsureHdlIndex(proj);
    const Json result = server::features::WorkspaceSymbols(index, query);

    Json symbols = Json::array();
    int count = 0;
    for (const Json& symbol : result) {
        if (count++ >= limit) break;
        Json item = Json::object();
        item["name"] = symbol.value("name", std::string());
        item["kind"] = SymbolKindName(symbol.value("kind", 0));
        item["containerName"] = symbol.value("containerName", std::string());
        item["location"] =
            symbol.contains("location") ? LocationJson(symbol["location"], proj) : Json(nullptr);
        symbols.push_back(std::move(item));
    }
    Json out = Json::object();
    out["ok"] = true;
    out["symbols"] = std::move(symbols);
    return out;
}

Json Bridge::DefinitionTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);
    const std::string path = ResolveFile(arguments.value("file", std::string()), proj);
    const int line = std::max(0, arguments.value("line", 1) - 1);
    const int column = std::max(0, arguments.value("column", 1) - 1);

    const hdl::HdlIndex& index = EnsureHdlIndex(proj);
    const Json result = server::features::Definition(index, path, line, column);

    Json locations = Json::array();
    if (result.is_array()) {
        for (const Json& location : result) {
            if (locations.size() >= 200) break;
            locations.push_back(LocationJson(location, proj));
        }
    } else if (result.is_object()) {
        locations.push_back(LocationJson(result, proj));
    }
    Json out = Json::object();
    out["ok"] = true;
    out["locations"] = std::move(locations);
    return out;
}

Json Bridge::ReferencesTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);
    const std::string path = ResolveFile(arguments.value("file", std::string()), proj);
    const int line = std::max(0, arguments.value("line", 1) - 1);
    const int column = std::max(0, arguments.value("column", 1) - 1);
    bool include_declaration = true;
    if (const auto it = arguments.find("include_declaration");
        it != arguments.end() && it->is_boolean()) {
        include_declaration = it->get<bool>();
    }

    const hdl::HdlIndex& index = EnsureHdlIndex(proj);
    const Json result = server::features::References(index, path, line, column, include_declaration);

    Json references = Json::array();
    int count = 0;
    for (const Json& location : result) {
        if (count++ >= 500) break;
        references.push_back(LocationJson(location, proj));
    }
    Json out = Json::object();
    out["ok"] = true;
    out["references"] = std::move(references);
    return out;
}

Json Bridge::HoverTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);
    const std::string path = ResolveFile(arguments.value("file", std::string()), proj);
    const int line = std::max(0, arguments.value("line", 1) - 1);
    const int column = std::max(0, arguments.value("column", 1) - 1);

    const hdl::HdlIndex& index = EnsureHdlIndex(proj);
    const Json result = server::features::Hover(index, path, line, column);

    Json out = Json::object();
    out["ok"] = true;
    out["hover"] = HoverText(result);
    return out;
}
// -----------------------------------------------------------------------------
//  3) module_info / duplicate_modules / search_hierarchy / symbol_path / call_hierarchy
//     —— 直接的 HDL 索引查询（Python ide.py / nav.py 的移植）
// -----------------------------------------------------------------------------
Json Bridge::ModuleInfoTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);
    const std::string module_name = arguments.value("module", std::string());
    if (module_name.empty()) throw ToolError("Missing required argument: module");

    const hdl::HdlIndex& index = EnsureHdlIndex(proj);
    const hdl::Module* module = index.ModuleByName(module_name);
    if (module == nullptr) throw ToolError("Module not found: " + module_name);

    Json ports = Json::array();
    for (const hdl::Port& port : module->ports) {
        Json item = Json::object();
        item["name"] = port.name;
        item["direction"] = port.direction;
        item["net"] = port.net;
        item["msb"] = port.msb.has_value() ? Json(*port.msb) : Json(nullptr);
        item["lsb"] = port.lsb.has_value() ? Json(*port.lsb) : Json(nullptr);
        item["width"] = port.width;
        item["line"] = port.line;
        item["character"] = port.character;
        item["file"] = port.file;
        ports.push_back(std::move(item));
    }
    Json parameters = Json::array();
    for (const std::string& name : module->parameters) parameters.push_back(name);

    Json instances = Json::array();
    for (const hdl::Instance* instance : index.AllInstances()) {
        if (instance->module != module_name) continue;
        if (instances.size() >= 200) break;
        Json item = Json::object();
        item["instance"] = instance->name;
        item["file"] = instance->file;
        item["line"] = instance->line + 1;
        instances.push_back(std::move(item));
    }

    Json out = Json::object();
    out["ok"] = true;
    out["module"] = module->name;
    out["file"] = module->file;
    out["line"] = module->line + 1;
    out["endLine"] = module->end_line + 1;
    out["ports"] = std::move(ports);
    out["parameters"] = std::move(parameters);
    out["instances"] = std::move(instances);
    return out;
}

Json Bridge::DuplicateModulesTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);
    const hdl::HdlIndex& index = EnsureHdlIndex(proj);

    // Python ide.duplicate_modules：按模块名分组，>=2 个定义就算重复。
    // ⚠ 注意 line / end_line 是 **0-based**（Python 原样透传，没有 +1）；不要"顺手"改成 1-based。
    std::map<std::string, std::vector<const hdl::Module*>> groups;
    for (const hdl::Module* module : ModulesSortedByName(index)) {
        groups[module->name].push_back(module);
    }

    Json duplicates = Json::array();
    for (const auto& [name, modules] : groups) {
        if (modules.size() < 2) continue;
        Json definitions = Json::array();
        for (const hdl::Module* module : modules) {
            Json item = Json::object();
            item["file"] = module->file;
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

Json Bridge::SearchHierarchyTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);
    const std::string query = arguments.value("query", std::string());
    std::string kind = "both";
    if (const auto it = arguments.find("kind"); it != arguments.end() && it->is_string()) {
        kind = LowerAscii(it->get<std::string>());
    }
    int limit = 200;
    if (const auto it = arguments.find("limit"); it != arguments.end() && it->is_number_integer()) {
        limit = std::clamp(it->get<int>(), 1, 2000);
    }
    const std::string needle = LowerAscii(query);
    if (needle.empty()) {
        Json out = Json::object();
        out["ok"] = true;
        out["matches"] = Json::array();
        return out;
    }

    const hdl::HdlIndex& index = EnsureHdlIndex(proj);

    Json matches = Json::array();
    std::set<std::string> seen;
    const auto add = [&](Json entry) {
        const std::string key = entry.dump();
        if (!seen.insert(key).second) return;
        matches.push_back(std::move(entry));
    };

    // 模块名命中
    for (const hdl::Module* module : ModulesSortedByName(index)) {
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
        if (static_cast<int>(matches.size()) >= limit) break;
    }

    Json trimmed = Json::array();
    int count = 0;
    for (const Json& item : matches) {
        if (count++ >= limit) break;
        if (kind == "module" || kind == "instance") {
            if (item.value("kind", std::string()) != kind) continue;
        }
        trimmed.push_back(item);
    }
    Json out = Json::object();
    out["ok"] = true;
    out["matches"] = std::move(trimmed);
    return out;
}

Json Bridge::SymbolPathTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);
    const std::string path = ResolveFile(arguments.value("file", std::string()), proj);
    const int line = std::max(0, arguments.value("line", 1) - 1);
    const int character = std::max(0, arguments.value("column", 1) - 1);

    const hdl::HdlIndex& index = EnsureHdlIndex(proj);

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
        Json out = Json::object();
        out["ok"] = false;
        out["error"] = "No copyable symbol under the cursor";
        return out;
    }

    const hdl::ParsedFile* file = index.File(path);
    const hdl::ModuleInfo* module =
        file != nullptr ? hdl::HdlIndex::ModuleAt(*file, line) : nullptr;
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
    while (current != nullptr && !proj.top.empty() && current->module.name != proj.top) {
        const std::string current_name = current->module.name;
        if (!visited.insert(current_name).second) break;
        const auto it = parents.find(current_name);
        if (it == parents.end()) break;
        const std::pair<const hdl::Module*, std::string>* chosen = nullptr;
        for (const auto& candidate : it->second) {
            if (candidate.first->name != current_name && !candidate.first->name.empty()) {
                chosen = &candidate;
                break;
            }
        }
        if (chosen == nullptr) break;
        chain.insert(chain.begin(), chosen->second);
        // 继续向上：父模块的 ModuleInfo 由索引按 (文件, 行) 反查
        const hdl::Module* parent_module = chosen->first;
        const hdl::ParsedFile* parent_file = index.File(parent_module->file);
        if (parent_file == nullptr) {
            current = nullptr;
            break;
        }
        const hdl::ModuleInfo* parent_info =
            hdl::HdlIndex::ModuleAt(*parent_file, parent_module->line);
        if (parent_info == nullptr || parent_info->module.name == current_name) {
            current = nullptr;
            break;
        }
        current = parent_info;
    }

    std::string instance_path;
    for (const std::string& segment : chain) {
        if (!instance_path.empty()) instance_path += "/";
        instance_path += segment;
    }
    std::string full_path = instance_path.empty() ? name : instance_path + "/" + name;

    Json out = Json::object();
    out["ok"] = true;
    out["name"] = name;
    out["path"] = full_path;
    out["instancePath"] = instance_path;
    out["module"] = module->module.name;
    return out;
}

Json Bridge::CallHierarchyTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);
    std::string direction = "both";
    if (const auto it = arguments.find("direction"); it != arguments.end() && it->is_string()) {
        direction = LowerAscii(it->get<std::string>());
    }

    const hdl::HdlIndex& index = EnsureHdlIndex(proj);

    std::string module_name = arguments.value("module", std::string());
    if (module_name.empty()) {
        // 没有 module 参数 -> 用光标位置解析（Python nav.call_hierarchy）
        const std::string file_value = arguments.value("file", std::string());
        if (file_value.empty()) throw ToolError("file/line/column or module is required");
        const std::string path = ResolveFile(file_value, proj);
        const int line = std::max(0, arguments.value("line", 1) - 1);
        const int character = std::max(0, arguments.value("column", 1) - 1);
        if (const std::optional<hdl::SymbolAtResult> symbol = index.SymbolAt(path, line, character)) {
            if (symbol->kind == "module") {
                module_name = symbol->name;
            } else if (symbol->kind == "instance") {
                module_name = symbol->module;
            }
        }
    }

    const std::optional<CallHierarchyData> found =
        module_name.empty() ? std::nullopt : CallHierarchyForModule(index, module_name);
    if (!found.has_value()) {
        Json out = Json::object();
        out["ok"] = true;
        out["module"] = "";
        out["incoming"] = Json::array();
        out["outgoing"] = Json::array();
        return out;
    }

    const hdl::Module* definition = found->definition;
    Json definition_json = Json(nullptr);
    if (definition != nullptr) {
        Json location = FileOf(definition->file, proj);
        Json range = Json::object();
        Json start = Json::object();
        start["line"] = definition->line + 1;
        start["column"] = definition->character + 1;
        range["start"] = start;
        range["end"] = start;
        location["range"] = std::move(range);
        location["line"] = definition->line + 1;
        location["column"] = definition->character + 1;
        definition_json = std::move(location);
    }

    Json out = Json::object();
    out["ok"] = true;
    out["module"] = module_name;
    out["definition"] = std::move(definition_json);
    out["incoming"] = (direction == "both" || direction == "incoming")
                          ? InstanceEntries(found->incoming)
                          : Json::array();
    out["outgoing"] = (direction == "both" || direction == "outgoing")
                          ? InstanceEntries(found->outgoing)
                          : Json::array();
    return out;
}
// =============================================================================
//  工具分派
// =============================================================================

Json Bridge::Call(std::string_view handler, const Json& arguments) {
    const std::string name(handler);
    if (name == "project_list") return ProjectList(arguments);
    if (name == "project_info") return ProjectInfo(arguments);
    if (name == "project_files") return ProjectFiles(arguments);
    if (name == "project_reports") return ProjectReports(arguments);
    if (name == "diagnostics") return DiagnosticsTool(arguments);
    if (name == "device_info") return DeviceInfoTool(arguments);
    if (name == "constraints_status") return ConstraintsStatus(arguments);
    if (name == "constraint_audit") return ConstraintAudit(arguments);
    if (name == "td_info") return TdInfoTool(arguments);
    if (name == "read_file") return ReadFileTool(arguments);
    if (name == "search_text") return SearchTextTool(arguments);
    if (name == "flow_status") return FlowStatus(arguments);
    if (name == "flow_log") return FlowLog(arguments);
    if (name == "flow_issues") return FlowIssues(arguments);
    if (name == "flow_wait") return FlowWait(arguments);
    if (name == "flow_run") return FlowRunTool(arguments);
    if (name == "flow_cancel") return FlowCancelTool(arguments);
    if (name == "strategy_compare") return StrategyCompareTool(arguments);
    if (name == "cables") return CablesTool(arguments);
    if (name == "completion") return CompletionTool(arguments);
    if (name == "instantiation") return InstantiationTool(arguments);
    if (name == "project_rename") return ProjectRenameTool(arguments);
    if (name == "project_add_source") return ProjectAddSourceTool(arguments);
    // ---- 阶段二 A 批：符号类 ----
    if (name == "project_structure") return ProjectStructure(arguments);
    if (name == "document_symbols") return DocumentSymbolsTool(arguments);
    if (name == "workspace_symbols") return WorkspaceSymbolsTool(arguments);
    if (name == "search_hierarchy") return SearchHierarchyTool(arguments);
    if (name == "definition") return DefinitionTool(arguments);
    if (name == "references") return ReferencesTool(arguments);
    if (name == "hover") return HoverTool(arguments);
    if (name == "symbol_path") return SymbolPathTool(arguments);
    if (name == "call_hierarchy") return CallHierarchyTool(arguments);
    if (name == "module_info") return ModuleInfoTool(arguments);
    if (name == "duplicate_modules") return DuplicateModulesTool(arguments);

    // ---- 显式占位：依赖尚未移植的模块 ----
    // 仍依赖尚未移植的能力：代码动作 / 重命名编辑（需要编辑构造层）
    if (name == "code_actions" || name == "rename_edits") {
        return Placeholder("LSP-side code action / rename edit constructors are not exposed "
                           "as a reusable feature API yet");
    }
    if (name == "format") {
        return Placeholder("Verible formatter bridge not ported yet");
    }
    if (name == "analysis_run") {
        return Placeholder("TD DRC/CDC analysis (fpgalsp.runAnalysis) is not implemented "
                           "in the C++ port yet; flow itself is available via fpga_flow_run");
    }
    if (name == "project_update") {
        return Placeholder("open_project -update / check_project_version are not implemented "
                           "in the C++ port yet (project_rename / project_add_source do work "
                           "on the .al itself)");
    }

    throw ToolError("Tool implementation missing: " + name);
}

// =============================================================================
//  工程类工具
// =============================================================================

Json Bridge::ProjectList(const Json& arguments) {
    std::string base = root_;
    if (const auto it = arguments.find("root"); it != arguments.end() && it->is_string() &&
                                                     !it->get<std::string>().empty()) {
        base = normalize_path(it->get<std::string>());
    }
    const std::vector<std::string> found = project::DiscoverProjects(base, 6);
    Json projects = Json::array();
    int index = 0;
    for (const std::string& al : found) {
        if (index++ >= 100) break;
        const project::Project parsed = project::ParseAl(al);
        Json item = Json::object();
        item["name"] = parsed.name;
        item["al"] = parsed.al_path;
        item["dir"] = parsed.dir;
        item["family"] = parsed.family;
        item["device"] = parsed.device;
        item["top"] = parsed.top;
        item["hdl"] = static_cast<int>(parsed.HdlFiles().size());
        item["warnings"] = static_cast<int>(parsed.warnings.size());
        projects.push_back(std::move(item));
    }
    Json out = Json::object();
    out["ok"] = true;
    out["root"] = base;
    out["projects"] = std::move(projects);
    out["selected"] = active_al_;
    return out;
}

Json Bridge::ProjectInfo(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);

    Json runs = Json::array();
    const std::string runs_dir = path_join(proj.dir, proj.name + "_Runs");
    if (IsDirectory(runs_dir)) {
        for (const std::string& name : ListSubdirectories(runs_dir)) {
            if (name == "best_result") continue;
            runs.push_back(name);
        }
    }

    Json files = Json::object();
    files["hdl"] = static_cast<int>(proj.HdlFiles().size());
    files["verilog"] = static_cast<int>(proj.VerilogFiles().size());
    files["vhdl"] = static_cast<int>(proj.VhdlFiles().size());
    files["adc"] = static_cast<int>(proj.AdcFiles().size());
    files["sdc"] = static_cast<int>(proj.SdcFiles().size());
    files["ip"] = static_cast<int>(proj.IpFiles().size());
    files["excluded"] = static_cast<int>(proj.NotInBuild().size());

    Json warnings = Json::array();
    for (const std::string& warning : proj.warnings) warnings.push_back(warning);

    bool device_cached = false;
    for (const auto& [key, value] : devices_) {
        if (value != nullptr && value->device == proj.device) device_cached = true;
    }

    Json out = Json::object();
    out["ok"] = true;
    out["project"] = proj.name;
    out["al"] = proj.al_path;
    out["dir"] = proj.dir;
    out["family"] = proj.family;
    out["device"] = proj.device;
    out["speed"] = proj.speed.value_or(std::string());
    out["top"] = proj.top;
    out["td_version"] = proj.td_version;
    out["files"] = std::move(files);
    out["warnings"] = std::move(warnings);
    out["runs"] = std::move(runs);
    out["deviceCached"] = device_cached;
    out["td"] = TdSummary();
    return out;
}

Json Bridge::ProjectFiles(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);
    std::string kind = "all";
    if (const auto it = arguments.find("kind"); it != arguments.end() && it->is_string()) {
        kind = LowerAscii(it->get<std::string>());
    }
    bool include_excluded = false;
    if (const auto it = arguments.find("include_excluded");
        it != arguments.end() && it->is_boolean()) {
        include_excluded = it->get<bool>();
    }

    Json files = Json::array();
    int total = 0;
    bool truncated = false;
    for (const project::AlFile& file : proj.files) {
        const std::string file_kind(project::FileKindName(file.kind));
        if (kind == "hdl") {
            if (file_kind != "verilog" && file_kind != "vhdl") continue;
        } else if (kind != "all" && file_kind != kind) {
            continue;
        }
        if (!include_excluded && !file.InBuild()) continue;
        ++total;
        if (files.size() >= 2000) {
            truncated = true;
            continue;
        }
        Json item = Json::object();
        item["file"] = file.path;
        item["relative"] = file.rel;
        item["kind"] = file_kind;
        item["compileOrder"] = file.compile_order;
        item["enabled"] = file.enabled;
        item["excluded"] = file.excluded;
        item["exists"] = file.exists;
        item["inBuild"] = file.InBuild();
        item["resolvedVia"] = std::string(project::ResolvedViaName(file.resolved_via));
        files.push_back(std::move(item));
    }

    Json out = Json::object();
    out["ok"] = true;
    out["project"] = proj.name;
    out["files"] = std::move(files);
    out["truncated"] = truncated;
    return out;
}

Json Bridge::ProjectReports(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);

    std::string stage = "all";
    std::string section = "summary";
    int limit = 100;
    bool include_raw = false;
    if (const auto it = arguments.find("stage"); it != arguments.end() && it->is_string()) {
        stage = LowerAscii(it->get<std::string>());
    }
    if (const auto it = arguments.find("section"); it != arguments.end() && it->is_string()) {
        section = LowerAscii(it->get<std::string>());
    }
    if (const auto it = arguments.find("limit"); it != arguments.end() && it->is_number_integer()) {
        limit = it->get<int>();
    }
    if (const auto it = arguments.find("include_raw");
        it != arguments.end() && it->is_boolean()) {
        include_raw = it->get<bool>();
    }
    limit = std::clamp(limit, 1, 1000);

    const reports::ReportsBundle bundle = reports::BuildReports(proj);
    const Json data = bundle.ToJson();
    return SummarizeReports(data, proj.name, stage, section, limit, include_raw);
}

// =============================================================================
//  诊断
// =============================================================================

namespace {

// 工程里的 ADC/SDC 诊断（对应 Python 的 offline 表里 adc/sdc 两部分）
void CollectConstraintDiagnostics(const project::Project& proj,
                                  const std::vector<constraints::Port>& ports,
                                  std::vector<std::pair<std::string, lsp::Diagnostic>>* out) {
    for (const project::AlFile* file : proj.AdcFiles()) {
        const std::string text = ReadTextFile(file->path);
        const std::vector<constraints::adc::Entry> entries = constraints::adc::Parse(text);
        for (const lsp::Diagnostic& diagnostic :
             constraints::adc::Diagnostics(entries, ports, nullptr, text)) {
            if (diagnostic.code == "BIT-8123") continue;  // 落在顶层模块端口声明行，由 hdl 层发布
            out->emplace_back(file->path, diagnostic);
        }
    }
    for (const project::AlFile* file : proj.SdcFiles()) {
        const std::string text = ReadTextFile(file->path);
        const std::vector<constraints::sdc::Command> commands = constraints::sdc::Parse(text);
        for (const lsp::Diagnostic& diagnostic :
             constraints::sdc::Diagnostics(commands, ports, nullptr)) {
            out->emplace_back(file->path, diagnostic);
        }
    }
}

}  // namespace

Json Bridge::DiagnosticsTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);

    std::vector<std::string> sources;
    bool have_sources = false;
    if (const auto it = arguments.find("sources"); it != arguments.end() && it->is_array()) {
        have_sources = true;
        for (const Json& item : *it) {
            if (item.is_string()) sources.push_back(LowerAscii(item.get<std::string>()));
        }
    }
    std::string file_value;
    if (const auto it = arguments.find("file"); it != arguments.end() && it->is_string()) {
        file_value = it->get<std::string>();
    }
    if (!have_sources) {
        sources = {"hdl", "adc", "sdc"};
        if (!file_value.empty()) sources.push_back("verible");
    }
    int limit = 500;
    if (const auto it = arguments.find("limit"); it != arguments.end() && it->is_number_integer()) {
        limit = it->get<int>();
    }
    limit = std::clamp(limit, 1, 2000);

    const auto want = [&sources](const char* name) {
        return std::find(sources.begin(), sources.end(), name) != sources.end();
    };

    std::string file_path;
    if (!file_value.empty()) file_path = ResolveFile(file_value, proj);

    // 端口表：优先 HDL 索引（portsSource 会如实标注来源）
    std::string ports_source;
    const std::vector<constraints::Port> ports = TopPorts(proj, &ports_source);

    std::vector<std::pair<std::string, lsp::Diagnostic>> raw;
    if (want("adc") || want("sdc")) CollectConstraintDiagnostics(proj, ports, &raw);

    Json diagnostics = Json::array();
    std::set<std::string> seen;
    for (const auto& [path, diagnostic] : raw) {
        if (!file_path.empty() && path != file_path) continue;
        const std::string source = diagnostic.source;
        const bool is_adc = source.find("adc") != std::string::npos;
        const bool is_sdc = source.find("sdc") != std::string::npos;
        if (is_adc && !want("adc")) continue;
        if (is_sdc && !want("sdc")) continue;
        const Json item = DiagnosticJson(diagnostic, path, proj);
        const std::string key = item.dump();
        if (!seen.insert(key).second) continue;
        if (diagnostics.size() < static_cast<size_t>(limit)) diagnostics.push_back(item);
    }

    Json unavailable = Json::object();
    bool td_ok = false;
    // TD 层（可选，需要真实 TD；Python 同）
    if (want("td")) {
        try {
            td::TdSession& session = EnsureSession();
            const lsp::DiagnosticList td_diags = td::RunReadHdl(proj, session);
            // read_hdl 会关闭 stdout 管道（PYTHON-BUGS.md P-6）：会话必须丢弃
            session.Close();
            session_.reset();
            td_ok = true;
            for (const lsp::Diagnostic& diagnostic : td_diags) {
                const std::string path = proj.al_path;
                if (!file_path.empty() && path != file_path) continue;
                if (diagnostics.size() >= static_cast<size_t>(limit)) break;
                diagnostics.push_back(DiagnosticJson(diagnostic, path, proj));
            }
        } catch (const std::exception& error) {
            unavailable["td"] = error.what();
        }
    }
    if (want("hdl")) unavailable["hdl"] = "HDL diagnostics come from the LSP server tier; this MCP build only surfaces adc/sdc/td";
    if (want("verible")) unavailable["verible"] = "Verible lint bridge not ported yet";
    (void)td_ok;

    Json out = Json::object();
    out["ok"] = true;
    out["project"] = proj.name;
    out["file"] = file_path;
    out["diagnostics"] = std::move(diagnostics);
    out["truncated"] = false;
    out["portsSource"] = ports_source;
    out["unavailable"] = std::move(unavailable);
    return out;
}

// =============================================================================
//  器件 / 约束
// =============================================================================

Json Bridge::DeviceInfoTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);

    bool include_pins = false;
    bool refresh = false;
    std::string pin_filter;
    int limit = 200;
    if (const auto it = arguments.find("include_pins");
        it != arguments.end() && it->is_boolean()) {
        include_pins = it->get<bool>();
    }
    if (const auto it = arguments.find("refresh"); it != arguments.end() && it->is_boolean()) {
        refresh = it->get<bool>();
    }
    if (const auto it = arguments.find("pin_filter"); it != arguments.end() && it->is_string()) {
        pin_filter = LowerAscii(it->get<std::string>());
    }
    if (const auto it = arguments.find("limit"); it != arguments.end() && it->is_number_integer()) {
        limit = it->get<int>();
    }
    limit = std::clamp(limit, 1, 5000);

    const td::DeviceInfo device = EnsureDeviceInfo(proj, refresh);

    Json out = Json::object();
    out["ok"] = true;
    out["device"] = proj.device;
    out["family"] = proj.family;
    out["package"] = device.package;
    out["db"] = device.db;
    out["pinCount"] = static_cast<int>(device.pins.size());

    if (include_pins) {
        Json pins = Json::array();
        for (const auto& [key, pin] : device.pins) {
            const std::string name = LowerAscii(pin.pe_raw.empty() ? pin.pe : pin.pe_raw);
            const std::string location = LowerAscii(pin.location);
            if (!pin_filter.empty() && name.find(pin_filter) == std::string::npos &&
                location.find(pin_filter) == std::string::npos) {
                continue;
            }
            Json item = Json::object();
            item["pin"] = name;
            item["location"] = location;
            item["intBank"] = pin.int_bank.has_value() ? Json(*pin.int_bank) : Json(nullptr);
            item["extBank"] = pin.ext_bank.has_value() ? Json(*pin.ext_bank) : Json(nullptr);
            item["clockCapable"] = pin.clock_capable;
            item["lvds"] = pin.lvds;
            pins.push_back(std::move(item));
            if (static_cast<int>(pins.size()) >= limit) break;
        }
        out["pins"] = std::move(pins);
    }
    return out;
}

namespace {

bool Contains(const std::vector<int>& bits, int bit) {
    return std::find(bits.begin(), bits.end(), bit) != bits.end();
}

}  // namespace

Json Bridge::ConstraintsStatus(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);

    std::string ports_source;
    const std::vector<constraints::Port> ports = TopPorts(proj, &ports_source);

    std::vector<constraints::adc::Entry> entries;
    Json adc_files = Json::array();
    for (const project::AlFile* file : proj.AdcFiles()) {
        adc_files.push_back(file->path);
        const std::string text = ReadTextFile(file->path);
        const auto parsed = constraints::adc::Parse(text);
        entries.insert(entries.end(), parsed.begin(), parsed.end());
    }

    const constraints::adc::CoverageReport coverage =
        constraints::adc::Coverage(entries, ports, proj.top);

    Json missing = Json::array();
    for (const constraints::Port& port : ports) {
        const auto covered = coverage.covered.find(port.name);
        const bool has_any = covered != coverage.covered.end();
        if (port.width.value_or(1) <= 1) {
            if (!has_any) missing.push_back(port.name);
            continue;
        }
        const std::vector<int> bits = constraints::PortBits(port);
        for (const int bit : bits) {
            if (!has_any || !Contains(covered->second, bit)) {
                missing.push_back(port.name + "[" + std::to_string(bit) + "]");
            }
        }
    }

    Json sdc = Json::array();
    for (const project::AlFile* file : proj.SdcFiles()) {
        const std::string text = ReadTextFile(file->path);
        Json item = Json::object();
        item["file"] = file->path;
        item["relative"] = file->rel;
        item["createClock"] = CountOccurrences(text, "create_clock");
        item["generatedClock"] = CountOccurrences(text, "create_generated_clock");
        item["derivePll"] = CountOccurrences(text, "derive_pll_clocks");
        sdc.push_back(std::move(item));
    }

    Json warnings = Json::array();
    for (const std::string& warning : proj.warnings) warnings.push_back(warning);

    Json out = Json::object();
    out["ok"] = true;
    out["project"] = proj.name;
    out["top"] = proj.top;
    out["portsSource"] = ports_source;
    out["adcFiles"] = std::move(adc_files);
    out["ports"] = static_cast<int>(ports.size());
    out["constrainedPorts"] = static_cast<int>(coverage.covered.size());
    out["missing"] = TrimListTo(missing, 200);
    out["missingCount"] = static_cast<int>(missing.size());
    out["sdc"] = std::move(sdc);
    out["warnings"] = std::move(warnings);
    return out;
}

Json Bridge::ConstraintAudit(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);

    bool include_sdc = true;
    bool include_device = true;
    if (const auto it = arguments.find("include_sdc"); it != arguments.end() && it->is_boolean()) {
        include_sdc = it->get<bool>();
    }
    if (const auto it = arguments.find("include_device");
        it != arguments.end() && it->is_boolean()) {
        include_device = it->get<bool>();
    }

    std::string ports_source;
    const std::vector<constraints::Port> ports = TopPorts(proj, &ports_source);

    std::vector<constraints::adc::Entry> entries;
    Json adc_files = Json::array();
    for (const project::AlFile* file : proj.AdcFiles()) {
        adc_files.push_back(file->path);
        const std::string text = ReadTextFile(file->path);
        const auto parsed = constraints::adc::Parse(text);
        entries.insert(entries.end(), parsed.begin(), parsed.end());
    }
    const constraints::adc::CoverageReport coverage =
        constraints::adc::Coverage(entries, ports, proj.top);

    // 器件：失败不致命（Python 用 device_error 字段报告）
    Json device_json = Json(nullptr);
    std::string device_error;
    bool device_loaded = false;
    if (include_device) {
        try {
            const td::DeviceInfo& device = EnsureDeviceInfo(proj, false);
            device_loaded = true;
            device_json = Json::object();
            device_json["package"] = device.package;
            device_json["db"] = device.db;
            device_json["pinCount"] = static_cast<int>(device.pins.size());
        } catch (const std::exception& error) {
            device_error = error.what();
        }
    }

    // adc::Diagnostics 需要 Json 形状的 device；把 DeviceInfo 转成 Python 的 device dict
    Json device_for_diag = Json(nullptr);
    if (device_loaded) {
        device_for_diag = Json::object();
        Json pins = Json::object();
        for (const auto& [key, pin] : EnsureDeviceInfo(proj, false).pins) {
            Json record = Json::object();
            record["pe"] = LowerAscii(pin.pe_raw.empty() ? pin.pe : pin.pe_raw);
            record["location"] = LowerAscii(pin.location);
            record["type"] = LowerAscii(pin.type);
            if (pin.int_bank.has_value()) record["int_bank"] = *pin.int_bank;
            if (pin.ext_bank.has_value()) record["ext_bank"] = *pin.ext_bank;
            record["clock_capable"] = pin.clock_capable;
            record["lvds"] = pin.lvds;
            pins[key] = std::move(record);
        }
        device_for_diag["pins"] = std::move(pins);
        Json dedicated = Json::array();
        for (const td::DedicatedPin& pin : EnsureDeviceInfo(proj, false).dedicated) {
            Json item = Json::object();
            item["pe"] = LowerAscii(pin.pe);
            item["role"] = pin.role;
            dedicated.push_back(std::move(item));
        }
        device_for_diag["dedicated"] = std::move(dedicated);
    }

    Json issues = Json::array();
    for (const project::AlFile* file : proj.AdcFiles()) {
        const std::string text = ReadTextFile(file->path);
        const auto entries_for_file = constraints::adc::Parse(text);
        const Json* device_ptr = device_for_diag.is_object() ? &device_for_diag : nullptr;
        for (const lsp::Diagnostic& diagnostic :
             constraints::adc::Diagnostics(entries_for_file, ports, device_ptr, text)) {
            Json item = Json::object();
            item["severity"] = SeverityName(diagnostic.severity);
            item["code"] = diagnostic.code.empty() ? "ADC" : diagnostic.code;
            item["message"] = diagnostic.message;
            item["file"] = file->path;
            item["line"] = diagnostic.range.start.line + 1;
            issues.push_back(std::move(item));
        }
    }

    Json sdc_summary = Json::array();
    if (include_sdc) {
        for (const project::AlFile* file : proj.SdcFiles()) {
            const std::string text = ReadTextFile(file->path);
            Json item = Json::object();
            item["file"] = file->path;
            item["createClock"] = CountOccurrences(text, "create_clock");
            item["generatedClock"] = CountOccurrences(text, "create_generated_clock");
            item["derivePll"] = CountOccurrences(text, "derive_pll_clocks");
            sdc_summary.push_back(std::move(item));
        }
    }

    const int total = coverage.total_bits;
    const int constrained = coverage.constrained_bits;
    const double percent = total > 0 ? std::round(constrained * 100.0 / total * 100.0) / 100.0 : 0.0;

    Json missing = Json::object();
    for (const auto& [name, bits] : coverage.missing) missing[name] = bits;
    Json unconstrained = Json::array();
    for (const std::string& name : coverage.unconstrained_ports) unconstrained.push_back(name);
    Json extra = Json::array();
    for (const constraints::adc::Entry& entry : coverage.extra_ports) {
        Json item = Json::object();
        item["port"] = entry.port;
        item["line"] = entry.line + 1;
        extra.push_back(std::move(item));
    }
    Json duplicate_ports = Json::array();
    for (const constraints::adc::DuplicatePort& duplicate : coverage.duplicate_ports) {
        Json item = Json::object();
        item["port"] = duplicate.port;
        item["lines"] = duplicate.lines;
        duplicate_ports.push_back(std::move(item));
    }
    Json duplicate_pins = Json::array();
    for (const constraints::adc::DuplicatePin& duplicate : coverage.duplicate_pins) {
        Json item = Json::object();
        item["pin"] = duplicate.pin;
        item["ports"] = duplicate.ports;
        item["lines"] = duplicate.lines;
        duplicate_pins.push_back(std::move(item));
    }

    Json warnings = Json::array();
    for (const std::string& warning : proj.warnings) warnings.push_back(warning);

    Json out = Json::object();
    out["ok"] = true;
    out["project"] = proj.name;
    out["top"] = proj.top;
    out["portsSource"] = ports_source;
    out["adcFiles"] = std::move(adc_files);
    out["totalBits"] = total;
    out["constrainedBits"] = constrained;
    out["coveragePercent"] = percent;
    out["missing"] = std::move(missing);
    out["unconstrainedPorts"] = std::move(unconstrained);
    out["extraPorts"] = std::move(extra);
    out["duplicatePorts"] = std::move(duplicate_ports);
    out["duplicatePins"] = std::move(duplicate_pins);
    out["issues"] = TrimListTo(issues, 500);
    out["issueCount"] = static_cast<int>(issues.size());
    out["deviceLoaded"] = device_loaded;
    out["deviceError"] = device_error;
    out["device"] = device_json;
    out["sdc"] = std::move(sdc_summary);
    out["warnings"] = std::move(warnings);
    return out;
}

// =============================================================================
//  TD / 文件 / 搜索
// =============================================================================

Json Bridge::TdInfoTool(const Json&) {
    Json out = Json::object();
    out["ok"] = true;
    out["td"] = TdSummary();
    return out;
}

Json Bridge::ReadFileTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);

    std::string file_value;
    if (const auto it = arguments.find("file"); it != arguments.end() && it->is_string()) {
        file_value = it->get<std::string>();
    }
    const std::string path = SafeProjectFile(file_value, proj);

    int offset = 1;
    int limit = 400;
    int max_chars = 200000;
    if (const auto it = arguments.find("offset"); it != arguments.end() && it->is_number_integer()) {
        offset = it->get<int>();
    }
    if (const auto it = arguments.find("limit"); it != arguments.end() && it->is_number_integer()) {
        limit = it->get<int>();
    }
    if (const auto it = arguments.find("max_chars");
        it != arguments.end() && it->is_number_integer()) {
        max_chars = it->get<int>();
    }
    offset = std::max(1, offset);
    limit = std::clamp(limit == 0 ? 400 : limit, 1, 5000);
    max_chars = std::clamp(max_chars == 0 ? 200000 : max_chars, 1000, 1000000);

    const std::string text = ReadTextFile(path);
    const std::vector<std::string> lines = SplitLines(text);
    const int total = static_cast<int>(lines.size());
    const int start = std::min(offset - 1, total);
    const int end = std::min(total, start + limit);
    std::string selected;
    for (int index = start; index < end; ++index) {
        if (index != start) selected.push_back('\n');
        selected += lines[static_cast<size_t>(index)];
    }
    bool truncated = end < total;
    if (static_cast<int>(selected.size()) > max_chars) {
        selected = selected.substr(0, static_cast<size_t>(max_chars));
        truncated = true;
    }

    Json out = Json::object();
    out["ok"] = true;
    out["file"] = path;
    out["relative"] = RelativeTo(path, proj.dir);
    out["lineStart"] = start + 1;
    out["lineEnd"] = end;
    out["totalLines"] = total;
    out["text"] = selected;
    out["truncated"] = truncated;
    return out;
}

Json Bridge::SearchTextTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);

    std::string query;
    if (const auto it = arguments.find("query"); it != arguments.end() && it->is_string()) {
        query = it->get<std::string>();
    }
    if (query.empty()) throw ToolError("query must not be empty");

    std::string path_filter;
    std::string glob;
    bool regex_mode = false;
    bool case_sensitive = false;
    int limit = 100;
    int context = 0;
    if (const auto it = arguments.find("path"); it != arguments.end() && it->is_string()) {
        path_filter = it->get<std::string>();
    }
    if (const auto it = arguments.find("glob"); it != arguments.end() && it->is_string()) {
        glob = it->get<std::string>();
    }
    if (const auto it = arguments.find("regex"); it != arguments.end() && it->is_boolean()) {
        regex_mode = it->get<bool>();
    }
    if (const auto it = arguments.find("case_sensitive");
        it != arguments.end() && it->is_boolean()) {
        case_sensitive = it->get<bool>();
    }
    if (const auto it = arguments.find("limit"); it != arguments.end() && it->is_number_integer()) {
        limit = it->get<int>();
    }
    if (const auto it = arguments.find("context"); it != arguments.end() && it->is_number_integer()) {
        context = it->get<int>();
    }
    limit = std::clamp(limit == 0 ? 100 : limit, 1, 1000);
    context = std::clamp(context, 0, 5);

    // 包一层捕获组：RE2 的变参只对应捕获组（见 text_util.h 的说明），
    // 没有捕获组时 FindAndConsumeN 拿不到匹配文本。
    std::string pattern = std::string("(?") + (case_sensitive ? "" : "i") + ")(" +
                          (regex_mode ? query : re2::RE2::QuoteMeta(query)) + ")";
    const re2::RE2 compiled(pattern);
    if (!compiled.ok()) throw ToolError("Invalid regex: " + query);

    std::vector<std::string> files;
    if (!path_filter.empty()) {
        const std::string target = SafeProjectPath(path_filter, proj);
        if (IsDirectory(target)) {
            std::error_code ec;
            for (fs::recursive_directory_iterator it(fs::u8path(target), ec), end;
                 it != end && !ec; it.increment(ec)) {
                const std::string name = ToUtf8(it->path().filename());
                if (it->path().filename() == ".fpgalsp" || name == "__pycache__") {
                    if (it->is_directory(ec)) it.disable_recursion_pending();
                    continue;
                }
                if (it->is_regular_file(ec)) files.push_back(ToUtf8(it->path()));
            }
        } else {
            files.push_back(target);
        }
    } else {
        for (const project::AlFile* file : proj.HdlFiles()) files.push_back(file->path);
        for (const project::AlFile* file : proj.AdcFiles()) files.push_back(file->path);
        for (const project::AlFile* file : proj.SdcFiles()) files.push_back(file->path);
        files.push_back(proj.al_path);
    }

    const std::vector<std::string> suffixes = {".v",  ".sv",  ".vh",  ".svh", ".vhd",
                                               ".vhdl", ".adc", ".sdc", ".al",  ".tcl"};
    Json matches = Json::array();
    int searched = 0;
    bool truncated = false;
    for (const std::string& file_path : files) {
        if (!glob.empty() && !GlobMatch(glob, BaseName(file_path))) continue;
        const std::string lowered = LowerAscii(file_path);
        bool suffix_ok = false;
        for (const std::string& suffix : suffixes) {
            if (lowered.size() >= suffix.size() &&
                lowered.compare(lowered.size() - suffix.size(), suffix.size(), suffix) == 0) {
                suffix_ok = true;
                break;
            }
        }
        if (!suffix_ok) continue;
        std::error_code ec;
        const auto size = fs::file_size(fs::u8path(file_path), ec);
        if (ec || size > 8u * 1024u * 1024u) continue;
        const std::string text = ReadTextFile(file_path);
        const std::vector<std::string> rows = SplitLines(text);
        ++searched;
        for (size_t index = 0; index < rows.size(); ++index) {
            absl::string_view rest(rows[index].data(), rows[index].size());
            re2::StringPiece found;
            while (true) {
                re2::RE2::Arg arg_found(&found);
                const re2::RE2::Arg* args[1] = {&arg_found};
                if (!re2::RE2::FindAndConsumeN(&rest, compiled, args, 1)) break;
                if (found.size() == 0 && rest.empty()) {
                    // 零长匹配：避免死循环（Python 的 finditer 会自动前进一格）
                }
                const size_t match_end = rows[index].size() - rest.size();
                const size_t match_start = match_end - found.size();
                Json item = Json::object();
                item["file"] = file_path;
                item["relative"] = RelativeTo(file_path, proj.dir);
                item["line"] = static_cast<int>(index) + 1;
                item["column"] = static_cast<int>(match_start) + 1;
                item["text"] = Trim(rows[index].substr(0, 500));
                if (context > 0) {
                    Json snippet = Json::array();
                    const int before = std::max<int>(0, static_cast<int>(index) - context);
                    const int after =
                        std::min<int>(static_cast<int>(rows.size()), static_cast<int>(index) + context + 1);
                    for (int row = before; row < after; ++row) snippet.push_back(rows[static_cast<size_t>(row)]);
                    item["context"] = std::move(snippet);
                }
                matches.push_back(std::move(item));
                if (static_cast<int>(matches.size()) >= limit) {
                    truncated = true;
                    break;
                }
                if (found.size() == 0) break;
            }
            if (static_cast<int>(matches.size()) >= limit) break;
        }
        if (static_cast<int>(matches.size()) >= limit) break;
    }

    Json out = Json::object();
    out["ok"] = true;
    out["query"] = query;
    out["files"] = searched;
    out["matches"] = std::move(matches);
    out["count"] = static_cast<int>(out["matches"].size());
    out["truncated"] = truncated;
    return out;
}

// =============================================================================
//  Flow（C++ 还没有 flow 模块：状态恒为 idle，日志/问题按磁盘文件解析）
// =============================================================================

namespace {

Json IdleFlowState() {
    Json flow = Json::object();
    flow["running"] = false;
    flow["stage"] = "idle";
    flow["elapsed"] = 0.0;
    flow["lastMessage"] = "";
    flow["ok"] = Json(nullptr);
    flow["log"] = Json::array();
    return flow;
}

std::string FlowBase(const project::Project& proj) {
    return path_join(proj.dir, ".fpgalsp/flow");
}

std::vector<std::string> FlowDirs(const project::Project& proj) {
    std::vector<std::string> dirs;
    const std::string base = FlowBase(proj);
    std::error_code ec;
    fs::directory_iterator it(fs::u8path(base), ec);
    if (ec) return dirs;
    std::vector<std::pair<std::string, long long>> found;
    for (const fs::directory_entry& entry : it) {
        const std::string name = ToUtf8(entry.path().filename());
        if (name.rfind("flow_", 0) != 0) continue;
        if (!entry.is_directory(ec)) continue;
        const auto mtime = fs::last_write_time(entry.path(), ec);
        found.emplace_back(ToUtf8(entry.path()),
                           ec ? 0 : static_cast<long long>(mtime.time_since_epoch().count()));
    }
    std::sort(found.begin(), found.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    for (auto& [path, stamp] : found) dirs.push_back(path);
    return dirs;
}

std::string SelectFlowDir(const project::Project& proj, const std::string& run_id) {
    if (!run_id.empty()) {
        const std::string name = Trim(run_id);
        if (name.rfind("flow_", 0) != 0 || BaseName(name) != name) {
            throw ToolError("Invalid run_id: " + run_id);
        }
        const std::string path = path_join(FlowBase(proj), name);
        if (!IsDirectory(path)) throw ToolError("Flow run not found: " + name);
        return path;
    }
    const std::vector<std::string> dirs = FlowDirs(proj);
    return dirs.empty() ? std::string() : dirs.front();
}

std::vector<std::string> RunLogPaths(const project::Project& proj) {
    std::vector<std::string> paths;
    const std::string runs = path_join(proj.dir, proj.name + "_Runs");
    std::error_code ec;
    fs::directory_iterator it(fs::u8path(runs), ec);
    if (!ec) {
        for (const fs::directory_entry& entry : it) {
            if (!entry.is_directory(ec)) continue;
            const std::string log = ToUtf8(entry.path()) + "/run.log";
            if (IsFile(log)) paths.push_back(log);
        }
    }
    return paths;
}

Json TailText(const std::string& path, int max_chars) {
    Json out = Json::object();
    out["path"] = path;
    std::error_code ec;
    const auto size = fs::file_size(fs::u8path(path), ec);
    if (ec) {
        out["error"] = "cannot stat file";
        out["text"] = "";
        out["truncated"] = false;
        out["size"] = 0;
        return out;
    }
    std::string text = ReadTextFile(path);
    const bool truncated = static_cast<int>(text.size()) > max_chars;
    if (truncated) text = text.substr(text.size() - static_cast<size_t>(max_chars));
    out["text"] = text;
    out["truncated"] = truncated;
    out["size"] = static_cast<long long>(size);
    out["lines"] = CountOccurrences(text, "\n") + (text.empty() ? 0 : 1);
    return out;
}

Json TailLines(const std::string& text, int lines) {
    const std::vector<std::string> rows = SplitLines(text);
    const int begin = std::max<int>(0, static_cast<int>(rows.size()) - lines);
    std::string out;
    for (int index = begin; index < static_cast<int>(rows.size()); ++index) {
        if (!out.empty()) out.push_back('\n');
        out += rows[static_cast<size_t>(index)];
    }
    return out;
}

// ---- 顶层端口文本扫描（HDL 索引就绪前的降级实现）----
//
// 支持的头写法：
//    module top( input clk, input [2:0] w, output reg [3:0] q );
//    module top(a, b);            // 非 ANSI 头：只给出方向未知的端口名
// 解析规则（保守，宁可少报 diganostics 也不虚构端口）：
//    · 只在模块头（module ... ; / )）范围内扫描
//    · 方向关键字 input/output/inout 之后可选 wire/reg/logic/signed/unsigned
//    · 可选 [msb:lsb] 位宽
//    · 之后是逗号分隔的一个或多个名字（`input a, b` 与 `input a` 都支持）
//    · 端口名不带位选择；非 ANSI 头里单独出现的名字视为 direction="inout" width=1
std::vector<ScannedPort> ScanPortHeader(std::string_view header, size_t header_offset);


std::vector<ScannedPort> ScanPortHeader(std::string_view raw_header, size_t header_offset) {
    std::vector<ScannedPort> ports;
    const std::string blanked = BlankComments(raw_header);
    const std::string_view header(blanked);

    // 逐字符扫描，遇到方向关键字就解析一小段声明
    const auto is_space = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
    };
    size_t position = 0;
    std::string current_direction;

    while (position < header.size()) {
        // 找下一个标识符
        while (position < header.size() && !IsIdentStart(header[position])) ++position;
        if (position >= header.size()) break;
        const size_t word_begin = position;
        while (position < header.size() && IsIdentChar(header[position])) ++position;
        const std::string word(header.substr(word_begin, position - word_begin));

        if (word == "input" || word == "output" || word == "inout") {
            current_direction = word;
            // 跳过类型修饰与位宽
            int width = 1;
            int msb = 0;
            int lsb = 0;
            size_t cursor = position;
            bool saw_bracket = false;
            while (cursor < header.size()) {
                while (cursor < header.size() && is_space(header[cursor])) ++cursor;
                if (cursor >= header.size()) break;
                if (header[cursor] == '[') {
                    const size_t close = header.find(']', cursor);
                    if (close == std::string_view::npos) break;
                    const std::string spec(header.substr(cursor + 1, close - cursor - 1));
                    const size_t colon = spec.find(':');
                    if (colon != std::string::npos) {
                        try {
                            msb = std::stoi(Trim(spec.substr(0, colon)));
                            lsb = std::stoi(Trim(spec.substr(colon + 1)));
                            width = std::abs(msb - lsb) + 1;
                            saw_bracket = true;
                        } catch (...) {
                            width = 1;
                        }
                    }
                    cursor = close + 1;
                    continue;
                }
                if (IsIdentStart(header[cursor])) {
                    const size_t begin = cursor;
                    while (cursor < header.size() && IsIdentChar(header[cursor])) ++cursor;
                    const std::string qualifier(header.substr(begin, cursor - begin));
                    if (qualifier == "wire" || qualifier == "reg" || qualifier == "logic" ||
                        qualifier == "signed" || qualifier == "unsigned" || qualifier == "var") {
                        continue;
                    }
                    // 到了端口名：先记录，再看后面是否还有逗号分隔的兄弟
                    ScannedPort port;
                    port.name = qualifier;
                    port.direction = current_direction;
                    port.width = saw_bracket ? width : 1;
                    port.msb = saw_bracket ? msb : 0;
                    port.lsb = saw_bracket ? lsb : 0;
                    // 行号：header 内的换行累计
                    const size_t absolute = header_offset + begin;
                    const int line = static_cast<int>(
                        std::count(header.begin(), header.begin() + static_cast<long>(begin), '\n'));
                    const size_t last_newline =
                        header.rfind('\n', begin == 0 ? 0 : begin - 1);
                    port.line = line;
                    port.character =
                        (last_newline == std::string_view::npos)
                            ? static_cast<int>(begin)
                            : static_cast<int>(begin - last_newline - 1);
                    (void)absolute;
                    ports.push_back(std::move(port));

                    // 兄弟名字：`, name` / `, [msb:lsb] name`
                    while (true) {
                        size_t look = cursor;
                        while (look < header.size() && is_space(header[look])) ++look;
                        if (look >= header.size() || header[look] != ',') break;
                        ++look;
                        while (look < header.size() && is_space(header[look])) ++look;
                        if (look < header.size() && header[look] == '[') {
                            const size_t close = header.find(']', look);
                            if (close == std::string_view::npos) break;
                            look = close + 1;
                            while (look < header.size() && is_space(header[look])) ++look;
                        }
                        if (look >= header.size() || !IsIdentStart(header[look])) break;
                        const size_t name_begin = look;
                        while (look < header.size() && IsIdentChar(header[look])) ++look;
                        const std::string name(header.substr(name_begin, look - name_begin));
                        // 逗号后面若是下一个方向的声明，说明这个逗号属于参数列表
                        if (name == "input" || name == "output" || name == "inout") break;
                        ScannedPort sibling;
                        sibling.name = name;
                        sibling.direction = current_direction;
                        sibling.width = saw_bracket ? width : 1;
                        sibling.msb = saw_bracket ? msb : 0;
                        sibling.lsb = saw_bracket ? lsb : 0;
                        sibling.line = static_cast<int>(
                            std::count(header.begin(), header.begin() + static_cast<long>(name_begin), '\n'));
                        const size_t newline = header.rfind('\n', name_begin == 0 ? 0 : name_begin - 1);
                        sibling.character = (newline == std::string_view::npos)
                                                ? static_cast<int>(name_begin)
                                                : static_cast<int>(name_begin - newline - 1);
                        ports.push_back(std::move(sibling));
                        cursor = look;
                    }
                    position = cursor;
                    break;
                }
                // 别的 token（比如 '='）-> 放弃这条声明
                position = cursor;
                current_direction.clear();
                break;
            }
            continue;
        }
    }
    return ports;
}

// 对应 Python _flow_issue_from_line：只认带 ERROR/WARNING/CRITICAL-WARNING 的行
std::optional<Json> FlowIssueFromLine(const std::string& line) {
    const std::string text = Trim(line);
    if (text.empty() || text.front() == '=') return std::nullopt;

    static const re2::RE2 severity_re(R"((?i)\b(CRITICAL-WARNING|WARNING|ERROR)\b)");
    static const re2::RE2 code_re(R"(\b([A-Z][A-Z0-9]{1,5}-[0-9]{3,5})\b)");
    static const re2::RE2 file_re(
        R"(([A-Za-z]:[\\/][^()\n]+?|[A-Za-z_][A-Za-z0-9_./\\-]*\.(?:v|sv|vh|svh|vhd|vhdl|adc|sdc|al))\(([0-9]+)\))");

    std::string severity;
    if (!re2::RE2::PartialMatch(text, severity_re, &severity)) return std::nullopt;
    for (char& c : severity) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    std::string code;
    if (!re2::RE2::PartialMatch(text, code_re, &code)) code = "FLOW";
    std::string file;
    std::string line_text;
    int line_number = 0;
    if (re2::RE2::PartialMatch(text, file_re, &file, &line_text)) {
        try {
            line_number = std::stoi(line_text);
        } catch (...) {
            line_number = 0;
        }
    }

    Json out = Json::object();
    out["severity"] = (severity == "ERROR") ? "error" : "warning";
    out["code"] = code;
    out["message"] = text.substr(0, 1200);
    out["file"] = file;
    out["line"] = line_number;
    return out;
}

// 对应 Python mcp_bridge._report_summary
Json TrimList(const Json& value, int limit) {
    Json out = Json::array();
    if (!value.is_array()) return out;
    int count = 0;
    for (const Json& item : value) {
        if (count++ >= limit) break;
        out.push_back(item);
    }
    return out;
}

const Json& Member(const Json& object, const char* key) {
    static const Json kNull = Json(nullptr);
    if (object.is_object()) {
        const auto it = object.find(key);
        if (it != object.end()) return *it;
    }
    return kNull;
}

Json SummarizeReports(const Json& data, const std::string& project_name, std::string stage,
                      std::string section, int limit, bool include_raw) {
    const Json* source = &data;
    if ((stage == "syn" || stage == "phy") && data.contains(stage) && data[stage].is_object()) {
        source = &data[stage];
    }

    Json result = Json::object();
    result["ok"] = data.value("ok", false);
    result["project"] = project_name;
    result["stage"] = stage;
    result["runs"] = Member(Member(data, "artifacts"), "runs");
    // Python: `source.get("files") or data.get("files") or {}` -> 缺失时是空对象，不是 null
    const Json& files_source = Member(*source, "files").is_object() ? Member(*source, "files")
                                                                    : Member(data, "files");
    result["files"] = files_source.is_object() ? files_source : Json::object();
    if (!data.value("ok", false)) {
        const Json& error = Member(data, "error");
        result["error"] = error.is_string() ? error : Json("No reports available");
        return result;
    }

    const Json& qor = Member(*source, "qor").is_object() ? Member(*source, "qor") : Member(data, "qor");
    const Json& timing = Member(*source, "timing").is_object() ? Member(*source, "timing")
                                                              : Member(data, "timing");
    const Json& area = Member(*source, "area").is_object() ? Member(*source, "area")
                                                           : Member(data, "area");
    const Json& flow = Member(*source, "flowStatus").is_object() ? Member(*source, "flowStatus")
                                                                 : Member(data, "flowStatus");
    const Json& analysis = Member(data, "analysis");

    if (section == "summary" || section == "all") {
        Json summary = Json::object();
        summary["qorStep"] = Member(qor, "step").is_string() ? Member(qor, "step") : Json("");
        summary["metrics"] = TrimList(Member(qor, "metrics"), limit);
        summary["suggestions"] = TrimList(Member(qor, "suggestions"), limit);
        summary["timing"] = Member(timing, "summary").is_object() ? Member(timing, "summary")
                                                                  : Json::object();
        summary["coverage"] = Member(timing, "coverage");
        summary["areaRows"] = TrimList(Member(area, "rows"), limit);
        summary["flowSteps"] = TrimList(Member(flow, "steps"), limit);
        result["summary"] = std::move(summary);

        const Json& drc = Member(analysis, "drc");
        const Json& cdc = Member(analysis, "cdc");
        Json drc_cdc = Json::object();
        drc_cdc["drcViolations"] = Member(drc, "violations");
        drc_cdc["drcClean"] = Member(drc, "clean");
        drc_cdc["drcRules"] = TrimList(Member(drc, "rules"), limit);
        drc_cdc["cdcCounts"] = Member(cdc, "counts").is_object() ? Member(cdc, "counts")
                                                                 : Json::object();
        result["drcCdc"] = std::move(drc_cdc);
    }
    if (section == "qor" || section == "all") {
        Json qor_out = Json::object();
        qor_out["step"] = Member(qor, "step").is_string() ? Member(qor, "step") : Json("");
        qor_out["metrics"] = TrimList(Member(qor, "metrics"), limit);
        qor_out["suggestions"] = TrimList(Member(qor, "suggestions"), limit);
        result["qor"] = std::move(qor_out);
    }
    if (section == "timing" || section == "all") {
        Json timing_out = Json::object();
        if (timing.is_object()) {
            for (auto it = timing.begin(); it != timing.end(); ++it) {
                if (it.key() == "raw" || it.key() == "paths" || it.key() == "groups" ||
                    it.key() == "clock_detail") {
                    continue;
                }
                timing_out[it.key()] = it.value();
            }
        }
        timing_out["paths"] = TrimList(Member(timing, "paths"), limit);
        timing_out["groups"] = TrimList(Member(timing, "groups"), std::max(1, limit / 10));
        result["timing"] = std::move(timing_out);
    }
    if (section == "area" || section == "all") {
        Json area_out = Json::object();
        area_out["rows"] = TrimList(Member(area, "rows"), limit);
        area_out["counts"] = Member(area, "counts").is_object() ? Member(area, "counts")
                                                                : Json::object();
        area_out["modules"] = TrimList(Member(area, "modules"), limit);
        result["area"] = std::move(area_out);
    }
    if (section == "flow" || section == "all") result["flow"] = flow;
    if (include_raw) {
        result["raw"] = Member(*source, "raw");
        result["rawFiles"] = Member(*source, "files");
    }
    return result;
}
}  // namespace

Json Bridge::FlowLog(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);

    std::string run_id;
    std::string kind = "both";
    int tail = 200;
    int max_chars = 200000;
    if (const auto it = arguments.find("run_id"); it != arguments.end() && it->is_string()) {
        run_id = it->get<std::string>();
    }
    if (const auto it = arguments.find("kind"); it != arguments.end() && it->is_string()) {
        kind = LowerAscii(it->get<std::string>());
    }
    if (const auto it = arguments.find("tail"); it != arguments.end() && it->is_number_integer()) {
        tail = it->get<int>();
    }
    if (const auto it = arguments.find("max_chars");
        it != arguments.end() && it->is_number_integer()) {
        max_chars = it->get<int>();
    }
    if (kind != "td" && kind != "run" && kind != "both") {
        throw ToolError("kind must be one of td/run/both");
    }
    tail = std::clamp(tail == 0 ? 200 : tail, 1, 5000);
    max_chars = std::clamp(max_chars == 0 ? 200000 : max_chars, 1000, 1000000);

    const std::string run_dir = SelectFlowDir(proj, run_id);
    if (run_dir.empty()) {
        Json out = Json::object();
        out["ok"] = false;
        out["error"] = "No flow run directory found";
        out["files"] = Json::array();
        return out;
    }

    Json files = Json::array();
    if (kind == "td" || kind == "both") {
        for (const std::string& name : ListFiles(run_dir)) {
            if (name.rfind("td_", 0) != 0) continue;
            if (!(name.size() >= 4 &&
                  (name.compare(name.size() - 4, 4, ".log") == 0 ||
                   name.compare(name.size() - 5, 5, ".logw") == 0))) {
                continue;
            }
            Json data = TailText(path_join(run_dir, name), max_chars);
            data["text"] = TailLines(data["text"].get<std::string>(), tail);
            data["kind"] = "td";
            files.push_back(std::move(data));
        }
    }
    if (kind == "run" || kind == "both") {
        for (const std::string& path : RunLogPaths(proj)) {
            Json data = TailText(path, max_chars);
            data["text"] = TailLines(data["text"].get<std::string>(), tail);
            data["kind"] = "run";
            files.push_back(std::move(data));
        }
    }

    Json out = Json::object();
    out["ok"] = true;
    out["runId"] = BaseName(run_dir);
    out["runDir"] = run_dir;
    out["files"] = std::move(files);
    out["count"] = static_cast<int>(out["files"].size());
    return out;
}

Json Bridge::FlowIssues(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);

    std::string run_id;
    int tail = 2000;
    int limit = 500;
    if (const auto it = arguments.find("run_id"); it != arguments.end() && it->is_string()) {
        run_id = it->get<std::string>();
    }
    if (const auto it = arguments.find("tail"); it != arguments.end() && it->is_number_integer()) {
        tail = it->get<int>();
    }
    if (const auto it = arguments.find("limit"); it != arguments.end() && it->is_number_integer()) {
        limit = it->get<int>();
    }
    tail = std::clamp(tail == 0 ? 2000 : tail, 1, 10000);
    limit = std::clamp(limit == 0 ? 500 : limit, 1, 2000);

    const std::string run_dir = SelectFlowDir(proj, run_id);

    Json issues = Json::array();
    std::set<std::string> seen;
    const auto add_issue = [&](const Json& issue) {
        if (!issue.is_object()) return;
        const std::string key = issue.dump();
        if (!seen.insert(key).second) return;
        if (static_cast<int>(issues.size()) < limit) issues.push_back(issue);
    };

    std::vector<std::string> logs;
    Json markers = Json::array();
    if (!run_dir.empty()) {
        for (const std::string& name : ListFiles(run_dir)) {
            if (name.size() >= 8 && name.compare(name.size() - 8, 8, ".error.f") == 0) {
                markers.push_back(name);
            }
        }
        for (const std::string& name : ListFiles(run_dir)) {
            if (name.rfind("td_", 0) != 0) continue;
            const bool is_log = name.size() >= 4 &&
                                (name.compare(name.size() - 4, 4, ".log") == 0 ||
                                 name.compare(name.size() - 5, 5, ".logw") == 0);
            if (!is_log) continue;
            const Json data = TailText(path_join(run_dir, name), 500000);
            logs.push_back(path_join(run_dir, name));
            for (const std::string& row : SplitLines(TailLines(data["text"].get<std::string>(), tail))) {
                if (auto issue = FlowIssueFromLine(row)) add_issue(*issue);
            }
        }
    }
    for (const std::string& path : RunLogPaths(proj)) {
        const Json data = TailText(path, 500000);
        logs.push_back(path);
        for (const std::string& row : SplitLines(TailLines(data["text"].get<std::string>(), tail))) {
            if (auto issue = FlowIssueFromLine(row)) add_issue(*issue);
        }
    }
    for (const Json& marker : markers) {
        const std::string name = marker.get<std::string>();
        Json issue = Json::object();
        issue["severity"] = "error";
        issue["code"] = "FLOW-STEP";
        issue["message"] = "run step failed: " + name;
        issue["file"] = "";
        issue["line"] = 0;
        add_issue(issue);
    }

    int errors = 0;
    int warnings = 0;
    for (const Json& issue : issues) {
        const std::string severity = issue.value("severity", "");
        if (severity == "error") ++errors;
        if (severity == "warning") ++warnings;
    }
    Json counts = Json::object();
    counts["error"] = errors;
    counts["warning"] = warnings;

    Json out = Json::object();
    out["ok"] = true;
    out["runId"] = run_dir.empty() ? std::string() : BaseName(run_dir);
    out["runDir"] = run_dir;
    out["running"] = false;
    out["stage"] = "idle";
    out["error"] = "";
    out["issues"] = std::move(issues);
    out["counts"] = std::move(counts);
    out["markers"] = std::move(markers);
    out["logs"] = std::move(logs);
    return out;
}

// -----------------------------------------------------------------------------
//  B 批：flow 引擎接入（flow_run / flow_cancel / flow_status / flow_wait /
//        strategy_compare / cables）
// -----------------------------------------------------------------------------
//  实现直接复用 `flow::FlowRunner` / `flow::CompareStrategies` / `flow::Inspect` ——
//  与 LSP 的 `fpgalsp.runFlow` / `compareStrategies` / `cables` 是**同一份实现**
//  （见 src/server/commands.cpp 的对应分支），MCP 这里只做参数抽取。
//
//  ⚠ 三条安全约定：
//  1. **不写 stdout**：FlowRunner 的 notify 回调在 MCP 里丢弃（flowEvent 是 LSP 通知形状，
//     混进 MCP 的 newline 流就是协议污染）；日志走 platform logger。
//  2. **工程切换必须重建 runner**：FlowRunner 绑定 Project 副本，跑在旧工程上比没跑更糟。
//  3. **flow 会往 <project>/.fpgalsp/flow 写日志**（P-4）：调用方要在临时副本上跑，
//     测试里由 e2e 负责 copytree；这里不替调用方改写工程路径。
flow::FlowRunner* Bridge::EnsureFlowRunner(const project::Project& proj) {
    if (flow_runner_ != nullptr && flow_project_ == proj.al_path) return flow_runner_.get();
    if (flow_runner_ != nullptr) {
        // 上一个工程的 runner：先取消再销毁（析构会 join 线程）
        flow_runner_->Cancel();
        flow_runner_.reset();
        flow_project_.clear();
    }
    const std::optional<td::TdChoice> choice = td::FindTd(options_.td_path);
    if (!choice.has_value()) throw ToolError("No TangDynasty installation found");
    td_choice_ = *choice;

    flow::FlowOptions flow_options;
    // notify：MCP 没有 fpgaLsp/flowEvent 通道，明确丢弃（不要接到 stdout）
    const flow::FlowRunner::NotifyFn notify = [](const jsonrpc::Json&) {};
    const flow::FlowRunner::LogFn log = [](int level, std::string_view message) {
        if (level >= 2) {
            fpgalsp::platform::default_logger().warn(std::string("[flow] ") + std::string(message));
        } else {
            fpgalsp::platform::default_logger().info(std::string("[flow] ") + std::string(message));
        }
    };
    flow_runner_ = std::make_unique<flow::FlowRunner>(proj, *choice, notify, log, flow_options);
    flow_project_ = proj.al_path;
    return flow_runner_.get();
}

Json Bridge::FlowRunTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);

    std::vector<std::string> stages;
    if (const auto it = arguments.find("stages"); it != arguments.end() && it->is_array()) {
        for (const Json& item : *it) {
            if (item.is_string()) stages.push_back(item.get<std::string>());
        }
    }
    if (stages.empty()) stages.push_back("all");  // 等价 GUI 的 Generate Bitstream
    int jobs = 6;
    if (const auto it = arguments.find("jobs"); it != arguments.end() && it->is_number_integer()) {
        jobs = std::clamp(it->get<int>(), 1, 64);
    }

    flow::FlowRunner* runner = EnsureFlowRunner(proj);
    return runner->Start(stages, jobs);
}

Json Bridge::FlowCancelTool(const Json& arguments) {
    (void)ProjectFor(arguments);  // 校验工程存在（与 Python 的 self.project(project) 一致）
    // runner 为 nullptr 或不在跑 -> {ok:true, note:"not running"}（客户端契约）
    if (flow_runner_ == nullptr) {
        Json out = Json::object();
        out["ok"] = true;
        out["note"] = "not running";
        return out;
    }
    return flow_runner_->Cancel();
}

Json Bridge::FlowStatus(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);
    const reports::ReportsBundle bundle = reports::BuildReports(proj);
    const Json data = bundle.ToJson();

    Json out = Json::object();
    out["ok"] = true;
    if (flow_runner_ != nullptr && flow_project_ == proj.al_path) {
        out["flow"] = flow_runner_->Status().ToJson();
    } else {
        out["flow"] = IdleFlowState();
    }
    out["artifacts"] = data.contains("artifacts") ? data["artifacts"] : Json::object();
    return out;
}

Json Bridge::FlowWait(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);

    int timeout = 3600;
    int interval = 1;
    if (const auto it = arguments.find("timeout"); it != arguments.end() && it->is_number_integer()) {
        timeout = std::clamp(it->get<int>(), 1, 7200);
    }
    if (const auto it = arguments.find("interval");
        it != arguments.end() && it->is_number_integer()) {
        interval = std::clamp(it->get<int>(), 1, 30);
    }

    if (flow_runner_ == nullptr || flow_project_ != proj.al_path) {
        // 对应 Python：runner 为空 / 别的工程 -> 立即返回，不发进度
        Json out = Json::object();
        out["ok"] = true;
        out["running"] = false;
        out["note"] = "no active flow for this project";
        out["flow"] = IdleFlowState();
        return out;
    }

    const auto started = std::chrono::steady_clock::now();
    const auto elapsed_seconds = [&started]() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    };
    while (true) {
        flow::FlowState state = flow_runner_->Status();
        const double elapsed = elapsed_seconds();
        const std::string message = !state.last_message.empty() ? state.last_message
                                  : !state.stage.empty()     ? state.stage
                                                             : "flow running";
        if (progress_) progress_(elapsed, message);
        if (!state.running) break;
        if (elapsed >= static_cast<double>(timeout)) {
            Json out = Json::object();
            out["ok"] = false;
            out["timedOut"] = true;
            out["running"] = true;
            out["elapsed"] = elapsed;
            out["flow"] = state.ToJson();
            out["issues"] = FlowIssues(arguments);
            return out;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(interval * 100));
    }

    flow::FlowState state = flow_runner_->Status();
    if (progress_) progress_(elapsed_seconds(), "flow finished");
    Json out = Json::object();
    out["ok"] = state.ok.value_or(true);
    out["timedOut"] = false;
    out["running"] = false;
    out["elapsed"] = elapsed_seconds();
    out["flow"] = state.ToJson();
    out["issues"] = FlowIssues(arguments);
    return out;
}

Json Bridge::StrategyCompareTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);

    std::optional<td::TdChoice> choice = td::FindTd(options_.td_path);
    if (!choice.has_value()) throw ToolError("No TangDynasty installation found");
    td_choice_ = *choice;

    std::vector<std::string> wanted;
    if (const auto it = arguments.find("strategies"); it != arguments.end() && it->is_array()) {
        for (const Json& item : *it) {
            if (item.is_string()) wanted.push_back(item.get<std::string>());
        }
    }
    flow::CompareOptions compare_options;
    if (const auto it = arguments.find("jobs"); it != arguments.end() && it->is_number_integer()) {
        compare_options.jobs = std::clamp(it->get<int>(), 1, 64);
    }
    return flow::CompareStrategies(proj, *choice, wanted, compare_options);
}

Json Bridge::CablesTool(const Json&) {
    // 对应 fpgalsp.cables：只读探测，**不烧录**
    const project::Project& proj = ActiveProject();
    std::optional<td::TdChoice> choice = td::FindTd(options_.td_path);
    if (!choice.has_value()) throw ToolError("No TangDynasty installation found");
    td_choice_ = *choice;
    return flow::Inspect(*choice, proj, flow::ProgramOptions{});
}
// -----------------------------------------------------------------------------
//  C 批：补全 / 实例化模板
// -----------------------------------------------------------------------------
//  completion  —— features::Completion（与 LSP 同一实现）+ Python bridge 的过滤/裁剪
//  instantiation —— 按 Python `_instantiation_text` 的纯文本格式生成（无 snippet 占位符）
//  两者都只读：不写文件、不启动 TD。
// -----------------------------------------------------------------------------
Json Bridge::CompletionTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);
    const std::string path = ResolveFile(arguments.value("file", std::string()), proj);
    const int line = std::max(0, arguments.value("line", 1) - 1);
    const int column = std::max(0, arguments.value("column", 1) - 1);

    std::string prefix = LowerAscii(arguments.value("prefix", std::string()));
    int limit = 50;
    if (const auto it = arguments.find("limit"); it != arguments.end() && it->is_number_integer()) {
        limit = std::clamp(it->get<int>(), 1, 500);
    }
    bool include_snippets = true;
    if (const auto it = arguments.find("include_snippets");
        it != arguments.end() && it->is_boolean()) {
        include_snippets = it->get<bool>();
    }

    const hdl::HdlIndex& index = EnsureHdlIndex(proj);
    // completion 需要"当前行内容"判断光标是否在行首/缩进；MCP 没有未保存缓冲区 -> 读磁盘
    const std::string text = ReadTextFile(path);
    const Json result = server::features::Completion(index, path, text, line, column);

    Json items = Json::array();
    bool truncated = false;
    const Json& raw_items = result.contains("items") ? result["items"] : Json::array();
    if (raw_items.is_array()) {
        for (const Json& item : raw_items) {
            if (!item.is_object()) continue;
            // Python：label 可能是 {label: ...} 形状的 TextEdit 关联对象
            std::string label;
            if (item.contains("label") && item["label"].is_string()) {
                label = item["label"].get<std::string>();
            } else if (item.contains("label") && item["label"].is_object()) {
                label = item["label"].value("label", std::string());
            }
            if (!prefix.empty() && LowerAscii(label).rfind(prefix, 0) != 0) continue;
            if (!include_snippets && item.value("insertTextFormat", 1) == 2) continue;

            Json out_item = Json::object();
            out_item["label"] = label;
            out_item["kind"] = SymbolKindName(item.value("kind", 0));
            out_item["detail"] = item.value("detail", std::string());
            out_item["insertText"] = item.value("insertText", std::string());
            out_item["insertTextFormat"] =
                item.contains("insertTextFormat") ? item["insertTextFormat"] : Json(nullptr);
            out_item["documentation"] = HoverText(item.contains("documentation")
                                                      ? Json{{"contents", item["documentation"]}}
                                                      : Json(nullptr));
            items.push_back(std::move(out_item));
            if (static_cast<int>(items.size()) >= limit) {
                truncated = true;
                break;
            }
        }
    }

    Json out = Json::object();
    out["ok"] = true;
    out["items"] = std::move(items);
    out["truncated"] = truncated;
    return out;
}

Json Bridge::InstantiationTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);
    const std::string module_name = arguments.value("module", std::string());
    if (module_name.empty()) throw ToolError("Missing required argument: module");

    const hdl::HdlIndex& index = EnsureHdlIndex(proj);
    const hdl::Module* definition = index.ModuleByName(module_name);
    if (definition == nullptr) throw ToolError("Module not found: " + module_name);

    std::string indent = arguments.value("indent", std::string());
    if (indent.empty()) indent = "    ";
    std::string instance = arguments.value("instance_name", std::string());
    if (instance.empty()) instance = "u_" + definition->name;

    // 逐行对应 Python server.py 的 _instantiation_text（纯文本，无 $1 占位符）
    const std::string default_instance = "u_" + definition->name;
    std::string text;
    if (!definition->parameters.empty()) {
        text += definition->name + " #(\n";
        for (size_t i = 0; i < definition->parameters.size(); ++i) {
            const std::string& parameter = definition->parameters[i];
            const bool last = (i + 1 == definition->parameters.size());
            text += indent + "    ." + parameter + " (" + parameter + ")" + (last ? "" : ",") + "\n";
        }
        text += indent + ") " + default_instance + " (\n";
    } else {
        text += definition->name + " " + default_instance + " (\n";
    }
    for (size_t i = 0; i < definition->ports.size(); ++i) {
        const std::string& port_name = definition->ports[i].name;
        const bool last = (i + 1 == definition->ports.size());
        text += indent + "    ." + port_name + " (" + port_name + ")" + (last ? "" : ",") + "\n";
    }
    text += indent + ");\n";

    // 自定义实例名：与 Python 一样做一次文本替换（默认实例名 -> 指定名）
    if (instance != default_instance) {
        size_t position = 0;
        while ((position = text.find(default_instance, position)) != std::string::npos) {
            text.replace(position, default_instance.size(), instance);
            position += instance.size();
        }
    }

    Json ports = Json::array();
    for (const hdl::Port& port : definition->ports) {
        Json item = Json::object();
        item["name"] = port.name;
        item["direction"] = port.direction;
        item["width"] = port.width;
        ports.push_back(std::move(item));
    }
    Json parameters = Json::array();
    for (const std::string& name : definition->parameters) parameters.push_back(name);

    Json out = Json::object();
    out["ok"] = true;
    out["module"] = definition->name;
    out["instance"] = instance;
    out["text"] = text;
    out["ports"] = std::move(ports);
    out["parameters"] = std::move(parameters);
    return out;
}
// -----------------------------------------------------------------------------
//  C 批：写工程（project_rename / project_add_source）
// -----------------------------------------------------------------------------
//  对应 Python：`mcp_bridge.project_rename/add_source` -> `ide.set_project_name/add_source_file`。
//
//  ⚠ 这是**唯一会修改用户文件**的工具组（MCP schema 里 requires="write"，
//    默认不暴露，必须 --allow-write）。所以流程刻意做重：
//      1. 先把新内容算好（纯字符串操作，不碰文件）
//      2. **原子写**：写同目录临时文件 -> rename 覆盖（失败不会留下半截文件）
//      3. 写完**重新解析 .al 验证**：只有预期字段变化才算成功
//      4. 验证失败 -> 用原文原子写回（回滚），并返回 {ok:false, error}
//  这套"算-写-验-回滚"是为了满足"失败时不要留下半改状态"。
// -----------------------------------------------------------------------------
namespace {

// 原子写：同目录临时文件 + rename 覆盖。失败返回 false（不抛）。
bool WriteFileAtomic(const std::string& path, const std::string& content) {
    const std::string temp = path + ".fpgalsp-tmp";
    if (!platform::write_file_utf8(temp, content)) return false;
    std::error_code ec;
    fs::rename(fs::u8path(temp), fs::u8path(path), ec);
    if (ec) {
        fs::remove(fs::u8path(temp), ec);
        return false;
    }
    return true;
}

std::string NormaliseSlashes(std::string text) {
    for (char& c : text) {
        if (c == '\\') c = '/';
    }
    return text;
}

std::string LowerText(std::string_view text) { return LowerAscii(text); }

// Python ide._relative：相对工程目录、正斜杠
std::string ProjectRelative(const std::string& path, const std::string& project_dir) {
    return NormaliseSlashes(RelativeTo(path, project_dir));
}

}  // namespace

Json Bridge::ProjectRenameTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);
    std::string name = Trim(arguments.value("name", std::string()));
    if (name.empty()) throw ToolError("Project name cannot be empty");
    if (name.find_first_of("<>\"&") != std::string::npos) {
        throw ToolError("Project name cannot contain < > \" &");
    }

    const std::string original = ReadTextFile(proj.al_path);
    if (original.empty()) throw ToolError("Cannot read " + proj.al_path);

    static const re2::RE2 name_tag(R"(<Name>[^<]*</Name>)");
    static const re2::RE2 project_tag(R"(<Project\b[^>]*>)");
    std::string updated;
    {
        const std::string replacement = "<Name>" + name + "</Name>";
        std::string::size_type position = original.find("<Name>");
        bool replaced = false;
        if (position != std::string::npos) {
            const std::string::size_type close = original.find("</Name>", position);
            if (close != std::string::npos) {
                updated = original.substr(0, position) + replacement +
                          original.substr(close + 7);
                replaced = true;
            }
        }
        if (!replaced) {
            std::string match;
            if (!re2::RE2::PartialMatch(original, project_tag, &match)) {
                throw ToolError("No <Project> tag in the .al");
            }
            const std::string::size_type insert_at = original.find(match) + match.size();
            updated = original.substr(0, insert_at) + "\n    <Name>" + name + "</Name>" +
                      original.substr(insert_at);
        }
    }
    (void)name_tag;

    if (updated == original) {
        Json out = Json::object();
        out["ok"] = true;
        out["changed"] = false;
        out["name"] = name;
        return out;
    }

    if (!WriteFileAtomic(proj.al_path, updated)) {
        throw ToolError("Cannot write " + proj.al_path);
    }

    // 验证：重新解析，只允许项目名变化
    const project::Project after = project::ParseAl(proj.al_path);
    if (after.name != name) {
        (void)WriteFileAtomic(proj.al_path, original);  // 回滚
        throw ToolError("Verification failed: project name is still '" + after.name + "' (rolled back)");
    }
    if (after.files.size() != proj.files.size()) {
        (void)WriteFileAtomic(proj.al_path, original);  // 回滚
        throw ToolError("Verification failed: file list changed unexpectedly (rolled back)");
    }

    Json out = Json::object();
    out["ok"] = true;
    out["changed"] = true;
    out["name"] = name;
    out["al"] = proj.al_path;
    return out;
}

Json Bridge::ProjectAddSourceTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);

    const std::string file_value = arguments.value("file", std::string());
    const std::string source = SafeProjectFile(file_value, proj);  // 必须在工程目录内且存在
    const std::string rel = ProjectRelative(source, proj.dir);

    const std::string original = ReadTextFile(proj.al_path);
    if (original.empty()) throw ToolError("Cannot read " + proj.al_path);

    // 定位 <Source_Files> ... </Source_Files>
    const std::string open_tag = "<Source_Files>";
    const std::string close_tag = "</Source_Files>";
    const std::string::size_type open_at = original.find(open_tag);
    const std::string::size_type close_at =
        open_at == std::string::npos ? std::string::npos
                                     : original.find(close_tag, open_at + open_tag.size());
    if (open_at == std::string::npos || close_at == std::string::npos) {
        throw ToolError("No <Source_Files> section in the .al");
    }
    const std::string body =
        original.substr(open_at + open_tag.size(), close_at - open_at - open_tag.size());

    // 已存在（大小写不敏感、斜杠归一）-> added:false
    const std::string rel_lower = LowerText(rel);
    {
        static const re2::RE2 file_path(R"re(<File\s+Path="([^"]+)")re");
        absl::string_view rest(body);
        re2::StringPiece captured;
        while (!rest.empty()) {
            re2::RE2::Arg arg(&captured);
            const re2::RE2::Arg* args[1] = {&arg};
            const size_t before = rest.size();
            if (!re2::RE2::FindAndConsumeN(&rest, file_path, args, 1)) break;
            if (rest.size() >= before) break;
            const std::string existing(captured.data(), captured.size());
            if (LowerText(NormaliseSlashes(existing)) == rel_lower) {
                Json out = Json::object();
                out["ok"] = true;
                out["added"] = false;
                out["rel"] = rel;
                out["compileOrder"] = Json(nullptr);
                return out;
            }
        }
    }

    // 下一个 CompileOrder = max + 1
    int compile_order = 1;
    {
        static const re2::RE2 order_re(R"re(Name="CompileOrder"\s+Val="([0-9]+)")re");
        absl::string_view rest(body);
        re2::StringPiece captured;
        while (!rest.empty()) {
            re2::RE2::Arg arg(&captured);
            const re2::RE2::Arg* args[1] = {&arg};
            const size_t before = rest.size();
            if (!re2::RE2::FindAndConsumeN(&rest, order_re, args, 1)) break;
            if (rest.size() >= before) break;
            try {
                compile_order = std::max(compile_order,
                                         std::stoi(std::string(captured.data(), captured.size())) + 1);
            } catch (...) {
                continue;
            }
        }
    }

    const bool vhdl = LowerText(rel).size() >= 4 &&
                      (LowerText(rel).compare(LowerText(rel).size() - 4, 4, ".vhd") == 0 ||
                       (LowerText(rel).size() >= 5 &&
                        LowerText(rel).compare(LowerText(rel).size() - 5, 5, ".vhdl") == 0));
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

    // 插入点：</Source_Files> 之前；若该行只有空白，则插到行首（保持闭合标签缩进）
    std::string::size_type insert_at = close_at;
    const std::string::size_type line_start =
        original.rfind('\n', insert_at == 0 ? 0 : insert_at - 1);
    const std::string::size_type begin = (line_start == std::string::npos) ? 0 : line_start + 1;
    if (Trim(original.substr(begin, insert_at - begin)).empty()) insert_at = begin;
    const std::string updated =
        original.substr(0, insert_at) + block + original.substr(insert_at);

    if (!WriteFileAtomic(proj.al_path, updated)) {
        throw ToolError("Cannot write " + proj.al_path);
    }

    // 验证：新 .al 必须能解析、文件数 +1、且包含这个 rel
    const project::Project after = project::ParseAl(proj.al_path);
    bool found = false;
    for (const project::AlFile& item : after.files) {
        if (LowerText(NormaliseSlashes(item.rel)) == rel_lower) found = true;
    }
    if (!found || after.files.size() != proj.files.size() + 1) {
        (void)WriteFileAtomic(proj.al_path, original);  // 回滚
        throw ToolError("Verification failed: the new source is not in the .al (rolled back)");
    }

    Json out = Json::object();
    out["ok"] = true;
    out["added"] = true;
    out["rel"] = rel;
    out["compileOrder"] = compile_order;
    out["fileCount"] = static_cast<int>(after.files.size());
    if (arguments.value("update", false)) {
        // .al 改完后 GUI 会调 open_project -update。C++ 侧还没有对应实现：
        // **显式说明**而不是假装成功（否则 agent 会以为 TD 已经同步）。
        out["updated"] = false;
        out["updateNote"] = "open_project -update is not implemented in the C++ port yet; "
                            "call fpgalsp.check / reopen the project in TD to resync";
    }
    return out;
}
}  // namespace fpgalsp::mcp
