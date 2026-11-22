// =============================================================================
//  src/verible_bridge/format_text.cpp
// =============================================================================
//  实现说明（踩过的坑都在这里，改动前请读完）：
//
//  1) style 必须由 `InitializeFromFlags()` 构造，**不要手工填 FormatStyle 字段**。
//     官方 CLI 走的就是这条路；它读的是 ABSL_FLAG 的定义（在
//     basic-format-style-init.cc / format-style-init.cc 里），因此默认值天然相同。
//     手工填字段会在 Verible 升级新增字段时静默落后。
//     → 本文件的做法：先 InitializeFromFlags 拿到"官方默认"，再**只覆盖**
//       DefaultStyleFlags()/调用方给出的那些 flag（OverlayStyleFlag）。
//
//  2) 为什么不直接把 style flag 塞给 absl 的全局 flag 注册表：
//     服务端是**多线程**的（格式化在 IO 线程，lint 在 worker 线程），改全局 flag
//     需要一把全局锁，而且会把"服务端进程的 flag 状态"变成隐式输入。
//     所以这里把 "‑‑name=value" 解析成对手工字段的赋值 —— 只影响本次调用的
//     style 副本，天然线程安全。
//
//  3) `ExecutionControl::max_search_states` 的**结构体默认值(10000) ≠ CLI 默认值(100000)**。
//     必须显式赋值（这里用 100000），否则遇到需要大量搜索的换行决策会提前报
//     "search space exceeded" —— 症状看起来像"格式化算法不同"，极难归因。
//     （见 doc/history/gate-reports/FORMAT-GATE-REPORT.md §7 坑 2）
//
//  4) `FormatVerilog()` 的签名里 `lines` 在 `control` **之前**，且 `lines` 为空表示
//     全文件格式化。传错位置时编译器给的是"C2665 没有重载可以转换所有参数类型"，
//     不直观，注意。
//
//  5) **不要在这里 set stdout 为二进制**：本函数只写 std::string，不碰 stdout。
//     `_setmode(_O_BINARY)` 是 format_dump（CLI 工具）才需要的事。
//
//  6) `verify_convergence`：官方 CLI 默认 true（会再格式化一遍检查是否收敛）。
//     服务端保留 true —— 它带来的开销可以接受（本项目最大文件 1.6 MB 之外都是
//     几十 KB 级），换来的是"格式化结果再次格式化不变"这一强保证，
//     避免用户每次保存都被改动一点点。
//
//  7) **默认值只有一个来源**（0.15.1 修正的回归）：
//     旧 Python 插件把排版风格放在 package.json 的 `fpgaLsp.verible.args` 默认值里
//     （160 列、**4 空格缩进**、一堆 alignment=align）。重写时服务端写死了
//     `indentation_spaces = 2`（Verible 默认），于是"格式化一次整份工程从 4 空格变 2 空格"。
//     现在：DefaultStyleFlags() 逐条复制旧插件默认值，服务端 settings 里的
//     `veribleArgs` 非空时覆盖它。
// =============================================================================

#include "verible_bridge/format_text.h"

#include <cstddef>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "verible/common/formatting/align.h"
#include "verible/common/formatting/basic-format-style-init.h"
#include "verible/common/formatting/basic-format-style.h"
#include "verible/verilog/formatting/format-style-init.h"
#include "verible/verilog/formatting/format-style.h"
#include "verible/verilog/formatting/formatter.h"
#include "verible_bridge/flag_tokens.h"

