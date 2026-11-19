// =============================================================================
//  tools/perf/profiler/sampler.cpp —— 无提权的用户态采样剖析器（perf-profiler）
// =============================================================================
//  为什么需要它：本会话是 Medium integrity（未提权），xperf/wpr 的 CPU 采样走
//  NT Kernel Logger（需要 SeSystemProfilePrivilege，只给管理员），实测：
//      xperf -on PROC_THREAD+LOADER+PROFILE  -> NT Kernel Logger: 无效标志 (0x3ec)
//      wpr   -start CPU                      -> Failed to enable the policy ...
//  所以这里用**用户态**手段做"函数级采样"：
//
//      定期 SuspendThread 目标进程的每个线程
//        -> GetThreadContext / StackWalk64（dbghelp）只收集**返回地址**
//        -> 按 GetThreadTimes 的 CPU 增量加权（睡眠/等待中的线程 delta=0，直接跳过，
//           否则样本会退化成 wall-clock）
//        -> 采样结束后**一次性**解析符号（第一版在采样循环里逐帧 SymFromAddr，
//           单轮 157 ms、每轮只有 1 个样本，attribution 完全失真 —— 实测踩过）
//
//  产出（CSV，见 --out）：
//      functions.csv     module,function,self_ms,inclusive_ms,self_samples,inclusive_samples
//      threads.csv       tid,thread_name,user_ms,kernel_ms,samples,sweeps,wait_streak,kernel_pct
//      sweeps.csv        sweep,ts_us,sweep_ms,threads_seen,threads_running,sampled_ms
//      meta.csv          key,value（含 stall_events / sweeps / sampled_cpu_ms）
//      samples_head.csv  tid,depth,module,function,weight_ms（前 N 条原始样本，可审计）
//
//  安全（Lead 明确要求）：
//    * 只挂 OpenThread 成功的线程；每次 SuspendThread 成功后**必有一次** ResumeThread；
//    * 跳过自己（不同进程，天然不会采到自己）；
//    * 连续多轮 delta=0 的线程记入 wait_streak，报告里当作"等待线程"而不是热点；
//    * 单次 Suspend+栈走查超过 50 ms 记一次 stall_events（>0 的轮次整轮作废重跑）。
//
//  协议：stdin 读到 'stop' 或 EOF 即停止并落盘；--duration-ms 兜底。
// =============================================================================

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include <dbghelp.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

struct Options {
    DWORD pid = 0;
    std::string exe;
    std::string out_dir;
    int interval_ms = 2;
    int duration_ms = 0;
    int raw_head_limit = 20000;
    int max_depth = 14;
    int stall_ms = 50;
};

struct FuncKey {
    std::string module;
    std::string function;
    bool operator<(const FuncKey& o) const {
        return module < o.module || (module == o.module && function < o.function);
    }
};

struct FuncStat {
    double self_ms = 0.0;
    double inclusive_ms = 0.0;
    long long self_samples = 0;
    long long inclusive_samples = 0;
};

struct ThreadStat {
    DWORD tid = 0;
    std::string name;
    double user_ms = 0.0;
    double kernel_ms = 0.0;
    long long samples = 0;
    long long sweeps_seen = 0;
    long long wait_streak = 0;      // 连续 delta=0 的轮数（>阈值视为"纯等待线程"）
    long long wait_streak_max = 0;
    ULONGLONG last_user = 0;
    ULONGLONG last_kernel = 0;
    ULONGLONG last_cycles = 0;
    ULONGLONG cycles_total = 0;
    bool opened = false;
    HANDLE handle = nullptr;
};

struct StackSample {
    DWORD tid = 0;
    double weight_ms = 0.0;
    std::vector<ULONG64> pcs;
};

std::string WideToUtf8(const wchar_t* w) {
    if (w == nullptr) return {};
    const int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return {};
    std::string out(static_cast<size_t>(need - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), need - 1, nullptr, nullptr);
    return out;
}

