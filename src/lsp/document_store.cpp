// =============================================================================
//  src/lsp/document_store.cpp
// =============================================================================

#include "lsp/document_store.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <utility>

#include "platform/encoding.h"
#include "platform/path.h"
#include "platform/utf16.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fpgalsp::lsp {
namespace {

namespace plat = fpgalsp::platform;

// 从 json 里安全取一个非负整数；缺失/类型不符/负数都回落到默认值。
std::size_t ReadSize(const Json& obj, const char* key, std::size_t default_value = 0) {
    if (!obj.is_object()) return default_value;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_number()) return default_value;
    try {
        const auto v = it->get<int64_t>();
        if (v < 0) return default_value;
        return static_cast<std::size_t>(v);
    } catch (...) {
        return default_value;
    }
}

bool HasObjectKey(const Json& obj, const char* key) {
    if (!obj.is_object()) return false;
    const auto it = obj.find(key);
    return it != obj.end() && it->is_object();
}

// path_key = normalize_path + 小写。Open 时算一次并持久化，查询不再重复做。
// P1-2：原实现每次 GetTextByPath 都要对每个打开文档的 path 调一次 normalize_path
//（字符串副本 + 逐段切分 + 多轮拼接，约 0.3–1 µs/文档）。
std::string PathKey(std::string_view path) {
    std::string key = plat::normalize_path(std::string(path));
    for (char& c : key) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return key;
}

}  // namespace

// -----------------------------------------------------------------------------
// contentChanges 解析
// -----------------------------------------------------------------------------

std::vector<ContentChange> ParseContentChanges(const Json& content_changes) {
    std::vector<ContentChange> out;

    if (!content_changes.is_array()) {
        return out;
    }

    for (const auto& item : content_changes) {
        if (!item.is_object()) {
            continue;  // 容错：跳过非对象元素
        }

        ContentChange change;

        // text 字段：缺失时按空串处理（等价于纯删除）
        const auto text_it = item.find("text");
        if (text_it != item.end() && text_it->is_string()) {
            try {
                change.text = text_it->get<std::string>();
            } catch (...) {
                change.text.clear();
            }
        }

        // range：缺失或 null -> 全文替换
        const auto range_it = item.find("range");
        if (range_it != item.end() && HasObjectKey(item, "range") &&
            HasObjectKey(*range_it, "start") && HasObjectKey(*range_it, "end")) {
            const Json& range = *range_it;
            const Json& start = range["start"];
            const Json& end = range["end"];
            change.has_range = true;
            change.start_line = ReadSize(start, "line");
            change.start_character = ReadSize(start, "character");
            change.end_line = ReadSize(end, "line");
            change.end_character = ReadSize(end, "character");
        }

        out.push_back(std::move(change));
    }

    return out;
}

// -----------------------------------------------------------------------------
// 纯函数：应用变更
// -----------------------------------------------------------------------------

std::string DocumentStore::ApplyChange(std::string_view text, const ContentChange& change) {
    // 没有 range = 全文替换
    if (!change.has_range) {
        return change.text;
    }

    // 把 (line, UTF-16 column) 换算成字节偏移。
    // position_to_byte_index 内部按行切分 + utf16_column_to_byte_index，
    // 越界自动 clamp，never-raise。
    std::size_t start = plat::position_to_byte_index(text, change.start_line,
                                                     change.start_character);
    std::size_t end = plat::position_to_byte_index(text, change.end_line, change.end_character);

    // 防御：end < start（客户端给了反向 range）时按 LSP 的常识处理为不删除，
    // 只在 start 处插入，避免 substr 抛 out_of_range。
    if (end < start) {
        end = start;
    }
    start = std::min(start, text.size());
    end = std::min(end, text.size());

    std::string out;
    out.reserve(start + change.text.size() + (text.size() - end));
    out.append(text.substr(0, start));
    out.append(change.text);
    out.append(text.substr(end));
    return out;
}

std::string DocumentStore::ApplyChanges(std::string_view text,
                                        const std::vector<ContentChange>& changes) {
    std::string current(text);
    for (const auto& change : changes) {
        current = ApplyChange(current, change);
    }
    return current;
}

