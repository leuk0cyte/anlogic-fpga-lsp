// =============================================================================
//  src/platform/write_service.cpp —— 统一写入服务实现
// =============================================================================
//  设计说明见 include 头与 doc/design/write-service.md。
//  这里刻意保持"零业务依赖"：只依赖 platform 层（path / encoding）。
//  写入路径分类 -> 策略校验 -> 备份 -> 原子写 -> 审计 -> 指标。
//  0.25.0：目录/删除/改名也走同一服务；新增测试用内存后端与指标快照。
// =============================================================================

#include "platform/write_service.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <system_error>

#include "platform/encoding.h"
#include "platform/path.h"

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#endif

namespace fpgalsp::platform::writes {

namespace {

std::atomic<unsigned long long> g_temp_sequence{0};
std::mutex g_audit_mutex;

// 指标：进程内累计（fpgalsp.metrics 读取快照）。
std::atomic<std::uint64_t> g_writes_ok{0};
std::atomic<std::uint64_t> g_writes_failed{0};
std::atomic<std::uint64_t> g_denied{0};
std::atomic<std::uint64_t> g_bytes_written{0};
std::atomic<std::uint64_t> g_dirs_created{0};
std::atomic<std::uint64_t> g_removals{0};
std::atomic<std::uint64_t> g_renames{0};
std::atomic<std::uint64_t> g_copies{0};
std::atomic<std::uint64_t> g_backups{0};
std::atomic<std::uint64_t> g_by_root[4] = {};

std::string LowerCopy(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::string FileNameOf(std::string_view path) {
    const std::size_t pos = path.find_last_of("/\\");
    return pos == std::string_view::npos ? std::string(path) : std::string(path.substr(pos + 1));
}

std::string DirOf(std::string_view path) {
    const std::size_t pos = path.find_last_of("/\\");
    if (pos == std::string_view::npos) return {};
    std::string dir(path.substr(0, pos));
    if (dir.empty() && !path.empty() && (path.front() == '/' || path.front() == '\\')) return "/";
    return dir;
}

bool EndsWithNoCase(std::string_view text, std::string_view suffix) {
    if (text.size() < suffix.size()) return false;
    return LowerCopy(text.substr(text.size() - suffix.size())) == LowerCopy(suffix);
}

bool IsRegularFile(const std::string& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(std::filesystem::path(path), ec);
}

// 安全约束：路径段里不允许出现 ".fpgalsp"（任何前缀形式都算），也不允许 ".."。
bool HasForbiddenSegment(const std::string& normalized, std::string* why) {
    std::size_t start = 0;
    while (start <= normalized.size()) {
        const std::size_t pos = normalized.find('/', start);
        const std::string_view raw =
            pos == std::string::npos
                ? std::string_view(normalized).substr(start)
                : std::string_view(normalized).substr(start, pos - start);
        const std::string lower = LowerCopy(raw);
        if (lower == "..") {
            if (why != nullptr) *why = "path traversal segment '..' is not allowed";
            return true;
        }
        if (lower.find(".fpgalsp") != std::string::npos) {
            if (why != nullptr) {
                *why = "writing into '.fpgalsp' is not allowed (state lives in the OS temp dir; "
                       "see platform::state_dir)";
            }
            return true;
        }
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    return false;
}

bool IsProjectUserFile(const std::string& rel_to_project) {
    static const char* kExtensions[] = {".al", ".adc", ".sdc"};
    for (const char* ext : kExtensions) {
        if (EndsWithNoCase(rel_to_project, ext)) return true;
    }
    // ChipWatcher 清单与配置：chip_debugger/user.mdn、user.cfg（TD GUI 也写这里）
    const std::string lower = LowerCopy(rel_to_project);
    if (lower.rfind("chip_debugger/", 0) != 0) return false;
    return EndsWithNoCase(rel_to_project, ".mdn") || EndsWithNoCase(rel_to_project, ".cfg");
}

std::string RelativeWithin(const std::string& path, const std::string& root) {
    const std::string norm_path = LowerCopy(normalize_path(path));
    std::string norm_root = LowerCopy(normalize_path(root));
    while (norm_root.size() > 1 && norm_root.back() == '/') norm_root.pop_back();
    if (norm_path.size() <= norm_root.size()) return {};
    if (norm_path.compare(0, norm_root.size(), norm_root) != 0) return {};
    if (norm_path[norm_root.size()] != '/') return {};
    return norm_path.substr(norm_root.size() + 1);
}

std::string TempRoot() {
    std::error_code ec;
    const std::filesystem::path tmp = std::filesystem::temp_directory_path(ec);
    return ec ? std::string() : normalize_path(tmp.string());
}

std::string JsonEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buffer;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

void BumpRoot(Root root) { g_by_root[static_cast<int>(root)].fetch_add(1); }

}  // namespace

// -----------------------------------------------------------------------------
// 后端（真实磁盘 / 测试用内存）
// -----------------------------------------------------------------------------

class DiskBackend final : public Backend {
  public:
    bool WriteFile(const std::string& path, std::string_view bytes) override {
        return write_file_utf8(path, bytes);
    }
    bool AppendFile(const std::string& path, std::string_view bytes) override {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(DirOf(path)), ec);
        std::ofstream out(path, std::ios::binary | std::ios::app);
        if (!out) return false;
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        return static_cast<bool>(out);
    }
    bool EnsureDir(const std::string& path) override {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path), ec);
        return !ec || std::filesystem::is_directory(std::filesystem::path(path), ec);
    }
    bool Remove(const std::string& path, bool recursive) override {
        std::error_code ec;
        if (recursive) {
            std::filesystem::remove_all(std::filesystem::path(path), ec);
        } else {
            std::filesystem::remove(std::filesystem::path(path), ec);
        }
        return !ec;
    }
    bool Rename(const std::string& from, const std::string& to) override {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(DirOf(to)), ec);
        std::filesystem::rename(std::filesystem::path(from), std::filesystem::path(to), ec);
        if (!ec) return true;
#ifdef _WIN32
        return MoveFileExW(std::filesystem::path(from).wstring().c_str(),
                           std::filesystem::path(to).wstring().c_str(),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        return false;
#endif
    }
    bool CopyTo(const std::string& from, const std::string& to) override {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(DirOf(to)), ec);
        std::filesystem::copy_file(std::filesystem::path(from), std::filesystem::path(to),
                                   std::filesystem::copy_options::overwrite_existing, ec);
        return !ec;
    }
    bool Exists(const std::string& path) override {
        std::error_code ec;
        return std::filesystem::exists(std::filesystem::path(path), ec);
    }
};

