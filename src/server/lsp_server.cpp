// =============================================================================
//  src/server/lsp_server.cpp —— LSP 服务主体（生命周期 / 诊断 / TD 会话）
// =============================================================================
//  对应参考实现：Y:\fpga-lsp\fpgalsp\server.py
//    on_initialize / didOpen / didChange / didSave / didClose
//    _publish_merged / _clear_td_path / _schedule_td_check / _td_check_worker
//
//  ⚠ 与 Python 版的一处**结构性**差异（行为契约不变）：
//    Python 的 TD 检查跑在 daemon 线程里，进程退出时靠 OS 强杀；C++ 版用可 join 的
//    线程 + Job Object（见 td_session.h）。exit 时先 Kill 会话再 join，保证
//    **0 个残留 td_commands_prompt.exe**（P-5）。
// =============================================================================

#include "server/lsp_server.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include "platform/log.h"
#include "platform/path.h"
#include "flow/flow_runner.h"
#include "hdl/hdl_index.h"
#include "td/td_diagnostics.h"
#include "td/td_install.h"
#include "td/td_session.h"

namespace fpgalsp::server {
namespace {

using clock_type = std::chrono::steady_clock;

// 读环境变量（对应 Python os.environ.get）。
// 直接 std::getenv 在 MSVC 下会报 C4996，所以 Windows 上走 _dupenv_s。
std::string ReadEnv(std::string_view name) {
#ifdef _WIN32
    char* buffer = nullptr;
    std::size_t size = 0;
    const std::string key(name);
    if (_dupenv_s(&buffer, &size, key.c_str()) != 0 || buffer == nullptr) return {};
    std::string value(buffer);
    std::free(buffer);
    return value;
#else
    const char* value = std::getenv(std::string(name).c_str());
    return value == nullptr ? std::string() : std::string(value);
#endif
}

// 对应 Python 的 `os.path.join(project.dir, ".fpgalsp", "run")`；
// 没有工程目录时回落到临时目录（Python 用 ~/.fpgalsp/run）。
std::string RunDir(const project::Project& project) {
    if (!project.dir.empty()) {
        return platform::path_join(project.dir, ".fpgalsp/run");
    }
    std::error_code ec;
    const auto temp = std::filesystem::temp_directory_path(ec);
    const std::string base = ec ? std::string(".") : temp.generic_string();
    return platform::path_join(base, "fpgalsp/run");
}

// 对应 Python _dedupe：(code, message, start.line, start.character) 相同即视为同一条。
// 保留**首次出现**的那条（与 Python 一致），用 set 做键避免 O(n²)。
void DedupeInPlace(lsp::DiagnosticList& diagnostics) {
    std::set<std::tuple<std::string, std::string, int, int>> seen;
    lsp::DiagnosticList out;
    out.reserve(diagnostics.size());
    for (auto& diag : diagnostics) {
        auto key = std::make_tuple(diag.code, diag.message, diag.range.start.line,
                                   diag.range.start.character);
        if (!seen.insert(std::move(key)).second) continue;
        out.push_back(std::move(diag));
    }
    diagnostics = std::move(out);
}

// 对应 Python 的排序键 (severity, start.line, start.character)：
// error(1) 在前，同严重度按位置。**全量、不截断**。
void SortBySeverity(lsp::DiagnosticList& diagnostics) {
    std::stable_sort(diagnostics.begin(), diagnostics.end(),
                     [](const lsp::Diagnostic& a, const lsp::Diagnostic& b) {
                         if (a.severity != b.severity) return a.severity < b.severity;
                         if (a.range.start.line != b.range.start.line) {
                             return a.range.start.line < b.range.start.line;
                         }
                         return a.range.start.character < b.range.start.character;
                     });
}

}  // namespace

// 工程摘要（对应 project.py project_summary；字段逐个对齐）
Json ProjectSummaryOf(const project::Project& project) {
    Json out = Json::object();
    out["al"] = project.al_path;
    out["dir"] = project.dir;
    out["name"] = project.name;
    out["family"] = project.family;
    out["device"] = project.device;
    out["top"] = project.top;
    out["hdl"] = project.HdlFiles().size();
    // Python 这里是 len(warnings)，不是列表本身
    out["warnings"] = project.warnings.size();
    return out;
}

Json ProjectSummaryOf(std::string_view al_path) {
    return ProjectSummaryOf(project::ParseAl(al_path));
}

// =============================================================================
//  Settings
// =============================================================================

void Settings::Update(const Json& options) {
    if (!options.is_object()) return;

    // 对应：section = options.get("fpgaLsp") if "fpgaLsp" in options else options
    const Json* section = &options;
    if (const auto it = options.find("fpgaLsp"); it != options.end()) {
        section = &(*it);
    }
    if (!section->is_object()) return;

    td_check_on_save = jsonrpc::GetBool(*section, "tdCheckOnSave", td_check_on_save);
    enable_td = jsonrpc::GetBool(*section, "enableTd", enable_td);

    const std::string project_file_value = jsonrpc::GetString(*section, "projectFile", project_file);
    project_file = project_file_value;
    const std::string td_path_value = jsonrpc::GetString(*section, "tdPath", td_path);
    td_path = td_path_value;
    const std::string verible_value = jsonrpc::GetString(*section, "veriblePath", verible_path);
    verible_path = verible_value;

    if (const auto it = section->find("veribleArgs"); it != section->end() && it->is_array()) {
        verible_args.clear();
        for (const auto& item : *it) {
            if (item.is_string()) verible_args.push_back(item.get<std::string>());
        }
    }
    verible_format = jsonrpc::GetBool(*section, "veribleFormat", verible_format);
    format_fallback = jsonrpc::GetBool(*section, "formatFallback", format_fallback);
}

// =============================================================================
//  构造 / 析构 / 关闭
// =============================================================================

LspServer::LspServer(jsonrpc::TransportBaseCommon& transport, std::string td_explicit)
    : transport_(transport), td_explicit_(std::move(td_explicit)) {}

LspServer::~LspServer() { Shutdown(); }

void LspServer::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        shutting_down_ = true;
        td_pending_ = false;
    }

    // 1) 先停 flow：runner 归 server 所有，必须在 transport 还活着的时候取消并 join。
    //    否则可能出现"进程退出时 flow worker 还在写 transport"的悬垂写。
    //    限时等待：取消后 worker 应在秒级退出，超时就交给 ~FlowRunner 兜底。
    {
        std::shared_ptr<flow::FlowRunner> runner;
        {
            std::lock_guard<std::mutex> lock(flow_mutex_);
            runner.swap(flow_);
        }
        if (runner) {
            try {
                runner->Cancel();
                if (!runner->WaitUntilIdle(10.0)) {
                    platform::default_logger().warn("flow 未在 10s 内停止，继续关停");
                }
            } catch (const std::exception& exc) {
                platform::default_logger().error(std::string("flow shutdown: ") + exc.what());
            } catch (...) {
                platform::default_logger().error("flow shutdown: unknown exception");
            }
            runner.reset();  // 在 transport 仍存活时析构 runner
        }
    }

    // 2) 再杀 TD（Job Object 会带走整棵进程树），最后 join TD 线程 ——
    //    否则 Run() 可能一直等超时。
    KillSession();
    if (td_worker_.joinable()) {
        td_worker_.join();
    }
    platform::default_logger().flush();
}

