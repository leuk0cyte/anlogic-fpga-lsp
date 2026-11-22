// =============================================================================
//  src/td/td_session.cpp
// =============================================================================
//  Windows 实现要点：
//   * CreatePipe + CreateProcessW，stderr 合并到 stdout（与 Python stderr=STDOUT 一致）
//   * 无缓冲语义：每次 ReadFile 拿到多少算多少（等价 Python bufsize=0）
//   * 读操作带超时：用 PeekNamedPipe 轮询（管道读本身不可中断）
//   * Job Object + JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE：父进程异常退出也不会留孤儿
// =============================================================================

#include "td/td_session.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "td/td_output.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

#include "platform/command_line.h"
#include "platform/encoding.h"
#include "platform/path.h"

namespace fpgalsp::td {
namespace {

namespace plat = fpgalsp::platform;

constexpr const char* kPrompt = "td > ";

std::string MakeTag() {
    // 对应 Python: "dsh%d" % (int(time.time() * 1000) % 1000000000)
    using namespace std::chrono;
    const auto ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    return "dsh" + std::to_string(static_cast<long long>(ms % 1000000000LL));
}

// D-3：options.work_dir 为空时**不再**让 TD 继承本进程的 cwd（那会把 TD 自己的
// 日志/中间文件撒到用户的工作目录或 IDE 的启动目录里），而是给本进程一个
// %TEMP%/fpgalsp-td-<pid> 的 scratch 目录。
//
// 为什么按"进程"而不是"会话"分目录：LSP 每次保存都会新建一个 TD 会话（read_hdl
// 会烧掉会话），按会话分目录会在 %TEMP% 里留下成百上千个空目录。TD 自己的日志名
// 带时间戳（td_YYYYMMDD_HHMMSS.logw），同一进程内的多个会话共享目录不会互相覆盖。
// 调用方仍可显式指定（bridge 侧应传 <project>/.fpgalsp/run，patch 见 B7-REPORT）。
std::string MakeScratchWorkDir() {
    std::error_code ec;
    std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec || base.empty()) base = std::filesystem::path(".");
#ifdef _WIN32
    const unsigned long pid = ::GetCurrentProcessId();
#else
    const unsigned long pid = 0;
#endif
    const std::string name = "fpgalsp-td-" + std::to_string(pid);
    std::filesystem::path dir = base / name;
    std::filesystem::create_directories(dir, ec);
    // 目录建不出来就退回"当前目录"，绝不因为 scratch 目录失败而让 TD 起不来
    if (ec) return std::string();
    // 全项目内部路径都是 UTF-8（Windows 用户名可能是非 ASCII，不能走 ACP 的
    // path::string()），所以这里显式按 UTF-8 取出。
    const std::u8string utf8 = dir.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

}  // namespace

namespace {

// TD 在会话 cwd 里留下的"一次性"文件：
//   * `td_<YYYYMMDD_HHMMSS>.log` / `.logw`  —— 每个短会话一份（GUI 的日志在 `<al>/log/`）
//   * `minidump-*.dmp`                      —— TD 崩溃时写的转储
// 会话结束后由 Impl::RelocateSessionFiles() 挪到 options.log_dir。
bool IsTransientSessionArtifact(std::string_view name) {
    const auto ends_with = [name](std::string_view suffix) {
        return name.size() >= suffix.size() &&
               name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    if (name.rfind("td_", 0) == 0 && (ends_with(".log") || ends_with(".logw"))) return true;
    if (name.rfind("minidump-", 0) == 0 && ends_with(".dmp")) return true;
    return false;
}

}  // namespace

struct TdSession::Impl {
#ifdef _WIN32
    HANDLE process = nullptr;
    HANDLE thread = nullptr;
    HANDLE stdin_write = nullptr;
    HANDLE stdout_read = nullptr;
    HANDLE job = nullptr;
#else
    void* process = nullptr;
#endif
    std::atomic<bool> alive{false};
    std::string buffer;  // 累积的 stdout 字节
    std::string work_dir;  // D-3：本会话实际使用的 cwd（空 = 未能创建）
    std::mutex io_mutex;
    unsigned long pid = 0;
    // 会话日志外移：目标目录 + 会话开始前 cwd 里已有的那些文件名（那些不是本次产生的）
    std::string log_dir;
    std::set<std::string> preexisting;
    bool relocated = false;

