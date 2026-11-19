// =============================================================================
//  tests/perf/probe_td.cpp —— TD / lint / 诊断调度的进程内成本
// =============================================================================
//  回答三个问题（对应 task-32）：
//    1) TD 探测（FindTd 扫盘）与 TD 进程启动/握手各多少 ms —— 与"我们的代码"分开；
//    2) 内嵌 Verible lint（AutofixText）单文件成本：13 KB / 100 KB，冷/热；
//    3) TdService 的 debounce / 单飞 / cancel 是否真的生效（用假 runner，不碰 TD）。
//
//  用法：
//    build\bin\perf_td.exe --iters 20                 # 只跑不需要 TD 的部分（默认）
//    build\bin\perf_td.exe --iters 20 --td           # 额外测真实 TD 会话启动/握手
//    build\bin\perf_td.exe --check-al <x.al>         # 额外跑一轮真实 read_hdl（%TEMP% 合成工程）
//
//  ⚠ 安全：真实 TD 只在 --td/--check-al 且工程位于 %TEMP% 时使用；work_dir 一律指到
//     %TEMP%\fpgalsp-perf\td\work（use_project_work_dir=false），不碰任何真实工程。
// =============================================================================
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "perf_common.h"

#include "platform/encoding.h"
#include "project/al_project.h"
#include "td/td_cancel.h"
#include "td/td_diagnostics.h"
#include "td/td_install.h"
#include "td/td_service.h"
#include "td/td_session.h"
#include "verible_bridge/lint_text.h"

namespace fs = std::filesystem;

namespace {

// -----------------------------------------------------------------------------
//  本地小工具
// -----------------------------------------------------------------------------

// 本进程 CPU 时间（user+kernel）。用来把"我们烧的 CPU"与"等外部进程的墙钟"分开。
double ProcessCpuMs() {
#if defined(_WIN32)
    FILETIME creation{}, exit_time{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit_time, &kernel, &user)) return 0.0;
    auto to_ms = [](const FILETIME& t) {
        const unsigned long long ticks =
            (static_cast<unsigned long long>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
        return static_cast<double>(ticks) / 10000.0;  // 100 ns -> ms
    };
    return to_ms(kernel) + to_ms(user);
#else
    return 0.0;
#endif
}

std::string MakeTempDir(std::string_view name) {
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec) / "fpgalsp-perf" / "td" / std::string(name);
    fs::create_directories(base, ec);
    std::string text = base.string();
    std::replace(text.begin(), text.end(), '\\', '/');
    return text;
}

// 生成"看起来像真实工程"的 Verilog：每 ~1.4 KB 一个模块。
// 故意混入一些会触发 lint 的写法（行尾空格、非首选关键字），让规则真的跑起来。
std::string MakeVerilog(std::size_t target_bytes) {
    std::string out;
    out.reserve(target_bytes + 2048);
    int index = 0;
    while (out.size() < target_bytes) {
        out += "module gen_" + std::to_string(index) + " (input wire clk, input wire [7:0] a, "
               "output reg [7:0] q);\n";
        for (int j = 0; j < 8; ++j) {
            out += "  wire [7:0] w_" + std::to_string(j) + ";\n";
            out += "  assign w_" + std::to_string(j) + " = a + 8'd" + std::to_string(j) + ";\n";
        }
        out += "  always @(posedge clk) begin\n";
        out += "    q <= w_0;\n";
        out += "  end\n";
        out += "endmodule\n\n";
        ++index;
    }
    return out;
}

// -----------------------------------------------------------------------------
//  假 runner：模拟"外部进程很慢"的那一段，但完全在进程内（毫秒级单测开销）
// -----------------------------------------------------------------------------
class FakeSlowRunner : public fpgalsp::td::TdCheckRunner {
public:
    struct Shared {
        std::atomic<int> concurrent{0};
        std::atomic<int> max_concurrent{0};
        std::atomic<int> starts{0};
        std::atomic<int> observed_cancel{0};
        std::atomic<double> last_cancel_latency_ms{0.0};
        std::atomic<double> max_cancel_latency_ms{0.0};
    };

    FakeSlowRunner(int work_ms, Shared* shared) : work_ms_(work_ms), shared_(shared) {}

