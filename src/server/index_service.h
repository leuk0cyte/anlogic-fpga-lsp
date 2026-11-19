// =============================================================================
//  src/server/index_service.h —— HDL 索引服务（不可变快照 + 单写者 + 增量更新）
// =============================================================================
//  为什么需要这一层（Phase 2 / A1）：
//    Phase B 之后单请求的固定开销已经降到亚毫秒，但**改一个字就全量重建索引**
//    的路径没变：`LspServer::IndexForProject` 只要发现工程签名变了（didChange 改了
//    缓冲区版本、watcher 报了文件变化），就再跑一遍 `hdl::BuildIndex`。
//    200 文件合成工程实测 edit→completion 24–26 ms（warm 5.5 ms），3.5–4.5× 回退。
//
//  本层的模型：
//    * **不可变快照**：`Snapshot = shared_ptr<const HdlIndex>`。`Get()` 只加一次
//      锁取指针，返回的索引在生命周期内**永不修改** —— 任何线程都可以拿着它在
//      锁外跑查询（documentSymbol / completion / references ...），不会被写者撕裂。
//    * **单写者**：所有修改（SetProject / OnFileChanged / OnWatchedFiles /
//      InvalidateAll）都在 `IndexService` 的同一把互斥量下串行；写者从不原地改
//      快照，而是 `Clone()` 一份再替换文件、然后**原子换指针**（copy-on-write）。
//    * **增量更新**：`OnFileChanged` 只解析那一个文件 + O(N) 指针级 Clone，
//      不再读盘/解析另外 N-1 个文件。
//    * **watcher 缺失时的懒校验**：`SetWatcherActive(false)` 之后，`Get()` 会用
//      "一次目录枚举带回整目录 (size, mtime)"的目录 stamp 比对（与 B8 的
//      `DirStampCache` 同策略），只对真正变了的文件做增量更新。
//
//  与 LspServer 的关系：
//    `lsp_server.cpp` 的 `IndexForProject(...)` 换成
//        service_.SetProject(project);  return service_.Get();
//    即可（集成由 Lead 做，本文件不依赖 server 层任何东西，只依赖 hdl + project）。
//
//  线程安全约定：
//    * 公开方法全部可并发调用；`Get()` 是**读**路径但会在锁内做懒重建（逻辑上是
//      "填缓存"），因此内部状态是 mutable 的。
//    * 解析/读盘发生在锁内（单文件 ~0.1–0.5 ms；冷构建 ~几十 ms）。这样写者之间
//      的语义最简单，也不会出现"两个线程同时为一个文件重复解析"。
// =============================================================================

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "hdl/hdl_index.h"
#include "project/al_project.h"

namespace fpgalsp::server {

// -----------------------------------------------------------------------------
//  IndexService
// -----------------------------------------------------------------------------
class IndexService {
public:
    using Snapshot = std::shared_ptr<const hdl::HdlIndex>;
    using TextLoader = hdl::TextLoader;

    // 计数器（A2/A4 会读它：generation 变化 = 索引内容变了）
    struct Stats {
        std::uint64_t generation = 0;  // 每安装一份新快照 +1
        std::size_t rebuilds = 0;      // 全量重建次数（冷构建 / .al 变更 / InvalidateAll）
        std::size_t file_updates = 0;  // 单文件增量安装/移除次数
    };

    // 目录 stamp：watcher 缺失时判断"磁盘上这个文件变了没变"
    struct FileStamp {
        bool exists = false;
        std::uintmax_t size = 0;
        std::int64_t mtime = 0;

        bool operator==(const FileStamp&) const = default;
    };

    // 默认 loader = platform::try_read_file_utf8（读盘，UTF-8，never-throw）。
    // 需要"未保存缓冲区优先"时由调用方注入（LspServer 传 LoadText 即可）。
    IndexService();
    explicit IndexService(TextLoader loader);
    ~IndexService();

    IndexService(const IndexService&) = delete;
    IndexService& operator=(const IndexService&) = delete;

