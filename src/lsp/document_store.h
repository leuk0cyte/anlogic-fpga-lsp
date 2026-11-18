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

    // 当前版本号；未打开或没有版本返回 nullopt。
    std::optional<int64_t> GetVersion(std::string_view uri) const;

    // 取不可变快照（线程安全）。文档未打开返回无效快照。
    DocumentSnapshot Snapshot(std::string_view uri) const;

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

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, Document> documents_;
    DiskReader disk_reader_;
};

}  // namespace fpgalsp::lsp