// =============================================================================
//  注册
// =============================================================================

void LspServer::RegisterAll(jsonrpc::Dispatcher& dispatcher) {
    // ---- 标准 LSP：生命周期与文档同步 ----
    dispatcher.Register("initialize",
                        [this](const Json& params) { return HandleInitialize(params); });
    dispatcher.Register("initialized",
                        [this](const Json& params) { return HandleNoop(params); });
    dispatcher.Register("textDocument/didOpen",
                        [this](const Json& params) { return HandleDidOpen(params); });
    dispatcher.Register("textDocument/didChange",
                        [this](const Json& params) { return HandleDidChange(params); });
    dispatcher.Register("textDocument/didSave",
                        [this](const Json& params) { return HandleDidSave(params); });
    dispatcher.Register("textDocument/didClose",
                        [this](const Json& params) { return HandleDidClose(params); });
    dispatcher.Register("workspace/didChangeConfiguration", [this](const Json& params) {
        return HandleDidChangeConfiguration(params);
    });

    // ---- 标准 LSP：符号类能力（hdl_handlers.cpp）----
    dispatcher.Register("textDocument/completion",
                        [this](const Json& params) { return HandleCompletion(params); });
    dispatcher.Register("textDocument/hover",
                        [this](const Json& params) { return HandleHover(params); });
    dispatcher.Register("textDocument/definition",
                        [this](const Json& params) { return HandleDefinition(params); });
    dispatcher.Register("textDocument/documentSymbol",
                        [this](const Json& params) { return HandleDocumentSymbol(params); });
    dispatcher.Register("workspace/symbol",
                        [this](const Json& params) { return HandleWorkspaceSymbol(params); });
    dispatcher.Register("textDocument/references",
                        [this](const Json& params) { return HandleReferences(params); });
    dispatcher.Register("textDocument/inlayHint",
                        [this](const Json& params) { return HandleInlayHint(params); });

    // ---- 标准 LSP：已声明能力对应的空实现（绝不 -32601，见 COMMANDS.md）----
    dispatcher.Register("textDocument/codeAction",
                        [this](const Json& params) { return HandleEmptyList(params); });
    dispatcher.Register("textDocument/formatting",
                        [this](const Json& params) { return HandleEmptyList(params); });
    dispatcher.Register("textDocument/rangeFormatting",
                        [this](const Json& params) { return HandleEmptyList(params); });
    dispatcher.Register("textDocument/prepareRename",
                        [this](const Json& params) { return HandleNull(params); });
    dispatcher.Register("textDocument/rename",
                        [this](const Json& params) { return HandleNull(params); });
    dispatcher.Register("textDocument/prepareCallHierarchy",
                        [this](const Json& params) { return HandleEmptyList(params); });
    dispatcher.Register("callHierarchy/incomingCalls",
                        [this](const Json& params) { return HandleEmptyList(params); });
    dispatcher.Register("callHierarchy/outgoingCalls",
                        [this](const Json& params) { return HandleEmptyList(params); });
    dispatcher.Register("textDocument/semanticTokens/full", [this](const Json& params) {
        return HandleSemanticTokensFull(params);
    });
    dispatcher.Register("textDocument/foldingRange",
                        [this](const Json& params) { return HandleEmptyList(params); });

    // ---- 自定义 v2 方法（PROTOCOL-V2.md）----
    dispatcher.Register("fpgaLsp/moduleTree",
                        [this](const Json& params) { return HandleModuleTree(params); });
    dispatcher.Register("fpgaLsp/reports",
                        [this](const Json& params) { return HandleReports(params); });
    dispatcher.Register("fpgaLsp/flowStatus",
                        [this](const Json& params) { return HandleFlowStatus(params); });

    // ---- workspace/executeCommand ----
    dispatcher.Register("workspace/executeCommand",
                        [this](const Json& params) { return HandleExecuteCommand(params); });

    platform::default_logger().info("LSP handler 注册完成（" +
                                    std::to_string(dispatcher.Methods().size()) + " 个方法）");
}

