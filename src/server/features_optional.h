// =============================================================================
//  src/server/features_optional.h —— LSP 3.17/3.18 可选合规项（A4–A12）
// =============================================================================
//  本层与 features.h / features_semantics.cpp 同一分层：
//      hdl / semantics / project  —— 纯语义（不认识 LSP）
//      features_optional          —— **纯函数**：输入索引/文本/位置，输出协议 JSON
//      lsp_server                 —— 只做 params 解包 + 状态（缓存/工程）注入
//
//  为什么逻辑必须在这一层（而不是直接写进 LspServer）：
//    * tests/unit/features_optional_test.cpp 只链接 fpga_lsp::server + fpga_lsp::hdl，
//      不启动 LSP 传输层，就能逐项断言 A4–A12 的返回形状与边界。
//    * A9 的 pull diagnostics 数据源是 LspServer 里"最近一次 push publishDiagnostics
//      的缓存"，所以形状构造函数在这里（纯函数），缓存本体在 LspServer
//      （state_mutex_ 保护的 latest_diagnostics_）。
//
//  约定（与 features.h 一致）：
//    * 位置一律 0-based 行 + **UTF-16 列**（走 platform/utf16）
//    * 全部 never-throw：畸形输入 -> 合法空值（`null` / `[]` / `{signatures:[]}`）
//    * 不返回固定空值糊弄：每一项在"拿得到数据"时都必须给出真实语义
// =============================================================================

#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "hdl/hdl_index.h"
#include "semantics/semantics.h"

namespace fpgalsp::server::features_optional {

using Json = nlohmann::json;

// -----------------------------------------------------------------------------
// A4 textDocument/signatureHelp
// -----------------------------------------------------------------------------
// 调用/例化括号内（`u_alu (`、`u_alu #(...) (`）给出被调模块的端口签名：
//   label = "module child(clk, d, q)"，parameters[i].label = 端口名，
//   parameters[i].documentation = "input [7:0]" 这类方向/位宽说明。
//   activeParameter = 括号内、光标之前**顶层**逗号个数（嵌套 .a(x,y) 里的逗号不算）。
// 拿不到模块定义（`.adc`、普通函数调用、光标不在括号内）时返回合法空
// {signatures:[],activeSignature:0,activeParameter:0}。
Json SignatureHelp(const hdl::HdlIndex& index, std::string_view path, std::string_view text,
                   int line, int character);

// -----------------------------------------------------------------------------
// A5 textDocument/documentLink
// -----------------------------------------------------------------------------
// **只在 `.al` 生效**：逐行扫 `<File Path="...">`（不依赖 xml_lite 的节点位置，
// TD 改格式也不会崩）。相对路径按 `.al` 所在目录解析（project::ResolveRelative）。
//   文件存在 -> {range,target:"file:///..."}
//   文件不存在 -> {range,tooltip:"file not found: ..."}（无 target，客户端置灰）
// 非 `.al` 文件返回 []（HDL include 链接不在本项范围）。
Json DocumentLinks(std::string_view path, std::string_view text);

// -----------------------------------------------------------------------------
// A6 textDocument/codeLens
// -----------------------------------------------------------------------------
// 本文件里每个 module 定义一条 lens：title = "N instantiations"
// （N = 全工程索引里 instance.module == module.name 的实例数），
// command = fpgalsp.searchHierarchy（arguments[0] = {query: <module>}）。
// resolveProvider=false：command 已完整，无需再 resolve。
Json CodeLenses(const hdl::HdlIndex& index, std::string_view path);

// -----------------------------------------------------------------------------
// A7 textDocument/documentHighlight
// -----------------------------------------------------------------------------
// 用 Verible token 流（HdlIndex::References），所以注释/字符串里的同名文本不会误命中。
// 只保留**同一文档**内的出现；kind 恒为 1（Text）—— token 流判不出读写关系，
// 按 A7 设计结论"宁可用 Text 也不说谎"（写入方见 doc/design/backend-features.md A7）。
Json DocumentHighlights(const hdl::HdlIndex& index, std::string_view path, int line,
                        int character);

// -----------------------------------------------------------------------------
// A8 textDocument/selectionRange
// -----------------------------------------------------------------------------
// 每个 position 一条链：单词 -> 所在行 -> 所在块（begin..end / { } / module..endmodule）
// -> 整篇。链上每一层的 range 都包含下一层（LSP 规范要求 parent 包容 child）。
// 非 HDL 文件（`.al/.adc/.sdc`）至少给出 单词 -> 行 -> 整篇 三级。
Json SelectionRanges(const hdl::HdlIndex& index, std::string_view path, std::string_view text,
                     const std::vector<std::pair<int, int>>& positions);

// -----------------------------------------------------------------------------
// A12 textDocument/semanticTokens/range
// -----------------------------------------------------------------------------
// 先按 [start, end) 过滤 decoded token（token 起点落在区间内），再**重新编码**：
// 过滤后的第一条 token 的 deltaLine/deltaStart 是相对 (0,0) 的绝对位置，
// 不能把全量 5 元组数组直接截断（那样第二条起会相对错误的上一 token）。
std::vector<semantics::SemToken> FilterSemanticTokens(
    const std::vector<semantics::SemToken>& tokens, int start_line, int start_character,
    int end_line, int end_character);

// index 可以为 nullptr（无工程时与全量版一样返回 {"data":[]}）。
Json SemanticTokensRange(const hdl::HdlIndex* index, std::string_view path,
                         std::string_view text, int start_line, int start_character,
                         int end_line, int end_character);

// A12 completionItem/resolve：模块候选补 `detail` + `documentation`（端口数 + 定义位置）。
// index 为 nullptr / 不是模块名时**原样返回** item（不丢客户端字段）。
Json ResolveCompletionItem(const hdl::HdlIndex* index, const Json& item);

// -----------------------------------------------------------------------------
// A9 pull diagnostics（形状构造；数据源 = LspServer 最近一次 push 的缓存）
// -----------------------------------------------------------------------------
// resultId 是 (uri, items) 的确定性指纹：内容不变 -> resultId 不变 ->
// 客户端携带 previousResultId 时可以回 {kind:"unchanged"}。
std::string DiagnosticsResultId(std::string_view uri, const Json& items);

// textDocument/diagnostic -> {kind:"full", items:[...], resultId:"..."}
Json BuildDocumentDiagnosticReport(const Json& items, std::string_view result_id);

// workspace/diagnostic 规范形状：{items:[{uri, kind:"full", items, resultId}]}
// 只包含服务端**曾经发布过**诊断（含"已清空为空数组"）的文件。
Json BuildWorkspaceDiagnosticReport(const std::map<std::string, Json>& latest);

}  // namespace fpgalsp::server::features_optional
