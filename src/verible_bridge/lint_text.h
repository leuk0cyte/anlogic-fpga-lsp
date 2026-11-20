// =============================================================================
//  src/verible_bridge/lint_text.h —— 内嵌 Verible lint autofix 的薄封装
// =============================================================================
//  为什么需要这一层（与 format_text.h 同一设计）：
//    * `src/server/` 不该直接 include Verible 的内部头文件（include 根、
//      absl 依赖、/bigobj、/WHOLEARCHIVE 等构建细节不该扩散到服务层）；
//    * lint 是**可选能力**：`FPGA_LSP_VERIBLE_ENABLED=OFF` 时整个桥不参与构建，
//      服务层用 `FPGA_LSP_HAVE_VERIBLE_BRIDGE` 宏决定是否提供 autofix code action。
//
//  对应参考实现：Y:\fpga-lsp\fpgalsp\verible.py::Verible.autofix —— Python 调外部
//  `verible-verilog-lint --autofix=inplace`；这里改成**进程内**跑同一套规则：
//      VerilogAnalyzer::AnalyzeAutomaticPreprocessFallback
//        + VerilogLintTextStructure(默认 ruleset)
//        + 收集 LintViolation.autofixes，按位置排序后**应用互不重叠的编辑**
//  产品约束（2026-11-18，Lead 拍板）：单一静态 exe 是硬目标，不依赖外部
//  verible-verilog-lint.exe。
//
//  ⚠ 与 Python 的**已知差异**（LSP-18 同族）：
//    内嵌规则集 c6b29d62 vs Python 外部 0.0-4296。同一输入下 autofix 的**文本结果
//    可能不同**（规则增删、同一规则措辞/行为变化）。这是版本基线差异，不是移植缺陷；
//    详见 doc/audit/2026-11-18/B3-REPORT.md。
//
//  ★ lint 规则参数（0.15.1）：
//    旧插件的 `fpgaLsp.verible.lintArgs` 默认值是**字符串**
//    `"--rules=+line-length=length:160"`，而旧服务端只接受 list（`isinstance(lint_args, list)`）
//    → 那个默认值其实**被静默丢弃**，Python 版实际跑的是 Verible 默认规则集（行宽 100）。
//    这里改为：默认 `DefaultLintFlags()`（行宽 160，与格式化列宽一致，也是旧插件
//    "本意"），并且**真正解析** `--rules=` / `--ruleset=` / `--rules_config=` /
//    `--rules_config_search` / `--waiver_files=`，让用户设置生效。
//
//  约定（与 format_text.h 一致）：
//    * **不抛异常**、不写盘、不读全局 absl flag（不调用 InitCommandLine —— 那会
//      解析服务端自己的 argv）；
//    * 空文本直接返回 ok=true 且 changed=false；
//    * 有任何失败（解析/配置）都返回 ok=false，调用方据此**不产生** code action
//      （Python 在工具失败时同样不产生）。
// =============================================================================

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace fpgalsp::verible_bridge {

// 旧插件 `fpgaLsp.verible.lintArgs` 的默认值（Python 版是字符串，这里归一成数组）。
// 唯一目的：让内嵌 lint 的行宽阈值（160）与 formatter 的 column_limit 一致 ——
// 否则"格式化一次就把刚写好的行判成超长"。
std::vector<std::string> DefaultLintFlags();

struct LintAutofixResult {
    bool ok = false;
    bool changed = false;
    std::string text;   // ok=true 时是修复后的文本（行尾按输入原样，调用方自行归一化）
    std::string error;  // 失败原因（不含路径，供日志/报告）
    int applied_edits = 0;
    std::vector<std::string> unknown_flags;  // 被忽略的 lint flag（调用方记日志）
};

// 用内嵌 Verible 的 lint 规则跑一遍 `text`，并把可应用的 autofix 合成一份新文本。
//   text       待检查的源码（UTF-8）
//   filename   仅用于诊断/规则的文件名相关判断（如 module-filename）
//   lint_flags 形如 "--rules=+line-length=length:160"；**空 = DefaultLintFlags()**
LintAutofixResult AutofixText(std::string_view text, std::string_view filename,
                              const std::vector<std::string>& lint_flags = {});

// -----------------------------------------------------------------------------
//  T1 / task-50：**只收集 violation**，不生成修复文本（服务端 lint 用）
// -----------------------------------------------------------------------------
//  与 AutofixText 走**同一条** Verible 管线（同一个 default ruleset、同一个
//  AnalyzeAutomaticPreprocessFallback 入口），区别只是：
//    * 不读 autofix、不拼新文本；
//    * 额外按 (line, column) 报出每个 violation 的位置与规则名。
//
//  ⚠ 坐标口径：`line` 是 0-based；`column` / `end_column` 是 **Unicode 码点**
//    列（Verible 的 LineColumn 语义，见 thirdparty/.../line-column-map.h）。
//    LSP 要的是 UTF-16 码元列，转换在 **server 侧**做
//    （platform::codepoint_index_to_utf16_column），本层不转 —— 保持 bridge 只认
//    "Verible 的坐标"，不用关心 LSP 的编码细节。
struct LintDiagnostic {
    int line = 0;
    int column = 0;      // 0-based, UTF-8 码点列
    int end_line = 0;
    int end_column = 0;  // 0-based, UTF-8 码点列
    int severity = 2;    // LSP 口径：1=Error 2=Warning 3=Info 4=Hint（lint 一律 2）
    std::string rule;    // Verible 规则名，如 "no-tabs"
    std::string message; // violation.reason
};

struct LintResult {
    bool ok = false;
    std::vector<LintDiagnostic> diagnostics;
    std::string error;  // ok=false 时给出原因（不抛异常）
    std::vector<std::string> unknown_flags;  // 被忽略的 lint flag（调用方记日志）
};

// 对 `text` 跑一遍内嵌 Verible lint 并返回结构化 violation。
// never-raise；空文本返回 ok=true + 空列表。
LintResult LintText(std::string_view text, std::string_view filename,
                    const std::vector<std::string>& lint_flags = {});

// 复现旧插件 `veribleDiagnostics.js::buildCommentIndex/isInsideComment` 的语义：
// **丢掉位置落在注释里的 `no-tabs` violation**（代码里的 tab 照报）。
//
// 为什么需要它（T1 / task-50，Lead 明确要求）：
//   旧插件在客户端把"注释里的 tab"过滤掉了；那个模块已删除。如果服务端直接吐
//   Verible 原始 violation，用户会在注释里看到一堆 "Use spaces, not tabs"，
//   比改之前更吵。这里保持等价语义。
//
// 注释识别：`//` 到行尾、`/* ... */`（可跨行）；`"..."` 字符串里的 `//` **不算**
// 注释（处理 `\"` 与 `\\` 转义）—— 与旧实现一致。
std::vector<LintDiagnostic> DropCommentNoTabs(std::string_view text,
                                             std::vector<LintDiagnostic> diagnostics);

}  // namespace fpgalsp::verible_bridge