// -----------------------------------------------------------------------------
// 构造 / 磁盘读取
// -----------------------------------------------------------------------------

DocumentStore::DocumentStore() {
    disk_reader_ = [](std::string_view path) -> std::optional<std::string> {
        const auto result = plat::read_file_utf8(std::string(path));
        if (!result.ok) return std::nullopt;
        return result.content;
    };
}

DocumentStore::~DocumentStore() = default;

void DocumentStore::SetDiskReader(DiskReader reader) {
    std::unique_lock lock(mutex_);
    disk_reader_ = std::move(reader);
}

// -----------------------------------------------------------------------------
// 生命周期
// -----------------------------------------------------------------------------

void DocumentStore::Open(std::string uri, std::string text, std::optional<int64_t> version,
                         std::string language_id, std::string path) {
    if (path.empty()) {
        // 未显式给出路径时从 uri 反推
        path = plat::uri_to_path(uri);
    }

    std::unique_lock lock(mutex_);
    // 同一 uri 再次 Open：先摘掉旧的 path 索引条目，否则旧路径会指向新文档。
    if (const auto old = documents_.find(uri); old != documents_.end()) {
        if (!old->second.path_key.empty()) path_index_.erase(old->second.path_key);
    }
    Document doc;
    doc.path = std::move(path);
    doc.path_key = PathKey(doc.path);
    doc.text = std::make_shared<const std::string>(std::move(text));
    doc.version = version;
    doc.language_id = std::move(language_id);
    if (!doc.path_key.empty()) path_index_[doc.path_key] = uri;
    documents_[std::move(uri)] = std::move(doc);
}

bool DocumentStore::Change(std::string_view uri, const std::vector<ContentChange>& changes,
                           std::optional<int64_t> version) {
    std::unique_lock lock(mutex_);

    const auto it = documents_.find(uri);
    if (it == documents_.end()) {
        // 对应 Python：`doc = self.docs.get(uri); if not doc: return`
        return false;
    }

    Document& doc = it->second;

    // 版本处理（复现 server.py on_textDocument_didChange）：
    //   if not version or version >= (doc.version or 0): doc.version = version
    // 即：空版本 / 0 不更新；比当前小的乱序版本被忽略但**不崩**、内容仍然应用。
    if (version.has_value() && *version != 0) {
        const int64_t current = doc.version.value_or(0);
        if (*version >= current) {
            doc.version = *version;
        }
    }

    // 依次应用。P1-2：整串替换（不原地改），共享出去的旧文本保持有效。
    doc.text = std::make_shared<const std::string>(ApplyChanges(*doc.text, changes));
    return true;
}

bool DocumentStore::Save(std::string_view uri, const std::optional<std::string>& text) {
    std::unique_lock lock(mutex_);

    const auto it = documents_.find(uri);
    if (it == documents_.end()) {
        return false;
    }
    // 对应 Python：`if doc and params.get("text") is not None: doc.text = params["text"]`
    if (text.has_value()) {
        it->second.text = std::make_shared<const std::string>(*text);
    }
    // didSave 之后磁盘内容可能刚被写新：丢掉该路径的缓存，下一次未打开读取走磁盘。
    const std::string path = it->second.path;
    lock.unlock();
    EvictCached(path);
    return true;
}

bool DocumentStore::Close(std::string_view uri) {
    std::string path;
    bool erased = false;
    {
        std::unique_lock lock(mutex_);
        const auto it = documents_.find(uri);
        if (it != documents_.end()) {
            path = it->second.path;
            if (!it->second.path_key.empty()) path_index_.erase(it->second.path_key);
            documents_.erase(it);
            erased = true;
        }
    }
    // didClose 之后 GetText 回落读盘；先失效缓存，避免"关闭后读到关闭前的旧文本"。
    if (erased) EvictCached(path);
    return erased;
}

// -----------------------------------------------------------------------------
// 查询
// -----------------------------------------------------------------------------

bool DocumentStore::IsOpen(std::string_view uri) const {
    std::shared_lock lock(mutex_);
    return documents_.find(uri) != documents_.end();
}

