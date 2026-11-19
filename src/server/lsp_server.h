// =============================================================================
//  src/server/lsp_server.h —— LSP 服务主体（把各层接成"编辑器可用"的闭环）
// =============================================================================
//  对应参考实现：Y:\fpga-lsp\fpgalsp\server.py :: FpgaLanguageServer
//
//  本层只做**编排**，不重复实现下层语义：
//    * 文档缓冲            -> lsp::DocumentStore（UTF-16 增量同步）
//    * 工程模型            -> project::{ParseAl,FindProjectAl,DiscoverProjects}
//    * 诊断形状            -> lsp::Diagnostic（所有来源唯一形状）
//    * TD 诊断             -> td::RunReadHdl（内部已做 ensure_device）
//    * 传输                -> jsonrpc::TransportBaseCommon（写操作线程安全）
//
//  线程模型（与 Python 的 daemon 线程等价，但改为可 join 的显式线程）：
//    * 主线程：只做 transport 读 + handler 分派（单线程）
//    * TD 检查线程：最多一个。`fpgalsp.check` / `didSave` 触发。
//      TD 检查期间再次触发不排队第二条线程，只置一个 pending 标志，
//      当前这轮结束后**再跑一轮**（复现 0.10.28 的"只排队一次"）。
//    * 诊断发布在 TD 线程里直接写 transport（WriteMessage 内部持锁）。
//
//  ⚠ read_hdl 会让 TD 关闭 stdout 管道（见 td_session.h 的硬约束），
//    所以每次 TD 检查都新建会话，检查结束立即 Kill —— 不跨检查复用会话。
// =============================================================================

#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "jsonrpc/dispatcher.h"
#include "jsonrpc/transport.h"
#include "lsp/diagnostic.h"
#include "lsp/document_store.h"
#include "project/al_project.h"
#include "td/td_service.h"
#include "flow/flow_runner.h"  // P-C4：SliceFlowLog 需要 FlowState 的完整定义
#include "server/index_service.h"
#include "server/lint_service.h"
#include "server/report_store.h"

namespace fpgalsp::td {
class TdSession;
struct TdChoice;
}

namespace fpgalsp::hdl {
class HdlIndex;
class ParseCache;
}  // namespace fpgalsp::hdl

namespace fpgalsp::flow {
class FlowRunner;
}  // namespace fpgalsp::flow

namespace fpgalsp::server {

using Json = nlohmann::json;
using jsonrpc::HandlerResult;

// -----------------------------------------------------------------------------
// 配置（对应 server.py 的 _Settings；键名逐字一致）
// -----------------------------------------------------------------------------
struct Settings {
    bool td_check_on_save = true;
    bool enable_td = true;
    std::string project_file;
    std::string td_path;

    // Verible 相关：本阶段只解析（格式化/lint 桥接是独立任务），保留键名以免客户端
    // 发来后被当成未知配置。
    std::string verible_path;
    std::vector<std::string> verible_args;
    bool verible_format = true;
    bool format_fallback = true;
    // T1：didChange 之后在服务端跑内嵌 Verible lint（默认开）。
    // 关掉后行为退回"只有 TD 诊断"（排查 lint 相关问题时用）。
    bool lint_on_change = true;

    // 对应 _Settings.update：接受 {"fpgaLsp": {...}} 或直接 {...}
    void Update(const Json& options);
};

// 器件摘要（fpgalsp.info / fpgalsp.device 用）
struct DeviceSummary {
    std::string device;
    std::string family;
    std::optional<std::string> speed;
    std::string db;
    std::string package;
    std::size_t pins = 0;
};

// -----------------------------------------------------------------------------
// LspServer
// -----------------------------------------------------------------------------
class LspServer {
public:
    // td_explicit 对应 Python 的 td_explicit（命令行 --td；这里由 main 传入）
    explicit LspServer(jsonrpc::TransportBaseCommon& transport, std::string td_explicit = {});
    ~LspServer();

    LspServer(const LspServer&) = delete;
    LspServer& operator=(const LspServer&) = delete;

    // 注册所有 handler（标准 LSP + fpgaLsp/* + workspace/executeCommand）。
    void RegisterAll(jsonrpc::Dispatcher& dispatcher);

