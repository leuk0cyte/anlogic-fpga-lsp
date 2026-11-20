// =============================================================================
//  src/flow/programmer.cpp —— BitWizard 烧录后端
// =============================================================================
//  逐条对照 Y:\fpga-lsp\fpgalsp\program.py。差别只有一处（有意加强）：
//  确认（confirm）下沉到本层，见头文件的"安全模型"第 2 条。
//
//  BitWizard 与 TD 用同一套哨兵协议，但提示符是 "bw > "（TdSession 里硬编码成
//  "td > "），所以这里单独实现一个最小进程封装，机制与 td_session.cpp 相同：
//    CreatePipe + CreateProcess(CREATE_SUSPENDED) -> 入 Job Object -> Resume
//    读用 PeekNamedPipe 轮询，kill 用 TerminateJobObject（整棵树，不留孤儿）
// =============================================================================

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "flow/programmer.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <re2/re2.h>

#include "platform/command_line.h"
#include "platform/encoding.h"
#include "platform/utf16.h"
#include "td/tcl_quote.h"

namespace fpgalsp::flow {
namespace {

namespace fs = std::filesystem;

constexpr const char* kPrompt = "bw > ";

std::string Slashes(std::string text) {
    for (char& c : text) {
        if (c == '\\') c = '/';
    }
    return text;
}

std::string Trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    const auto is_space = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
    };
    while (begin < end && is_space(text[begin])) ++begin;
    while (end > begin && is_space(text[end - 1])) --end;
    return std::string(text.substr(begin, end - begin));
}

std::vector<std::string> Lines(std::string_view text) {
    std::string normalised(text);
    for (char& c : normalised) {
        if (c == '\r') c = '\n';
    }
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = normalised.find('\n', start);
        if (pos == std::string::npos) {
            out.push_back(normalised.substr(start));
            return out;
        }
        out.push_back(normalised.substr(start, pos - start));
        start = pos + 1;
    }
}

bool IsFile(std::string_view path) {
    if (path.empty()) return false;
    std::error_code ec;
    return fs::is_regular_file(fs::path(std::string(path)), ec) && !ec;
}

int64_t FileSize(std::string_view path) {
    std::error_code ec;
    const auto size = fs::file_size(fs::path(std::string(path)), ec);
    return ec ? 0 : static_cast<int64_t>(size);
}

double Mtime(std::string_view path) {
    std::error_code ec;
    const auto time = fs::last_write_time(fs::path(std::string(path)), ec);
    if (ec) return 0.0;
    const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(time);
    return std::chrono::duration<double>(sys.time_since_epoch()).count();
}

std::string Join(std::string_view dir, std::string_view name) {
    if (dir.empty()) return std::string(name);
    std::string out(dir);
    if (out.back() != '/' && out.back() != '\\') out.push_back('/');
    out += name;
    return Slashes(std::move(out));
}

