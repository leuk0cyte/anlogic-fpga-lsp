// =============================================================================
//  src/td/td_diagnostics.cpp
// =============================================================================

#include "td/td_diagnostics.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

#include "platform/path.h"
#include "td/td_device.h"
#include "td/td_session.h"

namespace fpgalsp::td {
namespace {

namespace plat = fpgalsp::platform;

std::string ToLower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// 反斜杠 -> 正斜杠
std::string ToForwardSlashes(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c == '\\') c = '/';
    }
    return out;
}

bool IsAbsoluteDrivePath(std::string_view path) {
    // 对应 Python: re.match(r"^[A-Za-z]:/", clean)
    return path.size() >= 3 && std::isalpha(static_cast<unsigned char>(path[0])) &&
           path[1] == ':' && path[2] == '/';
}

bool FileExists(std::string_view path) {
    if (path.empty()) return false;
    try {
        return std::filesystem::is_regular_file(std::filesystem::path{std::string(path)});
    } catch (...) {
        return false;
    }
}

}  // namespace

// -----------------------------------------------------------------------------
// 路径解析
// -----------------------------------------------------------------------------

std::string ResolveTdFile(const std::optional<std::string>& path,
                          const project::Project& project) {
    // 对应 Python: if not path: return project.al_path
    if (!path.has_value() || path->empty()) {
        return project.al_path;
    }

    const std::string clean = ToForwardSlashes(*path);

    // 绝对路径直接返回
    if (IsAbsoluteDrivePath(clean)) {
        return clean;
    }

    // 相对路径：依次试 project.dir / declared_path，命中 isfile 者优先
    // 对应 Python:
    //   for base in (project.dir, project.declared_path):
    //       candidate = normpath(join(base, clean))
    //       if isfile(candidate): return candidate
    const std::string bases[2] = {project.dir, project.declared_path};
    for (const auto& base : bases) {
        if (base.empty()) continue;
        const std::string candidate = plat::normalize_path(plat::path_join(base, clean));
        if (FileExists(candidate)) {
            return candidate;
        }
    }

    // 都不存在：回落到 project.dir 拼接结果
    // 对应 Python: return normpath(join(project.dir, clean))
    if (!project.dir.empty()) {
        return plat::normalize_path(plat::path_join(project.dir, clean));
    }

    // project.dir 也没有：原样返回
    return clean;
}

// -----------------------------------------------------------------------------
// 单条消息映射
// -----------------------------------------------------------------------------

lsp::Diagnostic MapMessage(const TdMessage& message, const project::Project& project,
                           std::string_view backend) {
    // 行号：TD 1-based -> LSP 0-based
    // 对应 Python: line = (item["line"] - 1) if item["line"] else 0
    int line = 0;
    if (message.line.has_value() && *message.line != 0) {
        line = *message.line - 1;
        if (line < 0) line = 0;  // line=0 的畸形输入钳到 0，不产生负行号
    }

    std::string resolved_path = ResolveTdFile(message.path, project);

    std::string text = message.message;
    int severity = message.severity;

    // black box 升级为 warning
    // 对应 Python:
    //   if "black box" in message.lower() and "TD ignored this file" not in message:
    //       severity = 2
    //       message += " (IP/module not in the build; load the IP first in project mode)"
    const std::string lower = ToLower(text);
    if (lower.find("black box") != std::string::npos &&
        text.find("TD ignored this file") == std::string::npos) {
        severity = lsp::kWarning;
        text += " (IP/module not in the build; load the IP first in project mode)";
    }

    // HDL-8007 且无行号 -> 整个 .al 文件级
    // 对应 Python:
    //   if item["code"] == "HDL-8007" and item["line"] is None: path = project.al_path
    if (message.code == "HDL-8007" && !message.line.has_value()) {
        resolved_path = project.al_path;
    }

    // 构造 Diagnostic：character 恒为 0，range 宽度 1 列（对应 _diag 的 char+1）
    lsp::Diagnostic diag = lsp::Diagnostic::AtLine(line, 0, 1, severity, message.code,
                                                   "fpga-lsp:td", text);
    diag.WithBackend(backend);
    diag.WithData("path", lsp::Json(resolved_path));
    return diag;
}