class MemoryBackend final : public Backend {
  public:
    bool WriteFile(const std::string& path, std::string_view bytes) override {
        files_[path] = std::string(bytes);
        return true;
    }
    bool AppendFile(const std::string& path, std::string_view bytes) override {
        files_[path] += std::string(bytes);
        return true;
    }
    bool EnsureDir(const std::string& path) override {
        dirs_.insert(path);
        return true;
    }
    bool Remove(const std::string& path, bool recursive) override {
        bool removed = files_.erase(path) != 0;
        if (dirs_.erase(path) != 0) removed = true;
        if (recursive) {
            const std::string prefix = path + "/";
            for (auto it = files_.begin(); it != files_.end();) {
                if (it->first.rfind(prefix, 0) == 0) {
                    it = files_.erase(it);
                    removed = true;
                } else {
                    ++it;
                }
            }
            for (auto it = dirs_.begin(); it != dirs_.end();) {
                if (it->rfind(prefix, 0) == 0) {
                    it = dirs_.erase(it);
                    removed = true;
                } else {
                    ++it;
                }
            }
        }
        return removed || true;  // 删除不存在的东西也算成功（与 fs::remove 一致）
    }
    bool Rename(const std::string& from, const std::string& to) override {
        bool moved = false;
        if (const auto it = files_.find(from); it != files_.end()) {
            files_[to] = it->second;
            files_.erase(it);
            moved = true;
        }
        if (dirs_.erase(from) != 0) moved = true;
        dirs_.insert(to);
        const std::string prefix = from + "/";
        for (auto it = files_.begin(); it != files_.end();) {
            if (it->first.rfind(prefix, 0) == 0) {
                files_[to + "/" + it->first.substr(prefix.size())] = it->second;
                it = files_.erase(it);
                moved = true;
            } else {
                ++it;
            }
        }
        return moved || true;
    }
    bool CopyTo(const std::string& from, const std::string& to) override {
        const auto it = files_.find(from);
        if (it == files_.end()) return false;
        files_[to] = it->second;
        return true;
    }
    bool Exists(const std::string& path) override {
        return files_.count(path) != 0 || dirs_.count(path) != 0;
    }

