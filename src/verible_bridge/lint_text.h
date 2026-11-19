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
//    详见 audit/2026-11-18/B3-REPORT.md。
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

namespace fpgalsp::verible_bridge {

struct LintAutofixResult {
    bool ok = false;
    bool changed = false;
    std::string text;   // ok=true 时是修复后的文本（行尾按输入原样，调用方自行归一化）
    std::string error;  // 失败原因（不含路径，供日志/报告）
    int applied_edits = 0;
};

// 用内嵌 Verible 的 lint 规则跑一遍 `text`，并把可应用的 autofix 合成一份新文本。
//   text     待检查的源码（UTF-8）
//   filename 仅用于诊断/规则的文件名相关判断（如 module-filename）
LintAutofixResult AutofixText(std::string_view text, std::string_view filename);

}  // namespace fpgalsp::verible_bridge