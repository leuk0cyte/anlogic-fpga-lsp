// =============================================================================
//  src/td/tcl_quote.h —— Tcl 命令行的安全拼装（B7 / S-01 修复）
// =============================================================================
//  背景：TD / BitWizard 通过 **stdin 喂 Tcl 脚本**执行命令，`.al` 里的
//  <TOP_MODULE><MODULE>、<HardWare><Device>、<Name> 以及约束集/策略名都是
//  不可信内容。修好之前它们是裸拼的：
//
//      "read_hdl -file {%s} -top %s" % (files, project.top)      # 裸拼 top
//      "import_device %s -package %s"  % (db, project.device)    # 裸拼 device
//
//  一个 `<MODULE>top; exec calc</MODULE>` 就足以在 TD 的 Tcl 解释器里执行任意
//  命令（而且保存文件即自动触发 TD 检查）。详见
//  doc/audit/2026-11-18/static/SECURITY.md 的 S-01。
//
//  本文件的约定：
//   * TclQuote(value)     —— 总是返回一个 Tcl **单词**，其求值结果与 value 逐字节
//                            相同；对任何输入都不会失败。所有来自外部数据（路径、
//                            名字）的位置都必须过它。
//   * IsTclIdentifier()   —— 用于"名字"类参数（顶层模块名等）。不满足时调用方应
//                            **安全失败**（报错/诊断），而不是把值拼进去。
//   * IsTclToken()        —— 用于"器件名/速度/扫描模式/约束集/run 名"这类单 token。
//
//  为什么不用简化实现（只把 `"` 转义）：Tcl 的替换规则里 `[` `$` `;` `\` `{}`
//  都有语义，而 `&` `|` `^` 在 Tcl 里**没有**特殊含义（命令是写进 TD 的 stdin，
//  不经过 cmd.exe）。所以必须按 Tcl 的词法规则处理，而不是按 Windows shell 规则。
//
//  兼容性：TclQuote 在"普通路径/普通标识符"下输出与旧代码**逐字节相同**
//  （不加任何引号；含空格时用 `{...}`），因此现有 TD 行为与单测期望值不变。
// =============================================================================

#pragma once

#include <string>
#include <string_view>

namespace fpgalsp::td {

// -----------------------------------------------------------------------------
// 判定
// -----------------------------------------------------------------------------

// 该字符串能否作为 Tcl 单词**裸写**（无需引号/花括号）。
// 只允许可见 ASCII 且排除所有 Tcl 元字符：`$ [ ] { } \ " ;`（`#` 也保守排除）。
inline bool IsTclBareSafe(std::string_view value) {
    if (value.empty()) return false;
    for (const unsigned char uc : value) {
        if (uc <= 0x20 || uc >= 0x7F) return false;  // 空白 / 控制字符 / 非 ASCII
        switch (static_cast<char>(uc)) {
            case '$':
            case '[':
            case ']':
            case '{':
            case '}':
            case '\\':
            case '"':
            case ';':
            case '#':
                return false;
            default:
                break;
        }
    }
    return true;
}

// 保守的"名字"校验（顶层模块名 / 端口名等）：`^[A-Za-z_][A-Za-z0-9_$]*$`
// 允许 `$`（Verilog 标识符合法），拒绝 Tcl 元字符、空白、点号与非 ASCII。
inline bool IsTclIdentifier(std::string_view value) {
    if (value.empty()) return false;
    const char first = value.front();
    const bool first_ok = (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') ||
                          first == '_';
    if (!first_ok) return false;
    return std::ranges::all_of(value, [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '$';
    });
}

// 保守的"单 token"校验（器件名 / 速度 / 扫描模式 / 约束集名 / run 名）：
// `^[A-Za-z0-9][A-Za-z0-9_.+-]*$`
inline bool IsTclToken(std::string_view value) {
    if (value.empty()) return false;
    const char first = value.front();
    const bool first_ok = (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') ||
                          (first >= '0' && first <= '9');
    if (!first_ok) return false;
    return std::ranges::all_of(value, [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '.' || c == '+' || c == '-';
    });
}

// -----------------------------------------------------------------------------
// 引用
// -----------------------------------------------------------------------------

// 返回一个 Tcl 单词，其求值结果 == value（逐字节）。
// 三种形态，按安全性从宽到严：
//   1) 裸写           —— 全是安全可见字符（`Y:/proj/a.v`、`top`、`ef2_4.db`）
//   2) `{...}`        —— 含空格等需要分组、但不含 `\ { } 换行`（`{Y:/a b.v}`）
//   3) `"..."` + 转义 —— 含 `\ { }` 或换行：逐字符转义（`\`→`\\`、`"`→`\"`、
//                        `$`→`\$`、`[`→`\[`、换行→`\n`）
// 空串返回 `""`（否则命令会少一个参数）。
inline std::string TclQuote(std::string_view value) {
    if (value.empty()) return "\"\"";
    if (IsTclBareSafe(value)) return std::string(value);

    const bool brace_hostile = value.find_first_of("\\{}") != std::string_view::npos ||
                               value.contains('\n') ||
                               value.contains('\r');
    if (!brace_hostile) return "{" + std::string(value) + "}";

    std::string out;
    out.reserve(value.size() + 8);
    out.push_back('"');
    for (const char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '$': out += "\\$"; break;
            case '[': out += "\\["; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    out.push_back('"');
    return out;
}

// 总是产出"被分组"的单词：brace 安全时 `{...}`，否则退化成转义引号形式。
// 用在**历史上就是花括号形态**的位置（TD 的 CLI 示例写作 `program {<abs .bit>}`、
// `open_project {<abs .al>}`、`read_hdl -file {a.v b.v}`），这样合法输入的输出
// 与旧实现逐字节一致（既有单测/契约不漂移），而含 `}` `\` 换行的恶意值仍被转义。
inline std::string TclQuoteGrouped(std::string_view value) {
    const bool brace_hostile = value.find_first_of("\\{}") != std::string_view::npos ||
                               value.contains('\n') ||
                               value.contains('\r');
    if (!brace_hostile) return "{" + std::string(value) + "}";
    return TclQuote(value);
}

// 名字类参数：合法 -> TclQuote（实际就是裸写）；非法 -> 空 optional（调用方安全失败）。
// 用它代替 "先校验再拼" 的两步写法，避免有人只做了其中一步。
inline bool TryTclQuoteIdentifier(std::string_view value, std::string* out) {
    if (out == nullptr || !IsTclIdentifier(value)) return false;
    *out = TclQuote(value);
    return true;
}

// token 类参数：同上。
inline bool TryTclQuoteToken(std::string_view value, std::string* out) {
    if (out == nullptr || !IsTclToken(value)) return false;
    *out = TclQuote(value);
    return true;
}

}  // namespace fpgalsp::td