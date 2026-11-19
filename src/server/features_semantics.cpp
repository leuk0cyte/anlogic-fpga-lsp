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

Json LspServer::FeatureSemanticTokens(std::string_view path) {
    Json result = Json::object();
    result["data"] = Json::array();

    const std::string path_string(path);
    if (path_string.empty()) return result;

    // C1：只读工程 -> shared 访问器；shared_ptr 持有到 IndexForProject 返回。
    const std::shared_ptr<const project::Project> project = ProjectForPathShared(path);
    if (project == nullptr) return result;  // Python: index is None -> {"data": []}

    const std::shared_ptr<const hdl::HdlIndex> index = IndexForProject(*project);
    if (!index) return result;

    const std::optional<std::string> text = LoadText(path_string);
    if (!text.has_value()) return result;

    const std::vector<semantics::SemToken> tokens =
        semantics::SemanticTokens(index.get(), path, *text);
    const std::vector<int> encoded = semantics::EncodeTokens(tokens);

    Json data = Json::array();
    data.get_ref<nlohmann::json::array_t&>().reserve(encoded.size());
    for (const int value : encoded) data.push_back(value);
    result["data"] = std::move(data);
    return result;
}

// =============================================================================
//  textDocument/foldingRange
// =============================================================================

Json LspServer::FeatureFoldingRanges(std::string_view path) {
    const std::string path_string(path);
    if (path_string.empty()) return Json::array();

    const std::optional<std::string> text = LoadText(path_string);
    if (!text.has_value()) return Json::array();

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

    std::optional<std::string> text;
    if (!path.empty()) text = LoadText(path);
    if (!text.has_value()) {
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
    // C++ 侧语义高亮**始终**走内嵌 Verible 词法流（见 src/semantics/semantics.cpp），
    // 因此这两个标志恒为 true；Python 是"外部 verible 可用时"才 true。
    result["verible"] = true;
    result["veribleHighlight"] = true;
    return result;
}

}  // namespace fpgalsp::server