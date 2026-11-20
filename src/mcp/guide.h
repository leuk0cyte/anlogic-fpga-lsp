// =============================================================================
//  src/mcp/guide.h —— fpga_guide 工具的正文（纯文本，无工程/硬件依赖）
// =============================================================================
//  为什么单独成文件：
//    * `instructions` 只有 2000 字符预算，装不下"每个主题怎么串"的细节；
//    * 把这些字放进 tools.cpp 会让 schema 表（逐字对齐参考实现的部分）混进大段
//      散文，agent 读 schema 时也会被正文淹没。
//  所以：schema 只留一句话，正文由 fpga_guide 按 topic 现取。
//
//  ---- 约束 ----
//  1. topic 集合固定（见 kGuideTopics），与 tools.cpp 里 fpga_guide 的 enum 严格
//     一致；加/删 topic 必须两处同时改，否则 agent 会按 enum 传进来却拿到别的正文。
//  2. 每个 topic ≤ 3 KB（Markdown）。超了说明写成了散文，应该拆成"工具调用序列"。
//  3. 正文面向"没有上下文的 agent"：一定要有工具名、参数、前置条件、失败判据，
//     不要出现"如上所述""前面说过"这类跨段引用。
// =============================================================================

#pragma once

#include <string>
#include <string_view>

namespace fpgalsp::mcp {

// 归一化 topic：未知/空一律回落到 "overview"。
// 返回的 string_view 保证指向静态存储（可直接回填到响应里）。
std::string_view ResolveGuideTopic(std::string_view topic);

// 取该 topic 的 Markdown 正文（未知 topic 返回 overview 正文）。
std::string GuideText(std::string_view topic);

}  // namespace fpgalsp::mcp