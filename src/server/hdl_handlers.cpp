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
// =============================================================================

#include <string>
#include <string_view>

#include "hdl/hdl_index.h"
#include "jsonrpc/json_util.h"
#include "platform/path.h"
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

}  // namespace

// =============================================================================
//  textDocument/documentSymbol
// =============================================================================

Json LspServer::FeatureDocumentSymbols(std::string_view path) {
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
    auto project = ProjectForPath(path);
    if (!project.has_value()) return Json::object();
    const auto index = IndexForProject(*project);
    if (!index) return Json::object();
    // completion 要看"光标所在行"，所以必须有当前文本（缓冲区优先）
    const std::optional<std::string> text = LoadText(std::string(path));
    const std::string_view view =
        text.has_value() ? std::string_view(*text) : std::string_view();
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