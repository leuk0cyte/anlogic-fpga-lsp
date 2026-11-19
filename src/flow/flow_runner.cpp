// =============================================================================
//  src/flow/flow_runner.cpp —— Flow 引擎实现
// =============================================================================
//  逐条对照 Y:\fpga-lsp\fpgalsp\flow.py::FlowRunner 实现。
//  Python 用异常表达失败（TdError）；这里改成显式的 Failure 返回值，
//  因为整个 worker 跑在独立线程里，异常逃出线程会直接 terminate 进程。
// =============================================================================

// std::getenv 在 MSVC 上是 C4996；这里读的是用户设置的环境变量，风险为零。
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "flow/flow_runner.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <re2/re2.h>

#include "flow/flow_logic.h"
#include "platform/encoding.h"
#include "reports/reports_aggregate.h"
#include "td/td_device.h"
#include "td/td_output.h"
#include "td/td_session.h"
#include "td/tcl_quote.h"

namespace fpgalsp::flow {
namespace {

namespace fs = std::filesystem;

// -----------------------------------------------------------------------------
// 小工具（全部 never-throw）
// -----------------------------------------------------------------------------

std::string Slashes(std::string text) {
    for (char& c : text) {
        if (c == '\\') c = '/';
    }
    return text;
}

std::string Join(std::string_view dir, std::string_view name) {
    if (dir.empty()) return std::string(name);
    std::string out(dir);
    if (out.back() != '/' && out.back() != '\\') out.push_back('/');
    out += name;
    return Slashes(std::move(out));
}

bool IsFile(std::string_view path) {
    if (path.empty()) return false;
    std::error_code ec;
    return fs::is_regular_file(fs::path(std::string(path)), ec) && !ec;
}

bool IsDirectory(std::string_view path) {
    if (path.empty()) return false;
    std::error_code ec;
    return fs::is_directory(fs::path(std::string(path)), ec) && !ec;
}

bool MakeDirs(std::string_view path) {
    if (path.empty()) return false;
    std::error_code ec;
    fs::create_directories(fs::path(std::string(path)), ec);
    return !ec || fs::is_directory(fs::path(std::string(path)), ec);
}

std::vector<std::string> ListDir(std::string_view dir) {
    std::vector<std::string> names;
    if (dir.empty() || !IsDirectory(dir)) return names;
    std::error_code ec;
    fs::directory_iterator it(fs::path(std::string(dir)), ec);
    if (ec) return names;
    const fs::directory_iterator end;
    while (it != end) {
        const std::u8string name = it->path().filename().u8string();
        names.emplace_back(reinterpret_cast<const char*>(name.data()), name.size());
        it.increment(ec);
        if (ec) break;
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::string> ListSubdirs(std::string_view dir) {
    std::vector<std::string> names;
    if (dir.empty() || !IsDirectory(dir)) return names;
    std::error_code ec;
    fs::directory_iterator it(fs::path(std::string(dir)), ec);
    if (ec) return names;
    const fs::directory_iterator end;
    while (it != end) {
        std::error_code status_ec;
        if (fs::is_directory(it->path(), status_ec) && !status_ec) {
            const std::u8string name = it->path().filename().u8string();
            names.emplace_back(reinterpret_cast<const char*>(name.data()), name.size());
        }
        it.increment(ec);
        if (ec) break;
    }
    std::sort(names.begin(), names.end());
    return names;
}

double Mtime(std::string_view path) {
    if (path.empty()) return 0.0;
    std::error_code ec;
    const auto time = fs::last_write_time(fs::path(std::string(path)), ec);
    if (ec) return 0.0;
    const auto system_time = std::chrono::clock_cast<std::chrono::system_clock>(time);
    return std::chrono::duration<double>(system_time.time_since_epoch()).count();
}

bool WriteTextFile(std::string_view path, std::string_view text) {
    if (path.empty()) return false;
    std::error_code ec;
    fs::create_directories(fs::path(std::string(path)).parent_path(), ec);
    return platform::write_file_utf8(path, text);
}

std::string ReadTextFile(std::string_view path) {
    const auto result = platform::try_read_file_utf8(path);
    return result.has_value() ? *result : std::string();
}

std::string BaseName(std::string_view path) {
    const std::size_t pos = path.find_last_of("/\\");
    return pos == std::string_view::npos ? std::string(path) : std::string(path.substr(pos + 1));
}

bool EndsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

double Round1(double value) { return std::nearbyint(value * 10.0) / 10.0; }

std::string Hex8(unsigned long long value) {
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%08llx", value);
    return buffer;
}

std::string NormaliseNewlines(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c == '\r') c = '\n';
    }
    return out;
}

// 对应 Python 的 run.log 报错行提取（heartbeat 与 failure_detail 共用）
std::string RunLogErrorTail(std::string_view run_log_path) {
    const std::string text = ReadTextFile(run_log_path);
    if (text.empty()) return {};
    static const RE2 kWarningRe(R"(\b(?:ERROR|CRITICAL-WARNING|WARNING)\b)");
    const std::string normalised = NormaliseNewlines(text);
    std::vector<std::string> hits;
    std::size_t start = 0;
    while (start <= normalised.size()) {
        const std::size_t pos = normalised.find('\n', start);
        const std::string line = pos == std::string::npos
                                     ? normalised.substr(start)
                                     : normalised.substr(start, pos - start);
        std::string_view trimmed(line);
        while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) {
            trimmed.remove_prefix(1);
        }
        while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t')) {
            trimmed.remove_suffix(1);
        }
        if (!trimmed.empty() && trimmed.front() != '=' &&
            RE2::PartialMatch(std::string(trimmed), kWarningRe)) {
            hits.emplace_back(trimmed);
        }
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    if (hits.empty()) return {};
    const std::size_t from = hits.size() > 3 ? hits.size() - 3 : 0;
    std::string out;
    for (std::size_t i = from; i < hits.size(); ++i) {
        if (!out.empty()) out += " / ";
        out += hits[i];
    }
    return out;
}

// -----------------------------------------------------------------------------
// 真实 TD 会话
// -----------------------------------------------------------------------------

class TdFlowSession final : public FlowSession {
public:
    TdFlowSession(const td::TdChoice& td, const std::string& work_dir) {
        td::TdSessionOptions options;
        options.work_dir = work_dir;
        session_ = std::make_unique<td::TdSession>(td, options);  // 启动失败抛 TdError
    }