std::vector<std::string> ListFiles(std::string_view dir) {
    std::vector<std::string> names;
    std::error_code ec;
    if (dir.empty()) return names;
    fs::directory_iterator it(fs::path(std::string(dir)), ec);
    if (ec) return names;
    const fs::directory_iterator end;
    while (it != end) {
        std::error_code status_ec;
        if (fs::is_regular_file(it->path(), status_ec) && !status_ec) {
            const std::u8string name = it->path().filename().u8string();
            names.emplace_back(reinterpret_cast<const char*>(name.data()), name.size());
        }
        it.increment(ec);
        if (ec) break;
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::wstring Wide(std::string_view utf8) {
    const std::u16string utf16 = platform::utf8_to_utf16(utf8);
    std::wstring out;
    out.reserve(utf16.size());
    for (const char16_t unit : utf16) out.push_back(static_cast<wchar_t>(unit));
    return out;
}

std::string Narrow(std::wstring_view wide) {
    std::u16string utf16;
    utf16.reserve(wide.size());
    for (const wchar_t unit : wide) utf16.push_back(static_cast<char16_t>(unit));
    return platform::utf16_to_utf8(utf16);
}

// -----------------------------------------------------------------------------
// BitWizard 进程（最小实现，机制同 td_session.cpp）
// -----------------------------------------------------------------------------
class BwProcess {
public:
    BwProcess(const std::string& exe, const std::string& cwd, double startup_timeout,
              std::string* error);
    ~BwProcess() { Close(); }

    bool Run(std::string_view tcl, double timeout_seconds, std::string* out, std::string* error);
    void Kill();
    void Close();

private:
    std::string ReadUntil(std::string_view marker, double timeout_seconds);

    HANDLE process_ = nullptr;
    HANDLE thread_ = nullptr;
    HANDLE job_ = nullptr;
    HANDLE stdin_write_ = nullptr;
    HANDLE stdout_read_ = nullptr;
    std::string buffer_;
    bool alive_ = false;
    std::wstring work_dir_;
};

BwProcess::BwProcess(const std::string& exe, const std::string& cwd, double startup_timeout,
                     std::string* error) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE stdin_read = nullptr;
    HANDLE stdout_write = nullptr;
    if (!CreatePipe(&stdin_read, &stdin_write_, &sa, 0)) {
        if (error) *error = "CreatePipe(stdin) failed";
        return;
    }
    if (!CreatePipe(&stdout_read_, &stdout_write, &sa, 0)) {
        CloseHandle(stdin_read);
        CloseHandle(stdin_write_);
        stdin_write_ = nullptr;
        if (error) *error = "CreatePipe(stdout) failed";
        return;
    }
    SetHandleInformation(stdin_write_, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stdout_read_, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdin_read;
    si.hStdOutput = stdout_write;
    si.hStdError = stdout_write;

    PROCESS_INFORMATION pi{};
    // ⚠ B7 / S-02：同 td_session.cpp —— exe 走 lpApplicationName（权威），
    //   lpCommandLine 里的 argv[0] 按 CRT 规则加引号。旧写法（nullptr + 裸 exe）
    //   在 TD 装在带空格目录时会去执行 "C:\Program.exe"（CWE-428）。
    const std::wstring exe_wide = Wide(exe);
    std::wstring cmdline = platform::BuildWindowsCommandLine(exe_wide);
    std::vector<wchar_t> mutable_cmd(cmdline.begin(), cmdline.end());
    mutable_cmd.push_back(L'\0');
    work_dir_ = cwd.empty() ? std::wstring() : Wide(Slashes(cwd));

    const BOOL ok = CreateProcessW(exe_wide.c_str(), mutable_cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP, nullptr,
                                   work_dir_.empty() ? nullptr : work_dir_.c_str(), &si, &pi);
    CloseHandle(stdin_read);
    CloseHandle(stdout_write);
    if (!ok) {
        if (error) *error = "CreateProcess failed for " + exe;
        return;
    }

    process_ = pi.hProcess;
    thread_ = pi.hThread;

    // 先入 Job 再恢复，保证恢复后立刻派生的子进程也在 Job 里
    if (HANDLE job = CreateJobObjectW(nullptr, nullptr); job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        (void)SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info,
                                      sizeof(info));
        if (AssignProcessToJobObject(job, process_)) {
            job_ = job;
        } else {
            CloseHandle(job);
        }
    }
    ResumeThread(thread_);
    alive_ = true;

    try {
        (void)ReadUntil(kPrompt, startup_timeout);
    } catch (...) {
        Kill();
        if (error) *error = "BitWizard did not reach its prompt";
    }
}

std::string BwProcess::ReadUntil(std::string_view marker, double timeout_seconds) {
    if (!alive_) return {};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(
                                                                 static_cast<long long>(
                                                                     timeout_seconds * 1000.0));
    while (buffer_.find(marker) == std::string::npos) {
        DWORD available = 0;
        if (stdout_read_ == nullptr) break;
        if (!PeekNamedPipe(stdout_read_, nullptr, 0, nullptr, &available, nullptr)) {
            // 管道断开：进程已退出
            alive_ = false;
            break;
        }
        if (available == 0) {
            if (std::chrono::steady_clock::now() > deadline) {
                Kill();
                alive_ = false;
                std::string out;
                out.swap(buffer_);
                return out;
            }
            if (process_ != nullptr && WaitForSingleObject(process_, 0) == WAIT_OBJECT_0) {
                // 进程退出，把剩余字节读完
                DWORD rest = 0;
                if (!PeekNamedPipe(stdout_read_, nullptr, 0, nullptr, &rest, nullptr) ||
                    rest == 0) {
                    alive_ = false;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        char chunk[8192];
        DWORD read = 0;
        if (!ReadFile(stdout_read_, chunk, sizeof(chunk), &read, nullptr) || read == 0) {
            alive_ = false;
            break;
        }
        buffer_.append(chunk, read);
    }

    const std::size_t pos = buffer_.find(marker);
    if (pos == std::string::npos) {
        std::string out;
        out.swap(buffer_);
        return out;
    }
    const std::size_t idx = pos + marker.size();
    std::string out = buffer_.substr(0, idx);
    buffer_.erase(0, idx);
    return out;
}

bool BwProcess::Run(std::string_view tcl, double timeout_seconds, std::string* out,
                    std::string* error) {
    if (!alive_ || stdin_write_ == nullptr) {
        if (error) *error = "BitWizard session is not available";
        return false;
    }
    const std::string tag = std::to_string(static_cast<unsigned long long>(
                                std::chrono::steady_clock::now().time_since_epoch().count()) %
                                1000000000ULL);
    const std::string begin = "__B" + tag + "__";
    const std::string end = "__E" + tag + "__";
    std::string payload;
    payload += "puts \"" + begin + "\"\nflush stdout\n";
    payload += std::string(tcl) + "\n";
    payload += "puts \"" + end + "\"\nflush stdout\n";

    DWORD written = 0;
    if (!WriteFile(stdin_write_, payload.data(), static_cast<DWORD>(payload.size()), &written,
                   nullptr)) {
        if (error) *error = "cannot write to the BitWizard process";
        alive_ = false;
        return false;
    }
    std::string raw = ReadUntil(end, timeout_seconds);
    if (const std::size_t pos = raw.find(begin); pos != std::string::npos) {
        raw.erase(0, pos + begin.size());
    }
    if (const std::size_t pos = raw.rfind(end); pos != std::string::npos) raw.erase(pos);
    // 对应 Python raw.replace("bw > ", "")
    while (true) {
        const std::size_t pos = raw.find(kPrompt);
        if (pos == std::string::npos) break;
        raw.erase(pos, std::char_traits<char>::length(kPrompt));
    }
    if (out) *out = raw;
    return true;
}

void BwProcess::Kill() {
    if (job_ != nullptr) {
        TerminateJobObject(job_, 1);  // 整棵进程树
        CloseHandle(job_);
        job_ = nullptr;
    } else if (process_ != nullptr) {
        TerminateProcess(process_, 1);
    }
    alive_ = false;
}

void BwProcess::Close() {
    if (stdin_write_ != nullptr) {
        const char exit_command[] = "exit\n";
        DWORD written = 0;
        (void)WriteFile(stdin_write_, exit_command, sizeof(exit_command) - 1, &written, nullptr);
        if (process_ != nullptr) (void)WaitForSingleObject(process_, 4000);
    }
    if (stdin_write_ != nullptr) {
        CloseHandle(stdin_write_);
        stdin_write_ = nullptr;
    }
    if (stdout_read_ != nullptr) {
        CloseHandle(stdout_read_);
        stdout_read_ = nullptr;
    }
    if (!alive_) {
        if (process_ != nullptr) {
            CloseHandle(process_);
            process_ = nullptr;
        }
        if (thread_ != nullptr) {
            CloseHandle(thread_);
            thread_ = nullptr;
        }
        if (job_ != nullptr) {
            CloseHandle(job_);
            job_ = nullptr;
        }
        return;
    }
    Kill();
    if (process_ != nullptr) {
        CloseHandle(process_);
        process_ = nullptr;
    }
    if (thread_ != nullptr) {
        CloseHandle(thread_);
        thread_ = nullptr;
    }
}

// -----------------------------------------------------------------------------
// BwSessionLike 的真实实现
// -----------------------------------------------------------------------------
class BwCliSession final : public BwSessionLike {
public:
    BwCliSession(const std::string& exe, const std::string& cwd)
        : process_(exe, cwd, 30.0, &last_error_) {}

    std::string Run(std::string_view tcl, double timeout_seconds) override {
        std::string out;
        std::string error;
        if (!process_.Run(tcl, timeout_seconds, &out, &error)) {
            last_error_ = error;
            return {};
        }
        return out;
    }

    void Kill() override { process_.Kill(); }
    void Close() override { process_.Close(); }
    std::string LastError() const override { return last_error_; }

private:
    BwProcess process_;
    std::string last_error_;
};

std::string FindBwExe(const td::TdChoice& td, std::string* error) {
    const std::string exe = Join(Join(td.root, "bin"), "bw_commands_prompt.exe");
    if (IsFile(exe)) return exe;
    if (error) *error = "Cannot find bw_commands_prompt.exe (TD installation bin directory)";
    return {};
}

// 打开一次会话，跑若干命令。返回 false 表示"会话都建不起来"。
class SessionScope {
public:
    SessionScope(const td::TdChoice& td, const ProgramOptions& options, std::string* error) {
        if (options.session_factory) {
            session_ = options.session_factory(options.cwd);
            if (!session_ && error) *error = "cannot start BitWizard session";
            return;
        }
        const std::string exe = FindBwExe(td, error);
        if (exe.empty()) return;
        session_ = std::make_unique<BwCliSession>(exe, Slashes(options.cwd));
    }

    ~SessionScope() {
        if (session_) session_->Close();
    }

    SessionScope(const SessionScope&) = delete;
    SessionScope& operator=(const SessionScope&) = delete;

    BwSessionLike* get() const { return session_.get(); }
    std::string LastError() const { return session_ ? session_->LastError() : std::string(); }

private:
    std::unique_ptr<BwSessionLike> session_;
};

// 对应 Python 的 tail 提取：去掉空行、RUN-100、提示符残留，取最后 N 行
std::vector<std::string> TailLines(std::string_view text, std::size_t limit) {
    std::vector<std::string> out;
    for (const std::string& line : Lines(text)) {
        const std::string trimmed = Trim(line);
        if (trimmed.empty()) continue;
        if (trimmed.rfind("RUN-100", 0) == 0) continue;
        if (trimmed.rfind("bw >", 0) == 0) continue;
        out.push_back(trimmed);
    }
    if (out.size() > limit) out.erase(out.begin(), out.begin() + (out.size() - limit));
    return out;
}

jsonrpc::Json TailJson(const std::vector<std::string>& lines) {
    jsonrpc::Json out = jsonrpc::Json::array();
    for (const std::string& line : lines) out.push_back(line);
    return out;
}

}  // namespace

// -----------------------------------------------------------------------------
// 纯解析 / 判定
// -----------------------------------------------------------------------------

jsonrpc::Json CableInfo::ToJson() const {
    jsonrpc::Json out = jsonrpc::Json::object();
    out["index"] = index;
    out["device"] = device;
    return out;
}

std::vector<CableInfo> ParseCables(std::string_view text) {
    static const RE2 kDeviceRe(R"(([0-9]+)\s*:\s*([A-Za-z][A-Za-z0-9_]+))");
    // 2026.2 把表头与条目拆成了两行（本机实测，bw_2026-11-20 的真实输出）：
    //     PRG-1000 : Connect device:
    //     0:PH1A100              <- 注意冒号后没有空格
    // 旧实现只在含 "Connect device" 的那一行里找 `N:器件`，于是 2026.2 下永远解析出
    // **空列表** —— Inspect 报 "No cable/board detected"、Program 在 stage="cable" 直接拒绝，
    // 板子在环也烧不了。所以裸条目行必须单独认。
    static const RE2 kEntryLineRe(R"(^\s*([0-9]+)\s*:\s*([A-Za-z][A-Za-z0-9_]+)\s*$)");
    std::vector<CableInfo> cables;
    const auto add = [&cables](const std::string& index, std::string device) {
        const int value = std::atoi(index.c_str());
        for (const CableInfo& existing : cables) {
            if (existing.index == value && existing.device == device) return;  // 去重
        }
        CableInfo info;
        info.index = value;
        info.device = std::move(device);
        cables.push_back(std::move(info));
    };

    for (const std::string& line : Lines(text)) {
        const std::size_t marker = line.find("Connect device");
        if (marker == std::string::npos) {
            // 2026.2：器件条目独占一行，形如 "0:PH1A100"
            std::string index;
            std::string device;
            if (RE2::FullMatch(line, kEntryLineRe, &index, &device)) add(index, std::move(device));
            continue;
        }
        std::string rest = line.substr(marker + std::string("Connect device").size());
        // Python: rest.lstrip(":").strip()
        while (!rest.empty() && rest.front() == ':') rest.erase(rest.begin());
        rest = Trim(rest);

        re2::StringPiece input(rest);
        std::string index;
        std::string device;
        while (RE2::FindAndConsume(&input, kDeviceRe, &index, &device)) {
            add(index, std::move(device));
        }
    }
    return cables;
}

std::string ParseDeviceId(std::string_view text) {
    static const RE2 kIdRe(R"(^[A-Z]{2,}[A-Z0-9]{2,}$)");
    static const RE2 kCodeRe(R"(^[A-Z]{2,5}-\d)");
    for (const std::string& line : Lines(text)) {
        const std::string token = Trim(line);
        if (token.empty()) continue;
        if (token.find(':') != std::string::npos) continue;
        if (token.find(' ') != std::string::npos) continue;
        if (!RE2::FullMatch(token, kIdRe)) continue;
        if (RE2::PartialMatch(token, kCodeRe)) continue;
        return token;
    }
    return {};
}

std::string SeriesOf(std::string_view part) {
    // EF2L45LG144B -> EF2L / PH1A100 -> PH1A / EF2M45LG100 -> EF2M
    static const RE2 kSeriesRe(R"(^([A-Za-z]+\d+[A-Za-z]?))");
    const std::string body(part);
    std::string series;
    if (RE2::PartialMatch(body, kSeriesRe, &series)) {
        for (char& c : series) {
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        }
        return series;
    }
    std::string upper(body);
    for (char& c : upper) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return upper;
}

std::string LettersOf(std::string_view part) {
    static const RE2 kLettersRe(R"(^([A-Za-z]+))");
    const std::string body(part);
    std::string letters;
    if (RE2::PartialMatch(body, kLettersRe, &letters)) {
        for (char& c : letters) {
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        }
        return letters;
    }
    std::string upper(body);
    for (char& c : upper) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return upper;
}

DeviceComparison CompareDevices(std::string_view expected, std::string_view online) {
    DeviceComparison out;
    if (online.empty()) {
        out.match = false;
        out.warn = false;
        out.reason = "Cannot read the online device ID (read_device_id failed)";
        return out;
    }
    if (expected.empty()) {
        out.match = false;
        out.warn = true;
        out.reason = "Project has no target device";
        return out;
    }
    if (SeriesOf(expected) == SeriesOf(online)) {
        out.match = true;
        out.warn = false;
        return out;
    }
    if (LettersOf(expected) == LettersOf(online)) {
        out.match = false;
        out.warn = true;
        out.reason = "Same family, different device: project " + std::string(expected) +
                     " / online " + std::string(online);
        return out;
    }
    out.match = false;
    out.warn = false;
    out.reason = "Different family: project " + std::string(expected) + " (" +
                 SeriesOf(expected) + ") / online " + std::string(online) + " (" +
                 SeriesOf(online) + ")";
    return out;
}

BitCheck CheckBit(std::string_view path) {
    BitCheck out;
    if (path.empty()) {
        out.reason = "No .bit file specified";
        return out;
    }
    if (!IsFile(path)) {
        out.reason = "File does not exist: " + std::string(path);
        return out;
    }
    const std::string body(path);
    if (body.size() < 4 || body.compare(body.size() - 4, 4, ".bit") != 0) {
        // Python 用 lower().endswith(".bit")
        std::string lower(body);
        for (char& c : lower) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        if (lower.size() < 4 || lower.compare(lower.size() - 4, 4, ".bit") != 0) {
            out.reason = "Not a .bit file: " + body;
            return out;
        }
    }
    out.ok = true;
    out.size = FileSize(path);
    return out;
}

bool HasBwError(std::string_view text) {
    static const RE2 kErrorRe(R"(\b[A-Z]{2,6}-\d{3,5}\s+(?:ERROR|CRITICAL-WARNING)\b)");
    return RE2::PartialMatch(std::string(text), kErrorRe);
}

std::string ParseChipValidation(std::string_view text) {
    // 实测原文：`PRG-2014 : Chip validation success: PH1A100`
    static const RE2 kRe(R"(Chip validation success:\s*([A-Za-z][A-Za-z0-9_]+))");
    const std::string body(text);
    std::string part;
    if (RE2::PartialMatch(body, kRe, &part)) return part;
    return {};
}

bool HasDownloadSuccess(std::string_view text) {
    static const RE2 kRe(R"(Downloading succeeded|Download success|download succeeded)");
    return RE2::PartialMatch(std::string(text), kRe);
}

std::string DefaultBit(const project::Project& project) {
    const std::string runs = Join(project.dir, project.name + "_Runs");
    std::vector<std::string> candidates;
    for (const std::string& name : ListFiles(Join(runs, "best_result"))) {
        if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".bit") == 0) {
            candidates.push_back(Join(Join(runs, "best_result"), name));
        }
    }
    for (const std::string& name : ListFiles(Join(runs, "phy_1"))) {
        if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".bit") == 0) {
            candidates.push_back(Join(Join(runs, "phy_1"), name));
        }
    }
    if (candidates.empty()) return {};
    std::sort(candidates.begin(), candidates.end(),
              [](const std::string& a, const std::string& b) { return Mtime(a) > Mtime(b); });
    return Slashes(candidates.front());
}

