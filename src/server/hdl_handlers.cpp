// =============================================================================
//  src/server/hdl_handlers.cpp —— 符号类 LSP handler（薄包装）
// =============================================================================
//  ⚠ 本文件**只做三件事**：从 params 取参数 -> 拿索引 -> 调 features。
//    真正的"索引 -> JSON"逻辑全在 src/server/features.cpp（LSP 与 MCP 共用一份实现）。
//
//  已实现：
//    textDocument/documentSymbol   textDocument/hover      textDocument/definition
//    workspace/symbol              textDocument/references textDocument/inlayHint
//    textDocument/completion
//
//  ---- B10（parity-symbols）在本层加的东西：只有"按扩展名分派" ----------
//  `.adc` / `.sdc` 不是 HDL，走 HDL 索引只会得到空结果（探针：pin.adc 的
//  documentSymbol Python 47 条 / C++ 0 条）。所以 documentSymbol、hover、
//  completion 三个方法的最前面各加一个"是约束文档吗"的早退分支；语义本身全在
//  src/constraints（纯函数）与 src/server/features_constraints.cpp（协议 JSON 出口）。
//  **HDL 分支的行为一行未改**。
//
//  另有一处（B10 引入，B10b 已下沉）：completion 的 745 条器件原语 + 48 条 Verilog
//  关键字**曾经加在这一层**，导致 MCP `fpga_completion`（走 features::Completion）
//  只有 21 项。现在它们属于 `features::Completion`，本层只调它、不再追加。
// =============================================================================

#include <string>
#include <string_view>
#include <vector>

#include "constraints/port.h"
#include "hdl/hdl_index.h"
#include "jsonrpc/json_util.h"
#include "platform/path.h"
#include "server/constraints_features.h"
#include "server/features.h"
#include "server/lsp_server.h"