    // 关闭 TD 会话并停止 TD 线程。可重复调用（幂等）。
    // main 在 exit/EOF/异常三种路径上都必须调用它 —— 否则会留孤儿 TD 进程。
    void Shutdown();

    // flow 引擎（src/flow）：同一个工程复用同一个 runner；工程变了就重建
    // （否则 flow 会跑在旧工程上）。由 commands.cpp 的 runFlow/cancelFlow 使用。
    std::shared_ptr<flow::FlowRunner> EnsureFlowRunner(const project::Project& project,
                                                       const td::TdChoice& td);
    std::shared_ptr<flow::FlowRunner> flow_runner();  // 可能为 nullptr

    // 预留：flow 模块接入后由 flow runner 调用，推送 fpgaLsp/flowEvent 通知。
    // 现在没有任何调用点，但先固定签名与 wire 形状（PROTOCOL-V2.md §4）。
    void SendFlowEvent(const Json& event);

    // =========================================================================
    //  供**非 LSP 通道**复用（MCP 工具等）：直接返回协议 JSON，不经过传输层
    // =========================================================================
    //  ⚠ 与对应的 LSP handler 是**同一实现**：handler 只负责解包 params 再调这里。
    //     不要在 MCP 侧另写一份"索引 -> JSON"，两份实现必然漂移（见 features.h）。
    //  所有方法都不抛异常（内部已兜底），失败返回 `{ok:false}` 或 `null`/`[]`。
    Json FeatureInitialize();                  // 只取能力声明（不动工作区根）
    Json FeatureInitialize(const Json& params);  // LSP initialize 的完整语义
    Json FeatureDocumentSymbols(std::string_view path);
    Json FeatureWorkspaceSymbols(std::string_view query);
    Json FeatureDefinition(std::string_view path, int line, int character);
    Json FeatureHover(std::string_view path, int line, int character);
    Json FeatureReferences(std::string_view path, int line, int character,
                           bool include_declaration);
    Json FeatureInlayHints(std::string_view path);
    Json FeatureCompletion(std::string_view path, int line, int character);
    // uri 为空 = 用当前工程（与 fpgaLsp/moduleTree 的 params.uri 语义一致）
    Json FeatureModuleTree(std::string_view uri, bool include_primitives = false);
    // P-C2：懒加载子节点（offset/limit 分页）
    Json FeatureModuleChildren(const Json& params);
    // B-A3：改名会破坏 .al 的路径记录 —— 必须在客户端改名前给出 WorkspaceEdit。
    Json FeatureWillRenameFiles(const Json& params);
    // B-B6：两次 run 的 QoR/时序差异
    Json FeatureCompareRuns(const Json& params);
    // X-B2：run 列表与指标（趋势图数据源）
    Json FeatureListRuns(const Json& params);
    // B-B10：testbench 骨架（返回文本 + 解析自检）
    Json FeatureTestbench(const Json& params);
    // X-B3：约束表格数据源（.adc -> 结构化行 + 诊断数）
    Json FeatureConstraintTable(const Json& params);
    // B-B4：把分析缓存里的 DRC/CDC 违例发布成诊断（带解析到的源码位置）
    void PublishAnalysisDiagnostics(const project::Project& project);
    // 工程摘要（与 fpgalsp.info 同一实现）
    Json FeatureInfo();

    // ---- Phase B：能力层（LSP 与 MCP 共用的唯一实现）----
    //  这些方法的实现在 src/server/features_*.cpp；B2–B10 各自填充自己的模块，
    //  但**签名与 dispatch 已在此固定**，避免并行改 lsp_server.cpp。
    Json FeatureReports(const Json& params);
    Json FeatureFlowStatus(const Json& params = Json::object());
    Json FeatureSemanticTokens(std::string_view path);
    Json FeatureFoldingRanges(std::string_view path);
    Json FeatureCodeActions(std::string_view path, const Json& range, const Json& context);
    Json FeaturePrepareRename(std::string_view path, int line, int character);
    Json FeatureRename(std::string_view path, int line, int character,
                       std::string_view new_name);
    Json FeatureCallHierarchyPrepare(std::string_view path, int line, int character);
    Json FeatureCallHierarchyIncoming(const Json& item);
    Json FeatureCallHierarchyOutgoing(const Json& item);
    Json FeatureRangeFormatting(std::string_view path, const Json& range, const Json& options);
    Json FeatureConstraintSymbols(std::string_view path);
    Json FeatureDebugHighlight(const Json& params);
    Json FeatureRunAnalysis(const Json& params);
    Json FeatureUpdateProject(const Json& params);
    Json FeatureInstantiation(const Json& params);
    Json FeatureCopyInstantiation(const Json& params);
    Json FeatureSymbolPath(const Json& params);
    Json FeatureDuplicateModules(const Json& params);
    Json FeatureSearchHierarchy(const Json& params);
    Json FeatureAddSourceFile(const Json& params);
    Json FeatureSetProjectName(const Json& params);

