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
//    doc/history/gate-reports/FORMAT-GATE-REPORT.md 的「坑 2」有完整记录。
//
//  ★ 默认排版风格（0.15.1 修正 / 0.15.2 收口）：
//    **不再**在代码里写死 column_limit/indentation_spaces，而是用
//    `DefaultStyleFlags()` —— 它逐条复制旧 Python 插件 package.json 里
//    `fpgaLsp.verible.args` 的默认值（160 列 / **4 空格缩进** / try_wrap_long_lines /
//    一组 alignment=align / alignment_group_boundary=...）。
//    0.15.2 起插件的 `fpgaLsp.verible.args` 默认值改成**空数组**：这套风格只存在于
//    本文件一处，插件设置只在用户主动覆盖时才有内容（否则两边各一份，早晚走偏）。
//    历史教训：重写时把 indentation_spaces 写死成 Verible 的 2，而插件默认值仍是 4，
//    结果是"格式化一次就把整份工程从 4 空格改成 2 空格"（实测 15 KB 文件 241 行增、
//    251 行删）。默认值只能有**一个**来源，见下面的 kDefaultStyleFlags。
//
//  等价性保证：
//    同一组 style flag 下，本封装产出与 `verible-verilog-format` **逐字节一致**
//    （由 ctest -L formatgate 的 102/102 + 194/194 区间验证；服务端这条路径
//     另由 tests/unit/verible_style_test.cpp 覆盖）。
// =============================================================================

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace fpgalsp::verible_bridge {

// 旧插件（Python 版 `vscode-extension/package.json` 的 `fpgaLsp.verible.args`
// 默认值）的 style 参数，**逐条照抄**。这是团队排版风格的唯一来源：
//   * 改这里 = 改所有人的格式化结果，请同时改插件 package.json 的默认值；
//   * 字符串形式是 Verible CLI 的 flag（"--name=value"），与旧插件一致，
//     这样用户把 settings.json 里那串原样贴过来也是同样结果。
std::vector<std::string> DefaultStyleFlags();

struct FormatOptions {
    // "--flag=value" 形式的 Verible style flag。**空 = DefaultStyleFlags()**；
    // 非空 = **整体替换**（不是逐条合并）—— 这是刻意的语义：插件设置里写了什么就是什么，
    // 没写的字段回落到 Verible 原生默认。想改单项又不想丢掉团队风格，就把默认列表抄过去改。
    // 支持 "--flag=value" / "--flag value" / 裸 "flag=value"（宽松解析）；
    // bool flag 可以只写 "--flag"（等价 =true）。
    std::vector<std::string> style_flags;
};

struct FormatResult {
    bool ok = false;
    std::string text;    // ok=true 时是格式化结果；ok=false 时为原文
    std::string error;   // ok=false 时是失败原因（Verible 的 status message / 非法 flag）
    // 无法识别的 style flag（例如 Verible 升级后改名）。**不致命**：
    // 旧插件是把它交给 verible CLI 由 CLI 报错退出，服务端里那样做会让"一个拼错的
    // 参数 = 整个格式化不可用"，所以这里降级为"忽略 + 由调用方记日志"。
    std::vector<std::string> unknown_flags;
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