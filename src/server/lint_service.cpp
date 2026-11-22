// =============================================================================
//  src/server/lint_service.cpp —— 服务端 lint 的后台调度实现（T1 / task-50）
// =============================================================================

#include "server/lint_service.h"

#include <algorithm>
#include <utility>

namespace fpgalsp::server {

LintService::LintService(Runner runner, Sink sink, LintServiceOptions options)
    : runner_(std::move(runner)), sink_(std::move(sink)), options_(options) {
    worker_ = std::thread([this] { Loop(); });
}

LintService::~LintService() { Shutdown(); }

uint64_t LintService::Request(const std::string& path, std::string uri) {
    uint64_t generation = 0;
    bool coalesced = false;
    {
        std::scoped_lock lock(mutex_);
        if (stop_) return 0;
        generation = ++next_generation_;
        latest_generation_[path] = generation;
        const auto now = std::chrono::steady_clock::now();
        const auto ready_at = now + std::chrono::milliseconds(std::max(0, options_.debounce_ms));
        if (pending_.contains(path)) coalesced = true;
        pending_[path] = Pending{.uri=std::move(uri), .generation=generation, .requested_at=now, .ready_at=ready_at};
        {
            std::scoped_lock stats_lock(stats_mutex_);
            ++stats_.requests;
            if (coalesced) ++stats_.coalesced;
        }
    }
    cv_.notify_all();
    return generation;
}

void LintService::Cancel(const std::string& path) {
    {
        std::scoped_lock lock(mutex_);
        pending_.erase(path);
        // 让在跑的那一轮作废（结果算完会被丢弃，不发布）
        latest_generation_[path] = ++next_generation_;
    }
    cv_.notify_all();
}

void LintService::Shutdown() {
    {
        std::scoped_lock lock(mutex_);
        if (stop_ && !worker_.joinable()) return;
        stop_ = true;
        pending_.clear();
    }
    cv_.notify_all();
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
        worker_.join();
    }
}

LintService::Stats LintService::stats() const {
    std::scoped_lock lock(stats_mutex_);
    return stats_;
}

void LintService::Loop() {
    for (;;) {
        std::string path;
        std::string uri;
        uint64_t generation = 0;
        double queue_ms = 0.0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !pending_.empty(); });
            if (stop_) return;

            bool have_job = false;
            while (!stop_ && !have_job) {
                if (pending_.empty()) break;  // 全部被 Cancel
                auto best = pending_.begin();
                for (auto it = pending_.begin(); it != pending_.end(); ++it) {
                    if (it->second.ready_at < best->second.ready_at) best = it;
                }
                if (cv_.wait_until(lock, best->second.ready_at) == std::cv_status::timeout) {
                    path = best->first;
                    uri = best->second.uri;
                    generation = best->second.generation;
                    // queue 时间 = 最后一次请求 -> 真正开跑
                    queue_ms = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - best->second.requested_at)
                                   .count();
                    pending_.erase(best);
                    have_job = true;
                }
                // 被新请求唤醒：重新挑最早 ready 的那个（新请求会顺延 ready_at）
            }
            if (stop_) return;
            if (!have_job) continue;
        }

        const auto started = std::chrono::steady_clock::now();
        lsp::DiagnosticList diagnostics;
        try {
            if (runner_) diagnostics = runner_(path, uri);
        } catch (...) {
            // runner 自己应该 never-raise；这里兜底，本轮当作空结果（不发布错误诊断）
            diagnostics.clear();
        }
        const double elapsed =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();

        bool publish = false;
        {
            std::scoped_lock lock(mutex_);
            const auto it = latest_generation_.find(path);
            // 只有"这个文件的最新代次仍是这一轮"才发布：期间又编辑 / didClose 都会让代次前进
            publish = !stop_ && it != latest_generation_.end() && it->second == generation;
        }
        {
            std::scoped_lock stats_lock(stats_mutex_);
            stats_.last_ms = elapsed;
            stats_.last_queue_ms = queue_ms;
            if (publish) {
                ++stats_.runs;
            } else {
                ++stats_.superseded;
            }
        }
        if (publish && sink_) sink_(path, uri, std::move(diagnostics));
    }
}

}  // namespace fpgalsp::server