// =============================================================================
//  initialize
// =============================================================================

Json LspServer::FeatureInitialize() { return FeatureInitialize(Json::object()); }

Json LspServer::FeatureInitialize(const Json& params) {
    // 能力声明（LSP initialize 与 MCP 工具的 server_info 共用同一份）
    // 对应 Python: root = params.get("rootUri") or params.get("rootPath") or ""
    std::string root = jsonrpc::GetString(params, "rootUri");
    if (root.empty()) root = jsonrpc::GetString(params, "rootPath");
    if (root.empty()) {
        const Json folders = jsonrpc::GetArray(params, "workspaceFolders");
        if (!folders.empty() && folders.front().is_object()) {
            root = jsonrpc::GetString(folders.front(), "uri");
        }
    }
    // ⚠ 空 root 不覆盖已有值：MCP 侧只是想拿能力声明，不该把 LSP 的工作区根清掉
    if (!root.empty()) root_uri_ = root;

    if (const auto it = params.find("initializationOptions"); it != params.end()) {
        settings_.Update(*it);
    }

    Json capabilities = Json::object();

    // textDocumentSync: {openClose, change:2(Incremental), save:{includeText:false}}
    Json sync = Json::object();
    sync["openClose"] = true;
    sync["change"] = 2;
    Json save = Json::object();
    save["includeText"] = false;
    sync["save"] = std::move(save);
    capabilities["textDocumentSync"] = std::move(sync);

    Json completion = Json::object();
    completion["triggerCharacters"] = Json::array({".", "("});
    capabilities["completionProvider"] = std::move(completion);

    capabilities["hoverProvider"] = true;
    capabilities["definitionProvider"] = true;
    capabilities["documentSymbolProvider"] = true;
    capabilities["workspaceSymbolProvider"] = true;
    capabilities["referencesProvider"] = true;

    Json code_action = Json::object();
    code_action["codeActionKinds"] = Json::array({"refactor", "quickfix"});
    capabilities["codeActionProvider"] = std::move(code_action);

    // Verible 由内嵌副本提供，所以 formatting 始终声明（PROTOCOL-V2 v5）
    capabilities["documentFormattingProvider"] = true;
    capabilities["documentRangeFormattingProvider"] = true;

    Json rename = Json::object();
    rename["prepareProvider"] = true;
    capabilities["renameProvider"] = std::move(rename);

    capabilities["callHierarchyProvider"] = true;

    Json legend = Json::object();
    legend["tokenTypes"] = Json::array({"keyword", "type",      "class",   "property",
                                        "variable", "parameter", "function", "number",
                                        "comment",  "string",    "operator", "macro"});
    legend["tokenModifiers"] = Json::array();
    Json semantic = Json::object();
    semantic["legend"] = std::move(legend);
    semantic["full"] = true;
    capabilities["semanticTokensProvider"] = std::move(semantic);

    capabilities["foldingRangeProvider"] = true;
    capabilities["inlayHintProvider"] = true;

    // LSP 3.17：本项目的位置换算全部按 UTF-16 码元（document_store / utf16 层）
    capabilities["positionEncoding"] = "utf-16";

    Json result = Json::object();
    result["capabilities"] = std::move(capabilities);
    Json info = Json::object();
    info["name"] = "fpga-lsp";
    info["version"] = "0.1.0";
    result["serverInfo"] = std::move(info);

    platform::default_logger().info("initialize root=" + root_uri_);
    return result;
}

