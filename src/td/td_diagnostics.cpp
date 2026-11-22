// =============================================================================
//  src/td/td_diagnostics.cpp
// =============================================================================

#include "td/td_diagnostics.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include "platform/encoding.h"
#include "platform/log.h"
#include "platform/never_throw.h"
#include "platform/path.h"
#include "platform/write_service.h"
#include "td/td_device.h"
#include "td/td_session.h"
#include "td/tcl_quote.h"

#include "platform/text.h"
#include <array>
namespace fpgalsp::td {
namespace {

namespace plat = fpgalsp::platform;

using platform::ToLower;

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
    const std::array<std::string, 2> bases = {project.dir, project.declared_path};
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
    if (lower.contains("black box") &&
        !text.contains("TD ignored this file")) {
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
        if (f->path.contains(' ')) {
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
        if (path.contains(' ')) continue;
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
            if (FileExists(candidate) && !candidate.contains(' ')) {
                out.push_back(candidate);
            }
        }
    }
    return out;
}

std::string MakeReadHdlCommand(const std::vector<std::string>& files,
                               std::string_view top) {
    // 对应 Python: "read_hdl -file {%s} -top %s" % (_file_list_tcl(files), project.top or "top")
    // ⚠ B7 / S-01：`-file` 的列表与 `-top` 都必须是一个**完整 Tcl 单词**。
    //   旧实现把 project.top 裸拼进来，一个 `<MODULE>top; exec calc</MODULE>`
    //   就是一条任意命令（保存文件即自动触发）。现在：
    //     * 列表整体 TclQuote -> 普通路径下输出仍是 `{a.v b.v}`（行为不变）
    //     * top 走 TclQuote；调用方（RunReadHdl）另外还会拒绝非标识符
    const std::string top_name = top.empty() ? std::string("top") : std::string(top);
    return "read_hdl -file " + TclQuoteGrouped(FileListTcl(files)) + " -top " +
           TclQuote(top_name);
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
//  W4-B：import_device 成功候选缓存
// -----------------------------------------------------------------------------
//  实测（真实工程副本，EF2L45LG144B）：一轮会按 DbCandidates 顺序试 3 个候选，
//  ef2_1 / ef2_2 各失败一次（~15 ms/次），第 3 个 ef2_4 才成功（~230 ms）。
//  失败候选是纯白工；family 匹配不上时（家族为空/未知）候选会退化成"所有 db 按
//  字典序"，白工可达上百毫秒。
//
//  方案：把"上次成功的 db 名"按 (td_root, TD exe mtime, device, family, speed)
//  记到状态目录；下一轮**先试它**，成功直接用；失败则删掉该条并回退全量试错。
//    * 失效：key 含 td_root + exe mtime（换安装/升级自动 miss）、device/family/speed；
//      缓存文件缺失/损坏、条目对应的 db 文件不存在 -> 静默全量回退；
//    * 写盘失败只记 debug 日志：缓存是纯优化，不能变成新的失败点。
namespace {

constexpr std::size_t kDeviceCacheMaxEntries = 16;

std::string DeviceCachePath(const project::Project& project) {
    if (project.dir.empty()) return {};
    return platform::path_join(platform::state_dir(project.dir), "cache/td_device_db.json");
}

// key 里带 td_root + exe 的 mtime：TD 升级 / 换安装目录后旧条目自动失效。
std::string DeviceCacheKey(const project::Project& project, const td::TdChoice& td) {
    std::int64_t mtime = 0;
    std::error_code ec;
    const auto write_time =
        std::filesystem::last_write_time(std::filesystem::path(td.exe), ec);
    if (!ec) mtime = static_cast<std::int64_t>(write_time.time_since_epoch().count());
    std::string key = td.root;
    key += '|';
    key += std::to_string(mtime);
    key += '|';
    key += project.device;
    key += '|';
    key += project.family;
    key += '|';
    key += project.speed.has_value() ? *project.speed : std::string("-");
    return key;
}

std::optional<std::string> ReadDeviceCache(const std::string& path, const std::string& key) {
    if (path.empty()) return std::nullopt;
    const std::optional<std::string> text = platform::try_read_file_utf8(path);
    if (!text.has_value()) return std::nullopt;
    try {
        const nlohmann::json parsed = nlohmann::json::parse(*text);
        if (!parsed.is_object()) return std::nullopt;
        const auto it = parsed.find(key);
        if (it == parsed.end() || !it->is_object()) return std::nullopt;
        const auto db = it->find("db");
        if (db == it->end() || !db->is_string()) return std::nullopt;
        return db->get<std::string>();
    } catch (...) {
        return std::nullopt;  // 损坏 = 未命中
    }
}

void UpdateDeviceCache(const project::Project& project, const std::string& path,
                       const std::string& key, const std::optional<std::string>& db) {
    if (path.empty()) return;
    // A-4：写缓存是尽力而为，但**失败必须留一条日志**。以前整个函数被空 catch
    // 包住，磁盘满/权限拒绝时一次都不记，用户只看到"下一轮设备探测又变慢"。
    // NeverThrow 保留"既不外抛也不中断探测"的契约，并让被吞的异常进 debug 日志。
    platform::NeverThrow(
        [&] {
            nlohmann::json root = nlohmann::json::object();
            if (const std::optional<std::string> text = platform::try_read_file_utf8(path);
                text.has_value()) {
                try {
                    const nlohmann::json parsed = nlohmann::json::parse(*text);
                    if (parsed.is_object()) root = parsed;
                } catch (...) {
                    root = nlohmann::json::object();  // 缓存损坏：当空表重建
                }
            }
            if (db.has_value()) {
                root[key] = nlohmann::json{{"db", *db}};
                if (root.size() > kDeviceCacheMaxEntries) {
                    root = nlohmann::json::object();
                    root[key] = nlohmann::json{{"db", *db}};
                }
            } else {
                root.erase(key);
                if (root.empty()) return;  // 不留空文件
            }
            // 统一写服务：状态目录 + 原子写 + 审计（2026-11-22 写服务改造）。
            platform::IgnoreResult(platform::writes::Write(
                project.dir, path, root.dump(),
                platform::writes::Options{.caller = "td.device_cache"}));
        },
        "td.device_cache");
}

bool DbFileExists(std::string_view td_root, const std::string& db) {
    std::error_code ec;
    return std::filesystem::exists(
        std::filesystem::path(platform::path_join(td_root, "arch/" + db)), ec);
}

}  // namespace

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
    // ⚠ B7 / S-01：器件名来自 .al 的 <HardWare><Device>，是唯一的"包名"来源。
    //   合法形态（EF2L45LG144B）必须过白名单；非法值直接安全失败（报错，不拼命令）。
    //   注意这里**不做 quote 兜底**：非法器件名一定是工程坏了，让用户看到错误，
    //   而不是让 TD 去猜一个带引号的怪名字。
    if (!IsTclToken(package)) {
        result.error = "project device name contains characters that cannot be passed to TD: \"" +
                       package + "\"";
        return result;
    }

    // 速度后缀由 MakeImportDeviceCommand 内部生成（两条路径共用同一份拼装逻辑）

    // 显式给出 db：直接用它（对应 Python：不校验结果，信任调用方）
    if (!explicit_db.empty()) {
        try {
            // B7 / S-01：与下面的候选循环共用同一个构造函数（曾经是手拼的重复逻辑，
            // 手拼那份没有引号，是 S-01 的注入点之一）。
            session.Run(MakeImportDeviceCommand(explicit_db, package, project.speed), 90.0);
            result.ok = true;
            result.db = std::string(explicit_db);
            result.tried = 1;
            return result;
        } catch (const std::exception& exc) {
            result.error = std::string("import_device threw: ") + exc.what();
            return result;
        }
    }

    // W4-B：先试"上次成功的 db"（命中时省掉前面所有失败候选的 import）。
    std::string cache_path;
    std::string cache_key;
    const bool cache_enabled = !project.dir.empty();
    if (cache_enabled) {
        cache_key = DeviceCacheKey(project, session.td());
        cache_path = DeviceCachePath(project);
        const std::optional<std::string> cached = ReadDeviceCache(cache_path, cache_key);
        if (cached.has_value() && DbFileExists(td_root, *cached)) {
            ++result.tried;
            try {
                const std::string out = session.Run(
                    MakeImportDeviceCommand(*cached, package, project.speed), 90.0);
                if (!HasError(out)) {
                    result.ok = true;
                    result.db = *cached;
                    result.from_cache = true;
                    platform::default_logger().debug("import_device: cache hit " + *cached);
                    return result;
                }
            } catch (const std::exception& exc) {
                result.error = std::string(exc.what());
                return result;
            }
            // 缓存条目被 TD 拒绝（升级 / db 不再支持该器件）：删掉它，回退全量试错。
            UpdateDeviceCache(project, cache_path, cache_key, std::nullopt);
            platform::default_logger().debug("import_device: cached db rejected: " + *cached);
        }
    }

    // 按降级顺序逐个试
    std::vector<std::string> candidates;
    try {
        candidates = DbCandidates(td_root, project.family, project.device);
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
                // 记住成功候选（best-effort；写失败不影响本轮结果）
                if (cache_enabled) UpdateDeviceCache(project, cache_path, cache_key, result.db);
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
// A5 / task-25：按文件分组 + 变更对比
// -----------------------------------------------------------------------------

namespace {

// 指纹：只取"用户能看到"的字段。data 里有 path/backend 之类的装饰信息，参与比较
// 会让同一批诊断每轮都被判成"变了"，失去"只发布变更"的意义。
std::string DiagnosticFingerprint(const lsp::Diagnostic& d) {
    std::string out;
    out.reserve(64);
    out += std::to_string(d.range.start.line);
    out += ':';
    out += std::to_string(d.range.start.character);
    out += ':';
    out += std::to_string(d.range.end.line);
    out += ':';
    out += std::to_string(d.range.end.character);
    out += ':';
    out += std::to_string(d.severity);
    out += ':';
    out += d.code;
    out += ':';
    out += d.message;
    return out;
}

std::vector<std::string> FileFingerprints(const lsp::DiagnosticList& list) {
    std::vector<std::string> out;
    out.reserve(list.size());
    for (const lsp::Diagnostic& d : list) out.push_back(DiagnosticFingerprint(d));
    std::ranges::sort(out);
    return out;
}

// 把一批诊断按文件分表，同时填扁平列表
void GroupByFile(const project::Project& project, lsp::DiagnosticList* diagnostics,
                 TdCheckResult* result) {
    for (lsp::Diagnostic& d : *diagnostics) {
        const std::string key = DiagnosticFileKey(d, project.al_path);
        result->by_file[key].push_back(d);
    }
    result->all = std::move(*diagnostics);
}

}  // namespace

std::string DiagnosticFileKey(const lsp::Diagnostic& diagnostic,
                              std::string_view fallback_path) {
    if (diagnostic.data.is_object()) {
        const auto it = diagnostic.data.find("path");
        if (it != diagnostic.data.end() && it->is_string()) {
            const std::string path = it->get<std::string>();
            if (!path.empty()) return platform::normalize_path(path);
        }
    }
    return std::string(fallback_path);
}

std::vector<std::string> DiffChangedFiles(
    const std::map<std::string, lsp::DiagnosticList>& before,
    const std::map<std::string, lsp::DiagnosticList>& after) {
    std::vector<std::string> changed;
    auto it_before = before.begin();
    auto it_after = after.begin();
    while (it_before != before.end() || it_after != after.end()) {
        if (it_after == after.end() ||
            (it_before != before.end() && it_before->first < it_after->first)) {
            changed.push_back(it_before->first);  // 这个文件的诊断被清空了
            ++it_before;
            continue;
        }
        if (it_before == before.end() || it_after->first < it_before->first) {
            changed.push_back(it_after->first);  // 新出现的文件
            ++it_after;
            continue;
        }
        // 同一个文件：比指纹集合
        if (FileFingerprints(it_before->second) != FileFingerprints(it_after->second)) {
            changed.push_back(it_after->first);
        }
        ++it_before;
        ++it_after;
    }
    std::ranges::sort(changed);
    changed.erase(std::ranges::unique(changed).begin(), changed.end());
    return changed;
}

// -----------------------------------------------------------------------------
// RunReadHdlCheck / RunReadHdl
// -----------------------------------------------------------------------------

TdCheckResult RunReadHdlCheck(const project::Project& project, TdSession& session,
                              std::string_view explicit_db, const CancelToken* cancel) {
    TdCheckResult result;
    // ⚠ 两个秒表：total 用自己的（**从不 Restart**），watch 只在各阶段之间打点。
    //   早期版本用同一个 watch 的 ElapsedMs() 当 total，而它每次 LapMs() 都会重启，
    //   结果 total 只等于"最后一段"，比各段之和小一个数量级（A5 实测踩过）。
    PhaseStopwatch total_watch;
    PhaseStopwatch watch;

    const auto finish = [&]() -> TdCheckResult& {
        result.timings.total_ms = total_watch.ElapsedMs();
        return result;
    };
    const auto is_cancelled = [cancel] { return cancel != nullptr && cancel->cancelled(); };

    // 工程里没有 HDL -> 直接返回空（对应 Python: if not files: return []）
    const auto hdl = project.HdlFiles();
    if (hdl.empty()) {
        return finish();
    }

    // ⚠ B7 / S-01：<TOP_MODULE><MODULE> 来自 .al，是注入 TD Tcl 的主要入口
    //   （`top; exec calc` 完全合法地出现在 .al 里）。它不是"路径"，没有理由
    //   包含 Tcl 元字符 —— 非法即安全失败：给一条诊断并**不发送任何命令**，
    //   而不是把值 quote 之后让 TD 报一个莫名其妙的 unknown module。
    const std::string effective_top = project.top.empty() ? std::string("top") : project.top;
    if (!IsTclIdentifier(effective_top)) {
        lsp::Diagnostic diag = lsp::Diagnostic::AtLine(
            0, 0, 1, lsp::kError, "LSP-TCL-IDENT", "fpga-lsp:td",
            "Top module name \"" + effective_top +
                "\" contains characters that cannot be passed to TD; TD check skipped");
        diag.WithData("path", lsp::Json(project.al_path));
        lsp::DiagnosticList diags;
        diags.push_back(std::move(diag));
        GroupByFile(project, &diags, &result);
        result.failed = true;
        result.error = "invalid top module name";
        return finish();
    }

    if (is_cancelled()) {
        result.cancelled = true;
        return finish();
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

    watch.Restart();
    const EnsureDeviceResult device = EnsureDevice(project, session, td_root, explicit_db);
    result.timings.import_ms = watch.LapMs();
    result.timings.db_candidates = device.tried;

    // import_device 期间可能被杀（取消）：先看重置位，避免把 kill 造成的失败
    // 当成"器件库不支持"发布出去。
    if (is_cancelled()) {
        result.cancelled = true;
        return finish();
    }

    if (!device.ok) {
        // never-raise：import 失败产出一条诊断而不是抛异常
        lsp::Diagnostic diag = lsp::Diagnostic::AtLine(
            0, 0, 1, lsp::kError, "TD-DEVICE-IMPORT", "fpga-lsp:td",
            "Unable to import_device for " + project.device + ": " + device.error);
        diag.WithData("path", lsp::Json(project.al_path));
        lsp::DiagnosticList diags;
        diags.push_back(std::move(diag));
        GroupByFile(project, &diags, &result);
        result.failed = true;
        result.error = device.error;
        return finish();
    }

    // 执行 read_hdl（⚠ 本会话之后不可再用）。阻塞点：只有 kill hook 能中断它。
    std::string output;
    watch.Restart();
    try {
        output = session.Run(MakeReadHdlCommand(files, project.top), 300.0);
    } catch (const std::exception& exc) {
        result.timings.read_hdl_ms = watch.LapMs();
        if (is_cancelled()) {
            // 是被我们自己杀掉的：半截输出没有意义，不产生任何诊断
            result.cancelled = true;
            return finish();
        }
        lsp::Diagnostic diag = lsp::Diagnostic::AtLine(
            0, 0, 1, lsp::kError, "TD-READ-HDL-FAILED", "fpga-lsp:td",
            std::string("read_hdl failed: ") + exc.what());
        diag.WithData("path", lsp::Json(project.al_path));
        lsp::DiagnosticList diags;
        diags.push_back(std::move(diag));
        GroupByFile(project, &diags, &result);
        result.failed = true;
        result.error = exc.what();
        return finish();
    }
    result.timings.read_hdl_ms = watch.LapMs();

    if (is_cancelled()) {
        result.cancelled = true;
        return finish();
    }

    watch.Restart();
    lsp::DiagnosticList diags = DiagnosticsFromReadHdlOutput(output, project);
    result.timings.parse_ms = watch.LapMs();

    GroupByFile(project, &diags, &result);
    return finish();
}

lsp::DiagnosticList RunReadHdl(const project::Project& project, TdSession& session,
                               std::string_view explicit_db) {
    // 保留旧接口：签名与语义不变（失败时返回一条 TD-* 诊断，不抛）。
    return RunReadHdlCheck(project, session, explicit_db, nullptr).all;
}

}  // namespace fpgalsp::td