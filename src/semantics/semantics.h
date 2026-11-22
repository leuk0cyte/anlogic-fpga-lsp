// =============================================================================
//  src/semantics/semantics.h —— 语义高亮 / 折叠范围 / token 统计（纯逻辑层）
// =============================================================================
//  对应参考实现：Y:\fpga-lsp\fpgalsp\semantics.py
//      semantic_tokens()            -> SemanticTokens()
//      encode_tokens()              -> EncodeTokens()
//      folding_ranges()             -> FoldingRanges()
//      verible_tokens_to_semantic() -> （内部）
//      merge_tokens()               -> （内部）
//  以及 server.py 的 _reclassify_verible_identifiers 与 fpgalsp.debugHighlight 的
//  counts 汇总（-> TokenStats）。
//
//  ⚠ 契约（改动前先读 tests/unit/semantics_test.cpp）
//
//  1. token 类型名与顺序 = Python `TOKEN_TYPES`（LSP legend，12 项，顺序不可改）：
//       keyword, type, class, property, variable, parameter,
//       function, number, comment, string, operator, macro
//
//  2. 两类来源合并（与 Python 完全相同的流水线）：
//       primary = hdl 索引（module/port/instance）+ 行内正则（类型词/关键字/参数/
//                 声明/字面量/数字）—— 注释与字符串先被掩码挖空
//       extra   = Verible **原始词法流**（operator/comment/string/number/macro/keyword）
//                 再做一次"标识符按索引重分类"（variable -> class/property/function/...）
//     primary 在 (line,start) 相同时优先；最终按 (line,start) 排序并去掉同行重叠。
//     只用 primary 会丢掉全部 operator（一个 500 行的 .v 就差 1000+ token），所以
//     这里必须走 Verible 词法，而不是 hdl 的 filtered token 流。
//
//  3. 位置：0-based 行 + **UTF-16 列**（与 LSP 一致）。
//     Python 的 verible 换算用"码点下标"，在含非 BMP（emoji）的行上会少算 1 列；
//     本实现按 UTF-16 换算（更正确），差异见 doc/audit/2026-11-18/B2-REPORT.md。
//
//  4. 折叠范围是纯文本算法，区间与**顺序**都与 Python `folding_ranges()` 一致。
//
//  5. never-throw：任何解析/词法异常都退化为"没有 token"，绝不把异常抛给 dispatcher。
// =============================================================================

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "hdl/hdl_index.h"