using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
GetThreadDescriptionFn g_get_thread_description = nullptr;

std::string ThreadName(HANDLE h) {
    if (g_get_thread_description == nullptr || h == nullptr) return {};
    PWSTR raw = nullptr;
    if (FAILED(g_get_thread_description(h, &raw)) || raw == nullptr) return {};
    std::string name = WideToUtf8(raw);
    LocalFree(raw);
    return name;
}

double Ms(ULONGLONG hundred_ns) { return static_cast<double>(hundred_ns) / 10000.0; }

double NowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

BOOL CALLBACK ReadProcessMemoryCb(HANDLE process, DWORD64 address, PVOID buffer, DWORD size,
                                  LPDWORD bytes_read) {
    if (ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), buffer, size,
                          reinterpret_cast<SIZE_T*>(bytes_read))) {
        return TRUE;
    }
    if (bytes_read != nullptr) *bytes_read = 0;
    return FALSE;
}

std::vector<DWORD> ThreadsOf(DWORD pid) {
    std::vector<DWORD> tids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return tids;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (Thread32First(snap, &entry)) {
        do {
            if (entry.th32OwnerProcessID == pid) tids.push_back(entry.th32ThreadID);
        } while (Thread32Next(snap, &entry));
    }
    CloseHandle(snap);
    return tids;
}

void WriteCsv(const std::string& path, const char* header, const std::vector<std::string>& rows) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || f == nullptr) return;
    std::fprintf(f, "%s\r\n", header);
    for (const std::string& row : rows) std::fprintf(f, "%s\r\n", row.c_str());
    std::fclose(f);
}

