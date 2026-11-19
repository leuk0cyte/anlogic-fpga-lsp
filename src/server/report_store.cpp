// =============================================================================
//  src/server/report_store.cpp —— 报告缓存实现（Phase 2 / A3）
// =============================================================================
//  与 report_store.h 的设计说明配套。这里只列实现上的几个要点：
//    * 两把锁：`state_mutex_`（快照/工程状态，shared_mutex）与 `cache_mutex_`
//      （解析结果表）。**从不嵌套**：构建期间 `state_mutex_` 是写锁，
//      ReportParseCache 的回调只拿 `cache_mutex_`。
//    * `current_` 只在 `BuildReports` 期间指向正在构建的 ProjectState；接口本身
//      不带工程参数（reports 层的 ReportParseCache 是 per-build 的），所以这是
//      最小改动。单线程构建 + `state_mutex_` 写锁保证了它不会被并发改写。
//    * 解析结果以 `shared_ptr<const T>` 存放：命中的时候零拷贝、并且天然不可变。
//    * never-throw：所有失败（文件消失/读不到）都退化为"缓存未命中"，让
//      reports 层走它的普通路径（never-throw 的解析器）。
// =============================================================================

#include "server/report_store.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <utility>

#include "platform/path.h"
#include "reports/report_text.h"

namespace fpgalsp::server {
namespace {

namespace fs = std::filesystem;

std::int64_t MtimeNs(const fs::file_time_type& time) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
}

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

double ElapsedMs(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}

}  // namespace

ReportStore::ReportStore() = default;

// -----------------------------------------------------------------------------
// 路径 / stamp
// -----------------------------------------------------------------------------

std::string ReportStore::KeyFor(const project::Project& project) {
    // 优先用 .al 路径（一个工作区可能有多个工程）；没有 .al 时退化为 dir|name
    // （单元测试会直接构造这种最小 Project）。
    if (!project.al_path.empty()) return Lower(platform::normalize_path(project.al_path));
    return Lower(platform::normalize_path(project.dir) + "|" + project.name);
}

ReportStore::FileStamp ReportStore::StatOf(const std::string& path, bool* ok) {
    FileStamp stamp;
    if (path.empty()) {
        if (ok != nullptr) *ok = false;
        return stamp;
    }
    std::error_code ec;
    const fs::path native = fs::path(std::u8string(path.begin(), path.end()));
    const std::uintmax_t size = fs::file_size(native, ec);
    if (ec) {
        if (ok != nullptr) *ok = false;
        return stamp;
    }
    const fs::file_time_type mtime = fs::last_write_time(native, ec);
    if (ec) {
        if (ok != nullptr) *ok = false;
        return stamp;
    }
    stamp.size = static_cast<std::uint64_t>(size);
    stamp.mtime_ns = MtimeNs(mtime);
    stamp.valid = true;
    if (ok != nullptr) *ok = true;
    return stamp;
}

bool ReportStore::SameStamp(const FileStamp& left, const FileStamp& right) {
    if (!left.valid || !right.valid) return left.valid == right.valid;
    return left.size == right.size && left.mtime_ns == right.mtime_ns;
}

// -----------------------------------------------------------------------------
// 失效 / revision
// -----------------------------------------------------------------------------

void ReportStore::DropSnapshotsLocked(ProjectState& state) { state.snapshots.clear(); }

void ReportStore::BumpRevisionLocked(ProjectState& state) {
    ++state.revision;
    global_revision_.fetch_add(1, std::memory_order_relaxed);
}

std::size_t ReportStore::DropPathLocked(ProjectState& state, const std::string& path) {
    std::size_t dropped = 0;
    dropped += state.qor.erase(path);
    dropped += state.timing.erase(path);
    dropped += state.area.erase(path);
    dropped += state.flow.erase(path);
    dropped += state.suggestions.erase(path);
    return dropped;
}

