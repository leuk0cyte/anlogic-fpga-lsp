// =============================================================================
//  src/reports/report_text.h —— 报告解析的公共文本工具（never-throw）
// =============================================================================
//  对应参考实现：%PYREF%\fpgalsp\reports.py 里的 str 操作
//      text.split("\n") / line.strip() / line.split() / int() / float() / round(x, 2)
//
//  ⚠ 关键约定：本文件所有函数**都不抛异常**。
//      报告文件是外部输入，可能被截断、可能是二进制垃圾、可能不是 UTF-8。
//      CONTRACT.md 要求畸形输入返回空结构 + warnings，而不是把 ide 带崩。
//
//  ⚠ string_view 生命周期
//      SplitLines / Strip* 返回的是**指向入参的 view**，调用方必须保证源文本
//      在视图使用期间存活（正常用法：先 read 到一个 std::string，再切行）。
// =============================================================================

#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "platform/text.h"
namespace fpgalsp::reports {

// -----------------------------------------------------------------------------
// 行 / 空白
// -----------------------------------------------------------------------------

// 等价 Python 的 text.split("\n")：
//   "a\nb" -> {"a","b"}； "a\n" -> {"a",""}； "" -> {""}
// 注意末尾换行会产生一个空元素，这一点与 Python 一致，且有解析器依赖它
// （timing 的 legacy 兜底按索引回溯 6 行，行号必须与 Python 完全对齐）。
std::vector<std::string_view> SplitLines(std::string_view text);

// Python 的 str.split()（无参数）：按任意 ASCII 空白切分并丢弃空段。
std::vector<std::string> SplitWhitespace(std::string_view text);

// Python 的 str.strip() / lstrip() / rstrip()（ASCII 空白：空格 \t \n \v \f \r）。
// 报告里可能出现 UTF-8 中文（模块注释），这里只处理 ASCII 空白，与 Python 默认
// str.strip() 对 ASCII 输入的行为一致。
std::string_view Strip(std::string_view text);
std::string_view LStrip(std::string_view text);
std::string_view RStrip(std::string_view text);

// 只去**空格**（对应 Python 的 lstrip(" ") / rstrip(" ")）。
// area 的层级缩进用它：缩进只由空格构成，把制表符也吃掉会算错 level。
std::string_view LStripSpaces(std::string_view text);
std::string_view RStripSpaces(std::string_view text);

using platform::StartsWith;
using platform::EndsWith;
bool StartsWithAny(std::string_view text, std::initializer_list<std::string_view> prefixes);

// 大小写不敏感的 ASCII 比较（报告里 QoR 表头大小写不完全固定）
bool EqualsIgnoreCase(std::string_view a, std::string_view b);
using platform::ToLowerAscii;
// -----------------------------------------------------------------------------
// 数字
// -----------------------------------------------------------------------------

// Python int() 的 ASCII 子集：允许前后空白与 +/- 号，其余必须是十进制数字。
// 失败返回 false（调用方按 Python 的 `except ValueError` 处理）。
bool ParseInt(std::string_view text, int64_t& out);
bool ParseInt32(std::string_view text, int& out);

// Python float() 的 ASCII 子集。失败返回 fallback 且 ok=false。
// 允许 "1"、"1.5"、"-1.5"、".5"、"1."（Python 都接受）。
double ParseDouble(std::string_view text, bool& ok, double fallback = 0.0);

// Python 的 round(value, 2)。
//   用 nearbyint（默认舍入模式 = 就近取偶）而不是 std::round（.5 远离零），
//   因为 CPython 的 round() 对恰好落在 .xx5 的值是**四舍六入五成双**：
//       round(0.125, 2) == 0.12   （std::round 会给出 0.13，就与 Python 分叉了）
//   例：area 里 used*100.0/total 的结果必须逐字节对上 Python 的 JSON。
double Round2(double value);

// -----------------------------------------------------------------------------
// 资源名归一化（area 用）
// -----------------------------------------------------------------------------

// 对应 Python _norm_resource：
//   re.sub(r"[^a-z0-9]+", "_", name.strip().lower()).strip("_")
// 所有非 [a-z0-9] 连续段折叠成一个下划线。
// ⚠ 这就是「不假设固定资源名」的关键：PH1 打 "#logic lut"，EF2 打 "#lut"，
//   归一化后分别是 logic_lut / lut，两张表都能落到同一套别名上。
std::string NormResource(std::string_view name);

// -----------------------------------------------------------------------------
// 文件
// -----------------------------------------------------------------------------

// 读报告文件（never-throw）：打不开返回空串。
// 走 platform::read_file_utf8 —— 自动剥 BOM、非法字节替换成 U+FFFD，
// 与 Python 的 open(..., encoding="utf-8", errors="replace") 行为一致。
std::string ReadReportFile(std::string_view path);

// -----------------------------------------------------------------------------
// 正则自检
// -----------------------------------------------------------------------------

// 把 reports 模块用到的所有 re2 模式构造一遍，返回是否全部编译成功。
// 失败时把第一条错误写进 error。单元测试用它保证模式表没有笔误:
// re2 编译失败不会抛异常，只会让匹配永远失败 —— 那是"静默返回空"的经典来源。
bool AllReportPatternsValid(std::string* error);

}  // namespace fpgalsp::reports