std::string Fmt(const char* fmt, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    return std::string(buffer);
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "[sampler] missing value for %s\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--pid") opt.pid = static_cast<DWORD>(std::strtoul(next("--pid").c_str(), nullptr, 10));
        else if (a == "--exe") opt.exe = next("--exe");
        else if (a == "--out") opt.out_dir = next("--out");
        else if (a == "--interval-ms") opt.interval_ms = std::atoi(next("--interval-ms").c_str());
        else if (a == "--duration-ms") opt.duration_ms = std::atoi(next("--duration-ms").c_str());
        else if (a == "--raw-head") opt.raw_head_limit = std::atoi(next("--raw-head").c_str());
        else if (a == "--max-depth") opt.max_depth = std::atoi(next("--max-depth").c_str());
        else if (a == "--stall-ms") opt.stall_ms = std::atoi(next("--stall-ms").c_str());
        else if (a == "--help" || a == "-h") {
            std::printf("usage: sampler --pid N --exe PATH --out DIR [--interval-ms 2] [--duration-ms N]\n"
                        "  采样期间从 stdin 读到 'stop' 或 EOF 即停止\n");
            return 0;
        }
    }
    if (opt.pid == 0 || opt.out_dir.empty()) {
        std::fprintf(stderr, "[sampler] --pid and --out are required\n");
        return 2;
    }
    if (opt.interval_ms < 1) opt.interval_ms = 1;
    if (opt.max_depth < 8) opt.max_depth = 8;
    std::filesystem::create_directories(std::filesystem::path(opt.out_dir));
    // Sleep(1) 在默认 15.6 ms 定时器精度下会真的睡 15 ms（实测：sweep 本身 1 ms，但相邻
    // sweep 间隔 15 ms，采样率只有设计值的 1/7）。这里把精度提到 1 ms，退出时恢复。
    timeBeginPeriod(1);

    std::string sym_path = ".";
    {
        const size_t slash = opt.exe.find_last_of("\\/");
        if (slash != std::string::npos) sym_path = opt.exe.substr(0, slash);
    }
    // 不用 SYMOPT_DEFERRED_LOADS：延迟加载会让**第一次 StackWalk** 卡 ~1 s（加载 PDB），
    // 正好落在被测场景的窗口里（冷索引场景实测只有 32 个 sweep）。急切加载把这段
    // 开销挪到采样器启动阶段，配合下面的 ready 握手。
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);

    HANDLE process = OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_SUSPEND_RESUME, FALSE, opt.pid);
    if (process == nullptr) {
        std::fprintf(stderr, "[sampler] OpenProcess(%lu) failed: %lu\n", opt.pid, GetLastError());
        return 3;
    }
    if (!SymInitialize(process, sym_path.c_str(), TRUE)) {
        std::fprintf(stderr, "[sampler] SymInitialize failed: %lu\n", GetLastError());
    }
    SymRefreshModuleList(process);
    // 预热：解析一次已知地址，强制把主模块（fpgalsp-native.pdb）的符号表读进来
    {
        char warm_buffer[sizeof(SYMBOL_INFO) + 256] = {0};
        SYMBOL_INFO* warm = reinterpret_cast<SYMBOL_INFO*>(warm_buffer);
        warm->SizeOfStruct = sizeof(SYMBOL_INFO);
        warm->MaxNameLen = 255;
        DWORD64 displacement = 0;
        (void)SymFromAddr(process, reinterpret_cast<DWORD64>(&main), &displacement, warm);
    }
    std::printf("ready\n");
    std::fflush(stdout);

    g_get_thread_description = reinterpret_cast<GetThreadDescriptionFn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));

    std::map<DWORD, ThreadStat> threads;
    std::vector<StackSample> samples;
    samples.reserve(200000);
    std::vector<std::string> sweep_rows;

    const double started_ms = NowMs();
    long long sweep = 0;
    long long total_sampled_ms = 0;
    long long stall_events = 0;
    int threads_seen_max = 0;

    HANDLE stdin_handle = GetStdHandle(STD_INPUT_HANDLE);
    bool stop = false;
    std::vector<DWORD> tids;                 // 缓存的线程列表（定期刷新）
    double next_thread_refresh_ms = 0.0;

    while (!stop) {
        if (opt.duration_ms > 0 && (NowMs() - started_ms) > opt.duration_ms) break;

        if (stdin_handle != nullptr && stdin_handle != INVALID_HANDLE_VALUE) {
            DWORD available = 0;
            if (PeekNamedPipe(stdin_handle, nullptr, 0, nullptr, &available, nullptr) &&
                available > 0) {
                char buffer[64] = {0};
                DWORD read = 0;
                if (ReadFile(stdin_handle, buffer, sizeof(buffer) - 1, &read, nullptr) && read > 0) {
                    if (buffer[0] == 's' || buffer[0] == 'S' || buffer[0] == 0x1a) stop = true;
                }
            }
        }

        // ⚠ CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD) 是**全机**线程快照，实测单次
        //    ~95 ms（本机 192 核 + 多 agent 并发）—— 每轮都来一次会把采样周期从 3 ms
        //    拉到 100 ms。线程集合几乎不变，所以按 200 ms 刷新一次即可。
        if (tids.empty() || NowMs() >= next_thread_refresh_ms) {
            tids = ThreadsOf(opt.pid);
            next_thread_refresh_ms = NowMs() + 200.0;
        }
        if (tids.empty()) {
            Sleep(static_cast<DWORD>(opt.interval_ms));
            continue;
        }
        ++sweep;
        threads_seen_max = std::max<int>(threads_seen_max, static_cast<int>(tids.size()));
        const double sweep_started = NowMs();
        int running = 0;
        double swept_ms = 0.0;

        for (const DWORD tid : tids) {
            ThreadStat& stat = threads[tid];
            if (!stat.opened) {
                stat.tid = tid;
                stat.handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                             THREAD_QUERY_INFORMATION | THREAD_SET_LIMITED_INFORMATION,
                                         FALSE, tid);
                stat.opened = true;
                if (stat.handle != nullptr) stat.name = ThreadName(stat.handle);
            }
            HANDLE h = stat.handle;
            if (h == nullptr) continue;
            ++stat.sweeps_seen;

            FILETIME created{}, exited{}, kernel{}, user{};
            if (GetThreadTimes(h, &created, &exited, &kernel, &user)) {
                // 只用来做 user/kernel 拆分与**总量标定**：GetThreadTimes 的粒度是
                // 调度 tick（本机 15.6 ms），单独拿它做门控会让采样率只剩 ~64/s。
                const ULONGLONG k =
                    (static_cast<ULONGLONG>(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime;
                const ULONGLONG u =
                    (static_cast<ULONGLONG>(user.dwHighDateTime) << 32) | user.dwLowDateTime;
                stat.last_kernel = k;
                stat.last_user = u;
                stat.kernel_ms = Ms(k);
                stat.user_ms = Ms(u);
            }

            // 门控/权重用 QueryThreadCycleTime（TSC 级，粒度 ~µs）：这一拍真的在跑吗？
            ULONGLONG cycles = 0;
            ULONGLONG delta_cycles = 0;
            if (QueryThreadCycleTime(h, &cycles)) {
                if (cycles >= stat.last_cycles) delta_cycles = cycles - stat.last_cycles;
                stat.last_cycles = cycles;
                stat.cycles_total = cycles;
            }
            // 阈值：delta 太小说明这一拍几乎没跑（阻塞线程的 TSC 仍可能微增）。
            // 不设阈值会让每个空闲线程每轮都产出一个"近零权重"样本，把"样本占比"
            // 这一列变成 1/线程数 的无意义值（实测 b 场景 6 个线程各 1023 样本，
            // 而其中 4 个是 worker 池的等待线程）。
            const ULONGLONG min_cycles = static_cast<ULONGLONG>(opt.interval_ms) * 150000ULL;  // ~0.15 ms/MHz 量级
            if (delta_cycles < min_cycles) {
                ++stat.wait_streak;
                stat.wait_streak_max = std::max(stat.wait_streak_max, stat.wait_streak);
                continue;  // 睡眠/等待中的线程：不算样本（否则退化成 wall-clock）
            }
            stat.wait_streak = 0;

            const double t_suspend = NowMs();
            if (SuspendThread(h) == static_cast<DWORD>(-1)) continue;
            const double weight = static_cast<double>(delta_cycles);  // 单位：cycles，最后统一换算成 ms
            ++running;
            ++stat.samples;
            swept_ms += weight;

            StackSample sample;
            sample.tid = tid;
            sample.weight_ms = weight;
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(h, &context)) {
                STACKFRAME64 frame{};
                frame.AddrPC.Offset = context.Rip;
                frame.AddrPC.Mode = AddrModeFlat;
                frame.AddrStack.Offset = context.Rsp;
                frame.AddrStack.Mode = AddrModeFlat;
                frame.AddrFrame.Offset = context.Rbp;
                frame.AddrFrame.Mode = AddrModeFlat;
                while (static_cast<int>(sample.pcs.size()) < opt.max_depth &&
                       StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, h, &frame, &context,
                                   ReadProcessMemoryCb, SymFunctionTableAccess64,
                                   SymGetModuleBase64, nullptr)) {
                    if (frame.AddrPC.Offset == 0) break;
                    sample.pcs.push_back(frame.AddrPC.Offset);
                }
            }
            ResumeThread(h);

            const double suspend_cost = NowMs() - t_suspend;
            if (suspend_cost > opt.stall_ms) {
                ++stall_events;
                std::fprintf(stderr, "[sampler] stall %.1f ms on tid %lu (sweep %lld)\n",
                             suspend_cost, tid, sweep);
            }
            if (!sample.pcs.empty()) samples.push_back(std::move(sample));
        }

        total_sampled_ms += static_cast<long long>(swept_ms);
        sweep_rows.push_back(Fmt("%lld,%.1f,%.2f,%zu,%d,%.3f", sweep, NowMs() - started_ms,
                                 NowMs() - sweep_started, tids.size(), running, swept_ms));
        Sleep(static_cast<DWORD>(opt.interval_ms));
    }

    const double wall_ms = NowMs() - started_ms;
    for (auto& [tid, stat] : threads) {
        if (stat.handle != nullptr) CloseHandle(stat.handle);
    }

    // ---- 权重换算：cycles -> ms ----
    // QueryThreadCycleTime 的绝对频率不保证等于标称 TSC 频率（随 P-state 漂移），
    // 所以不猜频率，而是用**全线程 GetThreadTimes 的总 CPU 时间**标定一次：
    //     ms_per_cycle = sum(user+kernel of all threads) / sum(cycles of all samples)
    // 这样所有样本的权重之和 ≈ 真实 CPU 时间；分线程的 user/kernel 拆分仍来自
    // GetThreadTimes（粒度 15.6 ms，但对"整段场景的占比"足够准）。
    double total_cpu_ms = 0.0;
    for (const auto& entry : threads) {
        total_cpu_ms += entry.second.user_ms + entry.second.kernel_ms;
    }
    double total_weight = 0.0;
    for (const StackSample& s : samples) total_weight += s.weight_ms;
    double ms_per_cycle = 0.0;
    if (total_weight > 0.0 && total_cpu_ms > 0.0) {
        ms_per_cycle = total_cpu_ms / total_weight;
    } else if (total_weight > 0.0) {
        ms_per_cycle = 1.0 / 2.5e6;  // 退化估计：按 2.5 GHz
    }
    for (StackSample& s : samples) s.weight_ms *= ms_per_cycle;
    if (total_cpu_ms > 0.0) total_sampled_ms = static_cast<long long>(total_cpu_ms);

    // ---- 一次性符号解析（采样循环里做会慢两个数量级）----
    std::map<ULONG64, double> self_by_pc;
    std::map<ULONG64, double> incl_by_pc;
    std::map<ULONG64, long long> self_count;
    std::map<ULONG64, long long> incl_count;
    for (const StackSample& s : samples) {
        if (s.pcs.empty()) continue;
        self_by_pc[s.pcs[0]] += s.weight_ms;
        ++self_count[s.pcs[0]];
        std::set<ULONG64> seen;  // 同一栈里递归只算一次 inclusive
        for (const ULONG64 pc : s.pcs) {
            if (!seen.insert(pc).second) continue;
            incl_by_pc[pc] += s.weight_ms;
            ++incl_count[pc];
        }
    }

    struct Resolved {
        std::string module;
        std::string function;
    };
    std::map<ULONG64, Resolved> resolved;
    const auto resolve = [&](ULONG64 pc) -> const Resolved& {
        auto it = resolved.find(pc);
        if (it != resolved.end()) return it->second;
        Resolved r;
        char symbol_buffer[sizeof(SYMBOL_INFO) + 256] = {0};
        SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(symbol_buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        if (SymFromAddr(process, pc, &displacement, symbol)) {
            r.function = std::string(symbol->Name);
        } else {
            r.function = Fmt("0x%llx", static_cast<unsigned long long>(pc));
        }
        IMAGEHLP_MODULE64 module_info{};
        module_info.SizeOfStruct = sizeof(module_info);
        if (SymGetModuleInfo64(process, pc, &module_info)) {
            r.module = std::string(module_info.ModuleName);
        } else {
            r.module = "?";
        }
        return resolved.emplace(pc, std::move(r)).first->second;
    };

    std::map<FuncKey, FuncStat> functions;
    for (const auto& [pc, ms] : self_by_pc) {
        const Resolved& r = resolve(pc);
        FuncStat& stat = functions[FuncKey{r.module, r.function}];
        stat.self_ms += ms;
        stat.self_samples += self_count[pc];
    }
    for (const auto& [pc, ms] : incl_by_pc) {
        const Resolved& r = resolve(pc);
        FuncStat& stat = functions[FuncKey{r.module, r.function}];
        stat.inclusive_ms += ms;
        stat.inclusive_samples += incl_count[pc];
    }

    // 原始样本（前 N 条），供审计：保留 pc 解析后的名字
    std::vector<std::string> raw_rows;
    long long raw_emitted = 0;
    for (const StackSample& s : samples) {
        for (size_t depth = 0; depth < s.pcs.size(); ++depth) {
            if (raw_emitted >= opt.raw_head_limit) break;
            const Resolved& r = resolve(s.pcs[depth]);
            raw_rows.push_back(Fmt("%lu,%zu,%s,%s,%.3f", s.tid, depth, r.module.c_str(),
                                   r.function.c_str(), s.weight_ms));
            ++raw_emitted;
        }
        if (raw_emitted >= opt.raw_head_limit) break;
    }

    SymCleanup(process);
    CloseHandle(process);

    std::vector<std::pair<FuncKey, FuncStat>> sorted(functions.begin(), functions.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second.self_ms > b.second.self_ms; });
    std::vector<std::string> func_rows;
    for (const auto& [key, stat] : sorted) {
        func_rows.push_back(Fmt("%s,%s,%.3f,%.3f,%lld,%lld", key.module.c_str(),
                                key.function.c_str(), stat.self_ms, stat.inclusive_ms,
                                stat.self_samples, stat.inclusive_samples));
    }
    WriteCsv(opt.out_dir + "\\functions.csv",
             "module,function,self_ms,inclusive_ms,self_samples,inclusive_samples", func_rows);

    std::vector<std::string> thread_rows;
    for (const auto& [tid, stat] : threads) {
        const double total = stat.user_ms + stat.kernel_ms;
        thread_rows.push_back(Fmt("%lu,%s,%.3f,%.3f,%lld,%lld,%lld,%.1f", tid,
                                  stat.name.empty() ? "?" : stat.name.c_str(), stat.user_ms,
                                  stat.kernel_ms, stat.samples, stat.sweeps_seen,
                                  stat.wait_streak_max, total > 0 ? 100.0 * stat.kernel_ms / total : 0.0));
    }
    WriteCsv(opt.out_dir + "\\threads.csv",
             "tid,thread_name,user_ms,kernel_ms,samples,sweeps,wait_streak_max,kernel_pct",
             thread_rows);
    WriteCsv(opt.out_dir + "\\sweeps.csv",
             "sweep,ts_us,sweep_ms,threads_seen,threads_running,sampled_ms", sweep_rows);
    WriteCsv(opt.out_dir + "\\samples_head.csv", "tid,depth,module,function,weight_ms", raw_rows);

    std::vector<std::string> meta = {
        Fmt("csv,pid,%lu", opt.pid),
        Fmt("csv,exe,%s", opt.exe.c_str()),
        Fmt("csv,interval_ms,%d", opt.interval_ms),
        Fmt("csv,wall_ms,%.1f", wall_ms),
        Fmt("csv,sweeps,%lld", sweep),
        Fmt("csv,avg_sweep_ms,%.2f", sweep > 0 ? wall_ms / static_cast<double>(sweep) : 0.0),
        Fmt("csv,sampled_cpu_ms,%lld", total_sampled_ms),
        Fmt("csv,stall_events,%lld", stall_events),
        Fmt("csv,threads_seen_max,%d", threads_seen_max),
        Fmt("csv,distinct_functions,%zu", functions.size()),
        Fmt("csv,distinct_pcs,%zu", resolved.size()),
        Fmt("csv,samples,%zu", samples.size()),
        Fmt("csv,distinct_threads,%zu", threads.size()),
    };
    WriteCsv(opt.out_dir + "\\meta.csv", "kind,key,value", meta);

    timeEndPeriod(1);
    std::printf("[sampler] sweeps=%lld avg_sweep=%.2fms wall=%.0fms cpu=%.0fms samples=%zu "
                "functions=%zu stalls=%lld\n",
                sweep, sweep > 0 ? wall_ms / static_cast<double>(sweep) : 0.0, wall_ms,
                static_cast<double>(total_sampled_ms), samples.size(), functions.size(),
                stall_events);
    return stall_events > 0 ? 4 : 0;
}