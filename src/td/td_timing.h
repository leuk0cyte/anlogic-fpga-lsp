// =============================================================================
//  src/td/td_timing.h —— TD 检查的分段计时（A5 / task-25）
// =============================================================================
//  目的：把"一次 fpgalsp.check 到底慢在哪"变成可观测数字，而不是靠猜。
//
//  分段与真实代码的对应关系：
//      startup_ms   TdSession 构造：CreateProcessW + 读到 "td > " 横幅
//      import_ms    ensure_device：import_device（可能试多个 db 候选）
//      read_hdl_ms  read_hdl 一条命令（TD 真正解析 HDL 的时间）
//      parse_ms     输出文本 -> lsp::Diagnostic（消息映射 + 路径解析）
//      total_ms     以上四段 + 其余收尾（结果分表等）
//
//  开关：默认开启（4 次 steady_clock::now()，纳秒级，不值得做成纯编译期开关）。
//  需要绝对零开销或要对比"有/无计时"时用 SetTimingEnabled(false)；
//  测试里也可以用它验证"关掉后不填数字"。
// =============================================================================

#pragma once

#include <atomic>
#include <chrono>

namespace fpgalsp::td {

struct TdPhaseTimings {
    double startup_ms = 0.0;    // 进程启动 + 横幅
    double import_ms = 0.0;     // import_device（多候选时是总和）
    double read_hdl_ms = 0.0;   // read_hdl 本身
    double parse_ms = 0.0;      // 输出 -> Diagnostic 列表
    double total_ms = 0.0;      // 整轮（含未归类的收尾）
    int db_candidates = 0;      // 本轮试过的 db 候选数
    bool reused_session = false;  // 会话是否来自预热（A5 预热评估用）

    // 四段之和（用于核对 total 的"其余"部分）
    [[nodiscard]] double accounted_ms() const {
        return startup_ms + import_ms + read_hdl_ms + parse_ms;
    }
    [[nodiscard]] double other_ms() const { return total_ms > accounted_ms() ? total_ms - accounted_ms() : 0.0; }
};

namespace detail {
// 全局开关：默认开。用 relaxed 原子，读侧成本可忽略。
inline std::atomic<bool>& timing_enabled_flag() {
    static std::atomic<bool> enabled{true};
    return enabled;
}
}  // namespace detail

inline bool IsTimingEnabled() { return detail::timing_enabled_flag().load(std::memory_order_relaxed); }
inline void SetTimingEnabled(bool enabled) {
    detail::timing_enabled_flag().store(enabled, std::memory_order_relaxed);
}

// 纳秒级秒表；禁用时空转（不读时钟）。
class PhaseStopwatch {
public:
    PhaseStopwatch() { Restart(); }
    void Restart() {
        active_ = IsTimingEnabled();
        if (active_) start_ = std::chrono::steady_clock::now();
    }
    // 返回自 Restart() 起的毫秒数，并重新开始计时（便于连续打点）
    double LapMs() {
        if (!active_) return 0.0;
        const auto now = std::chrono::steady_clock::now();
        const double ms =
            std::chrono::duration<double, std::milli>(now - start_).count();
        start_ = now;
        return ms;
    }
    // 只读不重置
    [[nodiscard]] double ElapsedMs() const {
        if (!active_) return 0.0;
        const auto now = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::milli>(now - start_).count();
    }

private:
    std::chrono::steady_clock::time_point start_;
    bool active_ = false;
};

}  // namespace fpgalsp::td