  private:
    std::map<std::string, std::string> files_;
    std::set<std::string> dirs_;
};

namespace {

std::mutex g_backend_mutex;
std::unique_ptr<Backend> g_custom_backend;
DiskBackend g_disk_backend;

Backend& BackendRef() {
    std::lock_guard<std::mutex> lock(g_backend_mutex);
    return g_custom_backend != nullptr ? *g_custom_backend : g_disk_backend;
}

bool UsingMemoryBackend() {
    std::lock_guard<std::mutex> lock(g_backend_mutex);
    return g_custom_backend != nullptr;
}

void Audit(Root root, std::string_view project_dir, std::string_view display_path,
           std::size_t bytes, bool ok, std::string_view error, const char* caller) {
    if (UsingMemoryBackend()) return;  // 单测不写审计文件
    const char* disabled = std::getenv("FPGALSP_WRITE_AUDIT");
    if (disabled != nullptr && std::strcmp(disabled, "0") == 0) return;
    static const char* kRootNames[] = {"state", "project", "run", "temp"};
    const std::string audit_dir = state_dir(project_dir);
    const std::string audit_path = path_join(audit_dir, "logs/writes.jsonl");

    const auto now = std::chrono::system_clock::now();
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    std::string line = "{";
    line += "\"ts\":" + std::to_string(millis);
    line += ",\"root\":\"";
    line += kRootNames[static_cast<int>(root)];
    line += "\",\"path\":\"" + JsonEscape(display_path) + "\"";
    line += ",\"bytes\":" + std::to_string(bytes);
    line += ",\"ok\":";
    line += ok ? "true" : "false";
    line += ",\"caller\":\"" + JsonEscape(caller == nullptr ? "" : caller) + "\"";
    if (!ok) line += ",\"error\":\"" + JsonEscape(error) + "\"";
    line += "}\n";

    std::lock_guard<std::mutex> lock(g_audit_mutex);
    Backend& backend = BackendRef();
    (void)backend.EnsureDir(path_join(audit_dir, "logs"));
    (void)backend.AppendFile(audit_path, line);
}

bool AtomicWrite(const std::string& target, std::string_view bytes, std::string* error) {
    const std::string dir = DirOf(target);
    const std::string name = FileNameOf(target);
    if (dir.empty() || name.empty()) {
        if (error != nullptr) *error = "invalid target path: " + target;
        return false;
    }
    Backend& backend = BackendRef();
    if (!backend.EnsureDir(dir)) {
        if (error != nullptr) *error = "cannot create directory: " + dir;
        return false;
    }

#ifdef _WIN32
    const unsigned long long pid = static_cast<unsigned long long>(_getpid());
#else
    const unsigned long long pid = 0;
#endif
    const unsigned long long seq = g_temp_sequence.fetch_add(1) + 1;
    const std::string temp = path_join(dir, ".tmp-" + name + "-" + std::to_string(pid) + "-" +
                                                 std::to_string(seq));
    (void)backend.Remove(temp, false);
    if (!backend.WriteFile(temp, bytes)) {
        if (error != nullptr) *error = "cannot write temp file: " + temp;
        return false;
    }
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (backend.Rename(temp, target)) return true;
        if (error != nullptr) {
            *error = "cannot replace target (" + std::to_string(attempt + 1) + "/3): " + target;
        }
    }
    (void)backend.Remove(temp, false);
    return false;
}

void BackupIfRequested(const std::string& project_dir, const std::string& target,
                       bool backup) {
    if (!backup || !IsRegularFile(target)) return;
    const std::string rel = LowerCopy(DisplayPath(project_dir, target));
    if (rel.empty()) return;
    std::string safe;
    for (const char c : rel) {
        safe.push_back((c == '/' || c == '\\' || c == ':') ? '_' : c);
    }
    const std::string backup_path =
        path_join(path_join(state_dir(project_dir), "backup"), safe + ".bak");
    const ReadFileResult previous = read_file_utf8(target);
    if (!previous.ok) return;
    std::string ignored;
    if (AtomicWrite(backup_path, previous.content, &ignored)) g_backups.fetch_add(1);
}

}  // namespace

