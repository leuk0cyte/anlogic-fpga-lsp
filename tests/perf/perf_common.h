// =============================================================================
//  tests/perf/perf_common.h —— 性能探针的公共小工具
// =============================================================================
//  用法：
//      #include "perf_common.h"
//      int main() {
//          perf::Banner("index");                       // 打印机器/构建环境
//          auto t = perf::Bench("ParseAl(200)", 50, [&]{ ...; });
//          t.Print();                                   // p50/p95/max
//      }
//
//  设计取向（别在这里加花活）：
//    * 只依赖标准库；探针不注册成 ctest，纯人工/团队测量用。
//    * 计时用 steady_clock + 纳秒；样本先排序再算分位数（线性插值，与
//      tools/audit/bench_servers.py 的 percentile() 同定义，便于对齐）。
//    * 不做预热策略规定，但默认丢第一轮（warmup 参数）。
// =============================================================================
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
// windows.h 的 min/max 宏会把 std::min/std::max 打坏（C2589），必须提前压住。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

// 即使有人抢先包含了 windows.h，也用带括号的调用形式兜底。
#if defined(min) || defined(max)
#error "perf_common.h 必须在 windows.h 之前被包含（或定义 NOMINMAX）"
#endif

namespace perf {

using Clock = std::chrono::steady_clock;

inline double MsSince(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// 线性插值分位数，q∈[0,1]；sorted 必须已升序。
inline double Percentile(const std::vector<double>& sorted, double q) {
  if (sorted.empty()) return 0.0;
  if (sorted.size() == 1) return sorted.front();
  const double pos = q * static_cast<double>(sorted.size() - 1);
  const size_t lo = static_cast<size_t>(pos);
  const size_t hi = std::min(lo + 1, sorted.size() - 1);
  const double frac = pos - static_cast<double>(lo);
  return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

struct Stats {
  std::string label;
  std::vector<double> samples_ms;  // 已升序
  size_t warmup = 0;

  double Min() const { return samples_ms.empty() ? 0.0 : samples_ms.front(); }
  double Max() const { return samples_ms.empty() ? 0.0 : samples_ms.back(); }
  double P50() const { return Percentile(samples_ms, 0.50); }
  double P95() const { return Percentile(samples_ms, 0.95); }
  double Mean() const {
    if (samples_ms.empty()) return 0.0;
    double s = 0.0;
    for (double v : samples_ms) s += v;
    return s / static_cast<double>(samples_ms.size());
  }
  double Sum() const {
    double s = 0.0;
    for (double v : samples_ms) s += v;
    return s;
  }
  size_t N() const { return samples_ms.size(); }

  void Print() const {
    std::printf("%-34s n=%-4zu p50=%8.3f ms  p95=%8.3f ms  max=%8.3f ms  mean=%8.3f\n",
                label.c_str(), N(), P50(), P95(), Max(), Mean());
  }
  // 单行机器可读（便于脚本 grep）
  void PrintCsv() const {
    std::printf("csv,%s,%zu,%.4f,%.4f,%.4f,%.4f\n", label.c_str(), N(), P50(), P95(),
                Max(), Mean());
  }
};

// 跑 iters 次（前 warmup 次不计），返回统计量。fn 的返回值被忽略。
inline Stats Bench(std::string label, int iters, const std::function<void()>& fn,
                   int warmup = 3) {
  Stats st;
  st.label = std::move(label);
  st.warmup = static_cast<size_t>(std::max(0, warmup));
  st.samples_ms.reserve(static_cast<size_t>(std::max(0, iters)));
  for (int i = 0; i < warmup; ++i) fn();
  for (int i = 0; i < iters; ++i) {
    const auto t0 = Clock::now();
    fn();
    st.samples_ms.push_back(MsSince(t0));
  }
  std::sort(st.samples_ms.begin(), st.samples_ms.end());
  return st;
}

// 累加阶段的简单账本：把一次请求拆成若干阶段，最后按阶段打印占比。
class PhaseLedger {
 public:
  void Add(std::string_view phase, double ms) {
    for (auto& e : entries_) {
      if (e.first == phase) {
        e.second += ms;
        return;
      }
    }
    entries_.emplace_back(std::string(phase), ms);
  }
  void Print(std::string_view title) const {
    double total = 0.0;
    for (const auto& e : entries_) total += e.second;
    std::printf("--- %s (total %.3f ms) ---\n", std::string(title).c_str(), total);
    for (const auto& e : entries_) {
      std::printf("  %-28s %9.3f ms  %5.1f%%\n", e.first.c_str(), e.second,
                  total > 0 ? 100.0 * e.second / total : 0.0);
    }
  }
  void Clear() { entries_.clear(); }

 private:
  std::vector<std::pair<std::string, double>> entries_;
};

inline size_t CurrentWorkingSetBytes() {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  if (GetProcessMemoryInfo(GetCurrentProcess(),
                           reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                           sizeof(pmc))) {
    return static_cast<size_t>(pmc.WorkingSetSize);
  }
#endif
  return 0;
}

inline size_t PeakWorkingSetBytes() {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  if (GetProcessMemoryInfo(GetCurrentProcess(),
                           reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                           sizeof(pmc))) {
    return static_cast<size_t>(pmc.PeakWorkingSetSize);
  }
#endif
  return 0;
}

inline double Mb(size_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

inline void Banner(std::string_view area) {
  std::printf("=== perf probe: %s ===\n", std::string(area).c_str());
#if defined(_WIN32)
  SYSTEM_INFO si{};
  GetSystemInfo(&si);
  std::printf("machine: %lu logical cores\n", GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
#endif
  std::printf("build: %s %s\n", __DATE__, __TIME__);
#if defined(_MSC_VER)
  std::printf("compiler: MSVC %d\n", _MSC_VER);
#endif
  std::printf("cwd: %s\n", std::filesystem::current_path().string().c_str());
  std::printf("\n");
}

inline std::string ReadFile(const std::filesystem::path& p) {
  std::FILE* f = nullptr;
  if (_wfopen_s(&f, p.wstring().c_str(), L"rb") != 0 || f == nullptr) return {};
  std::fseek(f, 0, SEEK_END);
  const long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::string out;
  if (n > 0) {
    out.resize(static_cast<size_t>(n));
    const size_t got = std::fread(out.data(), 1, static_cast<size_t>(n), f);
    out.resize(got);
  }
  std::fclose(f);
  return out;
}

// 让 main 支持 `--iters N --warmup N`，省得每个探针各写一遍参数解析。
struct Cli {
  int iters = 30;
  int warmup = 3;
  std::string extra;  // 剩余位置参数
};

inline Cli ParseCli(int argc, char** argv) {
  Cli c;
  for (int i = 1; i < argc; ++i) {
    std::string_view a = argv[i];
    if (a == "--iters" && i + 1 < argc) {
      c.iters = std::atoi(argv[++i]);
    } else if (a == "--warmup" && i + 1 < argc) {
      c.warmup = std::atoi(argv[++i]);
    } else if (!c.extra.empty()) {
      c.extra += ' ';
      c.extra += a;
    } else {
      c.extra = a;
    }
  }
  return c;
}

}  // namespace perf