    // 取当前快照（廉价：一次加锁 + 一次 shared_ptr 拷贝）。
    // 没有工程 / 还没有构建过时返回 nullptr（等价于"空索引"）。
    // watcher 未激活时顺带做目录 stamp 懒校验（只对变了的文件增量更新）。
    Snapshot Get() const;

    // 设置/更新工程模型。**不立即构建**（懒到下一次 Get），因为 handler 里
    // "先拿工程再问索引"是两步，而很多请求最后并不需要索引。
    // 若 al_path 与 HDL 文件集合都没变，则只更新工程字段、保留快照（无任何解析）。
    void SetProject(project::Project project);

    // didChange（或 didSave）之后的单文件更新。
    //   text.has_value() -> 用这份文本（未保存缓冲区）解析并安装；
    //   text == nullopt  -> 从磁盘重读；读不到就从索引里移除（等价于删除）。
    // 不在工程 HDL 文件集合里的路径直接忽略（不改变快照）。
    void OnFileChanged(std::string path, std::optional<std::string> text);

    // didChangeWatchedFiles。`.al` 变了 -> 重新解析 .al 并**全量重建**（懒）；
    // 其它 HDL 文件 -> 只重读/重解析变了的那些（增量）。
    void OnWatchedFiles(const std::vector<std::string>& changed,
                        const std::vector<std::string>& deleted);

    // 丢掉快照，下一次 Get 全量重建（解析缓存保留，重建仍然很快）。
    void InvalidateAll();

    // watcher 是否可用（客户端声明了 workspace/didChangeWatchedFiles）。
    //   true  -> 信任 OnWatchedFiles / OnFileChanged，Get 不做 stamp 校验；
    //   false -> Get 每次做目录 stamp 懒校验（默认值：false = 安全优先）。
    bool watcher_active() const;
    void SetWatcherActive(bool active);

    Stats stats() const;

    // ---- additive（不改变上面约定的语义，供集成/测试/诊断用）----
    bool has_project() const;         // 是否设置过工程
    std::string al_path() const;      // 当前工程 .al 路径（无工程时为空）
    bool truncated() const;           // 上次构建是否因预算被截断
    std::size_t parse_errors() const; // 上次构建的解析错误条数
    std::size_t tracked_files() const;  // 工程 HDL 文件数（不含 IP 生成物之外的）

private:
    // 全部可变状态放在一个结构里，`Get()` 才能在 const 方法里做懒重建。
    struct State {
        project::Project project;
        bool have_project = false;
        bool dirty = true;  // 需要全量重建

        Snapshot snapshot;  // 不可变快照

        // 工程参与索引的文件（Verilog + VHDL + IP 生成物），原样大小写；
        // expected_keys 是同样集合的小写键，用于"这个路径属不属于本工程"的判定。
        std::vector<std::string> expected;
        std::map<std::string, std::size_t> expected_index;  // key -> expected 下标

        // watcher 缺失时的比对基线：key -> 磁盘 stamp
        std::map<std::string, FileStamp> stamps;
        bool stamps_initialized = false;

        bool watcher_active = false;
        bool truncated = false;
        std::size_t parse_errors = 0;
        Stats stats;

        hdl::ParseCache cache;  // 跨重建复用（key = path + 内容哈希）
    };

    // 以下 *Locked 方法都要求已持有 mutex_。
    Snapshot EnsureFreshLocked() const;   // dirty -> 全量；无 watcher -> stamp 校验
    void RebuildAllLocked() const;
    void ValidateStampsLocked() const;
    void RefreshStampsLocked() const;     // 只记录基线，不做增量更新
    void InstallLocked(std::shared_ptr<const hdl::HdlIndex> next) const;
    void SetExpectedLocked(std::vector<std::string> files);

    bool IsExpected(std::string_view key) const;

    mutable std::mutex mutex_;
    mutable State state_;
    TextLoader loader_;
};

}  // namespace fpgalsp::server