// DisplayPathImpl 先实现（BackupIfRequested 用到），再对外暴露。
std::string DisplayPathImpl(std::string_view project_dir, std::string_view path) {
    const std::string normalized = normalize_path(path);
    if (!project_dir.empty()) {
        const std::string rel = RelativeWithin(normalized, normalize_path(project_dir));
        if (!rel.empty()) return rel;
    }
    const std::string state_rel = RelativeWithin(normalized, state_dir(project_dir));
    if (!state_rel.empty()) return "<state>/" + state_rel;
    return normalized;
}

std::string DisplayPath(std::string_view project_dir, std::string_view path) {
    return DisplayPathImpl(project_dir, path);
}

bool Classify(std::string_view project_dir, std::string_view path, Root* out,
              std::string* error) {
    if (out == nullptr) return false;
    std::string raw(path);
    if (raw.empty()) {
        if (error != nullptr) *error = "empty path";
        return false;
    }
    if (raw.find('\0') != std::string::npos) {
        if (error != nullptr) *error = "path contains NUL";
        return false;
    }
    std::string normalized = normalize_path(raw);
    if (normalized.empty()) {
        if (error != nullptr) *error = "invalid path: " + raw;
        return false;
    }
    if (!is_absolute_path(normalized)) {
        if (project_dir.empty()) {
            if (error != nullptr) *error = "relative path without project dir: " + raw;
            return false;
        }
        normalized = path_join(normalize_path(project_dir), normalized);
    }
    std::string why;
    if (HasForbiddenSegment(normalized, &why)) {
        if (error != nullptr) *error = why;
        return false;
    }

    // 1) 状态目录（最优先：所有缓存/日志/审计都在这里）
    const std::string state_root = state_dir(project_dir);
    if (path_is_within_canonical(normalized, state_root)) {
        *out = Root::kState;
        return true;
    }

    // 2) 工程目录：run 目录（TD 产物）与登记过的用户文件
    if (!project_dir.empty() && path_is_within_canonical(normalized, project_dir)) {
        const std::string rel = RelativeWithin(normalized, std::string(project_dir));
        if (rel.empty()) {  // 工程根目录本身
            *out = Root::kProject;
            return true;
        }
        const std::size_t slash = rel.find('/');
        const std::string first = slash == std::string::npos ? rel : rel.substr(0, slash);
        if (first.size() > 5 && LowerCopy(first).substr(first.size() - 5) == "_runs") {
            *out = Root::kRun;
            return true;
        }
        if (IsProjectUserFile(rel)) {
            *out = Root::kProject;
            return true;
        }
        if (error != nullptr) {
            *error = "refusing to write unregistered project file: " + rel +
                     " (allowed: *.al/*.adc/*.sdc, chip_debugger/*.mdn|*.cfg, <prj>_Runs/**)";
        }
        return false;
    }

    // 3) 系统临时目录下的 fpgalsp scratch（放最后：工程目录即使位于 temp 下也应优先识别）
    const std::string temp_root = TempRoot();
    if (!temp_root.empty() && path_is_within_canonical(normalized, temp_root) &&
        LowerCopy(normalized).find("/fpgalsp") != std::string::npos) {
        *out = Root::kTemp;
        return true;
    }

    if (error != nullptr) *error = "path is outside all allowed roots: " + normalized;
    return false;
}

std::string StatePath(std::string_view project_dir, std::string_view rel) {
    return path_join(state_dir(project_dir), rel);
}