    std::string Run(std::string_view tcl, double timeout_seconds) override {
        if (!session_) return {};
        try {
            return session_->Run(tcl, timeout_seconds);
        } catch (const std::exception& exc) {
            // 超时 / 管道断开：记录原因，会话标记为不可用（不把异常抛给 flow 线程）
            last_error_ = exc.what();
            dead_ = true;
            return {};
        }
    }

    void Kill() override {
        if (!session_) return;
        // ⚠ 只杀一次：Cancel（调用线程）与 heartbeat（看门狗线程）都可能调到这里，
        //   TdSession::Kill() 会 CloseHandle(job)，并发调用会重复关同一个 handle。
        bool expected = false;
        if (!killed_.compare_exchange_strong(expected, true)) return;
        try {
            session_->Kill();  // Job Object 终止整棵进程树 -> 不留孤儿
        } catch (...) {
        }
        dead_ = true;
    }

    void Close() override {
        if (!session_) return;
        // ⚠ 不要显式调 session_->Close()：TdSession 的析构函数**自己会** Close()，
        //   显式调一次 + 析构再调一次会让 impl_->Cleanup() 跑两遍（重复关句柄）。
        //   这里只放掉所有权，让析构走唯一的那一次 Close。
        std::unique_ptr<td::TdSession> dying = std::move(session_);
        try {
            dying.reset();
        } catch (...) {
            // 析构里抛异常会 terminate，这里兜不住；至少不让它影响调用方
        }
    }

    std::string LastError() const override { return last_error_; }
    bool Dead() const { return dead_; }

private:
    std::unique_ptr<td::TdSession> session_;
    std::string last_error_;
    std::atomic<bool> killed_{false};
    bool dead_ = false;
};

// 一次步骤的失败（对应 Python 里抛出的 TdError）
struct Failure {
    bool failed = false;
    std::string reason;
};

Failure Fail(std::string reason) { return Failure{true, std::move(reason)}; }
Failure Ok() { return Failure{}; }

}  // namespace

// -----------------------------------------------------------------------------
// 环境 / 默认目录
// -----------------------------------------------------------------------------

double StallSecondsFromEnv(double fallback) {
    const char* raw = std::getenv("FPGALSP_FLOW_STALL_SECONDS");
    if (raw == nullptr || *raw == '\0') return fallback;
    char* end = nullptr;
    const double seconds = std::strtod(raw, &end);
    if (end == raw || !std::isfinite(seconds) || seconds <= 0.0) return fallback;
    return seconds;
}

std::string DefaultFlowRoot(const project::Project& project) {
    return Join(Join(project.dir, ".fpgalsp"), "flow");
}

std::string DefaultCacheRoot(const project::Project& project) {
    return Join(Join(project.dir, ".fpgalsp"), "cache");
}

// -----------------------------------------------------------------------------
// FlowState
// -----------------------------------------------------------------------------

jsonrpc::Json FlowState::ToJson() const {
    jsonrpc::Json out = jsonrpc::Json::object();
    out["running"] = running;
    out["stage"] = stage;
    out["stages"] = stages;
    out["runId"] = run_id;
    out["startedAt"] = started_at;
    out["elapsed"] = elapsed;
    out["lastMessage"] = last_message;
    if (ok.has_value()) {
        out["ok"] = *ok;
    } else {
        out["ok"] = nullptr;
    }
    out["error"] = error;
    out["log"] = log;
    out["jobs"] = jobs;
    return out;
}

// -----------------------------------------------------------------------------
// Impl
// -----------------------------------------------------------------------------

struct FlowRunner::Impl {
    FlowRunner* owner = nullptr;

    FlowRunner::NotifyFn notify;
    FlowRunner::LogFn log;

    mutable std::mutex mutex;
    FlowState state;

    // Start 本身也要串行：否则两个线程可能同时看到 running=false 并各起一条 worker
    std::mutex start_mutex;
    std::thread thread;
    std::atomic<bool> cancel{false};
    std::atomic<double> cancel_at{0.0};

    std::mutex session_mutex;
    std::unique_ptr<FlowSession> session;

    std::string run_dir;
    std::string stage_run;
    double stage_started = 0.0;
    std::string stall_reason;
    std::atomic<bool> heartbeat_stop{false};

