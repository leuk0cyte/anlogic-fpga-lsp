// =============================================================================
//  tests/unit/verible_style_test.cpp —— formatter / lint 的 style flag 解析
// =============================================================================
//  回归背景（0.15.0 的真 bug）：
//    旧 Python 插件把排版风格写在 package.json 的 `fpgaLsp.verible.args` 默认值里
//    （160 列 / **4 空格缩进** / 一批 alignment=align）。C++ 重写时服务端把
//    indentation_spaces 写死成 Verible 默认的 2，并且**从不读取**客户端发来的
//    `veribleArgs` / `veribleLintArgs`，结果"格式化一次整份工程从 4 空格变 2 空格"。
//
//  本文件盯三件事：
//    1) 默认 style 必须与旧插件默认值一致（缩进 4）；
//    2) 客户端给的 flag 必须真的生效（含两段式 "--flag value"）；
//    3) 认不出/取值非法的 flag 行为要明确（忽略 vs 失败）。
// =============================================================================

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "server/lsp_server.h"
#include "verible_bridge/format_text.h"
#include "verible_bridge/lint_text.h"

namespace {

using fpgalsp::verible_bridge::FormatOptions;
using fpgalsp::verible_bridge::FormatResult;
using fpgalsp::verible_bridge::FormatText;
using fpgalsp::verible_bridge::LintResult;
using fpgalsp::verible_bridge::LintText;

bool Has(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

int CountLines(const std::string& text) {
    return static_cast<int>(std::count(text.begin(), text.end(), '\n'));
}

// 需要被重新缩进的嵌套块（输入故意不缩进，formatter 一定会动它）
const char* kNested =
    "module m;\n"
    "always @(posedge clk) begin\n"
    "if (a) begin\n"
    "b <= c;\n"
    "end\n"
    "end\n"
    "endmodule\n";

// 120 列 / 200 列的行（line-length 阈值两侧）
std::string LongLine(int payload) {
    return "module m;\n  wire " + std::string(static_cast<std::size_t>(payload), 'a') +
           ";\nendmodule\n";
}

std::vector<std::string> Flags(std::initializer_list<const char*> flags) {
    std::vector<std::string> out;
    for (const char* flag : flags) out.emplace_back(flag);
    return out;
}

}  // namespace

// -----------------------------------------------------------------------------
// 1. 默认值 = 旧插件 package.json 的默认值
// -----------------------------------------------------------------------------
TEST(VeribleStyle, DefaultFlagsMatchOldPluginConfig) {
    const std::vector<std::string> flags = fpgalsp::verible_bridge::DefaultStyleFlags();
    ASSERT_FALSE(flags.empty());
    const auto has = [&flags](const std::string& expected) {
        return std::find(flags.begin(), flags.end(), expected) != flags.end();
    };
    // 这三条是"排版风格"的核心：列宽、缩进、长行折断
    EXPECT_TRUE(has("--column_limit=160")) << "列宽必须与旧插件一致（160）";
    EXPECT_TRUE(has("--indentation_spaces=4")) << "缩进必须与旧插件一致（4 空格）";
    EXPECT_TRUE(has("--try_wrap_long_lines=true"));
    // 对齐组：没有它们，连续赋值/端口声明不会被对齐
    EXPECT_TRUE(has("--assignment_statement_alignment=align"));
    EXPECT_TRUE(has("--port_declarations_alignment=align"));
    EXPECT_TRUE(has("--alignment_group_boundary=blank-lines-and-separator-comments"));
}

TEST(VeribleStyle, DefaultIndentationIsFourSpaces) {
    const FormatResult formatted = FormatText(kNested, "m.v");
    ASSERT_TRUE(formatted.ok) << formatted.error;
    EXPECT_TRUE(Has(formatted.text, "\n    always @(posedge clk) begin"))
        << "模块体内缩进应为 4 空格，实际：\n"
        << formatted.text;
    EXPECT_TRUE(Has(formatted.text, "\n        if (a) begin"))
        << "第二层缩进应为 8 空格，实际：\n"
        << formatted.text;
}

TEST(VeribleStyle, DefaultLintFlagsKeepLineLengthAt160) {
    const std::vector<std::string> flags = fpgalsp::verible_bridge::DefaultLintFlags();
    ASSERT_FALSE(flags.empty());
    EXPECT_NE(std::find(flags.begin(), flags.end(), "--rules=+line-length=length:160"),
              flags.end());
}

// -----------------------------------------------------------------------------
// 2. 客户端 flag 必须生效
// -----------------------------------------------------------------------------
TEST(VeribleStyle, ExplicitIndentationOverridesDefault) {
    FormatOptions options;
    options.style_flags = Flags({"--indentation_spaces=2"});
    const FormatResult formatted = FormatText(kNested, "m.v", options);
    ASSERT_TRUE(formatted.ok) << formatted.error;
    EXPECT_TRUE(Has(formatted.text, "\n  always @(posedge clk) begin")) << formatted.text;
    EXPECT_TRUE(Has(formatted.text, "\n    if (a) begin")) << formatted.text;
}

TEST(VeribleStyle, TwoTokenFlagFormIsAccepted) {
    // absl CLI 也接受 "--flag value"；旧插件的 args 是数组，用户很容易这么写
    FormatOptions options;
    options.style_flags = {"--indentation_spaces", "2"};
    const FormatResult formatted = FormatText(kNested, "m.v", options);
    ASSERT_TRUE(formatted.ok) << formatted.error;
    EXPECT_TRUE(Has(formatted.text, "\n  always @(posedge clk) begin")) << formatted.text;
}

TEST(VeribleStyle, ColumnLimitIsHonored) {
    const std::string text = LongLine(120);  // 122 列
    const FormatResult wide = FormatText(text, "m.v");  // 默认 160：一行放得下
    ASSERT_TRUE(wide.ok) << wide.error;

    FormatOptions narrow_options;
    narrow_options.style_flags = Flags({"--column_limit=80", "--try_wrap_long_lines=true"});
    const FormatResult narrow = FormatText(text, "m.v", narrow_options);
    ASSERT_TRUE(narrow.ok) << narrow.error;

    EXPECT_LT(CountLines(wide.text), CountLines(narrow.text))
        << "收紧列宽必须把长行折断：\n"
        << narrow.text;
}

// -----------------------------------------------------------------------------
// 3. 认不出 / 取值非法的 flag
// -----------------------------------------------------------------------------
TEST(VeribleStyle, UnknownFlagIsReportedButHarmless) {
    FormatOptions options;
    options.style_flags = Flags({"--indentation_spaces=4", "--brand_new_flag=42"});
    const FormatResult formatted = FormatText(kNested, "m.v", options);
    ASSERT_TRUE(formatted.ok) << "未知 flag 不应该让格式化整体失败";
    ASSERT_EQ(formatted.unknown_flags.size(), 1u);
    EXPECT_EQ(formatted.unknown_flags[0], "--brand_new_flag");
    // 认得的那个仍然生效
    EXPECT_TRUE(Has(formatted.text, "\n    always @(posedge clk) begin")) << formatted.text;
}

TEST(VeribleStyle, InvalidValueFailsInsteadOfSilentlyUsingWrongStyle) {
    FormatOptions options;
    options.style_flags = Flags({"--column_limit=abc"});
    const FormatResult formatted = FormatText(kNested, "m.v", options);
    EXPECT_FALSE(formatted.ok) << "取值非法必须失败，不能猜一个数就用";
    EXPECT_NE(formatted.error.find("column_limit"), std::string::npos) << formatted.error;
    EXPECT_EQ(formatted.text, std::string(kNested)) << "失败时返回原文";
}

// -----------------------------------------------------------------------------
// 4. lint 侧的 flag
// -----------------------------------------------------------------------------
TEST(VeribleLintFlags, DefaultThresholdIs160) {
    const LintResult at_120 = LintText(LongLine(106), "line_length.v");  // 约 120 列
    ASSERT_TRUE(at_120.ok) << at_120.error;
    for (const auto& diagnostic : at_120.diagnostics) {
        EXPECT_NE(diagnostic.rule, "line-length") << "120 列在 160 阈值下不该报";
    }

    const LintResult at_200 = LintText(LongLine(186), "line_length.v");  // 约 200 列
    ASSERT_TRUE(at_200.ok) << at_200.error;
    int long_lines = 0;
    for (const auto& diagnostic : at_200.diagnostics) {
        if (diagnostic.rule == "line-length") ++long_lines;
    }
    EXPECT_GE(long_lines, 1);
}

TEST(VeribleLintFlags, RulesFlagCanDisableARule) {
    const LintResult disabled =
        LintText(LongLine(186), "line_length.v", Flags({"--rules=-line-length"}));
    ASSERT_TRUE(disabled.ok) << disabled.error;
    for (const auto& diagnostic : disabled.diagnostics) {
        EXPECT_NE(diagnostic.rule, "line-length") << "--rules=-line-length 必须真的关掉它";
    }
}

TEST(VeribleLintFlags, RuleSetNoneProducesNoDiagnostics) {
    const LintResult result =
        LintText(LongLine(186), "line_length.v", Flags({"--ruleset=none"}));
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.diagnostics.empty()) << "--ruleset=none 应该一条都不报";
}

