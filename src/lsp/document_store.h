// =============================================================================
//  src/lsp/document_store.h —— 打开文档的缓冲区管理（UTF-16 增量同步）
// =============================================================================
//  对应参考实现：Y:\fpga-lsp\fpgalsp\server.py
//      on_textDocument_didOpen / didChange / didSave / didClose
//      _apply_incremental
//
//  LSP 增量同步要点（PROTOCOL-V2.md v7 附录：textDocumentSync.change = 2）：
//   * contentChanges[].range 的 start/end 是 **UTF-16 列**（不是字符索引、不是字节）
//   * 先按 range 删除，再在同一位置插入 text
//   * 同一批 contentChanges 必须 **依次** 应用（后一个的坐标基于前一个的结果）
//   * range 缺失/null = 全文替换
//
//  ⚠ 最关键的边界：range 端点落在 emoji（代理对）中间。
//    实现直接复用 platform/utf16.h 的 utf16_column_to_byte_index，
//    其 clamp 语义已与 Python 的 utf16_to_py 逐个数值核对过。
// =============================================================================

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace fpgalsp::lsp {

using Json = nlohmann::json;

// 一个已打开文档的不可变快照。worker 线程拿到它之后可以安全地随意读取，
// 主线程后续的编辑不会影响它。
struct DocumentSnapshot {
    std::string uri;
    std::string path;  // 规范化后的文件系统路径
    std::string text;
    std::optional<int64_t> version;
    std::string language_id;

    bool valid() const { return !uri.empty(); }
};

// contentChanges[] 里的一条内容变更。
struct ContentChange {
    // range 缺失或为 null -> 全文替换
    bool has_range = false;
    std::size_t start_line = 0;
    std::size_t start_character = 0;  // UTF-16 列
    std::size_t end_line = 0;
    std::size_t end_character = 0;  // UTF-16 列
    std::string text;
};

// 从 LSP 的 contentChanges 数组解析出 ContentChange 列表。
// 容错：不是对象的元素跳过；range 字段缺失/类型不对时退化成全文替换。
std::vector<ContentChange> ParseContentChanges(const Json& content_changes);

class DocumentStore {
public:
    DocumentStore();
    ~DocumentStore();

    DocumentStore(const DocumentStore&) = delete;
    DocumentStore& operator=(const DocumentStore&) = delete;

    // ---- 生命周期 ----

    // didOpen。若 uri 已存在则覆盖。
    void Open(std::string uri, std::string text, std::optional<int64_t> version = std::nullopt,
              std::string language_id = {}, std::string path = {});

    // didChange。doc 不存在时返回 false（对应 Python 的 `if not doc: return`）。
    // 依次应用 changes；版本号按 LSP 语义处理（见 .cpp 注释）。
    bool Change(std::string_view uri, const std::vector<ContentChange>& changes,
                std::optional<int64_t> version = std::nullopt);

    // didSave。params.text 存在时用其替换缓冲区内容。
    // 返回 false 表示文档不存在（仍会记录路径以便后续读盘）。
    bool Save(std::string_view uri, const std::optional<std::string>& text = std::nullopt);

    // didClose。之后 GetText 回落到读磁盘。
    bool Close(std::string_view uri);

    // ---- 查询 ----

    bool IsOpen(std::string_view uri) const;

    // 当前文本。已打开 -> 缓冲区内容；否则读磁盘（UTF-8，非法字节替换）。
    // 文件不存在返回 nullopt。
    std::optional<std::string> GetText(std::string_view uri) const;

    // 按**文件系统路径**取当前文本（大小写/分隔符/URI 编码无关）。
    // 客户端发来的 uri 形如 `file:///y%3A/...`（小写盘符 + 百分号编码），而内部
    // `path_to_uri()` 产出 `file:///Y:/...`；用 uri 查表会 miss -> 回落到磁盘 ->
    // 未保存修改不可见（格式化/编辑后语义高亮错位的根因）。
    std::optional<std::string> GetTextByPath(std::string_view path) const;

    // 当前版本号；未打开或没有版本返回 nullopt。
    std::optional<int64_t> GetVersion(std::string_view uri) const;

    // 取不可变快照（线程安全）。文档未打开返回无效快照。
    DocumentSnapshot Snapshot(std::string_view uri) const;

    // 文本缓存失效（文件系统路径，不是 uri）。
    //
    // 为什么必须有这条**外部**失效通道：缓存键是 (path, size, mtime)，它无法区分
    // "同大小且 mtime 被还原"的改写；而 `workspace/didChangeWatchedFiles` 事件
    // 本来就说明"磁盘变了"，此时信任 mtime 等于自欺。watcher 报变化/删除时调用。
    void EvictTextCache(const std::string& path) const;

    // 所有已打开文档的 uri 列表（测试/调试用）。
    std::vector<std::string> OpenUris() const;

    // 已打开文档数量。
    std::size_t Size() const;

    // ---- 纯函数：应用单个变更（便于单独测试） ----

    // 把 range 应用到 text 上。语义与 LSP 一致，越界自动 clamp，never-raise。
    static std::string ApplyChange(std::string_view text, const ContentChange& change);

    // 依次应用多条变更。
    static std::string ApplyChanges(std::string_view text,
                                    const std::vector<ContentChange>& changes);

    // 设置磁盘读取函数（测试注入用；默认用 platform::read_file_utf8）。
    using DiskReader = std::function<std::optional<std::string>(std::string_view path)>;
    void SetDiskReader(DiskReader reader);

private:
    struct Document {
        std::string path;
        std::string text;
        std::optional<int64_t> version;
        std::string language_id;
    };

    // -------------------------------------------------------------------------
    //  F5 / R6：未打开文档的磁盘文本缓存
    // -------------------------------------------------------------------------
    //  为什么需要：completion 每次经 LspServer::LoadText -> GetText，未打开文档
    //  直接读盘（100 KB 文件 0.828 ms，是补全算法本身的 4.5 倍）。
    //
    //  正确性的两条硬约束：
    //   1) **打开中的文档永远不走缓存**（GetText 先查缓冲区，命中即返回）——
    //      否则用户 edit 完会看到旧内容；
    //   2) 缓存键带 (mtime, size)：文件被外部改动 -> stamp 不匹配 -> 重新读盘，
    //      不会返回陈旧文本。
    //
    //  上界：64 条 / 32 MB 总字节 / 单条 8 MB，超出按 LRU 淘汰。
    //  依据：本项目工程典型 20–200 个 HDL 文件、单个 1–100 KB（真实工程 21 个 .v
    //  共 0.2 MB），200 个 100 KB 文件也只占 20 MB；32 MB 上限足够覆盖一个
    //  1000 文件的大工程而不会让常驻内存失控。
    struct FileStamp {
        std::uint64_t size = 0;
        std::int64_t mtime = 0;  // Windows: FILETIME (100ns since 1601)
        bool valid = false;
        bool operator==(const FileStamp&) const = default;
    };
    struct CachedText {
        std::string text;
        FileStamp stamp;
        std::uint64_t last_used = 0;  // 单调递增的 LRU 时间戳
    };

    static constexpr std::size_t kMaxCacheEntries = 64;
    static constexpr std::size_t kMaxCacheBytes = 32u * 1024u * 1024u;
    static constexpr std::size_t kMaxEntryBytes = 8u * 1024u * 1024u;

    // 单次系统调用拿 size+mtime（Windows: GetFileAttributesExW）。
    // 失败（文件不存在/无权限/是目录）返回 valid=false -> 该次调用不参与缓存。
    static FileStamp StatFile(const std::string& path);

    // 缓存读写（只碰 cache_mutex_，与 documents_ 的 shared_mutex 分开，
    // 避免缓存命中时还要拿文档表的写锁）。
    bool TryGetCached(const std::string& path, const FileStamp& stamp, std::string* out) const;
    void PutCached(const std::string& path, const FileStamp& stamp, std::string text) const;
    // 丢弃某个路径的缓存条目（didSave / didClose 时调用，属于防御性失效）。
    void EvictCached(const std::string& path) const;

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, Document> documents_;
    DiskReader disk_reader_;

    mutable std::mutex cache_mutex_;
    mutable std::unordered_map<std::string, CachedText> text_cache_;
    mutable std::size_t cache_bytes_ = 0;
    mutable std::uint64_t cache_clock_ = 0;
};

}  // namespace fpgalsp::lsp