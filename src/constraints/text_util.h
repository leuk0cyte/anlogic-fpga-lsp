// =============================================================================
//  src/constraints/text_util.h —— constraints 共用的文本小工具
// =============================================================================
//  为什么单独一个文件：
//    adc.cpp 与 sdc.cpp 都需要「按 Python 语义切行 / 去空白 / 大写 / repr /
//    float 解析」。这些语义看起来琐碎，但**每一条都直接影响与 Python 版逐字段
//    对照的结果**（例如 \v 也算行分隔、repr 的引号选择、float("nan") 是合法
//    数字）。集中在一处实现 + 一处单测，比在两份解析器里各写一遍更安全。
//
//  全部函数**不抛异常**（CONTRACT.md 硬要求），输入任意字节都返回确定结果。
// =============================================================================

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fpgalsp::constraints::text {

// Python str.isspace() 的 ASCII 子集：space / \t / \n / \r / \v / \f。
// 刻意**不**把 UTF-8 多字节空白（U+00A0、U+2028 …）算作空白：.adc/.sdc 是
// ASCII 格式，真出现多字节空白时按普通字符处理，行为确定且不会误解码。
bool IsSpace(char c);

// Python str.strip() / lstrip() / rstrip()（只按 IsSpace 处理）
std::string_view Strip(std::string_view s);
std::string_view LStrip(std::string_view s);
std::string_view RStrip(std::string_view s);

// Python str.splitlines()（单字节分隔符版本）
//   分隔符：\n、\r\n、\r、\v、\f、\x1c、\x1d、\x1e
//   末尾的分隔符**不**产生最后一行空串（与 Python 一致）
//   ⚠ 已知差异（刻意，记在这里以免后人以为是漏了）：Python 还认 U+0085 /
//     U+2028 / U+2029 三种**多字节**行分隔符。.adc/.sdc 是 ASCII 行式格式，
//     这条差异不会出现在真实输入里；为了不让字节级扫描去解码 UTF-8 我们不做。
std::vector<std::string_view> SplitLines(std::string_view text);

// ASCII 大小写转换（Python .upper()/.lower() 对 ASCII 部分等价）
std::string UpperAscii(std::string_view s);
std::string LowerAscii(std::string_view s);

// substring 的绝对起点，用于 Span 计算（越界输入一律夹到 [0, size]）
int FindOffset(std::string_view haystack, std::string_view needle);

// Python 的标识符前缀匹配：[A-Za-z_][A-Za-z0-9_]*，allow_dollar 时标识符字符还含 '$'。
// 返回匹配到的前缀（没有匹配则返回空）。对应 adc/sdc 里的 re.match("[A-Za-z_]…", …)。
//
// 为什么手写而不是用 re2：
//   这里只需要"从位置 0 开始的最长标识符前缀"，没有任何分支/回溯。手写版本逐字可读，
//   而且完全避开 re2 的一个易错点 —— RE2::Consume/FullMatch 的**变参**只对应捕获组
//   （捕获组 1..n），对**没有捕获组**的正则再传参数会直接返回 false。
std::string_view LeadingIdentifier(std::string_view text, bool allow_dollar = true);

// Python repr(str)：诊断消息里 {name!r} 的形态。
//   repr("clk")        -> 'clk'
//   repr("a'b")        -> "a'b"
//   repr("a'\"b")      -> 'a\'"b'
// 只处理会被消息用到的字符（引号/反斜杠/控制字符），非 ASCII 原样输出。
std::string PyRepr(std::string_view s);

// Python float(str)：失败返回 nullopt（对应 Python 抛 ValueError）。
//   必须支持：前后空白、正负号、小数、指数、nan / inf / infinity（大小写不敏感）、
//   数字之间的下划线（float("1_0") == 10.0）。
//   ⚠ 这里的「支持 nan/inf」不是为了正确，而是为了**忠实复现** Python 的
//     `-period nan` 缺陷（nan <= 0 为 False，于是不报诊断）。见 PYTHON-BUGS.md。
std::optional<double> PyFloat(std::string_view s);

// Python str.strip('"')：只去掉两端的双引号
std::string_view StripQuotes(std::string_view s);

}  // namespace fpgalsp::constraints::text