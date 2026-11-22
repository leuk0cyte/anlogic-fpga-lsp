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

#include "server/constraints_features.h"
#include "server/lsp_server.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include "platform/log.h"
#include "platform/path.h"
#include "platform/utf16.h"
#include "platform/version.h"
#include "flow/flow_runner.h"
#include "hdl/hdl_index.h"
#include "semantics/semantics.h"
#include "td/td_diagnostics.h"
#include "td/td_install.h"
#include "td/td_session.h"

// 内嵌 Verible formatter 桥（可选）。
//   链接了 fpga_lsp::verible_bridge 时 fmt 可用；否则走"返回空编辑"。
#if defined(FPGA_LSP_HAVE_VERIBLE_BRIDGE)
#include "verible_bridge/format_text.h"
#include "verible_bridge/lint_text.h"
#endif

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

// 会话 cwd：工程目录（TD 用 cwd 解析 .al 里的相对路径），没有工程目录时用系统临时目录。
// ⚠ 不要写成 `<project>/.fpgalsp/run`：那是旧位置，已废弃（状态目录统一走 state_dir()；
std::string RunDir(const project::Project& project) {
    if (!project.dir.empty()) {
        return project.dir;
    }
    std::error_code ec;
    const auto temp = std::filesystem::temp_directory_path(ec);
    const std::string base = ec ? std::string(".") : temp.generic_string();
    return platform::path_join(base, "fpgalsp/run");
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
    // 0.15.1：`veribleLintArgs` —— 旧插件的默认值是**字符串**
    // `"--rules=+line-length=length:160"`，而旧服务端只接受 list（isinstance(list)），
    // 所以那个默认值其实被静默丢弃了。这里两种写法都收：list 原样用；字符串按空白切。
    if (const auto it = section->find("veribleLintArgs"); it != section->end()) {
        std::vector<std::string> parsed;
        if (it->is_array()) {
            for (const auto& item : *it) {
                if (item.is_string()) parsed.push_back(item.get<std::string>());
            }
        } else if (it->is_string()) {
            std::istringstream stream(it->get<std::string>());
            for (std::string token; stream >> token;) parsed.push_back(token);
        }
        verible_lint_args = std::move(parsed);
    }
    verible_format = jsonrpc::GetBool(*section, "veribleFormat", verible_format);
    format_fallback = jsonrpc::GetBool(*section, "formatFallback", format_fallback);
    verible_autofix = jsonrpc::GetBool(*section, "veribleAutofix", verible_autofix);
    verible_highlight = jsonrpc::GetBool(*section, "veribleHighlight", verible_highlight);
    lint_on_change = jsonrpc::GetBool(*section, "lintOnChange", lint_on_change);
}

// =============================================================================
//  构造 / 析构 / 关闭
// =============================================================================

LspServer::LspServer(jsonrpc::TransportBaseCommon& transport, std::string td_explicit)
    : transport_(transport), td_explicit_(std::move(td_explicit)) {
    index_service_ = std::make_unique<IndexService>(
        [this](const std::string& path) { return LoadText(path); });
    report_store_ = std::make_unique<ReportStore>();

    // W3：TD 检查的**单飞队列 + 300 ms 去抖 + 取消**。
    //   以前是 lsp_server 里手写的"跑着不取消、只排队一次"循环（TdCheckLoop），
    //   A5（task-25）做好的 TdService 一直没接线 —— 这里把它接上。
    //   * 工厂：每一轮新建 TdSessionRunner（read_hdl 之后会话不可复用，见 td_session.h）；
    //   * use_project_work_dir=true：cwd = state_dir(project.dir)/run（系统临时目录；不写工程目录）
    //     （state-dir 外移由 platform 层负责，这里不比旧路径更"脏"）；
    //   * TdServiceOptions{}：debounce 300 ms / 取消在跑的一轮 / 取消后不再等（默认值，
    //     实测结论见 doc/audit/2026-11-18/phase2/A5-REPORT.md §2）。
    td_service_ = std::make_unique<td::TdService>(
        [this]() -> std::unique_ptr<td::TdCheckRunner> {
            const std::string configured =
                settings_.td_path.empty() ? td_explicit_ : settings_.td_path;
            const std::optional<td::TdChoice> choice = td::FindTd(configured);
            if (!choice.has_value()) return nullptr;  // -> failed("no TD check runner")
            return std::make_unique<td::TdSessionRunner>(*choice, td::TdSessionOptions{},
                                                         /*use_project_work_dir=*/true);
        },
        [this](const project::Project& project, td::TdCheckResult result) {
            // ApplyTdResult 接 const&：这里传左值即可（不要 std::move，门禁会报
            // performance-move-const-arg）。
            ApplyTdResult(project, result);
        },
        td::TdServiceOptions{/*debounce_ms=*/.debounce_ms=300});

    // T1：服务端 lint（内嵌 Verible）。worker 线程上跑，300 ms 去抖 + 每文件 single-flight。
    //   * Runner：取文档快照 -> LintText -> 转 LSP Diagnostic（UTF-16 列）。
    //   * Sink  ：写 diag_lint_ 后发布**并集**（lint + TD 互不覆盖）。
    //   sink 在 worker 线程，不能再调 Request()（见 lint_service.h 的注释）。
    lint_service_ = std::make_unique<LintService>(
        [this](const std::string& path, const std::string& /*uri*/) { return ComputeLint(path); },
        [this](const std::string& path, const std::string& /*uri*/,
               lsp::DiagnosticList diagnostics) {
            OnLintResult(path, std::move(diagnostics));
        },
        LintServiceOptions{/*debounce_ms=*/300});
}

LspServer::~LspServer() { Shutdown(); }