    // TD 报告过但可能不存在的 run.log（对应 Python _run_logs）
    std::mutex logs_mutex;
    std::vector<std::pair<std::string, std::string>> run_logs;  // (run_name, path)

    std::function<double()> clock;

    double Now() const {
        if (clock) return clock();
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        return std::chrono::duration<double>(now).count();
    }

    FlowSession* Session() {
        std::lock_guard<std::mutex> lock(session_mutex);
        return session.get();
    }
    void SetSession(std::unique_ptr<FlowSession> value) {
        std::lock_guard<std::mutex> lock(session_mutex);
        session = std::move(value);
    }
    void KillSession() {
        std::lock_guard<std::mutex> lock(session_mutex);
        if (session) session->Kill();
    }
    std::string SessionError() {
        std::lock_guard<std::mutex> lock(session_mutex);
        return session ? session->LastError() : std::string();
    }

    bool Cancelled() const { return cancel.load(); }
    void SetCancel(bool value) { cancel.store(value); }

    void SetStallReason(std::string reason) {
        std::lock_guard<std::mutex> lock(mutex);
        stall_reason = std::move(reason);
    }
    std::string GetStallReason() {
        std::lock_guard<std::mutex> lock(mutex);
        return stall_reason;
    }

    void RememberRunLog(const std::string& run_name, std::string_view text) {
        const std::string path = ReportedRunLogPath(text);
        if (path.empty()) return;
        std::lock_guard<std::mutex> lock(logs_mutex);
        for (auto& [name, existing] : run_logs) {
            if (name == run_name) {
                existing = path;
                return;
            }
        }
        run_logs.emplace_back(run_name, path);
    }

    std::string ReportedLog(const std::string& run_name) {
        std::lock_guard<std::mutex> lock(logs_mutex);
        for (const auto& [name, path] : run_logs) {
            if (name == run_name) return path;
        }
        return {};
    }

    std::vector<std::string> RunLogCandidates(const std::string& run_name) {
        std::vector<std::string> paths;
        const std::string reported = ReportedLog(run_name);
        if (!reported.empty()) paths.push_back(Slashes(reported));
        paths.push_back(Join(RunDirOf(run_name), "run.log"));
        std::vector<std::string> out;
        for (const std::string& path : paths) {
            const std::string normalised = Slashes(path);
            bool seen = false;
            for (const std::string& existing : out) {
                if (_stricmp(existing.c_str(), normalised.c_str()) == 0) seen = true;
            }
            if (!seen) out.push_back(normalised);
        }
        return out;
    }

    std::string RunsRoot() const {
        return Join(owner->project_.dir, owner->project_.name + "_Runs");
    }
    std::string RunDirOf(const std::string& run_name) const { return Join(RunsRoot(), run_name); }

    // ---- 事件 ----
    void Emit(const std::string& type, const std::string& message,
              std::optional<bool> ok = std::nullopt, jsonrpc::Json extra = jsonrpc::Json::object()) {
        jsonrpc::Json payload = jsonrpc::Json::object();
        {
            std::lock_guard<std::mutex> lock(mutex);
            const double elapsed =
                state.started_at > 0.0 ? Round1(Now() - state.started_at) : 0.0;
            if (!message.empty()) {
                state.last_message = message;
                state.log.push_back(FormatLogLine(elapsed, state.stage, message));
                state.log = TrimLog(std::move(state.log), 200);
            }
            if (type == "done" || type == "error") state.elapsed = elapsed;
            payload["runId"] = state.run_id;
            payload["type"] = type;
            payload["stage"] = state.stage;
            payload["elapsed"] = elapsed;
            payload["message"] = message;
            payload["ok"] = ok.has_value() ? jsonrpc::Json(*ok) : jsonrpc::Json(nullptr);
        }
        for (auto it = extra.begin(); it != extra.end(); ++it) payload[it.key()] = it.value();
        if (notify) {
            try {
                notify(payload);
            } catch (...) {
            }
        }
    }