jsonrpc::Json Refusal(std::string_view stage, std::string_view reason) {
    jsonrpc::Json out = jsonrpc::Json::object();
    out["ok"] = false;
    out["refused"] = true;
    out["stage"] = std::string(stage);
    out["reason"] = std::string(reason);
    return out;
}

// -----------------------------------------------------------------------------
// Inspect / Program / GenerateSvf
// -----------------------------------------------------------------------------

jsonrpc::Json Inspect(const td::TdChoice& td, const project::Project& project,
                      const ProgramOptions& options) {
    ProgramOptions effective = options;
    if (effective.cwd.empty()) effective.cwd = project.dir;

    jsonrpc::Json result = jsonrpc::Json::object();
    result["ok"] = false;
    result["expected"] = project.device;
    result["cables"] = jsonrpc::Json::array();
    result["device_id"] = "";
    result["match"] = false;
    result["warn"] = false;
    result["reason"] = "";

    const std::string bit =
        effective.bit_path.empty() ? DefaultBit(project) : Slashes(effective.bit_path);
    result["bit"] = bit;
    if (bit.empty()) {
        result["reason"] = "No .bit yet (run a flow or choose a file)";
        return result;
    }

    const BitCheck check = CheckBit(bit);
    {
        jsonrpc::Json check_json = jsonrpc::Json::object();
        check_json["ok"] = check.ok;
        check_json["reason"] = check.reason;
        check_json["size"] = check.size;
        result["bit_check"] = std::move(check_json);
    }

    std::string error;
    SessionScope scope(td, effective, &error);
    if (scope.get() == nullptr) {
        result["reason"] = error.empty() ? std::string("cannot start BitWizard") : error;
        return result;
    }

    const std::string cables_text =
        scope.get()->Run("list_cable", effective.list_cable_timeout);
    const std::vector<CableInfo> cables = ParseCables(cables_text);
    jsonrpc::Json cables_json = jsonrpc::Json::array();
    for (const CableInfo& cable : cables) cables_json.push_back(cable.ToJson());
    result["cables"] = std::move(cables_json);
    if (cables.empty()) {
        result["reason"] =
            "No cable/board detected (list_cable is empty). Connect a USB cable and try again.";
        return result;
    }

    const std::string id_text = scope.get()->Run("read_device_id", effective.device_id_timeout);
    const std::string device_id = ParseDeviceId(id_text);
    result["device_id"] = device_id;

    const DeviceComparison comparison = CompareDevices(project.device, device_id);
    result["match"] = comparison.match;
    result["warn"] = comparison.warn;
    result["ok"] = comparison.match && check.ok;
    if (!comparison.match) {
        result["reason"] = comparison.reason;
    } else if (!check.ok) {
        result["reason"] = check.reason;
    } else {
        const std::size_t slash = bit.find_last_of('/');
        const std::string base = slash == std::string::npos ? bit : bit.substr(slash + 1);
        result["reason"] = "Ready to program: " + device_id + " <- " + base;
    }
    return result;
}