    // ---- 供调试/测试查询（不参与协议）----
    const Settings& settings() const { return settings_; }
    std::size_t OpenDocumentCount() const { return docs_.Size(); }

    // =========================================================================
    //  P-D1：给"附着模式"的 MCP 会话共享同一份缓存
    // =========================================================================
    //  MCP 以前每次都 `reports::BuildReports`（审计实测冷构建 768–784 ms），
    //  与 LSP 侧完全无关。附着时（`--mcp-port` + `--attach-port`）让 Bridge 用这里的
    //  实例，就能命中同一份解析结果与快照。
    //  ⚠ 只允许"读"：调用方不得持有返回的指针跨请求，也不得调用会改变工程视图的接口。
    IndexService* index_service() { return index_service_.get(); }
    ReportStore* report_store() { return report_store_.get(); }

private:
    // =========================================================================
    //  handler（标准 LSP）
    // =========================================================================
    HandlerResult HandleInitialize(const Json& params);
    HandlerResult HandleDidOpen(const Json& params);
    HandlerResult HandleDidChange(const Json& params);
    HandlerResult HandleDidSave(const Json& params);
    HandlerResult HandleDidClose(const Json& params);
    HandlerResult HandleDidChangeConfiguration(const Json& params);
    HandlerResult HandleDidChangeWatchedFiles(const Json& params);
    HandlerResult HandleNoop(const Json& params);

    // 已声明能力对应的方法：本阶段一律返回**合法的空结果**（不返回 -32601，
    // 否则编辑器每次悬停/补全都会在日志里刷错误）。详见 doc/commands.md。
    HandlerResult HandleEmptyList(const Json& params);
    HandlerResult HandleEmptyObject(const Json& params);
    HandlerResult HandleNull(const Json& params);
    HandlerResult HandleSemanticTokensFull(const Json& params);
    HandlerResult HandleSemanticTokensDelta(const Json& params);

    // =========================================================================
    //  textDocument/formatting —— 内嵌 Verible formatter
    // =========================================================================
    //  链接了 fpga_lsp::verible_bridge 时（FPGA_LSP_VERIBLE_ENABLED=ON）走真实格式化；
    //  否则返回 []（等于"不改文本"），见 src/server/CMakeLists.txt。
    //
    //  为什么先只做全文档、rangeFormatting 仍返回 []：
    //    全文档走 `FormatVerilog + 空 LineNumberSet`，这条路径已被
    //    ctest -L formatgate 的 102/102 文件逐字节验证。
    //    区间格式化在 CLI 语义下是 `FormatVerilog + 非空 LineNumberSet`
    //    （同样已验证 194/194），但**编辑器的"格式化选区"用的是另一个 API**
    //    `FormatVerilogRange`（只输出区间那一段、不做收敛性检验），
    //    该 API 尚未做等价性验证（doc/history/gate-reports/FORMAT-GATE-REPORT.md §8）。
    //    在没验证前不接，避免"看起来能用但输出与官方 CLI 不同"。
    HandlerResult HandleFormatting(const Json& params);
    HandlerResult HandleRangeFormatting(const Json& params);

    // =========================================================================
    //  handler（符号类，定义在 hdl_handlers.cpp）
    // =========================================================================
    HandlerResult HandleCompletion(const Json& params);
    HandlerResult HandleHover(const Json& params);
    HandlerResult HandleDefinition(const Json& params);
    HandlerResult HandleDocumentSymbol(const Json& params);
    HandlerResult HandleWorkspaceSymbol(const Json& params);
    HandlerResult HandleReferences(const Json& params);
    HandlerResult HandleInlayHint(const Json& params);

