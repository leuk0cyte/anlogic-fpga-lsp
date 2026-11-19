// =============================================================================
//  src/td/td_cancel.h —— TD 检查的取消令牌（A5 / task-25）
// =============================================================================
//  为什么要"令牌"而不是简单的 bool：
//    一个 TD 检查有两个可中断点，性质完全不同：
//      1) **协作点**：import_device 之前 / read_hdl 之后 —— 检查一下 cancelled()
//         就能立刻放弃，不需要任何系统调用；
//      2) **阻塞点**：read_hdl 正在跑（单条命令最长 300 s），此时唯一的出路是
//         杀掉 TD 进程树（TdSession::Kill → Job Object）。协作点永远等不到。
//
//  所以令牌同时携带一个"打断动作"（kill hook）：运行中的检查把自己当前会话的
//  Kill 注册进来，任何线程调 Cancel() 就会立刻终止该进程树 —— TdSession::ReadUntil
//  随即抛 TdError，RunReadHdl 的 never-raise 外壳接住并把本轮标记为 cancelled。
//
//  线程安全：Cancel() 可在任意线程调用；重复调用幂等（hook 只跑一次）。
//  ⚠ kill hook 在 Cancel() 的调用线程上执行（同步），所以它必须是非阻塞的
//   —— TdSession::Kill 只是 TerminateJobObject/TerminateProcess，符合要求。
// =============================================================================

#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <utility>

namespace fpgalsp::td {

class CancelToken {
public:
    CancelToken() = default;
    CancelToken(const CancelToken&) = delete;
    CancelToken& operator=(const CancelToken&) = delete;

    // 请求取消。先置位（保证协作点一定能看到），再跑 kill hook（保证阻塞点退出）。
    // 幂等：第二次调用不会重复执行 hook。
    void Cancel() {
        const bool was_cancelled = cancelled_.exchange(true, std::memory_order_acq_rel);
        if (was_cancelled) return;
        std::function<void()> hook;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            hook = hook_;
        }
        if (hook) hook();
    }

    bool cancelled() const { return cancelled_.load(std::memory_order_acquire); }

    // 注册/替换"如何打断当前会话"。运行中的检查在两端注册：
    //   开始前 SetKillHook([session]{ session->Kill(); })
    //   结束后   ClearKillHook()   —— 避免令牌活过会话生命周期
    //
    // 注意这两个方法是 **const** 的：令牌在检查执行期间以 `const CancelToken&`
    // 传下去（检查只应"读"取消状态），但注册/注销 kill hook 是令牌自身的同步
    // 状态变更，逻辑上不改变"令牌身份"，所以用 mutable 成员实现。
    void SetKillHook(std::function<void()> hook) const {
        std::function<void()> to_run;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // 已经在取消状态：立刻执行新 hook（保护"注册前就被取消"的竞态）
            if (cancelled_.load(std::memory_order_acquire)) {
                to_run = hook;
            } else {
                hook_ = std::move(hook);
            }
        }
        if (to_run) to_run();
    }

    void ClearKillHook() const {
        std::lock_guard<std::mutex> lock(mutex_);
        hook_ = nullptr;
    }

private:
    std::atomic<bool> cancelled_{false};
    mutable std::mutex mutex_;
    mutable std::function<void()> hook_;
};

}  // namespace fpgalsp::td