DocumentStore::FileStamp DocumentStore::StatFile(const std::string& path) {
    FileStamp stamp;
#ifdef _WIN32
    // 一次 GetFileAttributesExW 同时拿到 size 与 mtime：比
    // std::filesystem::file_size + last_write_time 少一次系统调用
    // （本机单次 fs 调用 ~33 µs 量级，见 doc/audit/2026-11-18 HOTSPOTS.md §0）。
    const std::u16string wide16 = plat::utf8_to_utf16(path);
    if (wide16.empty()) return stamp;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (::GetFileAttributesExW(reinterpret_cast<LPCWSTR>(wide16.c_str()), GetFileExInfoStandard,
                               &data) == FALSE) {
        return stamp;
    }
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return stamp;
    stamp.size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    stamp.mtime = (static_cast<std::int64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                  static_cast<std::int64_t>(data.ftLastWriteTime.dwLowDateTime);
    stamp.valid = true;
#else
    std::error_code ec;
    const auto size = std::filesystem::file_size(std::filesystem::path(path), ec);
    if (ec) return stamp;
    const auto mtime = std::filesystem::last_write_time(std::filesystem::path(path), ec);
    if (ec) return stamp;
    stamp.size = static_cast<std::uint64_t>(size);
    stamp.mtime = static_cast<std::int64_t>(mtime.time_since_epoch().count());
    stamp.valid = true;
#endif
    return stamp;
}

bool DocumentStore::TryGetCached(const std::string& path, const FileStamp& stamp,
                                 std::string* out) const {
    if (out == nullptr) return false;
    std::shared_ptr<const std::string> shared;
    if (!TryGetCachedShared(path, stamp, &shared) || !shared) return false;
    *out = *shared;
    return true;
}

bool DocumentStore::TryGetCachedShared(const std::string& path, const FileStamp& stamp,
                                       std::shared_ptr<const std::string>* out) const {
    if (out == nullptr) return false;
    std::lock_guard<std::mutex> lock(cache_mutex_);
    const auto it = text_cache_.find(plat::path_key(path));
    if (it == text_cache_.end()) return false;
    if (!(it->second.stamp == stamp)) return false;  // 文件被改过 -> 当未命中
    it->second.last_used = ++cache_clock_;
    *out = it->second.text;  // 共享只读文本：缓存淘汰也不会让调用方悬垂
    return true;
}

void DocumentStore::PutCached(const std::string& path, const FileStamp& stamp,
                              std::shared_ptr<const std::string> text) const {
    if (!text || text->size() > kMaxEntryBytes || !stamp.valid) return;  // 单文件过大：不缓存
    const std::size_t bytes = text->size();
    std::lock_guard<std::mutex> lock(cache_mutex_);
    const std::string key = plat::path_key(path);
    if (const auto it = text_cache_.find(key); it != text_cache_.end()) {
        cache_bytes_ -= it->second.text ? it->second.text->size() : 0;
        text_cache_.erase(it);
    }
    // LRU 淘汰：先按条数，再按字节预算
    while (!text_cache_.empty() &&
           (text_cache_.size() >= kMaxCacheEntries ||
            cache_bytes_ + bytes > kMaxCacheBytes)) {
        auto victim = text_cache_.begin();
        for (auto p = text_cache_.begin(); p != text_cache_.end(); ++p) {
            if (p->second.last_used < victim->second.last_used) victim = p;
        }
        cache_bytes_ -= victim->second.text ? victim->second.text->size() : 0;
        text_cache_.erase(victim);
    }
    CachedText entry;
    entry.text = std::move(text);
    entry.stamp = stamp;
    entry.last_used = ++cache_clock_;
    cache_bytes_ += bytes;
    text_cache_.emplace(key, std::move(entry));
}

void DocumentStore::EvictCached(const std::string& path) const {
    if (path.empty()) return;
    std::lock_guard<std::mutex> lock(cache_mutex_);
    const auto it = text_cache_.find(plat::path_key(path));
    if (it == text_cache_.end()) return;
    cache_bytes_ -= it->second.text ? it->second.text->size() : 0;
    text_cache_.erase(it);
}

// 审查 C1（P0）的修复：watcher 说"磁盘变了"，就不再信任 (size,mtime)。
void DocumentStore::EvictTextCache(const std::string& path) const { EvictCached(path); }

std::shared_ptr<const std::string> DocumentStore::GetTextByPathShared(
    std::string_view path) const {
    const std::string wanted = PathKey(path);
    if (wanted.empty()) return nullptr;
    {
        std::shared_lock lock(mutex_);
        // P1-2：path→uri 索引，O(1)；不再遍历所有文档做 normalize_path。
        const auto found = path_index_.find(wanted);
        if (found != path_index_.end()) {
            const auto doc = documents_.find(found->second);
            if (doc != documents_.end()) return doc->second.text;  // 未保存缓冲区优先
        }
    }
    // 未打开：走"路径 -> uri -> 磁盘缓存"路径（缓存条目本身也是 shared_ptr）。
    return GetShared(plat::path_to_uri(std::string(path)));
}

std::optional<std::string> DocumentStore::GetTextByPath(std::string_view path) const {
    const std::shared_ptr<const std::string> shared = GetTextByPathShared(path);
    if (!shared) return std::nullopt;
    return *shared;
}

std::shared_ptr<const std::string> DocumentStore::GetShared(std::string_view uri) const {
    // ① 打开中的文档：**永远**返回缓冲区，绝不查缓存。
    {
        std::shared_lock lock(mutex_);
        const auto it = documents_.find(uri);
        if (it != documents_.end()) return it->second.text;
    }

    // 未打开：读磁盘。路径从 uri 反推。
    const std::string path = plat::uri_to_path(std::string(uri));
    if (path.empty()) return nullptr;

    // ② 缓存命中判定带 (mtime, size)：stamp 一致才复用，文件改过就重读。
    const FileStamp stamp = StatFile(path);
    if (stamp.valid) {
        std::shared_ptr<const std::string> cached;
        if (TryGetCachedShared(path, stamp, &cached)) return cached;
    }

    DiskReader reader;
    {
        std::shared_lock lock(mutex_);
        reader = disk_reader_;
    }
    if (!reader) return nullptr;
    std::optional<std::string> text = reader(path);
    if (!text.has_value()) return nullptr;
    auto shared = std::make_shared<const std::string>(std::move(*text));
    if (stamp.valid) PutCached(path, stamp, shared);
    return shared;
}

std::optional<std::string> DocumentStore::GetText(std::string_view uri) const {
    const std::shared_ptr<const std::string> shared = GetShared(uri);
    if (!shared) return std::nullopt;
    return *shared;
}

std::optional<int64_t> DocumentStore::GetVersion(std::string_view uri) const {
    std::shared_lock lock(mutex_);
    const auto it = documents_.find(uri);
    if (it == documents_.end()) return std::nullopt;
    return it->second.version;
}

std::optional<int64_t> DocumentStore::GetVersionByPath(std::string_view path) const {
    const std::string wanted = PathKey(path);
    if (wanted.empty()) return std::nullopt;
    std::shared_lock lock(mutex_);
    const auto found = path_index_.find(wanted);
    if (found == path_index_.end()) return std::nullopt;
    const auto it = documents_.find(found->second);
    if (it == documents_.end()) return std::nullopt;
    return it->second.version;
}

DocumentSnapshot DocumentStore::Snapshot(std::string_view uri) const {
    std::shared_lock lock(mutex_);
    const auto it = documents_.find(uri);
    if (it == documents_.end()) {
        return DocumentSnapshot{};
    }
    DocumentSnapshot snap;
    snap.uri = std::string(uri);
    snap.path = it->second.path;
    snap.text = *it->second.text;  // 拷贝：快照与后续编辑隔离
    snap.version = it->second.version;
    snap.language_id = it->second.language_id;
    return snap;
}

std::vector<std::string> DocumentStore::OpenUris() const {
    std::shared_lock lock(mutex_);
    std::vector<std::string> out;
    out.reserve(documents_.size());
    for (const auto& [uri, _] : documents_) out.push_back(uri);
    std::sort(out.begin(), out.end());
    return out;
}

std::size_t DocumentStore::Size() const {
    std::shared_lock lock(mutex_);
    return documents_.size();
}

}  // namespace fpgalsp::lsp