HandlerResult LspServer::HandleInitialize(const Json& params) {
    return HandlerResult::Ok(FeatureInitialize(params));
}

HandlerResult LspServer::HandleNoop(const Json& params) {
    (void)params;
    return HandlerResult::Ok(Json());
}

HandlerResult LspServer::HandleNull(const Json& params) {
    (void)params;
    return HandlerResult::Ok(Json(nullptr));
}

HandlerResult LspServer::HandleEmptyList(const Json& params) {
    (void)params;
    return HandlerResult::Ok(Json::array());
}

HandlerResult LspServer::HandleSemanticTokensFull(const Json& params) {
    (void)params;
    Json result = Json::object();
    result["data"] = Json::array();
    return HandlerResult::Ok(std::move(result));
}

// =============================================================================
//  HDL 索引（src/hdl）
// =============================================================================

std::optional<std::string> LspServer::LoadText(const std::string& path) {
    // DocumentStore：已打开 -> 缓冲区内容；否则读盘（UTF-8，非法字节替换）
    return docs_.GetText(platform::path_to_uri(path));
}

std::string LspServer::ProjectSignature(const project::Project& project) {
    // 只需要"变了没变"，所以用 (路径, 版本或 mtime+size) 拼串即可，不做哈希。
    std::string signature = project.al_path;
    std::vector<std::string> files = hdl::VerilogFilesFor(project);
    for (const std::string& path : hdl::VhdlFilesFor(project)) files.push_back(path);

    for (const std::string& path : files) {
        signature += '\n';
        signature += path;
        signature += '|';
        if (const auto version = docs_.GetVersion(platform::path_to_uri(path));
            version.has_value()) {
            signature += 'v';
            signature += std::to_string(*version);
            continue;
        }
        std::error_code ec;
        const auto size = std::filesystem::file_size(std::filesystem::path(path), ec);
        if (ec) {
            signature += "missing";
            continue;
        }
        const auto write_time =
            std::filesystem::last_write_time(std::filesystem::path(path), ec);
        signature += std::to_string(size);
        signature += ':';
        signature += ec ? std::string("?")
                        : std::to_string(
                              static_cast<long long>(write_time.time_since_epoch().count()));
    }
    return signature;
}

std::shared_ptr<const hdl::HdlIndex> LspServer::IndexForProject(const project::Project& project,
                                                                bool* truncated,
                                                                std::size_t* parse_errors) {
    const std::string signature = ProjectSignature(project);
    {
        std::lock_guard<std::mutex> lock(hdl_mutex_);
        const auto it = hdl_cache_.find(project.al_path);
        if (it != hdl_cache_.end() && it->second.signature == signature) {
            if (truncated != nullptr) *truncated = it->second.truncated;
            if (parse_errors != nullptr) *parse_errors = it->second.parse_errors;
            return it->second.index;
        }
    }

    if (!hdl_parse_cache_) hdl_parse_cache_ = std::make_unique<hdl::ParseCache>();
    hdl::BuildOptions options;
    options.cache = hdl_parse_cache_.get();

    hdl::BuildResult built = hdl::BuildIndex(
        project, [this](const std::string& path) { return LoadText(path); }, options);

    auto index = std::make_shared<hdl::HdlIndex>(std::move(built.index));
    platform::default_logger().info("HDL 索引重建: " + project.al_path + " -> " +
                                    std::to_string(index->FileCount()) + " 文件 / " +
                                    std::to_string(index->ModuleCount()) + " 模块" +
                                    (built.truncated ? "（已截断）" : ""));

    {
        std::lock_guard<std::mutex> lock(hdl_mutex_);
        // 只保留少量工程（多工程工作区里也就是当前这一两个）
        if (hdl_cache_.size() >= 4) hdl_cache_.clear();
        HdlIndexEntry entry;
        entry.signature = signature;
        entry.index = index;
        entry.truncated = built.truncated;
        entry.parse_errors = built.parse_error_count;
        hdl_cache_[project.al_path] = std::move(entry);
    }

    if (truncated != nullptr) *truncated = built.truncated;
    if (parse_errors != nullptr) *parse_errors = built.parse_error_count;
    return index;
}