bool ReportStore::CheckStaleLocked(ProjectState& state, const project::Project& project) {
    bool changed = false;
    std::size_t dropped = 0;

    // 1) `*_Runs` 目录：新 run 出现 / 目录被删时，之前挑中的文件可能整体换掉，
    //    这种情况下把该工程的解析结果全丢（否则会继续拿旧 run 的报告）。
    const std::string runs = platform::normalize_path(project.dir + "/" + project.name + "_Runs");
    bool runs_ok = false;
    const FileStamp runs_stamp = StatOf(runs, &runs_ok);
    if (!state.initialised) {
        state.runs_stamp = runs_stamp;
    } else if (!SameStamp(state.runs_stamp, runs_stamp)) {
        dropped += state.qor.size() + state.timing.size() + state.area.size() +
                   state.flow.size() + state.suggestions.size();
        state.qor.clear();
        state.timing.clear();
        state.area.clear();
        state.flow.clear();
        state.suggestions.clear();
        state.runs_stamp = runs_stamp;
        changed = true;
    }

    // 2) 逐文件 stamp：内容/大小任一变化就丢该文件的解析结果。
    const auto check_table = [&dropped, &changed](auto& table) {
        for (auto it = table.begin(); it != table.end();) {
            bool ok = false;
            const FileStamp stamp = StatOf(it->first, &ok);
            if (!ok || !SameStamp(it->second.stamp, stamp)) {
                it = table.erase(it);
                ++dropped;
                changed = true;
            } else {
                ++it;
            }
        }
    };
    check_table(state.qor);
    check_table(state.timing);
    check_table(state.area);
    check_table(state.flow);
    check_table(state.suggestions);

    if (changed) {
        DropSnapshotsLocked(state);
        BumpRevisionLocked(state);
        std::lock_guard<std::mutex> lock(cache_mutex_);
        stats_.invalidations += dropped;
        ++stats_.revisions;
    }
    return changed;
}

bool ReportStore::CheckForChanges(const project::Project& project) {
    const std::string key = KeyFor(project);
    std::unique_lock<std::shared_mutex> write(state_mutex_);
    ProjectState& state = projects_[key];
    if (!state.initialised) {
        state.al_path = project.al_path;
        state.dir = project.dir;
        state.name = project.name;
        state.revision = 1;
        state.initialised = true;
        global_revision_.fetch_add(1, std::memory_order_relaxed);
    }
    return CheckStaleLocked(state, project);
}

bool ReportStore::AnyStaleUnlocked(const ProjectState& state,
                                   const project::Project& project) const {
    const std::string runs = platform::normalize_path(project.dir + "/" + project.name + "_Runs");
    bool runs_ok = false;
    const FileStamp runs_stamp = StatOf(runs, &runs_ok);
    if (!SameStamp(state.runs_stamp, runs_stamp)) return true;

    const auto stale = [](const auto& table) {
        for (const auto& item : table) {
            bool ok = false;
            const FileStamp stamp = StatOf(item.first, &ok);
            if (!ok || !SameStamp(item.second.stamp, stamp)) return true;
        }
        return false;
    };
    return stale(state.qor) || stale(state.timing) || stale(state.area) || stale(state.flow) ||
           stale(state.suggestions);
}

std::size_t ReportStore::InvalidatePaths(const std::vector<std::string>& paths) {
    if (paths.empty()) return 0;
    std::vector<std::string> normalized;
    normalized.reserve(paths.size());
    for (const std::string& path : paths) normalized.push_back(platform::normalize_path(path));

    std::unique_lock<std::shared_mutex> write(state_mutex_);
    std::size_t dropped = 0;
    for (auto& [key, state] : projects_) {
        std::size_t project_dropped = 0;
        for (const std::string& path : normalized) project_dropped += DropPathLocked(state, path);
        if (project_dropped == 0) continue;
        dropped += project_dropped;
        DropSnapshotsLocked(state);
        BumpRevisionLocked(state);
    }
    if (dropped != 0) {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        stats_.invalidations += dropped;
        ++stats_.revisions;
    }
    return dropped;
}

void ReportStore::InvalidateAll() {
    std::unique_lock<std::shared_mutex> write(state_mutex_);
    std::size_t dropped = 0;
    for (auto& [key, state] : projects_) {
        dropped += state.qor.size() + state.timing.size() + state.area.size() +
                   state.flow.size() + state.suggestions.size();
        state.qor.clear();
        state.timing.clear();
        state.area.clear();
        state.flow.clear();
        state.suggestions.clear();
        DropSnapshotsLocked(state);
        BumpRevisionLocked(state);
        state.runs_stamp = FileStamp{};
    }
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        stats_.invalidations += dropped;
        ++stats_.revisions;
    }
}