    // 对应 Python _record：把一段输出拆成非空行逐条发 log
    void Record(std::string_view text) {
        const std::string normalised = NormaliseNewlines(text);
        std::size_t start = 0;
        while (start <= normalised.size()) {
            const std::size_t pos = normalised.find('\n', start);
            const std::string line = pos == std::string::npos
                                         ? normalised.substr(start)
                                         : normalised.substr(start, pos - start);
            std::string_view trimmed(line);
            while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) {
                trimmed.remove_prefix(1);
            }
            while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t')) {
                trimmed.remove_suffix(1);
            }
            if (!trimmed.empty()) Emit("log", std::string(trimmed));
            if (pos == std::string::npos) break;
            start = pos + 1;
        }
    }

    void SetStage(const std::string& stage) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            state.stage = stage;
        }
        Emit("step", "Starting " + StageLabel(stage));
    }

    void LogToHost(const std::string& message) {
        if (!log) return;
        try {
            log(3, message);  // 3 = LSP MessageType.Log/Info
        } catch (...) {
        }
    }

    // ---- 运行目录 ----
    void ClearStopFlags() {
        for (const std::string& name : ListSubdirs(RunsRoot())) {
            const std::string stop = Join(RunDirOf(name), ".stop.f");
            if (IsFile(stop)) {
                std::error_code ec;
                fs::remove(fs::path(stop), ec);
            }
        }
    }

    void WriteStopFiles() {
        for (const std::string& name : ListSubdirs(RunsRoot())) {
            if (name == "best_result") continue;
            (void)WriteTextFile(Join(RunDirOf(name), ".stop.f"), "stop\n");
        }
    }

    std::string RunDir() const {
        std::lock_guard<std::mutex> lock(mutex);
        return run_dir;
    }
    void SetRunDir(const std::string& value) {
        std::lock_guard<std::mutex> lock(mutex);
        run_dir = value;
    }
    void SetStageRun(const std::string& value) {
        std::lock_guard<std::mutex> lock(mutex);
        stage_run = value;
    }
    std::string GetStageRun() {
        std::lock_guard<std::mutex> lock(mutex);
        return stage_run;
    }
    void SetStageStarted(double value) {
        std::lock_guard<std::mutex> lock(mutex);
        stage_started = value;
    }
    double GetStageStarted() {
        std::lock_guard<std::mutex> lock(mutex);
        return stage_started;
    }
    bool IsRunning() {
        std::lock_guard<std::mutex> lock(mutex);
        return state.running;
    }

    // -------------------------------------------------------------------------
    // 判定所需的现场快照
    // -------------------------------------------------------------------------
    RunSnapshot Snapshot(const std::string& run_name) {
        RunSnapshot snapshot;
        snapshot.run_name = run_name;
        const std::string dir = RunDirOf(run_name);
        snapshot.run_dir = dir;
        snapshot.run_dir_names = ListDir(dir);
        snapshot.project_name = owner->project_.name;
        snapshot.project_bit_exists = IsFile(Join(dir, owner->project_.name + ".bit"));

        for (const std::string& candidate : RunLogCandidates(run_name)) {
            if (IsFile(candidate)) {
                snapshot.run_log_tail = RunLogErrorTail(candidate);
                break;
            }
            const std::string reported = ReportedLog(run_name);
            if (!reported.empty() && Slashes(reported) == candidate) {
                snapshot.missing_run_log = candidate;
            }
        }
        return snapshot;
    }

    double RunActivityMtime(const std::string& run_name) {
        const std::string dir = RunDirOf(run_name);
        double newest = 0.0;
        for (const std::string& name : ListDir(dir)) {
            newest = std::max(newest, Mtime(Join(dir, name)));
        }
        return newest;
    }

    bool WaitForRunStart(const std::string& run_name, double timeout_seconds) {
        const double deadline = Now() + timeout_seconds;
        while (Now() < deadline) {
            if (!IsRunning() || Cancelled()) return false;
            if (RunStarted(ListDir(RunDirOf(run_name)))) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return RunStarted(ListDir(RunDirOf(run_name)));
    }

    // -------------------------------------------------------------------------
    // 步骤
    // -------------------------------------------------------------------------
    Failure ImportDevice() {
        std::vector<std::string> candidates;
        if (!owner->options_.explicit_db.empty()) {
            candidates.push_back(owner->options_.explicit_db);
        } else {
            candidates = td::DbCandidates(owner->td_.root, owner->project_.family);
        }
        if (candidates.empty()) {
            return Fail("no device database (arch/*.db) under " + owner->td_.root);
        }

        std::string last_error;
        for (const std::string& db : candidates) {
            if (Cancelled()) return Fail("cancelled");
            FlowSession* session = Session();
            if (session == nullptr) return Fail("flow session not started");

            const std::string command =
                td::MakeImportDeviceCommand(db, owner->project_.device, owner->project_.speed);
            const std::string out = session->Run(command, 180.0);
            if (!td::HasError(out)) {
                Record("device: " + owner->project_.device + " (" + db + ")");
                return Ok();
            }
            last_error = ErrorLines(out, 1);
        }
        return Fail("import_device failed: " + last_error);
    }

    Failure RunStage(const std::string& run_name, int jobs) {
        ClearStopFlags();
        Emit("progress", "reset_runs {" + run_name + "}");
        // B7 / S-01：run_name 目前都是内部常量（syn_1/phy_1），但仍按"不可信"处理，
        // 走 TclQuoteGrouped —— 正常名字输出与旧实现逐字节相同（`{ syn_1 }`）。
        Record(RunCommand("reset_runs " + td::TclQuoteGrouped(" " + run_name + " "), 120.0));

        Emit("progress", "launch_runs {" + run_name + "} -jobs " + std::to_string(jobs));
        const std::string launch = "launch_runs " + td::TclQuoteGrouped(" " + run_name + " ") +
                                   " -jobs " + std::to_string(jobs);
        std::string out = RunCommand(launch, 300.0);
        RememberRunLog(run_name, out);
        if (td::HasError(out)) {
            Record(out);
            RunSnapshot snapshot = Snapshot(run_name);
            return Fail("launch_runs " + run_name + " failed: " +
                        BuildFailureDetail(out, ErrorMarkers(snapshot.run_dir_names),
                                           snapshot.run_log_tail, snapshot.run_dir));
        }
        Record(out);

        if (!WaitForRunStart(run_name, owner->options_.start_timeout_seconds)) {
            return Fail(run_name +
                        " did not start: no run trace (.read_design.begin.f / run.log) appeared. "
                        "The most common cause is an empty device_name in settings.cfg (PRJ-8001) "
                        "or a stale TD process; see " +
                        RunDirOf(run_name) + " and " + RunDir());
        }

        const double started = Now();
        SetStageRun(run_name);
        SetStageStarted(started);
        SetStallReason({});

        out = RunCommand("wait_run " + td::TclQuote(run_name) + " -quiet",
                         owner->options_.timeout_seconds);
        RememberRunLog(run_name, out);
        SetStageRun({});
        const double elapsed = Now() - started;

        RunSnapshot snapshot = Snapshot(run_name);
        const RunOutcome outcome = DecideRunOutcome(snapshot, out);
        if (outcome.accepted) {
            if (outcome.tolerated_prj_error) {
                // 产物齐全、只是 TD 报了个 PRJ-8806：记下原文与结论，便于事后追查
                Record(out);
                Emit("log", run_name +
                                " completed despite TD PRJ-8806; artifacts are present");
            }
        } else {
            return Fail(run_name + " failed: " + outcome.failure_detail);
        }

        jsonrpc::Json extra = jsonrpc::Json::object();
        extra["run"] = run_name;
        extra["runElapsed"] = Round1(elapsed);
        char message[160];
        std::snprintf(message, sizeof(message), "%s complete (%.1fs)", run_name.c_str(), elapsed);
        Emit("step", message, std::nullopt, extra);
        return Ok();
    }

    std::string RunCommand(const std::string& tcl, double timeout_seconds) {
        FlowSession* session = Session();
        if (session == nullptr) return {};
        return session->Run(tcl, timeout_seconds);
    }

    Failure RunRouteOnly() {
        const std::string phy_dir = RunDirOf("phy_1");
        std::vector<std::string> candidates;
        for (const std::string& name : ListDir(phy_dir)) {
            if (!EndsWith(name, "_place.db") && !EndsWith(name, "_route.db")) continue;
            const std::string path = Join(phy_dir, name);
            if (IsFile(path)) candidates.push_back(path);
        }
        if (candidates.empty()) {
            return Fail("No reusable placement database (no *_place.db under " + phy_dir +
                        "): run Place & Route first");
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const std::string& a, const std::string& b) { return Mtime(a) > Mtime(b); });
        const std::string source_db = Slashes(candidates.front());

        SetStage("route");
        Emit("progress", "import_db " + BaseName(source_db));
        std::string out = RunCommand("import_db " + td::TclQuoteGrouped(source_db), 300.0);
        if (td::HasFatal(out)) return Fail("import_db failed: " + ErrorLines(out));

        const double t0 = Now();
        out = RunCommand("route", owner->options_.timeout_seconds);
        if (td::HasFatal(out)) return Fail("route failed: " + ErrorLines(out));
        char message[160];
        std::snprintf(message, sizeof(message), "route complete (%.1fs)", Now() - t0);
        Emit("step", message);

        for (const char* command : {"fix_hold", "update_timing -mode final"}) {
            out = RunCommand(command, 300.0);
            if (td::HasFatal(out)) return Fail(std::string(command) + " failed: " + ErrorLines(out));
        }

        const std::string dir = RunDir();
        const std::string bit = Slashes(Join(dir, owner->project_.name + ".bit"));
        const std::string timing = Join(dir, "route_timing.rpt");
        const std::string qor = Join(dir, "route.qor");
        const std::string area = Join(dir, "route.area");

        std::vector<std::pair<std::string, std::string>> produced_targets = {
            {"bitgen -bit " + td::TclQuoteGrouped(bit), bit},
            {"report_timing_summary -file " + td::TclQuoteGrouped(timing), timing},
            {"report_qor -step route -file " + td::TclQuoteGrouped(qor), qor},
            {"report_area -file " + td::TclQuoteGrouped(area), area},
        };
        std::vector<std::pair<std::string, std::string>> produced;  // (扩展名, 路径)
        for (const auto& [command, target] : produced_targets) {
            out = RunCommand(command, 900.0);
            if (td::HasFatal(out)) Record(out);
            if (IsFile(target)) {
                const std::size_t dot = BaseName(target).find_last_of('.');
                produced.emplace_back(dot == std::string::npos ? std::string()
                                                               : BaseName(target).substr(dot + 1),
                                      Slashes(target));
            }
        }

        const auto pick = [&produced](const std::string& key) {
            for (const auto& [name, path] : produced) {
                if (name == key) return path;
            }
            return std::string();
        };

        jsonrpc::Json payload = jsonrpc::Json::object();
        payload["kind"] = "route_only";
        payload["generatedAt"] = Now();
        payload["elapsed"] = Round1(Now() - started_at_value());
        payload["source_db"] = source_db;
        payload["bit"] = pick("bit");
        payload["timing"] = pick("rpt");
        payload["qor"] = pick("qor");
        payload["area"] = pick("area");

        const std::string cache = owner->options_.cache_root;
        if (MakeDirs(cache)) {
            (void)WriteTextFile(Join(cache, "last_flow.json"),
                                jsonrpc::Serialize(payload, /*compact=*/false));
        }

        Emit("step", "Incremental flow: rerunning route only (reusing " + BaseName(source_db) +
                         ")");
        return Ok();
    }

    double started_at_value() {
        std::lock_guard<std::mutex> lock(mutex);
        return state.started_at;
    }

    // -------------------------------------------------------------------------
    // 心跳 / 看门狗
    // -------------------------------------------------------------------------
    void Heartbeat(std::vector<std::string> stages) {
        (void)stages;
        std::string last_message;
        std::vector<std::pair<std::string, std::string>> seen_warnings;
        static const RE2 kWarningRe(
            R"([A-Z][A-Z0-9]{1,5}-\d{3,5}\s+(?:ERROR|CRITICAL-WARNING|WARNING)|)"
            R"(\b(?:ERROR|CRITICAL-WARNING|WARNING)\b)");

        while (!heartbeat_stop.load()) {
            std::string stage;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!state.running) return;
                stage = state.stage;
            }
            if (stage != "syn" && stage != "phy") {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }
            const std::string run_name = stage == "syn" ? "syn_1" : "phy_1";
            const std::string run_base = RunDirOf(run_name);

            std::string log_path;
            for (const std::string& candidate : RunLogCandidates(run_name)) {
                if (IsFile(candidate)) {
                    log_path = candidate;
                    break;
                }
            }
            if (log_path.empty()) {
                const std::vector<std::string> candidates = RunLogCandidates(run_name);
                if (!candidates.empty()) log_path = candidates.front();
            }

            std::vector<std::string> tail;
            if (!log_path.empty() && IsFile(log_path)) {
                const std::string text = ReadTextFile(log_path);
                const std::string normalised = NormaliseNewlines(text);
                // 只取最后 20000 字符当作 tail（对应 Python seek(size-20000)）
                const std::size_t from =
                    normalised.size() > 20000 ? normalised.size() - 20000 : 0;
                std::size_t start = from;
                while (start <= normalised.size()) {
                    const std::size_t pos = normalised.find('\n', start);
                    tail.push_back(pos == std::string::npos
                                       ? normalised.substr(start)
                                       : normalised.substr(start, pos - start));
                    if (pos == std::string::npos) break;
                    start = pos + 1;
                }
                // 新出现的告警行转成 log（去重）
                for (const std::string& line : tail) {
                    std::string_view trimmed(line);
                    while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) {
                        trimmed.remove_prefix(1);
                    }
                    while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t')) {
                        trimmed.remove_suffix(1);
                    }
                    if (trimmed.empty() || !RE2::PartialMatch(std::string(trimmed), kWarningRe)) {
                        continue;
                    }
                    bool seen = false;
                    for (const auto& [path, text_line] : seen_warnings) {
                        if (path == log_path && text_line == trimmed) seen = true;
                    }
                    if (seen) continue;
                    seen_warnings.emplace_back(log_path, std::string(trimmed));
                    const std::string clipped = trimmed.size() > 300
                                                    ? std::string(trimmed.substr(0, 300))
                                                    : std::string(trimmed);
                    Emit("log", "[run.log] " + clipped);
                }
            }

            std::string phase;
            std::string message;
            if (!tail.empty()) {
                const PhaseDetection detection = DetectProgress(tail);
                phase = detection.phase;
                message = detection.message;
            } else {
                // 没有 run.log：用 TD 写下的步骤标记推导进度
                const std::vector<std::string> done = CompletedStepsFromMarkers(ListDir(run_base));
                if (!done.empty()) {
                    for (const PhaseToken& token : PhaseTokens()) {
                        if (std::string(token.token) == done.back()) phase = token.label;
                    }
                    message = "Completed steps: ";
                    for (std::size_t i = 0; i < done.size(); ++i) {
                        if (i != 0) message += ", ";
                        message += done[i];
                    }
                }
            }

            // 取消：先让 TD 自己按 .stop.f 收尾，超过宽限期才强杀
            if (Cancelled()) {
                const double grace = owner->options_.cancel_kill_grace_seconds;
                if (grace <= 0.0 || Now() - cancel_at.load() > grace) {
                    KillSession();
                    return;
                }
            }

            // stall 看门狗
            const std::string active = GetStageRun();
            if (!active.empty()) {
                const double activity = RunActivityMtime(active);
                const double now = Now();
                const std::string reason =
                    StallReason(activity, now, GetStageStarted(), owner->options_.stall_seconds);
                if (!reason.empty()) {
                    SetStallReason(active + " " + reason);
                    Emit("error", "Watchdog: " + active + " " + reason);
                    KillSession();
                    return;
                }
            }

            if (!message.empty() && message != last_message) {
                last_message = message;
                Emit("progress", (phase.empty() ? "" : phase + " · ") + message);
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(static_cast<int>(owner->options_.heartbeat_seconds *
                                                           1000.0)));
        }
    }

    // -------------------------------------------------------------------------
    // worker
    // -------------------------------------------------------------------------
    void Worker(const std::string& run_id, std::vector<std::string> stages, int jobs) {
        const std::string dir = Join(owner->options_.flow_root, run_id);
        SetRunDir(dir);
        if (owner->options_.write_run_logs) (void)MakeDirs(dir);

        std::thread heartbeat_thread;
        // ⚠⚠ 必须用 RAII 保证 join：Worker 里有**7 处**提前 return
        //   （import_device 失败 / open_project 失败 / run 失败 / 取消 …），
        //   任何一处直接 return 都会让局部的 joinable std::thread 在析构时
        //   std::terminate() -> Windows __fastfail -> 进程以 0xC0000409 崩掉。
        //   这不是 EH/编译选项问题，是 std::thread 的语言层面契约。
        struct JoinOnExit {
            std::thread& thread;
            ~JoinOnExit() {
                if (thread.joinable()) thread.join();
            }
        };
        JoinOnExit join_guard{heartbeat_thread};
        try {
            {
                std::string joined;
                for (const std::string& stage : stages) {
                    if (!joined.empty()) joined += " + ";
                    joined += StageLabel(stage);
                }
                char message[256];
                std::snprintf(message, sizeof(message), "TD job started: %s (jobs=%d)",
                              joined.c_str(), jobs);
                Emit("start", message);
            }

            // 建会话（真实 TD 启动失败 -> 这里接管成 error，不让异常逃出线程）
            std::unique_ptr<FlowSession> session;
            try {
                if (owner->options_.session_factory) {
                    session = owner->options_.session_factory(dir);
                } else {
                    session = std::make_unique<TdFlowSession>(owner->td_, dir);
                }
            } catch (const std::exception& exc) {
                FinishError(std::string("cannot start TD: ") + exc.what());
                return;
            }
            if (!session) {
                FinishError("cannot start TD session");
                return;
            }
            SetSession(std::move(session));

            if (owner->options_.heartbeat_seconds > 0.0) {
                heartbeat_thread = std::thread([this, stages]() {
                    // 看门狗线程同样不允许把异常带出去（它会在 Kill 会话后继续跑）
                    try {
                        Heartbeat(stages);
                    } catch (...) {
                    }
                });
            }

            if (Failure failure = ImportDevice(); failure.failed) {
                FinishError(failure.reason);
                return;
            }

            if (stages.size() == 1 && stages[0] == "route") {
                if (Failure failure = RunRouteOnly(); failure.failed) {
                    FinishError(failure.reason);
                    return;
                }
            } else {
                // Python 不检查 open_project 的输出（失败会在 launch_runs 阶段暴露），保持一致
                (void)RunCommand("open_project " + td::TclQuoteGrouped(Slashes(owner->project_.al_path)),
                         300.0);
                Emit("progress", "Project loaded: " + owner->project_.name);

                for (const std::string& stage : stages) {
                    if (stage == "syn") {
                        SetStage("syn");
                        if (Failure failure = RunStage("syn_1", jobs); failure.failed) {
                            FinishError(failure.reason);
                            return;
                        }
                    } else if (stage == "phy") {
                        SetStage("phy");
                        if (Failure failure = RunStage("phy_1", jobs); failure.failed) {
                            FinishError(failure.reason);
                            return;
                        }
                        Emit("progress", "save_best_bits");
                        Record(RunCommand("save_best_bits", 300.0));
                    }
                }
            }

            Emit("step", "Collecting reports");
            const reports::ReportsBundle bundle = reports::BuildReports(owner->project_);
            const jsonrpc::Json report_json = bundle.ToJson();

            if (Cancelled()) {
                FinishError("cancelled", /*cancelled=*/true);
                return;
            }

            const std::string summary = SummaryFromReports(report_json);
            {
                std::lock_guard<std::mutex> lock(mutex);
                state.running = false;
                state.stage = "done";
                state.ok = true;
            }
            Emit("done", summary, true);
        } catch (const std::exception& exc) {
            FinishError(exc.what());
        } catch (...) {
            FinishError("unknown error");
        }

        // 收尾：停心跳、关会话
        heartbeat_stop.store(true);
        if (heartbeat_thread.joinable()) heartbeat_thread.join();
        // 注意：会话所有权在 SetSession 时已经转进 Impl，这里必须关 Impl 的那份。
        // （FinishError 里也关一次；重复 Close 是幂等的。）
        CloseSession();
    }

    void CloseSession() {
        std::lock_guard<std::mutex> lock(session_mutex);
        if (session) session->Close();
        session.reset();
    }

    // 给 catch 处理器用的安全版：FinishError 自己绝不能再抛
    // （它是在异常处理路径上被调用的，抛出去就直接 terminate 了）
    void SafeFinishError(std::string reason) noexcept {
        try {
            FinishError(std::move(reason));
        } catch (...) {
        }
    }

    void FinishError(std::string reason, bool cancelled = false) {
        const bool was_cancelled = cancelled || Cancelled();
        std::string effective = was_cancelled ? std::string("cancelled") : std::move(reason);
        if (!was_cancelled) {
            const std::string stall = GetStallReason();
            if (!stall.empty()) effective = stall;
            if (effective.empty()) {
                const std::string session_error = SessionError();
                if (!session_error.empty()) effective = session_error;
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            state.running = false;
            state.stage = was_cancelled ? "cancelled" : "error";
            state.ok = false;
            state.error = effective;
        }
        Emit("error", was_cancelled ? "Cancelled" : "Failed: " + effective, false);

        // 失败/取消也必须收尾（heartbeat 与 worker 的常驻路径都会走到这里）
        heartbeat_stop.store(true);
        CloseSession();
        LogToHost(was_cancelled ? "flow cancelled" : "flow failed: " + effective);
    }
};

