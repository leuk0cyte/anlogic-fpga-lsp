// =============================================================================
//  src/server/async_dispatcher.cpp —— A2：异步重请求 + 取消 + 指标
// =============================================================================

#include "server/async_dispatcher.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <utility>

#include "platform/log.h"

#include <array>
namespace fpgalsp::server {
namespace {

std::atomic<std::uint64_t> g_queued{0};
std::atomic<std::uint64_t> g_completed{0};
std::atomic<std::uint64_t> g_cancelled{0};
std::atomic<std::uint64_t> g_failed{0};
std::atomic<std::uint64_t> g_slow{0};
std::atomic<std::uint64_t> g_inflight{0};
std::atomic<double> g_max_queue_wait_ms{0.0};
std::atomic<double> g_max_run_ms{0.0};

constexpr double kSlowRequestMs = 50.0;

double ElapsedMs(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}

void UpdateMax(std::atomic<double>& target, double value) {
    double current = target.load(std::memory_order_relaxed);
    while (value > current &&
           !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

}  // namespace

bool IsHeavyMethod(std::string_view method) {
    // 只列「不依赖可变索引快照 / 自带锁」的方法；索引类方法等 A1 接入后再迁入。
    static constexpr auto kHeavy = std::to_array<const char*>({
        "fpgaLsp/reports",
        // P-C6（M1）：索引类方法基于不可变快照（IndexService::Get 内部已加锁），
        // 以前留在 IO 线程上，冷索引时会和 didChange 抢同一个线程。
        // 迁入 worker 后 IO 线程只负责收发，语义不变（响应字节逐字节相同）。
        "fpgaLsp/moduleTree",
        "textDocument/semanticTokens/full",
        "textDocument/semanticTokens/full/delta",
        "fpgalsp.runAnalysis",
        "fpgalsp.updateProject",
        "fpgalsp.runFlow",
        "fpgalsp.cancelFlow",
        "fpgalsp.compareStrategies",
        "fpgalsp.program",
        "fpgalsp.svfgen",
        "fpgalsp.cables",
        "fpgalsp.device",
        "fpgalsp.reports",
        // F1/R0：codeAction 要跑内嵌 Verible 的全量 parse+lint（实测 13 KB=43 ms、
        // 100 KB=353 ms），以前和 didChange 一起挤在 IO 线程上 —— 实测把按键的
        // didChange→publishDiagnostics p95 从 1 ms 拖到 343 ms。移到 worker 后
        // IO 线程只负责收发，响应字节不变（F1 报告有 before/after 协议快照 diff）。
        "textDocument/codeAction",
    });
    return std::ranges::any_of(kHeavy,
                               [method](const char* name) { return method == name; });
}

namespace {

// executeCommand 的真正重活在 params.command 里：必须按命令名分类，
// 否则 `workspace/executeCommand` 会被当成快请求在 IO 线程同步执行，
// 重新引入队头阻塞（实测 device 会卡住 hover ~477 ms）。
bool IsHeavyRequest(std::string_view method, const jsonrpc::Json& params) {
    if (IsHeavyMethod(method)) return true;
    if (method != "workspace/executeCommand") return false;
    return IsHeavyMethod(jsonrpc::GetString(params, "command"));
}

}  // namespace

AsyncMetricsSnapshot GetAsyncMetrics() {
    AsyncMetricsSnapshot snapshot;
    snapshot.queued = g_queued.load();
    snapshot.completed = g_completed.load();
    snapshot.cancelled = g_cancelled.load();
    snapshot.failed = g_failed.load();
    snapshot.slow = g_slow.load();
    snapshot.inflight = g_inflight.load();
    snapshot.max_queue_wait_ms = g_max_queue_wait_ms.load();
    snapshot.max_run_ms = g_max_run_ms.load();
    return snapshot;
}

AsyncDispatcher::AsyncDispatcher(jsonrpc::Dispatcher& inner,
                                 jsonrpc::TransportBaseCommon& transport, int workers)
    : inner_(inner), transport_(transport) {
    const int count = std::clamp(workers, 1, 8);
    workers_.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        workers_.emplace_back([this]() { WorkerLoop(); });
    }
}

AsyncDispatcher::~AsyncDispatcher() { Shutdown(); }

void AsyncDispatcher::Shutdown() {
    {
        std::scoped_lock lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
        for (auto& [key, token] : inflight_) token->store(true);
        queue_.clear();
    }
    cv_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
}

bool AsyncDispatcher::Dispatch(const jsonrpc::Json& message) {
    if (!message.is_object() || !message.contains("method")) {
        return inner_.Dispatch(message);
    }
    const std::string method = jsonrpc::GetString(message, "method");
    if (method == "$/cancelRequest") {
        HandleCancelRequest(jsonrpc::GetObject(message, "params"));
        return false;
    }

    const auto id = jsonrpc::GetMessageId(message);
    const jsonrpc::Json params = jsonrpc::GetObject(message, "params");
    if (!IsHeavyRequest(method, params) || !id.has_value()) {
        return inner_.Dispatch(message);
    }

    Task task;
    task.id = *id;
    task.id_key = id->ToJson().dump();
    task.method = method;
    task.params = params;
    task.cancelled = std::make_shared<std::atomic<bool>>(false);
    task.enqueued = std::chrono::steady_clock::now();

    {
        std::scoped_lock lock(mutex_);
        if (stopping_) {
            WriteError(task, jsonrpc::kInternalError, "server is shutting down");
            return true;
        }
        inflight_[task.id_key] = task.cancelled;
        queue_.push_back(std::move(task));
    }
    g_queued.fetch_add(1);
    g_inflight.fetch_add(1);
    cv_.notify_one();
    return true;
}

void AsyncDispatcher::HandleCancelRequest(const jsonrpc::Json& params) {
    if (!params.is_object() || !params.contains("id")) return;
    const std::string key = params["id"].dump();
    std::scoped_lock lock(mutex_);
    const auto it = inflight_.find(key);
    if (it != inflight_.end()) it->second->store(true);
}

void AsyncDispatcher::WorkerLoop() {
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() { return stopping_ || !queue_.empty(); });
            if (stopping_ && queue_.empty()) return;
            task = std::move(queue_.front());
            queue_.pop_front();
        }

