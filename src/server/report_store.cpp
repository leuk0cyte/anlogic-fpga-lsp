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

using Json = jsonrpc::Json;

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

// -----------------------------------------------------------------------------
//  limit：构建期裁剪 timing 的 paths（F3）
// -----------------------------------------------------------------------------
//  修复前的实现（commands.cpp）只在"当前节点"上找 `paths`，且只下钻 `syn/phy` 两层，
//  而真实形状是 `data.timing.paths` / `data.phy.timing.paths`，`groups[].paths`
//  更是从没被裁过 —— 于是 `limit=200` 是 **no-op**（32.7 MB 原样返回）。
//
//  这里递归整棵树（对象 + 数组），对每个含 `paths` 的节点裁剪并打 `truncated`，
//  与"先拷贝再裁"的结果逐字节一致；区别只是提前到构建期做，快照因此变小。
void TrimPathsInPlace(Json& node, std::uint32_t limit) {
    if (node.is_array()) {
        for (Json& item : node) TrimPathsInPlace(item, limit);
        return;
    }
    if (!node.is_object()) return;
    if (node.contains("paths") && node["paths"].is_array() &&
        node["paths"].size() > static_cast<std::size_t>(limit)) {
        node["paths"].erase(node["paths"].begin() + static_cast<std::ptrdiff_t>(limit),
                            node["paths"].end());
        node["truncated"] = true;
    }
    for (auto it = node.begin(); it != node.end(); ++it) TrimPathsInPlace(it.value(), limit);
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

    // F3：构建现在跑在 state_mutex_ 之外，解析表由 cache_mutex_ 保护。
    // 锁序固定 state -> cache（LookupOrParse 只拿 cache，不反向）。
    std::lock_guard<std::mutex> tables(cache_mutex_);

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
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
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
    std::lock_guard<std::mutex> tables(cache_mutex_);  // F3：构建在 state 锁外，表要单独护
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
    std::lock_guard<std::mutex> tables(cache_mutex_);
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
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        stats_.invalidations += dropped;
        ++stats_.revisions;
    }
    return dropped;
}

void ReportStore::InvalidateAll() {
    std::unique_lock<std::shared_mutex> write(state_mutex_);
    std::lock_guard<std::mutex> tables(cache_mutex_);
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
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        stats_.invalidations += dropped;
        ++stats_.revisions;
    }
}

std::uint64_t ReportStore::revision() const {
    return global_revision_.load(std::memory_order_relaxed);
}

ReportStoreStats ReportStore::stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