// -----------------------------------------------------------------------------
// 公开 API
// -----------------------------------------------------------------------------

FlowRunner::FlowRunner(project::Project project, td::TdChoice td, NotifyFn notify, LogFn log,
                       FlowOptions options)
    : impl_(std::make_unique<Impl>()),
      project_(std::move(project)),
      td_(std::move(td)),
      options_(std::move(options)) {
    if (options_.stall_seconds <= 0.0) options_.stall_seconds = StallSecondsFromEnv(600.0);
    if (options_.flow_root.empty()) options_.flow_root = DefaultFlowRoot(project_);
    if (options_.cache_root.empty()) options_.cache_root = DefaultCacheRoot(project_);
    impl_->owner = this;
    impl_->notify = std::move(notify);
    impl_->log = std::move(log);
    impl_->state.stage = "idle";
}

FlowRunner::~FlowRunner() {
    impl_->SetCancel(true);
    impl_->heartbeat_stop.store(true);
    impl_->KillSession();
    if (impl_->thread.joinable()) impl_->thread.join();
    std::lock_guard<std::mutex> lock(impl_->session_mutex);
    if (impl_->session) impl_->session->Close();
    impl_->session.reset();
}

void FlowRunner::SetClockForTest(std::function<double()> clock) {
    impl_->clock = std::move(clock);
}

