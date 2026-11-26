// =============================================================================
//  src/reports/artifacts.cpp —— 流程产物探测
// =============================================================================

#include "reports/artifacts.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <re2/re2.h>

#include "platform/utf16.h"
#include "reports/report_text.h"

namespace fpgalsp::reports {
namespace {

namespace fs = std::filesystem;

// FILETIME(1601-01-01 起的 100ns) -> Unix epoch 秒。
// 常数与算式与 CPython 的 os.stat() 一致，这样 mtime 能和 Python 逐值对上。
constexpr double kFiletimeEpochOffset = 116444736000000000.0;

std::wstring WidePath(std::string_view utf8) {
    const std::u16string utf16 = platform::utf8_to_utf16(utf8);
    std::wstring wide;
    wide.reserve(utf16.size());
    for (const char16_t unit : utf16) wide.push_back(static_cast<wchar_t>(unit));
    return wide;
}

std::string Utf8FromPath(const fs::path& path) {
    const std::u8string utf8 = path.u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

}  // namespace

jsonrpc::Json ArtifactFile::ToJson() const {
    jsonrpc::Json out = jsonrpc::Json::object();
    out["path"] = path;
    out["exists"] = exists;
    if (size.has_value()) out["size"] = *size;
    if (mtime.has_value()) out["mtime"] = *mtime;
    return out;
}

jsonrpc::Json RunEntry::ToJson() const {
    jsonrpc::Json out = jsonrpc::Json::object();
    out["name"] = name;
    out["dir"] = dir;
    out["exists"] = exists;
    return out;
}

jsonrpc::Json ArtifactsReport::ToJson() const {
    jsonrpc::Json runs_json = jsonrpc::Json::array();
    for (const RunEntry& run : runs) runs_json.push_back(run.ToJson());

    jsonrpc::Json out = jsonrpc::Json::object();
    out["runs"] = std::move(runs_json);
    out["bit"] = bit.ToJson();
    out["syn_db"] = syn_db.ToJson();
    out["route_db"] = route_db.ToJson();
    return out;
}

std::string JoinPath(std::string_view dir, std::string_view name) {
    if (dir.empty()) return std::string(name);
    std::string out(dir);
    const char tail = out.back();
    if (tail != '/' && tail != '\\') {
        // ⚠ 这里用 '\\' 而不是 '/'，是**故意的**（见 PYTHON-BUGS P-13）：
        //   Python 侧是 os.path.join(dir, name)，而 os.sep 在 Windows 上是 '\\'。
        //   project.dir 本身来自 norm()，是正斜杠；于是 reports.collect 输出的路径
        //   实际是 "C:/work/proj\\CorpusBeta_Runs\\phy_1\\route.qor" 这种混合形态。
        //   为了 JSON 逐字段一致，这里复现同样的拼接规则。
        //   （Windows 上两种分隔符等价，webview 的 vscode.Uri.file 都能处理。）
        out.push_back('\\');
    }
    out += name;
    return out;
}

// 测试与内部比较用：把分隔符统一成正斜杠，只用于展示/断言，不参与 JSON 输出
std::string ToForwardSlashes(std::string text) {
    for (char& c : text) {
        if (c == '\\') c = '/';
    }
    return text;
}

bool IsFile(std::string_view path) {
    if (path.empty()) return false;
    std::error_code ec;
    const fs::file_status status = fs::status(fs::path(WidePath(path)), ec);
    if (ec) return false;
    return fs::is_regular_file(status);
}

bool IsDirectory(std::string_view path) {
    if (path.empty()) return false;
    std::error_code ec;
    const fs::file_status status = fs::status(fs::path(WidePath(path)), ec);
    if (ec) return false;
    return fs::is_directory(status);
}

ArtifactFile StatFile(std::string_view path) {
    ArtifactFile result;
    result.path = std::string(path);
    if (path.empty()) return result;

    // 用 Win32 而不是 std::filesystem::last_write_time：要拿到与 os.stat() 完全
    // 相同的 size/mtime（std::filesystem 的 file_clock 换算会有 1e-7 级别偏差）。
    WIN32_FILE_ATTRIBUTE_DATA data{};
    const std::wstring wide = WidePath(path);
    if (!GetFileAttributesExW(wide.c_str(), GetFileExInfoStandard, &data)) return result;
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return result;

    const unsigned long long size =
        (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    const unsigned long long ticks =
        (static_cast<unsigned long long>(data.ftLastWriteTime.dwHighDateTime) << 32) |
        data.ftLastWriteTime.dwLowDateTime;

    result.exists = true;
    result.size = static_cast<int64_t>(size);
    result.mtime = (static_cast<double>(ticks) - kFiletimeEpochOffset) / 10000000.0;
    return result;
}

std::vector<std::string> ListSubdirectories(std::string_view dir) {
    std::vector<std::string> out;
    if (dir.empty()) return out;

    std::error_code ec;
    fs::directory_iterator iterator(fs::path(WidePath(dir)), ec);
    if (ec) return out;
    const fs::directory_iterator end;
    while (iterator != end) {
        const fs::path& entry_path = iterator->path();
        std::error_code status_ec;
        if (fs::is_directory(entry_path, status_ec) && !status_ec) {
            out.push_back(Utf8FromPath(entry_path.filename()));
        }
        iterator.increment(ec);
        if (ec) break;
    }
    std::ranges::sort(out);
    return out;
}

std::vector<std::string> GlobReportFiles(std::string_view dir, std::string_view pattern) {
    std::vector<std::string> out;
    if (dir.empty() || pattern.empty()) return out;

    const std::string pattern_string(pattern);
    const RE2 regex(pattern_string);
    if (!regex.ok()) return out;  // never-throw：坏模式 = 什么都没匹配到

    std::error_code ec;
    fs::directory_iterator iterator(fs::path(WidePath(dir)), ec);
    if (ec) return out;
    const fs::directory_iterator end;
    while (iterator != end) {
        const fs::path& entry_path = iterator->path();
        std::error_code status_ec;
        if (fs::is_regular_file(entry_path, status_ec) && !status_ec) {
            const std::string name = Utf8FromPath(entry_path.filename());
            if (RE2::FullMatch(name, regex)) {
                out.push_back(JoinPath(dir, name));
            }
        }
        iterator.increment(ec);
        if (ec) break;
    }
    std::ranges::sort(out);
    return out;
}

std::string FirstExisting(const std::vector<std::string>& candidates) {
    for (const std::string& candidate : candidates) {
        if (!candidate.empty() && IsFile(candidate)) return candidate;
    }
    return {};
}

std::string RunsDirectory(std::string_view project_dir, std::string_view project_name) {
    std::string base = JoinPath(project_dir, std::string(project_name) + "_Runs");
    if (IsDirectory(base)) return base;

    // 工程名与目录名可能对不上（改了 <Name> 之后旧 runs 目录还叫老名字）：
    // 退而取第一个 "*_Runs" 目录，与 Python 一致。
    for (const std::string& entry : ListSubdirectories(project_dir)) {
        if (EndsWith(entry, "_Runs")) return JoinPath(project_dir, entry);
    }
    return base;
}

ArtifactsReport CollectArtifacts(const project::Project& project) {
    ArtifactsReport report;
    report.runs_dir = RunsDirectory(project.dir, project.name);
    if (!IsDirectory(report.runs_dir)) {
        report.runs_found = false;
        return report;
    }
    report.runs_found = true;

    for (const std::string& name : ListSubdirectories(report.runs_dir)) {
        if (name == "best_result") continue;  // Python 把 best_result 从 runs 列表里排除
        RunEntry entry;
        entry.name = name;
        entry.dir = JoinPath(report.runs_dir, name);
        entry.exists = true;
        report.runs.push_back(std::move(entry));
    }

    const std::string syn = JoinPath(report.runs_dir, "syn_1");
    const std::string phy = JoinPath(report.runs_dir, "phy_1");
    const std::string best = JoinPath(report.runs_dir, "best_result");

    std::vector<std::string> bit_candidates = GlobReportFiles(best, R"(.*\.bit)");
    for (const std::string& path : GlobReportFiles(phy, R"(.*\.bit)")) {
        bit_candidates.push_back(path);
    }
    report.bit = StatFile(FirstExisting(bit_candidates));

    std::vector<std::string> route_candidates = GlobReportFiles(phy, R"(.*_pr\.db)");
    for (const std::string& path : GlobReportFiles(syn, R"(.*_gate\.db)")) {
        route_candidates.push_back(path);
    }
    report.route_db = StatFile(FirstExisting(route_candidates));

    // 协议 §2 要求 artifacts.syn_db；Python 版从未填过这个字段（见 PYTHON-BUGS P-9）
    report.syn_db = StatFile(FirstExisting(GlobReportFiles(syn, R"(.*_gate\.db)")));
    return report;
}

}  // namespace fpgalsp::reports