// =============================================================================
//  src/td/td_output.h —— TD stdout 解析（纯函数，不触碰进程）
// =============================================================================
//  对应参考实现：Y:\fpga-lsp\fpgalsp\td.py
//      clean_lines / parse_message_lines / has_error / has_fatal
//
//  ⚠ 全部用 re2 实现。TD 日志可达数 MB，std::regex 有栈溢出与指数回溯风险。
//
//  ---- 正则在 td.py 中的原文（逐字复现）----
//  MSG_RE        = ^(?P<code>[A-Z][A-Z0-9]{1,5}-\d{3,5})\s*
//                  (?:(?P<sev>ERROR|CRITICAL-WARNING|WARNING|INFO)\s*:?\s*)?(?P<msg>.*)$
//  IN_FILE_RE    = (?P<path>[A-Za-z]:[/\\][^()]+?)\((?P<line>\d+)\)\s*$
//  REL_IN_FILE_RE= (?P<path>[A-Za-z_][A-Za-z0-9_./\\-]*\.(?:v|sv|vh|svh|vhd|vhdl|adc|sdc))
//                  \((?P<line>\d+)\)\s*$
//  IGNORED_FILE_RE = file\s+['"](?P<path>[^'"]+\.(?:v|sv|vh|svh|vhd|vhdl))['"]\s+ignored due to
//                    errors   （忽略大小写）
//  ERROR_RE      = \b[A-Z][A-Z0-9]{1,5}-\d{3,5}\s+(?:ERROR|CRITICAL-WARNING)\b
//  FATAL_RE      = \b[A-Z][A-Z0-9]{1,5}-\d{3,5}\s+ERROR\b
//  SEVERITY      = {"ERROR": 1, "CRITICAL-WARNING": 2, "WARNING": 2, "INFO": 3}
//
//  ---- 实测样本（TD 2026.2，%WORKSPACE%\CorpusAlpha 真实工程）----
//    HDL-1007 : analyze verilog file %WORKSPACE%/CorpusAlpha/src/trans/trans.v
//    HDL-1007 : elaborate module top in %WORKSPACE%/CorpusAlpha/src/top.v(15)
//    HDL-7144 CRITICAL-WARNING: instantiate unknown module pll_gen in %WORKSPACE%/CorpusAlpha/src/top.v(66)
//    HDL-1008 : device name is not set, so there is no need to read *macro.v
//
//  其中「无 severity 的 HDL-1007 → INFO(3)」是 0.10.23 的关键修复：若误判成 warning，
//  20 个文件的工程会刷出 20 条噪音诊断。
// =============================================================================

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fpgalsp::td {

// 严重级别（数值与 LSP DiagnosticSeverity 一致）
inline constexpr int kSeverityError = 1;
inline constexpr int kSeverityWarning = 2;
inline constexpr int kSeverityInfo = 3;

// 一条 TD 消息（对应 Python 的 {code, severity, message, path, line}）
struct TdMessage {
    std::string code;      // 如 "HDL-8007"
    int severity = kSeverityInfo;
    std::string message;
    std::optional<std::string> path;  // 已把反斜杠换成正斜杠
    std::optional<int> line;          // 1-based（与 TD 输出一致）
};

// 对应 Python clean_lines：
//   * 先把 "\r\n" -> "\n"，再把残余 "\r" -> "\n"
//   * 按 "\n" 切分
//   * 每行 rstrip
//   * 丢弃 strip 后为空的行
std::vector<std::string> CleanLines(std::string_view text);

// 对应 Python parse_message_lines。
std::vector<TdMessage> ParseMessageLines(std::string_view text);

// 对应 Python has_error：文本里是否含 <CODE> ERROR / CRITICAL-WARNING
bool HasError(std::string_view text);

// 对应 Python has_fatal：文本里是否含 <CODE> ERROR（不含 CRITICAL-WARNING）
bool HasFatal(std::string_view text);

// 对应 Python run() 的后处理：截取 __B<tag>__ 与 __E<tag>__ 之间，
// 然后**全局**替换 "td > "。
//
// ⚠ 注意 replace 是全局的：若 TD 输出本身含 "td > " 字面（HDL 注释、路径），
//   会被一并吃掉。这是 Python 版的既有行为，本实现保持一致（见单测
//   TdOutputKnownDefect 的说明）。将来若要修正，请先改那条测试。
std::string StripSentinelsAndPrompt(std::string_view raw, std::string_view tag);

}  // namespace fpgalsp::td