jsonrpc::Json FlowRunner::Start(const std::vector<std::string>& stages, int jobs) {
    std::lock_guard<std::mutex> start_lock(impl_->start_mutex);
    const std::vector<std::string> normalised = NormaliseStages(stages);
    if (normalised.empty()) {
        jsonrpc::Json out = jsonrpc::Json::object();
        out["ok"] = false;
        out["error"] = "unknown stage; use syn / phy / all";
        return out;
    }

    std::string run_id;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->state.running) {
            jsonrpc::Json out = jsonrpc::Json::object();
            out["ok"] = false;
            out["error"] = "busy";
            out["runId"] = impl_->state.run_id;
            return out;
        }
        run_id = "flow_" + Hex8(static_cast<unsigned long long>(
                                    std::chrono::steady_clock::now().time_since_epoch().count()) ^
                                static_cast<unsigned long long>(
                                    std::chrono::system_clock::now().time_since_epoch().count()));
        impl_->cancel.store(false);
        impl_->cancel_at.store(0.0);
        impl_->heartbeat_stop.store(false);
        impl_->state.running = true;
        impl_->state.stage = "starting";
        impl_->state.stages = normalised;
        impl_->state.run_id = run_id;
        impl_->state.started_at = impl_->Now();
        impl_->state.elapsed = 0.0;
        impl_->state.ok = std::nullopt;
        impl_->state.error.clear();
        impl_->state.last_message = "Preparing TD job";
        impl_->state.log.clear();
        impl_->state.jobs = jobs > 0 ? jobs : 6;
        impl_->stall_reason.clear();
        impl_->run_dir.clear();
        impl_->stage_run.clear();
        impl_->stage_started = 0.0;
    }

    if (impl_->thread.joinable()) impl_->thread.join();
    const int effective_jobs = jobs > 0 ? jobs : 6;
    impl_->thread = std::thread([this, run_id, normalised, effective_jobs]() {
        // ⚠⚠ 这个 catch-all 是**强制**的，不是为了"优雅"：
        //   异常一旦逃出 std::thread 的函数体就会 std::terminate()，而 Windows 上
        //   terminate 走 __fastfail(FAST_FAIL_FATAL_APP_EXIT) -> 进程以 0xC0000409 崩掉。
        //   触发路径真实存在：取消时另一线程 Kill() 掉会话，阻塞中的
        //   td::TdSession::ReadUntil 会 throw TdError（见 PYTHON-BUGS P-16）。
        //   后台线程 + 外部中断的场景（stall 看门狗、heartbeat、未来任何 worker）同理。
        try {
            impl_->Worker(run_id, normalised, effective_jobs);
        } catch (const std::exception& exc) {
            impl_->SafeFinishError(std::string("unexpected: ") + exc.what());
        } catch (...) {
            impl_->SafeFinishError("unexpected non-std exception");
        }
    });

    jsonrpc::Json out = jsonrpc::Json::object();
    out["ok"] = true;
    out["runId"] = run_id;
    out["stages"] = normalised;
    return out;
}