    // ---- Phase B：标准方法（实现在 lsp_server.cpp，内部只调 Feature*）----
    HandlerResult HandleCodeAction(const Json& params);
    HandlerResult HandlePrepareRename(const Json& params);
    HandlerResult HandleRename(const Json& params);
    HandlerResult HandlePrepareCallHierarchy(const Json& params);
    HandlerResult HandleIncomingCalls(const Json& params);
    HandlerResult HandleOutgoingCalls(const Json& params);
    HandlerResult HandleFoldingRange(const Json& params);

    // =========================================================================
    //  handler（自定义 v2 方法 / executeCommand）—— 定义在 commands.cpp
    // =========================================================================
    HandlerResult HandleModuleTree(const Json& params);
    HandlerResult HandleModuleChildren(const Json& params);
    HandlerResult HandleWillRenameFiles(const Json& params);
    HandlerResult HandleDidRenameFiles(const Json& params);
    HandlerResult HandleReports(const Json& params);
    HandlerResult HandleFlowStatus(const Json& params);
    HandlerResult HandleExecuteCommand(const Json& params);
    Json TdSummary();
    // P-D2：带 5 s TTL 的 FindTd（负结果也缓存，避免缺 TD 时每请求都扫安装目录）
    std::optional<td::TdChoice> FindTdCached(const std::string& configured);
    HandlerResult CommandDevice();

    // =========================================================================
    //  诊断
    // =========================================================================
    // 对应 server.py _publish_merged：
    //   * only_path == nullptr -> 全量发布（含"这次已经没有诊断了"的文件，发空数组清屏）
    //   * only_path != nullptr -> 只发这一个文件；它没有诊断时发空数组
    void PublishMerged(const project::Project& project, const std::string* only_path);
    // 对应 _clear_td_path：保存时立刻清掉该文件的 TD 诊断并重新发布
    void ClearTdPath(const project::Project& project, std::string_view path);
    // 对应 _schedule_td_check：请求一轮 TD 检查（**非阻塞**）。
    // W3：真正的调度交给 td::TdService（单飞 + 300 ms 去抖 + 取消），
    //     这里只做 enable_td / shutting_down 的门禁。
    void ScheduleTdCheck(const project::Project& project);
    // W3：TdService 的 worker 线程回调 —— 一轮结果落地并按文件发布
    //     （被取消的轮次不会进来：TdService 只在 !cancelled && !stop_ 时回调）。
    void ApplyTdResult(const project::Project& project, td::TdCheckResult result);
    // 保留下来的**同步单轮**实现（建会话 -> RunReadHdl -> 落表 -> 发布 -> 杀会话）。
    // 异步路径已全部改走 TdService/ApplyTdResult；本函数当前无生产调用方，
    // 仅作为"要同步等一轮"时的备用入口（A5 设计里同样保留）。
    void RunTdCheckOnce(const project::Project& project);
    // didOpen/didChange/didSave 之后重发该文件（或全部）的诊断
    void RunDiagnostics(std::string_view path, bool publish_all);

    // ---- T1 / task-50：服务端 lint（内嵌 Verible）+ 按来源合并 ----
    // 请求该文件的 lint 重算（非阻塞，300 ms 去抖，worker 线程执行）。
    void ScheduleLint(const std::string& path, const std::string& uri);
    // worker 线程：取文档快照 -> 内嵌 Verible -> 转成 LSP Diagnostic（UTF-16 列）。
    lsp::DiagnosticList ComputeLint(const std::string& path);
    // worker 线程回调：写 diag_lint_ 并发布该文件的**并集**（lint + TD）。
    void OnLintResult(const std::string& path, lsp::DiagnosticList diagnostics);
    // 发布一个文件的并集（lint + TD）。project == nullptr 时只发 lint。
    void PublishMergedPath(const project::Project* project, const std::string& path);

    void PublishDiagnostics(std::string_view path, const lsp::DiagnosticList& diagnostics);
    void SendNotification(std::string_view method, Json params);
    // Phase 2：索引/报告变化推送（插件定向刷新用；旧客户端会忽略未知通知）
    void MaybeNotifyIndexChanged(const std::vector<std::string>& paths);
    // D9：paths = 本次变化/删除的报告文件；sections 由文件名推断，不再构建快照。
    void SendReportsChanged(const project::Project& project,
                            const std::vector<std::string>& paths = {});
    void LogMessage(int type, std::string_view message);

