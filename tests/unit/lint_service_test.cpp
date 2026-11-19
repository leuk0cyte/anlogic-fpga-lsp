// =============================================================================
//  tests/unit/lint_service_test.cpp —— T1 / task-50
// =============================================================================
//  锁定三组不变量：
//    1) 诊断**按来源合并**：lint 与 TD 互不覆盖（两种到达顺序都要一致）；
//    2) 注释里的 no-tabs 不发布（等价旧插件 buildCommentIndex/isInsideComment），
//       代码里的 tab 照报，字符串里的 "//" 不算注释；
//    3) LintService 的调度语义：去抖合并、同文件 single-flight、被取代的结果不发布、
//       Cancel / Shutdown 后不发布。
// =============================================================================

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "lsp/diagnostic.h"
#include "server/lint_service.h"
#include "server/lsp_server.h"
#include "verible_bridge/lint_text.h"

namespace {

using fpgalsp::lsp::Diagnostic;
using fpgalsp::lsp::DiagnosticList;

Diagnostic Make(int line, int character, std::string source, std::string code,
                std::string message) {
    Diagnostic diagnostic = Diagnostic::AtLine(line, character, character + 1, fpgalsp::lsp::kWarning,
                                               std::move(code), std::move(source),
                                               std::move(message));
    return diagnostic;
}

void SleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// -----------------------------------------------------------------------------
//  1) 按来源合并
// -----------------------------------------------------------------------------

TEST(MergeDiagnosticSources, LintAndTdCoexistOnSameLine) {
    const DiagnosticList lint = {Make(3, 0, "fpga-lsp:verible", "no-tabs", "Use spaces, not tabs"),
                                 Make(7, 4, "fpga-lsp:verible", "line-length", "too long")};
    const DiagnosticList td = {Make(3, 0, "fpga-lsp:td", "HDL-1234", "undefined module: foo")};

    const DiagnosticList merged = fpgalsp::server::MergeDiagnosticSources(lint, td);
    ASSERT_EQ(merged.size(), 3u) << "lint 与 TD 必须都保留（互不覆盖）";

    int lint_count = 0;
    int td_count = 0;
    for (const Diagnostic& diagnostic : merged) {
        if (diagnostic.source == "fpga-lsp:verible") ++lint_count;
        if (diagnostic.source == "fpga-lsp:td") ++td_count;
    }
    EXPECT_EQ(lint_count, 2);
    EXPECT_EQ(td_count, 1);

    // 同一位置：先 line/char，再 severity/source —— TD 与 lint 都在 line 3
    EXPECT_EQ(merged[0].range.start.line, 3);
    EXPECT_EQ(merged[1].range.start.line, 3);
    EXPECT_EQ(merged[2].range.start.line, 7);
}

TEST(MergeDiagnosticSources, OrderIndependent) {
    const DiagnosticList lint = {Make(1, 2, "fpga-lsp:verible", "no-tabs", "a"),
                                 Make(5, 0, "fpga-lsp:verible", "no-tabs", "b")};
    const DiagnosticList td = {Make(1, 0, "fpga-lsp:td", "HDL-1", "td-a"),
                               Make(9, 0, "fpga-lsp:td", "HDL-2", "td-b")};

    const DiagnosticList one = fpgalsp::server::MergeDiagnosticSources(lint, td);
    const DiagnosticList two = fpgalsp::server::MergeDiagnosticSources(td, lint);
    ASSERT_EQ(one.size(), two.size());
    for (std::size_t i = 0; i < one.size(); ++i) {
        EXPECT_EQ(one[i].source, two[i].source);
        EXPECT_EQ(one[i].range.start.line, two[i].range.start.line);
        EXPECT_EQ(one[i].range.start.character, two[i].range.start.character);
    }
}

TEST(MergeDiagnosticSources, DuplicatesWithinSameSourceCollapse) {
    const DiagnosticList lint = {Make(2, 0, "fpga-lsp:verible", "no-tabs", "same"),
                                 Make(2, 0, "fpga-lsp:verible", "no-tabs", "same")};
    const DiagnosticList merged = fpgalsp::server::MergeDiagnosticSources(lint, {});
    EXPECT_EQ(merged.size(), 1u);
}

// -----------------------------------------------------------------------------
//  2) 注释掩码：注释里的 no-tabs 不报，代码里的照报
// -----------------------------------------------------------------------------
//  6 个 tab：3 个在代码里（行 2/4/6），3 个在注释里（行 3、行 5 的块注释、行 7 的行注释）
const char* kTabsText =
    "module m;\n"
    "\tassign a = 1'b0;\n"              // 行 1: 代码 tab -> 报
    "  // \tcomment tab\n"              // 行 2: 行注释 tab -> 不报
    "\tassign b = 1'b1;\n"              // 行 3: 代码 tab -> 报
    "  /* block \tcomment\n"            // 行 4: 块注释开始（注释 tab） -> 不报
    "     still comment \t*/\n"         // 行 5: 块注释内 tab -> 不报
    "\tassign c = 1'b0;\n"              // 行 6: 代码 tab -> 报
    "endmodule\n";

TEST(CommentMask, DropsNoTabsInsideCommentsOnly) {
    // 1) 纯函数：给 6 个 no-tabs（3 代码 + 3 注释）-> 只留 3 个
    std::vector<fpgalsp::verible_bridge::LintDiagnostic> violations;
    const auto add = [&](int line, int column) {
        fpgalsp::verible_bridge::LintDiagnostic diagnostic;
        diagnostic.line = line;
        diagnostic.column = column;
        diagnostic.end_line = line;
        diagnostic.end_column = column + 1;
        diagnostic.rule = "no-tabs";
        diagnostic.message = "Use spaces, not tabs.";
        violations.push_back(std::move(diagnostic));
    };
    add(1, 0);   // 代码
    add(2, 5);   // 行注释里的 tab
    add(3, 0);   // 代码
    add(4, 10);  // 块注释第一行
    add(5, 17);  // 块注释第二行
    add(6, 0);   // 代码

    const auto kept =
        fpgalsp::verible_bridge::DropCommentNoTabs(kTabsText, std::move(violations));
    ASSERT_EQ(kept.size(), 3u) << "6 个 tab 里只有 3 个在代码中";
    EXPECT_EQ(kept[0].line, 1);
    EXPECT_EQ(kept[1].line, 3);
    EXPECT_EQ(kept[2].line, 6);
}

TEST(CommentMask, StringWithSlashesIsNotAComment) {
    // 字符串里的 "//" 不能让后面的 tab 被当成注释
    const std::string text =
        "module m;\n"
        "  initial $display(\"http://example\");\t\n"   // 行 1：字符串 + 代码 tab
        "  initial $display(\"a\\\"//b\");\t\n"        // 行 2：转义引号 + 代码 tab
        "endmodule\n";
    std::vector<fpgalsp::verible_bridge::LintDiagnostic> violations;
    for (int line : {1, 2}) {
        fpgalsp::verible_bridge::LintDiagnostic diagnostic;
        diagnostic.line = line;
        diagnostic.column = 34;  // 行尾 tab 附近（列值不精确也没关系：它不是 0 列）
        diagnostic.end_line = line;
        diagnostic.end_column = 35;
        diagnostic.rule = "no-tabs";
        violations.push_back(std::move(diagnostic));
    }
    // 只要位置**不在**注释里就保留：两行都应在代码区（掩码在该列不应命中）
    const auto kept = fpgalsp::verible_bridge::DropCommentNoTabs(text, std::move(violations));
    EXPECT_EQ(kept.size(), 2u) << "字符串内的 // 不应把后续代码判成注释";
}

TEST(CommentMask, VeribleNoTabsIsMaskedEndToEnd) {
    // 真跑一次内嵌 Verible：确认它确实产出 no-tabs，并且掩码后只剩代码里的 3 条
    const auto result = fpgalsp::verible_bridge::LintText(kTabsText, "tabs.v");
    ASSERT_TRUE(result.ok) << result.error;
    int total_no_tabs = 0;
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.rule == "no-tabs") ++total_no_tabs;
    }
    EXPECT_EQ(total_no_tabs, 6) << "Verible 应报 6 个 tab（3 代码 + 3 注释）";
    const auto kept = fpgalsp::verible_bridge::DropCommentNoTabs(kTabsText, result.diagnostics);
    int kept_no_tabs = 0;
    for (const auto& diagnostic : kept) {
        if (diagnostic.rule == "no-tabs") ++kept_no_tabs;
    }
    EXPECT_EQ(kept_no_tabs, 3) << "注释里的 3 个必须被丢弃";
}

