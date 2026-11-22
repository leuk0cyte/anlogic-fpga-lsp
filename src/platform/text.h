// =============================================================================
//  src/platform/text.h —— 全库唯一的 ASCII 文本小工具实现（header-only）
// =============================================================================
//  为什么有这个文件（2026-11-22 现代化审计 B-1）：
//    在 0.26.0 时代，`Trim` / `LowerAscii` / `ToLower` / `EndsWith` / `Join`
//    这类小函数在 src/ 里有 **43 个各自独立的定义点**（8 份 Trim、8 份
//    LowerAscii、6 份 ToLower、6 份 EndsWith、4 份 Join …），而且 0.26 新写的
//    `features_optional.cpp` 还在继续复制 —— 说明"复制一份"是本仓库的默认惯性。
//    轮子多的直接代价不只是重复代码：
//      * `EndsWith` 的 6 份里有 2 份曾经**没有 `size() >= suffix.size()` 保护**，
//        一旦有人"顺手简化"，`text.size() - suffix.size()` 会在 size_t 下溢，
//        随后 `compare(pos)` 抛 `std::out_of_range` —— 重复实现直接变成缺陷面；
//      * 4 份 `Join` 各自维护"反斜杠转正斜杠 + 补分隔符"的规则，路径语义随时分叉。
//
//  合并口径（重要，避免"为了合并而合并"）：
//    * 只合并**语义逐字节相同**的 ASCII 实现 —— 全部按字节处理，不做 locale
//      相关的 `std::tolower`/`std::isspace` 行为（C locale 下固然等价，但把
//      隐式依赖全局 locale 的调用集中到一处反而更危险）。
//    * **不合并**语义不同的轮子，例如：
//        - `constraints::text::Strip/SplitLines`（复刻 Python str.strip/splitlines，
//          认 \v\f\x1c-\x1e，且末尾分隔符不产生空行）—— 它有独立的 Python 对照口径；
//        - `reports::SplitLines`（复刻 Python `text.split("\n")`，**保留**末尾空行，
//          timing 的 legacy 兜底依赖这个差异）；
//        - `path_join`（会做 normalize_path：折叠 `..`/`.`、空目录转绝对路径），
//          与 flow 里的 `Join`（只拼分隔符、不做 normalize）语义不同，故两者并存；
//        - `xml_lite` 的 `IsSpace`（XML 空白只有 space/\t/\r/\n，不含 \v\f）。
//
//  Trim 的空白集合：ASCII 空白 space/\t/\r/\n/\v/\f（与 CTYPE 在 C locale 下一致）。
//    历史上 analysis.cpp 那份只 trim 4 种（不含 \v\f），合并后统一成 6 种：\v\f
//    出现在报告/路径两端时会被多剥掉，语义更正确且这些字符在本库输入里不出现在
//    行首尾的"有意义"位置（报告生成器本身按 \n 分行）。
//
//  全部函数 never-throw：输入任意字节都返回确定结果。为了能被放在公共头里，
//  这里只 include <string>/<string_view>（不含 <format> 之类的重模板）。
// =============================================================================

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace fpgalsp::platform {

// ASCII 空白（与 `std::isspace` 在 C locale 下一致）。
constexpr bool IsAsciiSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

// 历史别名（flow_status 等文件原本叫 IsSpace）。语义与 IsAsciiSpace 相同；
// 注意 `xml_lite` 里的 `IsSpace` **不是**这一份（XML 空白不含 \v\f），故不迁移。
constexpr bool IsSpace(char c) noexcept { return IsAsciiSpace(c); }

// 去两端 ASCII 空白（返回新字符串，保持原有 `Trim` 的值语义）。
inline std::string Trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && IsAsciiSpace(text[begin])) ++begin;
    while (end > begin && IsAsciiSpace(text[end - 1])) --end;
    return std::string(text.substr(begin, end - begin));
}

// ASCII 小写化（只折叠 A-Z；>=0x80 的字节原样保留，不做 UTF-8 解码）。
inline std::string LowerAscii(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

// ASCII 大写化。
inline std::string UpperAscii(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

// 历史别名（各文件原本的命名不同，统一到 LowerAscii/UpperAscii 之上）。
inline std::string ToLower(std::string_view text) { return LowerAscii(text); }
inline std::string ToLowerAscii(std::string_view text) { return LowerAscii(text); }
inline std::string ToUpper(std::string_view text) { return UpperAscii(text); }

// 前缀 / 后缀判定。⚠ 必须保留长度保护：`std::string_view::ends_with` 本身
// 对短串是安全的，但历史上手写版用 `compare(size() - suffix.size())` 时没有
// 保护就会 size_t 下溢（见文件头）。这两个函数就是那个保护唯一存在的地方。
inline bool StartsWith(std::string_view text, std::string_view prefix) noexcept {
    return text.size() >= prefix.size() && text.starts_with(prefix);
}

inline bool EndsWith(std::string_view text, std::string_view suffix) noexcept {
    return text.size() >= suffix.size() && text.ends_with(suffix);
}

// 目录拼接：`dir` + 一个分隔符 + `name`，并把反斜杠统一成正斜杠。
//   * `dir` 为空时直接返回 `name`（**不**像 `platform::path_join` 那样把
//     相对路径转成绝对路径）；
//   * 不做 normalize_path：不折叠 `..`/`.`，调用方拿到的是纯字符串拼接结果。
//   这两条就是它与 `platform::path_join` 的语义差异，必须保留（flow 的 run
//   目录拼接依赖"空目录 = 相对当前目录"）。
inline std::string Join(std::string_view dir, std::string_view name) {
    if (dir.empty()) return std::string(name);
    std::string out(dir);
    if (out.back() != '/' && out.back() != '\\') out.push_back('/');
    out.append(name);
    for (char& c : out) {
        if (c == '\\') c = '/';
    }
    return out;
}

}  // namespace fpgalsp::platform
