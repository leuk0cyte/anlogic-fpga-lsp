// =============================================================================
//  src/verible_bridge/format_text.h —— 内嵌 Verible formatter 的薄封装
// =============================================================================
//  为什么需要这一层：
//    * `src/server/` 是编排层，不应该直接 include Verible 的内部头文件
//      （那会把 "verible/..." 的 include 根、absl 依赖、/bigobj 等构建细节
//       扩散到服务层）。
//    * formatter 是**可选能力**：`FPGA_LSP_VERIBLE_ENABLED=OFF` 时整个桥
//      不参与构建，`src/server` 用 `FPGA_LSP_HAVE_VERIBLE_BRIDGE` 宏决定
//      走真实格式化还是返回空编辑。
//
//  ⚠ 与 src/tools/format_dump/main.cpp 的关系（重要，别各写一套）：
//    format_dump 是**门禁工具**，它的职责是"证明移植与官方二进制逐字节一致"，
//    因此它复刻了官方 CLI 的**全部**行为（argv 解析、failsafe 退出码、诊断走 stdout、
//    `_setmode(_O_BINARY)`）。
//    本文件是**服务端调用**，只需要"给文本 -> 拿格式化结果"，不需要 CLI 语义。
//    两者共享的是**同一个 Verible API 调用形状**：
//        InitializeFromFlags(&style)  +  FormatVerilog(text, name, style, out, lines, control)
//    其中 `ExecutionControl::max_search_states` 的结构体默认值(10000) **不等于**
//    官方 CLI 的默认值(100000)，两者都必须显式赋 flag 值 —— 这个坑在
//    tests/FORMAT-GATE-REPORT.md 的「坑 2」有完整记录。
//
//  等价性保证：
//    本封装产出的格式化结果，与 `verible-verilog-format` 在同一 style 下
//    **逐字节一致**（由 ctest -L formatgate 的 102/102 + 194/194 区间验证）。
// =============================================================================

#pragma once

#include <string>
#include <string_view>

namespace fpgalsp::verible_bridge {

// 格式化选项。默认值与项目既有约定保持一致：
//   * column_limit = 160
//     —— 不是 Verible 的 100。本项目的 lint 规则 `line-length` 在 0.10.30 起
//        设为 160 就是为了**与 formatter 对齐**；如果这里回到 100，
//        那么"格式化一次就把刚写好的行判成超长"，形成 format/lint 互相打架。
//   * indentation_spaces = 2
//   * line_terminator = auto（跟随输入：LF 输入出 LF，CRLF 输入出 CRLF）
struct FormatOptions {
    int column_limit = 160;
    int indentation_spaces = 2;
    int wrap_spaces = 4;
    // true = 保留输入的行尾风格（与官方 --line_terminator=auto 一致）
    bool auto_line_terminator = true;
};

struct FormatResult {
    bool ok = false;
    std::string text;    // ok=true 时是格式化结果；ok=false 时为原文
    std::string error;   // ok=false 时是失败原因（Verible 的 status message）
};

// 格式化整个文件。
//
// 约定（与项目其它层一致）：
//   * **不抛异常**。Verible 内部失败会转成 ok=false + error。
//   * `text` 为空或只有空白时直接原样返回（ok=true），不调用 formatter。
//   * filename 只用于诊断信息（`#include`、错误消息里的文件名）。
FormatResult FormatText(std::string_view text, std::string_view filename,
                        const FormatOptions& options = {});

}  // namespace fpgalsp::verible_bridge