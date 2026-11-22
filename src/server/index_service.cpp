// =============================================================================
//  src/server/index_service.cpp —— 索引服务实现（增量更新 + 不可变快照）
// =============================================================================
//  写者路径（全部在同一把 mutex_ 下）：
//    SetProject      -> 只在"工程 HDL 文件集合变了"时标 dirty（懒全量）
//    OnFileChanged   -> Clone() + ReplaceFile(单文件) + 换指针
//    OnWatchedFiles  -> .al 变更 -> 懒全量；其它文件 -> 逐个增量
//    Get()           -> dirty ? 全量 : (watcher 缺失 ? stamp 懒校验 : 直接返回)
//
//  读路径（Get）只做一次加锁 + 一次 shared_ptr 拷贝；调用方拿到快照后**在锁外**
//  跑查询，写者替换的是指针而不是快照内容，所以读永远看到完整一致的索引。
// =============================================================================

#include "server/index_service.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <set>
#include <utility>

#include "platform/encoding.h"
#include "platform/path.h"

namespace fpgalsp::server {
namespace {

std::string KeyOf(std::string_view path) {
    if (path.empty()) return {};
    return platform::path_key(platform::normalize_path(path));
}

std::optional<std::string> ReadDisk(const std::string& path) {
    return platform::try_read_file_utf8(path);
}

bool IsAlPath(std::string_view path) {
    return platform::path_key(platform::path_extension(path)) == ".al";
}

// 工程参与索引的文件：.v/.sv/.vh/.svh + .vhd/.vhdl，再并上 IP 生成的 HDL。
// 顺序与 hdl::BuildIndex 完全一致（先 Verilog 后 VHDL）。
std::vector<std::string> ExpectedFiles(const project::Project& project) {
    std::vector<std::string> files = hdl::VerilogFilesFor(project);
    for (const std::string& path : hdl::VhdlFilesFor(project)) files.push_back(path);
    return files;
}

// 一次目录枚举带回整目录的 (size, mtime)；与 LspServer::ProjectSignature 里的
// DirStampCache 同策略（枚举不到就按"磁盘上没有"处理）。
//
// R2（F2）：调用方传**预计算好的** (dir, name)，这里不再做 substr / normalize / 小写。
// 旧实现每个文件每次 Get() 都付一次 "find_last_of + substr + path_key"，1000 文件时
// 光这一层就占 stamp 校验的 ~30%。
class DirStamps {
public:
    const IndexService::FileStamp* Find(const std::string& dir, const std::string& name) {
        if (dir.empty() || name.empty()) return nullptr;
        auto it = dirs_.find(dir);
        if (it == dirs_.end()) it = dirs_.emplace(dir, LoadDir(dir)).first;
        const auto found = it->second.find(name);
        return found == it->second.end() ? nullptr : &found->second;
    }

private:
    static std::map<std::string, IndexService::FileStamp> LoadDir(const std::string& dir) {
        std::map<std::string, IndexService::FileStamp> entries;
        std::error_code ec;
        std::filesystem::directory_iterator iter{std::filesystem::path(dir), ec};
        if (ec) return entries;
        const std::filesystem::directory_iterator end;
        for (; iter != end; iter.increment(ec)) {
            if (ec) break;
            const std::filesystem::directory_entry& entry = *iter;
            std::error_code entry_ec;
            const std::uintmax_t size = entry.file_size(entry_ec);
            if (entry_ec) continue;
            const std::filesystem::file_time_type write_time = entry.last_write_time(entry_ec);
            if (entry_ec) continue;
            // 文件名按 UTF-8 字节比较（平台层的路径就是 UTF-8 std::string）
            const std::u8string u8 = entry.path().filename().u8string();
            std::string name(reinterpret_cast<const char*>(u8.data()), u8.size());

            IndexService::FileStamp stamp;
            stamp.exists = true;
            stamp.size = size;
            stamp.mtime = static_cast<std::int64_t>(write_time.time_since_epoch().count());
            entries.emplace(platform::path_key(name), stamp);
        }
        return entries;
    }