jsonrpc::Json Program(const td::TdChoice& td, const project::Project& project,
                      const ProgramOptions& options) {
    // ---- 安全规则 2：没有明确确认就绝对不烧 ----
    if (!options.confirm) {
        jsonrpc::Json refusal = Refusal(
            "confirm",
            "program requires confirm:true (the IDE must ask the user first); nothing was sent "
            "to the device");
        refusal["expected"] = project.device;
        return refusal;
    }

    std::string mode = options.mode;
    for (char& c : mode) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (mode != "sram" && mode != "flash") {
        jsonrpc::Json out = jsonrpc::Json::object();
        out["ok"] = false;
        out["error"] = "Unknown mode " + options.mode + " (sram / flash)";
        return out;
    }

    ProgramOptions effective = options;
    if (effective.cwd.empty()) effective.cwd = project.dir;

    // ---- 预检（含安全规则 1：list_cable 为空则拒绝）----
    const jsonrpc::Json pre = Inspect(td, project, effective);
    bool has_cables = false;
    if (const auto cables = pre.find("cables"); cables != pre.end() && cables->is_array()) {
        has_cables = !cables->empty();
    }
    if (!has_cables) {
        jsonrpc::Json refusal = Refusal(
            "cable", pre.contains("reason") && pre["reason"].is_string()
                         ? pre["reason"].get<std::string>()
                         : "No cable/board detected (list_cable is empty)");
        refusal["expected"] = project.device;
        refusal["device_id"] = pre.value("device_id", std::string());
        refusal["bit"] = pre.value("bit", std::string());
        return refusal;
    }

    // ---- 安全规则 3：器件不匹配 -> 一律拒绝 ----
    // ⚠ 与 Python 的**唯一一处有意收紧**（task-19 §3.2 的硬要求）：
    //   Python 在 allow_mismatch=true 时允许"同系列不同型号"直接烧；
    //   这里连那种情况也拒绝，因为烧错 bitstream 的代价是硬件。
    //   客户端要二次确认时必须换成匹配的工程/重新综合，而不是绕过这道闸门。
    const bool match = pre.value("match", false);
    if (!match) {
        const std::string reason = pre.value("reason", std::string("device mismatch"));
        jsonrpc::Json refusal = Refusal("device", reason);
        refusal["expected"] = project.device;
        refusal["device_id"] = pre.value("device_id", std::string());
        refusal["bit"] = pre.value("bit", std::string());
        refusal["allowMismatchIgnored"] = options.allow_mismatch;
        refusal["hint"] =
            "the bitstream must match the attached device; build the project for " +
            pre.value("device_id", std::string());
        return refusal;
    }

    const std::string bit =
        effective.bit_path.empty() ? DefaultBit(project) : Slashes(effective.bit_path);
    const BitCheck check = CheckBit(bit);
    if (!check.ok) {
        jsonrpc::Json refusal = Refusal("bit", check.reason);
        refusal["bit"] = bit;
        return refusal;
    }

    // ⚠ B7 / S-01：bit 路径可能来自 LSP 参数或 `<project>/<Name>.bit`（<Name> 来自
    //   .al），旧实现用裸 `{...}` 包，路径里一个 `}` 就能跳出并追加命令。
    //   TclQuoteGrouped 对正常路径输出仍是 `{Y:/x.bit}`（与既有测试/契约一致）。
    std::string style = effective.command_style.empty() ? std::string("download")
                                                        : effective.command_style;
    for (char& c : style) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    const std::string quoted = td::TclQuoteGrouped(bit);
    std::string command;
    if (style == "legacy") {
        command = (mode == "sram" ? std::string("program ") : std::string("program_spi ")) + quoted;
    } else {
        // 2026.2 的 GUI 实际下发形态（本机有成功日志），详见 programmer.h 顶部说明。
        command = std::string("download -mode ") +
                  (mode == "sram" ? std::string("jtag") : std::string("program_spi")) + " -bit " +
                  quoted + " -spd " + std::to_string(effective.speed) + " -cable " +
                  std::to_string(effective.cable);
        if (mode == "flash") {
            command += " -flashsize " + std::to_string(effective.flash_size) + " -v";
        }
    }
    const std::string label = mode == "sram" ? "SRAM (JTAG)" : "Flash (SPI)";

    std::string error;
    SessionScope scope(td, effective, &error);
    if (scope.get() == nullptr) {
        jsonrpc::Json out = jsonrpc::Json::object();
        out["ok"] = false;
        out["error"] = error.empty() ? std::string("cannot start BitWizard") : error;
        out["command"] = command;
        return out;
    }

    const std::string out_text = scope.get()->Run(command, effective.program_timeout);
    const std::vector<std::string> tail = TailLines(out_text, 40);
    // 2026.2 的成功凭据：`PRG-2014 : Chip validation success: <系列>`（download 一定打印）
    // 和收尾的 `GUI-1001 : Downloading succeeded!`。**退出码不可信**（出错也返回 0），
    // 所以"没有成功标记"同样判失败 —— 不能因为日志里没有 ERROR 就当成烧好了。
    const std::string validation = ParseChipValidation(out_text);
    const bool success_marker = HasDownloadSuccess(out_text);
    const bool bw_error = HasBwError(out_text);
    const bool ok = !bw_error && (!validation.empty() || success_marker);

    jsonrpc::Json out = jsonrpc::Json::object();
    out["ok"] = ok;
    out["mode"] = mode;
    out["mode_label"] = label;
    out["command_style"] = style;
    out["bit"] = bit;
    out["device_id"] = pre.value("device_id", std::string());
    out["expected"] = project.device;
    out["chip_validation"] = validation;
    out["command"] = command;
    out["log"] = TailJson(tail);
    if (ok) {
        out["error"] = "";
    } else if (bw_error) {
        out["error"] =
            "BitWizard error: " + (tail.empty() ? std::string("see logs") : tail.back());
    } else {
        out["error"] =
            "BitWizard reported no error but also no success marker "
            "(expected 'Chip validation success:' or 'Downloading succeeded') - see logs";
    }
    return out;
}