lsp::DiagnosticList MapMessages(const std::vector<TdMessage>& messages,
                                const project::Project& project, std::string_view backend) {
    lsp::DiagnosticList out;
    out.reserve(messages.size());
    for (const auto& msg : messages) {
        out.push_back(MapMessage(msg, project, backend));
    }
    return out;
}

// -----------------------------------------------------------------------------
// LSP-PATH-SPACE
// -----------------------------------------------------------------------------

std::vector<const project::AlFile*> SkippedSpaceHdlFiles(const project::Project& project) {
    // 对应 Python: skipped = [f for f in project.hdl_files() if " " in f.path]
    std::vector<const project::AlFile*> out;
    for (const project::AlFile* f : project.HdlFiles()) {
        if (f->path.find(' ') != std::string::npos) {
            out.push_back(f);
        }
    }
    return out;
}

lsp::DiagnosticList MakePathSpaceDiagnostics(const project::Project& project) {
    lsp::DiagnosticList out;
    for (const project::AlFile* f : SkippedSpaceHdlFiles(project)) {
        // 对应 Python _diag(0, "...", "LSP-PATH-SPACE", 2, "fpga-lsp:td", path=project.al_path)
        const std::string message =
            "File name contains spaces and cannot be passed to TD's -file argument "
            "(Tcl splits it): " + f->rel;

        lsp::Diagnostic diag =
            lsp::Diagnostic::AtLine(0, 0, 1, lsp::kWarning, "LSP-PATH-SPACE",
                                    "fpga-lsp:td", message);
        diag.WithData("path", lsp::Json(project.al_path));
        out.push_back(std::move(diag));
    }
    return out;
}

// -----------------------------------------------------------------------------
// Tcl 命令行
// -----------------------------------------------------------------------------

std::string FileListTcl(const std::vector<std::string>& paths) {
    // 对应 Python _file_list_tcl：含空格的跳过（TD 会在空白处切分 -file 列表）
    std::string out;
    for (const auto& path : paths) {
        if (path.find(' ') != std::string::npos) continue;
        if (!out.empty()) out += ' ';
        out += ToForwardSlashes(path);
    }
    return out;
}

std::vector<std::string> IpGeneratedHdl(const project::Project& project) {
    // 对应 Python ip_generated_hdl
    std::vector<std::string> out;
    for (const project::AlFile* ip : project.IpFiles()) {
        const std::string stem = [&] {
            const std::size_t dot = ip->path.find_last_of('.');
            const std::size_t slash = ip->path.find_last_of('/');
            if (dot == std::string::npos) return ip->path;
            if (slash != std::string::npos && dot < slash) return ip->path;
            return ip->path.substr(0, dot);
        }();

        for (const char* ext : {".v", ".sv", ".vhd", ".vhdl"}) {
            const std::string candidate = stem + ext;
            if (FileExists(candidate) && candidate.find(' ') == std::string::npos) {
                out.push_back(candidate);
            }
        }
    }
    return out;
}

std::string MakeReadHdlCommand(const std::vector<std::string>& files,
                               std::string_view top) {
    // 对应 Python: "read_hdl -file {%s} -top %s" % (_file_list_tcl(files), project.top or "top")
    const std::string top_name = top.empty() ? std::string("top") : std::string(top);
    return "read_hdl -file {" + FileListTcl(files) + "} -top " + top_name;
}

// -----------------------------------------------------------------------------
// 从输出文本产出诊断（离线可测）
// -----------------------------------------------------------------------------

lsp::DiagnosticList DiagnosticsFromReadHdlOutput(std::string_view output,
                                                 const project::Project& project) {
    lsp::DiagnosticList diags;

    // 1) 消息映射
    const auto messages = ParseMessageLines(output);
    for (const auto& msg : messages) {
        diags.push_back(MapMessage(msg, project, "read_hdl"));
    }

    // 2) 文件名含空格的主动提示
    for (auto& d : MakePathSpaceDiagnostics(project)) {
        diags.push_back(std::move(d));
    }

    return diags;
}