namespace fpgalsp::verible_bridge {

// 逐条照抄旧 Python 插件 `vscode-extension/package.json` 里
// `fpgaLsp.verible.args` 的默认值（顺序也保持一致，便于与插件 diff）。
// 修改这里请同步改插件 package.json —— 两处不一致就会出现"编辑器显示 4 空格、
// 服务端实际 2 空格"这种最难查的 bug。
std::vector<std::string> DefaultStyleFlags() {
    return {
        "--column_limit=160",
        "--indentation_spaces=4",
        "--wrap_spaces=4",
        "--try_wrap_long_lines=true",
        "--assignment_statement_alignment=align",
        "--case_items_alignment=align",
        "--distribution_items_alignment=align",
        "--enum_assignment_statement_alignment=align",
        "--module_net_variable_alignment=align",
        "--class_member_variable_alignment=align",
        "--struct_union_members_alignment=align",
        "--parameter_declaration_alignment=align",
        "--port_declarations_alignment=align",
        "--formal_parameters_alignment=align",
        "--named_parameter_alignment=align",
        "--named_port_alignment=align",
        "--port_declarations_right_align_packed_dimensions=true",
        "--port_declarations_right_align_unpacked_dimensions=true",
        "--formal_parameters_indentation=indent",
        "--named_parameter_indentation=indent",
        "--named_port_indentation=indent",
        "--port_declarations_indentation=indent",
        "--alignment_group_boundary=blank-lines-and-separator-comments",
    };
}

namespace {

namespace fmt = verilog::formatter;

enum class ApplyOutcome {
    kOk,        // 认得，且取值合法
    kUnknown,   // 不认识的 flag -> 记进 FormatResult::unknown_flags
    kInvalid,   // 认识，但取值非法 -> 格式化失败（比静默用错值安全）
};

// ParsedFlag / SplitToken / ParseBoolValue 都在 verible_bridge/flag_tokens.h
// （formatter 与 linter 共用同一套命令行写法，见那个文件的说明）。

ApplyOutcome SetInt(const ParsedFlag& flag, int* field, std::string* error) {
    if (!flag.has_value) {
        *error = "--" + flag.name + " requires an integer value (e.g. --" + flag.name + "=160)";
        return ApplyOutcome::kInvalid;
    }
    int parsed = 0;
    if (!ParseIntValue(flag.value, &parsed)) {
        *error = "--" + flag.name + "=" + flag.value + " is not a valid integer";
        return ApplyOutcome::kInvalid;
    }
    *field = parsed;
    return ApplyOutcome::kOk;
}

ApplyOutcome SetBool(const ParsedFlag& flag, bool* field, std::string* error) {
    if (!flag.has_value) {  // "--try_wrap_long_lines" 等价 true（absl flag 同语义）
        *field = true;
        return ApplyOutcome::kOk;
    }
    bool parsed = false;
    if (!ParseBoolValue(flag.value, &parsed)) {
        *error = "--" + flag.name + "=" + flag.value + " is not a valid boolean (true/false)";
        return ApplyOutcome::kInvalid;
    }
    *field = parsed;
    return ApplyOutcome::kOk;
}

// 把一条 flag 叠加到 style 上。**只覆盖** flag 里出现的字段；
// 没出现的字段保持 InitializeFromFlags() 给出的官方默认值。
ApplyOutcome OverlayStyleFlag(const ParsedFlag& flag, fmt::FormatStyle* style,
                              std::string* error) {
    const std::string& name = flag.name;

    // ---- 基础数值 ---------------------------------------------------------
    if (name == "column_limit") return SetInt(flag, &style->column_limit, error);
    if (name == "indentation_spaces") {
        return SetInt(flag, &style->indentation_spaces, error);
    }
    if (name == "wrap_spaces") return SetInt(flag, &style->wrap_spaces, error);
    if (name == "over_column_limit_penalty") {
        return SetInt(flag, &style->over_column_limit_penalty, error);
    }
    if (name == "line_break_penalty") {
        return SetInt(flag, &style->line_break_penalty, error);
    }

    // ---- 布尔开关 ---------------------------------------------------------
    if (name == "try_wrap_long_lines") {
        return SetBool(flag, &style->try_wrap_long_lines, error);
    }
    if (name == "expand_coverpoints") {
        return SetBool(flag, &style->expand_coverpoints, error);
    }
    if (name == "compact_indexing_and_selections") {
        return SetBool(flag, &style->compact_indexing_and_selections, error);
    }
    if (name == "class_parameter_space") {
        return SetBool(flag, &style->class_parameter_space, error);
    }
    if (name == "wrap_end_else_clauses") {
        return SetBool(flag, &style->wrap_end_else_clauses, error);
    }
    if (name == "port_declarations_right_align_packed_dimensions") {
        return SetBool(flag, &style->port_declarations_right_align_packed_dimensions, error);
    }
    if (name == "port_declarations_right_align_unpacked_dimensions") {
        return SetBool(flag, &style->port_declarations_right_align_unpacked_dimensions, error);
    }

    // ---- 枚举 -------------------------------------------------------------
    // 三类枚举各自用 Verible 自己的 AbslParseFlag 解析（不要手写字符串比较，
    // 否则 Verible 新增枚举值时会静默拒绝）。
    const auto set_alignment = [&](verible::AlignmentPolicy* field) {
        verible::AlignmentPolicy parsed{};
        std::string parse_error;
        if (!verible::AbslParseFlag(flag.value, &parsed, &parse_error)) {
            *error = "--" + name + "=" + flag.value + " is invalid: " + parse_error;
            return ApplyOutcome::kInvalid;
        }
        *field = parsed;
        return ApplyOutcome::kOk;
    };
    const auto set_indentation = [&](verible::IndentationStyle* field) {
        verible::IndentationStyle parsed{};
        std::string parse_error;
        if (!verible::AbslParseFlag(flag.value, &parsed, &parse_error)) {
            *error = "--" + name + "=" + flag.value + " is invalid: " + parse_error;
            return ApplyOutcome::kInvalid;
        }
        *field = parsed;
        return ApplyOutcome::kOk;
    };

    if (name == "assignment_statement_alignment") {
        return set_alignment(&style->assignment_statement_alignment);
    }
    if (name == "case_items_alignment") return set_alignment(&style->case_items_alignment);
    if (name == "distribution_items_alignment") {
        return set_alignment(&style->distribution_items_alignment);
    }
    if (name == "enum_assignment_statement_alignment") {
        return set_alignment(&style->enum_assignment_statement_alignment);
    }
    if (name == "module_net_variable_alignment") {
        return set_alignment(&style->module_net_variable_alignment);
    }
    if (name == "class_member_variable_alignment") {
        return set_alignment(&style->class_member_variable_alignment);
    }
    if (name == "struct_union_members_alignment") {
        return set_alignment(&style->struct_union_members_alignment);
    }
    if (name == "parameter_declaration_alignment") {
        return set_alignment(&style->parameter_declaration_alignment);
    }
    if (name == "port_declarations_alignment") {
        return set_alignment(&style->port_declarations_alignment);
    }
    if (name == "formal_parameters_alignment") {
        return set_alignment(&style->formal_parameters_alignment);
    }
    if (name == "named_parameter_alignment") {
        return set_alignment(&style->named_parameter_alignment);
    }
    if (name == "named_port_alignment") return set_alignment(&style->named_port_alignment);

    if (name == "formal_parameters_indentation") {
        return set_indentation(&style->formal_parameters_indentation);
    }
    if (name == "named_parameter_indentation") {
        return set_indentation(&style->named_parameter_indentation);
    }
    if (name == "named_port_indentation") {
        return set_indentation(&style->named_port_indentation);
    }
    if (name == "port_declarations_indentation") {
        return set_indentation(&style->port_declarations_indentation);
    }

    if (name == "alignment_group_boundary") {
        fmt::AlignmentGroupBoundary parsed{};
        std::string parse_error;
        if (!fmt::AbslParseFlag(flag.value, &parsed, &parse_error)) {
            *error = "--" + name + "=" + flag.value + " is invalid: " + parse_error;
            return ApplyOutcome::kInvalid;
        }
        style->alignment_group_boundary = parsed;
        return ApplyOutcome::kOk;
    }

    if (name == "line_terminator") {
        verible::LineTerminatorOptionStyle parsed{};
        std::string parse_error;
        if (!verible::AbslParseFlag(flag.value, &parsed, &parse_error)) {
            *error = "--" + name + "=" + flag.value + " is invalid: " + parse_error;
            return ApplyOutcome::kInvalid;
        }
        style->line_terminator = parsed;
        return ApplyOutcome::kOk;
    }

    return ApplyOutcome::kUnknown;
}

}  // namespace

FormatResult FormatText(std::string_view text, std::string_view filename,
                        const FormatOptions& options) {
    FormatResult result;
    result.text = std::string(text);

    // 空输入 / 纯空白：不调用 formatter。
    // 理由：Verible 对空文件会给出合法的空结果，但走一遍解析没有意义；
    // 更重要的是某些"只有注释"的文件在 range 模式下行为容易与直觉不符，
    // 这里直接短路，返回原文（ok=true，等价于"已符合格式"）。
    if (text.find_first_not_of(" \t\r\n") == std::string_view::npos) {
        result.ok = true;
        return result;
    }

    // ---- style：官方默认 + 覆盖 ---------------------------------------------
    fmt::FormatStyle style;
    fmt::InitializeFromFlags(&style);

    const std::vector<std::string> flags =
        options.style_flags.empty() ? DefaultStyleFlags() : options.style_flags;

    for (const ParsedFlag& flag : ParseFlagList(flags)) {
        std::string error;
        switch (OverlayStyleFlag(flag, &style, &error)) {
            case ApplyOutcome::kOk:
                break;
            case ApplyOutcome::kUnknown:
                // 记原文而不是归一化后的 name，方便用户对着自己的 settings 找
                result.unknown_flags.push_back("--" + flag.name);
                break;
            case ApplyOutcome::kInvalid:
                // 非法取值 -> 明确失败（不猜）。调用方保留原文，不会写坏文件。
                result.ok = false;
                result.error = error;
                return result;
        }
    }

    // ---- ExecutionControl：逐项对齐官方 CLI 默认值 ---------------------------
    fmt::ExecutionControl control;
    // ★ 结构体默认 10000，CLI 默认 100000（见文件头注释 3）
    control.max_search_states = 100000;
    // 官方 CLI 默认 true；保留它换取"再格式化不变"的强保证（见文件头注释 6）
    control.verify_convergence = true;
    // 诊断不要写到任何全局流里；服务端日志另行处理
    control.stream = nullptr;

    // lines 为空 = 全文件格式化（见文件头注释 4）
    const verible::LineNumberSet lines_to_format{};

    std::ostringstream formatted;
    const absl::Status status = verilog::formatter::FormatVerilog(
        text, filename, style, formatted, lines_to_format, control);

    if (!status.ok()) {
        // 失败时保留原文（与官方 --failsafe_success 的"原文永不丢失"一致），
        // 但**明确告知调用方失败**，不要伪装成"已格式化"。
        result.ok = false;
        result.error = std::string(status.message());
        return result;
    }

    result.text = formatted.str();
    result.ok = true;
    return result;
}

}  // namespace fpgalsp::verible_bridge