TEST(VeribleLintFlags, UnknownFlagIsReportedButHarmless) {
    const LintResult result =
        LintText(LongLine(186), "line_length.v", Flags({"--brand_new_flag=1"}));
    ASSERT_TRUE(result.ok) << "未知 lint flag 不应该让 lint 整体失败";
    ASSERT_EQ(result.unknown_flags.size(), 1u);
    EXPECT_EQ(result.unknown_flags[0], "--brand_new_flag");
}

TEST(VeribleLintFlags, InvalidRuleNameFailsLoudly) {
    const LintResult result =
        LintText(LongLine(186), "line_length.v", Flags({"--rules=+no-such-rule"}));
    EXPECT_FALSE(result.ok) << "拼错规则名必须报错，而不是当作'没有告警'";
    EXPECT_FALSE(result.error.empty());
}

// -----------------------------------------------------------------------------
// 5. Settings：两种写法都要收（旧插件默认是字符串，服务端只认数组 -> 曾经被丢弃）
// -----------------------------------------------------------------------------
TEST(VeribleSettings, LintArgsAcceptArrayAndLegacyString) {
    using fpgalsp::server::Settings;

    Settings from_array;
    from_array.Update(nlohmann::json{
        {"fpgaLsp", {{"veribleLintArgs", {"--rules=+line-length=length:120"}}}}});
    ASSERT_EQ(from_array.verible_lint_args.size(), 1u);
    EXPECT_EQ(from_array.verible_lint_args[0], "--rules=+line-length=length:120");

    Settings from_string;  // 旧插件 package.json 的默认形状
    from_string.Update(nlohmann::json{{"fpgaLsp",
                                       {{"veribleLintArgs", "--rules=+line-length=length:140"}}}});
    ASSERT_EQ(from_string.verible_lint_args.size(), 1u);
    EXPECT_EQ(from_string.verible_lint_args[0], "--rules=+line-length=length:140");

    Settings from_nothing;
    from_nothing.Update(nlohmann::json{{"fpgaLsp", {{"tdCheckOnSave", false}}}});
    EXPECT_TRUE(from_nothing.verible_lint_args.empty())
        << "客户端没发 = 空，由桥回落 DefaultLintFlags()";
}

TEST(VeribleSettings, StyleArgsAreForwarded) {
    using fpgalsp::server::Settings;

    Settings settings;
    settings.Update(nlohmann::json{
        {"fpgaLsp", {{"veribleArgs", {"--indentation_spaces=4", "--column_limit=160"}}}}});
    ASSERT_EQ(settings.verible_args.size(), 2u);
    EXPECT_EQ(settings.verible_args[0], "--indentation_spaces=4");

    // 真的走一遍：设置里的 flag 必须改变格式化结果
    FormatOptions options;
    options.style_flags = settings.verible_args;
    const FormatResult formatted = FormatText(kNested, "m.v", options);
    ASSERT_TRUE(formatted.ok) << formatted.error;
    EXPECT_TRUE(Has(formatted.text, "\n    always @(posedge clk) begin")) << formatted.text;
}