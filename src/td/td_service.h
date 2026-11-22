// =============================================================================
//  src/td/td_service.h —— TD 检查的单飞队列 + 取消 + 去抖（A5 / task-25）
// =============================================================================
//  背景（实测，见 doc/audit/2026-11-18/phase2/A5-REPORT.md）：
//    一次 fpgalsp.check 的真实开销是"进程启动 + import_device + read_hdl"，
//    其中 read_hdl 是大头（TD 真在解析 HDL）。旧实现（lsp_server.cpp 的路
//    TdCheckLoop）的策略是：
//        * 检查进行中新保存到达 -> 只置 td_pending_，**不取消**在跑的那轮；
//        * 于是用户连按 5 次保存，要先白等当前轮跑完（可能十几秒），
//          再把最新的一轮跑一遍 —— 期间 Problems 面板一直是旧数据；
//        * 而且每一轮都要重新启动一个 TD 进程。
//
//  本文件把策略从"服务器里的一段循环"抽成可测的独立组件：
//
//      Request(project)  ──►  [300 ms 去抖窗口]  ──►  单飞执行
//                              ▲                      │
//                              └── 新请求取消在跑的一轮 ┘
//
//  三条保证：
//    1) **单飞**：任何时刻最多一个 TD 检查在跑（TD 是重进程，且 read_hdl 会消耗会话）；
//    2) **只跑最新**：新请求取消在跑的一轮（kill 进程树）并替换 pending，
//       永远不会有"跑完旧的再跑一遍最新的"这种白工；
//    3) **去抖**：最后一次请求之后静默 300 ms 才开跑，连续保存只触发一次。
//
//  ⚠ 取消是"尽力而为 + 有代价"的：被取消的一轮已经把 TD 进程拉起来了，
//    杀掉它本身要几十毫秒；但这个代价远小于让它跑完（read_hdl 数秒~数十秒）。
// =============================================================================

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "project/al_project.h"
#include "td/td_cancel.h"
#include "td/td_diagnostics.h"
#include "td/td_install.h"
#include "td/td_session.h"

namespace fpgalsp::td {

// -----------------------------------------------------------------------------
// 执行一轮检查的抽象
// -----------------------------------------------------------------------------
// 抽象出来的唯一理由：让"队列/取消/去抖"这些**策略**能在毫秒级单测里验证，
// 不需要真实 TD（真实 TD 一轮要几秒，且必须有安装）。
class TdCheckRunner {
public:
    virtual ~TdCheckRunner() = default;
    // 执行一轮；必须响应 cancel（协作点检查 cancelled()，阻塞点靠 kill hook）。
    virtual TdCheckResult Run(const project::Project& project, const CancelToken& cancel) = 0;
};

// 真实实现：建 TdSession -> RunReadHdlCheck -> Kill。
// 与 lsp_server::RunTdCheckOnce 的既有行为一致（read_hdl 之后会话必须丢弃）。
class TdSessionRunner final : public TdCheckRunner {
public:
    // use_project_work_dir:
    //   true （默认）：cwd = **`.al` 所在目录**（与 TD GUI 一致）。TD 用 cwd 解析 `.al` 里的
    //        相对路径，cwd 落在 %TEMP% 会让被移动过的工程报 HDL-8007；会话日志由
    //        TdSession 归位到 `<al>/log/`，工程根目录不留痕；
    //   false：cwd 用 TdSessionOptions 的值；留空则走 B7/D-3 的 %TEMP% scratch，
    //        完全不碰用户工程。
    TdSessionRunner(TdChoice td, TdSessionOptions options = {}, bool use_project_work_dir = true);

    TdCheckResult Run(const project::Project& project, const CancelToken& cancel) override;

    // 上一次运行时实际使用的会话 work_dir（诊断用）
    [[nodiscard]] const std::string& last_work_dir() const { return last_work_dir_; }

private:
    TdChoice td_;
    TdSessionOptions options_;
    bool use_project_work_dir_ = true;
    std::string last_work_dir_;
};

// -----------------------------------------------------------------------------
// 服务
// -----------------------------------------------------------------------------

struct TdServiceOptions {
    // 去抖窗口：最后一次 Request 之后静默这么久才开跑。
    // 300 ms 是"人手连续保存"（Ctrl+S 连按、格式化后保存）的经验值：
    // 太小会在编辑风暴里反复起停 TD，太大会让单次保存的诊断明显延迟。
    int debounce_ms = 300;
    // 取消在跑的一轮之后，新一轮的等待时间。
    // 为什么默认 0：用户刚刚保存过、而且我们已经为此杀掉了一轮在跑的检查 ——
    // 再等 300 ms 纯属白等（实测 C2：这一项让"保存落在 read_hdl 期间"的
    // 最终发布时间从 1999 ms 降到 ~1700 ms）。抖动保护由 debounce_ms 负责，
    // 这里不需要第二个窗口。
    int debounce_after_cancel_ms = 0;
    // 是否取消在跑的一轮。
    //   true （默认）：适合 read_hdl 较长的工程（剩下的一轮可能要跑十几秒）
    //   false        ：退化成"排队一次"的旧行为，适合小工程（一轮 < 1 s，
    //                  杀掉重跑反而比让它跑完更慢，见 A5-REPORT §2 C2 数据）
    bool cancel_running = true;
};

class TdService {
public:
    // 每个 runner 只服务一轮（真实 TD 会话不可复用），所以是"工厂"而不是实例。
    using RunnerFactory = std::function<std::unique_ptr<TdCheckRunner>()>;
    // 结果回调：**只在未被取消的轮次**调用（线程 = worker 线程）。
    using ResultSink = std::function<void(const project::Project&, TdCheckResult)>;

    TdService(RunnerFactory factory, ResultSink sink, TdServiceOptions options = {});
    ~TdService();

    TdService(const TdService&) = delete;
    TdService& operator=(const TdService&) = delete;

    // 请求一轮检查（非阻塞）。连续调用会合并成一轮；在跑的一轮会被取消。
    // 返回本次请求的序号（单调递增，便于调用方确认"我收到的是不是最新那轮"）。
    uint64_t Request(project::Project project);

    // 取消在跑的一轮并丢弃待跑的一轮（例如 exit / 关工程）。
    void CancelActive();

    // 停止 worker 并 join。幂等；析构里也会调用。
    void Shutdown();

    struct Stats {
        uint64_t requests = 0;    // Request() 次数
        uint64_t runs = 0;        // 真正跑完并回调的轮次
        uint64_t cancelled = 0;   // 被打断的轮次（kill 进程树）
        uint64_t superseded = 0;  // 跑完了但已被更新请求取代（结果丢弃，不发布）
        uint64_t coalesced = 0;   // 被去抖/合并掉的请求数（pending 被覆盖）
        double last_total_ms = 0;     // 最近一轮 td 内部耗时（timings.total_ms）
        double last_queue_ms = 0;     // 最近一轮从 Request 到开跑的等待
    };
    Stats stats() const;

private:
    struct Pending {
        project::Project project;
        uint64_t seq = 0;
        std::chrono::steady_clock::time_point requested_at;  // 最后一次请求时刻
        std::chrono::steady_clock::time_point ready_at;      // requested_at + debounce
    };

    void Loop();

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    uint64_t next_seq_ = 0;
    std::optional<Pending> pending_;
    std::shared_ptr<CancelToken> active_;
    std::thread worker_;

    RunnerFactory factory_;
    ResultSink sink_;
    TdServiceOptions options_;

    mutable std::mutex stats_mutex_;
    Stats stats_;
};

}  // namespace fpgalsp::td