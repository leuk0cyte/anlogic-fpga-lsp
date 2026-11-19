// =============================================================================
//  src/server/features_analysis.cpp —— DRC/CDC 分析 + project_update
// =============================================================================
//  OWNER: task B5 (parity-mcp)
//
//  这两个 Feature 是 **LSP executeCommand 与 MCP 工具的唯一实现**：
//      LSP  commands.cpp: fpgalsp.runAnalysis / fpgalsp.updateProject
//      MCP  bridge.cpp : fpga_analysis_run / fpga_project_update（B6 接线）
//  所以这里只做"取当前工程 + 调 analysis::"，不复制任何解析逻辑。
//
//  对应 Python（server.py:2317-2327 / 2230-2243）的失败语义：
//    * 没有工程      -> {ok:false, error:".al project not found"}
//    * 找不到 TD     -> analysis::Run 内部返回 {ok:false, error:"No TangDynasty installation found"}
//    * 任何异常      -> {ok:false, error}（绝不抛到 dispatcher）
// =============================================================================

#include <exception>
#include <optional>
#include <string>
#include <string_view>

#include "analysis/analysis.h"
#include "server/lsp_server.h"

namespace fpgalsp::server {
namespace {

Json ToolFailure(std::string_view message) {
    Json result = Json::object();
    result["ok"] = false;
    result["error"] = std::string(message);
    return result;
}

}  // namespace

// 对应 Python on_workspace_executeCommand("fpgalsp.runAnalysis")：
//   project = self._project(self._root_path()); td_info = find_td(...); analysis.run(...)
Json LspServer::FeatureRunAnalysis(const Json& /*params*/) {
    std::optional<project::Project> project;
    try {
        project = CurrentProject();
    } catch (const std::exception& exc) {
        return ToolFailure(exc.what());
    }
    if (!project.has_value()) return ToolFailure(".al project not found");
    try {
        return analysis::Run(*project, td_explicit_,
                             [this](std::string_view message) {
                                 LogMessage(3, message);  // 3 = Info（对应 Python log()）
                             });
    } catch (const std::exception& exc) {
        return ToolFailure(exc.what());
    } catch (...) {
        return ToolFailure("unknown exception in runAnalysis");
    }
}

// 对应 Python on_workspace_executeCommand("fpgalsp.updateProject")：
//   check_project_version -file <al>  +  open_project <al> -update
Json LspServer::FeatureUpdateProject(const Json& /*params*/) {
    std::optional<project::Project> project;
    try {
        project = CurrentProject();
    } catch (const std::exception& exc) {
        return ToolFailure(exc.what());
    }
    if (!project.has_value()) return ToolFailure(".al project not found");
    try {
        return analysis::UpdateProject(*project, td_explicit_,
                                       [this](std::string_view message) {
                                           LogMessage(3, message);
                                       });
    } catch (const std::exception& exc) {
        return ToolFailure(exc.what());
    } catch (...) {
        return ToolFailure("unknown exception in updateProject");
    }
}

}  // namespace fpgalsp::server