// =============================================================================
//  文档生命周期
// =============================================================================

HandlerResult LspServer::HandleDidOpen(const Json& params) {
    const Json document = jsonrpc::GetObject(params, "textDocument");
    const std::string uri = jsonrpc::GetString(document, "uri");
    if (uri.empty()) return HandlerResult::Ok(Json());

    const std::string path = platform::uri_to_path(uri);
    std::optional<int64_t> version;
    if (const auto it = document.find("version");
        it != document.end() && it->is_number_integer()) {
        version = it->get<int64_t>();
    }
    docs_.Open(uri, jsonrpc::GetString(document, "text"), version,
               jsonrpc::GetString(document, "languageId"), path);
    RunDiagnostics(path, false);
    return HandlerResult::Ok(Json());
}

HandlerResult LspServer::HandleDidChange(const Json& params) {
    const Json document = jsonrpc::GetObject(params, "textDocument");
    const std::string uri = jsonrpc::GetString(document, "uri");
    if (uri.empty()) return HandlerResult::Ok(Json());

    std::optional<int64_t> version;
    if (const auto it = document.find("version");
        it != document.end() && it->is_number_integer()) {
        version = it->get<int64_t>();
    }
    const auto changes = lsp::ParseContentChanges(jsonrpc::GetArray(params, "contentChanges"));
    docs_.Change(uri, changes, version);

    const lsp::DocumentSnapshot snapshot = docs_.Snapshot(uri);
    const std::string path = snapshot.valid() ? snapshot.path : platform::uri_to_path(uri);
    RunDiagnostics(path, false);
    return HandlerResult::Ok(Json());
}

HandlerResult LspServer::HandleDidSave(const Json& params) {
    const Json document = jsonrpc::GetObject(params, "textDocument");
    const std::string uri = jsonrpc::GetString(document, "uri");
    if (uri.empty()) return HandlerResult::Ok(Json());

    std::optional<std::string> text;
    if (const auto it = params.find("text"); it != params.end() && it->is_string()) {
        text = it->get<std::string>();
    }
    docs_.Save(uri, text);

    const lsp::DocumentSnapshot snapshot = docs_.Snapshot(uri);
    const std::string path = snapshot.valid() ? snapshot.path : platform::uri_to_path(uri);

    RunDiagnostics(path, true);

    // 0.10.28 的行为：保存时**立即**清空该文件的 TD 诊断，然后**只排队一次**重跑。
    //   * 先清 —— 否则"改好了但旧错误还挂着"，用户会以为没生效
    //   * 再排队 —— 若上一轮还在跑，只置一个 pending 标志（不叠加线程）
    if (settings_.td_check_on_save && settings_.enable_td) {
        if (auto project = ProjectForPath(path); project.has_value()) {
            ClearTdPath(*project, path);
            ScheduleTdCheck(*project);
        }
    }
    return HandlerResult::Ok(Json());
}

HandlerResult LspServer::HandleDidClose(const Json& params) {
    const std::string uri = jsonrpc::GetStringPath(params, {"textDocument", "uri"});
    if (uri.empty()) return HandlerResult::Ok(Json());
    const std::string path = platform::uri_to_path(uri);
    docs_.Close(uri);
    // 对应 Python didClose：把该文件的诊断清空（否则关掉文件后 Problems 还留着）
    PublishDiagnostics(path, lsp::DiagnosticList{});
    return HandlerResult::Ok(Json());
}

HandlerResult LspServer::HandleDidChangeConfiguration(const Json& params) {
    const std::string previous_project = settings_.project_file;
    settings_.Update(jsonrpc::GetObject(params, "settings"));
    if (settings_.project_file != previous_project) {
        InvalidateProjectCaches();
    }
    return HandlerResult::Ok(Json());
}