void LspServer::Shutdown() {
    {
        std::scoped_lock lock(state_mutex_);
        shutting_down_ = true;
    }

    // 0) W3：先停 TD 服务 —— CancelActive + join。取消会 TerminateJobObject 杀掉
    //    正在跑的 TD 进程树；被取消的轮次**不回调** ApplyTdResult（TdService 只在
    //    !cancelled && !stop_ && !superseded 时回调），所以不会有"shutdown 之后
    //    还往 transport 写诊断"的悬垂写。
    if (td_service_) td_service_->Shutdown();
    // 0b) T1：再停 lint 服务 —— 清 pending + join；退出后不再有 lint 发布。
    if (lint_service_) lint_service_->Shutdown();

    // 1) 再停 flow：runner 归 server 所有，必须在 transport 还活着的时候取消并 join。
    //    否则可能出现"进程退出时 flow worker 还在写 transport"的悬垂写。
    //    限时等待：取消后 worker 应在秒级退出，超时就交给 ~FlowRunner 兜底。
    {
        std::shared_ptr<flow::FlowRunner> runner;
        {
            std::scoped_lock lock(flow_mutex_);
            runner.swap(flow_);
        }
        if (runner) {
            try {
                runner->Cancel();
                if (!runner->WaitUntilIdle(10.0)) {
                    platform::default_logger().warn("flow did not stop within 10s; continuing shutdown");
                }
            } catch (const std::exception& exc) {
                platform::default_logger().error(std::string("flow shutdown: ") + exc.what());
            } catch (...) {
                platform::default_logger().error("flow shutdown: unknown exception");
            }
            runner.reset();  // 在 transport 仍存活时析构 runner
        }
    }

    // 2) 最后杀 device 查询用的会话（Job Object 会带走整棵进程树）。
    //    TD 检查的会话由 TdService 的 runner 自己持有并在 Run 里 Kill，不经过这里。
    KillSession();
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
    // Phase 2：客户端 FileSystemWatcher 已经会发这个通知；之前没有 handler，事件被静默丢弃。
    // A1 的 IndexService 接入前，这里只做轻量登记（签名机制仍会在请求时兜底）。
    dispatcher.Register("workspace/didChangeWatchedFiles", [this](const Json& params) {
        return HandleDidChangeWatchedFiles(params);
    });
    // A1：声明了 changeNotifications 就必须有 handler（否则 dispatcher 静默丢弃）。
    dispatcher.Register("workspace/didChangeWorkspaceFolders", [this](const Json& params) {
        return HandleDidChangeWorkspaceFolders(params);
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

    // ---- 标准 LSP：已实现的能力（Phase B 起不再是空返回）----
    dispatcher.Register("textDocument/codeAction",
                        [this](const Json& params) { return HandleCodeAction(params); });
    dispatcher.Register("textDocument/formatting",
                        [this](const Json& params) { return HandleFormatting(params); });
    dispatcher.Register("textDocument/rangeFormatting",
                        [this](const Json& params) { return HandleRangeFormatting(params); });
    dispatcher.Register("textDocument/prepareRename",
                        [this](const Json& params) { return HandlePrepareRename(params); });
    dispatcher.Register("textDocument/rename",
                        [this](const Json& params) { return HandleRename(params); });
    dispatcher.Register("textDocument/prepareCallHierarchy",
                        [this](const Json& params) { return HandlePrepareCallHierarchy(params); });
    dispatcher.Register("callHierarchy/incomingCalls",
                        [this](const Json& params) { return HandleIncomingCalls(params); });
    dispatcher.Register("callHierarchy/outgoingCalls",
                        [this](const Json& params) { return HandleOutgoingCalls(params); });
    dispatcher.Register("textDocument/semanticTokens/full", [this](const Json& params) {
        return HandleSemanticTokensFull(params);
    });
    dispatcher.Register("textDocument/semanticTokens/full/delta", [this](const Json& params) {
        return HandleSemanticTokensDelta(params);
    });
    dispatcher.Register("textDocument/foldingRange",
                        [this](const Json& params) { return HandleFoldingRange(params); });

    // ---- A4–A12：LSP 3.17/3.18 可选合规项（features_optional.cpp）----
    dispatcher.Register("textDocument/signatureHelp",
                        [this](const Json& params) { return HandleSignatureHelp(params); });
    dispatcher.Register("textDocument/documentLink",
                        [this](const Json& params) { return HandleDocumentLink(params); });
    dispatcher.Register("textDocument/codeLens",
                        [this](const Json& params) { return HandleCodeLens(params); });
    dispatcher.Register("textDocument/documentHighlight", [this](const Json& params) {
        return HandleDocumentHighlight(params);
    });
    dispatcher.Register("textDocument/selectionRange",
                        [this](const Json& params) { return HandleSelectionRange(params); });
    dispatcher.Register("textDocument/semanticTokens/range", [this](const Json& params) {
        return HandleSemanticTokensRange(params);
    });
    dispatcher.Register("completionItem/resolve",
                        [this](const Json& params) { return HandleCompletionResolve(params); });
    // A9：pull diagnostics（与 push 共存；数据源是最近一次 push 的缓存）
    dispatcher.Register("textDocument/diagnostic", [this](const Json& params) {
        return HandleDocumentDiagnostic(params);
    });
    dispatcher.Register("workspace/diagnostic", [this](const Json& params) {
        return HandleWorkspaceDiagnostic(params);
    });
    // A10：willSave（通知，no-op）/ willSaveWaitUntil（请求，立即返回 []）
    dispatcher.Register("textDocument/willSave",
                        [this](const Json& params) { return HandleWillSave(params); });
    dispatcher.Register("textDocument/willSaveWaitUntil", [this](const Json& params) {
        return HandleWillSaveWaitUntil(params);
    });
    // A11：`$/` 前缀默认被 dispatcher 静默丢弃；这两个是显式注册的例外。
    dispatcher.Register("$/setTrace",
                        [this](const Json& params) { return HandleSetTrace(params); });
    dispatcher.Register("$/logTrace",
                        [this](const Json& params) { return HandleLogTrace(params); });

    // ---- 自定义 v2 方法（PROTOCOL-V2.md）----
    dispatcher.Register("workspace/willRenameFiles",
                        [this](const Json& params) { return HandleWillRenameFiles(params); });
    dispatcher.Register("workspace/didRenameFiles",
                        [this](const Json& params) { return HandleDidRenameFiles(params); });
    dispatcher.Register("fpgaLsp/moduleChildren",
                        [this](const Json& params) { return HandleModuleChildren(params); });
    dispatcher.Register("fpgaLsp/moduleTree",
                        [this](const Json& params) { return HandleModuleTree(params); });
    dispatcher.Register("fpgaLsp/reports",
                        [this](const Json& params) { return HandleReports(params); });
    dispatcher.Register("fpgaLsp/flowStatus",
                        [this](const Json& params) { return HandleFlowStatus(params); });

    // ---- workspace/executeCommand ----
    dispatcher.Register("workspace/executeCommand",
                        [this](const Json& params) { return HandleExecuteCommand(params); });

    platform::default_logger().info("LSP handler registration complete (" +
                                    std::to_string(dispatcher.Methods().size()) + " methods)");
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

    // A1：记住完整的工作区文件夹列表（didChangeWorkspaceFolders 用它判断"当前根是否已被移除"）。
    // 只在本条参数真的带列表时才覆盖 —— MCP 侧会用空 params 调本函数取能力声明。
    const Json folder_array = jsonrpc::GetArray(params, "workspaceFolders");
    if (!folder_array.empty()) {
        workspace_folders_.clear();
        for (const Json& folder : folder_array) {
            if (!folder.is_object()) continue;
            const std::string uri = jsonrpc::GetString(folder, "uri");
            if (!uri.empty()) workspace_folders_.push_back(uri);
        }
    } else if (!root.empty() && workspace_folders_.empty()) {
        workspace_folders_.push_back(root);
    }

    if (const auto it = params.find("initializationOptions"); it != params.end()) {
        settings_.Update(*it);
    }

    Json capabilities = Json::object();

    // textDocumentSync: {openClose, change:2(Incremental), save:{includeText:true}}
    //   includeText=true 与 Python 一致：didSave 时把缓冲区交给服务端，避免落盘竞态。
    Json sync = Json::object();
    sync["openClose"] = true;
    sync["change"] = 2;
    Json save = Json::object();
    save["includeText"] = true;
    sync["save"] = std::move(save);
    // A10：willSave 只收通知；willSaveWaitUntil 允许保存前请求编辑（必须立即返回）。
    sync["willSave"] = true;
    sync["willSaveWaitUntil"] = true;
    capabilities["textDocumentSync"] = std::move(sync);

    // 与 Python 的 completionProvider 逐字对齐（集合相同、顺序不参与契约）
    Json completion = Json::object();
    completion["triggerCharacters"] =
        Json::array({"{", "=", " ", "[", "\t", "|"});
    // A12：轻量 item 先下发，documentation/detail 在 completionItem/resolve 补齐。
    completion["resolveProvider"] = true;
    capabilities["completionProvider"] = std::move(completion);

    // A4：例化/端口列表里的签名帮助（`(` 与 `,` 触发；`,` 重新触发）。
    Json signature_help = Json::object();
    signature_help["triggerCharacters"] = Json::array({"(", ","});
    signature_help["retriggerCharacters"] = Json::array({","});
    capabilities["signatureHelpProvider"] = std::move(signature_help);

    // A5：`.al` 的 `<File Path>` -> 可点击链接；resolve 阶段无需再查。
    Json document_link = Json::object();
    document_link["resolveProvider"] = false;
    capabilities["documentLinkProvider"] = std::move(document_link);

    // A6：每个 module 定义一条 "N instantiations" lens（command 已完整）。
    Json code_lens = Json::object();
    code_lens["resolveProvider"] = false;
    capabilities["codeLensProvider"] = std::move(code_lens);

    // A7 / A8：布尔能力（无参数）
    capabilities["documentHighlightProvider"] = true;
    capabilities["selectionRangeProvider"] = true;

    // 多根工作区：Python 声明 supported + changeNotifications
    Json folders = Json::object();
    folders["supported"] = true;
    folders["changeNotifications"] = true;
    Json workspace = Json::object();
    workspace["workspaceFolders"] = std::move(folders);
    // B-A3：改名通知。只声明 HDL 扩展名（.al 由我们改，不由用户手工改）。
    Json rename_filters = Json::array();
    Json filter = Json::object();
    filter["scheme"] = "file";
    Json pattern = Json::object();
    pattern["glob"] = "**/*.{v,sv,vh,svh,vhd,vhdl}";
    filter["pattern"] = std::move(pattern);
    rename_filters.push_back(std::move(filter));
    Json operations = Json::object();
    Json will_rename = Json::object();
    will_rename["filters"] = rename_filters;
    operations["willRename"] = std::move(will_rename);
    Json did_rename = Json::object();
    did_rename["filters"] = rename_filters;
    operations["didRename"] = std::move(did_rename);
    workspace["fileOperations"] = std::move(operations);
    capabilities["workspace"] = std::move(workspace);

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

    // B-A2：命令清单必须对第三方客户端可见（25 条已实现命令）。
    // 表在 commands.cpp 的 ExecuteCommandNames()，与 dispatch 共用同一份来源。
    Json execute = Json::object();
    Json command_list = Json::array();
    for (const std::string& name : AdvertisedCommandNames()) command_list.push_back(name);
    execute["commands"] = std::move(command_list);
    capabilities["executeCommandProvider"] = std::move(execute);

    Json legend = Json::object();
    legend["tokenTypes"] = Json::array({"keyword", "type",      "class",   "property",
                                        "variable", "parameter", "function", "number",
                                        "comment",  "string",    "operator", "macro",});
    legend["tokenModifiers"] = Json::array();
    Json semantic = Json::object();
    semantic["legend"] = std::move(legend);
    semantic["full"] = Json::object({{"delta", true}});
    // A12：`textDocument/semanticTokens/range`（按 range 过滤后重新编码）。
    semantic["range"] = true;
    capabilities["semanticTokensProvider"] = std::move(semantic);

    capabilities["foldingRangeProvider"] = true;
    capabilities["inlayHintProvider"] = true;

    // A9：pull diagnostics 能力声明。identifier 让客户端能区分多个诊断来源；
    // interFileDependencies=false：单文件报告不依赖别的文件（诊断表按文件维护）；
    // workspaceDiagnostics=true：支持 workspace/diagnostic。
    Json diagnostic = Json::object();
    diagnostic["identifier"] = "fpga-lsp";
    diagnostic["interFileDependencies"] = false;
    diagnostic["workspaceDiagnostics"] = true;
    capabilities["diagnosticProvider"] = std::move(diagnostic);

    // LSP 3.17：本项目的位置换算全部按 UTF-16 码元（document_store / utf16 层）
    capabilities["positionEncoding"] = "utf-16";

    Json result = Json::object();
    result["capabilities"] = std::move(capabilities);
    Json info = Json::object();
    info["name"] = "fpga-lsp";
    info["version"] = platform::kServerVersion;
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

// Phase 2：客户端 FileSystemWatcher 的变化通知。
// A1 的 IndexService 接入后，这里会调用 OnWatchedFiles() 做增量更新；当前先记录，
// 具体失效仍由下一次请求的工程签名兜底（不改变行为）。
HandlerResult LspServer::HandleDidChangeWatchedFiles(const Json& params) {
    const Json changes = jsonrpc::GetArray(params, "changes");
    std::vector<std::string> changed;
    std::vector<std::string> deleted;
    for (const Json& change : changes) {
        if (!change.is_object()) continue;
        const std::string uri = jsonrpc::GetString(change, "uri");
        if (uri.empty()) continue;
        const std::string path = platform::uri_to_path(uri);
        const int type = jsonrpc::GetInt(change, "type", 2);
        if (type == 3) {
            deleted.push_back(path);
        } else {
            changed.push_back(path);
        }
    }
    if (index_service_) {
        index_service_->SetWatcherActive(true);
        index_service_->OnWatchedFiles(changed, deleted);
    }
    // 审查 C1（P0）：文本缓存键是 (path,size,mtime)，watcher 事件说明磁盘确实变了，
    // 而 (size,mtime) 无法识别"同大小 + mtime 被还原"的改写 —— 必须在这里显式失效，
    // 否则 completion/hover/codeAction/ADC-SDC 会一直读到旧文本。
    for (const std::string& path : changed) docs_.EvictTextCache(path);
    for (const std::string& path : deleted) docs_.EvictTextCache(path);
    if (report_store_) {
        std::vector<std::string> all = changed;
        all.insert(all.end(), deleted.begin(), deleted.end());
        if (report_store_->InvalidatePaths(all) != 0) {
            // C1：只读使用 -> shared 访问器，省掉每事件的 Project 深拷贝。
            if (const std::shared_ptr<const project::Project> project = CurrentProjectShared();
                project != nullptr) {
                SendReportsChanged(*project, all);
            }
        }
    }
    MaybeNotifyIndexChanged(changed);
    platform::default_logger().debug("didChangeWatchedFiles: " +
                                     std::to_string(changed.size()) + " changed, " +
                                     std::to_string(deleted.size()) + " deleted");
    return HandlerResult::Ok(Json());
}

// A1：`workspace/didChangeWorkspaceFolders`。
// 我们声明了 `workspace.workspaceFolders.changeNotifications:true`，客户端（VS Code 多根工作区）
// 增删文件夹时就会发这个通知；以前没有 handler，dispatcher 静默丢弃 → 服务端仍按旧根解析工程，
// 新加的文件夹看不到、删掉的还留着（审计 2026-11-22 的 A1）。
HandlerResult LspServer::HandleDidChangeWorkspaceFolders(const Json& params) {
    const auto event_it = params.find("event");
    if (event_it == params.end() || !event_it->is_object()) return HandlerResult::Ok(Json());
    const Json& event = *event_it;

    std::size_t removed_count = 0;
    if (const auto removed = event.find("removed");
        removed != event.end() && removed->is_array()) {
        for (const Json& folder : *removed) {
            if (!folder.is_object()) continue;
            const std::string uri = jsonrpc::GetString(folder, "uri");
            if (uri.empty()) continue;
            const auto it = std::ranges::find(workspace_folders_, uri);
            if (it != workspace_folders_.end()) {
                workspace_folders_.erase(it);
                ++removed_count;
            }
        }
    }
    std::size_t added_count = 0;
    if (const auto added = event.find("added"); added != event.end() && added->is_array()) {
        for (const Json& folder : *added) {
            if (!folder.is_object()) continue;
            const std::string uri = jsonrpc::GetString(folder, "uri");
            if (uri.empty()) continue;
            if (std::ranges::find(workspace_folders_, uri) ==
                workspace_folders_.end()) {
                workspace_folders_.push_back(uri);
                ++added_count;
            }
        }
    }
    // 当前主根被移除 -> 换成列表里的第一个；列表空了就回到"无工作区"。
    // （root 也可能是 initialize 用 rootUri 给的、不在 folders 列表里：那种情况不动它。）
    if (workspace_folders_.empty()) {
        if (removed_count > 0) root_uri_.clear();
    } else if (root_uri_.empty() ||
               std::ranges::find(workspace_folders_, root_uri_) ==
                   workspace_folders_.end()) {
        root_uri_ = workspace_folders_.front();
    }
    // 根变了：工程/发现/报告缓存全部按路径缓存，必须失效（下一次请求会重新解析）。
    InvalidateProjectCaches();
    if (report_store_) report_store_->InvalidateAll();
    platform::default_logger().info("didChangeWorkspaceFolders: +" +
                                    std::to_string(added_count) + "/-" +
                                    std::to_string(removed_count) + " root=" + root_uri_);
    return HandlerResult::Ok(Json());
}

// Phase 2：索引快照 generation 变了才推送（插件据此定向刷新；旧客户端忽略）。
// P-C1：带上 `modules[]` / `full` —— 客户端早已实现按模块打补丁
// （vscode-extension/src/moduleTree.js:221-255），以前服务端只发提示，
// 于是每次保存都要回传一份全树（实测 16,426 B）。
void LspServer::MaybeNotifyIndexChanged(const std::vector<std::string>& paths) {
    if (!index_service_ || paths.empty()) return;
    const std::uint64_t generation = index_service_->stats().generation;
    if (generation == 0 || generation == last_index_generation_) return;
    last_index_generation_ = generation;
    const IndexService::ChangeSet changes = index_service_->LastChanges();
    Json params = Json::object();
    params["al"] = index_service_->al_path();
    params["generation"] = generation;
    params["paths"] = paths;
    if (changes.full) {
        params["full"] = true;
    } else if (!changes.modules.empty()) {
        Json modules = Json::array();
        for (const IndexService::ModuleDelta& delta : changes.modules) {
            Json item = Json::object();
            item["module"] = delta.module;
            item["portCount"] = delta.port_count;
            item["defined"] = delta.defined;
            modules.push_back(std::move(item));
        }
        params["modules"] = std::move(modules);
    }
    SendNotification("fpgaLsp/indexChanged", std::move(params));
}

void LspServer::SendReportsChanged(const project::Project& project,
                                   const std::vector<std::string>& paths) {
    if (!report_store_) return;
    // D9：这里**不再** Get(kAll)。通知只需要"哪些段受影响" + revision，
    // 而 Get(kAll) 在报告刚变时是一次冷构建（审计 768–784 ms，还在 IO 线程上）。
    (void)project;  // 保留参数：调用点已有工程对象，日志/未来按工程过滤会用到
    const std::uint32_t affected = SectionsForPaths(paths);
    Json params = Json::object();
    params["stage"] = "auto";
    Json sections = Json::array();
    for (const char* name : {"files", "artifacts", "qor", "timing", "area", "flow"}) {
        bool ok = false;
        const std::uint32_t bit = reports::sections::Parse(name, &ok);
        if (ok && (affected & bit) != 0) sections.push_back(name);
    }
    params["sections"] = std::move(sections);
    params["revision"] = report_store_->revision();
    // 兼容字段：旧客户端只读 generatedAt 也不会因此构建快照（0 = 未构建）。
    params["generatedAt"] = 0.0;
    if (!paths.empty()) {
        Json changed_paths = Json::array();
        for (const std::string& path : paths) changed_paths.push_back(path);
        params["paths"] = std::move(changed_paths);
    }
    SendNotification("fpgaLsp/reportsChanged", std::move(params));
}

HandlerResult LspServer::HandleNull(const Json& params) {
    (void)params;
    return HandlerResult::Ok(Json(nullptr));
}

HandlerResult LspServer::HandleEmptyList(const Json& params) {
    (void)params;
    return HandlerResult::Ok(Json::array());
}

// =============================================================================
//  textDocument/formatting —— 内嵌 Verible formatter
// =============================================================================
//  返回**单条覆盖全文的 TextEdit**（PROTOCOL-V2 v4/v5 的约定：
//    "textDocument/formatting 返回单条覆盖全文的 TextEdit；未找到 Verible 时返回 []（不报错）"）
//
//  行为细节（都有实测依据）：
//    * 文本没变 → 返回 []。不回一条"把原文替换成原文"的编辑，否则每次保存都会
//      让编辑器的 dirty 标记闪一下。
//    * 格式化失败（语法错误等）→ 返回 []，**不抛错也不弹窗**。
//      用户正在输入时文件常常是残缺的，弹错会打断；真正的语法问题由
//      TD / Verible lint 的 diagnostics 通道报告（那里有行号能定位）。
//    * 空文档 / 纯空白 → 直接 []（桥里已短路）。
HandlerResult LspServer::HandleFormatting(const Json& params) {
#if !defined(FPGA_LSP_HAVE_VERIBLE_BRIDGE)
    // 未链接 formatter 桥：保持旧契约 —— 合法空结果，绝不 -32601
    (void)params;
    return HandlerResult::Ok(Json::array());
#else
    // 取 uri：params.textDocument.uri（不可信输入，逐层判类型，缺失即空）
    std::string uri;
    if (auto doc = params.find("textDocument");
        doc != params.end() && doc->is_object()) {
        if (auto it = doc->find("uri"); it != doc->end() && it->is_string()) {
            uri = it->get<std::string>();
        }
    }
    if (uri.empty()) return HandlerResult::Ok(Json::array());

    const std::string path = platform::uri_to_path(uri);
    const auto text = LoadText(path);  // 未保存缓冲区优先，落盘回落
    if (!text.has_value()) return HandlerResult::Ok(Json::array());

    // 排版风格来自插件设置（`fpgaLsp.verible.args`）；空 = 桥里的旧插件默认值。
    // 这里**不能**再用 FormatText 的默认参数：0.15.0 就是这么丢掉 4 空格缩进的。
    verible_bridge::FormatOptions format_options;
    format_options.style_flags = settings_.verible_args;
    const auto result = verible_bridge::FormatText(*text, path, format_options);
    if (!result.ok && !result.error.empty()) {
        // 非法 style flag / Verible 失败：记日志（不弹窗），下面照旧返回空编辑
        platform::log_warn("format: " + path + ": " + result.error);
    }
    if (!result.ok || result.text == *text) {
        // 失败、或"本来就已格式化"：都返回空编辑数组
        return HandlerResult::Ok(Json::array());
    }

    // 覆盖全文的单条 TextEdit。
    // 结束位置按**旧文本**的末尾算（LSP 语义：编辑基于请求时的文档状态）。
    //
    // 末尾位置要精确：
    //   * 文本以 '\n' 结尾 → 末尾是"下一行的第 0 列"（行号 = 换行符个数）
    //   * 不以 '\n' 结尾  → 末尾是最后一行的末列
    // 只写 {line:0, character:0}..{line:大数,0} 在某些客户端上会多删一个空行。
    std::size_t last_break = text->rfind('\n');
    int end_line = 0;
    int end_char = 0;
    if (last_break == std::string::npos) {
        end_char = static_cast<int>(text->size());
    } else {
        end_line = static_cast<int>(
            std::count(text->begin(), text->end(), '\n'));
        end_char = static_cast<int>(text->size() - last_break - 1);
    }

    Json range;
    range["start"] = {{"line", 0}, {"character", 0}};
    range["end"] = {{"line", end_line}, {"character", end_char}};

    Json edit;
    edit["range"] = std::move(range);
    edit["newText"] = result.text;

    Json edits = Json::array();
    edits.push_back(std::move(edit));
    // 覆盖全文的编辑会让客户端本地的 semantic token 全部作废；这里把服务端那份
    // 缓存也丢掉，下一次请求必然是 full —— 否则可能拿旧基线算 delta，高亮会错位。
    {
        std::scoped_lock lock(semantic_mutex_);
        semantic_cache_.erase(uri);
    }
    return HandlerResult::Ok(std::move(edits));
#endif
}

namespace {

bool TextDocumentUri(const Json& params, std::string* uri) {
    const auto doc = params.find("textDocument");
    if (doc == params.end() || !doc->is_object()) return false;
    const auto value = doc->find("uri");
    if (value == doc->end() || !value->is_string()) return false;
    *uri = value->get<std::string>();
    return !uri->empty();
}

bool CursorPosition(const Json& params, int* line, int* character) {
    const auto pos = params.find("position");
    if (pos == params.end() || !pos->is_object()) return false;
    *line = jsonrpc::GetInt(*pos, "line", -1);
    *character = jsonrpc::GetInt(*pos, "character", -1);
    return *line >= 0 && *character >= 0;
}

}  // namespace

// textDocument/rangeFormatting —— Phase B 接入（B4 填 FeatureRangeFormatting）
HandlerResult LspServer::HandleRangeFormatting(const Json& params) {
    std::string uri;
    if (!TextDocumentUri(params, &uri)) return HandlerResult::Ok(Json::array());
    const Json range = params.contains("range") && params["range"].is_object()
                           ? params["range"]
                           : Json::object();
    const Json options = params.contains("options") && params["options"].is_object()
                             ? params["options"]
                             : Json::object();
    return HandlerResult::Ok(
        FeatureRangeFormatting(platform::uri_to_path(uri), range, options));
}

namespace {

// P1-1：resultId 直接对编码好的整数数组做 64-bit FNV，不再 `data.dump()`
// 生成 30–60 KB 临时字符串再逐字符 hash（一次 full 请求省掉整份序列化 + 分配）。
// 确定性：同一 (uri, data) 恒等；不同 data 的碰撞概率保持 64-bit FNV 级别。
std::string SemanticResultId(const std::vector<int>& data, std::string_view uri) {
    std::uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](unsigned char c) {
        hash ^= c;
        hash *= 1099511628211ull;
    };
    for (const char c : uri) mix(static_cast<unsigned char>(c));
    mix(0xff);  // 分隔符，避免 (uri, data) 边界歧义
    for (const int value : data) {
        const auto bits = static_cast<std::uint32_t>(value);
        mix(static_cast<unsigned char>(bits & 0xFFu));
        mix(static_cast<unsigned char>((bits >> 8) & 0xFFu));
        mix(static_cast<unsigned char>((bits >> 16) & 0xFFu));
        mix(static_cast<unsigned char>((bits >> 24) & 0xFFu));
    }
    // 与 snprintf("%016llx") 逐字节一致
    return std::format("{:016x}", static_cast<unsigned long long>(hash));
}

// 编码后的 token -> LSP `data` 数组。只在**组装响应**时做一次。
Json SemanticDataArray(const std::vector<int>& encoded) {
    Json data = Json::array();
    data.get_ref<nlohmann::json::array_t&>().reserve(encoded.size());
    for (const int value : encoded) data.push_back(value);
    return data;
}

// SemanticDeltaEdits 的整数数组版本（handler 热路径用；Json 版本保留给单测/其它调用方）。
Json SemanticDeltaEditsVector(const std::vector<int>& previous,
                              const std::vector<int>& current) {
    constexpr std::size_t kStride = 5;
    const std::size_t before = previous.size();
    const std::size_t after = current.size();

    std::size_t prefix = 0;
    while (prefix + kStride <= before && prefix + kStride <= after) {
        bool same = true;
        for (std::size_t k = 0; k < kStride; ++k) {
            if (previous[prefix + k] != current[prefix + k]) {
                same = false;
                break;
            }
        }
        if (!same) break;
        prefix += kStride;
    }

    std::size_t suffix = 0;
    while (suffix + kStride <= before - prefix && suffix + kStride <= after - prefix) {
        const std::size_t p = before - kStride - suffix;
        const std::size_t c = after - kStride - suffix;
        bool same = true;
        for (std::size_t k = 0; k < kStride; ++k) {
            if (previous[p + k] != current[c + k]) {
                same = false;
                break;
            }
        }
        if (!same) break;
        suffix += kStride;
    }

    const std::size_t delete_count = before - prefix - suffix;
    const std::size_t insert_count = after - prefix - suffix;
    Json edits = Json::array();
    if (delete_count == 0 && insert_count == 0) return edits;

    Json edit = Json::object();
    edit["start"] = prefix;
    edit["deleteCount"] = delete_count;
    if (insert_count > 0) {
        Json slice = Json::array();
        slice.get_ref<nlohmann::json::array_t&>().reserve(insert_count);
        for (std::size_t i = prefix; i < prefix + insert_count; ++i) {
            slice.push_back(current[i]);
        }
        edit["data"] = std::move(slice);
    }
    edits.push_back(std::move(edit));
    return edits;
}

}  // namespace

// P-D5：真正的语义高亮增量。
// 数据是 5 元组数组（deltaLine, deltaStart, length, type, modifiers），所以前缀/后缀
// 只能落在 5 的倍数上。返回 0 或 1 条编辑：
//   * 完全相同时 0 条（客户端复用旧 token）；
//   * 否则一条 {start, deleteCount, data}，只覆盖真正变了的区间。
// 旧实现把**整个新数组**塞进 deleteCount = 旧长度 的编辑里，等于"旧数组 + 新数组"
// 都传一遍，比 full 还贵。
jsonrpc::Json SemanticDeltaEdits(const jsonrpc::Json& previous,
                                 const jsonrpc::Json& current) {
    constexpr std::size_t kStride = 5;
    const std::size_t before = previous.is_array() ? previous.size() : 0;
    const std::size_t after = current.is_array() ? current.size() : 0;

    std::size_t prefix = 0;
    while (prefix + kStride <= before && prefix + kStride <= after) {
        bool same = true;
        for (std::size_t k = 0; k < kStride; ++k) {
            if (previous[prefix + k] != current[prefix + k]) {
                same = false;
                break;
            }
        }
        if (!same) break;
        prefix += kStride;
    }

    std::size_t suffix = 0;
    while (suffix + kStride <= before - prefix && suffix + kStride <= after - prefix) {
        const std::size_t p = before - kStride - suffix;
        const std::size_t c = after - kStride - suffix;
        bool same = true;
        for (std::size_t k = 0; k < kStride; ++k) {
            if (previous[p + k] != current[c + k]) {
                same = false;
                break;
            }
        }
        if (!same) break;
        suffix += kStride;
    }

    const std::size_t delete_count = before - prefix - suffix;
    const std::size_t insert_count = after - prefix - suffix;
    Json edits = Json::array();
    if (delete_count == 0 && insert_count == 0) return edits;

    Json edit = Json::object();
    edit["start"] = prefix;
    edit["deleteCount"] = delete_count;
    if (insert_count > 0) {
        Json slice = Json::array();
        for (std::size_t i = prefix; i < prefix + insert_count; ++i) {
            slice.push_back(current[i]);
        }
        edit["data"] = std::move(slice);
    }
    edits.push_back(std::move(edit));
    return edits;
}

HandlerResult LspServer::HandleSemanticTokensFull(const Json& params) {
    std::string uri;
    if (!TextDocumentUri(params, &uri)) {
        Json empty = Json::object();
        empty["data"] = Json::array();
        return HandlerResult::Ok(std::move(empty));
    }
    const std::string path = platform::uri_to_path(uri);
    const SemanticTokenContext context = SemanticTokenContextFor(path);
    const std::shared_ptr<const std::string> text = LoadTextShared(path);
    const std::shared_ptr<const std::vector<int>> encoded = ComputeSemanticTokens(context, text);
    static const std::vector<int> kNoSemanticData;
    const std::string id = SemanticResultId(encoded ? *encoded : kNoSemanticData, uri);
    {
        // P1-1：锁内只做 shared_ptr 赋值（零深拷贝），不再拷贝整份 token 数组。
        std::scoped_lock lock(semantic_mutex_);
        SemanticCacheEntry& entry = semantic_cache_[uri];
        entry.result_id = id;
        entry.encoded = encoded;
        entry.version = context.version;
        entry.index_generation = context.index_generation;
        entry.have_meta = context.have_generation;
    }
    Json result = Json::object();
    result["data"] = encoded ? SemanticDataArray(*encoded) : Json::array();
    result["resultId"] = id;
    return HandlerResult::Ok(std::move(result));
}

// Phase 2 / B2：semanticTokens 增量。
//   * 文档版本与索引 generation 都没变 -> 直接回空 edits（连 tokenize 都不跑）；
//   * 变了 -> 只回真正变化的那一段 5 元组（vector<int> 直接比较，零 JSON 拷贝）。
HandlerResult LspServer::HandleSemanticTokensDelta(const Json& params) {
    std::string uri;
    if (!TextDocumentUri(params, &uri)) {
        Json empty = Json::object();
        empty["edits"] = Json::array();
        return HandlerResult::Ok(std::move(empty));
    }
    const std::string previous = jsonrpc::GetString(params, "previousResultId");
    const std::string path = platform::uri_to_path(uri);
    const SemanticTokenContext context = SemanticTokenContextFor(path);

    // P1-1 增量短路：同版本 + 同索引 generation + 客户端基线就是缓存的那份
    // -> 内容不可能变，直接空 edits（旧实现这里会先全量重算再发现"没变"）。
    if (!previous.empty() && context.version.has_value() && context.have_generation) {
        std::scoped_lock lock(semantic_mutex_);
        if (const auto it = semantic_cache_.find(uri); it != semantic_cache_.end()) {
            const SemanticCacheEntry& entry = it->second;
            if (entry.encoded && entry.have_meta && entry.result_id == previous &&
                entry.version == context.version &&
                entry.index_generation == context.index_generation) {
                Json result = Json::object();
                result["resultId"] = previous;
                result["edits"] = Json::array();
                return HandlerResult::Ok(std::move(result));
            }
        }
    }

    const std::shared_ptr<const std::string> text = LoadTextShared(path);
    const std::shared_ptr<const std::vector<int>> encoded = ComputeSemanticTokens(context, text);
    static const std::vector<int> kNoSemanticData;
    const std::string id = SemanticResultId(encoded ? *encoded : kNoSemanticData, uri);

    Json edits = Json::array();
    bool full_result = false;
    {
        std::scoped_lock lock(semantic_mutex_);
        SemanticCacheEntry& entry = semantic_cache_[uri];
        // ⚠ 必须尊重 previousResultId：delta 的编辑是相对**客户端手里那份**数据算的。
        //   如果客户端的基线不是我们缓存的那一份（典型场景：格式化返回一条覆盖全文的
        //   编辑，客户端本地 token 已被作废/重建，或者中间穿插过一次 full 请求把
        //   entry.data 推进了），再回 delta 就会让客户端把"差量"套到错误的基线上 ——
        //   表现就是**格式化之后语法高亮错位**。这种情况只能回全量。
        if (previous.empty() || previous != entry.result_id) {
            full_result = true;
        } else if (entry.result_id != id) {
            // P-D5：只发真正变化的那一段（而不是"新数组 + deleteCount=旧长度"）。
            static const std::vector<int> kEmpty;
            edits = SemanticDeltaEditsVector(entry.encoded ? *entry.encoded : kEmpty,
                                             encoded ? *encoded : kEmpty);
        }
        entry.result_id = id;
        entry.encoded = encoded;
        entry.version = context.version;
        entry.index_generation = context.index_generation;
        entry.have_meta = context.have_generation;
    }
    Json result = Json::object();
    result["resultId"] = id;
    if (full_result) {
        result["data"] = encoded ? SemanticDataArray(*encoded) : Json::array();
    } else {
        result["edits"] = std::move(edits);
    }
    (void)previous;
    return HandlerResult::Ok(std::move(result));
}

HandlerResult LspServer::HandleFoldingRange(const Json& params) {
    std::string uri;
    if (!TextDocumentUri(params, &uri)) return HandlerResult::Ok(Json::array());
    return HandlerResult::Ok(FeatureFoldingRanges(platform::uri_to_path(uri)));
}

HandlerResult LspServer::HandleCodeAction(const Json& params) {
    std::string uri;
    if (!TextDocumentUri(params, &uri)) return HandlerResult::Ok(Json::array());
    const Json range = params.contains("range") && params["range"].is_object()
                           ? params["range"]
                           : Json::object();
    const Json context = params.contains("context") && params["context"].is_object()
                             ? params["context"]
                             : Json::object();
    return HandlerResult::Ok(
        FeatureCodeActions(platform::uri_to_path(uri), range, context));
}

HandlerResult LspServer::HandlePrepareRename(const Json& params) {
    std::string uri;
    int line = 0;
    int character = 0;
    if (!TextDocumentUri(params, &uri) || !CursorPosition(params, &line, &character)) {
        return HandlerResult::Ok(Json(nullptr));
    }
    return HandlerResult::Ok(
        FeaturePrepareRename(platform::uri_to_path(uri), line, character));
}

HandlerResult LspServer::HandleRename(const Json& params) {
    std::string uri;
    int line = 0;
    int character = 0;
    if (!TextDocumentUri(params, &uri) || !CursorPosition(params, &line, &character)) {
        return HandlerResult::Ok(Json(nullptr));
    }
    const std::string new_name = jsonrpc::GetString(params, "newName");
    return HandlerResult::Ok(
        FeatureRename(platform::uri_to_path(uri), line, character, new_name));
}

HandlerResult LspServer::HandlePrepareCallHierarchy(const Json& params) {
    std::string uri;
    int line = 0;
    int character = 0;
    if (!TextDocumentUri(params, &uri) || !CursorPosition(params, &line, &character)) {
        return HandlerResult::Ok(Json::array());
    }
    return HandlerResult::Ok(
        FeatureCallHierarchyPrepare(platform::uri_to_path(uri), line, character));
}

HandlerResult LspServer::HandleIncomingCalls(const Json& params) {
    const Json item = params.contains("item") && params["item"].is_object()
                          ? params["item"]
                          : Json::object();
    return HandlerResult::Ok(FeatureCallHierarchyIncoming(item));
}

HandlerResult LspServer::HandleOutgoingCalls(const Json& params) {
    const Json item = params.contains("item") && params["item"].is_object()
                          ? params["item"]
                          : Json::object();
    return HandlerResult::Ok(FeatureCallHierarchyOutgoing(item));
}

// =============================================================================
//  HDL 索引（src/hdl）
// =============================================================================

std::optional<std::string> LspServer::LoadText(const std::string& path) {
    // DocumentStore：按**路径**查（客户端 uri 是 `file:///y%3A/...`，与内部
    // `path_to_uri()` 的大小写/编码不同，用 uri 直接查表会 miss -> 回落到磁盘 ->
    // 未保存修改不可见，格式化后语义高亮错位）。未打开时再按 uri 走磁盘缓存。
    return docs_.GetTextByPath(path);
}

std::shared_ptr<const std::string> LspServer::LoadTextShared(const std::string& path) {
    // P1-2：热路径（semanticTokens/folding）零整文件拷贝。返回的 shared_ptr 指向
    // DocumentStore 内部的不可变字符串，锁外安全。
    return docs_.GetTextByPathShared(path);
}

namespace {

std::string LowerAsciiPath(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

// B8/PERF-01：把"每文件 2 次系统调用"（file_size + last_write_time）换成
// "每目录 1 次枚举"。Windows 上一次目录枚举就带回整目录的 (size, mtime)，
// 所以 200 文件工程的签名从 ~13 ms 降到 <1 ms。
//
// 正确性：枚举不到（目录不可读、条目缺失、非常规文件名）就返回 nullptr，
// 调用方回落到原来的逐文件查询 —— 输出与旧实现逐字节一致。
class DirStampCache {
public:
    struct Stamp {
        std::uintmax_t size = 0;
        std::int64_t mtime = 0;
    };

    const Stamp* Find(const std::string& path) {
        const std::size_t slash = path.find_last_of("/\\");
        if (slash == std::string::npos || slash + 1 >= path.size()) return nullptr;
        const std::string dir = path.substr(0, slash);
        const std::string name = LowerAsciiPath(std::string_view(path).substr(slash + 1));

        auto it = dirs_.find(dir);
        if (it == dirs_.end()) {
            it = dirs_.emplace(dir, LoadDir(dir)).first;
        }
        const auto found = it->second.find(name);
        return found == it->second.end() ? nullptr : &found->second;
    }

private:
    static std::map<std::string, Stamp> LoadDir(const std::string& dir) {
        std::map<std::string, Stamp> entries;
        std::error_code ec;
        std::filesystem::directory_iterator iter{std::filesystem::path(dir), ec};
        if (ec) return entries;
        const std::filesystem::directory_iterator end;
        for (; iter != end; iter.increment(ec)) {
            if (ec) break;
            const std::filesystem::directory_entry& entry = *iter;
            std::error_code entry_ec;
            const std::uintmax_t size = entry.file_size(entry_ec);
            if (entry_ec) continue;
            const std::filesystem::file_time_type write_time = entry.last_write_time(entry_ec);
            if (entry_ec) continue;
            // 文件名按 UTF-8 字节比较（平台层的路径就是 UTF-8 std::string）
            const std::u8string u8 = entry.path().filename().u8string();
            std::string name(reinterpret_cast<const char*>(u8.data()), u8.size());
            Stamp stamp;
            stamp.size = size;
            stamp.mtime = static_cast<std::int64_t>(write_time.time_since_epoch().count());
            entries.emplace(LowerAsciiPath(name), stamp);
        }
        return entries;
    }

    std::map<std::string, std::map<std::string, Stamp>> dirs_;
};

}  // namespace

std::string LspServer::ProjectSignature(const project::Project& project) {
    // 只需要"变了没变"，所以用 (路径, 版本或 mtime+size) 拼串即可，不做哈希。
    std::string signature = project.al_path;
    std::vector<std::string> files = hdl::VerilogFilesFor(project);
    for (const std::string& path : hdl::VhdlFilesFor(project)) files.push_back(path);
    signature.reserve(signature.size() + files.size() * 48);

    // 没有打开任何文档时不必逐文件问 DocumentStore（省掉 2 次字符串构造 + 加锁 + 查表）。
    const bool check_buffers = docs_.Size() > 0;
    DirStampCache stamps;

    for (const std::string& path : files) {
        signature += '\n';
        signature += path;
        signature += '|';
        if (check_buffers) {
            if (const auto version = docs_.GetVersion(platform::path_to_uri(path));
                version.has_value()) {
                signature += 'v';
                signature += std::to_string(*version);
                continue;
            }
        }
        if (const DirStampCache::Stamp* stamp = stamps.Find(path); stamp != nullptr) {
            signature += std::to_string(stamp->size);
            signature += ':';
            signature += std::to_string(stamp->mtime);
            continue;
        }
        // 回落：目录枚举不可用时的原路径（行为与 B8 之前完全一致）
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
    // Phase 2 / A1：IndexService 内部做增量更新；这里只负责把工程模型交给它。
    // 快照是不可变的，调用方可以在锁外安全查询。
    // C1：SetProject 按 const& 接收，这里只发生一次拷贝（交给 state_.project）。
    index_service_->SetProject(project);
    std::shared_ptr<const hdl::HdlIndex> snapshot = index_service_->Get();
    if (truncated != nullptr) *truncated = index_service_->truncated();
    if (parse_errors != nullptr) *parse_errors = index_service_->parse_errors();
    if (!snapshot) snapshot = std::make_shared<hdl::HdlIndex>();
    return snapshot;
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
    // Phase 2 / A1：用未保存缓冲区更新索引（只重解析这一份文件）
    // C1：走 shared 访问器 —— 缓存命中时零 Project 拷贝，SetProject 只为 state_ 拷一份。
    if (const std::shared_ptr<const project::Project> project = ProjectForPathShared(path);
        project != nullptr) {
        index_service_->SetProject(*project);
    }
    index_service_->OnFileChanged(path, jsonrpc::GetString(document, "text"));
    RunDiagnostics(path, false);
    // T1：didOpen 之后也排一次 lint（打开文件就该看到波浪线）
    ScheduleLint(path, uri);
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
    // Phase 2 / A1：先确保工程已绑定，否则首个 didChange 会被 IndexService 当作"非工程文件"
    // C1：shared 访问器 + const& SetProject = 每请求只剩 state_.project 那一份拷贝。
    if (const std::shared_ptr<const project::Project> project = ProjectForPathShared(path);
        project != nullptr) {
        index_service_->SetProject(*project);
    }
    // Phase 2 / A1：增量更新该文件的索引快照
    if (const auto text = docs_.GetText(uri); text.has_value()) {
        index_service_->OnFileChanged(path, *text);
    }
    RunDiagnostics(path, false);
    // T1：按键路径**只入队**（加锁 + notify，~µs），lint 在 worker 上算，绝不在这里跑 Verible。
    ScheduleLint(path, uri);
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

    // Phase 2 / A1：保存后基于缓冲区（includeText=true）增量更新索引
    // C1：shared 访问器 + const& SetProject = 每请求只剩 state_.project 那一份拷贝。
    if (const std::shared_ptr<const project::Project> project = ProjectForPathShared(path);
        project != nullptr) {
        index_service_->SetProject(*project);
    }
    if (const auto buffer = docs_.GetText(uri); buffer.has_value()) {
        index_service_->OnFileChanged(path, *buffer);
    } else {
        index_service_->OnFileChanged(path, std::nullopt);
    }
    MaybeNotifyIndexChanged({path});

    RunDiagnostics(path, true);

    // T1：保存也排一次 lint（相当于"停手"信号，通常随后就会去抖开跑）
    ScheduleLint(path, uri);

    // 0.10.28 的行为：保存时**立即**清空该文件的 TD 诊断，然后**只排队一次**重跑。
    //   * 先清 —— 否则"改好了但旧错误还挂着"，用户会以为没生效
    //   * 再排队 —— 若上一轮还在跑，只置一个 pending 标志（不叠加线程）
    if (settings_.td_check_on_save && settings_.enable_td) {
        // C1：ClearTdPath / ScheduleTdCheck 都只读工程（TdService 内部自己拷一份），
        // 用 shared 访问器省掉这次深拷贝；shared_ptr 持有到两次调用结束。
        if (const std::shared_ptr<const project::Project> project = ProjectForPathShared(path);
            project != nullptr) {
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
    // Phase 2 / A1：关闭后回落到磁盘内容（丢弃未保存缓冲区）
    index_service_->OnFileChanged(path, std::nullopt);
    // T1：取消该文件的 lint 调度，并丢掉 lint 诊断（在跑的那轮结果也会作废）。
    if (lint_service_) lint_service_->Cancel(path);
    {
        std::scoped_lock lock(state_mutex_);
        diag_lint_.erase(path);
    }
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

namespace {

bool SameDiagnosticList(const lsp::DiagnosticList& left, const lsp::DiagnosticList& right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        const lsp::Diagnostic& a = left[i];
        const lsp::Diagnostic& b = right[i];
        if (a.range.start.line != b.range.start.line ||
            a.range.start.character != b.range.start.character ||
            a.range.end.line != b.range.end.line ||
            a.range.end.character != b.range.end.character || a.severity != b.severity ||
            a.code != b.code || a.message != b.message) {
            return false;
        }
    }
    return true;
}

// Phase 2 / B2：只发布发生变化的文件（新增/删除/内容变化）。
std::vector<std::string> ChangedDiagnosticFiles(
    const std::map<std::string, lsp::DiagnosticList>& before,
    const std::map<std::string, lsp::DiagnosticList>& after) {
    std::vector<std::string> changed;
    for (const auto& [path, list] : after) {
        const auto it = before.find(path);
        if (it == before.end() || !SameDiagnosticList(it->second, list)) changed.push_back(path);
    }
    for (const auto& [path, list] : before) {
        (void)list;
        if (!after.contains(path)) changed.push_back(path);
    }
    return changed;
}

}  // namespace

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
    platform::default_logger().info(message);
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

    // A9：把**这次真正发出去的**数组同步进 pull 缓存（push 行为一行未改）。
    // 空数组也要存：pull 必须能反映"诊断已被清空"，而不是继续返回上次的旧条目。
    {
        std::scoped_lock lock(state_mutex_);
        latest_diagnostics_[uri] = wire;
    }

    Json params = Json::object();
    params["uri"] = uri;
    params["diagnostics"] = std::move(wire);
    if (const auto version = docs_.GetVersion(uri); version.has_value()) {
        params["version"] = *version;
    }
    SendNotification("textDocument/publishDiagnostics", std::move(params));
}

// =============================================================================
//  诊断发布：按来源合并（T1 / task-50）
// =============================================================================
//  两张表（diag_lint_ / diag_td_）**永不互相写**：本函数只做只读合并。
//  红线：lint 不能覆盖 TD、TD 不能覆盖 lint —— 单测见 tests/unit/lint_service_test.cpp。
lsp::DiagnosticList MergeDiagnosticSources(const lsp::DiagnosticList& lint,
                                           const lsp::DiagnosticList& td) {
    lsp::DiagnosticList merged;
    merged.reserve(lint.size() + td.size());
    merged.insert(merged.end(), lint.begin(), lint.end());
    merged.insert(merged.end(), td.begin(), td.end());

    // 稳定排序：severity -> line -> character -> source -> code -> message。
    // 为什么 severity 放第一位：这是 2026-11-19 之前（只有 TD 诊断时）的既有顺序，
    // tests/e2e/lsp_smoke.py 有断言锁着它；lint 并进来之后保持同样的可读性
    // （error 永远在最前），也让通知内容可回归比较。
    std::ranges::stable_sort(merged,
                     [](const lsp::Diagnostic& a, const lsp::Diagnostic& b) {
                         if (a.severity != b.severity) return a.severity < b.severity;
                         if (a.range.start.line != b.range.start.line) {
                             return a.range.start.line < b.range.start.line;
                         }
                         if (a.range.start.character != b.range.start.character) {
                             return a.range.start.character < b.range.start.character;
                         }
                         if (a.source != b.source) return a.source < b.source;
                         if (a.code != b.code) return a.code < b.code;
                         return a.message < b.message;
                     });

    // 完全相同的条目（同 range/severity/code/source/message）只留一条；
    // 不同来源即使文案相同也都会保留（source 不同 -> 不去重）。
    const auto same = [](const lsp::Diagnostic& a, const lsp::Diagnostic& b) {
        return a.range.start.line == b.range.start.line &&
               a.range.start.character == b.range.start.character &&
               a.range.end.line == b.range.end.line &&
               a.range.end.character == b.range.end.character && a.severity == b.severity &&
               a.code == b.code && a.source == b.source && a.message == b.message;
    };
    merged.erase(std::ranges::unique(merged, same).begin(), merged.end());
    return merged;
}

// B-B8：工程自身的诊断（.al 里记了但磁盘上没有的文件、解析告警）。
// 这一条以前只写在 fpga_info 的 warnings 计数里，编辑器 Problems 完全看不见 ——
// 用户看到的是"0 error"，而 elaborate 根本没跑（PYTHON-BUGS P-1 同族）。
lsp::DiagnosticList ProjectDiagnostics(const project::Project& project) {
    lsp::DiagnosticList out;
    const auto rel_or = [](const project::AlFile& file) -> std::string {
        return file.rel.empty() ? file.path : file.rel;
    };

    int missing = 0;
    for (const project::AlFile& file : project.files) {
        if (file.exists || !file.enabled || file.excluded) continue;
        ++missing;
        lsp::Diagnostic diagnostic;
        diagnostic.severity = lsp::kError;
        diagnostic.code = "AL-MISSING-FILE";
        diagnostic.source = "fpga-lsp:project";
        diagnostic.message = "File listed in .al but missing on disk: " + rel_or(file);
        diagnostic.range.start = lsp::Position{.line=0, .character=0};
        diagnostic.range.end = lsp::Position{.line=0, .character=1};
        lsp::Json data = lsp::Json::object();
        data["origin"] = "al";
        data["rel"] = file.rel;
        diagnostic.data = std::move(data);
        out.push_back(std::move(diagnostic));
        if (out.size() >= 20) break;  // 工程级诊断不刷屏
    }
    (void)missing;

    for (const std::string& warning : project.warnings) {
        if (warning.empty()) continue;
        lsp::Diagnostic diagnostic;
        diagnostic.severity = lsp::kWarning;
        diagnostic.code = "AL-WARNING";
        diagnostic.source = "fpga-lsp:project";
        diagnostic.message = warning;
        diagnostic.range.start = lsp::Position{.line=0, .character=0};
        diagnostic.range.end = lsp::Position{.line=0, .character=1};
        out.push_back(std::move(diagnostic));
        if (out.size() >= 40) break;
    }
    return out;
}

void LspServer::PublishMergedPath(const project::Project* project, const std::string& path) {
    lsp::DiagnosticList lint;
    lsp::DiagnosticList td;
    {
        std::scoped_lock lock(state_mutex_);
        if (const auto it = diag_lint_.find(path); it != diag_lint_.end()) lint = it->second;
        if (project != nullptr) {
            if (const auto table = diag_td_.find(project->al_path); table != diag_td_.end()) {
                if (const auto it = table->second.find(path); it != table->second.end()) {
                    td = it->second;
                }
            }
            // B-B4：DRC/CDC 违例（runAnalysis 的结果）
            if (const auto table = diag_analysis_.find(project->al_path);
                table != diag_analysis_.end()) {
                if (const auto it = table->second.find(path); it != table->second.end()) {
                    td.insert(td.end(), it->second.begin(), it->second.end());
                }
            }
        }
    }
    // B-B8：.al 本身的诊断（缺文件 / 解析告警）与 lint、TD 一起合并发布。
    lsp::DiagnosticList project_diags;
    if (project != nullptr && path == project->al_path) {
        project_diags = ProjectDiagnostics(*project);
    }

    // B-B1：.adc / .sdc 的诊断以前只有 MCP 工具能看见（Problems 一条都不出）。
    // 这里接第三个来源；端口表用 HDL 索引（与 MCP 的 fpga_diagnostics 同源）。
    if (project != nullptr && IsConstraintDocument(path)) {
        if (const std::optional<std::string> text = LoadText(path); text.has_value()) {
            std::vector<constraints::Port> ports;
            if (!project->top.empty()) {
                if (const std::shared_ptr<const hdl::HdlIndex> index =
                        IndexForProject(*project, nullptr, nullptr);
                    index != nullptr) {
                    if (const hdl::Module* module = index->ModuleByName(project->top);
                        module != nullptr) {
                        for (const hdl::Port& port : module->ports) {
                            constraints::Port converted;
                            converted.name = port.name;
                            converted.direction = port.direction;
                            converted.width = port.width;
                            converted.msb = port.msb;
                            converted.lsb = port.lsb;
                            converted.line = port.line;
                            converted.character = port.character;
                            converted.file = port.file;
                            ports.push_back(std::move(converted));
                        }
                    }
                }
            }
            const lsp::DiagnosticList constraint_diags =
                ConstraintDiagnostics(path, *text, ports);
            project_diags.insert(project_diags.end(), constraint_diags.begin(),
                                 constraint_diags.end());
        }
    }
    if (lint.empty() && td.empty() && project_diags.empty()) {
        PublishDiagnostics(path, lsp::DiagnosticList{});
        return;
    }
    PublishDiagnostics(path, MergeDiagnosticSources(MergeDiagnosticSources(lint, td),
                                                    project_diags));
}

void LspServer::PublishMerged(const project::Project& project, const std::string* only_path) {
    std::map<std::string, lsp::DiagnosticList> table;
    std::vector<std::string> paths;
    {
        std::scoped_lock lock(state_mutex_);
        if (const auto it = diag_td_.find(project.al_path); it != diag_td_.end()) {
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
        // 注意：即使 TD 表里没有这个文件，也要走 PublishMergedPath ——
        // 它可能只有 lint 诊断（T1 之前这里会直接发空数组，把 lint 清掉）。
        paths = {*only_path};
    }

    for (const auto& path : paths) {
        PublishMergedPath(&project, path);
    }
    (void)table;
}

void LspServer::ClearTdPath(const project::Project& project, std::string_view path) {
    bool changed = false;
    {
        std::scoped_lock lock(state_mutex_);
        if (auto it = diag_td_.find(project.al_path); it != diag_td_.end()) {
            if (it->second.erase(std::string(path)) > 0) changed = true;
        }
    }
    if (!changed) return;

    const std::string key(path);
    PublishMerged(project, &key);
}

void LspServer::RunDiagnostics(std::string_view path, bool publish_all) {
    // C1：这是 didChange 每敲一次字都会走的路径 —— 用 shared 访问器，避免每次
    // 为了发布诊断再深拷贝一份工程模型（PublishMerged 只读，shared_ptr 持有到调用结束）。
    if (const std::shared_ptr<const project::Project> project = ProjectForPathShared(path);
        project != nullptr) {
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
//  T1 / task-50：服务端 lint（内嵌 Verible）
// =============================================================================

namespace {

bool IsHdlTextPath(std::string_view lower_path) {
    const auto ends_with = [&](std::string_view suffix) {
        return lower_path.size() >= suffix.size() &&
               lower_path.ends_with(suffix);
    };
    return ends_with(".v") || ends_with(".sv") || ends_with(".vh") || ends_with(".svh");
}

std::string LowerPath(std::string_view path) {
    std::string out(path);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

}  // namespace

void LspServer::ScheduleLint(const std::string& path, const std::string& uri) {
    if (!settings_.lint_on_change || !lint_service_) return;
    if (!IsHdlTextPath(LowerPath(path))) return;   // .adc/.sdc 不走 Verilog lint
    if (!uri.empty() && !docs_.IsOpen(uri)) return;  // 只 lint 已打开文档
    // 非阻塞：只入队 + 去抖；真正算在 LintService 的 worker 线程上（0.3 ms 级）。
    lint_service_->Request(path, uri);
}

lsp::DiagnosticList LspServer::ComputeLint(const std::string& path) {
#if defined(FPGA_LSP_HAVE_VERIBLE_BRIDGE)
    // 关键：**在入口取文档快照**（LoadText -> DocumentStore 的值拷贝）。
    // worker 线程后续只读这份拷贝，不会和 IO 线程的 didChange 撕裂。
    const std::optional<std::string> loaded = LoadText(path);
    if (!loaded.has_value() || loaded->empty()) return {};
    const std::string& text = *loaded;

    verible_bridge::LintResult result =
        verible_bridge::LintText(text, path, settings_.verible_lint_args);
    if (!result.unknown_flags.empty()) {
        // 认不出的 lint flag 只记一次日志（用户可能手写了过时参数）
        std::string joined;
        for (const std::string& flag : result.unknown_flags) {
            if (!joined.empty()) joined += ' ';
            joined += flag;
        }
        platform::log_warn("lint: ignoring unknown arguments " + joined);
    }
    if (!result.ok) return {};

    // 与旧插件等价：注释里的 no-tabs 不报（代码里的照报）。
    std::vector<verible_bridge::LintDiagnostic> violations =
        verible_bridge::DropCommentNoTabs(text, std::move(result.diagnostics));

    constexpr std::size_t kMaxLintDiagnostics = 1000;  // 安全上限（防病态文件刷爆通知）
    lsp::DiagnosticList diagnostics;
    diagnostics.reserve(std::min(violations.size(), kMaxLintDiagnostics));
    for (const verible_bridge::LintDiagnostic& item : violations) {
        if (diagnostics.size() >= kMaxLintDiagnostics) break;
        // Verible 的列是**码点**列（0-based）；LSP 要 UTF-16 码元列。
        const std::string_view line = platform::line_at(text, static_cast<std::size_t>(item.line));
        const int start = static_cast<int>(platform::codepoint_index_to_utf16_column(
            line, static_cast<std::size_t>(std::max(0, item.column))));
        int end = start + 1;
        if (item.end_line == item.line) {
            const int raw_end = static_cast<int>(platform::codepoint_index_to_utf16_column(
                line, static_cast<std::size_t>(std::max(0, item.end_column))));
            end = std::max(raw_end, start + 1);
        }
        lsp::Diagnostic diagnostic =
            lsp::Diagnostic::Span(item.line, start, item.line, end, lsp::kWarning, item.rule,
                                  "fpga-lsp:verible", item.message);
        diagnostic.WithBackend("verible").WithData("origin", "verible");
        diagnostics.push_back(std::move(diagnostic));
    }
    return diagnostics;
#else
    (void)path;
    return {};
#endif
}

void LspServer::OnLintResult(const std::string& path, lsp::DiagnosticList diagnostics) {
    {
        std::scoped_lock lock(state_mutex_);
        if (shutting_down_) return;
        // 只写 lint 表：TD 表保持不动（这就是"互不覆盖"的落地）。
        diag_lint_[path] = std::move(diagnostics);
    }
    // 发布该文件的并集（lint + TD）。找不到工程时只发 lint。
    // C1：PublishMergedPath 只读工程 -> shared 访问器；shared_ptr 持有到调用返回。
    const std::shared_ptr<const project::Project> owner = ProjectForPathShared(path);
    PublishMergedPath(owner != nullptr ? owner.get() : nullptr, path);
}

// =============================================================================
//  TD 检查
// =============================================================================

void LspServer::ScheduleTdCheck(const project::Project& project) {
    // W3：门禁放在这里（TdService 不认识 settings）。文案与旧实现一致。
    if (!settings_.enable_td) {
        LogMessage(2, "TD check skipped: fpgaLsp.enableTd = false");
        return;
    }
    {
        std::scoped_lock lock(state_mutex_);
        if (shutting_down_) return;
    }
    if (!td_service_) return;
    // 非阻塞：300 ms 去抖内合并；若已有检查在跑，TdService 会取消它（kill 进程树）
    // 并只保留最后一次请求 —— 连续保存只跑 1 轮。
    td_service_->Request(project);
}

// W3：一轮 TD 检查的结果落地（TdService 的 worker 线程调用）。
// 语义与旧的 RunTdCheckOnce 尾部逐字对齐：失败清表 + 全量发布；成功只发布变更文件；
// 日志格式不变（"TD check complete: N problem(s) in X.XXs"）。
void LspServer::ApplyTdResult(const project::Project& project, const td::TdCheckResult& result) {
    if (result.cancelled) return;  // 防御：TdService 已保证被取消的轮次不回调

    // 保留旧的失败文案：工厂拿不到 TD 时给出与 CreateSession 相同的提示。
    std::string error = result.error;
    if (error == "no TD check runner available") {
        error = "No TangDynasty installation found (set fpgaLsp.tdPath if it is not on PATH)";
    }

    if (result.failed) {
        // 失败必须清掉旧表，否则上一版诊断会永远挂在 Problems 里。
        {
            std::scoped_lock lock(state_mutex_);
            diag_td_.erase(project.al_path);
        }
        PublishMerged(project, nullptr);
        LogMessage(1, "TD check failed: " + error);
        return;
    }

    std::map<std::string, lsp::DiagnosticList> before;
    {
        std::scoped_lock lock(state_mutex_);
        if (shutting_down_) return;
        before = diag_td_[project.al_path];
        diag_td_[project.al_path] = result.by_file;
    }
    // 只发布**变更**的文件（与旧实现同一个 ChangedDiagnosticFiles 口径），
    // 避免每次保存整工程 Problems 闪烁。
    for (const std::string& path : ChangedDiagnosticFiles(before, result.by_file)) {
        PublishMerged(project, &path);
    }

    // 旧实现用"从建会话到发布的墙钟"，这里用 TD 轮次内部耗时（startup+import+read_hdl+parse），
    // 差 ~1 ms（我们的分组/发布时间），格式与文案不变。
    LogMessage(3, std::format("TD check complete: {} problem(s) in {:.2f}s", result.total(),
                              result.timings.total_ms / 1000.0));
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
            std::scoped_lock lock(state_mutex_);
            diag_td_.erase(project.al_path);
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
        std::scoped_lock lock(session_mutex_);
        if (session_ == session) session_.reset();
    }

    // 若这轮检查是被 Shutdown（exit/EOF）中断的，输出必然被截断：
    // 绝不能把"读了一半的空结果"当成"这个工程没有问题"发布出去。
    // （实测：exit 时 Kill 会话，read_hdl 的输出会截断成空 → 会误报 0 条诊断。）
    {
        std::scoped_lock lock(state_mutex_);
        if (shutting_down_) {
            platform::default_logger().info("TD check interrupted by shutdown; skipping publish");
            return;
        }
    }

    if (!failure.empty()) {
        {
            std::scoped_lock lock(state_mutex_);
            diag_td_.erase(project.al_path);
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

    std::map<std::string, lsp::DiagnosticList> before;
    {
        std::scoped_lock lock(state_mutex_);
        before = diag_td_[project.al_path];
        diag_td_[project.al_path] = table;
    }
    // Phase 2 / B2：只发变更文件，避免每次保存整工程 Problems 闪烁。
    for (const std::string& path : ChangedDiagnosticFiles(before, table)) {
        PublishMerged(project, &path);
    }

    const double elapsed = std::chrono::duration<double>(clock_type::now() - started).count();
    LogMessage(3, std::format("TD check complete: {} problem(s) in {:.2f}s", total, elapsed));
}

// =============================================================================
//  TD 会话
// =============================================================================

bool LspServer::SessionAlive() const {
    std::scoped_lock lock(session_mutex_);
    return session_ != nullptr && session_->Alive();
}

void LspServer::KillSession() {
    std::shared_ptr<td::TdSession> session;
    {
        std::scoped_lock lock(session_mutex_);
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
    // 会话 cwd 必须是 `.al` 所在目录（相对路径才解析得对），而 TD 会把
    // `td_<时间戳>.log/.logw`、`minidump-*.dmp` 写进 cwd —— 于是短会话日志落在
    // **工程根目录**（2026-11-22 运行期审计实测：LSP 的 `fpgalsp.device` 每次都在
    // `%WORKSPACE%\CorpusAlpha` 留 30 KB 日志）。GUI 自己把它们放在 `<al>/log/`，flow / probe /
    // MCP bridge 三条路径也都已经传了 log_dir，这里补齐 LSP 这条。
    if (!project.dir.empty()) options.log_dir = platform::path_join(project.dir, "log");

    try {
        auto session = std::make_shared<td::TdSession>(*choice, options);
        {
            std::scoped_lock lock(session_mutex_);
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

std::shared_ptr<const project::Project> LspServer::ParseAlCachedShared(
    const std::string& al_path) {
    // 对应 Python project.parse_al_cached：按 .al 的 (mtime, size) 决定是否重新解析。
    // C1：缓存条目持有 shared_ptr<const Project>，命缓存时只拷指针（零深拷贝）。
    std::error_code ec;
    const std::filesystem::path al_file(al_path);
    const std::uintmax_t size = std::filesystem::file_size(al_file, ec);
    if (ec) {  // .al 不存在/不可读：不缓存，保持原行为
        return std::make_shared<const project::Project>(project::ParseAl(al_path));
    }
    const std::filesystem::file_time_type write_time =
        std::filesystem::last_write_time(al_file, ec);
    if (ec) return std::make_shared<const project::Project>(project::ParseAl(al_path));
    const std::int64_t mtime =
        static_cast<std::int64_t>(write_time.time_since_epoch().count());

    {
        std::scoped_lock lock(project_cache_mutex_);
        const auto it = project_cache_.find(al_path);
        if (it != project_cache_.end() && it->second.mtime == mtime &&
            it->second.size == size) {
            // 锁内只拷贝 shared_ptr（引用计数 +1），返回后对象由调用方共同持有。
            return it->second.project;
        }
    }

    // 解析仍然在锁外（与旧实现一致：锁只保护缓存表）。
    std::shared_ptr<const project::Project> parsed =
        std::make_shared<const project::Project>(project::ParseAl(al_path));
    {
        std::scoped_lock lock(project_cache_mutex_);
        if (project_cache_.size() >= 8) project_cache_.clear();
        ParsedProjectEntry entry;
        entry.mtime = mtime;
        entry.size = size;
        entry.project = parsed;  // 共享同一份模型：不再为缓存拷一份
        project_cache_[al_path] = std::move(entry);
    }
    return parsed;
}

std::optional<project::Project> LspServer::ParseAlCached(const std::string& al_path) {
    // 旧的值语义入口：签名/行为保持不变（调用方拿到一份独立的副本）。
    const std::shared_ptr<const project::Project> parsed = ParseAlCachedShared(al_path);
    if (parsed == nullptr) return std::nullopt;  // 防御：正常路径不会发生
    return *parsed;
}

std::optional<project::Project> LspServer::ProjectForPath(std::string_view path) {
    // 值语义包装：保留给既有调用者；每请求热路径改用 ProjectForPathShared。
    const std::shared_ptr<const project::Project> project = ProjectForPathShared(path);
    if (project == nullptr) return std::nullopt;
    return *project;  // 1 份拷贝（旧签名要求按值返回）
}

std::shared_ptr<const project::Project> LspServer::ProjectForPathShared(std::string_view path) {
    if (path.empty()) return nullptr;

    const std::string from_env = ReadEnv("FPGALSP_PROJECT");

    // 对应 Python: settings.project_file or project_override or os.environ["FPGALSP_PROJECT"]
    const std::string configured = !settings_.project_file.empty() ? settings_.project_file
                                 : !project_override_.empty()      ? project_override_
                                                                   : from_env;

    const std::string root = RootPath();

    // 查找结果做 5 秒记忆（对应 Python discover_projects 的 5s 缓存）。
    // 为什么必须缓存：didChange 每敲一个字都会走这里；工程上方没有 .al 时
    // FindProjectAl 会递归扫工作区根 —— 不缓存会让大工作区打字变卡。
    //
    // F2/R3b：键从 `path` 改成 `dirname(path)`。同一目录下的文件发现结果必然相同
    // （`FindAl` 只看向上 6 层目录；`DiscoverProjects` + `Affinity` 对同目录文件给
    // 相同 tier/分数 —— Affinity 只比较目录前缀），所以按目录键不改变结果，但把
    // "每个文件一次" 变成 "每个目录一次"。
    // 特例：path == root（CurrentProject）时用 root 本身做键，避免退化成父目录。
    const std::string normalized_path = platform::normalize_path(path);
    const std::string scope =
        (normalized_path == root) ? root : platform::path_dirname(normalized_path);
    const std::string key = configured + "|" + root + "|" + scope;
    const auto now = clock_type::now();
    {
        std::scoped_lock lock(discovery_mutex_);
        if (const auto it = discovery_cache_.find(key); it != discovery_cache_.end()) {
            if (now - it->second.at < std::chrono::seconds(5)) {
                it->second.last_used = now;
                if (it->second.al.empty()) return nullptr;
                // 与旧实现一样在发现锁内解析（锁序不变）；命缓存只拷 shared_ptr。
                return ParseAlCachedShared(it->second.al);
            }
        }
    }

    const auto al = ResolveProjectAl(root, normalized_path, configured);
    {
        std::scoped_lock lock(discovery_mutex_);
        // LRU 淘汰（原来"≥64 条整体清空"会让 200 文件工程周期性全失效）
        if (discovery_cache_.size() >= kDiscoveryCapacity) {
            auto victim = discovery_cache_.begin();
            for (auto it = discovery_cache_.begin(); it != discovery_cache_.end(); ++it) {
                if (it->second.last_used < victim->second.last_used) victim = it;
            }
            discovery_cache_.erase(victim);
        }
        DiscoveryEntry entry;
        entry.at = now;
        entry.last_used = now;
        entry.al = al.has_value() ? *al : std::string();
        discovery_cache_[key] = std::move(entry);
    }
    if (!al.has_value()) return nullptr;
    return ParseAlCachedShared(*al);
}

std::optional<std::string> LspServer::ResolveProjectAl(const std::string& root,
                                                       const std::string& path,
                                                       const std::string& configured) {
    // 1) 显式配置优先（与 project::FindProjectAl 第一步逐字一致）
    if (!configured.empty()) {
        const std::string al = platform::normalize_path(configured);
        return project::DefaultFileExists(al) ? std::optional<std::string>(al) : std::nullopt;
    }

    // 2) 向上找
    if (auto al = project::FindAl(path)) return al;

    // 3) 工作区递归扫描 + 亲和度（F2：扫描结果按 root 缓存 5 s）
    if (root.empty()) return std::nullopt;
    std::vector<std::string> candidates;
    {
        std::scoped_lock lock(discovery_mutex_);
        const auto it = discovered_cache_.find(root);
        if (it != discovered_cache_.end() && clock_type::now() - it->second.at < std::chrono::seconds(5)) {
            candidates = it->second.candidates;  // 拷贝：锁外做 affinity
        }
    }
    if (candidates.empty()) {
        candidates = project::DiscoverProjects(root);
        std::scoped_lock lock(discovery_mutex_);
        discovered_cache_[root] = DiscoveredProjectsEntry{.at=clock_type::now(), .candidates=candidates};
    }
    if (candidates.empty()) return std::nullopt;
    if (candidates.size() == 1) return candidates[0];

    std::string best = candidates[0];
    auto best_score = project::Affinity(best, path);
    for (std::size_t i = 1; i < candidates.size(); ++i) {
        const auto score = project::Affinity(candidates[i], path);
        if (score > best_score) {
            best = candidates[i];
            best_score = score;
        }
    }
    return best;
}

std::shared_ptr<const project::Project> LspServer::CurrentProjectShared() {
    const std::string root = RootPath();
    if (root.empty()) return nullptr;
    return ProjectForPathShared(root);
}

std::optional<project::Project> LspServer::CurrentProject() {
    // 值语义入口（签名不变）：只有这一处拷贝，保留给需要独立副本/长期持有的调用者。
    const std::shared_ptr<const project::Project> project = CurrentProjectShared();
    if (project == nullptr) return std::nullopt;
    return *project;
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
        std::scoped_lock lock(state_mutex_);
        diag_td_.clear();
        published_.clear();
    }
    {
        // 工程切换后旧索引不再有意义（键是 al_path，留着也只是占内存）
        std::scoped_lock lock(hdl_mutex_);
        hdl_cache_.clear();
    }
    {
        // 同上：已解析的 Project 也一起丢（下次按 (mtime, size) 重新解析）
        std::scoped_lock lock(project_cache_mutex_);
        project_cache_.clear();
    }
    {
        // F2/R3b：工作区扫描结果与按目录的发现缓存也一起失效
        std::scoped_lock lock(discovery_mutex_);
        discovery_cache_.clear();
        discovered_cache_.clear();
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
    std::scoped_lock lock(flow_mutex_);
    return flow_;
}

std::shared_ptr<flow::FlowRunner> LspServer::EnsureFlowRunner(const project::Project& project,
                                                             const td::TdChoice& td) {
    std::scoped_lock lock(flow_mutex_);
    // 同一个 .al 复用 runner；工程变了必须重建，否则 flow 会跑到旧工程上
    if (flow_ && flow_->project().al_path == project.al_path) return flow_;

    flow_ = std::make_shared<flow::FlowRunner>(
        project, td,
        // 通知：直接发 fpgaLsp/flowEvent（transport 写操作线程安全，可从 worker 线程调用）
        [this, project](const Json& event) {
            SendFlowEvent(event);
            // P-D7：flow 刚写完报告时，watcher 的 glob 不覆盖 *_Runs/**（只有 .v/.sv/.vhd/
            // .adc/.sdc/.al），所以 reportsChanged 只能靠这里主动检查一次。
            // CheckForChanges 只 stat 已知报告文件，代价 ≈1 ms（审计 REPORTS.md）。
            const std::string type = jsonrpc::GetString(event, "type");
            if ((type == "done" || type == "error") && report_store_ != nullptr) {
                if (const std::shared_ptr<const project::Project> current = CurrentProjectShared();
                    current != nullptr && current->al_path == project.al_path) {
                    if (report_store_->CheckForChanges(*current)) {
                        SendReportsChanged(*current, {});
                    }
                }
            }
        },
        // 日志：走 window/logMessage（同样线程安全）
        [this](int type, std::string_view text) { LogMessage(type, text); });
    platform::default_logger().info("flow runner created: " + project.al_path);
    return flow_;
}

}  // namespace fpgalsp::server