Result Write(std::string_view project_dir, std::string_view path, std::string_view bytes,
             const Options& options) {
    Result result;
    try {
        std::string error;
        Root root = Root::kState;
        if (!Classify(project_dir, path, &root, &error)) {
            g_denied.fetch_add(1);
            result.ok = false;
            result.error = error;
            return result;
        }
        std::string normalized = normalize_path(path);
        if (!is_absolute_path(normalized)) {
            normalized = path_join(normalize_path(project_dir), normalized);
        }
        result.root = root;
        result.path = normalized;
        const std::string display = DisplayPathImpl(project_dir, normalized);

        if (options.dry_run) {
            result.ok = true;
            return result;
        }

        // kProject 只允许写到已存在的目录（不替用户在工程里新建目录树）。
        if (root == Root::kProject) {
            const std::string parent = DirOf(normalized);
            if (parent.empty() || !BackendRef().Exists(parent)) {
                result.ok = false;
                result.error = "parent directory does not exist for project file: " + normalized;
                g_writes_failed.fetch_add(1);
                Audit(root, project_dir, display, bytes.size(), false, result.error,
                      options.caller);
                return result;
            }
        }

        BackupIfRequested(std::string(project_dir), normalized, options.backup);
        std::string write_error;
        if (!AtomicWrite(normalized, bytes, &write_error)) {
            result.ok = false;
            result.error = write_error;
            g_writes_failed.fetch_add(1);
            Audit(root, project_dir, display, bytes.size(), false, write_error, options.caller);
            return result;
        }
        result.ok = true;
        g_writes_ok.fetch_add(1);
        g_bytes_written.fetch_add(bytes.size());
        BumpRoot(root);
        Audit(root, project_dir, display, bytes.size(), true, {}, options.caller);
        return result;
    } catch (const std::exception& exc) {
        g_writes_failed.fetch_add(1);
        result.ok = false;
        result.error = std::string("write failed: ") + exc.what();
        return result;
    } catch (...) {
        g_writes_failed.fetch_add(1);
        result.ok = false;
        result.error = "write failed: unknown exception";
        return result;
    }
}

Result AppendLine(std::string_view project_dir, std::string_view path, std::string_view line,
                  const Options& options) {
    Result result;
    try {
        Root root = Root::kState;
        std::string error;
        if (!Classify(project_dir, path, &root, &error)) {
            g_denied.fetch_add(1);
            result.ok = false;
            result.error = error;
            return result;
        }
        if (root == Root::kProject) {
            result.ok = false;
            result.error = "append is not allowed on project files: " + std::string(path);
            return result;
        }
        std::string normalized = normalize_path(path);
        if (!is_absolute_path(normalized)) {
            normalized = path_join(normalize_path(project_dir), normalized);
        }
        result.root = root;
        result.path = normalized;
        if (options.dry_run) {
            result.ok = true;
            return result;
        }
        Backend& backend = BackendRef();
        if (!backend.EnsureDir(DirOf(normalized))) {
            result.ok = false;
            result.error = "cannot create directory for log: " + DirOf(normalized);
            return result;
        }
        std::string payload(line);
        if (payload.empty() || payload.back() != '\n') payload.push_back('\n');
        if (!backend.AppendFile(normalized, payload)) {
            result.ok = false;
            result.error = "append failed: " + normalized;
            return result;
        }
        result.ok = true;
        return result;
    } catch (const std::exception& exc) {
        result.ok = false;
        result.error = std::string("append failed: ") + exc.what();
        return result;
    } catch (...) {
        result.ok = false;
        result.error = "append failed: unknown exception";
        return result;
    }
}

Result EnsureDir(std::string_view project_dir, std::string_view path,
                 const Options& options) {
    Result result;
    try {
        std::string normalized = normalize_path(path);
        if (normalized.empty() || normalized.find('\0') != std::string::npos) {
            result.ok = false;
            result.error = "invalid path";
            return result;
        }
        if (!is_absolute_path(normalized)) {
            if (project_dir.empty()) {
                result.ok = false;
                result.error = "relative path without project dir: " + std::string(path);
                return result;
            }
            normalized = path_join(normalize_path(project_dir), normalized);
        }
        Root root = Root::kState;
        std::string error;
        if (!Classify(project_dir, normalized, &root, &error)) {
            // Classify 只认"登记过的工程文件"；目录（如工程内已存在的 chip_debugger/）另判：
            // 工程内**已存在**的目录允许 EnsureDir（不新建），其它情况仍拒绝。
            if (!project_dir.empty() &&
                path_is_within_canonical(normalized, normalize_path(project_dir))) {
                const std::string rel = RelativeWithin(normalized, normalize_path(project_dir));
                const bool registered_dir =
                    rel == "chip_debugger" || rel.rfind("chip_debugger/", 0) == 0;
                if (!BackendRef().Exists(normalized) && !registered_dir) {
                    g_denied.fetch_add(1);
                    result.ok = false;
                    result.error = error;
                    return result;
                }
                if (!BackendRef().EnsureDir(normalized)) {
                    result.ok = false;
                    result.error = "cannot create directory: " + normalized;
                    return result;
                }
                g_dirs_created.fetch_add(1);
                result.root = Root::kProject;
                result.path = normalized;
                result.ok = true;
                return result;
            }
            g_denied.fetch_add(1);
            result.ok = false;
            result.error = error;
            return result;
        }
        result.root = root;
        result.path = normalized;
        if (options.dry_run) {
            result.ok = true;
            return result;
        }
        Backend& backend = BackendRef();
        if (root == Root::kProject) {
            // 工程目录只允许"已经存在"，不替用户新建目录。
            if (!backend.Exists(normalized)) {
                result.ok = false;
                result.error = "refusing to create a directory inside the project: " + normalized;
                g_denied.fetch_add(1);
                return result;
            }
            result.ok = true;
            return result;
        }
        if (!backend.EnsureDir(normalized)) {
            result.ok = false;
            result.error = "cannot create directory: " + normalized;
            return result;
        }
        g_dirs_created.fetch_add(1);
        result.ok = true;
        return result;
    } catch (const std::exception& exc) {
        result.ok = false;
        result.error = std::string("ensure_dir failed: ") + exc.what();
        return result;
    } catch (...) {
        result.ok = false;
        result.error = "ensure_dir failed: unknown exception";
        return result;
    }
}