    // =========================================================================
    //  工程 / 路径
    // =========================================================================
    std::string RootPath() const;
    // 对应 _project(path)：配置 > 向上查找 > 工作区递归扫描 + 亲和度
    // 值语义入口（签名不变）；热路径请用 ProjectForPathShared 免掉这层拷贝。
    std::optional<project::Project> ProjectForPath(std::string_view path);
    // 对应 _project(self._root_path())：当前工程
    std::optional<project::Project> CurrentProject();
    // CurrentProject 的共享版本（热路径用）：命中缓存时零 Project 拷贝。
    // 与 ProjectForPathShared 一样，调用方必须把 shared_ptr 持有到使用点结束。
    std::shared_ptr<const project::Project> CurrentProjectShared();
    // 对应 _project_from_params(params)：params.uri 所属工程，缺省回落当前工程
    std::optional<project::Project> ProjectFromParams(const Json& params);
    void InvalidateProjectCaches();

    // =========================================================================
    //  HDL 索引（src/hdl；按 (工程, 文件签名) 缓存，缓冲区内容优先）
    // =========================================================================
    struct HdlIndexEntry {
        std::string signature;
        std::shared_ptr<const hdl::HdlIndex> index;
        bool truncated = false;
        std::size_t parse_errors = 0;
    };

    // 取当前工程的 HDL 索引（必要时重建）。永不返回 nullptr 以外的异常：
    // 解析失败只会体现为 parse_errors / 空索引。
    std::shared_ptr<const hdl::HdlIndex> IndexForProject(const project::Project& project,
                                                         bool* truncated = nullptr,
                                                         std::size_t* parse_errors = nullptr);
    // 读文本：未保存缓冲区优先，落盘回落（DocumentStore 已实现这个语义）
    std::optional<std::string> LoadText(const std::string& path);
    // 文件签名：路径 + (缓冲区版本 | 磁盘 mtime+size)
    std::string ProjectSignature(const project::Project& project);

    // =========================================================================
    //  TD 会话
    // =========================================================================
    std::shared_ptr<td::TdSession> CreateSession(const project::Project& project,
                                                 std::string* error);
    void KillSession();
    bool SessionAlive() const;
    // 对应 load_device_info：import_device + 4 条 query_current_package
    std::optional<DeviceSummary> LoadDeviceSummary(const project::Project& project,
                                                   td::TdSession& session, std::string* error);
    // 对应 _device_cached：先看内存表，再看工程目录下的 .fpgalsp/cache（不启动 TD）
    std::optional<DeviceSummary> CachedDevice(const project::Project& project);

    // =========================================================================
    //  状态
    // =========================================================================
    // 工程查找结果缓存项（对应 Python discover_projects 的 5 秒缓存）
    // F2/R3b：键从 path 改成 **dirname**（同目录文件共享一条），容量按 LRU 淘汰
    // （原来是"满 64 条整体清空"，200 文件工程会被周期性清空 3 次）。
    struct DiscoveryEntry {
        std::chrono::steady_clock::time_point at;
        std::chrono::steady_clock::time_point last_used;
        std::string al;  // 空串 = 上次查找"没有工程"
    };

    // F2/R3b：工作区级 `DiscoverProjects` 结果缓存（TTL 5 s，键 = 规范化 root）。
    // 非工程文件（不在任何 .al 之下）时，`FindProjectAl` 会递归扫全工作区
    // （实测 18–77 ms），这一层缓存把它摊到每 5 秒一次，而不是每请求一次。
    struct DiscoveredProjectsEntry {
        std::chrono::steady_clock::time_point at;
        std::vector<std::string> candidates;
    };

    // 与 `project::FindProjectAl` 逐句等价，但把"递归扫描"换成带 TTL 的缓存。
    // ⚠ 若 al_project.cpp 的发现逻辑变化，这里要同步（报告 F2.md 已登记该耦合）。
    std::optional<std::string> ResolveProjectAl(const std::string& root, const std::string& path,
                                                const std::string& configured);

    jsonrpc::TransportBaseCommon& transport_;
    std::string td_explicit_;
    Settings settings_;

    lsp::DocumentStore docs_;
    std::string root_uri_;
    std::string project_override_;

    // TD 会话：同一时刻最多一个（TD 检查 / device 查询共用）
    mutable std::mutex session_mutex_;
    std::shared_ptr<td::TdSession> session_;