std::uint64_t ReportStore::revision() const {
    return global_revision_.load(std::memory_order_relaxed);
}

ReportStoreStats ReportStore::stats() const {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    return stats_;
}

std::size_t ReportStore::cached_file_count() const {
    std::shared_lock<std::shared_mutex> read(state_mutex_);
    std::size_t count = 0;
    for (const auto& [key, state] : projects_) {
        count += state.qor.size() + state.timing.size() + state.area.size() + state.flow.size() +
                 state.suggestions.size();
    }
    return count;
}

std::size_t ReportStore::snapshot_count() const {
    std::shared_lock<std::shared_mutex> read(state_mutex_);
    std::size_t count = 0;
    for (const auto& [key, state] : projects_) count += state.snapshots.size();
    return count;
}

// -----------------------------------------------------------------------------
// Get
// -----------------------------------------------------------------------------

std::shared_ptr<const ReportsSnapshot> ReportStore::Get(const project::Project& project,
                                                        std::uint32_t mask, bool force) {
    const std::string key = KeyFor(project);

    // ---- 快路径：已发布的快照直接返回（shared lock，可并发）----
    //   ⚠ 这里也要做"请求时检查"：LSP 没有 watcher 事件时，报告文件可能已经变了。
    //   AnyStaleUnlocked 只 stat、不改状态；发现过期就落到慢路径统一失效 + 重建。
    if (!force) {
        std::shared_lock<std::shared_mutex> read(state_mutex_);
        const auto it = projects_.find(key);
        if (it != projects_.end()) {
            const auto snapshot = it->second.snapshots.find(mask);
            if (snapshot != it->second.snapshots.end() &&
                !AnyStaleUnlocked(it->second, project)) {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                ++stats_.gets;
                ++stats_.snapshot_hits;
                return snapshot->second;
            }
        }
    }

    // ---- 慢路径：检查失效 + 按需构建 ----
    std::unique_lock<std::shared_mutex> write(state_mutex_);
    ProjectState& state = projects_[key];
    if (!state.initialised) {
        state.al_path = project.al_path;
        state.dir = project.dir;
        state.name = project.name;
        state.revision = 1;
        state.initialised = true;
        global_revision_.fetch_add(1, std::memory_order_relaxed);
    }

    bool changed = CheckStaleLocked(state, project);
    if (force) {
        std::vector<std::string> paths;
        paths.reserve(state.qor.size() + state.timing.size() + state.area.size() +
                      state.flow.size() + state.suggestions.size());
        for (const auto& item : state.qor) paths.push_back(item.first);
        for (const auto& item : state.timing) paths.push_back(item.first);
        for (const auto& item : state.area) paths.push_back(item.first);
        for (const auto& item : state.flow) paths.push_back(item.first);
        for (const auto& item : state.suggestions) paths.push_back(item.first);
        std::size_t dropped = 0;
        for (const std::string& path : paths) dropped += DropPathLocked(state, path);
        DropSnapshotsLocked(state);
        BumpRevisionLocked(state);
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            stats_.invalidations += dropped;
            ++stats_.revisions;
        }
        changed = true;
        state.runs_stamp = FileStamp{};  // 下一轮 CheckStale 重新记录
    }

    if (!changed) {
        const auto snapshot = state.snapshots.find(mask);
        if (snapshot != state.snapshots.end()) {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            ++stats_.gets;
            ++stats_.snapshot_hits;
            return snapshot->second;
        }
    }

    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        ++stats_.gets;
    }

    // 构建：current_ 让 ReportParseCache 回调知道解析结果该记到哪个工程。
    // state_mutex_ 写锁保证同一时刻只有一个构建。
    current_ = &state;
    const auto started = std::chrono::steady_clock::now();
    reports::ReportsBuildOptions options;
    options.sections = mask;
    options.cache = this;
    const reports::ReportsBundle bundle = reports::BuildReports(project, options);
    const double elapsed_ms = ElapsedMs(started);
    current_ = nullptr;

    auto snapshot = std::make_shared<ReportsSnapshot>();
    snapshot->revision = state.revision;
    snapshot->mask = mask;
    snapshot->al_path = project.al_path;
    snapshot->project_name = project.name;
    snapshot->generated_at = bundle.generated_at;
    snapshot->data = bundle.ToJson();
    snapshot->bytes = snapshot->data.dump().size();
    state.snapshots[mask] = snapshot;

    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        ++stats_.builds;
        stats_.last_build_ms = elapsed_ms;
        stats_.total_build_ms += elapsed_ms;
        ++build_serial_;
    }
    return snapshot;
}