// -----------------------------------------------------------------------------
//  3) LintService 调度语义
// -----------------------------------------------------------------------------

namespace {

struct Recorder {
    std::atomic<int> calls{0};
    std::atomic<int> published{0};
    std::atomic<int> last_published_count{0};
    std::atomic<int> run_ms{40};
};

}  // namespace

TEST(LintService, DebounceCoalescesAndSingleFlight) {
    Recorder recorder;
    std::atomic<int> concurrent{0};
    std::atomic<int> max_concurrent{0};

    fpgalsp::server::LintServiceOptions options;
    options.debounce_ms = 120;
    fpgalsp::server::LintService service(
        [&](const std::string&, const std::string&) {
            ++recorder.calls;
            const int now = concurrent.fetch_add(1) + 1;
            int previous = max_concurrent.load();
            while (now > previous && !max_concurrent.compare_exchange_weak(previous, now)) {
            }
            SleepMs(recorder.run_ms.load());
            --concurrent;
            return DiagnosticList{Make(0, 0, "fpga-lsp:verible", "no-tabs", "x")};
        },
        [&](const std::string&, const std::string&, DiagnosticList diagnostics) {
            ++recorder.published;
            recorder.last_published_count = static_cast<int>(diagnostics.size());
        },
        options);

    for (int i = 0; i < 5; ++i) {
        service.Request("a.v", "file:///a.v");
        SleepMs(20);
    }
    SleepMs(400);
    const auto stats = service.stats();
    service.Shutdown();

    EXPECT_EQ(stats.requests, 5u);
    EXPECT_EQ(stats.coalesced, 4u) << "5 次请求只保留最后一次";
    EXPECT_EQ(stats.runs, 1u) << "只应跑 1 轮";
    EXPECT_EQ(recorder.calls.load(), 1) << "runner 只被调用一次（去抖生效）";
    EXPECT_EQ(max_concurrent.load(), 1) << "同文件 single-flight";
    EXPECT_EQ(recorder.published.load(), 1);
    EXPECT_EQ(recorder.last_published_count.load(), 1);
}