// =============================================================================
//  诊断发布
// =============================================================================

std::string DiagnosticPath(const lsp::Diagnostic& diagnostic, std::string_view fallback) {
    if (diagnostic.data.is_object()) {
        if (const auto it = diagnostic.data.find("path");
            it != diagnostic.data.end() && it->is_string()) {
            const std::string path = it->get<std::string>();
            if (!path.empty()) return platform::normalize_path(path);
        }
    }
    return std::string(fallback);
}

void LspServer::SendNotification(std::string_view method, Json params) {
    Json message = jsonrpc::MakeNotification(method, std::move(params));
    transport_.WriteMessage(message);  // 线程安全；写失败只会标记 closed，不抛
}

void LspServer::LogMessage(int type, std::string_view message) {
    Json params = Json::object();
    params["type"] = type;
    params["message"] = std::string(message);
    SendNotification("window/logMessage", std::move(params));
    platform::default_logger().info(std::string(message));
}

void LspServer::PublishDiagnostics(std::string_view path,
                                   const lsp::DiagnosticList& diagnostics) {
    const std::string uri = platform::path_to_uri(path);

    // 发布前统一 sanitize（对应 Python _sanitize），并摘掉内部用的 data["path"]，
    // 保证喂给编辑器的是纯 LSP 形状。**全量发布、不截断**。
    Json wire = Json::array();
    for (const auto& diag : diagnostics) {
        lsp::Diagnostic copy = lsp::Sanitize(diag, "fpga-lsp");
        if (copy.data.is_object()) copy.data.erase("path");
        wire.push_back(lsp::ToJson(copy));
    }

    Json params = Json::object();
    params["uri"] = uri;
    params["diagnostics"] = std::move(wire);
    if (const auto version = docs_.GetVersion(uri); version.has_value()) {
        params["version"] = *version;
    }
    SendNotification("textDocument/publishDiagnostics", std::move(params));
}

void LspServer::PublishMerged(const project::Project& project, const std::string* only_path) {
    std::map<std::string, lsp::DiagnosticList> table;
    std::vector<std::string> paths;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (const auto it = td_diags_.find(project.al_path); it != td_diags_.end()) {
            table = it->second;
        }
        std::set<std::string> all;
        for (const auto& [path, list] : table) all.insert(path);
        auto& previous = published_[project.al_path];
        if (only_path == nullptr) {
            // 全量发布时也要通知"这次已经没有诊断了"的文件（发空数组清屏）
            for (const auto& path : previous) all.insert(path);
        }
        previous = all;
        paths.assign(all.begin(), all.end());
    }

    if (only_path != nullptr) {
        if (table.find(*only_path) == table.end()) {
            PublishDiagnostics(*only_path, lsp::DiagnosticList{});
            return;
        }
        paths = {*only_path};
    }

    for (const auto& path : paths) {
        lsp::DiagnosticList diagnostics;
        if (const auto it = table.find(path); it != table.end()) {
            diagnostics = it->second;
        }
        DedupeInPlace(diagnostics);
        SortBySeverity(diagnostics);  // error 在前
        PublishDiagnostics(path, diagnostics);
    }
}

void LspServer::ClearTdPath(const project::Project& project, std::string_view path) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (auto it = td_diags_.find(project.al_path); it != td_diags_.end()) {
            if (it->second.erase(std::string(path)) > 0) changed = true;
        }
    }
    if (!changed) return;

    const std::string key(path);
    PublishMerged(project, &key);
}

void LspServer::RunDiagnostics(std::string_view path, bool publish_all) {
    if (auto project = ProjectForPath(path); project.has_value()) {
        if (publish_all) {
            PublishMerged(*project, nullptr);
        } else {
            const std::string only{path};
            PublishMerged(*project, &only);
        }
        return;
    }
    // 没有 .al 工程时 Python 会跑单文件分析（ADC/SDC）。这两个分析器在本项目里
    // 属于独立任务，本阶段没有；此处只清掉该文件的旧诊断，避免"旧诊断卡住"。
    PublishDiagnostics(path, lsp::DiagnosticList{});
}

// =============================================================================
//  TD 检查
// =============================================================================