namespace fpgalsp::server {
namespace {

std::string TextDocumentUri(const Json& params) {
    return jsonrpc::GetStringPath(params, {"textDocument", "uri"});
}

void ReadPosition(const Json& params, int* line, int* character) {
    const Json position = jsonrpc::GetObject(params, "position");
    *line = jsonrpc::GetInt(position, "line", 0);
    *character = jsonrpc::GetInt(position, "character", 0);
}

// 顶层端口表（Python `_top_ports`）：约束文档的 hover / 补全只需要这三个字段。
// 拿不到工程或索引时返回空表 —— 与 Python 的 `ports = []` 分支一致。
std::vector<constraints::Port> TopPortsForPath(const project::Project& project,
                                               const hdl::HdlIndex& index) {
    std::vector<constraints::Port> ports;
    if (project.top.empty()) return ports;
    const hdl::Module* module = index.ModuleByName(project.top);
    if (module == nullptr) return ports;
    ports.reserve(module->ports.size());
    for (const hdl::Port& port : module->ports) {
        constraints::Port converted;
        converted.name = port.name;
        converted.direction = port.direction;
        converted.msb = port.msb;
        converted.lsb = port.lsb;
        converted.width = port.width;
        converted.line = port.line;
        converted.character = port.character;
        converted.file = port.file;
        ports.push_back(std::move(converted));
    }
    return ports;
}

// HDL completion 的补充候选（745 器件原语 + 48 关键字）**不在这一层**：
// 它是 `features::Completion` 的一部分（LSP 与 MCP 必须拿到同一份候选集，
// 任何一边漏掉都会被 parity 探针抓出来 —— B10b 就是修这个）。

}  // namespace

// =============================================================================
//  textDocument/documentSymbol
// =============================================================================

Json LspServer::FeatureDocumentSymbols(std::string_view path) {
    // .adc / .sdc：约束文档（B10）
    if (IsConstraintDocument(path)) return FeatureConstraintSymbols(path);
    auto project = ProjectForPath(path);
    if (!project.has_value()) return Json::array();
    const auto index = IndexForProject(*project);
    if (!index) return Json::array();
    return features::DocumentSymbols(*index, path);
}

HandlerResult LspServer::HandleDocumentSymbol(const Json& params) {
    const std::string uri = TextDocumentUri(params);
    if (uri.empty()) return HandlerResult::Ok(Json::array());
    return HandlerResult::Ok(FeatureDocumentSymbols(platform::uri_to_path(uri)));
}

// =============================================================================
//  workspace/symbol
// =============================================================================

Json LspServer::FeatureWorkspaceSymbols(std::string_view query) {
    auto project = CurrentProject();
    if (!project.has_value()) return Json::array();
    const auto index = IndexForProject(*project);
    if (!index) return Json::array();
    return features::WorkspaceSymbols(*index, query);
}

HandlerResult LspServer::HandleWorkspaceSymbol(const Json& params) {
    return HandlerResult::Ok(FeatureWorkspaceSymbols(jsonrpc::GetString(params, "query")));
}

// =============================================================================
//  textDocument/definition
// =============================================================================

Json LspServer::FeatureDefinition(std::string_view path, int line, int character) {
    auto project = ProjectForPath(path);
    if (!project.has_value()) return Json(nullptr);
    const auto index = IndexForProject(*project);
    if (!index) return Json(nullptr);
    return features::Definition(*index, path, line, character);
}

HandlerResult LspServer::HandleDefinition(const Json& params) {
    const std::string uri = TextDocumentUri(params);
    if (uri.empty()) return HandlerResult::Ok(Json(nullptr));
    int line = 0;
    int character = 0;
    ReadPosition(params, &line, &character);
    return HandlerResult::Ok(FeatureDefinition(platform::uri_to_path(uri), line, character));
}

// =============================================================================
//  textDocument/hover
// =============================================================================

Json LspServer::FeatureHover(std::string_view path, int line, int character) {
    // .adc / .sdc：约束文档（B10）。约束 hover 不依赖 HDL 索引；只有 `port`
    // 上下文才用顶层端口表，没有工程时按 Python 的空端口表走。
    if (IsConstraintDocument(path)) {
        const std::optional<std::string> text = LoadText(std::string(path));
        const std::string_view view =
            text.has_value() ? std::string_view(*text) : std::string_view();
        std::vector<constraints::Port> ports;
        if (auto project = ProjectForPath(path); project.has_value()) {
            if (const auto index = IndexForProject(*project); index) {
                ports = TopPortsForPath(*project, *index);
            }
        }
        return ConstraintHover(path, view, line, character, ports);
    }
    auto project = ProjectForPath(path);
    if (!project.has_value()) return Json(nullptr);
    const auto index = IndexForProject(*project);
    if (!index) return Json(nullptr);
    return features::Hover(*index, path, line, character);
}

HandlerResult LspServer::HandleHover(const Json& params) {
    const std::string uri = TextDocumentUri(params);
    if (uri.empty()) return HandlerResult::Ok(Json(nullptr));
    int line = 0;
    int character = 0;
    ReadPosition(params, &line, &character);
    return HandlerResult::Ok(FeatureHover(platform::uri_to_path(uri), line, character));
}

// =============================================================================
//  textDocument/references
// =============================================================================

Json LspServer::FeatureReferences(std::string_view path, int line, int character,
                                  bool include_declaration) {
    auto project = ProjectForPath(path);
    if (!project.has_value()) return Json::array();
    const auto index = IndexForProject(*project);
    if (!index) return Json::array();
    return features::References(*index, path, line, character, include_declaration);
}

HandlerResult LspServer::HandleReferences(const Json& params) {
    const std::string uri = TextDocumentUri(params);
    if (uri.empty()) return HandlerResult::Ok(Json::array());
    int line = 0;
    int character = 0;
    ReadPosition(params, &line, &character);
    const bool include_declaration =
        jsonrpc::GetBool(jsonrpc::GetObject(params, "context"), "includeDeclaration", true);
    return HandlerResult::Ok(FeatureReferences(platform::uri_to_path(uri), line, character,
                                               include_declaration));
}

// =============================================================================
//  textDocument/inlayHint
// =============================================================================

Json LspServer::FeatureInlayHints(std::string_view path) {
    auto project = ProjectForPath(path);
    if (!project.has_value()) return Json::array();
    const auto index = IndexForProject(*project);
    if (!index) return Json::array();
    return features::InlayHints(*index, path);
}

HandlerResult LspServer::HandleInlayHint(const Json& params) {
    const std::string uri = TextDocumentUri(params);
    if (uri.empty()) return HandlerResult::Ok(Json::array());
    return HandlerResult::Ok(FeatureInlayHints(platform::uri_to_path(uri)));
}

// =============================================================================
//  textDocument/completion
// =============================================================================

Json LspServer::FeatureCompletion(std::string_view path, int line, int character) {
    // completion 要看"光标所在行"，所以必须有当前文本（缓冲区优先）
    const std::optional<std::string> text = LoadText(std::string(path));
    const std::string_view view =
        text.has_value() ? std::string_view(*text) : std::string_view();

    // .adc / .sdc：约束文档（B10）
    if (IsConstraintDocument(path)) {
        std::vector<constraints::Port> ports;
        if (auto project = ProjectForPath(path); project.has_value()) {
            if (const auto index = IndexForProject(*project); index) {
                ports = TopPortsForPath(*project, *index);
            }
        }
        return ConstraintCompletion(path, view, line, character, ports);
    }

    auto project = ProjectForPath(path);
    if (!project.has_value()) return Json::object();
    const auto index = IndexForProject(*project);
    if (!index) return Json::object();
    // 候选集（模块 + 实例化模板 + 745 器件原语 + 48 关键字）全在 features::Completion 里，
    // 这里**不要**再追加任何东西：MCP 的 fpga_completion 调的是同一个函数。
    return features::Completion(*index, path, view, line, character);
}

HandlerResult LspServer::HandleCompletion(const Json& params) {
    const std::string uri = TextDocumentUri(params);
    if (uri.empty()) return HandlerResult::Ok(Json::object());
    int line = 0;
    int character = 0;
    ReadPosition(params, &line, &character);
    return HandlerResult::Ok(FeatureCompletion(platform::uri_to_path(uri), line, character));
}

}  // namespace fpgalsp::server