    std::map<std::string, std::map<std::string, IndexService::FileStamp>> dirs_;
};

// 解析一个文件：命中解析缓存就直接共享（B8/PERF-03 的 shared_ptr 语义）。
std::shared_ptr<const hdl::ParsedFile> ParseOne(const std::string& path, std::string_view text,
                                                hdl::ParseCache* cache) {
    if (cache != nullptr) {
        if (auto hit = cache->Get(path, text); hit != nullptr) return hit;
    }
    auto fresh = std::make_shared<hdl::ParsedFile>(hdl::ParseFile(path, text));
    if (cache != nullptr) cache->Put(path, text, fresh);
    return fresh;
}

std::optional<std::string> LoadSafely(const hdl::TextLoader& loader, const std::string& path) {
    if (!loader) return std::nullopt;
    try {
        return loader(path);
    } catch (...) {
        return std::nullopt;  // loader 是外部注入的：never-throw 由我们兜
    }
}

// 两个工程模型"对索引而言是否等价"：只看参与索引的字段（路径/种类/是否在 build）。
// 这个比较是 O(#文件) 的字符串相等，比重新算一遍 ExpectedFiles（两次 build +
// 小写化 + std::set）便宜一个数量级 —— 集成方每个请求都会调 SetProject。
bool SameIndexInputs(const project::Project& a, const project::Project& b) {
    if (a.al_path != b.al_path) return false;
    if (a.files.size() != b.files.size()) return false;
    for (std::size_t i = 0; i < a.files.size(); ++i) {
        const project::AlFile& left = a.files[i];
        const project::AlFile& right = b.files[i];
        if (left.path != right.path || left.kind != right.kind ||
            left.enabled != right.enabled || left.excluded != right.excluded ||
            left.exists != right.exists) {
            return false;
        }
    }
    return true;
}

}  // namespace

// =============================================================================
// 生命周期
// =============================================================================

IndexService::IndexService() : IndexService(TextLoader{}) {}

IndexService::IndexService(TextLoader loader) : loader_(std::move(loader)) {
    if (!loader_) loader_ = &ReadDisk;  // 默认：读盘（UTF-8，never-throw）
}

IndexService::~IndexService() = default;

// =============================================================================
// 读路径
// =============================================================================

IndexService::Snapshot IndexService::Get() const {
    std::scoped_lock lock(mutex_);
    return EnsureFreshLocked();
}

IndexService::Snapshot IndexService::EnsureFreshLocked() const {
    if (!state_.have_project) return nullptr;
    if (state_.dirty) {
        RebuildAllLocked();
    } else if (!state_.watcher_active && state_.snapshot != nullptr) {
        // R3（F2）：TTL 门控。didChange 走缓冲区增量，不依赖磁盘 stamp；
        // 只有 didClose / watcher 事件 / InvalidateAll 会请求强制校验
        // （P2-A：didSave 的 nullopt 路径已收敛到"只重载那一个文件"，不再 force）。
        const bool due =
            state_.force_validation ||
            (state_.stamp_ttl <= std::chrono::milliseconds(0)) ||
            (std::chrono::steady_clock::now() - state_.last_validation >= state_.stamp_ttl);
        if (due) ValidateStampsLocked();
    }
    return state_.snapshot;
}

// =============================================================================
// 写路径
// =============================================================================

void IndexService::SetProject(const project::Project& project) {
    std::scoped_lock lock(mutex_);

    // 工程模型没变（同一个 .al、同一批 HDL 文件）：只换字段，不动索引、不重算
    // 文件集合。集成方可以每个请求都调用（ProjectForPath 本来就是每请求一次）。
    // C1：形参是 const&，下面这两处赋值就是每请求**唯一**一次 Project 深拷贝。
    if (state_.have_project && SameIndexInputs(project, state_.project)) {
        state_.project = project;
        return;
    }

    std::vector<std::string> files = ExpectedFiles(project);
    state_.project = project;
    state_.have_project = true;
    SetExpectedLocked(std::move(files));  // 里面会重算键 + 清空/重置 stamp 基线
    state_.dirty = true;
}

void IndexService::OnFileChanged(const std::string& path, std::optional<std::string> text) {
    std::scoped_lock lock(mutex_);
    if (!state_.have_project || path.empty()) return;

    const std::string key = KeyOf(path);
    const State::ExpectedFile* expected = FindExpectedLocked(key);
    // 只有工程的 HDL 文件会影响索引；其它文件（含 .al）走 OnWatchedFiles。
    if (expected == nullptr) {
        state_.last_changes = ChangeSet{};
        return;
    }

    // P2-A：没有缓冲区文本 = didClose（以及没带 text 的 didSave）：磁盘内容优先，
    // **只重载这一个文件**。旧实现在这里 RequestValidationLocked()，把"全工程 stamp
    // 校验"压到下一次 Get()（audit/perf2：+1~3 ms、偶发 22 ms）；现在别的文件的外部
    // 改动由 watcher 事件（全量校验）或 TTL 窗口兜底。
    if (!text.has_value()) {
        ValidateOneLocked(*expected);
        return;
    }

    // 快照还没建、或者已经脏了（.al 刚变更过）：不做增量。
    // 下一次 Get 的全量重建会通过 loader（集成方传入缓冲区优先的 LoadText）
    // 拿到最新内容，结果与"先做一次增量再全量"相同。
    if (state_.dirty || state_.snapshot == nullptr) {
        // 还没有快照可比对：让客户端整树重取（下一次 Get 会重建索引）。
        state_.last_changes = ChangeSet{};
        state_.last_changes.paths = {path};
        state_.last_changes.full = true;
        return;
    }

    // P-C1：先记下变化前的模块名（用于判定"模块增删 = 拓扑变化"）。
    std::vector<std::string> before_modules;
    if (state_.snapshot != nullptr) {
        for (const hdl::ModuleInfo* info : state_.snapshot->ModulesOfFile(path)) {
            before_modules.push_back(info->module.name);
        }
    }

    auto next = std::make_shared<hdl::HdlIndex>(state_.snapshot->Clone());
    next->ReplaceFile(ParseOne(path, *text, &state_.cache));
    ++state_.stats.file_updates;
    InstallLocked(std::move(next));
    RecordChangesLocked({path}, before_modules);

    // stamp 基线跟着走：记的是**磁盘当前状态**（缓冲区内容可能领先磁盘，
    // 用户保存后 mtime 变化会再触发一次重读，最多多解析一次，结果相同）。
    SyncStampLocked(*expected);
}

// P-C1：把"变化后这些文件定义的模块"记进 last_changes。
// 判定口径：
//   * 变化前有、变化后没有  -> 模块被删/改名 -> full（客户端整树重取）
//   * 变化前没有、变化后有  -> 新模块        -> full（否则新节点插不进树）
//   * 名字没变              -> modules 里带 portCount/defined，客户端就地打补丁
void IndexService::RecordChangesLocked(const std::vector<std::string>& paths,
                                       const std::vector<std::string>& before_modules) const {
    ChangeSet changes;
    changes.paths = paths;
    if (state_.snapshot != nullptr) {
        std::set<std::string> before(before_modules.begin(), before_modules.end());
        std::set<std::string> after;
        for (const std::string& path : paths) {
            for (const hdl::ModuleInfo* info : state_.snapshot->ModulesOfFile(path)) {
                const std::string& name = info->module.name;
                if (!after.insert(name).second) continue;  // 同名多份定义只报一次
                ModuleDelta delta;
                delta.module = name;
                delta.port_count = static_cast<int>(info->module.ports.size());
                delta.defined = state_.snapshot->ModuleByName(name) != nullptr;
                changes.modules.push_back(std::move(delta));
                if (!before.contains(name)) changes.full = true;  // 新增模块
            }
        }
        for (const std::string& name : before) {
            if (!after.contains(name)) {
                changes.full = true;  // 模块消失（改名/删除）
                break;
            }
        }
    }
    state_.last_changes = std::move(changes);
}

IndexService::ChangeSet IndexService::LastChanges() const {
    std::scoped_lock lock(mutex_);
    return state_.last_changes;
}

void IndexService::OnWatchedFiles(const std::vector<std::string>& changed,
                                  const std::vector<std::string>& deleted) {
    std::scoped_lock lock(mutex_);
    if (!state_.have_project) return;

    bool al_changed = false;
    for (const std::string& path : changed) {
        if (IsAlPath(path)) al_changed = true;
    }
    for (const std::string& path : deleted) {
        if (IsAlPath(path)) al_changed = true;
    }

    if (al_changed) {
        // .al 是"哪些文件参与构建"的权威来源：重解析（拿新的文件集合/top/器件），
        // 然后标脏——下一次 Get 全量重建。
        if (!state_.project.al_path.empty()) {
            project::Project fresh = project::ParseAl(state_.project.al_path);
            if (!fresh.al_path.empty()) state_.project = std::move(fresh);
        }
        SetExpectedLocked(ExpectedFiles(state_.project));
        state_.dirty = true;
        // P-C1：工程文件集合可能变了 → 客户端必须整树重取。
        state_.last_changes = ChangeSet{};
        state_.last_changes.paths = changed;
        state_.last_changes.paths.insert(state_.last_changes.paths.end(), deleted.begin(),
                                         deleted.end());
        state_.last_changes.full = true;
        return;
    }

    // watcher 报了变化：磁盘状态可能变过，下一次 Get 要重新校验（不受 TTL 限制）。
    RequestValidationLocked();

    if (state_.dirty || state_.snapshot == nullptr) {
        state_.last_changes = ChangeSet{};
        state_.last_changes.paths = changed;
        state_.last_changes.full = true;
        return;
    }

    std::vector<std::string> before_modules;
    for (const std::string& path : changed) {
        for (const hdl::ModuleInfo* info : state_.snapshot->ModulesOfFile(path)) {
            before_modules.push_back(info->module.name);
        }
    }

    auto next = std::make_shared<hdl::HdlIndex>(state_.snapshot->Clone());
    DirStamps disk;
    bool touched = false;

    for (const std::string& path : deleted) {
        if (next->RemoveFile(path)) {
            ++state_.stats.file_updates;
            touched = true;
        }
        if (const State::ExpectedFile* expected = FindExpectedLocked(KeyOf(path));
            expected != nullptr) {
            if (const auto it = state_.expected_index.find(expected->key);
                it != state_.expected_index.end() && it->second < state_.stamps.size() &&
                state_.stamps_initialized) {
                state_.stamps[it->second] = FileStamp{};  // 磁盘上已经没有了
            }
        }
    }

    for (const std::string& path : changed) {
        const std::string key = KeyOf(path);
        const State::ExpectedFile* expected = FindExpectedLocked(key);
        if (expected == nullptr && next->File(path) == nullptr) continue;  // 与本工程无关
        const std::optional<std::string> text = LoadSafely(loader_, path);
        if (text.has_value()) {
            next->ReplaceFile(ParseOne(path, *text, &state_.cache));
        } else {
            next->RemoveFile(path);
        }
        ++state_.stats.file_updates;
        touched = true;
        if (expected != nullptr) {
            if (const auto it = state_.expected_index.find(expected->key);
                it != state_.expected_index.end() && it->second < state_.stamps.size() &&
                state_.stamps_initialized) {
                const FileStamp* now = disk.Find(expected->dir, expected->name);
                state_.stamps[it->second] = now != nullptr ? *now : FileStamp{};
            }
        }
    }

    if (touched) {
        InstallLocked(std::move(next));
        // P-C1：变更摘要（含模块增删 → full）
        RecordChangesLocked(changed, before_modules);
    } else {
        state_.last_changes = ChangeSet{};
    }
}

void IndexService::InvalidateAll() {
    std::scoped_lock lock(mutex_);
    state_.dirty = true;
    state_.stamps.assign(state_.expected.size(), FileStamp{});
    state_.stamps_initialized = false;
    state_.force_validation = true;
}

void IndexService::SetWatcherActive(bool active) {
    std::scoped_lock lock(mutex_);
    if (state_.watcher_active == active) return;
    state_.watcher_active = active;
    // 从"有 watcher"切到"没有"：此刻磁盘状态就是基线（索引刚构建/刚增量过），
    // 不等下一次 Get 再补 —— 否则切换瞬间的改动会被漏掉。
    if (!active) RefreshStampsLocked();
}

// =============================================================================
// R3：stamp 校验 TTL（可用性 vs 外部改动可见性）
// =============================================================================

std::chrono::milliseconds IndexService::DefaultStampValidationInterval() {
    return std::chrono::milliseconds(250);
}

void IndexService::SetStampValidationInterval(std::chrono::milliseconds interval) {
    std::scoped_lock lock(mutex_);
    state_.stamp_ttl = interval < std::chrono::milliseconds(0) ? std::chrono::milliseconds(0)
                                                               : interval;
    state_.force_validation = true;  // 改了间隔，立刻按新设置校验一次
}

std::chrono::milliseconds IndexService::stamp_validation_interval() const {
    std::scoped_lock lock(mutex_);
    return state_.stamp_ttl;
}

// =============================================================================
// 观察/诊断
// =============================================================================

bool IndexService::watcher_active() const {
    std::scoped_lock lock(mutex_);
    return state_.watcher_active;
}

IndexService::Stats IndexService::stats() const {
    std::scoped_lock lock(mutex_);
    return state_.stats;
}

bool IndexService::has_project() const {
    std::scoped_lock lock(mutex_);
    return state_.have_project;
}

std::string IndexService::al_path() const {
    std::scoped_lock lock(mutex_);
    return state_.project.al_path;
}

bool IndexService::truncated() const {
    std::scoped_lock lock(mutex_);
    return state_.truncated;
}

std::size_t IndexService::parse_errors() const {
    std::scoped_lock lock(mutex_);
    return state_.parse_errors;
}

std::size_t IndexService::tracked_files() const {
    std::scoped_lock lock(mutex_);
    return state_.expected.size();
}

// =============================================================================
// 内部（全部要求已持有 mutex_）
// =============================================================================

void IndexService::RebuildAllLocked() const {
    state_.dirty = false;
    if (!state_.have_project) {
        state_.snapshot.reset();
        return;
    }

    hdl::BuildOptions options;
    options.cache = &state_.cache;
    // 冷构建并行解析（A1 第 3 点）：N = min(8, hw-1)，保序合并。
    options.parse_threads = hdl::RecommendedParseThreads();

    hdl::BuildResult built = hdl::BuildIndex(state_.project, loader_, options);
    auto next = std::make_shared<hdl::HdlIndex>(std::move(built.index));

    state_.truncated = built.truncated;
    state_.parse_errors = built.parse_error_count;
    ++state_.stats.rebuilds;

    // stamp 基线：watcher 缺失时**立刻**建立（此刻索引内容就是刚读到的磁盘/缓冲区
    // 状态），否则基线会拖到下一次 Get 才建立 —— 那期间磁盘上的改动就会被漏掉。
    // watcher 可用时不需要基线（信任 didChangeWatchedFiles）。
    state_.stamps_initialized = false;
    state_.force_validation = false;
    if (!state_.watcher_active) RefreshStampsLocked();

    InstallLocked(std::move(next));
}

void IndexService::ValidateOneLocked(const State::ExpectedFile& file) const {
    // 快照还没建、或者已经脏了（.al 刚变更过）：下一次 Get() 的全量重建会通过 loader
    // （集成方传的是"缓冲区优先"的 LoadText）拿到最新内容，这里不做单文件增量。
    if (state_.snapshot == nullptr || state_.dirty) {
        state_.last_changes = ChangeSet{};
        state_.last_changes.paths = {file.path};
        state_.last_changes.full = true;
        return;
    }

    std::vector<std::string> before_modules;
    for (const hdl::ModuleInfo* info : state_.snapshot->ModulesOfFile(file.path)) {
        before_modules.push_back(info->module.name);
    }

    // ⚠ 这里**必须**无条件回落到磁盘，不能"stamp 没变就跳过"：
    //   didClose 的语义是把未保存的缓冲区改动丢掉、回落到磁盘内容，而磁盘
    //   size/mtime 从头到尾没变过（用户没保存）。内容真的没变时 ParseOne 会命中
    //   内容哈希缓存（一次哈希 + 查表，~µs），不会重新跑 Verible。
    auto next = std::make_shared<hdl::HdlIndex>(state_.snapshot->Clone());
    const std::optional<std::string> disk_text = LoadSafely(loader_, file.path);
    if (disk_text.has_value()) {
        next->ReplaceFile(ParseOne(file.path, *disk_text, &state_.cache));
    } else {
        next->RemoveFile(file.path);  // 读不到（删除/不可读）=> 从索引移除
    }
    ++state_.stats.file_updates;
    InstallLocked(std::move(next));
    RecordChangesLocked({file.path}, before_modules);

    // stamp 基线跟着走到磁盘当前状态：否则下一次 TTL 校验会把这个刚重载过的文件
    // 再读一遍（结果相同，但白付一次文件枚举 + 解析 + generation 抖动）。
    SyncStampLocked(file);
}

void IndexService::ValidateStampsLocked() const {
    state_.last_validation = std::chrono::steady_clock::now();
    state_.force_validation = false;

    if (!state_.stamps_initialized) {
        RefreshStampsLocked();  // 第一次：只取基线（索引内容就是按当时的磁盘/缓冲区建的）
        return;
    }
    if (state_.snapshot == nullptr) return;

    DirStamps disk;
    std::vector<std::size_t> changed;  // 下标，避免再按 key 反查
    std::vector<std::size_t> removed;
    changed.reserve(4);
    removed.reserve(4);

    for (std::size_t i = 0; i < state_.expected.size(); ++i) {
        const State::ExpectedFile& file = state_.expected[i];
        const FileStamp* now = disk.Find(file.dir, file.name);
        const FileStamp current = now != nullptr ? *now : FileStamp{};
        if (state_.stamps[i] == current) continue;
        if (current.exists) {
            changed.push_back(i);
        } else {
            removed.push_back(i);
        }
    }

    if (changed.empty() && removed.empty()) return;

    auto next = std::make_shared<hdl::HdlIndex>(state_.snapshot->Clone());
    bool touched = false;

    for (const std::size_t i : removed) {
        const State::ExpectedFile& file = state_.expected[i];
        if (next->RemoveFile(file.path)) {
            ++state_.stats.file_updates;
            touched = true;
        }
        state_.stamps[i] = FileStamp{};  // 磁盘上已经没有了
    }

    for (const std::size_t i : changed) {
        const State::ExpectedFile& file = state_.expected[i];
        const std::optional<std::string> text = LoadSafely(loader_, file.path);
        if (text.has_value()) {
            next->ReplaceFile(ParseOne(file.path, *text, &state_.cache));
        } else {
            next->RemoveFile(file.path);
        }
        ++state_.stats.file_updates;
        touched = true;
        const FileStamp* now = disk.Find(file.dir, file.name);
        state_.stamps[i] = now != nullptr ? *now : FileStamp{};
    }

    if (touched) InstallLocked(std::move(next));
}

void IndexService::RefreshStampsLocked() const {
    DirStamps disk;
    state_.stamps.assign(state_.expected.size(), FileStamp{});
    for (std::size_t i = 0; i < state_.expected.size(); ++i) {
        const State::ExpectedFile& file = state_.expected[i];
        const FileStamp* now = disk.Find(file.dir, file.name);
        state_.stamps[i] = now != nullptr ? *now : FileStamp{};
    }
    state_.stamps_initialized = true;
    state_.last_validation = std::chrono::steady_clock::now();
}

void IndexService::InstallLocked(std::shared_ptr<const hdl::HdlIndex> next) const {
    state_.snapshot = std::move(next);
    ++state_.stats.generation;
}

void IndexService::SetExpectedLocked(std::vector<std::string> files) {
    // R2（F2）：键只在这里算一次（SetProject 变集合 / .al 变更 / 重建时），
    // 之后每个请求的 stamp 校验直接用它。
    state_.expected.clear();
    state_.expected.resize(files.size());
    state_.expected_index.clear();
    for (std::size_t i = 0; i < files.size(); ++i) {
        State::ExpectedFile& entry = state_.expected[i];
        entry.path = std::move(files[i]);
        const std::string normalized = platform::normalize_path(entry.path);
        entry.key = platform::path_key(normalized);
        const std::size_t slash = normalized.find_last_of("/\\");
        if (slash == std::string::npos || slash + 1 >= normalized.size()) {
            entry.dir.clear();
            entry.name = entry.key;
        } else {
            entry.dir = normalized.substr(0, slash);
            entry.name = platform::path_key(std::string_view(normalized).substr(slash + 1));
        }
        state_.expected_index.emplace(entry.key, i);
    }
    state_.stamps.assign(state_.expected.size(), FileStamp{});
    state_.stamps_initialized = false;
    state_.force_validation = true;
}

const IndexService::State::ExpectedFile* IndexService::FindExpectedLocked(
    std::string_view key) const {
    const auto it = state_.expected_index.find(std::string(key));
    if (it == state_.expected_index.end()) return nullptr;
    if (it->second >= state_.expected.size()) return nullptr;
    return &state_.expected[it->second];
}

void IndexService::SyncStampLocked(const State::ExpectedFile& file) const {
    if (!state_.stamps_initialized) return;
    const auto it = state_.expected_index.find(file.key);
    if (it == state_.expected_index.end() || it->second >= state_.stamps.size()) return;
    DirStamps disk;
    const FileStamp* now = disk.Find(file.dir, file.name);
    state_.stamps[it->second] = now != nullptr ? *now : FileStamp{};
}

void IndexService::RequestValidationLocked() const { state_.force_validation = true; }

bool IndexService::IsExpected(std::string_view key) const {
    return state_.expected_index.contains(std::string(key));
}

}  // namespace fpgalsp::server