void LspServer::ScheduleTdCheck(const project::Project& project) {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (shutting_down_) return;
        if (td_running_) {
            // 检查期间又保存了：不丢弃，当前这轮结束后再跑一轮（只置一个标志）
            td_pending_ = true;
            platform::default_logger().info("TD 检查进行中，已排队一次重跑");
            return;
        }
        td_running_ = true;
    }

    // 上一轮的 thread 对象可能还没回收（它已把 td_running_ 置 false 并返回）
    if (td_worker_.joinable()) td_worker_.join();

    td_worker_ = std::thread([this, project]() { TdCheckLoop(project); });
}

void LspServer::TdCheckLoop(project::Project project) {
    for (;;) {
        RunTdCheckOnce(project);

        bool again = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (td_pending_ && !shutting_down_) {
                td_pending_ = false;
                again = true;
            } else {
                td_pending_ = false;
                td_running_ = false;
            }
        }
        if (!again) return;
    }
}

void LspServer::RunTdCheckOnce(const project::Project& project) {
    const auto started = clock_type::now();

    if (!settings_.enable_td) {
        LogMessage(2, "TD check skipped: fpgaLsp.enableTd = false");
        return;
    }

    std::string error;
    std::shared_ptr<td::TdSession> session = CreateSession(project, &error);
    if (!session) {
        // 建不起会话：必须清掉旧表，否则上一版诊断会永远挂在 Problems 里
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            td_diags_.erase(project.al_path);
        }
        PublishMerged(project, nullptr);
        LogMessage(1, "TD check failed: " + error);
        return;
    }

    lsp::DiagnosticList diagnostics;
    std::string failure;
    try {
        // RunReadHdl 内部：ensure_device -> read_hdl -> 映射成 lsp::Diagnostic
        // （路径放在 data["path"] 里，发布前摘掉）
        diagnostics = td::RunReadHdl(project, *session, "");
    } catch (const std::exception& exc) {
        failure = exc.what();
    } catch (...) {
        failure = "unknown exception";
    }

    // read_hdl 之后 TD 关闭了 stdout 管道（见 td_session.h 硬约束），会话不可复用；
    // 直接 Kill（Close 会写 exit 并等 8s，此时管道已死，纯属浪费）。
    session->Kill();
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        if (session_ == session) session_.reset();
    }

    // 若这轮检查是被 Shutdown（exit/EOF）中断的，输出必然被截断：
    // 绝不能把"读了一半的空结果"当成"这个工程没有问题"发布出去。
    // （实测：exit 时 Kill 会话，read_hdl 的输出会截断成空 → 会误报 0 条诊断。）
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (shutting_down_) {
            platform::default_logger().info("TD 检查因退出被中断，跳过发布");
            return;
        }
    }

    if (!failure.empty()) {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            td_diags_.erase(project.al_path);
        }
        PublishMerged(project, nullptr);
        LogMessage(1, "TD check failed: " + failure);
        return;
    }

    std::map<std::string, lsp::DiagnosticList> table;
    for (auto& diagnostic : diagnostics) {
        const std::string path = DiagnosticPath(diagnostic, project.al_path);
        table[path].push_back(std::move(diagnostic));
    }

    std::size_t total = 0;
    for (const auto& [path, list] : table) {
        (void)path;
        total += list.size();
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        td_diags_[project.al_path] = std::move(table);
    }
    PublishMerged(project, nullptr);

    const double elapsed = std::chrono::duration<double>(clock_type::now() - started).count();
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "TD check complete: %zu problem(s) in %.2fs",
                  total, elapsed);
    LogMessage(3, buffer);
}

// =============================================================================
//  TD 会话
// =============================================================================

bool LspServer::SessionAlive() const {
    std::lock_guard<std::mutex> lock(session_mutex_);
    return session_ != nullptr && session_->Alive();
}

void LspServer::KillSession() {
    std::shared_ptr<td::TdSession> session;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        session = std::move(session_);
        session_ = nullptr;
    }
    if (session) session->Kill();
}

std::shared_ptr<td::TdSession> LspServer::CreateSession(const project::Project& project,
                                                        std::string* error) {
    if (!settings_.enable_td) {
        if (error) *error = "fpgaLsp.enableTd = false";
        return nullptr;
    }
    const std::string configured = settings_.td_path.empty() ? td_explicit_ : settings_.td_path;
    const std::optional<td::TdChoice> choice = td::FindTd(configured);
    if (!choice.has_value()) {
        if (error) {
            *error = "No TangDynasty installation found (set fpgaLsp.tdPath if it is not on PATH)";
        }
        return nullptr;
    }

    td::TdSessionOptions options;
    options.work_dir = RunDir(project);

    try {
        auto session = std::make_shared<td::TdSession>(*choice, options);
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            session_ = session;
        }
        return session;
    } catch (const std::exception& exc) {
        if (error) *error = exc.what();
        return nullptr;
    }
}