    // 把本次会话新产生的 td_*.log/.logw、minidump-*.dmp 从 cwd 挪到 log_dir。
    // 只用"文件是否在会话开始时就存在"做判据（不做时间比较：TD 的日志名带的是它自己的
    // 启动时间，不是我们的会话时间；而且同一目录可能被多个会话共用）。
    void RelocateSessionFiles() {
        if (relocated || log_dir.empty() || work_dir.empty()) return;
        relocated = true;
#ifdef _WIN32
        // TerminateProcess 是异步的：先等进程真的死掉，否则文件可能还被它占着
        if (process != nullptr) WaitForSingleObject(process, 5000);
#endif
        std::vector<std::string> names;
        {
            std::error_code ec;
            for (std::filesystem::directory_iterator it(std::filesystem::path(work_dir), ec);
                 !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
                const std::string name = it->path().filename().string();
                if (!IsTransientSessionArtifact(name)) continue;
                if (preexisting.find(name) != preexisting.end()) continue;
                names.push_back(name);
            }
        }
        if (names.empty()) return;
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(log_dir), ec);
        for (const std::string& name : names) {
            const std::filesystem::path from = std::filesystem::path(work_dir) / name;
            const std::filesystem::path to = std::filesystem::path(log_dir) / name;
            bool moved = false;
            for (int attempt = 0; attempt < 3 && !moved; ++attempt) {
                if (attempt > 0) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                std::error_code move_ec;
                std::filesystem::rename(from, to, move_ec);
                if (!move_ec) {
                    moved = true;
                    break;
                }
                // 跨卷（state 目录在 C:）时 rename 会失败，退回复制 + 删除
                move_ec.clear();
                std::filesystem::copy_file(from, to,
                                           std::filesystem::copy_options::overwrite_existing,
                                           move_ec);
                if (!move_ec) {
                    std::filesystem::remove(from, move_ec);
                    moved = true;
                }
            }
        }
    }

    ~Impl() { Cleanup(); }

    void Cleanup() {
#ifdef _WIN32
        if (job != nullptr) {
            // KILL_ON_JOB_CLOSE 会把整棵进程树带走
            CloseHandle(job);
            job = nullptr;
        }
        if (stdin_write != nullptr) {
            CloseHandle(stdin_write);
            stdin_write = nullptr;
        }
        if (stdout_read != nullptr) {
            CloseHandle(stdout_read);
            stdout_read = nullptr;
        }
        if (thread != nullptr) {
            CloseHandle(thread);
            thread = nullptr;
        }
        if (process != nullptr) {
            CloseHandle(process);
            process = nullptr;
        }
#endif
        alive = false;
    }

#ifdef _WIN32
    // 从管道读一批（有超时）。返回空 + eof / timed_out 标记。
    std::string ReadChunk(bool* eof, bool* timed_out,
                          std::chrono::steady_clock::time_point deadline);
#endif
};

#ifdef _WIN32
std::string TdSession::Impl::ReadChunk(bool* eof, bool* timed_out,
                                       std::chrono::steady_clock::time_point deadline) {
    *eof = false;
    *timed_out = false;

    while (true) {
        DWORD available = 0;
        if (!PeekNamedPipe(stdout_read, nullptr, 0, nullptr, &available, nullptr)) {
            // 管道句柄失效。注意：这**不一定**代表进程退出 ——
            // 实测 read_hdl 之后 TD 会关闭自己那侧的 stdout，但进程仍存活
            // （Python 版同样观察到此现象，见 PYTHON-PARITY.md §5.3）。
            // 交给调用方按"进程是否真的退出"来判定。
            *eof = true;
            return {};
        }
        if (available > 0) {
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!ReadFile(stdout_read, chunk.data(), available, &read, nullptr)) {
                *eof = true;
                return {};
            }
            chunk.resize(read);
            return chunk;
        }
        if (process != nullptr && WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
            *eof = true;
            return {};
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            *timed_out = true;
            return {};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
#endif

namespace {

#ifdef _WIN32

std::wstring Widen(const std::string& s) {
    // 简单 UTF-8 -> UTF-16
    if (s.empty()) return {};
    const int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                         nullptr, 0);
    std::wstring out(static_cast<std::size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), need);
    return out;
}

std::string Narrow(const wchar_t* s, int len) {
    if (len <= 0) return {};
    const int need = WideCharToMultiByte(CP_UTF8, 0, s, len, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, len, out.data(), need, nullptr, nullptr);
    return out;
}

// 把进程加入 Job Object，并设置"job 句柄关闭即杀进程树"
bool AssignToJob(HANDLE process, HANDLE* out_job) {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr) return false;

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info,
                                 sizeof(info))) {
        CloseHandle(job);
        return false;
    }
    if (!AssignProcessToJobObject(job, process)) {
        CloseHandle(job);
        return false;
    }
    *out_job = job;
    return true;
}

#endif

}  // namespace

// -----------------------------------------------------------------------------
// 构造 / 析构
// -----------------------------------------------------------------------------

