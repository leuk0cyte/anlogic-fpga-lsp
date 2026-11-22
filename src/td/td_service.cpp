// =============================================================================
//  src/td/td_service.cpp —— TdService 实现（A5 / task-25）
// =============================================================================

#include "td/td_service.h"

#include <utility>

#include "platform/path.h"

namespace fpgalsp::td {

// -----------------------------------------------------------------------------
// TdSessionRunner
// -----------------------------------------------------------------------------

TdSessionRunner::TdSessionRunner(TdChoice td, TdSessionOptions options, bool use_project_work_dir)
    : td_(std::move(td)), options_(std::move(options)), use_project_work_dir_(use_project_work_dir) {}

TdCheckResult TdSessionRunner::Run(const project::Project& project, const CancelToken& cancel) {
    TdCheckResult result;

    // 会话 cwd 用**状态目录**（`%TEMP%\fpgalsp\<工程名>-<hash8>\run`）。
    // ⚠ 这里曾经硬编码 `<project>/.fpgalsp/run`：每次 LSP 保存触发 TD 检查都会在工程根
    //   重新长出 `.fpgalsp`（2026-11-22 实测复现：删掉后一次 didSave 立刻重建 `run/`）。
    //   工程目录里的 `.fpgalsp` 已废弃（Wave 3），状态一律走 platform::state_dir()。
    TdSessionOptions options = options_;
    if (use_project_work_dir_ && project.dir.empty() == false) {
        options.work_dir = platform::path_join(platform::state_dir(project.dir), "run");
    }

    // 会话启动（CreateProcessW + 读横幅）单独计时：它是"预热"唯一能省掉的一段。
    PhaseStopwatch watch;
    std::unique_ptr<TdSession> session;
    try {
        session = std::make_unique<TdSession>(td_, options);
    } catch (const std::exception& exc) {
        result.failed = true;
        result.error = std::string("cannot start TD: ") + exc.what();
        result.timings.total_ms = watch.ElapsedMs();
        return result;
    }
    result.timings.startup_ms = watch.LapMs();
    last_work_dir_ = session->WorkDir();

    // 取消的"阻塞点"出口：杀掉整棵进程树 -> RunReadHdlCheck 里那个 Run 会抛错。
    // 注意 hook 里只捕获裸指针：令牌不会活过 session（下面 ClearKillHook + 作用域）。
    TdSession* raw = session.get();
    cancel.SetKillHook([raw] { raw->Kill(); });

    if (cancel.cancelled()) {
        cancel.ClearKillHook();
        session->Kill();
        result.cancelled = true;
        result.timings.total_ms = result.timings.startup_ms;
        return result;
    }

    TdCheckResult inner = RunReadHdlCheck(project, *session, {}, &cancel);
    cancel.ClearKillHook();

    // read_hdl 之后 TD 会关闭自己那侧的 stdout（td_session.h 的硬约束）：会话不可复用，
    // 直接 Kill（Close 会写 exit 并等 8s，此时管道已死，纯属浪费）。
    session->Kill();

    // total 以"调用方视角"重新算：startup + 检查其余部分
    inner.timings.startup_ms = result.timings.startup_ms;
    inner.timings.total_ms = inner.timings.total_ms + result.timings.startup_ms;
    return inner;
}

// -----------------------------------------------------------------------------
// TdService
// -----------------------------------------------------------------------------

TdService::TdService(RunnerFactory factory, ResultSink sink, TdServiceOptions options)
    : factory_(std::move(factory)), sink_(std::move(sink)), options_(options) {
    worker_ = std::thread([this] { Loop(); });
}

TdService::~TdService() { Shutdown(); }

uint64_t TdService::Request(project::Project project) {
    std::shared_ptr<CancelToken> to_cancel;
    uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_) return 0;
        seq = ++next_seq_;
        const auto now = std::chrono::steady_clock::now();
        const bool running = static_cast<bool>(active_);
        // 在跑的一轮被取消时，新一轮不该再等一个完整的去抖窗口（见 options 注释）
        const int wait_ms = running ? options_.debounce_after_cancel_ms : options_.debounce_ms;
        const auto ready_at = now + std::chrono::milliseconds(wait_ms > 0 ? wait_ms : 0);
        if (pending_.has_value()) {
            // 还在去抖窗口里：覆盖 pending（只跑最新的那份 project），并顺延静默期
            std::lock_guard<std::mutex> stats_lock(stats_mutex_);
            ++stats_.coalesced;
        }
        pending_ = Pending{std::move(project), seq, now, ready_at};
        {
            std::lock_guard<std::mutex> stats_lock(stats_mutex_);
            ++stats_.requests;
        }
        // 在跑的一轮必须让路：先取消（置位 + kill 进程树），worker 会把它标成 cancelled
        if (options_.cancel_running) to_cancel = active_;
    }
    // 在锁外取消：Cancel() 会同步执行 kill hook（TerminateProcess），不要持锁做系统调用
    if (to_cancel) to_cancel->Cancel();
    cv_.notify_all();
    return seq;
}

