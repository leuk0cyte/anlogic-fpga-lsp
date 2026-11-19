// =============================================================================
//  src/mcp/bridge.h —— MCP 工具实现层（把工具调用接到 feature 层）
// =============================================================================
//  参考实现：Y:\fpga-lsp\fpgalsp\mcp_bridge.py 的 FpgaMcpBridge（1441 行）
//
//  ---- 分层 ----
//    ToolDispatcher（mcp_server.h）
//        └── Bridge（本文件）：工程解析 / 文件读写 / 诊断 / 器件 / 约束 / 报告 / flow 日志
//              └── feature 层：project / td / reports / constraints / platform
//
//  ---- 分阶段实现（task-18 明确要求）----
//    ✅ 真实实现（15 个，不依赖 hdl 索引 / flow / Verible / program 模块）：
//       project_list, project_info, project_files, project_reports,
//       diagnostics, device_info, constraints_status, constraint_audit,
//       td_info, read_file, search_text, flow_status, flow_log, flow_issues, flow_wait
//    ⛔ 显式占位（24 个）：依赖尚未移植的模块，统一返回
//       {ok:false, error:"not implemented: <缺什么>"}。
//       **不静默返回空对象**：静默成功会让 agent 以为"查到了，只是没有数据"。
//
//  ---- ⚠ 文本扫描 HDL 时务必把注释替换成空格（不是删除）----
//  真实的 top.v 里有这种行：
//      input SUPSTS,        // driver enable output signal
//  如果按"删除注释"实现，后面的所有列号会整体左移，span 全错位；如果按"不处理注释"，
//  扫描器会把注释里的 "output signal" 当成一个端口（我们真踩过：25 个端口被扫成 26 个）。
//  正确做法：把注释字节**替换成空格并保留换行**，长度不变 -> 偏移全部有效。
//  实现见 bridge.cpp 的 BlankComments()。
//
//  ---- 工具 → 数据源对照表（加新工具前先读这张表）----
//
//  | 工具 | 数据源 | 为什么 | 位置基准 |
//  | --- | --- | --- | --- |
//  | document_symbols / workspace_symbols / definition / references / hover / project_structure
//  |   | `server::features::*` | LSP handler 与 MCP **共用同一实现**，不会漂移；
//  |   |                       | features 无状态、不持有 transport | 输入 0-based，输出转 1-based |
//  | module_info / duplicate_modules / search_hierarchy / symbol_path / call_hierarchy
//  |   | `hdl::HdlIndex` | C++ 侧没有对应 feature API（Python 的 ide.py / nav.py 未上移） | 同上；但
//  |   |                 |                                                  duplicate_modules /
//  |   |                 |                                                  search_hierarchy 的
//  |   |                 |                                                  line **保持 0-based** |
//  | flow_run / flow_cancel / flow_status / flow_wait | `flow::FlowRunner` | 与 LSP 的 runFlow 同一实现 |
//  | strategy_compare | `flow::CompareStrategies` | 与 fpgalsp.compareStrategies 同一实现 |
//  | cables | `flow::Inspect` | 与 fpgalsp.cables 同一实现（只读，不烧录） |
//  | project_list / info / files / reports / diagnostics / device_info / constraints_* / read_file /
//  |   search_text / td_info / flow_log / flow_issues | `project::` / `td::` / `reports::` /
//  |   `constraints::` / platform | 各自的 feature 层 |
//
//  **不要**为了"看起来统一"把符号工具合并成一个数据源：features 是"index -> LSP JSON"，
//  而 5 个 hdl 直查工具需要的是"index -> MCP 自定义形状"，两者职责不同。
//
//  ---- 两个刻意的降级（都写进响应字段，便于审计）----
//  1. 顶层端口表：**优先 HDL 索引**（与 server 的符号工具同一数据源），此时
//     `"portsSource": "hdl-index"`。只有索引里没有 top 模块（或端口为空）时才回落到
//     文本扫描，那时标注 `"portsSource": "text-scan"` —— 这个字段是给调用方的
//     "数据可信度提示"，不要删。
//  2. device 缓存：Python 写 `<project>/.fpgalsp/cache/*.json`。本实现**不写工程
//     目录**（PYTHON-BUGS.md P-4 的修复方向），只在进程内缓存 —— 同一进程内
//     重复调用不会重复 import_device。
// =============================================================================

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "constraints/port.h"
#include "mcp/mcp_server.h"
#include "mcp/tools.h"
#include "server/index_service.h"  // D5：HDL 索引改由 IndexService 持有（增量 + TTL）
#include "td/td_install.h"