    // 诊断表：**按来源分开存**（T1）。发布时合并，互不覆盖。
    //   * diag_td_  : al_path -> (file_path -> 诊断)
    //   * diag_lint_: file_path -> 诊断（lint 是单文件概念，不需要工程键）
    mutable std::mutex state_mutex_;
    std::map<std::string, std::map<std::string, lsp::DiagnosticList>> diag_td_;
    // B-B4：runAnalysis 的 DRC/CDC 违例（al_path -> (file -> 诊断)），与 TD/lint 一起发布
    std::map<std::string, std::map<std::string, lsp::DiagnosticList>> diag_analysis_;
    std::map<std::string, lsp::DiagnosticList> diag_lint_;
    std::map<std::string, std::set<std::string>> published_;
    bool shutting_down_ = false;
    // T1：lint 的后台调度（每文件去抖 + single-flight + 结果作废）。Shutdown 里 join。
    std::unique_ptr<LintService> lint_service_;
    // W3：TD 检查的单飞队列（300 ms 去抖 + 取消在跑的一轮）。
    // 以前这里是 td_running_/td_pending_/td_worker_ 三个字段 + 手写 TdCheckLoop；
    // 现在策略全部在 td::TdService 里（src/td/td_service.h），本类只剩"请求"和"落地"。
    std::unique_ptr<td::TdService> td_service_;

    // 器件缓存（对应 server.device_mem，键为 device|family|speed）
    mutable std::mutex device_mutex_;
    std::map<std::string, DeviceSummary> device_mem_;

    // 工程查找结果缓存（5 秒 TTL；对应 Python discover_projects 的 5s 缓存）
    // F2/R3b：键 = configured|root|dirname(path)，LRU 淘汰（容量 kDiscoveryCapacity）。
    mutable std::mutex discovery_mutex_;
    std::map<std::string, DiscoveryEntry> discovery_cache_;
    std::map<std::string, DiscoveredProjectsEntry> discovered_cache_;
    static constexpr std::size_t kDiscoveryCapacity = 256;

    // 工程解析缓存：al_path -> (mtime, size, Project)，对应 Python project.parse_al_cached。
    // B8/PERF-01：发现缓存只记了 .al 路径，命中后原先仍每次 project::ParseAl()
    //（读 .al 全文 + XML 解析 + 每条目 resolve/exists），200 文件工程每个请求多付 ~18 ms。
    // 这里按 .al 的 (mtime, size) 缓存已解析的 Project，语义与 Python 的 mtime 缓存一致。
    //
    // C1：条目持有 shared_ptr<const Project>，命中时只拷指针（引用计数 +1），
    // 不再把整份模型深拷贝出来。shared_ptr 保证对象在任何使用者手里都不会悬垂。
    struct ParsedProjectEntry {
        std::int64_t mtime = 0;
        std::uintmax_t size = 0;
        std::shared_ptr<const project::Project> project;
    };
    // 缓存命中时**零** Project 拷贝（只拷贝 shared_ptr）。永不返回 nullptr：
    // 解析失败时 project::ParseAl 会给出带 warnings 的空工程（never-throw）。
    std::shared_ptr<const project::Project> ParseAlCachedShared(const std::string& al_path);
    // 旧的值语义入口（签名/行为保持不变）：内部走 ParseAlCachedShared + 一次拷贝。
    std::optional<project::Project> ParseAlCached(const std::string& al_path);
    // 与 ProjectForPath 等价，但返回共享模型：命中缓存时零 Project 拷贝。
    // ⚠ 调用方必须把 shared_ptr 持有到使用点结束（不要只留下 `const Project&` 引用）。
    std::shared_ptr<const project::Project> ProjectForPathShared(std::string_view path);
    mutable std::mutex project_cache_mutex_;
    std::map<std::string, ParsedProjectEntry> project_cache_;

    // HDL 索引缓存：al_path -> (签名, 索引)。签名不变就复用。
    // 索引构建只读 DocumentStore 的快照 / 磁盘，不在 worker 线程里跑（handler 很轻）。
    std::mutex hdl_mutex_;
    std::map<std::string, HdlIndexEntry> hdl_cache_;
    // 解析缓存按 (path, 内容哈希)，跨工程重建也能命
    std::unique_ptr<hdl::ParseCache> hdl_parse_cache_;

