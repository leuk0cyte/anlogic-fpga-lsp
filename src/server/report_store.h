// =============================================================================
//  src/server/report_store.h —— 报告缓存（文件级 mtime/size + 分节懒构建 + 不可变快照）
// =============================================================================
//  Phase 2 / A3。目标（来自 task-23 与 FIX-REPORT §6）：
//    * `fpgaLsp/reports` 不再每次请求都把几十个报告文件全量读盘 + 解析；
//    * 插件可以分节拉取（timing / area / qor / flow），只解析被请求的段；
//    * 报告文件变化时 revision 变化，供 LspServer 推送 `fpgaLsp/reportsChanged`（Lead 接线）。
//
//  设计（为什么是这个形状）：
//    1. **文件级缓存 key = (path, mtime, size)**：解析结果按路径存放，命中条件是
//       "stamp 与上次完全一致"。stamp 变了就丢该文件的解析结果（report_store.cpp
//       的 `CheckStaleLocked`），下次构建重新读 + 解析。
//    2. **分节懒构建**：`Get(project, mask)` 只把 `mask` 传给
//       `reports::BuildReports(project, {sections, cache})`；被请求的段才会真正读盘解析
//       （cache 未命中的文件才会读）。不同 mask 的快照分开缓存，互不覆盖。
//    3. **不可变快照**：`Snapshot` 一旦发布就不再修改（const + shared_ptr），
//       读路径可以并发拿同一份；构建只在新 revision 下产生新快照。
//    4. **线程安全**：`state_mutex_`（shared_mutex）保护快照/项目状态——快路径
//       shared lock，构建与失效 unique lock（"读并发、写单线程"，与 LSP 的
//       state_mutex_ 一个套路）；`cache_mutex_` 单独保护解析结果表，因为构建时
//       会回调本对象的 ReportParseCache 方法（两把锁不嵌套，避免自锁）。
//    5. **no-cache 路径不变**：`reports::BuildReports(project)` 仍是旧行为，
//       测试/工具/CLI 不受影响；只有经过本 store 的请求才走缓存。
//
//  ⚠ 已知边界（写进 A3-REPORT）：
//    * `sections::kFiles / kArtifacts` **不裁剪输出**（`ok/error` 依赖 runs 目录判定，
//      且 ToJson 本来就带这两个键）；真正省掉的是 qor/timing/area/flow 的解析。
//    * 新 run 目录出现的检测靠 `*_Runs` 目录的 mtime；若只是"旧文件内容变了"，
//      走的是逐文件 stamp 对比。watcher 事件优先（LspServer 传 InvalidatePaths）。
// =============================================================================

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include "jsonrpc/json_util.h"
#include "project/al_project.h"
#include "reports/reports_aggregate.h"

namespace fpgalsp::server {

// 一次构建结果（不可变）。`data` 就是 `ReportsBundle::ToJson()` 的结果，
// 与 Phase 1 的 `fpgaLsp/reports` 逐字段一致（默认 mask=kAll 时）。
struct ReportsSnapshot {
    std::uint64_t revision = 0;      // 本工程的 revision（变化即意味着需要重发）
    std::uint32_t mask = 0;          // 这份快照包含的 section 掩码
    std::string al_path;
    std::string project_name;
    double generated_at = 0.0;
    jsonrpc::Json data;              // BuildReports(...).ToJson()
    std::size_t bytes = 0;           // data.dump().size()，便于定位"快照过大"
};

// 统计（诊断/性能报告用；全部单调递增，除 last/total 耗时）。
struct ReportStoreStats {
    std::uint64_t gets = 0;             // Get() 次数
    std::uint64_t snapshot_hits = 0;    // 直接命中已发布快照
    std::uint64_t builds = 0;           // 真正调用 BuildReports 的次数
    std::uint64_t file_reads = 0;       // 读盘次数（cache miss）
    std::uint64_t file_hits = 0;        // 命中解析缓存的次数
    std::uint64_t parses = 0;           // 解析次数
    std::uint64_t parse_hits = 0;       // 命中解析结果的次数
    std::uint64_t stamp_checks = 0;     // stat 次数
    std::uint64_t invalidations = 0;    // 被丢弃的解析结果条数
    std::uint64_t revisions = 0;        // revision 递增次数
    double last_build_ms = 0.0;
    double total_build_ms = 0.0;
};

class ReportStore final : public reports::ReportParseCache {
  public:
    ReportStore();
    ~ReportStore() override = default;

    ReportStore(const ReportStore&) = delete;
    ReportStore& operator=(const ReportStore&) = delete;

