// =============================================================================
//  src/flow/hardware_lease.h —— 进程级"谁在占 JTAG"的租约
// =============================================================================
//  为什么需要：一次烧录（programmer.cpp）与一次探针采集（probe.cpp）都会起
//  bw/td 进程去驱动同一根下载线。两个同时跑的下场是随机的：轻则命令互相打断，
//  重则 bit 下载到一半被另一个进程抢总线。
//
//  设计刻意保持极简：
//    * 进程内（不是跨进程）—— 同一台机器上开两个后端进程仍然会撞，这是已知边界；
//    * 非阻塞 try_acquire —— 拿不到立刻拒绝，**不排队**（排队会让 UI 卡住且掩盖问题）；
//    * 拒绝形状复用 flow::Refusal 的字段（ok/refused/stage/reason），客户端已有识别逻辑。
//
//  只读操作（list_cable / read_device_id）**不取租约** —— 它们不改变硬件状态，
//  而且预检时如果还要抢锁，用户体验会变成"点一下没反应"。
// =============================================================================

#pragma once

#include <mutex>
#include <string>
#include <string_view>

namespace fpgalsp::flow {

class HardwareLease {
public:
    // 成功返回 true 并记下 owner；已被占用返回 false（owner 不变）。
    static bool TryAcquire(std::string_view owner) {
        std::lock_guard<std::mutex> lock(Mutex());
        if (!OwnerRef().empty()) return false;
        OwnerRef().assign(owner);
        return true;
    }

    static void Release() {
        std::lock_guard<std::mutex> lock(Mutex());
        OwnerRef().clear();
    }

    static std::string Owner() {
        std::lock_guard<std::mutex> lock(Mutex());
        return OwnerRef();
    }

private:
    static std::mutex& Mutex() {
        static std::mutex mutex;
        return mutex;
    }
    static std::string& OwnerRef() {
        static std::string owner;
        return owner;
    }
};

// RAII：构造时抢，析构时放。held() 为 false 时调用方必须拒绝。
class HardwareLeaseGuard {
public:
    explicit HardwareLeaseGuard(std::string_view owner) : held_(HardwareLease::TryAcquire(owner)) {}
    ~HardwareLeaseGuard() {
        if (held_) HardwareLease::Release();
    }

    HardwareLeaseGuard(const HardwareLeaseGuard&) = delete;
    HardwareLeaseGuard& operator=(const HardwareLeaseGuard&) = delete;

    bool held() const { return held_; }

private:
    bool held_ = false;
};

}  // namespace fpgalsp::flow