    // Phase 2 / A1：增量索引服务（不可变快照 + 单写者）。创建于构造期，
    // loader 捕获 this 以复用 DocumentStore 的未保存缓冲区。
    std::unique_ptr<IndexService> index_service_;
    // Phase 2 / A3：报告缓存（文件级 stamp + 分节快照）
    std::unique_ptr<ReportStore> report_store_;
    // Phase 2 / B2：semanticTokens 增量（按 uri 记住上次 resultId/data）
    struct SemanticCacheEntry {
        std::string result_id;
        Json data;
    };
    std::mutex semantic_mutex_;
    std::map<std::string, SemanticCacheEntry> semantic_cache_;
    // 上次推送过 indexChanged 的 generation（避免同一版本重复通知）
    std::uint64_t last_index_generation_ = 0;

    // flow 引擎（src/flow）。runner 内部自带 worker 线程与 cancel/join 语义。
    std::mutex flow_mutex_;
    std::shared_ptr<flow::FlowRunner> flow_;

    // P-D2：TD 安装根探测缓存。`td::FindTd` 每次都枚举安装目录（审计 0.89–0.93 ms），
    // 而它几乎不可能在几秒内变化；reports/project_info 都走同一条路。
    std::mutex td_cache_mutex_;
    std::string td_cache_key_;
    std::optional<td::TdChoice> td_cache_value_;
    bool td_cache_valid_ = false;
    std::chrono::steady_clock::time_point td_cache_at_{};
};

// JSON 里取诊断的落点路径：td::MapMessage 把绝对路径写在 data["path"]（见
// td_diagnostics.h 的约定），发布前要把它从 data 里摘掉（Python 是 pop("_path")）。
std::string DiagnosticPath(const lsp::Diagnostic& diagnostic, std::string_view fallback);

// T1 / task-50：按来源合并诊断（lint + TD）并**稳定排序**。
//   * 排序键：line -> character -> severity -> source -> code -> message（确定性，便于"内容相同就不重发"）
//   * 去重：完全相同的 (range,severity,code,source,message) 只留一条
//   * 红线：**lint 不覆盖 TD、TD 不覆盖 lint** —— 两个来源的条目都会保留
// B-B8：工程级诊断（.al 缺文件 / 解析告警），发布到 .al 文件上。
lsp::DiagnosticList ProjectDiagnostics(const project::Project& project);

lsp::DiagnosticList MergeDiagnosticSources(const lsp::DiagnosticList& lint,
                                           const lsp::DiagnosticList& td);

// 工程摘要（对应 project.py project_summary；注意 warnings 是**条数**不是列表）
Json ProjectSummaryOf(const project::Project& project);
Json ProjectSummaryOf(std::string_view al_path);

// 已实现的 `workspace/executeCommand` 命令名（capability 声明与 dispatch 共用的唯一来源）。
// 新增命令时必须同时登记；`tests/unit/commands_test.cpp` 会逐个断言"能分发"，
// 防止出现"声明了但没实现"（B-A2 的验收条件）。
const std::vector<std::string>& ExecuteCommandNames();

// P-C4：`fpgaLsp/flowStatus` 的日志切片（纯函数，单测直接覆盖）。
// since 为空 = 旧客户端语义（回完整窗口）；给了 since 就只回序号 > since 的行，
// 若客户端落后于窗口（since < logStartSeq）第一行是 truncated 标记。
// 返回的数组同时带 logSeq / logStartSeq 两个字段的信息由调用方写入 flow 对象。
jsonrpc::Json SliceFlowLog(const flow::FlowState& state,
                           std::optional<std::uint64_t> since, std::uint64_t* log_start_seq);

// P-D5：语义高亮的真增量（纯函数，单测直接覆盖）。
// 输入/输出都是 LSP 的 5 元组数组；只返回覆盖变化区间的那一条编辑（或空数组）。
jsonrpc::Json SemanticDeltaEdits(const jsonrpc::Json& previous, const jsonrpc::Json& current);

// capability 里广播的子集：全表减去扩展自己 contributes 的 3 条（check/info/restart）。
// vscode-languageclient 会为广播的每条命令注册 VS Code 命令，重复注册会让 initialize 失败。
const std::vector<std::string>& AdvertisedCommandNames();

}  // namespace fpgalsp::server