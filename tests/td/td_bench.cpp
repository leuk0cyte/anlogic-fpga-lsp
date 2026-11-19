// =============================================================================
//  tests/td/td_bench.cpp —— A5：TD 检查流水线的真机测量（task-25）
// =============================================================================
//  用法（不注册进默认 ctest；LABEL bench）：
//      ctest --test-dir build-td -L bench --output-on-failure
//      或直接  build-td\bin\td_bench.exe
//
//  测什么（全部在 %TEMP% 的工程副本上跑，绝不碰真实工程）：
//    A. 单次检查的分段耗时：startup / import_device / read_hdl / parse / total
//    B. "连续保存 5 次"：
//         B1 旧策略（lsp_server 现状）：在跑的一轮不取消，只排一次 pending
//         B2 新策略（TdService）：300 ms 去抖 + 取消在跑的一轮
//       对比：TD 启动次数、总耗时、发布次数（= 白工与 UI 闪烁）
//    C. 取消延迟：read_hdl 进行中请求新一轮，旧的一轮多久退出
//    D. 预热评估：单独测量 startup、空闲 3 s 后会话是否可用、read_hdl 之后是否可复用
//    E. 进程残留
//
//  输出：人读表格 + 以 "A5|" 开头的机器可读行（便于贴进报告）。
// =============================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "project/al_project.h"
#include "td/td_cancel.h"
#include "td/td_diagnostics.h"
#include "td/td_install.h"
#include "td/td_service.h"
#include "td/td_session.h"
#include "td/td_timing.h"

namespace {

namespace fs = std::filesystem;
using namespace fpgalsp;
using namespace fpgalsp::td;
using namespace std::chrono_literals;

constexpr const char* kTdRelatedExes[] = {"td_commands_prompt.exe", "td_commands.exe", "td.exe",
                                          "td_client.exe"};

int TotalTdProcesses() {
    int total = 0;
    for (const char* name : kTdRelatedExes) {
        const int n = CountProcessesByName(name);
        if (n > 0) total += n;
    }
    return total;
}

double MsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}

void Row(std::string_view label, double a, double b = -1, double c = -1, double d = -1) {
    std::printf("  %-32s%11.1f", std::string(label).c_str(), a);
    if (b >= 0) std::printf(" | %9.1f", b);
    if (c >= 0) std::printf(" | %9.1f", c);
    if (d >= 0) std::printf(" | %9.1f", d);
    std::printf("\n");
}