TdSession::TdSession(const TdChoice& td, const TdSessionOptions& options) : td_(td) {
    impl_ = std::make_unique<Impl>();

    // D-3：未显式指定 cwd 时用独立 scratch 目录，别让 TD 往"当前目录"写东西。
    std::string work_dir = options.work_dir;
    if (work_dir.empty()) work_dir = MakeScratchWorkDir();
    impl_->work_dir = work_dir;

    // 对应 Python: os.makedirs(cwd, exist_ok=True)
    if (!work_dir.empty()) {
        plat::ensure_directory(work_dir);
    }

    // 会话日志外移（见 TdSessionOptions::log_dir 的注释）：先记下 cwd 里**已经存在**的
    // td_*/minidump-*，结束时只挪这次会话新产生的那些，绝不碰用户自己启动的 TD 日志。
    impl_->log_dir = options.log_dir;
    if (!impl_->log_dir.empty() && !work_dir.empty()) {
        std::error_code ec;
        for (std::filesystem::directory_iterator it(std::filesystem::path(work_dir), ec);
             !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (IsTransientSessionArtifact(name)) impl_->preexisting.insert(name);
        }
    }

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE stdin_read = nullptr;
    HANDLE stdout_write = nullptr;
    if (!CreatePipe(&stdin_read, &impl_->stdin_write, &sa, 0)) {
        throw TdError("CreatePipe(stdin) failed");
    }
    if (!CreatePipe(&impl_->stdout_read, &stdout_write, &sa, 0)) {
        CloseHandle(stdin_read);
        throw TdError("CreatePipe(stdout) failed");
    }
    // 我们不希望子进程继承我们自己的读/写端
    SetHandleInformation(impl_->stdin_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(impl_->stdout_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdin_read;
    si.hStdOutput = stdout_write;
    si.hStdError = stdout_write;  // 关键：stderr 合并到 stdout
    PROCESS_INFORMATION pi{};

    // ⚠ B7 / S-02：exe 路径可能含空格（"C:\Program Files\...\td_commands_prompt.exe"）。
    //   旧实现把 exe 原样塞进 lpCommandLine 且 lpApplicationName=nullptr，Windows 会
    //   按空白切分去猜可执行文件 -> 试图运行 "C:\Program.exe"（CWE-428）。
    //   现在：lpApplicationName 传权威的 exe 全路径，lpCommandLine 里的 argv[0] 按
    //   CRT 规则加引号（td 目前不接受任何命令行参数，args 为空）。
    const std::wstring exe_wide = Widen(td.exe);
    std::wstring cmdline = plat::BuildWindowsCommandLine(exe_wide);
    std::vector<wchar_t> mutable_cmd(cmdline.begin(), cmdline.end());
    mutable_cmd.push_back(L'\0');

    const std::wstring workdir = work_dir.empty() ? std::wstring()
                                                  : Widen(plat::normalize_path(work_dir));

    const BOOL ok = CreateProcessW(
        exe_wide.c_str(), mutable_cmd.data(), nullptr, nullptr, TRUE,
        CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP, nullptr,
        workdir.empty() ? nullptr : workdir.c_str(), &si, &pi);

    // 父进程侧不再需要子进程的端点
    CloseHandle(stdin_read);
    CloseHandle(stdout_write);

    if (!ok) {
        throw TdError("CreateProcess failed for " + td.exe);
    }

    impl_->process = pi.hProcess;
    impl_->thread = pi.hThread;
    impl_->pid = pi.dwProcessId;

    // 关键：先加入 Job，再恢复运行 —— 避免恢复后瞬间派生的子进程漏出 job
    HANDLE job = nullptr;
    if (AssignToJob(pi.hProcess, &job)) {
        impl_->job = job;
    }
    ResumeThread(pi.hThread);

    impl_->alive = true;

    // 读启动横幅，直到看到提示符（对应 Python _read_until(b"td > ", startup_timeout)）
    try {
        banner_ = ReadUntil(kPrompt, options.startup_timeout_seconds);
    } catch (...) {
        Kill();
        throw;
    }
#else
    throw TdError("TdSession is Windows-only");
#endif
}

TdSession::~TdSession() { Close(); }

// -----------------------------------------------------------------------------
// 读取
// -----------------------------------------------------------------------------

std::string TdSession::ReadUntil(std::string_view marker, double timeout_seconds) {
    std::lock_guard<std::mutex> lock(impl_->io_mutex);

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(
                              static_cast<long long>(timeout_seconds * 1000.0));

#ifdef _WIN32
    while (impl_->buffer.find(marker) == std::string::npos) {
        bool eof = false;
        bool timed_out = false;
        const std::string chunk = impl_->ReadChunk(&eof, &timed_out, deadline);
        if (!chunk.empty()) {
            impl_->buffer += chunk;
            continue;
        }
        if (timed_out) {
            Kill();
            throw TdError("TD command timed out (" + std::to_string(timeout_seconds) +
                          "s); session closed");
        }
        if (eof) {
            // 关键区分（实测 read_hdl 场景）：
            //   管道断开 + 进程已退出 -> 真正的 "exited unexpectedly"
            //   管道断开 + 进程仍存活 -> TD 关掉了自己那侧的 stdout，
            //                            把已收到的内容返回给调用方（不抛）
            // Python 版对 read_hdl 的处理是"拿到多少算多少"，这里保持一致。
            if (impl_->process != nullptr &&
                WaitForSingleObject(impl_->process, 0) != WAIT_OBJECT_0) {
                // 进程还活着：标记会话不可再写，返回已缓冲内容
                impl_->alive = false;
                std::string out = impl_->buffer;
                impl_->buffer.clear();
                return out;
            }
            Kill();
            throw TdError("TD process exited unexpectedly");
        }
    }

    const std::size_t idx = impl_->buffer.find(marker) + marker.size();
    std::string out = impl_->buffer.substr(0, idx);
    impl_->buffer = impl_->buffer.substr(idx);
    return out;
#else
    throw TdError("TdSession is Windows-only");
#endif
}

// -----------------------------------------------------------------------------
// 执行
// -----------------------------------------------------------------------------

std::string TdSession::Run(std::string_view tcl, double timeout_seconds) {
    if (!Alive()) {
        throw TdError("TD session is closed");
    }
    const std::string tag = MakeTag();
    const std::string begin = "__B" + tag + "__";
    const std::string end = "__E" + tag + "__";

    std::string payload;
    payload += "puts \"" + begin + "\"\n";
    payload += "flush stdout\n";
    payload += std::string(tcl) + "\n";
    payload += "puts \"" + end + "\"\n";
    payload += "flush stdout\n";

    {
        std::lock_guard<std::mutex> lock(impl_->io_mutex);
#ifdef _WIN32
        DWORD written = 0;
        if (!WriteFile(impl_->stdin_write, payload.data(),
                       static_cast<DWORD>(payload.size()), &written, nullptr) ||
            written != payload.size()) {
            throw TdError("Failed to write to TD stdin");
        }
#else
        throw TdError("TdSession is Windows-only");
#endif
    }

    const std::string raw = ReadUntil(end, timeout_seconds);
    return StripSentinelsAndPrompt(raw, tag);
}

bool TdSession::Alive() const {
#ifdef _WIN32
    if (!impl_->alive) return false;
    if (impl_->process == nullptr) return false;
    return WaitForSingleObject(impl_->process, 0) != WAIT_OBJECT_0;
#else
    return false;
#endif
}

void TdSession::Kill() {
    if (impl_ == nullptr) return;
#ifdef _WIN32
    if (impl_->job != nullptr) {
        // KILL_ON_JOB_CLOSE：关掉 job 即杀掉整棵进程树
        TerminateJobObject(impl_->job, 1);
        CloseHandle(impl_->job);
        impl_->job = nullptr;
    }
    if (impl_->process != nullptr) {
        TerminateProcess(impl_->process, 1);
    }
#endif
    impl_->alive = false;
    // 会话结束：把这次新产生的 td_*.log/.logw、minidump-*.dmp 从 cwd 挪到
    // options.log_dir（通常是 `<al>/log`，和 TD GUI 的日志放一起）。
    impl_->RelocateSessionFiles();
}

void TdSession::Close() {
    if (impl_ == nullptr) return;
    if (Alive()) {
#ifdef _WIN32
        // 先礼貌退出（对应 Python: 写 "exit\n" 并 wait 8s）
        {
            std::lock_guard<std::mutex> lock(impl_->io_mutex);
            const char* exit_cmd = "exit\n";
            DWORD written = 0;
            WriteFile(impl_->stdin_write, exit_cmd,
                      static_cast<DWORD>(std::strlen(exit_cmd)), &written, nullptr);
        }
        if (impl_->process != nullptr) {
            WaitForSingleObject(impl_->process, 8000);
        }
#endif
    }
    Kill();
    impl_->Cleanup();
}

unsigned long TdSession::ProcessId() const { return impl_->pid; }

const std::string& TdSession::WorkDir() const {
    static const std::string kEmpty;
    return impl_ != nullptr ? impl_->work_dir : kEmpty;
}

// -----------------------------------------------------------------------------
// 进程计数（测试用）
// -----------------------------------------------------------------------------

int CountProcessesByName(std::string_view exe_name) {
#ifdef _WIN32
    int count = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return -1;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    const std::string want = [&] {
        std::string s(exe_name);
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }();

    if (Process32FirstW(snapshot, &entry)) {
        do {
            std::string name = Narrow(entry.szExeFile, static_cast<int>(wcslen(entry.szExeFile)));
            for (char& c : name) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (name == want) ++count;
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return count;
#else
    return -1;
#endif
}

}  // namespace fpgalsp::td