std::size_t ReportStore::cached_file_count() const {
    std::shared_lock<std::shared_mutex> read(state_mutex_);
    std::lock_guard<std::mutex> tables(cache_mutex_);
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
                                                        std::uint32_t mask, bool force,
                                                        std::uint32_t limit) {
    const std::string key = KeyFor(project);
    const std::uint64_t slot = SnapshotSlot(mask, limit);

    // ---- 快路径：已发布的快照直接返回（shared lock，可并发）----
    //   ⚠ 这里也要做"请求时检查"：LSP 没有 watcher 事件时，报告文件可能已经变了。
    //   AnyStaleUnlocked 只 stat、不改状态；发现过期就落到慢路径统一失效 + 重建。
    if (!force) {
        std::shared_lock<std::shared_mutex> read(state_mutex_);
        const auto it = projects_.find(key);
        if (it != projects_.end()) {
            const auto snapshot = it->second.snapshots.find(slot);
            if (snapshot != it->second.snapshots.end() &&
                !AnyStaleUnlocked(it->second, project)) {
                // 统计用独立的短锁：绝不能因为别的线程正在解析大文件（cache_mutex_）而排队。
                std::lock_guard<std::mutex> stats_lock(stats_mutex_);
                ++stats_.gets;
                ++stats_.snapshot_hits;
                return snapshot->second;
            }
        }
    }

    // ---- 慢路径（F3）：构建在 state_mutex_ **之外**完成，只在这里串行化 ----
    //   为什么：以前整个 BuildReports 持 state_mutex_ 写锁，一次 700 ms 的冷构建会把
    //   同 store 的**所有**请求（包括 1.9 KB 的 summary）挡住 655–728 ms。
    // 轻掩码（只有 files/artifacts，不解析任何报告正文）**不必**排进 build_mutex_：
    // BuildReports 在这条路径上不会回调 ReportParseCache（current_ 保持 nullptr），
    // 所以它可以与一个正在跑的重量级构建并行，直接先就绪先发布（F3 / R5b）。
    const bool heavy = (mask & reports::sections::kHeavy) != 0u;
    std::unique_lock<std::mutex> build_guard(build_mutex_, std::defer_lock);
    if (heavy) build_guard.lock();

    // 双检：可能有别的线程刚刚建好
    if (!force) {
        std::shared_lock<std::shared_mutex> read(state_mutex_);
        const auto it = projects_.find(key);
        if (it != projects_.end()) {
            const auto snapshot = it->second.snapshots.find(slot);
            if (snapshot != it->second.snapshots.end() &&
                !AnyStaleUnlocked(it->second, project)) {
                std::lock_guard<std::mutex> stats_lock(stats_mutex_);
                ++stats_.gets;
                ++stats_.snapshot_hits;
                return snapshot->second;
            }
        }
    }

    // (1) 极短临界区：确保 ProjectState 存在、做失效检查、拿到指针
    //     （projects_ 是 std::map，插入别的 key 不会让已有节点的指针失效）
    ProjectState* state = nullptr;
    bool changed = false;
    {
        std::unique_lock<std::shared_mutex> write(state_mutex_);
        state = &projects_[key];
        if (!state->initialised) {
            state->al_path = project.al_path;
            state->dir = project.dir;
            state->name = project.name;
            state->revision = 1;
            state->initialised = true;
            global_revision_.fetch_add(1, std::memory_order_relaxed);
        }

        changed = CheckStaleLocked(*state, project);
        if (force) {
            std::vector<std::string> paths;
            paths.reserve(state->qor.size() + state->timing.size() + state->area.size() +
                          state->flow.size() + state->suggestions.size());
            for (const auto& item : state->qor) paths.push_back(item.first);
            for (const auto& item : state->timing) paths.push_back(item.first);
            for (const auto& item : state->area) paths.push_back(item.first);
            for (const auto& item : state->flow) paths.push_back(item.first);
            for (const auto& item : state->suggestions) paths.push_back(item.first);
            std::size_t dropped = 0;
            for (const std::string& path : paths) dropped += DropPathLocked(*state, path);
            DropSnapshotsLocked(*state);
            BumpRevisionLocked(*state);
            {
                std::lock_guard<std::mutex> stats_lock(stats_mutex_);
                stats_.invalidations += dropped;
                ++stats_.revisions;
            }
            changed = true;
            state->runs_stamp = FileStamp{};  // 下一轮 CheckStale 重新记录
        }

        if (!changed) {
            const auto snapshot = state->snapshots.find(slot);
            if (snapshot != state->snapshots.end()) {
                std::lock_guard<std::mutex> stats_lock(stats_mutex_);
                ++stats_.gets;
                ++stats_.snapshot_hits;
                return snapshot->second;
            }
        }
        {
            std::lock_guard<std::mutex> stats_lock(stats_mutex_);
            ++stats_.gets;
        }
    }  // ← 这里释放 state_mutex_：接下来几百毫秒的解析不再挡任何请求

    // (2) 锁外构建。`current_` 只在持 build_mutex_ 时使用，所以是安全的。
    current_ = state;
    const auto started = std::chrono::steady_clock::now();
    reports::ReportsBuildOptions options;
    options.sections = mask;
    options.cache = this;
    reports::ReportsBundle bundle = reports::BuildReports(project, options);
    const double elapsed_ms = ElapsedMs(started);
    current_ = nullptr;

    auto snapshot = std::make_shared<ReportsSnapshot>();
    snapshot->mask = mask;
    snapshot->al_path = project.al_path;
    snapshot->project_name = project.name;
    snapshot->generated_at = bundle.generated_at;
    snapshot->data = bundle.ToJson();
    // F3：limit 在**构建期**生效（原来在 commands.cpp 里对响应副本裁剪，且实际是 no-op）。
    if (limit > 0) TrimPathsInPlace(snapshot->data, limit);
    // F3：不再为诊断字段做一次 56 MB 的 dump（原来 100–145 ms 白工）。bytes 保持 0。
    snapshot->bytes = 0;

    // (3) 极短临界区：发布
    {
        std::unique_lock<std::shared_mutex> write(state_mutex_);
        snapshot->revision = state->revision;
        state->snapshots[slot] = snapshot;
    }
    {
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        ++stats_.builds;
        stats_.last_build_ms = elapsed_ms;
        stats_.total_build_ms += elapsed_ms;
    }
    ++build_serial_;
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

    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        const auto it = table.find(key);
        if (it != table.end() && SameStamp(it->second.stamp, stamp)) {
            std::lock_guard<std::mutex> stats_lock(stats_mutex_);
            ++stats_.stamp_checks;
            ++stats_.parse_hits;
            ++stats_.file_hits;
            return it->second.value;
        }
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        ++stats_.stamp_checks;
    }
    // F3：解析移到 cache_mutex_ **之外**。以前整个 10 MB 解析（200+ ms）在锁内，
    // 会把任何需要 cache 锁的路径（失效检查、统计）一起堵住。
    // 构建本身由 build_mutex_ 串行化，所以同一个文件不会被两个构建同时解析。
    T value = parse();
    auto shared = std::make_shared<const T>(std::move(value));
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        ++stats_.parses;
        ++stats_.file_reads;
        table[key] = Parsed<T>{stamp, shared};
    }
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