// -----------------------------------------------------------------------------
// ReportParseCache
// -----------------------------------------------------------------------------

template <typename T>
std::shared_ptr<const T> ReportStore::LookupOrParse(std::map<std::string, Parsed<T>>& table,
                                                    const std::string& path,
                                                    const std::function<T()>& parse) {
    bool ok = false;
    const FileStamp stamp = StatOf(path, &ok);
    if (!ok) return nullptr;  // 文件不存在 / 读不到：回落到 reports 层的普通路径

    // key 统一用 normalize_path：reports 层的路径是 os.path.join 拼出来的
    // （可能是 Y:/a\b\c 这种混合分隔符，P-13），而 InvalidatePaths 的入参来自
    // LSP/watcher，通常是正斜杠。不归一会导致"事件失效打不中缓存"。
    const std::string key = platform::normalize_path(path);

    std::lock_guard<std::mutex> lock(cache_mutex_);
    ++stats_.stamp_checks;
    const auto it = table.find(key);
    if (it != table.end() && SameStamp(it->second.stamp, stamp)) {
        ++stats_.parse_hits;
        ++stats_.file_hits;
        return it->second.value;
    }
    // 解析在锁内进行：这样同一文件被两个段同时命中时也只会解析一次；
    // 解析器本身是 never-throw 的纯函数，耗时可接受（见 A3-REPORT 的 p95）。
    T value = parse();
    ++stats_.parses;
    ++stats_.file_reads;
    auto shared = std::make_shared<const T>(std::move(value));
    table[key] = Parsed<T>{stamp, shared};
    return shared;
}

std::shared_ptr<const reports::QorReport> ReportStore::GetQor(const std::string& path,
                                                             std::string_view step) {
    if (current_ == nullptr) return nullptr;
    const std::string step_copy(step);
    return LookupOrParse<reports::QorReport>(
        current_->qor, path,
        [&path, &step_copy] {
            return reports::ParseQor(reports::ReadReportFile(path), step_copy);
        });
}

std::shared_ptr<const reports::TimingReport> ReportStore::GetTiming(const std::string& path,
                                                                    std::string_view filename) {
    if (current_ == nullptr) return nullptr;
    const std::string name_copy(filename);
    return LookupOrParse<reports::TimingReport>(
        current_->timing, path,
        [&path, &name_copy] {
            return reports::ParseTiming(reports::ReadReportFile(path), name_copy);
        });
}

std::shared_ptr<const reports::AreaReport> ReportStore::GetArea(const std::string& path) {
    if (current_ == nullptr) return nullptr;
    return LookupOrParse<reports::AreaReport>(current_->area, path, [&path] {
        return reports::ParseArea(reports::ReadReportFile(path));
    });
}

std::shared_ptr<const reports::FlowStatusReport> ReportStore::GetFlowStatus(
    const std::string& path) {
    if (current_ == nullptr) return nullptr;
    return LookupOrParse<reports::FlowStatusReport>(current_->flow, path, [&path] {
        return reports::ParseFlowStatusText(reports::ReadReportFile(path));
    });
}

std::shared_ptr<const std::vector<reports::QorSuggestion>> ReportStore::GetSuggestions(
    const std::string& path) {
    if (current_ == nullptr) return nullptr;
    return LookupOrParse<std::vector<reports::QorSuggestion>>(
        current_->suggestions, path,
        [&path] { return reports::ParseSuggestionsTcl(path); });
}

}  // namespace fpgalsp::server