jsonrpc::Json GenerateSvf(const td::TdChoice& td, const project::Project& project,
                          const ProgramOptions& options) {
    ProgramOptions effective = options;
    if (effective.cwd.empty()) effective.cwd = project.dir;

    const std::string bit =
        effective.bit_path.empty() ? DefaultBit(project) : Slashes(effective.bit_path);
    const BitCheck check = CheckBit(bit);
    if (!check.ok) {
        jsonrpc::Json out = jsonrpc::Json::object();
        out["ok"] = false;
        out["error"] = check.reason;
        return out;
    }

    std::string svf = effective.svf_path;
    if (svf.empty()) {
        const std::size_t slash = bit.find_last_of('/');
        const std::string dir = slash == std::string::npos ? std::string() : bit.substr(0, slash);
        std::string stem = slash == std::string::npos ? bit : bit.substr(slash + 1);
        if (const std::size_t dot = stem.find_last_of('.'); dot != std::string::npos) {
            stem.erase(dot);
        }
        svf = (dir.empty() ? std::string() : dir + "/") + stem + ".svf";
    }
    svf = Slashes(std::move(svf));

    const std::string scan_mode =
        effective.scan_mode.empty() ? std::string("prog_sram") : effective.scan_mode;
    // ⚠ B7 / S-01：scan_mode 直接来自 LSP 参数（fpgalsp.svfgen 的 scanMode），
    //   旧实现裸拼 -> 一条 `prog_sram; exec ...` 就能注入。它不是路径，没有理由
    //   含元字符：非法即安全失败（返回错误，不下发任何命令）。
    if (!td::IsTclToken(scan_mode)) {
        jsonrpc::Json out = jsonrpc::Json::object();
        out["ok"] = false;
        out["bit"] = bit;
        out["error"] = "Invalid scanMode \"" + scan_mode +
                       "\" (expected a plain token such as prog_sram)";
        return out;
    }
    const std::string command = "svfgen -bit " + td::TclQuoteGrouped(bit) + " -svf " +
                                td::TclQuoteGrouped(svf) + " -scan_mode " + scan_mode;

    std::string error;
    SessionScope scope(td, effective, &error);
    if (scope.get() == nullptr) {
        jsonrpc::Json out = jsonrpc::Json::object();
        out["ok"] = false;
        out["error"] = error.empty() ? std::string("cannot start BitWizard") : error;
        out["command"] = command;
        return out;
    }

    const std::string out_text = scope.get()->Run(command, effective.svf_timeout);
    const bool exists = IsFile(svf);

    jsonrpc::Json out = jsonrpc::Json::object();
    out["ok"] = exists && !HasBwError(out_text);
    out["bit"] = bit;
    out["svf"] = svf;
    out["scan_mode"] = scan_mode;
    out["command"] = command;
    out["size"] = exists ? FileSize(svf) : 0;
    out["error"] = exists ? "" : "No .svf was generated (see BitWizard output)";
    out["log"] = TailJson(TailLines(out_text, 20));
    return out;
}

}  // namespace fpgalsp::flow