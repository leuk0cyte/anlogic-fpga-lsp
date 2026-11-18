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
    // 工程摘要（与 fpgalsp.info 同一实现）
    Json FeatureInfo();

    // ---- 供调试/测试查询（不参与协议）----
    const Settings& settings() const { return settings_; }
    std::size_t OpenDocumentCount() const { return docs_.Size(); }

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
    HandlerResult HandleNoop(const Json& params);

    // 已声明能力对应的方法：本阶段一律返回**合法的空结果**（不返回 -32601，
    // 否则编辑器每次悬停/补全都会在日志里刷错误）。详见 COMMANDS.md。
    HandlerResult HandleEmptyList(const Json& params);
    HandlerResult HandleEmptyObject(const Json& params);
    HandlerResult HandleNull(const Json& params);
    HandlerResult HandleSemanticTokensFull(const Json& params);

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
    //    该 API 尚未做等价性验证（tests/FORMAT-GATE-REPORT.md §8）。
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

    // =========================================================================
    //  handler（自定义 v2 方法 / executeCommand）—— 定义在 commands.cpp
    // =========================================================================
    HandlerResult HandleModuleTree(const Json& params);
    HandlerResult HandleReports(const Json& params);
    HandlerResult HandleFlowStatus(const Json& params);
    HandlerResult HandleExecuteCommand(const Json& params);
    Json TdSummary();
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
    // 对应 _schedule_td_check
    void ScheduleTdCheck(const project::Project& project);
    // TD 线程主体（内部循环，处理 pending）
    void TdCheckLoop(project::Project project);
    // 执行一次完整检查（建会话 -> RunReadHdl -> 落表 -> 发布 -> 杀会话）
    void RunTdCheckOnce(const project::Project& project);
    // didOpen/didChange/didSave 之后重发该文件（或全部）的诊断
    void RunDiagnostics(std::string_view path, bool publish_all);

    void PublishDiagnostics(std::string_view path, const lsp::DiagnosticList& diagnostics);
    void SendNotification(std::string_view method, Json params);
    void LogMessage(int type, std::string_view message);

    // =========================================================================
    //  工程 / 路径
    // =========================================================================
    std::string RootPath() const;
    // 对应 _project(path)：配置 > 向上查找 > 工作区递归扫描 + 亲和度
    std::optional<project::Project> ProjectForPath(std::string_view path);
    // 对应 _project(self._root_path())：当前工程
    std::optional<project::Project> CurrentProject();
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
    struct DiscoveryEntry {
        std::chrono::steady_clock::time_point at;
        std::string al;  // 空串 = 上次查找"没有工程"
    };

    jsonrpc::TransportBaseCommon& transport_;
    std::string td_explicit_;
    Settings settings_;

    lsp::DocumentStore docs_;
    std::string root_uri_;
    std::string project_override_;

    // TD 会话：同一时刻最多一个（TD 检查 / device 查询共用）
    mutable std::mutex session_mutex_;
    std::shared_ptr<td::TdSession> session_;

    // 诊断表：al_path -> (file_path -> 诊断)
    mutable std::mutex state_mutex_;
    std::map<std::string, std::map<std::string, lsp::DiagnosticList>> td_diags_;
    std::map<std::string, std::set<std::string>> published_;
    bool td_running_ = false;
    bool td_pending_ = false;
    bool shutting_down_ = false;
    std::thread td_worker_;

    // 器件缓存（对应 server.device_mem，键为 device|family|speed）
    mutable std::mutex device_mutex_;
    std::map<std::string, DeviceSummary> device_mem_;

    // 工程查找结果缓存（5 秒 TTL；对应 Python discover_projects 的 5s 缓存）
    mutable std::mutex discovery_mutex_;
    std::map<std::string, DiscoveryEntry> discovery_cache_;

    // HDL 索引缓存：al_path -> (签名, 索引)。签名不变就复用。
    // 索引构建只读 DocumentStore 的快照 / 磁盘，不在 worker 线程里跑（handler 很轻）。
    std::mutex hdl_mutex_;
    std::map<std::string, HdlIndexEntry> hdl_cache_;
    // 解析缓存按 (path, 内容哈希)，跨工程重建也能命
    std::unique_ptr<hdl::ParseCache> hdl_parse_cache_;

    // flow 引擎（src/flow）。runner 内部自带 worker 线程与 cancel/join 语义。
    std::mutex flow_mutex_;
    std::shared_ptr<flow::FlowRunner> flow_;
};

// JSON 里取诊断的落点路径：td::MapMessage 把绝对路径写在 data["path"]（见
// td_diagnostics.h 的约定），发布前要把它从 data 里摘掉（Python 是 pop("_path")）。
std::string DiagnosticPath(const lsp::Diagnostic& diagnostic, std::string_view fallback);

// 工程摘要（对应 project.py project_summary；注意 warnings 是**条数**不是列表）
Json ProjectSummaryOf(const project::Project& project);
Json ProjectSummaryOf(std::string_view al_path);

}  // namespace fpgalsp::server