Result Remove(std::string_view project_dir, std::string_view path, bool recursive,
              const Options& options) {
    Result result;
    try {
        Root root = Root::kState;
        std::string error;
        if (!Classify(project_dir, path, &root, &error)) {
            g_denied.fetch_add(1);
            result.ok = false;
            result.error = error;
            return result;
        }
        if (root == Root::kProject) {
            // 只允许删除我们/TD 生成的 chip_debugger 配置文件（user.mdn / user.cfg）；
            // 用户的 .al/.adc/.sdc 一律拒绝。
            const std::string rel = RelativeWithin(normalize_path(path),
                                                   normalize_path(project_dir));
            const std::string lower = LowerCopy(rel);
            const bool generated = lower.rfind("chip_debugger/", 0) == 0 &&
                                   (EndsWithNoCase(rel, ".mdn") || EndsWithNoCase(rel, ".cfg"));
            if (!generated) {
                g_denied.fetch_add(1);
                result.ok = false;
                result.error = "removing project files is not allowed: " + std::string(path);
                return result;
            }
        }
        std::string normalized = normalize_path(path);
        if (!is_absolute_path(normalized)) {
            normalized = path_join(normalize_path(project_dir), normalized);
        }
        result.root = root;
        result.path = normalized;
        if (options.dry_run) {
            result.ok = true;
            return result;
        }
        if (!BackendRef().Remove(normalized, recursive)) {
            result.ok = false;
            result.error = "remove failed: " + normalized;
            return result;
        }
        g_removals.fetch_add(1);
        result.ok = true;
        return result;
    } catch (const std::exception& exc) {
        result.ok = false;
        result.error = std::string("remove failed: ") + exc.what();
        return result;
    } catch (...) {
        result.ok = false;
        result.error = "remove failed: unknown exception";
        return result;
    }
}

Result Rename(std::string_view project_dir, std::string_view from, std::string_view to,
              const Options& options) {
    Result result;
    try {
        Root from_root = Root::kState;
        Root to_root = Root::kState;
        std::string error;
        if (!Classify(project_dir, from, &from_root, &error)) {
            g_denied.fetch_add(1);
            result.ok = false;
            result.error = error;
            return result;
        }
        if (!Classify(project_dir, to, &to_root, &error)) {
            g_denied.fetch_add(1);
            result.ok = false;
            result.error = error;
            return result;
        }
        if (from_root == Root::kProject || to_root == Root::kProject) {
            g_denied.fetch_add(1);
            result.ok = false;
            result.error = "renaming project files is not allowed";
            return result;
        }
        if (from_root != to_root) {
            g_denied.fetch_add(1);
            result.ok = false;
            result.error = "rename across roots is not allowed (cannot cross state/run/temp)";
            return result;
        }
        std::string from_norm = normalize_path(from);
        std::string to_norm = normalize_path(to);
        if (!is_absolute_path(from_norm)) {
            from_norm = path_join(normalize_path(project_dir), from_norm);
        }
        if (!is_absolute_path(to_norm)) {
            to_norm = path_join(normalize_path(project_dir), to_norm);
        }
        result.root = from_root;
        result.path = to_norm;
        if (options.dry_run) {
            result.ok = true;
            return result;
        }
        if (!options.force && BackendRef().Exists(to_norm)) {
            result.ok = false;
            result.error = "rename target already exists (pass force=true): " + to_norm;
            return result;
        }
        if (!BackendRef().Rename(from_norm, to_norm)) {
            result.ok = false;
            result.error = "rename failed: " + from_norm + " -> " + to_norm;
            return result;
        }
        g_renames.fetch_add(1);
        result.ok = true;
        return result;
    } catch (const std::exception& exc) {
        result.ok = false;
        result.error = std::string("rename failed: ") + exc.what();
        return result;
    } catch (...) {
        result.ok = false;
        result.error = "rename failed: unknown exception";
        return result;
    }
}

