// =============================================================================
//  src/server/async_dispatcher.h —— LSP 请求的异步调度（Phase 2 / A2）
// =============================================================================
//  为什么需要：
//    Phase B 的 dispatch 是同步的：一条 fpgaLsp/reports / runAnalysis / runFlow
//    会把整个 stdin 读循环卡住，后续 hover/completion/didChange 全部排队。
//    本层把「重请求」放到固定大小 worker 池，快请求与通知仍在 IO 线程，
//    从而消除队头阻塞，并支持 LSP 3.17 的 $/cancelRequest。
//
//  线程模型：
//    * IO 线程：ReadMessage -> Dispatch()。通知（didOpen/didChange/...）永远同步执行，
//      保证文档状态顺序；快请求也同步（它们本身就是亚毫秒级）。
//    * Worker 池（默认 4，clamp 1..8）：只执行 IsHeavyMethod() 白名单里的请求；响应直接写 transport
//      （TransportBaseCommon::WriteMessage 内部持锁，线程安全）。
//    * 取消：$/cancelRequest 只置 token；worker 在调用前检查；已经开始的请求在
//      返回后若已取消，则回 -32800 而不是结果。
//
//  约束：heavy 白名单里的 handler 必须是「只读快照 / 自带锁」的方法。当前包括
//  reports、analysis、flow、program、device 等；索引类方法要等 A1 的不可变快照
//  接入后再迁入，避免与 State 线程的索引更新竞争。
// =============================================================================

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "jsonrpc/dispatcher.h"
#include "jsonrpc/json_util.h"
#include "jsonrpc/transport.h"

namespace fpgalsp::server {

// 是否属于「重请求」白名单（见文件头约束）。
bool IsHeavyMethod(std::string_view method);

struct AsyncMetricsSnapshot {
    std::uint64_t queued = 0;
    std::uint64_t completed = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t failed = 0;
    std::uint64_t slow = 0;
    std::uint64_t inflight = 0;
    double max_queue_wait_ms = 0.0;
    double max_run_ms = 0.0;
};

// 进程级单例计数器（fpgalsp.metrics 用）。
AsyncMetricsSnapshot GetAsyncMetrics();

class AsyncDispatcher {
public:
    AsyncDispatcher(jsonrpc::Dispatcher& inner, jsonrpc::TransportBaseCommon& transport,
                    int workers = 2);
    ~AsyncDispatcher();

    AsyncDispatcher(const AsyncDispatcher&) = delete;
    AsyncDispatcher& operator=(const AsyncDispatcher&) = delete;

    // 与 Dispatcher::Dispatch 同签名；重请求异步、其余同步。
    bool Dispatch(const jsonrpc::Json& message);

    // 停止接收新任务并 join 所有 worker（main 在销毁 transport/server 前调用）。
    void Shutdown();

private:
    struct Task {
        std::string id_key;
        jsonrpc::MessageId id;
        std::string method;
        jsonrpc::Json params;
        std::shared_ptr<std::atomic<bool>> cancelled;
        std::chrono::steady_clock::time_point enqueued;
    };

    void WorkerLoop();
    void HandleCancelRequest(const jsonrpc::Json& params);
    void WriteResult(const Task& task, const jsonrpc::HandlerResult& result);
    void WriteError(const Task& task, int code, const std::string& message);

    jsonrpc::Dispatcher& inner_;
    jsonrpc::TransportBaseCommon& transport_;
    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Task> queue_;
    std::unordered_map<std::string, std::shared_ptr<std::atomic<bool>>> inflight_;
    bool stopping_ = false;
};

}  // namespace fpgalsp::server