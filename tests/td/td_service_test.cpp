// =============================================================================
//  tests/td/td_service_test.cpp —— A5：单飞队列 / 取消 / 去抖 / 分段统计
// =============================================================================
//  这些用例**不需要真实 TD**：用假 runner 把"策略"与"TD 进程"解耦，
//  因此可以标 LABEL unit，在默认 ctest 里跑（毫秒级）。
//  真实 TD 的分段数据在 tests/td/td_bench.cpp（LABEL bench）与
//  td_diagnostics_integration_test.cpp 里。
// =============================================================================

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "lsp/diagnostic.h"
#include "project/al_project.h"
#include "td/td_cancel.h"
#include "td/td_diagnostics.h"
#include "td/td_service.h"

namespace {

using namespace fpgalsp;
using namespace fpgalsp::td;
using namespace std::chrono_literals;

project::Project MakeProject(const std::string& name) {
    project::Project p;
    p.dir = "Y:/proj";
    p.al_path = "Y:/proj/" + name + ".al";
    p.name = name;
    p.top = "top";
    p.device = "EF2L45LG144B";
    p.family = "EF2";
    return p;
}

lsp::Diagnostic MakeDiag(int line, const std::string& code, const std::string& message) {
    return lsp::Diagnostic::AtLine(line, 0, 1, lsp::kError, code, "fpga-lsp:td", message);
}

// 轮询等待（避免 sleep 竞态）；超时返回 false
template <typename Predicate>
bool WaitFor(Predicate predicate, int timeout_ms = 3000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return predicate();
}

// -----------------------------------------------------------------------------
// 假 runner：记录并发度，按用例需要"阻塞直到被取消"
// -----------------------------------------------------------------------------
class FakeRunner : public TdCheckRunner {
public:
    enum class Mode {
        kInstant,    // 立刻成功返回
        kBlockUntilCancelled,  // 一直转圈直到 cancel->cancelled()（模拟 read_hdl 阻塞点）
        kWaitGate,   // 一直转圈直到 gate 打开（模拟"这一轮自己会跑完"）
    };

    struct Shared {
        std::atomic<int> concurrent{0};
        std::atomic<int> max_concurrent{0};
        std::atomic<int> started{0};
        std::atomic<int> finished{0};
        std::atomic<bool> gate{false};
        std::mutex mutex;
        std::vector<std::string> project_names;  // 开跑的 project 顺序
        std::vector<std::string> sink_names;     // sink 收到的 project 顺序
        std::atomic<int> cancelled{0};
    };

    FakeRunner(Mode mode, Shared* shared) : mode_(mode), shared_(shared) {}