void TdService::CancelActive() {
    std::shared_ptr<CancelToken> to_cancel;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.reset();
        to_cancel = active_;
    }
    if (to_cancel) to_cancel->Cancel();
    cv_.notify_all();
}

void TdService::Shutdown() {
    std::shared_ptr<CancelToken> to_cancel;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        pending_.reset();
        to_cancel = active_;
    }
    if (to_cancel) to_cancel->Cancel();
    cv_.notify_all();
    // 从 worker 线程里调 Shutdown（例如 sink 里触发了退出）时不能 join 自己；
    // 那种情况下线程会在 Loop 里看到 stop_ 自行返回，由后续（主线程）的 Shutdown
    // 或析构完成 join。
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
        worker_.join();
    }
}

TdService::Stats TdService::stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

void TdService::Loop() {
    for (;;) {
        Pending job;
        std::shared_ptr<CancelToken> token;
        double queue_ms = 0.0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || pending_.has_value(); });
            if (stop_) return;

            // 去抖：等到"最后一次请求 + debounce"。期间每来一个新请求，Request()
            // 都会把 ready_at 顺延并 notify，所以这里醒来后重新等即可。
            while (!stop_ && pending_.has_value()) {
                if (cv_.wait_until(lock, pending_->ready_at) == std::cv_status::timeout) break;
            }
            if (stop_ || !pending_.has_value()) continue;

            const auto requested_at = std::chrono::steady_clock::now();
            job = std::move(*pending_);
            pending_.reset();
            // 排队等待 = 从"最后一次请求"到"真正开跑"（含去抖窗口）
            queue_ms = std::chrono::duration<double, std::milli>(requested_at - job.requested_at)
                           .count();
            if (queue_ms < 0.0) queue_ms = 0.0;
            token = std::make_shared<CancelToken>();
            active_ = token;
        }

        std::unique_ptr<TdCheckRunner> runner;
        TdCheckResult result;
        try {
            runner = factory_ ? factory_() : nullptr;
            if (!runner) {
                result.failed = true;
                result.error = "no TD check runner available";
            } else {
                result = runner->Run(job.project, *token);
            }
        } catch (const std::exception& exc) {
            result.failed = true;
            result.error = std::string("TD check threw: ") + exc.what();
        } catch (...) {
            result.failed = true;
            result.error = "TD check threw an unknown exception";
        }

        bool publish = false;
        bool superseded = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            active_.reset();
            // 跑完时如果已经有更新的请求在 pending：这一轮的结果**已经过期**。
            // 发布它只会让 Problems 闪一下旧数据（旧策略就是这样），所以直接丢弃。
            superseded = pending_.has_value();
            publish = !result.cancelled && !stop_ && !superseded;
        }
        {
            std::lock_guard<std::mutex> stats_lock(stats_mutex_);
            if (result.cancelled || stop_) {
                ++stats_.cancelled;
            } else if (superseded) {
                ++stats_.superseded;
            } else {
                ++stats_.runs;
                stats_.last_total_ms = result.timings.total_ms;
                stats_.last_queue_ms = queue_ms;
            }
        }
        // 只在锁外发布；取消的轮次**绝不发布**（半截 read_hdl 输出会误报 0 条诊断）
        if (publish && sink_) {
            sink_(job.project, std::move(result));
        }
    }
}

}  // namespace fpgalsp::td