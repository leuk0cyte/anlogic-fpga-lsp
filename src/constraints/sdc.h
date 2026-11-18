// =============================================================================
//  src/constraints/sdc.h —— SDC 时序约束（.sdc）解析 / 诊断 / 补全
// =============================================================================
//  参考实现：Y:\fpga-lsp\fpgalsp\sdc.py（1068 行）+ CONTRACT.md §"sdc.py"
//
//  **与 Verible 无关**：SDC 是 Tcl 方言，Verible 只懂 SystemVerilog，这里是自己
//  写的浅层 Tcl 词法器。浅到什么程度（与 Python 版一致）：
//    · `{ ... }` 词组（整组算一个参数，花括号剥掉）
//    · `[ ... ]` 命令替换（整段算一个不透明参数）
//    · `#` 注释（含行尾注释，但要尊重 {} / [] / "" / 转义）
//    · `\` 续行
//    · `;` 命令分隔
//  不做变量替换、不做 Tcl 的完整 word-splitting —— 对约束文件足够，且**可预测**。
//
//  命名空间 `constraints::sdc` 与 Python 模块一一对应：
//      sdc.parse(text)       ->  sdc::Parse(text)
//      sdc.diagnostics(...)  ->  sdc::Diagnostics(...)
//      sdc.context(...)      ->  sdc::Context(...)
//
//  ---- 硬约束 ----
//  1. **不抛异常**：畸形输入产生 `SDC-SYNTAX` 诊断，绝不崩（CONTRACT.md）。
//  2. 诊断严重度逐值复现 Python：
//       SDC-SYNTAX 1 / SDC-PORT 1 / SDC-PERIOD 1 / SDC-CMD 2 /
//       SDC-DELAY 2 / SDC-NOCLOCK 2 / SDC-OPT 3
//  3. **诊断口径保守优先**：没有顶层端口表时不做 get_ports 检查（单独预览一个
//     .sdc 不该报一堆"端口不存在"）。这条比"多查一点"重要 —— 误报会让人关功能。
//
//  ---- 本工程新增（Python 版没有）：引用但从未定义的时钟名 ----
//  `UndefinedClockDiagnostics()` 产出 `SDC-CLOCK`（severity 2）。它与
//  `Diagnostics()` 分开，保证后者与 Python 逐字段等价。刻意做得很保守：
//    · 文件里**一条** create_clock/create_generated_clock 都没有时不做判断
//      （时钟可能来自 derive_pll_clocks / 其它 include 的 sdc）
//    · 只校验纯值（`-clock foo`），带 `[]` 的命令替换、带通配符的名字一律跳过
//  见 tests/PYTHON-BUGS.md 的登记（Python 侧完全没有这个检查）。
// =============================================================================

#pragma once

#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "constraints/port.h"
#include "lsp/diagnostic.h"

namespace fpgalsp::constraints {
namespace sdc {

// Tcl 词法单元。字段与 Python 版 token dict 一一对应（span 均为 0-based 列）。
enum class TokenKind {
    kWord,       // 裸词
    kBrace,      // { ... }
    kBracket,    // [ ... ]
    kQuote,      // " ... "
    kSeparator,  // ;
};

struct Token {
    TokenKind kind = TokenKind::kWord;
    std::string text;        // 原始文本（含花括号/引号）
    std::string inner_text;  // 花括号/引号内部的文本
    int start = 0;
    int end = 0;
    int inner_start = 0;
    int inner_end = 0;
    bool closed = true;  // 花括号/方括号/引号是否闭合
};

struct SyntaxError {
    int character = 0;
    std::string message;
};

// 一条 SDC 命令。字段与 CONTRACT.md 的 command dict 一致。
struct Command {
    int line = 0;  // **逻辑行**的首行行号（续行时指向起始行）
    std::string command;
    std::vector<std::string> args;
    std::string raw;  // 逻辑行原文（可能含多行拼接）
    Span command_span;
    std::vector<Token> arg_tokens;
    std::vector<SyntaxError> syntax;
};

struct TokenizeResult {
    std::vector<Token> tokens;
    std::vector<SyntaxError> errors;
};

// 浅层 Tcl 词法。offset 是给 `[ ... ]` 内部重扫用的绝对列偏移。
// **不抛异常**；未闭合的 { [ " 会带 closed=false 并产生诊断（引号除外，Python 同）。
TokenizeResult Tokenize(std::string_view text, int offset = 0);

// 解析整篇 .sdc。**不抛异常**。
std::vector<Command> Parse(std::string_view text);

// 命令 / 选项表（Python `CLOCK_COMMANDS` / `DELAY_COMMANDS` /
// `GET_PORTS_OPTIONS` / `COMMAND_OPTIONS` 逐项复制）
const std::set<std::string>& ClockCommands();
const std::set<std::string>& DelayCommands();
const std::set<std::string>& GetPortsOptions();
// 已建模的 SDC 命令（`COMMAND_OPTIONS` 的键）。补全候选用这一组。
const std::set<std::string>& KnownCommands();
// 该命令认识的选项；nullptr = 未建模，不做选项检查（Python 同语义）
const std::set<std::string>* OptionsFor(std::string_view command);

// 与 Python `sdc.diagnostics(cmds, ports, known_commands)` **逐字段等价**。
//   · known_commands == nullptr  <=>  Python 的 None（跳过命令名检查）
//   · ports 为空时跳过 get_ports 端口存在性检查（单独预览 .sdc 场景）
std::vector<lsp::Diagnostic> Diagnostics(const std::vector<Command>& commands,
                                         const std::vector<Port>& ports,
                                         const std::set<std::string>* known_commands = nullptr);

// 光标上下文：kind ∈ {"command","port","clock_name","option"}；option 时 command 有值。
struct CursorContext {
    std::string kind;
    Span span;
    std::string command;
};

std::optional<CursorContext> Context(std::string_view text, int line, int character);

// -----------------------------------------------------------------------------
//  补全
// -----------------------------------------------------------------------------

// 文档里 `create_clock/create_generated_clock -name X` 定义过的时钟名
// （Python server.py 用同一正则；顺序按出现顺序，去重）。
std::vector<std::string> DefinedClockNames(std::string_view text);

struct CompletionItem {
    std::string label;
    std::string detail;
    std::string kind;  // "command" | "port" | "clock" | "option"
};

// 补全候选：命令名 + 已定义时钟名（+ 端口名 / 选项，按光标上下文）。
std::vector<CompletionItem> Completion(std::string_view text, int line, int character,
                                       const std::set<std::string>& known_commands,
                                       const std::vector<Port>& ports);

// -----------------------------------------------------------------------------
//  附加诊断（Python 版没有）：引用但从未定义的时钟名
// -----------------------------------------------------------------------------

std::vector<lsp::Diagnostic> UndefinedClockDiagnostics(const std::vector<Command>& commands);

// -----------------------------------------------------------------------------
//  JSON 序列化（与 Python dict 的 JSON 形状一致）
// -----------------------------------------------------------------------------

Json ToJson(const Token& token);
Json ToJson(const Command& command);
Json ToJson(const std::vector<Command>& commands);

}  // namespace sdc
}  // namespace fpgalsp::constraints