    TdCheckResult Run(const project::Project& project, const CancelToken& cancel) override {
        const int now = shared_->concurrent.fetch_add(1) + 1;
        int observed = shared_->max_concurrent.load();
        while (now > observed && !shared_->max_concurrent.compare_exchange_weak(observed, now)) {
        }
        {
            std::lock_guard<std::mutex> lock(shared_->mutex);
            shared_->project_names.push_back(project.name);
        }
        shared_->started.fetch_add(1);

        TdCheckResult result;
        if (mode_ != Mode::kInstant) {
            const auto deadline = std::chrono::steady_clock::now() + 10s;
            while (!cancel.cancelled() && std::chrono::steady_clock::now() < deadline) {
                if (mode_ == Mode::kWaitGate && shared_->gate.load()) break;
                std::this_thread::sleep_for(2ms);
            }
            if (cancel.cancelled()) {
                result.cancelled = true;
                shared_->cancelled.fetch_add(1);
                shared_->concurrent.fetch_sub(1);
                shared_->finished.fetch_add(1);
                return result;
            }
        }
        // 模拟"一轮成功的检查"：一条诊断 + 分段数字
        lsp::Diagnostic diag = MakeDiag(3, "HDL-7144", "fake problem for " + project.name);
        diag.WithData("path", lsp::Json(project.al_path));
        result.all.push_back(diag);
        result.by_file[project.al_path].push_back(diag);
        result.timings.startup_ms = 1.0;
        result.timings.import_ms = 2.0;
        result.timings.read_hdl_ms = 3.0;
        result.timings.parse_ms = 4.0;
        result.timings.total_ms = 10.0;
        shared_->concurrent.fetch_sub(1);
        shared_->finished.fetch_add(1);
        return result;
    }

private:
    Mode mode_;
    Shared* shared_;
};

// -----------------------------------------------------------------------------
// 去抖
// -----------------------------------------------------------------------------

TEST(TdServiceDebounce, BurstOfFiveSavesRunsOnce) {
    FakeRunner::Shared shared;
    const auto started = std::chrono::steady_clock::now();
    TdService service(
        [&shared] {
            return std::make_unique<FakeRunner>(FakeRunner::Mode::kInstant, &shared);
        },
        [&shared](const project::Project& project, TdCheckResult result) {
            std::lock_guard<std::mutex> lock(shared.mutex);
            shared.sink_names.push_back(project.name);
            (void)result;
        },
        TdServiceOptions{/*debounce_ms=*/120});

    for (int i = 0; i < 5; ++i) {
        service.Request(MakeProject("p" + std::to_string(i)));
        std::this_thread::sleep_for(10ms);  // 人手连续保存的间隔远大于 10ms
    }
    ASSERT_TRUE(WaitFor([&shared] { return shared.finished.load() >= 1; }));
    // 再来一次，确认不是"永远只跑一次"
    service.Request(MakeProject("last"));
    ASSERT_TRUE(WaitFor([&shared] { return shared.finished.load() >= 2; }));

    const auto elapsed_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    service.Shutdown();

    const auto stats = service.stats();
    EXPECT_EQ(stats.requests, 6u);
    EXPECT_EQ(stats.runs, 2u) << "5 次连发只应合并成 1 轮";
    EXPECT_EQ(stats.cancelled, 0u);
    EXPECT_GE(stats.coalesced, 4u);
    // 只应看到"最后一次"的 project 名（p4 + last）
    std::lock_guard<std::mutex> lock(shared.mutex);
    EXPECT_EQ(shared.sink_names.size(), 2u);
    if (shared.sink_names.size() == 2) {
        EXPECT_EQ(shared.sink_names[0], "p4");
        EXPECT_EQ(shared.sink_names[1], "last");
    }
    // 去抖窗口确实生效（两轮之间至少有一个 120ms 的静默期）
    EXPECT_GE(elapsed_ms, 200.0);
}

TEST(TdServiceDebounce, ZeroDebounceStartsImmediately) {
    FakeRunner::Shared shared;
    TdService service(
        [&shared] { return std::make_unique<FakeRunner>(FakeRunner::Mode::kInstant, &shared); },
        [](const project::Project&, TdCheckResult) {},
        TdServiceOptions{/*debounce_ms=*/0});
    service.Request(MakeProject("now"));
    EXPECT_TRUE(WaitFor([&shared] { return shared.finished.load() >= 1; }, 2000));
    service.Shutdown();
    EXPECT_EQ(service.stats().runs, 1u);
}

// -----------------------------------------------------------------------------
// 取消（正在跑的一轮让路）
// -----------------------------------------------------------------------------

TEST(TdServiceCancel, NewSaveCancelsRunningRoundAndRunsOnlyLatest) {
    FakeRunner::Shared shared;
    std::atomic<bool> first_started{false};
    TdService service(
        [&shared, &first_started] {
            // 第一轮阻塞，之后立刻返回，便于断言"取消后只跑最新那轮"
            const bool first = !first_started.load();
            if (first) {
                first_started.store(true);
                return std::make_unique<FakeRunner>(FakeRunner::Mode::kBlockUntilCancelled,
                                                    &shared);
            }
            return std::make_unique<FakeRunner>(FakeRunner::Mode::kInstant, &shared);
        },
        [&shared](const project::Project& project, TdCheckResult result) {
            std::lock_guard<std::mutex> lock(shared.mutex);
            shared.sink_names.push_back(project.name);
            (void)result;
        },
        TdServiceOptions{/*debounce_ms=*/0});

    service.Request(MakeProject("first"));
    ASSERT_TRUE(WaitFor([&shared] { return shared.started.load() >= 1; }))
        << "第一轮应该已经开始（阻塞中）";

    // 第二轮请求：应取消第一轮（kill hook 由真实 runner 注册；这里靠 cancelled() 协作）
    service.Request(MakeProject("second"));
    ASSERT_TRUE(WaitFor([&shared] { return shared.finished.load() >= 2; }, 5000));

    service.Shutdown();
    const auto stats = service.stats();
    EXPECT_EQ(stats.cancelled, 1u) << "在跑的一轮必须被取消，而不是等它跑完";
    EXPECT_EQ(stats.runs, 1u);
    EXPECT_EQ(shared.max_concurrent.load(), 1) << "必须单飞";

    std::lock_guard<std::mutex> lock(shared.mutex);
    ASSERT_EQ(shared.sink_names.size(), 1u);
    EXPECT_EQ(shared.sink_names[0], "second") << "只应发布最新一轮的结果";
}

TEST(TdServiceCancel, QueueModeDropsSupersededResultInsteadOfPublishingIt) {
    // cancel_running=false：在跑的一轮**不杀**，但它跑完时已经有更新的请求 ->
    // 结果视为过期、直接丢弃（旧策略会把过期结果也发出去，Problems 会闪一下）。
    FakeRunner::Shared shared;
    std::atomic<bool> first_started{false};
    TdService service(
        [&shared, &first_started] {
            const bool first = !first_started.load();
            if (first) {
                first_started.store(true);
                return std::make_unique<FakeRunner>(FakeRunner::Mode::kWaitGate, &shared);
            }
            return std::make_unique<FakeRunner>(FakeRunner::Mode::kInstant, &shared);
        },
        [&shared](const project::Project& project, TdCheckResult) {
            std::lock_guard<std::mutex> lock(shared.mutex);
            shared.sink_names.push_back(project.name);
        },
        TdServiceOptions{/*debounce_ms=*/0, /*debounce_after_cancel_ms=*/0,
                         /*cancel_running=*/false});

    service.Request(MakeProject("old"));
    ASSERT_TRUE(WaitFor([&shared] { return shared.started.load() >= 1; }));
    // 第二个请求到达：不取消，等第一轮自己结束
    service.Request(MakeProject("new"));
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(shared.finished.load(), 0) << "cancel_running=false 时不应打断在跑的一轮";
    EXPECT_EQ(service.stats().cancelled, 0u);

    shared.gate.store(true);  // 第一轮"自己跑完"
    ASSERT_TRUE(WaitFor([&shared] { return shared.finished.load() >= 2; }, 5000));
    service.Shutdown();

    const auto stats = service.stats();
    EXPECT_EQ(stats.cancelled, 0u);
    EXPECT_EQ(stats.superseded, 1u) << "过期的一轮应被标记 superseded";
    EXPECT_EQ(stats.runs, 1u);
    std::lock_guard<std::mutex> lock(shared.mutex);
    ASSERT_EQ(shared.sink_names.size(), 1u);
    EXPECT_EQ(shared.sink_names[0], "new") << "只发布最新一轮";
}

TEST(TdServiceCancel, CancelActiveDropsEverything) {
    FakeRunner::Shared shared;
    TdService service(
        [&shared] {
            return std::make_unique<FakeRunner>(FakeRunner::Mode::kBlockUntilCancelled, &shared);
        },
        [&shared](const project::Project& project, TdCheckResult) {
            std::lock_guard<std::mutex> lock(shared.mutex);
            shared.sink_names.push_back(project.name);
        },
        TdServiceOptions{/*debounce_ms=*/0});

    service.Request(MakeProject("doomed"));
    ASSERT_TRUE(WaitFor([&shared] { return shared.started.load() >= 1; }));
    service.CancelActive();
    EXPECT_TRUE(WaitFor([&shared] { return shared.cancelled.load() >= 1; }));
    service.Shutdown();

    std::lock_guard<std::mutex> lock(shared.mutex);
    EXPECT_TRUE(shared.sink_names.empty()) << "取消的轮次绝不能发布（半截结果会误报 0 条诊断）";
    EXPECT_EQ(service.stats().runs, 0u);
}

TEST(TdServiceCancel, ShutdownWhileRunningIsClean) {
    FakeRunner::Shared shared;
    {
        TdService service(
            [&shared] {
                return std::make_unique<FakeRunner>(FakeRunner::Mode::kBlockUntilCancelled,
                                                    &shared);
            },
            [](const project::Project&, TdCheckResult) {},
            TdServiceOptions{/*debounce_ms=*/0});
        service.Request(MakeProject("x"));
        ASSERT_TRUE(WaitFor([&shared] { return shared.started.load() >= 1; }));
        service.Shutdown();  // 析构前显式关：应取消在跑的一轮并 join
        EXPECT_EQ(service.stats().runs, 0u);
        EXPECT_GE(service.stats().cancelled, 1u);
    }
    // 走到这里说明 worker 线程已被 join（没有 terminate / 悬挂）
    SUCCEED();
}

// -----------------------------------------------------------------------------
// 阶段统计的透传
// -----------------------------------------------------------------------------

TEST(TdServiceStats, ForwardsPhaseTimings) {
    FakeRunner::Shared shared;
    TdPhaseTimings seen;
    TdService service(
        [&shared] { return std::make_unique<FakeRunner>(FakeRunner::Mode::kInstant, &shared); },
        [&seen](const project::Project&, TdCheckResult result) { seen = result.timings; },
        TdServiceOptions{/*debounce_ms=*/0});
    service.Request(MakeProject("t"));
    ASSERT_TRUE(WaitFor([&shared] { return shared.finished.load() >= 1; }));
    service.Shutdown();

    EXPECT_DOUBLE_EQ(seen.startup_ms, 1.0);
    EXPECT_DOUBLE_EQ(seen.import_ms, 2.0);
    EXPECT_DOUBLE_EQ(seen.read_hdl_ms, 3.0);
    EXPECT_DOUBLE_EQ(seen.parse_ms, 4.0);
    EXPECT_DOUBLE_EQ(seen.accounted_ms(), 10.0);
    EXPECT_DOUBLE_EQ(seen.other_ms(), 0.0);
    EXPECT_DOUBLE_EQ(service.stats().last_total_ms, 10.0);
}

// =============================================================================
//  按文件切分 / 只发布变更（DiffChangedFiles）
// =============================================================================

TEST(TdDiagnosticsSplit, GroupsByDataPathThenFallsBackToAlPath) {
    lsp::Diagnostic with_path = MakeDiag(1, "A", "m");
    with_path.WithData("path", lsp::Json("Y:/proj/a.v"));
    lsp::Diagnostic no_path = MakeDiag(2, "B", "m");

    EXPECT_EQ(DiagnosticFileKey(with_path, "Y:/proj/demo.al"), "Y:/proj/a.v");
    EXPECT_EQ(DiagnosticFileKey(no_path, "Y:/proj/demo.al"), "Y:/proj/demo.al");
    // 反斜杠要被归一
    lsp::Diagnostic win = MakeDiag(3, "C", "m");
    win.WithData("path", lsp::Json("Y:\\proj\\b.v"));
    EXPECT_EQ(DiagnosticFileKey(win, "Y:/proj/demo.al"), "Y:/proj/b.v");
}

TEST(TdDiagnosticsSplit, DiffDetectsAddedRemovedAndModified) {
    std::map<std::string, lsp::DiagnosticList> before;
    std::map<std::string, lsp::DiagnosticList> after;

    before["Y:/p/a.v"] = {MakeDiag(1, "X", "same")};
    after["Y:/p/a.v"] = {MakeDiag(1, "X", "same")};  // 不变

    before["Y:/p/gone.v"] = {MakeDiag(2, "Y", "bye")};  // 消失
    after["Y:/p/new.v"] = {MakeDiag(3, "Z", "hi")};     // 新增

    before["Y:/p/mod.v"] = {MakeDiag(4, "W", "old message")};
    after["Y:/p/mod.v"] = {MakeDiag(4, "W", "new message")};  // 内容变化

    const std::vector<std::string> changed = DiffChangedFiles(before, after);
    ASSERT_EQ(changed.size(), 3u);
    EXPECT_EQ(changed[0], "Y:/p/gone.v");
    EXPECT_EQ(changed[1], "Y:/p/mod.v");
    EXPECT_EQ(changed[2], "Y:/p/new.v");
}

TEST(TdDiagnosticsSplit, DiffIgnoresOrderingAndDecoration) {
    std::map<std::string, lsp::DiagnosticList> before;
    std::map<std::string, lsp::DiagnosticList> after;
    // 同一文件两条诊断，只是顺序不同 -> 不算变化
    before["Y:/p/a.v"] = {MakeDiag(1, "X", "m1"), MakeDiag(5, "Y", "m2")};
    after["Y:/p/a.v"] = {MakeDiag(5, "Y", "m2"), MakeDiag(1, "X", "m1")};
    EXPECT_TRUE(DiffChangedFiles(before, after).empty());
}

}  // namespace