        const double queue_ms = ElapsedMs(task.enqueued);
        UpdateMax(g_max_queue_wait_ms, queue_ms);

        if (task.cancelled->load()) {
            WriteError(task, jsonrpc::kRequestCancelled, "Request cancelled");
            g_cancelled.fetch_add(1);
            g_inflight.fetch_sub(1);
            {
                std::scoped_lock lock(mutex_);
                inflight_.erase(task.id_key);
            }
            continue;
        }

        const auto started = std::chrono::steady_clock::now();
        jsonrpc::HandlerResult result;
        try {
            const auto invoked = inner_.Invoke(task.method, task.params);
            if (!invoked.has_value()) {
                WriteError(task, jsonrpc::kMethodNotFound,
                           "unsupported method: " + task.method);
                g_failed.fetch_add(1);
                g_inflight.fetch_sub(1);
                std::scoped_lock lock(mutex_);
                inflight_.erase(task.id_key);
                continue;
            }
            result = *invoked;
        } catch (const std::exception& exc) {
            platform::default_logger().error("async handler " + task.method +
                                             " crashed: " + exc.what());
            WriteError(task, jsonrpc::kInternalError,
                       std::string("exception: ") + exc.what());
            g_failed.fetch_add(1);
            g_inflight.fetch_sub(1);
            std::scoped_lock lock(mutex_);
            inflight_.erase(task.id_key);
            continue;
        } catch (...) {
            WriteError(task, jsonrpc::kInternalError, "exception: unknown");
            g_failed.fetch_add(1);
            g_inflight.fetch_sub(1);
            std::scoped_lock lock(mutex_);
            inflight_.erase(task.id_key);
            continue;
        }

        const double run_ms = ElapsedMs(started);
        UpdateMax(g_max_run_ms, run_ms);
        if (run_ms > kSlowRequestMs) {
            g_slow.fetch_add(1);
            platform::default_logger().info("slow request " + task.method + " " +
                                            std::to_string(static_cast<int>(run_ms)) + " ms");
        }

        if (task.cancelled->load()) {
            WriteError(task, jsonrpc::kRequestCancelled, "Request cancelled");
            g_cancelled.fetch_add(1);
        } else {
            WriteResult(task, result);
            g_completed.fetch_add(1);
        }
        g_inflight.fetch_sub(1);
        {
            std::scoped_lock lock(mutex_);
            inflight_.erase(task.id_key);
        }
    }
}

void AsyncDispatcher::WriteResult(const Task& task, const jsonrpc::HandlerResult& result) {
    if (result.is_error) {
        transport_.WriteMessage(jsonrpc::MakeError(task.id, result.error_code,
                                                    result.error_message, result.error_data));
    } else {
        transport_.WriteMessage(jsonrpc::MakeResult(task.id, result.result));
    }
}

void AsyncDispatcher::WriteError(const Task& task, int code, const std::string& message) {
    transport_.WriteMessage(jsonrpc::MakeError(task.id, code, message));
}

}  // namespace fpgalsp::server