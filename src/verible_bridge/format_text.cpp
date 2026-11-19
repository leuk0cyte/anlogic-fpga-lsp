// =============================================================================
//  src/verible_bridge/format_text.cpp
// =============================================================================
//  实现说明（踩过的坑都在这里，改动前请读完）：
//
//  1) style 必须由 `InitializeFromFlags()` 构造，**不要手工填 FormatStyle 字段**。
//     官方 CLI 走的就是这条路；它读的是 ABSL_FLAG 的定义（在
//     basic-format-style-init.cc / format-style-init.cc 里），因此默认值天然相同。
//     手工填字段会在 Verible 升级新增字段时静默落后。
//
//  2) `ExecutionControl::max_search_states` 的**结构体默认值(10000) ≠ CLI 默认值(100000)**。
//     必须显式赋值（这里用 100000），否则遇到需要大量搜索的换行决策会提前报
//     "search space exceeded" —— 症状看起来像"格式化算法不同"，极难归因。
//     （见 doc/history/gate-reports/FORMAT-GATE-REPORT.md §7 坑 2）
//
//  3) `FormatVerilog()` 的签名里 `lines` 在 `control` **之前**，且 `lines` 为空表示
//     全文件格式化。传错位置时编译器给的是"C2665 没有重载可以转换所有参数类型"，
//     不直观，注意。
//
//  4) **不要在这里 set stdout 为二进制**：本函数只写 std::string，不碰 stdout。
//     `_setmode(_O_BINARY)` 是 format_dump（CLI 工具）才需要的事。
//
//  5) `verify_convergence`：官方 CLI 默认 true（会再格式化一遍检查是否收敛）。
//     服务端保留 true —— 它带来的开销可以接受（本项目最大文件 1.6 MB 之外都是
//     几十 KB 级），换来的是"格式化结果再次格式化不变"这一强保证，
//     避免用户每次保存都被改动一点点。
// =============================================================================

#include "verible_bridge/format_text.h"

#include <sstream>
#include <string>

#include "absl/status/status.h"
#include "verible/common/formatting/basic-format-style-init.h"
#include "verible/verilog/formatting/format-style-init.h"
#include "verible/verilog/formatting/formatter.h"

namespace fpgalsp::verible_bridge {
namespace {

// 把 FormatOptions 应用到 style 上。
//
// 注意：`InitializeFromFlags()` 已经把 ABSL_FLAG 的默认值写进 style 了，
// 这里只**覆盖**我们关心的几个字段。没覆盖的字段（各种 *_alignment、
// *_indentation 等）保持 Verible 的官方默认值 —— 这正是我们要的：
// 与 `verible-verilog-format` 在相同参数下逐字节一致。
void ApplyOptions(const FormatOptions& options,
                  verilog::formatter::FormatStyle* style) {
    style->column_limit = options.column_limit;
    style->indentation_spaces = options.indentation_spaces;
    style->wrap_spaces = options.wrap_spaces;
    if (options.auto_line_terminator) {
        style->line_terminator = verible::LineTerminatorOptionStyle::kAuto;
    }
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

    // ---- style：官方同一个入口 ------------------------------------------------
    verilog::formatter::FormatStyle style;
    verilog::formatter::InitializeFromFlags(&style);
    ApplyOptions(options, &style);

    // ---- ExecutionControl：逐项对齐官方 CLI 默认值 ---------------------------
    verilog::formatter::ExecutionControl control;
    // ★ 结构体默认 10000，CLI 默认 100000（见文件头注释 2）
    control.max_search_states = 100000;
    // 官方 CLI 默认 true；保留它换取"再格式化不变"的强保证（见文件头注释 5）
    control.verify_convergence = true;
    // 诊断不要写到任何全局流里；服务端日志另行处理
    control.stream = nullptr;

    // lines 为空 = 全文件格式化（见文件头注释 3）
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