namespace fpgalsp::project {
struct Project;
}

namespace fpgalsp::td {
struct DeviceInfo;
class TdSession;
}  // namespace fpgalsp::td

namespace fpgalsp::hdl {
class HdlIndex;
class ParseCache;
}  // namespace fpgalsp::hdl

namespace fpgalsp::flow {
class FlowRunner;
}  // namespace fpgalsp::flow

namespace fpgalsp::server {
class LspServer;
class ReportStore;
}  // namespace fpgalsp::server



namespace fpgalsp::mcp {

// 文本扫描出来的顶层端口（降级实现；HDL 索引就绪后由索引提供）
struct ScannedPort {
    std::string name;
    std::string direction;
    int width = 1;
    int msb = 0;
    int lsb = 0;
    int line = 0;       // 0-based
    int character = 0;  // 0-based
};

struct BridgeOptions {
    std::string root;     // 工作区根（绝对，正斜杠）
    std::string project;  // 显式 .al / 目录 / 工程名（可空）
    std::string td_path;  // 显式 TD 安装根（可空）
    // Phase 2 / B1：非空时直接复用宿主 LSP 进程的 Engine（单 Engine 多 transport）；
    // 为空时保持旧行为（懒构造 NullTransport LspServer）。
    server::LspServer* shared_server = nullptr;
};

class Bridge : public ToolDispatcher {
  public:
    // 根目录不存在时抛 ToolError（对应 Python FpgaMcpBridge.__init__）
    explicit Bridge(BridgeOptions options);
    ~Bridge() override;

    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;

    Json Call(std::string_view handler, const Json& arguments) override;

    // P-B6：只读资源的目录与读取（工程信息 / 文件清单 / 报告摘要 / flow 状态）。
    Json ListResources() override;
    std::optional<std::string> ReadResource(std::string_view uri) override;

    // 关闭 TD 会话（幂等）。main 在退出路径上显式调用，避免孤儿进程。
    void Shutdown();

  private:
    // 报告缓存：附着模式下用宿主 LspServer 的实例（P-D1），独立模式才自建。
    // 以前每次 `fpga_project_reports` 都 `reports::BuildReports`（冷 768–784 ms）。
    server::ReportStore& Reports();

    // ---- 工程 ----
    const project::Project& ActiveProject();
    const project::Project& ProjectFor(const Json& arguments);
    std::string ResolveAl(const std::string& value) const;
    std::string DiscoverDefaultAl() const;
    std::string ResolveFile(const std::string& value, const project::Project& proj) const;
    std::string SafeProjectFile(const std::string& value, const project::Project& proj) const;
    std::string SafeProjectPath(const std::string& value, const project::Project& proj) const;
    static bool IsWithin(const std::string& path, const std::string& root);

    // ---- 器件 ----
    const td::DeviceInfo& EnsureDeviceInfo(const project::Project& proj, bool refresh);
    td::TdSession& EnsureSession(const project::Project& project);

    // ---- 报错工具 ----
    [[noreturn]] static void Fail(const std::string& message) { throw ToolError(message); }

    // ---- 工具实现 ----
    Json ProjectList(const Json& arguments);
    Json ProjectInfo(const Json& arguments);
    Json ProjectFiles(const Json& arguments);
    Json ProjectReports(const Json& arguments);
    Json DiagnosticsTool(const Json& arguments);
    Json DeviceInfoTool(const Json& arguments);
    Json ConstraintsStatus(const Json& arguments);
    Json ConstraintAudit(const Json& arguments);
    Json TdInfoTool(const Json& arguments);
    Json ReadFileTool(const Json& arguments);
    Json SearchTextTool(const Json& arguments);
    Json FlowStatus(const Json& arguments);
    Json FlowLog(const Json& arguments);
    Json FlowIssues(const Json& arguments);
    Json FlowWait(const Json& arguments);
    Json FlowRunTool(const Json& arguments);
    Json FlowCancelTool(const Json& arguments);
    // P-B5：job = runId 的稳定别名（flow_run 已经异步，这里补查询/取消入口）
    Json JobStatusTool(const Json& arguments);
    Json JobCancelTool(const Json& arguments);
    Json StrategyCompareTool(const Json& arguments);
    Json CablesTool(const Json& arguments);
    Json CompletionTool(const Json& arguments);
    Json InstantiationTool(const Json& arguments);
    Json ProjectRenameTool(const Json& arguments);
    Json ProjectAddSourceTool(const Json& arguments);
    // Phase B / B6：真实实现（替换 MCP 占位）
    Json FormatTool(const Json& arguments);
    Json AnalysisRunTool(const Json& arguments);
    Json ProjectUpdateTool(const Json& arguments);
    // B6c：与 LSP 同一条实现（FeatureCodeActions / FeatureRename），
    // 通过一个"无 transport 的 LspServer"上下文调用（见 bridge.cpp 的 NullTransport）。
    Json CodeActionsTool(const Json& arguments);
    Json RenameEditsTool(const Json& arguments);
    // 懒构造的 feature 上下文（只在 code_actions / rename_edits 用到）
    server::LspServer& FeatureServer();