Result Copy(std::string_view project_dir, std::string_view from, std::string_view to,
            const Options& options) {
    Result result;
    try {
        Root from_root = Root::kState;
        Root to_root = Root::kState;
        std::string error;
        if (!Classify(project_dir, from, &from_root, &error)) {
            g_denied.fetch_add(1);
            result.ok = false;
            result.error = error;
            return result;
        }
        // 目标：非工程 root 一律允许；工程内只允许登记过的生成目录（chip_debugger/、log/）。
        bool project_dest_ok = false;
        if (!Classify(project_dir, to, &to_root, &error)) {
            if (!project_dir.empty() &&
                path_is_within_canonical(normalize_path(to), normalize_path(project_dir))) {
                const std::string rel = RelativeWithin(normalize_path(to),
                                                       normalize_path(project_dir));
                const std::string lower = LowerCopy(rel);
                project_dest_ok = lower.rfind("chip_debugger/", 0) == 0 ||
                                  lower.rfind("log/", 0) == 0;
                to_root = Root::kProject;
            }
            if (!project_dest_ok) {
                g_denied.fetch_add(1);
                result.ok = false;
                result.error = error;
                return result;
            }
        } else if (to_root == Root::kProject) {
            project_dest_ok = true;
        }
        std::string from_norm = normalize_path(from);
        std::string to_norm = normalize_path(to);
        if (!is_absolute_path(from_norm)) from_norm = path_join(normalize_path(project_dir), from_norm);
        if (!is_absolute_path(to_norm)) to_norm = path_join(normalize_path(project_dir), to_norm);
        result.root = to_root;
        result.path = to_norm;
        if (options.dry_run) {
            result.ok = true;
            return result;
        }
        if (!BackendRef().CopyTo(from_norm, to_norm)) {
            result.ok = false;
            result.error = "copy failed: " + from_norm + " -> " + to_norm;
            return result;
        }
        g_copies.fetch_add(1);
        result.ok = true;
        return result;
    } catch (const std::exception& exc) {
        result.ok = false;
        result.error = std::string("copy failed: ") + exc.what();
        return result;
    } catch (...) {
        result.ok = false;
        result.error = "copy failed: unknown exception";
        return result;
    }
}

Metrics SnapshotMetrics() {
    Metrics metrics;
    metrics.writes_ok = g_writes_ok.load();
    metrics.writes_failed = g_writes_failed.load();
    metrics.denied = g_denied.load();
    metrics.bytes_written = g_bytes_written.load();
    metrics.dirs_created = g_dirs_created.load();
    metrics.removals = g_removals.load();
    metrics.renames = g_renames.load();
    metrics.copies = g_copies.load();
    metrics.backups = g_backups.load();
    for (int i = 0; i < 4; ++i) metrics.by_root[i] = g_by_root[i].load();
    return metrics;
}

void ResetMetricsForTest() {
    for (std::atomic<std::uint64_t>* counter :
         {&g_writes_ok, &g_writes_failed, &g_denied, &g_bytes_written, &g_dirs_created,
          &g_removals, &g_renames, &g_backups, &g_copies}) {
        counter->store(0);
    }
    for (std::atomic<std::uint64_t>& counter : g_by_root) counter.store(0);
}

std::unique_ptr<Backend> MakeMemoryBackend() { return std::make_unique<MemoryBackend>(); }

void SetBackendForTest(std::unique_ptr<Backend> backend) {
    std::lock_guard<std::mutex> lock(g_backend_mutex);
    g_custom_backend = std::move(backend);
}

void ResetBackendForTest() {
    std::lock_guard<std::mutex> lock(g_backend_mutex);
    g_custom_backend.reset();
}

}  // namespace fpgalsp::platform::writes
