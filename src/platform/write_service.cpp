// =============================================================================
//  src/platform/write_service.cpp —— 统一写入服务实现
// =============================================================================
//  设计说明见 include 头与 doc/design/write-service.md。
//  这里刻意保持"零业务依赖"：只依赖 platform 层（path / encoding）。
//  写入路径分类 -> 策略校验 -> 备份 -> 原子写 -> 审计。
// =============================================================================

#include "platform/write_service.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>

#include "platform/encoding.h"
#include "platform/path.h"

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#endif

namespace fpgalsp::platform::writes {

// 由本文件末尾实现；匿名 namespace 里的备份逻辑要先用到它。
std::string DisplayPathImpl(std::string_view project_dir, std::string_view path);

namespace {

std::atomic<unsigned long long> g_temp_sequence{0};
std::mutex g_audit_mutex;

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

// 原子写：同目录临时文件 -> 重命名。失败清理临时文件，返回可诊断 error。
bool AtomicWrite(const std::string& target, std::string_view bytes, std::string* error) {
    const std::string dir = DirOf(target);
    const std::string name = FileNameOf(target);
    if (dir.empty() || name.empty()) {
        if (error != nullptr) *error = "invalid target path: " + target;
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(dir), ec);
    if (ec) {
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
    (void)std::filesystem::remove(std::filesystem::path(temp), ec);
    if (!write_file_utf8(temp, bytes)) {
        if (error != nullptr) *error = "cannot write temp file: " + temp;
        return false;
    }

    for (int attempt = 0; attempt < 3; ++attempt) {
        std::error_code rename_ec;
        std::filesystem::rename(std::filesystem::path(temp), std::filesystem::path(target),
                                rename_ec);
        if (!rename_ec) return true;

#ifdef _WIN32
        // std::filesystem::rename 在目标已存在/被杀软占用时会失败；用 MoveFileEx 兜底。
        if (MoveFileExW(std::filesystem::path(temp).wstring().c_str(),
                        std::filesystem::path(target).wstring().c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
            return true;
        }
#endif
        if (error != nullptr) {
            *error = "cannot replace target (" + std::to_string(attempt + 1) + "/3): " +
                     target + " (" + rename_ec.message() + ")";
        }
    }
    std::error_code cleanup_ec;
    (void)std::filesystem::remove(std::filesystem::path(temp), cleanup_ec);
    return false;
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

void Audit(Root root, std::string_view project_dir, std::string_view display_path,
           std::size_t bytes, bool ok, std::string_view error, const char* caller) {
    const char* disabled = std::getenv("FPGALSP_WRITE_AUDIT");
    if (disabled != nullptr && std::strcmp(disabled, "0") == 0) return;
    static const char* kRootNames[] = {"state", "project", "run", "temp"};
    const std::string audit_dir = state_dir(project_dir);
    const std::string audit_path = path_join(audit_dir, "logs/writes.jsonl");
    const auto now = std::chrono::system_clock::now();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch())
                            .count();
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
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path_join(audit_dir, "logs")), ec);
    std::ofstream out(audit_path, std::ios::binary | std::ios::app);
    if (!out) return;
    out << line;
    out.flush();
}

void BackupIfRequested(const std::string& project_dir, const std::string& target,
                       bool backup) {
    if (!backup || !IsRegularFile(target)) return;
    const std::string rel = LowerCopy(DisplayPathImpl(project_dir, target));
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
    (void)AtomicWrite(backup_path, previous.content, &ignored);
}

}  // namespace

// DisplayPath 先实现（BackupIfRequested 用到），再对外暴露。
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


    // 3) 工程目录：run 目录（TD 产物）与登记过的用户文件
    if (!project_dir.empty() && path_is_within_canonical(normalized, project_dir)) {
        const std::string rel = RelativeWithin(normalized, std::string(project_dir));
        const std::size_t slash = rel.find('/');
        const std::string first = slash == std::string::npos ? rel : rel.substr(0, slash);
        if (first.size() > 5 && LowerCopy(first).substr(first.size() - 5) == "_runs") {
            // TD 的 run 目录：允许创建（取消用的 .stop.f 可能在 run 目录刚建时写入），
            // 但仍限定在 <prj>_Runs/** 之下。
            *out = Root::kRun;
            return true;
        }
        if (IsProjectUserFile(rel)) {
            *out = Root::kProject;
            return true;
        }
        if (error != nullptr) {
            *error = "refusing to write unregistered project file: " + rel +
                     " (allowed: *.al/*.adc/*.sdc, chip_debugger/*.mdn, <prj>_Runs/**)";
        }
        return false;
    }

    // 4) 系统临时目录下的 fpgalsp scratch（放最后：工程目录即使位于 temp 下也应优先识别）
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

std::string DisplayPath(std::string_view project_dir, std::string_view path) {
    return DisplayPathImpl(project_dir, path);
}

Result Write(std::string_view project_dir, std::string_view path, std::string_view bytes,
             const Options& options) {
    Result result;
    try {
        std::string error;
        Root root = Root::kState;
        if (!Classify(project_dir, path, &root, &error)) {
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
            // dry-run：只校验路径策略，不落盘、不审计。
            result.ok = true;
            return result;
        }

        // kProject 只允许写到已存在的目录（不替用户在工程里新建目录树）；
        // kState/kTemp/kRun 允许自动建目录（缓存/日志/run 产物）。
        if (root == Root::kProject) {
            std::error_code dir_ec;
            const std::string parent = DirOf(normalized);
            if (parent.empty() ||
                !std::filesystem::is_directory(std::filesystem::path(parent), dir_ec)) {
                result.ok = false;
                result.error = "parent directory does not exist for project file: " + normalized;
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
            Audit(root, project_dir, display, bytes.size(), false, write_error, options.caller);
            return result;
        }
        result.ok = true;
        Audit(root, project_dir, display, bytes.size(), true, {}, options.caller);
        return result;
    } catch (const std::exception& exc) {
        result.ok = false;
        result.error = std::string("write failed: ") + exc.what();
        return result;
    } catch (...) {
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
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(DirOf(normalized)), ec);
        if (ec) {
            result.ok = false;
            result.error = "cannot create directory for log: " + DirOf(normalized);
            return result;
        }
        std::ofstream out(normalized, std::ios::binary | std::ios::app);
        if (!out) {
            result.ok = false;
            result.error = "cannot append to: " + normalized;
            return result;
        }
        out.write(line.data(), static_cast<std::streamsize>(line.size()));
        if (line.empty() || line.back() != '\n') out.put('\n');
        out.flush();
        result.ok = static_cast<bool>(out);
        if (!result.ok) result.error = "append failed: " + normalized;
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

}  // namespace fpgalsp::platform::writes
