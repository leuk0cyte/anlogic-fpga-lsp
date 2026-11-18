// =============================================================================
//  src/lsp/document_store.cpp
// =============================================================================

#include "lsp/document_store.h"

#include <algorithm>
#include <mutex>
#include <utility>

#include "platform/encoding.h"
#include "platform/path.h"
#include "platform/utf16.h"

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
    Document doc;
    doc.path = std::move(path);
    doc.text = std::move(text);
    doc.version = version;
    doc.language_id = std::move(language_id);
    documents_[std::move(uri)] = std::move(doc);
}

bool DocumentStore::Change(std::string_view uri, const std::vector<ContentChange>& changes,
                           std::optional<int64_t> version) {
    std::unique_lock lock(mutex_);

    const auto it = documents_.find(std::string(uri));
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

    // 依次应用
    doc.text = ApplyChanges(doc.text, changes);
    return true;
}

bool DocumentStore::Save(std::string_view uri, const std::optional<std::string>& text) {
    std::unique_lock lock(mutex_);

    const auto it = documents_.find(std::string(uri));
    if (it == documents_.end()) {
        return false;
    }
    // 对应 Python：`if doc and params.get("text") is not None: doc.text = params["text"]`
    if (text.has_value()) {
        it->second.text = *text;
    }
    return true;
}

bool DocumentStore::Close(std::string_view uri) {
    std::unique_lock lock(mutex_);
    return documents_.erase(std::string(uri)) > 0;
}

// -----------------------------------------------------------------------------
// 查询
// -----------------------------------------------------------------------------

bool DocumentStore::IsOpen(std::string_view uri) const {
    std::shared_lock lock(mutex_);
    return documents_.find(std::string(uri)) != documents_.end();
}

std::optional<std::string> DocumentStore::GetText(std::string_view uri) const {
    std::string path;
    {
        std::shared_lock lock(mutex_);
        const auto it = documents_.find(std::string(uri));
        if (it != documents_.end()) {
            return it->second.text;  // 未保存缓冲区优先
        }
    }

    // 未打开：读磁盘。路径从 uri 反推。
    path = plat::uri_to_path(std::string(uri));
    if (path.empty()) {
        return std::nullopt;
    }

    DiskReader reader;
    {
        std::shared_lock lock(mutex_);
        reader = disk_reader_;
    }
    if (!reader) return std::nullopt;
    return reader(path);
}

std::optional<int64_t> DocumentStore::GetVersion(std::string_view uri) const {
    std::shared_lock lock(mutex_);
    const auto it = documents_.find(std::string(uri));
    if (it == documents_.end()) return std::nullopt;
    return it->second.version;
}

DocumentSnapshot DocumentStore::Snapshot(std::string_view uri) const {
    std::shared_lock lock(mutex_);
    const auto it = documents_.find(std::string(uri));
    if (it == documents_.end()) {
        return DocumentSnapshot{};
    }
    DocumentSnapshot snap;
    snap.uri = std::string(uri);
    snap.path = it->second.path;
    snap.text = it->second.text;  // 拷贝：快照与后续编辑隔离
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