jsonrpc::Json FlowRunner::Cancel() {
    bool running = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        running = impl_->state.running;
        if (!running) {
            jsonrpc::Json out = jsonrpc::Json::object();
            out["ok"] = true;
            out["note"] = "not running";
            return out;
        }
        impl_->cancel.store(true);
        impl_->cancel_at.store(impl_->Now());
    }
    // 先写 .stop.f：TD 的 flow 每一步都会检查它，多数情况下会自己干净退出。
    // 只有超过 cancel_kill_grace_seconds 还没停（或 grace=0）才强杀进程树。
    impl_->WriteStopFiles();
    if (options_.cancel_kill_grace_seconds <= 0.0) {
        impl_->KillSession();
        impl_->Emit("log", "Cancel requested (.stop.f written; terminating the TD process)");
    } else {
        impl_->Emit("log", "Cancel requested (.stop.f written; waiting for TD to stop)");
    }
    jsonrpc::Json out = jsonrpc::Json::object();
    out["ok"] = true;
    return out;
}

FlowState FlowRunner::Status() const {
    FlowState state;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        state = impl_->state;
    }
    if (state.running && state.started_at > 0.0) {
        state.elapsed = Round1(impl_->Now() - state.started_at);
    }
    state.log = TrimLog(std::move(state.log), 200);
    return state;
}

bool FlowRunner::WaitUntilIdle(double timeout_seconds) {
    const double deadline = impl_->Now() + timeout_seconds;
    while (impl_->Now() < deadline) {
        if (!impl_->IsRunning()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return !impl_->IsRunning();
}

}  // namespace fpgalsp::flow