    fpgalsp::td::TdCheckResult Run(const fpgalsp::project::Project& project,
                                   const fpgalsp::td::CancelToken& cancel) override {
        using clock = std::chrono::steady_clock;
        (void)project;

        ++shared_->starts;
        const int now = shared_->concurrent.fetch_add(1) + 1;
        int previous = shared_->max_concurrent.load();
        while (now > previous && !shared_->max_concurrent.compare_exchange_weak(previous, now)) {
        }

        const auto started_at = clock::now();
        // 模拟"阻塞在外部进程里"：kill hook 一到就立刻退出循环
        std::atomic<bool> hook_fired{false};
        std::atomic<double> hook_at_ms{0.0};
        cancel.SetKillHook([this, &hook_fired, &hook_at_ms, started_at] {
            hook_at_ms.store(std::chrono::duration<double, std::milli>(
                                 clock::now() - started_at)
                                 .count());
            hook_fired.store(true);
            ++shared_->observed_cancel;
        });

        while (!cancel.cancelled() && !hook_fired.load()) {
            const double elapsed =
                std::chrono::duration<double, std::milli>(clock::now() - started_at).count();
            if (elapsed >= static_cast<double>(work_ms_)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        cancel.ClearKillHook();

        const double total_ms =
            std::chrono::duration<double, std::milli>(clock::now() - started_at).count();
        if (hook_fired.load()) {
            shared_->last_cancel_latency_ms.store(hook_at_ms.load());
            double previous_latency = shared_->max_cancel_latency_ms.load();
            while (hook_at_ms.load() > previous_latency &&
                   !shared_->max_cancel_latency_ms.compare_exchange_weak(previous_latency,
                                                                        hook_at_ms.load())) {
            }
        }
        (void)shared_->last_cancel_latency_ms.load();

        --shared_->concurrent;

        fpgalsp::td::TdCheckResult result;
        result.cancelled = cancel.cancelled() || hook_fired.load();
        // 让 TdService 的 last_total_ms 有点意义（真实 runner 填的是 TD 内部耗时）
        result.timings.total_ms = total_ms;
        return result;
    }

private:
    int work_ms_ = 0;
    Shared* shared_ = nullptr;
};

// TdService 场景 A：连发 5 个请求 -> 只跑 1 轮（debounce 合并）
void ScenarioDebounce(int debounce_ms) {
    using clock = std::chrono::steady_clock;
    FakeSlowRunner::Shared shared;
    std::atomic<int> sink_calls{0};
    std::atomic<double> sink_at_ms{0.0};
    const auto origin = clock::now();

    fpgalsp::td::TdServiceOptions options;
    options.debounce_ms = debounce_ms;
    options.cancel_running = true;

    fpgalsp::td::TdService service(
        [&shared]() -> std::unique_ptr<fpgalsp::td::TdCheckRunner> {
            return std::make_unique<FakeSlowRunner>(200, &shared);
        },
        [&sink_calls, &sink_at_ms, origin](const fpgalsp::project::Project&,
                                          fpgalsp::td::TdCheckResult) {
            ++sink_calls;
            sink_at_ms.store(
                std::chrono::duration<double, std::milli>(clock::now() - origin).count());
        },
        options);

    fpgalsp::project::Project project;
    project.al_path = "C:/fake/debounce.al";
    project.dir = "C:/fake";

    // 5 次请求，间隔 40 ms（模拟连续 Ctrl+S），总跨 160 ms
    const auto burst_start = clock::now();
    for (int i = 0; i < 5; ++i) {
        service.Request(project);
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    const double last_request_ms =
        std::chrono::duration<double, std::milli>(clock::now() - burst_start).count();

    // 等结果
    for (int i = 0; i < 200 && sink_calls.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto stats = service.stats();
    service.Shutdown();

    std::printf("--- 场景 A：连发 5 个请求（间隔 40 ms，runner 工作 200 ms）---\n");
    std::printf("  requests=%llu runs=%llu cancelled=%llu superseded=%llu coalesced=%llu\n",
                static_cast<unsigned long long>(stats.requests),
                static_cast<unsigned long long>(stats.runs),
                static_cast<unsigned long long>(stats.cancelled),
                static_cast<unsigned long long>(stats.superseded),
                static_cast<unsigned long long>(stats.coalesced));
    std::printf("  runner_starts=%d sink_calls=%d max_concurrent=%d\n", shared.starts.load(),
                sink_calls.load(), shared.max_concurrent.load());
    std::printf("  last_request_at=%.1f ms  first_run_started_at=%.1f ms  "
                "debounce_from_last_request=%.1f ms  sink_at=%.1f ms\n",
                last_request_ms,
                last_request_ms + (sink_at_ms.load() - last_request_ms) - 200.0,
                (sink_at_ms.load() - 200.0) - last_request_ms, sink_at_ms.load());
    const bool ok = stats.runs == 1 && shared.starts.load() == 1 &&
                    stats.coalesced == 4 && sink_calls.load() == 1;
    std::printf("  verdict: %s\n\n", ok ? "OK（单飞 + 去抖合并生效）" : "FAIL");
}

// TdService 场景 B：跑到一半来新请求 -> 取消在跑的一轮，只发布最新一轮
void ScenarioCancel(int debounce_ms) {
    using clock = std::chrono::steady_clock;
    FakeSlowRunner::Shared shared;
    std::atomic<int> sink_calls{0};
    const auto origin = clock::now();
    std::atomic<double> first_cancel_at_ms{0.0};

    fpgalsp::td::TdServiceOptions options;
    options.debounce_ms = debounce_ms;
    options.debounce_after_cancel_ms = 0;
    options.cancel_running = true;

    fpgalsp::td::TdService service(
        [&shared]() -> std::unique_ptr<fpgalsp::td::TdCheckRunner> {
            return std::make_unique<FakeSlowRunner>(600, &shared);  // 每轮 600 ms
        },
        [&sink_calls](const fpgalsp::project::Project&, fpgalsp::td::TdCheckResult) {
            ++sink_calls;
        },
        options);

    fpgalsp::project::Project project;
    project.al_path = "C:/fake/cancel.al";
    project.dir = "C:/fake";

    service.Request(project);
    // 等第一轮真正开跑（debounce + 余量）
    std::this_thread::sleep_for(std::chrono::milliseconds(debounce_ms + 150));
    const int starts_before = shared.starts.load();
    service.Request(project);  // <- 应该取消第一轮
    for (int i = 0; i < 200 && sink_calls.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto stats = service.stats();
    service.Shutdown();

    std::printf("--- 场景 B：跑到一半再来请求（每轮 600 ms，debounce %d ms）---\n", debounce_ms);
    std::printf("  runner_started_before_second_request=%d\n", starts_before);
    std::printf("  requests=%llu runs=%llu cancelled=%llu superseded=%llu coalesced=%llu\n",
                static_cast<unsigned long long>(stats.requests),
                static_cast<unsigned long long>(stats.runs),
                static_cast<unsigned long long>(stats.cancelled),
                static_cast<unsigned long long>(stats.superseded),
                static_cast<unsigned long long>(stats.coalesced));
    std::printf("  runner_starts=%d observed_cancel=%d max_cancel_latency=%.1f ms\n",
                shared.starts.load(), shared.observed_cancel.load(),
                shared.max_cancel_latency_ms.load());
    std::printf("  sink_calls=%d max_concurrent=%d\n", sink_calls.load(),
                shared.max_concurrent.load());
    const bool ok = stats.cancelled >= 1 && stats.runs == 1 && sink_calls.load() == 1 &&
                    shared.max_concurrent.load() == 1 && shared.observed_cancel.load() >= 1;
    std::printf("  verdict: %s\n\n", ok ? "OK（取消生效、被取消轮不发布、无并发）" : "FAIL");
    (void)first_cancel_at_ms;
}

// TdService 场景 C：Request 之后立刻 Shutdown -> 不应有回调（退出路径）
void ScenarioShutdownNoPublish() {
    FakeSlowRunner::Shared shared;
    std::atomic<int> sink_calls{0};
    fpgalsp::td::TdServiceOptions options;
    options.debounce_ms = 50;
    fpgalsp::td::TdService service(
        [&shared]() -> std::unique_ptr<fpgalsp::td::TdCheckRunner> {
            return std::make_unique<FakeSlowRunner>(400, &shared);
        },
        [&sink_calls](const fpgalsp::project::Project&, fpgalsp::td::TdCheckResult) {
            ++sink_calls;
        },
        options);
    fpgalsp::project::Project project;
    project.al_path = "C:/fake/shutdown.al";
    project.dir = "C:/fake";
    service.Request(project);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // 已开跑
    const auto before = std::chrono::steady_clock::now();
    service.Shutdown();
    const double shutdown_ms = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - before)
                                   .count();
    std::printf("--- 场景 C：跑到一半 Shutdown（400 ms 的一轮）---\n");
    std::printf("  shutdown_ms=%.1f  sink_calls=%d  observed_cancel=%d\n", shutdown_ms,
                sink_calls.load(), shared.observed_cancel.load());
    std::printf("  verdict: %s\n\n",
                sink_calls.load() == 0 ? "OK（退出不发布）" : "FAIL（退出后仍发布了结果）");
}

// -----------------------------------------------------------------------------
//  lint（内嵌 Verible）
// -----------------------------------------------------------------------------
void RunLintBench(int iters, int warmup) {
    using fpgalsp::verible_bridge::AutofixText;

    // 进程内第一次调用：包含 Verible 静态表/config 的一次性初始化
    const std::string tiny = "module tiny (input wire clk);\nendmodule\n";
    {
        const auto t0 = std::chrono::steady_clock::now();
        const auto result = AutofixText(tiny, "tiny.v");
        const double ms = perf::MsSince(t0);
        std::printf("lint tiny (process-first call, includes init) : %8.3f ms ok=%d changed=%d\n",
                    ms, static_cast<int>(result.ok), static_cast<int>(result.changed));
    }

    const std::string text13 = MakeVerilog(13 * 1024);
    const std::string text100 = MakeVerilog(100 * 1024);
    std::printf("lint inputs: 13KB=%zu bytes, 100KB=%zu bytes\n", text13.size(), text100.size());

    {
        const auto t0 = std::chrono::steady_clock::now();
        const auto result = AutofixText(text13, "gen13.v");
        std::printf("lint 13KB (cold, first time this text)        : %8.3f ms ok=%d edits=%d\n",
                    perf::MsSince(t0), static_cast<int>(result.ok), result.applied_edits);
    }
    {
        auto st = perf::Bench(
            "AutofixText 13KB (warm)", iters,
            [&] { (void)AutofixText(text13, "gen13.v"); }, warmup);
        st.Print();
        st.PrintCsv();
    }
    {
        const auto t0 = std::chrono::steady_clock::now();
        const auto result = AutofixText(text100, "gen100.v");
        std::printf("lint 100KB (cold, first time this text)       : %8.3f ms ok=%d edits=%d\n",
                    perf::MsSince(t0), static_cast<int>(result.ok), result.applied_edits);
    }
    {
        auto st = perf::Bench(
            "AutofixText 100KB (warm)", iters,
            [&] { (void)AutofixText(text100, "gen100.v"); }, warmup);
        st.Print();
        st.PrintCsv();
    }
    std::printf("(lint 是 O(文件大小)；codeAction 调用一次 = 全量 parse + 全规则 lint，进程内无缓存)\n\n");
}

// -----------------------------------------------------------------------------
//  TD 探测（FindTd 扫盘）
// -----------------------------------------------------------------------------
void RunFindTdBench(int iters, int warmup) {
    using namespace fpgalsp::td;

    {
        const auto t0 = std::chrono::steady_clock::now();
        const auto installed = Installs();
        const double ms = perf::MsSince(t0);
        std::printf("td::Installs() (process-first call)           : %8.3f ms  found=%zu\n", ms,
                    installed.size());
        for (const auto& install : installed) {
            std::printf("    install: version=%s root=%s\n", install.version.c_str(),
                        install.root.c_str());
        }
    }
    {
        auto st = perf::Bench(
            "td::FindTd(\"\")", iters, [] { (void)FindTd(""); }, warmup);
        st.Print();
        st.PrintCsv();
    }
    {
        auto st = perf::Bench(
            "td::Installs()", iters, [] { (void)Installs(); }, warmup);
        st.Print();
        st.PrintCsv();
    }
    {
        // 显式路径应该完全跳过扫盘：对照实验
        const auto installed = Installs();
        const std::string root = installed.empty() ? std::string() : installed.front().root;
        auto st = perf::Bench(
            "td::FindTd(explicit root)", iters, [&] { (void)FindTd(root); }, warmup);
        st.Print();
        st.PrintCsv();
    }
    std::printf("\n");
}

// -----------------------------------------------------------------------------
//  真实 TD：会话启动/握手（只 Kill，不发任何命令）
// -----------------------------------------------------------------------------
bool RunTdSpawnBench(int iters, const std::string& work_dir) {
    using namespace fpgalsp::td;
    const std::optional<TdChoice> choice = FindTd("");
    if (!choice.has_value()) {
        std::printf("TD spawn: SKIP（本机没有找到 TangDynasty）\n\n");
        return false;
    }
    TdSessionOptions options;
    options.work_dir = work_dir;

    std::vector<double> spawn_ms;
    std::vector<double> kill_ms;
    for (int i = 0; i < iters; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        auto session = std::make_unique<TdSession>(*choice, options);
        const double started = perf::MsSince(t0);
        const auto t1 = std::chrono::steady_clock::now();
        session->Kill();
        const double killed = perf::MsSince(t1);
        spawn_ms.push_back(started);
        kill_ms.push_back(killed);
    }
    std::sort(spawn_ms.begin(), spawn_ms.end());
    std::sort(kill_ms.begin(), kill_ms.end());

    std::printf("--- 真实 TD 进程启动 + 握手（提示符 \"td > \", 不含任何命令）---\n");
    std::printf("  n=%zu  spawn p50=%.1f ms p95=%.1f ms max=%.1f ms\n", spawn_ms.size(),
                perf::Percentile(spawn_ms, 0.5), perf::Percentile(spawn_ms, 0.95), spawn_ms.back());
    std::printf("  Kill  p50=%.1f ms p95=%.1f ms max=%.1f ms\n", perf::Percentile(kill_ms, 0.5),
                perf::Percentile(kill_ms, 0.95), kill_ms.back());
    std::printf("  td exe=%s\n\n", choice->exe.c_str());
    return true;
}

// -----------------------------------------------------------------------------
//  真实 TD：一整轮 read_hdl（合成工程），给出 startup/import/read_hdl/parse 分解
// -----------------------------------------------------------------------------
void RunTdCheckOnce(const std::string& al_path, const std::string& work_dir) {
    using namespace fpgalsp::td;
    const std::optional<TdChoice> choice = FindTd("");
    if (!choice.has_value()) {
        std::printf("TD check: SKIP（本机没有找到 TangDynasty）\n");
        return;
    }
    const fpgalsp::project::Project project = fpgalsp::project::ParseAl(al_path);
    std::printf("--- 真实 TD 一轮检查（合成工程）---\n");
    std::printf("  al=%s  top=%s  device=%s  hdl=%zu\n", project.al_path.c_str(),
                project.top.c_str(), project.device.c_str(), project.HdlFiles().size());

    TdSessionOptions options;
    options.work_dir = work_dir;
    const double cpu_before = ProcessCpuMs();
    const auto t0 = std::chrono::steady_clock::now();

    TdSessionRunner runner(*choice, options, /*use_project_work_dir=*/false);
    CancelToken cancel;
    const TdCheckResult result = runner.Run(project, cancel);

    const double wall_ms = perf::MsSince(t0);
    const double cpu_ms = ProcessCpuMs() - cpu_before;

    std::printf("  failed=%d cancelled=%d diagnostics=%zu\n", static_cast<int>(result.failed),
                static_cast<int>(result.cancelled), result.total());
    std::printf("  phases: startup=%.1f  import=%.1f  read_hdl=%.1f  parse=%.1f  other=%.1f  "
                "total=%.1f ms\n",
                result.timings.startup_ms, result.timings.import_ms, result.timings.read_hdl_ms,
                result.timings.parse_ms, result.timings.other_ms(), result.timings.total_ms);
    std::printf("  wall=%.1f ms   our_cpu=%.1f ms  (cpu/wall=%.1f%%)\n", wall_ms, cpu_ms,
                wall_ms > 0 ? 100.0 * cpu_ms / wall_ms : 0.0);
    if (!result.error.empty()) std::printf("  error=%s\n", result.error.c_str());
    std::printf("  work_dir=%s\n\n", runner.last_work_dir().c_str());
}

}  // namespace

int main(int argc, char** argv) {
    perf::Cli cli = perf::ParseCli(argc, argv);

    // --td / --check-al 是本探针的扩展参数（ParseCli 只认 --iters/--warmup）
    bool with_td = false;
    std::string check_al;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--td") with_td = true;
        if (arg == "--check-al" && i + 1 < argc) check_al = argv[++i];
    }

    perf::Banner("td");
    const std::string work_dir = MakeTempDir("work");
    std::printf("work_dir=%s\n\n", work_dir.c_str());

    RunFindTdBench(cli.iters, cli.warmup);
    RunLintBench(cli.iters, cli.warmup);
    ScenarioDebounce(300);
    ScenarioCancel(300);
    ScenarioShutdownNoPublish();

    if (with_td) RunTdSpawnBench(std::min(cli.iters, 10), work_dir);
    if (!check_al.empty()) RunTdCheckOnce(check_al, work_dir);

    std::printf("working_set=%.2f MB peak=%.2f MB\n", perf::Mb(perf::CurrentWorkingSetBytes()),
                perf::Mb(perf::PeakWorkingSetBytes()));
    return 0;
}