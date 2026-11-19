// =============================================================================
//  src/server/features_rename.cpp —— textDocument/prepareRename + rename
// =============================================================================
//  OWNER: task B3 (parity-lsp)。对应矩阵 LSP-04 / LSP-05（S0）。
//
//  对应参考实现：
//    Y:\fpga-lsp\fpgalsp\server.py:1670-1700  on_textDocument_prepareRename / on_textDocument_rename
//    Y:\fpga-lsp\fpgalsp\nav.py:16-115        prepare / rename_edits / block_scopes
//    Y:\fpga-lsp\fpgalsp\refs.py:68-142       find / token_span（code_only 搜索）
//
//  契约（probe 实测，见 audit/2026-11-18/parity-lsp/PROBE.md §3.1）：
//    prepareRename(clk_50M @58:6) -> {range:58:5-58:12, placeholder:"clk_50M"}
//    rename(clk_50M -> clk_50M_probe)
//        -> {changes: {file:///…/top.v: [4 条 {range, newText}]}}
//           行 58:5 / 62:13 / 68:13 / 73:6（top.v 里全部 4 处代码内出现）
//
//  ⚠ 与 Python 的已知差异（都在 B3-REPORT.md 里记了）：
//    1. 失败口径：Python 的 rename 在「光标处没有可重命名的符号 / 新名字非法」时
//       **抛 ValueError**（JSON-RPC 错误响应）；这里返回 `null`（合法空结果）。
//       LSP 客户端把 null 当成"这次重命名不可用"，UX 等价，且不会让别的客户端
//       把一次编辑器内的拼写错误变成协议错误。
//    2. 搜索范围：Python 在 `project.hdl_files()`（.al 的 Verilog + VHDL）上做
//       正则+代码掩码；这里在 HDL 索引的 **token 流**上做同名比较。两者对"注释/
//       字符串里的同名文本"都不命中（等价），差别只有：VHDL 文件没有 token 流
//       （Python 能在 .vhd 文本里匹配），以及 IP 生成的 .v 我们也会搜到（更多）。
//    3. 列号：一律 UTF-16（Python 在非 BMP 前缀时按码点，属继承缺陷）。
// =============================================================================

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hdl/hdl_index.h"
#include "jsonrpc/json_util.h"
#include "platform/path.h"
#include "server/features_rename_util.h"
#include "server/lsp_server.h"