namespace fpgalsp::semantics {

// -----------------------------------------------------------------------------
//  token 类型（顺序即 LSP legend；与 Python semantics.TOKEN_TYPES 逐项一致）
// -----------------------------------------------------------------------------
enum TokenType : int {
    kKeyword = 0,
    kType = 1,
    kClass = 2,
    kProperty = 3,
    kVariable = 4,
    kParameter = 5,
    kFunction = 6,
    kNumber = 7,
    kComment = 8,
    kString = 9,
    kOperator = 10,
    kMacro = 11,
};

inline constexpr int kTokenTypeCount = 12;

// legend 名（initialize 的 semanticTokensProvider.legend.tokenTypes 也用它）
std::string_view TokenTypeName(int type);

// Verible 的 y-symbol 名（`verilog_symbol_name()` 的返回值，带引号的显示名，
// 例如 `"module"` / `'('` / `SymbolIdentifier`）-> 语义类型；-1 = 丢弃。
// 逐条复刻 Python `semantics._kind_to_type`。
int TokenTypeFromSymbolName(std::string_view symbol_name);

// -----------------------------------------------------------------------------
//  decoded token
// -----------------------------------------------------------------------------
struct SemToken {
    int line = 0;
    int start = 0;   // UTF-16 列
    int length = 0;  // UTF-16 码元数
    int type = 0;    // TokenType
};

// 完整流水线（index 可以为 nullptr —— 此时与 Python 一样返回空）。
std::vector<SemToken> SemanticTokens(const hdl::HdlIndex* index, std::string_view path,
                                     std::string_view text);

// -----------------------------------------------------------------------------
//  P2-A：行级增量的有状态入口
// -----------------------------------------------------------------------------
// 语义：**输出与 `SemanticTokens(index, path, text)` 逐 token 相同**（line/start/
// length/type 全等，顺序相同），只是复用上一次请求里未变化行的 token。
//
//   * 缓存键 = (path, **索引内容指纹**)。注意不是 index 指针、也不是 generation：
//     IndexService 每处理一次 didChange 都会重装快照（generation +1），用 generation
//     当键会导致永远不命中。指纹逐字段覆盖 SymbolAt / 索引候选真正会读的字段
//     （本文件的 modules/ports/signals/params/instances 位置与名字 + 全局模块/端口名），
//     只有索引**语义内容**变了才失效。
//   * 文本不变 + 索引内容不变 -> 直接返回上次数组（零重算）。
//   * 文本变了 -> 用**字节级**公共前缀/后缀定出变化行；只有当变化行两侧的锚点都
//     落在"干净行首"（不在块注释/字符串里、上一行不以 '\' 或 '/' 结尾）且区域
//     重新扫描后仍以代码状态收尾时，才做增量；否则**全量重算**（正确性优先）。
//   * **行数变化也走增量**：未变行的 token 行号整体平移 ±delta；索引侧（module/
//     port/signal/param/instance 的 `SymbolAt` 位置）按同一 delta 逐条校验
//     （RecordsMatchUnderShift）—— 平移映射不成立（声明增删、改名、变化区内位移）
//     才回退全量。
//   `index_generation` 只用于诊断/统计，不参与命中判定。
//
//  线程安全：内部互斥 + 不可变快照（shared_ptr），可在多 worker 上并发调用。
//  返回 shared_ptr 便于服务端零拷贝缓存/响应。
std::shared_ptr<const std::vector<SemToken>> SemanticTokensCached(
    const hdl::HdlIndex* index, std::string_view path, std::string_view text,
    std::uint64_t index_generation);
std::shared_ptr<const std::vector<SemToken>> SemanticTokensCached(
    const hdl::HdlIndex* index, std::string_view path,
    const std::shared_ptr<const std::string>& text, std::uint64_t index_generation);

// 测试/诊断：命中计数（用来证明"增量确实跑了"，而不是每次都全量回退）。
struct SemanticTokenCacheStats {
    std::uint64_t unchanged = 0;    // 文本与索引内容都没变 -> 直接复用
    std::uint64_t incremental = 0;  // 行级增量命中
    std::uint64_t full = 0;         // 全量重算（首次 + 所有回退情形）
    std::uint64_t full_index_changed = 0;  // 其中因"索引内容真的变了"而全量
};
SemanticTokenCacheStats GetSemanticTokenCacheStats();
// 清空缓存与计数（单测用；服务端不需要）。
void ResetSemanticTokenCache();

// LSP delta 编码（每组 5 个整数：deltaLine, deltaStart, length, type, 0）
std::vector<int> EncodeTokens(const std::vector<SemToken>& tokens);

// fpgalsp.debugHighlight 的统计：类型名 -> 条数（只统计出现过的类型）
struct TokenStats {
    int total = 0;
    std::map<std::string, int, std::less<>> counts;  // 按名字排序（JSON 对象无顺序可言）
};
TokenStats Summarize(const std::vector<SemToken>& tokens);

// -----------------------------------------------------------------------------
//  折叠范围（对应 Python folding_ranges）
// -----------------------------------------------------------------------------
struct FoldingRange {
    int start_line = 0;
    int end_line = 0;
    bool comment = false;  // true -> kind: "comment"
};
std::vector<FoldingRange> FoldingRanges(std::string_view text);

// -----------------------------------------------------------------------------
//  供测试/诊断
// -----------------------------------------------------------------------------
// 每字节掩码：1 = 代码，0 = 注释/字符串（对应 Python refs.code_mask，长度 = 字节数）
std::vector<char> CodeMask(std::string_view text);

// 只取"hdl 索引 + 行内正则"这一半（Python semantic_tokens 的返回值），便于单测
std::vector<SemToken> HdlSideTokens(const hdl::HdlIndex* index, std::string_view path,
                                   std::string_view text);

// 只取 Verible 原始词法流 -> decoded token（Python verible_tokens_to_semantic 的等价物）；
// path 非空时会做标识符重分类（Python `_reclassify_verible_identifiers`）。
std::vector<SemToken> VeribleSideTokens(const hdl::HdlIndex* index, std::string_view path,
                                       std::string_view text);

}  // namespace fpgalsp::semantics