// =============================================================================
//  工程 / 路径
// =============================================================================

std::string LspServer::RootPath() const {
    if (root_uri_.empty()) return {};
    if (platform::uri_is_file(root_uri_)) return platform::uri_to_path(root_uri_);
    return platform::normalize_path(root_uri_);
}

std::optional<project::Project> LspServer::ProjectForPath(std::string_view path) {
    if (path.empty()) return std::nullopt;

    const std::string from_env = ReadEnv("FPGALSP_PROJECT");

    // 对应 Python: settings.project_file or project_override or os.environ["FPGALSP_PROJECT"]
    const std::string configured = !settings_.project_file.empty() ? settings_.project_file
                                 : !project_override_.empty()      ? project_override_
                                                                   : from_env;

    const std::string root = RootPath();

    // 查找结果做 5 秒记忆（对应 Python discover_projects 的 5s 缓存）。
    // 为什么必须缓存：didChange 每敲一个字都会走这里；工程上方没有 .al 时
    // FindProjectAl 会递归扫工作区根 —— 不缓存会让大工作区打字变卡。
    const std::string key = configured + "|" + root + "|" + std::string(path);
    {
        std::lock_guard<std::mutex> lock(discovery_mutex_);
        if (const auto it = discovery_cache_.find(key); it != discovery_cache_.end()) {
            if (clock_type::now() - it->second.at < std::chrono::seconds(5)) {
                return it->second.al.empty()
                           ? std::nullopt
                           : std::optional<project::Project>(project::ParseAl(it->second.al));
            }
        }
    }

    const auto al = project::FindProjectAl(root, path, configured);
    {
        std::lock_guard<std::mutex> lock(discovery_mutex_);
        DiscoveryEntry entry;
        entry.at = clock_type::now();
        entry.al = al.has_value() ? *al : std::string();
        // 简单的容量控制：超过 64 条就整体清空（缓存只是加速，丢了不影响正确性）
        if (discovery_cache_.size() >= 64) discovery_cache_.clear();
        discovery_cache_[key] = std::move(entry);
    }
    if (!al.has_value()) return std::nullopt;
    return project::ParseAl(*al);
}

std::optional<project::Project> LspServer::CurrentProject() {
    const std::string root = RootPath();
    if (root.empty()) {
        return std::nullopt;
    }
    return ProjectForPath(root);
}

std::optional<project::Project> LspServer::ProjectFromParams(const Json& params) {
    const std::string uri = jsonrpc::GetString(params, "uri");
    if (!uri.empty()) {
        if (auto project = ProjectForPath(platform::uri_to_path(uri)); project.has_value()) {
            return project;
        }
    }
    return CurrentProject();
}

void LspServer::InvalidateProjectCaches() {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        td_diags_.clear();
        published_.clear();
    }
    {
        // 工程切换后旧索引不再有意义（键是 al_path，留着也只是占内存）
        std::lock_guard<std::mutex> lock(hdl_mutex_);
        hdl_cache_.clear();
    }
}

void LspServer::SendFlowEvent(const Json& event) {
    // PROTOCOL-V2.md §4：type ∈ start|progress|log|step|done|error，done/error 时 ok 为布尔
    SendNotification("fpgaLsp/flowEvent", event);
}

// =============================================================================
//  flow 引擎（src/flow）
// =============================================================================

std::shared_ptr<flow::FlowRunner> LspServer::flow_runner() {
    std::lock_guard<std::mutex> lock(flow_mutex_);
    return flow_;
}

std::shared_ptr<flow::FlowRunner> LspServer::EnsureFlowRunner(const project::Project& project,
                                                             const td::TdChoice& td) {
    std::lock_guard<std::mutex> lock(flow_mutex_);
    // 同一个 .al 复用 runner；工程变了必须重建，否则 flow 会跑到旧工程上
    if (flow_ && flow_->project().al_path == project.al_path) return flow_;

    flow_ = std::make_shared<flow::FlowRunner>(
        project, td,
        // 通知：直接发 fpgaLsp/flowEvent（transport 写操作线程安全，可从 worker 线程调用）
        [this](const Json& event) { SendFlowEvent(event); },
        // 日志：走 window/logMessage（同样线程安全）
        [this](int type, std::string_view text) { LogMessage(type, text); });
    platform::default_logger().info("flow runner 已创建: " + project.al_path);
    return flow_;
}

}  // namespace fpgalsp::server