TEST(LintService, NewRequestSupersedesRunningRound) {
    Recorder recorder;
    recorder.run_ms = 250;
    std::atomic<int> calls{0};
    fpgalsp::server::LintServiceOptions options;
    options.debounce_ms = 30;
    fpgalsp::server::LintService service(
        [&](const std::string&, const std::string&) {
            ++calls;
            SleepMs(recorder.run_ms.load());
            return DiagnosticList{Make(0, 0, "fpga-lsp:verible", "no-tabs", "x")};
        },
        [&](const std::string&, const std::string&, DiagnosticList) { ++recorder.published; },
        options);

    service.Request("a.v", "file:///a.v");
    SleepMs(120);  // 第一轮已经在跑（debounce 30 + 余量）
    service.Request("a.v", "file:///a.v");  // 应取代第一轮
    SleepMs(700);
    const auto stats = service.stats();
    service.Shutdown();

    EXPECT_GE(calls.load(), 2) << "第二轮应真正开跑";
    EXPECT_EQ(recorder.published.load(), 1) << "被取代的那轮**不发布**，只发布最新一轮";
    EXPECT_GE(stats.superseded, 1u);
}

TEST(LintService, CancelDropsPendingAndPublishesNothing) {
    std::atomic<int> calls{0};
    std::atomic<int> published{0};
    fpgalsp::server::LintServiceOptions options;
    options.debounce_ms = 120;
    fpgalsp::server::LintService service(
        [&](const std::string&, const std::string&) {
            ++calls;
            return DiagnosticList{Make(0, 0, "fpga-lsp:verible", "no-tabs", "x")};
        },
        [&](const std::string&, const std::string&, DiagnosticList) { ++published; }, options);

    service.Request("a.v", "file:///a.v");
    SleepMs(20);
    service.Cancel("a.v");  // didClose
    SleepMs(300);
    service.Shutdown();

    EXPECT_EQ(calls.load(), 0) << "取消发生在去抖窗口内 -> runner 根本不该跑";
    EXPECT_EQ(published.load(), 0);
}

TEST(LintService, ShutdownJoinsAndDoesNotPublish) {
    std::atomic<int> published{0};
    std::atomic<int> started{0};
    fpgalsp::server::LintServiceOptions options;
    options.debounce_ms = 20;
    fpgalsp::server::LintService service(
        [&](const std::string&, const std::string&) {
            ++started;
            SleepMs(300);  // 模拟一次较慢的 Verible
            return DiagnosticList{Make(0, 0, "fpga-lsp:verible", "no-tabs", "x")};
        },
        [&](const std::string&, const std::string&, DiagnosticList) { ++published; }, options);

    service.Request("a.v", "file:///a.v");
    for (int i = 0; i < 100 && started.load() == 0; ++i) SleepMs(10);
    ASSERT_EQ(started.load(), 1) << "runner 应该已经开跑";
    service.Shutdown();  // 必须 join 且返回后不再发布
    SleepMs(100);
    EXPECT_EQ(published.load(), 0) << "退出前不发布";
}

}  // namespace