    // ---- 符号类（A 批，11 个）----
    Json ProjectStructure(const Json& arguments);
    Json DocumentSymbolsTool(const Json& arguments);
    Json WorkspaceSymbolsTool(const Json& arguments);
    Json SearchHierarchyTool(const Json& arguments);
    Json DefinitionTool(const Json& arguments);
    Json ReferencesTool(const Json& arguments);
    Json HoverTool(const Json& arguments);
    Json SymbolPathTool(const Json& arguments);
    Json CallHierarchyTool(const Json& arguments);
    Json ModuleInfoTool(const Json& arguments);
    Json DuplicateModulesTool(const Json& arguments);

    // ---- 辅助 ----
    Json TdSummary() const;

    // flow 引擎（与 LSP 的 runFlow 同一实现）。按工程重建：flow 跑在旧工程上比没跑更糟。
    flow::FlowRunner* EnsureFlowRunner(const project::Project& proj);
    // 当前 runner 对应的 .al（空 = 还没有 runner）
    std::string flow_project_;

    // ---- 符号层（阶段二 A 批）----
    // HDL 索引（按工程缓存；文件内容从磁盘读，MCP 没有未保存缓冲区概念）
    const hdl::HdlIndex& EnsureHdlIndex(const project::Project& proj);
    // 顶层端口：优先取 HDL 索引（portsSource = "hdl-index"）；
    // 索引里没有 top 模块时回落到文本扫描（portsSource = "text-scan"）。
    std::vector<constraints::Port> TopPorts(const project::Project& proj,
                                           std::string* source);
    // 文本扫描顶层端口（HDL 索引就绪前的降级实现）
    std::vector<ScannedPort> ScanTopPorts(const project::Project& proj) const;

    BridgeOptions options_;
    std::string root_;
    std::string active_al_;
    std::map<std::string, std::shared_ptr<const project::Project>> projects_;
    std::map<std::string, std::unique_ptr<td::DeviceInfo>> devices_;
    std::unique_ptr<td::TdSession> session_;
    std::optional<td::TdChoice> td_choice_;

    // 符号层：HDL 索引按工程缓存（JSON 形状由 server::features 产出，无状态）
    //
    // D5（2026-11-19）：旧实现只在 `hdl_project_ != proj.al_path` 时重建，
    // 于是 MCP 长连接（agent 会话）**永远看到第一次建好的索引** —— 用户改了文件
    // 也读不到新内容。现在改用一个 `server::IndexService`：它内部按
    // (path, mtime, size) 做增量校验（默认 250 ms TTL，didSave/watcher 事件走强制校验），
    // 与 LSP 侧同一套语义，既不会读到陈旧索引，也不会每请求全量重建。
    std::unique_ptr<server::IndexService> index_service_;
    // 上一次取到的不可变快照：保证返回的 `const HdlIndex&` 在本次调用期间有效
    std::shared_ptr<const hdl::HdlIndex> hdl_snapshot_;
    std::unique_ptr<flow::FlowRunner> flow_runner_;
    // B6c：code_actions / rename_edits 的 feature 上下文。
    // LspServer 的 Feature* 需要 ProjectForPath / IndexForProject / LoadText 这些
    // "服务层状态"，而 Bridge 侧只有自己的一份；为了**只保留一份实现**，这里持有一个
    // 无 transport 的 LspServer（NullTransport），懒构造并只在两个工具里使用。
    std::unique_ptr<server::LspServer> feature_server_;
    // 报告解析/快照缓存（P-D1）。附着模式指向 host 的实例，独立模式自建。
    std::unique_ptr<server::ReportStore> owned_report_store_;
};

}  // namespace fpgalsp::mcp