namespace fpgalsp::server {
namespace {

constexpr std::size_t kMaxRenameOccurrences = 500;  // Python refs.MAX_REFERENCES

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

// 可重命名目标（对应 Python nav.prepare 的返回 dict）
struct RenameTarget {
    std::string name;
    std::string kind;
    std::string file;
    int line = 0;
    int character = 0;
    int end_character = 0;
};

// -----------------------------------------------------------------------------
//  "是不是 HDL 文件" —— 决定 prepareRename 能不能有结果
// -----------------------------------------------------------------------------
// Python 的 nav.prepare 只是 `index.symbol_at(path, line, char)`，而 Python 的 HDL
// 索引**只装 .al 里的 Verilog + VHDL**（project.hdl_files()）。所以对 .adc/.sdc，
// Python 恒返回 null（probe: boundaries[adc:prepareRename] / [sdc:prepareRename]）。
//
// ⚠ C++ 的 HdlIndex 在 Phase B 之后**也装了** .adc/.sdc 的约束符号（B10 / LSP-11），
//    如果不在这里挡一下，C++ 会给出 Python 没有的 rename 结果 -> 契约漂移。
//    重命名引脚/SDC 符号属于 additive 功能，要先改契约（见 B3-REPORT.md）。
bool IsHdlPath(std::string_view path) {
    return hdl::IsVerilogPath(path) || hdl::IsVhdlPath(path);
}

// 一次出现（对应 refs.find 的 {file, line, character, end_character}）
struct Occurrence {
    std::string file;
    int line = 0;
    int character = 0;
    int end_character = 0;
};

// 对应 Python nav.prepare：查 HDL 符号索引。
//
// `allow_token_fallback` 只有 rename 用（Python 的 nav.rename_edits 在 prepare 失败后
// 用 refs.token_span 兜底，让 `genvar i` 这类不在符号索引里的标识符也能改名）；
// **prepareRename 本身不能兜底** —— Python 的 nav.prepare 只返回 symbol_at 的结果，
// 否则在 `wire` 这类关键字上会给出 Python 没有的 placeholder（probe 实测：
// prepareRename(58:0) 在 Python 侧是 null）。
std::optional<RenameTarget> PrepareTarget(const hdl::HdlIndex& index, const std::string& path,
                                          int line, int character, std::string_view text,
                                          bool allow_token_fallback) {
    if (const std::optional<hdl::SymbolAtResult> symbol = index.SymbolAt(path, line, character);
        symbol.has_value() && !symbol->name.empty()) {
        RenameTarget target;
        target.name = symbol->name;
        target.kind = symbol->kind;
        target.file = symbol->file.empty() ? path : symbol->file;
        target.line = symbol->span.line;
        target.character = symbol->span.start_character;
        target.end_character = symbol->span.end_character;
        if (target.end_character <= target.character) {
            target.end_character = target.character + static_cast<int>(
                                                       platform::utf16_len(target.name));
        }
        return target;
    }
    if (!allow_token_fallback) return std::nullopt;

    const edit::TextToken token = edit::TextTokenAt(text, line, character);
    if (!token.found) return std::nullopt;
    RenameTarget target;
    target.name = token.text;
    target.kind = "identifier";
    target.file = path;
    target.line = line;
    target.character = token.start;
    target.end_character = token.end;
    return target;
}

// 全工程 token 级同名搜索（对应 refs.find(..., code_only=True)）。
// 用 token 而不是正则，注释/字符串里的同名文本天然不会命中。
std::vector<Occurrence> FindOccurrences(const hdl::HdlIndex& index, std::string_view name) {
    std::vector<Occurrence> out;
    const std::string wanted = edit::IdentKey(name);
    if (wanted.empty()) return out;

    for (const std::string& path : index.Paths()) {
        // Python 只在 project.hdl_files()（Verilog + VHDL）上搜，见 IsHdlPath 的说明。
        if (!IsHdlPath(path)) continue;
        const hdl::ParsedFile* file = index.File(path);
        if (file == nullptr) continue;
        for (const hdl::Token& token : file->tokens) {
            if (token.kind != 'i') continue;
            if (edit::IdentKey(token.text) != wanted) continue;
            Occurrence occurrence;
            occurrence.file = file->path;
            occurrence.line = token.line;
            occurrence.character = token.character;
            occurrence.end_character = token.end_character;
            out.push_back(std::move(occurrence));
            if (out.size() >= kMaxRenameOccurrences) return out;  // Python: results[:500]
        }
    }
    return out;
}

}  // namespace

// =============================================================================
//  textDocument/prepareRename
// =============================================================================

Json LspServer::FeaturePrepareRename(std::string_view path, int line, int character) {
    const std::string path_string(path);
    if (path_string.empty()) return Json(nullptr);
    // Python 的 HDL 索引里没有 .adc/.sdc -> symbol_at 恒为 None -> null
    if (!IsHdlPath(path_string)) return Json(nullptr);

    const std::optional<project::Project> project = ProjectForPath(path_string);
    if (!project.has_value()) return Json(nullptr);
    const std::shared_ptr<const hdl::HdlIndex> index = IndexForProject(*project);
    if (!index) return Json(nullptr);

    std::string text;
    if (const std::optional<std::string> loaded = LoadText(path_string); loaded.has_value()) {
        text = edit::NormalizeNewlines(*loaded);
    }
    const std::optional<RenameTarget> target =
        PrepareTarget(*index, path_string, line, character, text,
                      /*allow_token_fallback=*/false);
    if (!target.has_value() || target->name.empty()) return Json(nullptr);

    // Python: end_character or character + 1
    Json result = Json::object();
    result["range"] = MakeRange(target->line, target->character, target->line,
                                target->end_character > target->character
                                    ? target->end_character
                                    : target->character + 1);
    result["placeholder"] = target->name;
    return result;
}

// =============================================================================
//  textDocument/rename
// =============================================================================

Json LspServer::FeatureRename(std::string_view path, int line, int character,
                              std::string_view new_name) {
    const std::string path_string(path);
    if (path_string.empty()) return Json(nullptr);

    const std::optional<project::Project> project = ProjectForPath(path_string);
    if (!project.has_value()) return Json(nullptr);
    const std::shared_ptr<const hdl::HdlIndex> index = IndexForProject(*project);
    if (!index) return Json(nullptr);

    std::string text;
    if (const std::optional<std::string> loaded = LoadText(path_string); loaded.has_value()) {
        text = edit::NormalizeNewlines(*loaded);
    }

    const std::optional<RenameTarget> target =
        PrepareTarget(*index, path_string, line, character, text,
                      /*allow_token_fallback=*/true);
    if (!target.has_value() || target->name.empty()) return Json(nullptr);
    if (!edit::IsValidVerilogIdentifier(new_name)) return Json(nullptr);

    // Python: 新名字与旧名字相同时返回 {"edits": {}} -> changes 为空对象
    Json changes = Json::object();
    if (new_name == target->name) {
        Json result = Json::object();
        result["changes"] = std::move(changes);
        return result;
    }

    std::vector<Occurrence> found = FindOccurrences(*index, target->name);

    // generate 作用域：声明落在 `generate … endgenerate` / generate-for 体内时，
    // 只重命名该块（且只在声明文件）里的出现（nav.rename_edits:101-110）。
    std::string declaration_text;
    if (const std::optional<std::string> loaded = LoadText(target->file);
        loaded.has_value()) {
        declaration_text = edit::NormalizeNewlines(*loaded);
    }
    const std::vector<edit::BlockScope> scopes = edit::BlockScopes(declaration_text);
    if (const edit::BlockScope* scope = edit::EnclosingScope(scopes, target->line);
        scope != nullptr) {
        std::vector<Occurrence> filtered;
        filtered.reserve(found.size());
        for (Occurrence& occurrence : found) {
            if (!platform::path_equals_ignore_case(occurrence.file, target->file)) continue;
            if (occurrence.line < scope->start || occurrence.line > scope->end) continue;
            filtered.push_back(std::move(occurrence));
        }
        found = std::move(filtered);
    }

    // 按文件分组，保持首次出现顺序（Python dict 的插入顺序）
    std::vector<std::pair<std::string, std::vector<const Occurrence*>>> groups;
    for (const Occurrence& occurrence : found) {
        bool placed = false;
        for (auto& [uri, items] : groups) {
            if (uri == platform::path_to_uri(occurrence.file)) {
                items.push_back(&occurrence);
                placed = true;
                break;
            }
        }
        if (!placed) {
            groups.emplace_back(platform::path_to_uri(occurrence.file),
                                std::vector<const Occurrence*>{&occurrence});
        }
    }

    for (const auto& [uri, items] : groups) {
        Json edits = Json::array();
        for (const Occurrence* occurrence : items) {
            Json edit = Json::object();
            edit["range"] = MakeRange(occurrence->line, occurrence->character,
                                      occurrence->line, occurrence->end_character);
            edit["newText"] = std::string(new_name);
            edits.push_back(std::move(edit));
        }
        changes[uri] = std::move(edits);
    }

    Json result = Json::object();
    result["changes"] = std::move(changes);
    return result;
}

}  // namespace fpgalsp::server