// 复制工程到 %TEMP%：跳过构建产物目录（.fpgalsp / *_Runs / log），其余原样拷贝
bool CopyProjectTo(const fs::path& from, const fs::path& to, std::string* error) {
    std::error_code ec;
    fs::remove_all(to, ec);
    fs::create_directories(to, ec);
    if (ec) {
        *error = "create_directories: " + ec.message();
        return false;
    }
    for (fs::recursive_directory_iterator it(from, ec), end; it != end && !ec; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (it->is_directory(ec)) {
            if (name == ".fpgalsp" || name == "log" ||
                (name.size() > 5 && name.compare(name.size() - 5, 5, "_Runs") == 0)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        const fs::path rel = fs::relative(it->path(), from, ec);
        if (ec) break;
        const fs::path dst = to / rel;
        fs::create_directories(dst.parent_path(), ec);
        if (ec) break;
        fs::copy_file(it->path(), dst, fs::copy_options::overwrite_existing, ec);
        if (ec) break;
    }
    if (ec) {
        *error = "copy: " + ec.message();
        return false;
    }
    return true;
}

// 一轮完整检查（真实 TD）：会话启动 + import_device + read_hdl + 解析
TdCheckResult RunOneCheck(const project::Project& project, const TdChoice& td,
                          const TdSessionOptions& options, const CancelToken* cancel) {
    static CancelToken no_cancel;
    TdSessionRunner runner(td, options);
    return runner.Run(project, cancel != nullptr ? *cancel : no_cancel);
}

}  // namespace

int main() {
    std::printf("============================================================\n");
    std::printf(" A5 / task-25  TD 诊断流水线测量\n");
    std::printf("============================================================\n");

    // ---- 0) 环境 ----
    const auto td = FindTd();
    if (!td.has_value()) {
        std::printf("[skip] 本机没有可用的 TD 安装\n");
        return 0;
    }
    std::printf("TD root      : %s\n", td->root.c_str());
    std::printf("TD version   : %s\n", td->version.c_str());

    const fs::path corpus = fs::path(FPGA_LSP_SOURCE_DIR) / "tests" / "gate-corpus" / "corpus_alpha";
    if (!fs::exists(corpus / "demo_project.al")) {
        std::printf("[skip] 找不到语料 %s\n", corpus.string().c_str());
        return 0;
    }

    std::error_code ec;
    const std::string stamp =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const fs::path work_root = fs::temp_directory_path(ec) / ("fpgalsp-a5-bench-" + stamp);
    fs::create_directories(work_root, ec);
    const fs::path proj_dir = work_root / "proj";
    std::string copy_error;
    if (!CopyProjectTo(corpus, proj_dir, &copy_error)) {
        std::printf("[skip] 复制语料失败: %s\n", copy_error.c_str());
        return 0;
    }
    const project::Project project = project::ParseAl((proj_dir / "demo_project.al").string());
    std::printf("project      : %s (hdl=%zu)\n", project.al_path.c_str(), project.HdlFiles().size());
    std::printf("work copy    : %s\n", proj_dir.string().c_str());
    std::printf("baseline td processes: %d\n", TotalTdProcesses());

    TdSessionOptions session_options;
    session_options.work_dir = (work_root / "td-scratch").string();
    session_options.startup_timeout_seconds = 60.0;

    // ---- A) 单次检查分段 ----
    std::printf("\n[A] 单次检查分段耗时（3 次）\n");
    std::printf("  %-32s%11s | %9s | %9s | %9s\n", "run", "startup", "import", "read_hdl",
                "parse");
    std::vector<TdPhaseTimings> samples;
    for (int i = 0; i < 3; ++i) {
        const TdCheckResult r = RunOneCheck(project, *td, session_options, nullptr);
        samples.push_back(r.timings);
        Row("run " + std::to_string(i + 1), r.timings.startup_ms, r.timings.import_ms,
            r.timings.read_hdl_ms, r.timings.parse_ms);
        std::printf("  %-32s%11.1f  (db_candidates=%d diags=%zu cancelled=%d)\n", "  total",
                    r.timings.total_ms, r.timings.db_candidates, r.total(),
                    static_cast<int>(r.cancelled));
    }
    TdPhaseTimings mean;
    for (const TdPhaseTimings& s : samples) {
        const double n = static_cast<double>(samples.size());
        mean.startup_ms += s.startup_ms / n;
        mean.import_ms += s.import_ms / n;
        mean.read_hdl_ms += s.read_hdl_ms / n;
        mean.parse_ms += s.parse_ms / n;
        mean.total_ms += s.total_ms / n;
        mean.db_candidates = s.db_candidates;
    }
    Row("MEAN", mean.startup_ms, mean.import_ms, mean.read_hdl_ms, mean.parse_ms);
    std::printf("  %-32s%11.1f  (accounted %.1f + other %.1f)\n", "  MEAN total",
                mean.total_ms, mean.accounted_ms(), mean.other_ms());
    std::printf("  A5|single|startup=%.1f|import=%.1f|read_hdl=%.1f|parse=%.1f|total=%.1f|"
                "other=%.1f|db_candidates=%d\n",
                mean.startup_ms, mean.import_ms, mean.read_hdl_ms, mean.parse_ms, mean.total_ms,
                mean.other_ms(), mean.db_candidates);

    // ---- B) 连续保存 5 次 ----
    std::printf("\n[B] 连续保存 5 次（每 100 ms 一次保存）\n");
    std::printf("  旧策略 = lsp_server 现状：在跑的一轮不取消，只排一次 pending\n");

    // B1 旧策略
    {
        std::printf("\n  B1 旧策略：\n");
        const int baseline = TotalTdProcesses();
        const auto t0 = std::chrono::steady_clock::now();
        // 4 次保存发生在第一轮进行中
        std::thread saves([t0] {
            for (int i = 0; i < 4; ++i) {
                std::this_thread::sleep_for(100ms);
                std::printf("      save#%d at +%8.1f ms (排队 pending)\n", i + 1, MsSince(t0));
            }
        });
        const TdCheckResult r1 = RunOneCheck(project, *td, session_options, nullptr);
        saves.join();
        std::printf("      round1 done at +%8.1f ms (diags=%zu)\n", MsSince(t0), r1.total());
        const TdCheckResult r2 = RunOneCheck(project, *td, session_options, nullptr);
        const double wall = MsSince(t0);
        std::printf("      round2 done at +%8.1f ms (diags=%zu)\n", wall, r2.total());
        std::printf("    wall=%8.1f ms  td_starts=2  publishes=2  (baseline_procs=%d)\n", wall,
                    baseline);
        std::printf("  A5|burst_legacy|wall=%.1f|td_starts=2|publishes=2|baseline_procs=%d\n", wall,
                    baseline);
    }

    // B2 新策略
    {
        std::printf("\n  B2 新策略（TdService：300 ms 去抖 + 取消）：\n");
        const auto burst_start = std::chrono::steady_clock::now();
        std::atomic<int> publishes{0};
        std::atomic<int> td_starts{0};
        TdService service(
            [&td, &session_options, &td_starts] {
                td_starts.fetch_add(1);
                return std::make_unique<TdSessionRunner>(*td, session_options);
            },
            [&publishes, burst_start](const project::Project&, TdCheckResult r) {
                publishes.fetch_add(1);
                std::printf("      publish at +%8.1f ms (diags=%zu)\n", MsSince(burst_start),
                            r.total());
            },
            TdServiceOptions{/*debounce_ms=*/300});

        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 5; ++i) {
            service.Request(project);
            std::printf("      save#%d at +%8.1f ms\n", i + 1, MsSince(t0));
            std::this_thread::sleep_for(100ms);
        }
        const auto deadline = std::chrono::steady_clock::now() + 120s;
        while (publishes.load() < 1 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(20ms);
        }
        const double wall = MsSince(t0);
        service.Shutdown();
        const TdService::Stats stats = service.stats();
        std::printf("    wall=%8.1f ms  td_starts=%d  publishes=%d  requests=%llu runs=%llu "
                    "cancelled=%llu coalesced=%llu\n",
                    wall, td_starts.load(), publishes.load(),
                    static_cast<unsigned long long>(stats.requests),
                    static_cast<unsigned long long>(stats.runs),
                    static_cast<unsigned long long>(stats.cancelled),
                    static_cast<unsigned long long>(stats.coalesced));
        std::printf("  A5|burst_service|wall=%.1f|td_starts=%d|publishes=%d|requests=%llu|runs=%llu|"
                    "cancelled=%llu|coalesced=%llu\n",
                    wall, td_starts.load(), publishes.load(),
                    static_cast<unsigned long long>(stats.requests),
                    static_cast<unsigned long long>(stats.runs),
                    static_cast<unsigned long long>(stats.cancelled),
                    static_cast<unsigned long long>(stats.coalesced));
    }

    // ---- C) 取消延迟 ----
    std::printf("\n[C] 取消延迟（read_hdl 进行中请求取消）\n");
    {
        CancelToken token;
        std::atomic<double> cancel_at{-1.0};
        const auto t0 = std::chrono::steady_clock::now();
        std::thread canceller([&token, &cancel_at, t0] {
            std::this_thread::sleep_for(800ms);  // 本机 startup 约 300-500 ms，800 ms 落在 read_hdl 内
            cancel_at.store(MsSince(t0));
            token.Cancel();
        });
        const TdCheckResult r = RunOneCheck(project, *td, session_options, &token);
        const double done_at = MsSince(t0);
        canceller.join();
        std::printf("    cancel at +%8.1f ms -> runner returned at +%8.1f ms "
                    "(latency %.1f ms), cancelled=%d, partial_diags=%zu\n",
                    cancel_at.load(), done_at, done_at - cancel_at.load(),
                    static_cast<int>(r.cancelled), r.total());
        std::printf("  A5|cancel|sent_at=%.1f|returned_at=%.1f|latency=%.1f|cancelled=%d|"
                    "partial_diags=%zu\n",
                    cancel_at.load(), done_at, done_at - cancel_at.load(),
                    static_cast<int>(r.cancelled), r.total());
    }

    // ---- C2) 取消 vs 等它跑完：第 2 次保存在第 1 轮 read_hdl 期间到达 ----
    std::printf("\n[C2] 第 2 次保存在第 1 轮 read_hdl 期间到达（间隔 800 ms）\n");
    {
        const auto t0 = std::chrono::steady_clock::now();
        const TdCheckResult r1 = RunOneCheck(project, *td, session_options, nullptr);
        const double first_done = MsSince(t0);
        const TdCheckResult r2 = RunOneCheck(project, *td, session_options, nullptr);
        const double legacy_final = MsSince(t0);
        (void)r1;
        (void)r2;
        std::printf("    旧策略：round1 done +%8.1f ms, final publish +%8.1f ms (td_starts=2)\n",
                    first_done, legacy_final);
        std::printf("  A5|burst2_legacy|final=%.1f|td_starts=2\n", legacy_final);

        std::atomic<int> publishes{0};
        std::atomic<int> starts{0};
        std::atomic<double> publish_at{-1.0};
        const auto burst_start = std::chrono::steady_clock::now();
        TdService service(
            [&td, &session_options, &starts] {
                starts.fetch_add(1);
                return std::make_unique<TdSessionRunner>(*td, session_options);
            },
            [&publishes, &publish_at, burst_start](const project::Project&, TdCheckResult) {
                publishes.fetch_add(1);
                publish_at.store(MsSince(burst_start));
            },
            TdServiceOptions{/*debounce_ms=*/300});
        service.Request(project);
        std::this_thread::sleep_for(800ms);  // 第 1 轮此时应在 read_hdl 内
        service.Request(project);            // 第二次保存 -> 取消 + 只跑最新
        const auto deadline = std::chrono::steady_clock::now() + 120s;
        while (publishes.load() < 1 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(20ms);
        }
        service.Shutdown();
        const TdService::Stats st = service.stats();
        std::printf("    新策略：final publish +%8.1f ms (td_starts=%d cancelled=%llu runs=%llu)\n",
                    publish_at.load(), starts.load(),
                    static_cast<unsigned long long>(st.cancelled),
                    static_cast<unsigned long long>(st.runs));
        std::printf("  A5|burst2_service|final=%.1f|td_starts=%d|cancelled=%llu|runs=%llu\n",
                    publish_at.load(), starts.load(),
                    static_cast<unsigned long long>(st.cancelled),
                    static_cast<unsigned long long>(st.runs));

        // C2b: 同样的时序，但 cancel_running=false（排队而非取消）
        std::atomic<int> publishes_b{0};
        std::atomic<int> starts_b{0};
        std::atomic<double> publish_at_b{-1.0};
        const auto burst_start_b = std::chrono::steady_clock::now();
        TdService service_b(
            [&td, &session_options, &starts_b] {
                starts_b.fetch_add(1);
                return std::make_unique<TdSessionRunner>(*td, session_options);
            },
            [&publishes_b, &publish_at_b, burst_start_b](const project::Project&, TdCheckResult) {
                publishes_b.fetch_add(1);
                publish_at_b.store(MsSince(burst_start_b));
            },
            TdServiceOptions{/*debounce_ms=*/300, /*debounce_after_cancel_ms=*/0,
                             /*cancel_running=*/false});
        service_b.Request(project);
        std::this_thread::sleep_for(800ms);
        service_b.Request(project);
        const auto deadline_b = std::chrono::steady_clock::now() + 120s;
        while (publishes_b.load() < 1 && std::chrono::steady_clock::now() < deadline_b) {
            std::this_thread::sleep_for(20ms);
        }
        service_b.Shutdown();
        const TdService::Stats st_b = service_b.stats();
        std::printf("    新策略(不取消)：final publish +%8.1f ms (td_starts=%d cancelled=%llu "
                    "runs=%llu publishes=%d)\n",
                    publish_at_b.load(), starts_b.load(),
                    static_cast<unsigned long long>(st_b.cancelled),
                    static_cast<unsigned long long>(st_b.runs), publishes_b.load());
        std::printf("  A5|burst2_service_queue|final=%.1f|td_starts=%d|cancelled=%llu|runs=%llu|"
                    "publishes=%d\n",
                    publish_at_b.load(), starts_b.load(),
                    static_cast<unsigned long long>(st_b.cancelled),
                    static_cast<unsigned long long>(st_b.runs), publishes_b.load());
    }

    // ---- D) 预热评估 ----
    std::printf("\n[D] 预热评估（空闲时预启动会话，保存时复用）\n");
    {
        // D1 startup 单独计时
        std::vector<double> startups;
        for (int i = 0; i < 3; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            std::unique_ptr<TdSession> session;
            session = std::make_unique<TdSession>(*td, session_options);
            startups.push_back(MsSince(t0));
            session->Kill();
        }
        std::sort(startups.begin(), startups.end());
        const double startup_median = startups[startups.size() / 2];

        // D2 预热会话空闲 3 s 后还能不能用
        bool usable_after_idle = false;
        double idle_probe_ms = -1.0;
        {
            TdSession session(*td, session_options);
            std::this_thread::sleep_for(3000ms);
            const auto t0 = std::chrono::steady_clock::now();
            try {
                const std::string out = session.Run("puts __a5_idle_probe__", 30.0);
                usable_after_idle = out.find("__a5_idle_probe__") != std::string::npos;
                idle_probe_ms = MsSince(t0);
            } catch (...) {
                usable_after_idle = false;
            }
            session.Kill();
        }

        // D3 read_hdl 之后同一个会话还能不能发命令（决定预热能否服务多次）
        //    分三步探测，避免"第一条命令碰巧还能出声"造成误判：
        //      a) puts 探针（轻命令）
        //      b) 第二条 read_hdl（真正的重命令）
        //      c) 再一条 puts（确认 b 之后管道还在）
        bool usable_after_read_hdl = true;
        bool second_read_hdl_ok = false;
        bool usable_after_second_read_hdl = true;
        std::string probe_after_read_hdl;
        {
            TdSession session(*td, session_options);
            static const CancelToken no_cancel;
            const TdCheckResult first = RunReadHdlCheck(project, session, {}, &no_cancel);
            std::printf("    (first read_hdl: diags=%zu, %.1f ms)\n", first.total(),
                        first.timings.read_hdl_ms);
            try {
                probe_after_read_hdl = session.Run("puts __a5_after_read_hdl__", 15.0);
                usable_after_read_hdl =
                    probe_after_read_hdl.find("__a5_after_read_hdl__") != std::string::npos;
            } catch (const std::exception& exc) {
                probe_after_read_hdl = std::string("threw: ") + exc.what();
                usable_after_read_hdl = false;
            }
            try {
                const TdCheckResult second = RunReadHdlCheck(project, session, {}, &no_cancel);
                second_read_hdl_ok = !second.cancelled && !second.failed;
                std::printf("    (second read_hdl: diags=%zu, %.1f ms, ok=%d)\n", second.total(),
                            second.timings.read_hdl_ms, static_cast<int>(second_read_hdl_ok));
            } catch (const std::exception& exc) {
                std::printf("    (second read_hdl threw: %s)\n", exc.what());
                second_read_hdl_ok = false;
            }
            try {
                const std::string again = session.Run("puts __a5_third_probe__", 15.0);
                usable_after_second_read_hdl =
                    again.find("__a5_third_probe__") != std::string::npos;
            } catch (...) {
                usable_after_second_read_hdl = false;
            }
            session.Kill();
        }

        std::printf("    startup median              : %8.1f ms (n=3, min %.1f, max %.1f)\n",
                    startup_median, startups.front(), startups.back());
        std::printf("    prewarmed session + idle 3 s: usable=%d (probe %.1f ms)\n",
                    static_cast<int>(usable_after_idle), idle_probe_ms);
        std::printf("    after 1st read_hdl          : puts_ok=%d  second_read_hdl_ok=%d  "
                    "puts_after_2nd=%d  [%s]\n",
                    static_cast<int>(usable_after_read_hdl), static_cast<int>(second_read_hdl_ok),
                    static_cast<int>(usable_after_second_read_hdl),
                    probe_after_read_hdl.substr(0, 50).c_str());
        std::printf("  A5|prewarm|startup_median=%.1f|usable_after_idle_3s=%d|"
                    "usable_after_read_hdl=%d|second_read_hdl_ok=%d|usable_after_second_read_hdl=%d|"
                    "startup_share_of_total=%.1f%%\n",
                    startup_median, static_cast<int>(usable_after_idle),
                    static_cast<int>(usable_after_read_hdl), static_cast<int>(second_read_hdl_ok),
                    static_cast<int>(usable_after_second_read_hdl),
                    mean.total_ms > 0 ? startup_median * 100.0 / mean.total_ms : 0.0);
    }

    // ---- E) 残留 ----
    std::printf("\n[E] 进程残留\n");
    {
        int residue = TotalTdProcesses();
        for (int i = 0; i < 100 && residue > 0; ++i) {
            std::this_thread::sleep_for(100ms);
            residue = TotalTdProcesses();
        }
        std::printf("    residual td processes: %d\n", residue);
        std::printf("  A5|residue|processes=%d\n", residue);
    }

    fs::remove_all(work_root, ec);
    std::printf("\n[done] work copy removed: %s\n", work_root.string().c_str());
    return 0;
}