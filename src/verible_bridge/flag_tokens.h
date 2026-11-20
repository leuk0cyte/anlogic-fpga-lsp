// =============================================================================
//  src/verible_bridge/flag_tokens.h —— "--flag=value" 词法（formatter/lint 共用）
// =============================================================================
//  为什么单独一个头文件：
//    * formatter 的 style flag 与 linter 的 `--rules=` 是**同一族**命令行写法
//      （都来自旧插件 package.json 的默认真实字符串），解析规则必须一致：
//        --column_limit=160     ← 插件默认写法
//        --try_wrap_long_lines  ← 只写开关 = true
//        --indentation_spaces 4 ← absl CLI 也接受的两段式
//        column_limit=160       ← 宽松：不带前导 --
//    * 两处各写一份解析，早晚会不一致（这个 bug 类在这项目里出现过：
//      "插件默认 4 空格 / 服务端写死 2 空格"）。
//
//  只做**纯字符串**处理，不 include 任何 Verible/absl 头 —— 这样它可以被任意
//  单元测试直接 include。
// =============================================================================

#pragma once

#include <charconv>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fpgalsp::verible_bridge {

struct ParsedFlag {
    std::string name;   // 去掉前导 '-' 与 "=value" 的部分
    std::string value;  // "=value" 或两段式里的第二段
    bool has_value = false;
};

// 解析**单个** token；不带取值的返回 has_value=false。
// 返回 false 表示 token 为空 / 只有横线 —— 调用方跳过。
inline bool SplitToken(std::string_view token, ParsedFlag* out) {
    out->name.clear();
    out->value.clear();
    out->has_value = false;
    if (token.empty()) return false;
    if (token.size() > 2 && token[0] == '-' && token[1] == '-') {
        token.remove_prefix(2);
    } else if (token.size() > 1 && token[0] == '-') {
        token.remove_prefix(1);
    }
    const std::size_t equals = token.find('=');
    if (equals == std::string_view::npos) {
        out->name = std::string(token);
        return !out->name.empty();
    }
    out->name = std::string(token.substr(0, equals));
    out->value = std::string(token.substr(equals + 1));
    out->has_value = true;
    return !out->name.empty();
}

// 把一串 flag 归一成 ParsedFlag 列表，并处理"flag 后跟独立取值"的两段式
// （"--indentation_spaces" "4"）：只有当下一项不以 '-' 开头时才当作取值。
inline std::vector<ParsedFlag> ParseFlagList(const std::vector<std::string>& flags) {
    std::vector<ParsedFlag> parsed;
    parsed.reserve(flags.size());
    for (std::size_t index = 0; index < flags.size(); ++index) {
        ParsedFlag flag;
        if (!SplitToken(flags[index], &flag)) continue;
        if (!flag.has_value && index + 1 < flags.size()) {
            const std::string& next = flags[index + 1];
            if (!next.empty() && next[0] != '-') {
                flag.value = next;
                flag.has_value = true;
                ++index;
            }
        }
        parsed.push_back(std::move(flag));
    }
    return parsed;
}

// 布尔取值：与 absl flag 常见写法一致（true/false/1/0/yes/no/on/off）。
inline bool ParseBoolValue(std::string_view text, bool* out) {
    if (text == "true" || text == "TRUE" || text == "True" || text == "1" ||
        text == "yes" || text == "on") {
        *out = true;
        return true;
    }
    if (text == "false" || text == "FALSE" || text == "False" || text == "0" ||
        text == "no" || text == "off") {
        *out = false;
        return true;
    }
    return false;
}

// 整数取值；必须整串都能解析（"160abc" 视为非法）。
inline bool ParseIntValue(std::string_view text, int* out) {
    if (text.empty()) return false;
    int parsed = 0;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, parsed);
    if (result.ec != std::errc() || result.ptr != last) return false;
    *out = parsed;
    return true;
}

}  // namespace fpgalsp::verible_bridge