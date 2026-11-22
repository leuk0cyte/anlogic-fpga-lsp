// =============================================================================
//  src/server/lint_service.h —— 服务端 lint 的后台调度（T1 / task-50）
// =============================================================================
//  目标：`didChange` 之后用**内嵌 Verible** 在 worker 线程上重算 lint 诊断，
//        不阻塞 IO 线程（F1 的教训：一次同步 Verible 调用就能把 didChange 堵到 342 ms）。
//
//  策略（与 `td::TdService` 同构，但按**文件**而不是工程）：
//    * 每文件 250–300 ms 去抖：连续敲键只跑最后一次；
//    * 同一文件 single-flight：永远只有一个 worker 在算它；
//    * 新请求**取代**旧请求：正在算的那一轮算完直接丢弃（Verible 不能中途 kill，
//      但结果按"代次"作废，不会发布过期诊断）；
//    * `Cancel(path)`（didClose）会清 pending 并让在跑的轮次作废；
//    * `Shutdown()` join worker，且退出前**不发布**任何结果。
//
//  线程模型：
//    * `Request()` 只在 IO 线程被调用，只加锁 + notify（O(log n)，微秒级）；
//    * `Runner` 与 `Sink` 都在 **worker 线程**执行；Sink 里禁止再调 `Request()`（会自锁）。
// =============================================================================

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "lsp/diagnostic.h"

namespace fpgalsp::server {

struct LintServiceOptions {
    // 最后一次编辑之后静默这么久才开跑。300 ms 与 TdService 一致；
    // 人类连续敲键的间隔通常 <200 ms，所以"停下来"时才真正算。
    int debounce_ms = 300;
};

class LintService {
public:
    // worker 线程上执行：给定 path/uri，返回该文件的 lint 诊断（已转成 LSP 形状）。
    using Runner = std::function<lsp::DiagnosticList(const std::string& path,
                                                     const std::string& uri)>;
    // worker 线程上执行：一轮有效结果（未被取消/取代）的回调。
    using Sink = std::function<void(const std::string& path, const std::string& uri,
                                    lsp::DiagnosticList diagnostics)>;

    LintService(Runner runner, Sink sink, LintServiceOptions options = {});
    ~LintService();

    LintService(const LintService&) = delete;
    LintService& operator=(const LintService&) = delete;

    // 请求重算一个**已打开**文档的 lint（非阻塞）。返回该文件的请求代号。
    uint64_t Request(const std::string& path, std::string uri);

    // 放弃某文件（didClose）：清掉 pending，并让在跑的轮次结果作废。
    void Cancel(const std::string& path);

    // 停止 worker 并 join（幂等）。之后不再有任何 Sink 调用。
    void Shutdown();

    struct Stats {
        uint64_t requests = 0;    // Request() 次数
        uint64_t runs = 0;        // 真正跑完并回调的轮次
        uint64_t superseded = 0;  // 跑完但已被新请求/取消取代（结果丢弃）
        uint64_t coalesced = 0;   // 去抖窗口内被合并掉的请求
        double last_ms = 0.0;     // 最近一轮 Runner 的耗时
        double last_queue_ms = 0.0;  // 最近一轮从"最后一次请求"到开跑
    };
    Stats stats() const;

private:
    struct Pending {
        std::string uri;
        uint64_t generation = 0;
        std::chrono::steady_clock::time_point requested_at;
        std::chrono::steady_clock::time_point ready_at;
    };

    void Loop();

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    uint64_t next_generation_ = 0;
    std::map<std::string, Pending> pending_;
    // 每文件"最新代次"：Request/Cancel 都 +1；在跑的轮次结果只有代次仍相等才发布。
    std::map<std::string, uint64_t> latest_generation_;
    std::thread worker_;

    Runner runner_;
    Sink sink_;
    LintServiceOptions options_;

    mutable std::mutex stats_mutex_;
    Stats stats_;
};

}  // namespace fpgalsp::server