// -----------------------------------------------------------------------------
// ensure_device
// -----------------------------------------------------------------------------

EnsureDeviceResult EnsureDevice(const project::Project& project, TdSession& session,
                                std::string_view td_root,
                                std::string_view explicit_db) {
    EnsureDeviceResult result;

    const std::string package = project.device;
    if (package.empty()) {
        result.error = "project has no device";
        return result;
    }

    const std::string speed_suffix = SpeedArgSuffix(project.speed);

    // 显式给出 db：直接用它（对应 Python：不校验结果，信任调用方）
    if (!explicit_db.empty()) {
        try {
            session.Run("import_device " + std::string(explicit_db) + " -package " + package +
                            speed_suffix,
                        90.0);
            result.ok = true;
            result.db = std::string(explicit_db);
            result.tried = 1;
            return result;
        } catch (const std::exception& exc) {
            result.error = std::string("import_device threw: ") + exc.what();
            return result;
        }
    }

    // 按降级顺序逐个试
    std::vector<std::string> candidates;
    try {
        candidates = DbCandidates(td_root, project.family);
    } catch (...) {
        candidates.clear();
    }

    for (const auto& candidate : candidates) {
        ++result.tried;
        try {
            const std::string out = session.Run(
                MakeImportDeviceCommand(candidate, package, project.speed), 90.0);
            if (!HasError(out)) {
                result.ok = true;
                result.db = candidate;
                return result;
            }
        } catch (const std::exception& exc) {
            // 单个候选失败不应中断整体流程，记录原因继续
            result.error = std::string(exc.what());
            break;
        }
    }

    if (result.error.empty()) {
        result.error = "No device library supports " + package + " (tried " +
                       std::to_string(result.tried) + " candidates)";
    }
    return result;
}

// -----------------------------------------------------------------------------
// RunReadHdl
// -----------------------------------------------------------------------------

lsp::DiagnosticList RunReadHdl(const project::Project& project, TdSession& session,
                               std::string_view explicit_db) {
    lsp::DiagnosticList diags;

    // 工程里没有 HDL -> 直接返回空（对应 Python: if not files: return []）
    const auto hdl = project.HdlFiles();
    if (hdl.empty()) {
        return diags;
    }

    std::vector<std::string> files;
    files.reserve(hdl.size());
    for (const auto* f : hdl) files.push_back(f->path);
    for (auto& extra : IpGeneratedHdl(project)) files.push_back(std::move(extra));

    // import_device 必须在 read_hdl 之前（也必须在 open_project 之前，
    // 否则 TD 的 run 会以 PRJ-8001 死掉）
    const std::string td_root = [&] {
        try {
            const std::string exe = session.td().exe;
            const std::size_t bin = exe.rfind("/bin/");
            return bin == std::string::npos ? std::string() : exe.substr(0, bin);
        } catch (...) {
            return std::string();
        }
    }();

    const EnsureDeviceResult device = EnsureDevice(project, session, td_root, explicit_db);
    if (!device.ok) {
        // never-raise：import 失败产出一条诊断而不是抛异常
        lsp::Diagnostic diag = lsp::Diagnostic::AtLine(
            0, 0, 1, lsp::kError, "TD-DEVICE-IMPORT", "fpga-lsp:td",
            "Unable to import_device for " + project.device + ": " + device.error);
        diag.WithData("path", lsp::Json(project.al_path));
        diags.push_back(std::move(diag));
        return diags;
    }

    // 执行 read_hdl（⚠ 本会话之后不可再用）
    std::string output;
    try {
        output = session.Run(MakeReadHdlCommand(files, project.top), 300.0);
    } catch (const std::exception& exc) {
        lsp::Diagnostic diag = lsp::Diagnostic::AtLine(
            0, 0, 1, lsp::kError, "TD-READ-HDL-FAILED", "fpga-lsp:td",
            std::string("read_hdl failed: ") + exc.what());
        diag.WithData("path", lsp::Json(project.al_path));
        diags.push_back(std::move(diag));
        return diags;
    }

    return DiagnosticsFromReadHdlOutput(output, project);
}

}  // namespace fpgalsp::td