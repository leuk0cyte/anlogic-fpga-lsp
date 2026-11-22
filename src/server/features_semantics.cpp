// =============================================================================
//  src/server/features_semantics.cpp —— 语义高亮 / 折叠 / debugHighlight
// =============================================================================
//  OWNER: task B2 (parity-lsp)。B1 只放可编译的 stub，签名与 dispatch 已固定。
//
//  本文件**只做协议编解码**，全部语义逻辑在 src/semantics/（LSP 与 MCP 共用一份）：
//      textDocument/semanticTokens/full -> semantics::SemanticTokens + EncodeTokens
//      textDocument/foldingRange        -> semantics::FoldingRanges
//      fpgalsp.debugHighlight           -> semantics::SemanticTokens + Summarize
//
//  与 Python（server.py:1787-1852 / 2244-2267）的字段契约：
//    semanticTokens   {"data": [deltaLine, deltaStart, length, type, 0, ...]}
//    foldingRange     [{"startLine", "endLine"} | {"startLine","endLine","kind":"comment"}]
//    debugHighlight   {ok, path, tokens, counts{type: n}, verible, veribleHighlight}
//
//  ⚠ 失败语义：没有工程 / 读不到文本 -> 合法的空结果（`{"data":[]}` / `[]`），
//    绝不 -32601；debugHighlight 例外，它按 Python 返回 {ok:false,error}。
// =============================================================================

#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hdl/hdl_index.h"
#include "jsonrpc/json_util.h"
#include "platform/path.h"
#include "semantics/semantics.h"
#include "server/lsp_server.h"

namespace fpgalsp::server {

// =============================================================================
//  textDocument/semanticTokens/full
// =============================================================================

LspServer::SemanticTokenContext LspServer::SemanticTokenContextFor(std::string_view path) {
    SemanticTokenContext context;
    context.path = std::string(path);
    if (context.path.empty()) return context;
    // `fpgaLsp.verible.highlight=false`：不生成任何语义 token（客户端就不会上色）。
    if (!settings_.verible_highlight) return context;

    // C1：只读工程 -> shared 访问器；shared_ptr 持有到使用点结束。
    const std::shared_ptr<const project::Project> project = ProjectForPathShared(path);
    // 打开文档的版本号：delta 短路判据（未打开 = nullopt -> 不短路）。
    context.version = docs_.GetVersionByPath(context.path);
    if (project == nullptr) return context;  // Python: index is None -> 空 token

    context.index = IndexForProject(*project);
    if (context.index && index_service_) {
        context.index_generation = index_service_->stats().generation;
        context.have_generation = true;
    }
    return context;
}

std::shared_ptr<const std::vector<int>> LspServer::ComputeSemanticTokens(
    const SemanticTokenContext& context, const std::shared_ptr<const std::string>& text) {
    if (!context.index || !text) return nullptr;
    // P2-A：行级增量。缓存键是"索引内容指纹"（不是 generation —— 每次 didChange
    // IndexService 都会重装快照并让 generation +1，拿它当键永远不会命中）。
    // 索引内容真变了时 semantics 内部会自动全量，所以这里无条件走缓存入口。
    const std::shared_ptr<const std::vector<semantics::SemToken>> tokens =
        semantics::SemanticTokensCached(context.index.get(), context.path, text,
                                        context.index_generation);
    return std::make_shared<const std::vector<int>>(semantics::EncodeTokens(*tokens));
}

// =============================================================================
//  textDocument/foldingRange
// =============================================================================

Json LspServer::FeatureFoldingRanges(std::string_view path) {
    const std::string path_string(path);
    if (path_string.empty()) return Json::array();

    // P1-2：共享缓冲文本（零整文件拷贝）。
    const std::shared_ptr<const std::string> text = LoadTextShared(path_string);
    if (!text) return Json::array();

    Json ranges = Json::array();
    for (const semantics::FoldingRange& range : semantics::FoldingRanges(*text)) {
        Json item = Json::object();
        item["startLine"] = range.start_line;
        item["endLine"] = range.end_line;
        if (range.comment) item["kind"] = "comment";
        ranges.push_back(std::move(item));
    }
    return ranges;
}

// =============================================================================
//  fpgalsp.debugHighlight
// =============================================================================
//  Python（server.py:2244-2267）：参数是 executeCommand 的 arguments[0].uri。
//  返回 token 总数与按类型的计数，扩展用它在 .fpgalsp/logs/highlight.txt 里
//  自证"语义高亮到底有没有 token"。

Json LspServer::FeatureDebugHighlight(const Json& params) {
    Json result = Json::object();

    const Json arguments = jsonrpc::GetArray(params, "arguments");
    Json options = Json::object();
    if (!arguments.empty() && arguments.front().is_object()) options = arguments.front();
    const std::string uri = jsonrpc::GetString(options, "uri");

    std::string path;
    if (!uri.empty()) path = platform::uri_to_path(uri);

    std::shared_ptr<const std::string> text;
    if (!path.empty()) text = LoadTextShared(path);
    if (!text) {
        result["ok"] = false;
        result["error"] = "document not open: " + uri;
        return result;
    }

    // C1：只读工程 -> shared 访问器。
    const std::shared_ptr<const project::Project> project = ProjectForPathShared(path);
    std::vector<semantics::SemToken> tokens;
    if (project != nullptr) {
        const std::shared_ptr<const hdl::HdlIndex> index = IndexForProject(*project);
        if (index) tokens = semantics::SemanticTokens(index.get(), path, *text);
    }
    const semantics::TokenStats stats = semantics::Summarize(tokens);

    result["ok"] = true;
    result["path"] = path;
    result["tokens"] = stats.total;
    Json counts = Json::object();
    for (const auto& [name, count] : stats.counts) counts[name] = count;
    result["counts"] = std::move(counts);
    // 这两个标志报告"当前是否真的在出语义高亮"：内嵌 Verible 词法流始终可用，
    // 但 `fpgaLsp.verible.highlight=false` 时 ComputeSemanticTokens 会返回空 token，
    // 所以这里必须跟随设置，而不是恒 true。
    result["verible"] = settings_.verible_highlight;
    result["veribleHighlight"] = settings_.verible_highlight;
    return result;
}

}  // namespace fpgalsp::server