    // 取一份快照。mask 语义见 reports/sections。force=true 丢弃解析缓存并重建。
    // 返回的 shared_ptr 可以跨线程持有；内容不会再变。
    std::shared_ptr<const ReportsSnapshot> Get(const project::Project& project,
                                              std::uint32_t mask = reports::sections::kAll,
                                              bool force = false);

    // 请求时检查（LSP 没有 watcher 事件时的兜底）：文件 stamp / runs 目录变化 -> revision++。
    // 返回 true = 本次检查让 revision 变了（调用方应推送 reportsChanged）。
    bool CheckForChanges(const project::Project& project);

    // watcher / didSave 事件：作废指定文件（报告路径）的解析结果。
    // 路径按 normalize 后比较；返回被丢弃的条目数。
    std::size_t InvalidatePaths(const std::vector<std::string>& paths);

    // 全量作废（工程关闭 / 配置变化 / 新建 run）。
    void InvalidateAll();

    // 全局 revision：任何工程任何失效都会 +1。用于"是否需要推送"的粗判断。
    std::uint64_t revision() const;

    ReportStoreStats stats() const;

    // 观测用：当前缓存了多少个文件/多少个快照（测试断言用）。
    std::size_t cached_file_count() const;
    std::size_t snapshot_count() const;

    // ---- reports::ReportParseCache（只在 BuildReports 构建期间被调用）----
    std::shared_ptr<const reports::QorReport> GetQor(const std::string& path,
                                                     std::string_view step) override;
    std::shared_ptr<const reports::TimingReport> GetTiming(const std::string& path,
                                                           std::string_view filename) override;
    std::shared_ptr<const reports::AreaReport> GetArea(const std::string& path) override;
    std::shared_ptr<const reports::FlowStatusReport> GetFlowStatus(
        const std::string& path) override;
    std::shared_ptr<const std::vector<reports::QorSuggestion>> GetSuggestions(
        const std::string& path) override;

  private:
    struct FileStamp {
        std::uint64_t size = 0;
        std::int64_t mtime_ns = 0;
        bool valid = false;
    };

    template <typename T>
    struct Parsed {
        FileStamp stamp;
        std::shared_ptr<const T> value;
    };

    struct ProjectState {
        std::string al_path;
        std::string dir;
        std::string name;
        FileStamp runs_stamp;
        std::map<std::string, Parsed<reports::QorReport>> qor;
        std::map<std::string, Parsed<reports::TimingReport>> timing;
        std::map<std::string, Parsed<reports::AreaReport>> area;
        std::map<std::string, Parsed<reports::FlowStatusReport>> flow;
        std::map<std::string, Parsed<std::vector<reports::QorSuggestion>>> suggestions;
        std::map<std::uint32_t, std::shared_ptr<const ReportsSnapshot>> snapshots;
        std::uint64_t revision = 0;
        bool initialised = false;
    };

    static std::string KeyFor(const project::Project& project);
    static FileStamp StatOf(const std::string& path, bool* ok);
    static bool SameStamp(const FileStamp& left, const FileStamp& right);

    // 在持有 state_mutex_ 的写锁下调用：对比所有已缓存文件的 stamp + runs 目录。
    bool CheckStaleLocked(ProjectState& state, const project::Project& project);
    // 只读版本（shared lock 下可调）：只判断"是否可能过期"，不改任何状态。
    // 快路径用它实现"没有 watcher 时的请求时检查"——发现过期就落到慢路径统一处理。
    bool AnyStaleUnlocked(const ProjectState& state, const project::Project& project) const;
    void DropSnapshotsLocked(ProjectState& state);
    void BumpRevisionLocked(ProjectState& state);
    // 丢弃某个文件的全部解析结果（4 类 + suggestions）
    std::size_t DropPathLocked(ProjectState& state, const std::string& path);

    // ReportParseCache 的实现（cache_mutex_ 保护；构建期间 current_ 指向正在构建的工程）
    template <typename T>
    std::shared_ptr<const T> LookupOrParse(std::map<std::string, Parsed<T>>& table,
                                           const std::string& path,
                                           const std::function<T()>& parse);

    mutable std::shared_mutex state_mutex_;
    mutable std::mutex cache_mutex_;
    std::map<std::string, ProjectState> projects_;  // key = al_path 小写
    ProjectState* current_ = nullptr;               // 构建期间指向 projects_ 中的元素
    ReportStoreStats stats_;
    std::atomic<std::uint64_t> global_revision_{0};
    std::atomic<std::uint64_t> build_serial_{0};
};

}  // namespace fpgalsp::server