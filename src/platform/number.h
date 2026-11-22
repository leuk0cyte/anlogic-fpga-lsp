// =============================================================================
//  src/platform/number.h —— 整数文本解析的唯一实现（A-1）
// =============================================================================
//  为什么需要：`std::atoi` 在 0.26.0 扩散到 37 处（probe.cpp 一个文件 23 处）。
//  atoi 的三个缺陷正是"解析外部文本"最不能有的：
//    1) 失败返回 0 —— `--attach-port abc` 静默变成"绑定随机端口"；
//    2) 溢出是**未定义行为**（实践中截断）—— `70000` 变 4464，连到别的端口；
//    3) 不报告"尾巴没吃完" —— `12abc` 静默当 12。
//
//  两个入口，按调用点语义二选一：
//    * `ParseDecimal<T>`：**严格**，给"用户显式给的值"（CLI / env）。
//      from_chars 不分配、不抛、不受 locale 影响；失败返回带原因的 expected，
//      调用方必须显式报错并退出。
//    * `DigitsToIntSaturating`：**宽松 + 饱和**，给"从工具输出里抓出来的位号/
//      计数"（ChipWatcher/TD 报告）。一个荒谬的位号应当被当成"越界"（后续会
//      被既有范围检查拒掉），而不是让整个探针解析失败；绝不静默变成 0。
//      语义与 `constraints/adc.cpp` 里 0.25 时代就正确的 `DigitsToInt` 一致
//      （这正是本报告最有力的内部证据：正确实现早就在库里，只是没被复用）。
//
//  纯头文件、无状态、不抛异常。
// =============================================================================

#pragma once

#include <charconv>
#include <concepts>
#include <cstddef>
#include <expected>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace fpgalsp::platform {

namespace detail {

// 给错误消息用的类型名（std::format 不支持直接格式化类型，且本库不用 RTTI 名）。
template <std::integral T>
constexpr std::string_view IntTypeName() noexcept {
    if constexpr (std::is_same_v<T, char>) {
        return "char";
    } else if constexpr (std::is_same_v<T, signed char>) {
        return "signed char";
    } else if constexpr (std::is_same_v<T, unsigned char>) {
        return "unsigned char";
    } else if constexpr (std::is_same_v<T, short>) {
        return "short";
    } else if constexpr (std::is_same_v<T, unsigned short>) {
        return "unsigned short";
    } else if constexpr (std::is_same_v<T, int>) {
        return "int";
    } else if constexpr (std::is_same_v<T, unsigned int>) {
        return "unsigned int";
    } else if constexpr (std::is_same_v<T, long>) {
        return "long";
    } else if constexpr (std::is_same_v<T, unsigned long>) {
        return "unsigned long";
    } else if constexpr (std::is_same_v<T, long long>) {
        return "long long";
    } else if constexpr (std::is_same_v<T, unsigned long long>) {
        return "unsigned long long";
    } else {
        return "整数类型";
    }
}

}  // namespace detail

// 严格解析十进制整数（可带正负号；**不接受**前后空白、不接受 0x 前缀、不接受
// 除数字外的任何字符）。成功返回 T；失败返回可直接展示的中文原因。
//
//   ParseDecimal<int>("-42")        -> -42
//   ParseDecimal<int>("abc")        -> unexpected("不是整数: 'abc'")
//   ParseDecimal<uint16_t>("70000") -> unexpected("超出 unsigned short 范围: '70000'")
//   ParseDecimal<unsigned short>("-1") -> unexpected(...)（负号对无符号非法）
template <std::integral T>
[[nodiscard]] std::expected<T, std::string> ParseDecimal(std::string_view text) {
    T value{};
    const char* const begin = text.data();
    const char* const end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec == std::errc::result_out_of_range) {
        return std::unexpected(
            std::format("超出 {} 范围: '{}'", detail::IntTypeName<T>(), text));
    }
    if (ec != std::errc{} || ptr != end) {
        return std::unexpected(std::format("不是整数: '{}'", text));
    }
    return value;
}

// 把一段可能很脏的文本里的十进制数字解析成 int，**饱和**到 int 上下限。
//   * 跳过非数字字符（与 0.25 的 adc::DigitsToInt 语义一致：报告里的
//     `CWC_BUS_NUM = 3` 这种行、`<width>32</width>` 这种片段都能直接喂）；
//   * 前导 `-`/`+` 有效（TD 报告里 data_offset 等字段可能带负号）；
//   * 超长数字 -> INT_MAX / INT_MIN（"荒谬位号 = 越界"，而不是 0）。
[[nodiscard]] constexpr int DigitsToIntSaturating(std::string_view text) noexcept {
    // ⚠ Windows 的 <windows.h>（未定义 NOMINMAX 时）把 max/min 定义成宏：直接写
    // `std::numeric_limits<int>::max()` 会被宏展开破坏（clang 报 "too few arguments
    // to function-like macro"）。多一层括号就绕开宏，比要求每个 TU 定义 NOMINMAX 稳。
    constexpr long long kMax = (std::numeric_limits<int>::max)();
    constexpr long long kMin = -(std::numeric_limits<int>::max)();
    std::size_t index = 0;
    bool negative = false;
    if (index < text.size() && (text[index] == '-' || text[index] == '+')) {
        negative = text[index] == '-';
        ++index;
    }
    long long value = 0;
    for (; index < text.size(); ++index) {
        const char c = text[index];
        if (c < '0' || c > '9') continue;
        value = value * 10 + (c - '0');
        if (value > kMax) return static_cast<int>(negative ? kMin : kMax);
    }
    return static_cast<int>(negative ? -value : value);
}

}  // namespace fpgalsp::platform
