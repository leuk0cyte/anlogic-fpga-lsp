// =============================================================================
//  src/server/features_commands.cpp —— IDE 类 executeCommand 的后端
// =============================================================================
//  OWNER: task B1a (features-ide)。B1 只放 stub，签名与 dispatch 已固定。
//
//  本文件只做三件事（真正的逻辑在 src/server/ide_features.{h,cpp}）：
//    1. 从 params 解出 uri / 工程 / 索引 / 位置
//    2. 调 ide:: 的纯函数
//    3. 把结果原样返回（字段就是 Python executeCommand 的字段，不加工）
//
//  为什么要拆：同一套逻辑 MCP 也要用（B6 会把 bridge.cpp 的重复实现改成调
//  ide:: 的纯函数）。LSP 的 params 是 {uri, position} 形态，MCP 的入参是
//  {file, line, column}（1-based），两者的**坐标换算**留在各自协议层。
//
//  契约（Python 字段，probe_lsp raw.json commands[6..14]）：
//    instantiate       {ok, module, instanceName, text, portCount} / module 为空时 {ok, modules}
//    copyInstantiation {ok, module, text, portCount}
//    symbolPath        {ok, name, path, instancePath, module}
//    duplicateModules  {ok, duplicates}
//    searchHierarchy   {ok, matches}
//    addSourceFile     {ok, added, rel, compileOrder}
//    setProjectName    {ok, changed, name}
//  失败一律 {ok:false, error}（与 Python 同口径），绝不抛、绝不 -32601。
// =============================================================================

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hdl/hdl_index.h"
#include "jsonrpc/json_util.h"
#include "platform/path.h"
#include "project/al_project.h"
#include "server/ide_features.h"
#include "server/lsp_server.h"

namespace fpgalsp::server {
namespace {

Json Failure(std::string message) {
    Json result = Json::object();
    result["ok"] = false;
    result["error"] = std::move(message);
    return result;
}

// Python server.py::_doc_from_params 的等价物：
//   LSP：params.uri 必须**已打开**（Python 未打开时 doc is None -> "document not open: <uri>"）
//   MCP：params.file（B6 复用），允许只读磁盘（没有缓冲区概念）
std::optional<std::string> DocumentPath(const lsp::DocumentStore& docs, const Json& params,
                                        std::string* error) {
    const std::string uri = jsonrpc::GetString(params, "uri");
    if (!uri.empty()) {
        if (!docs.IsOpen(uri)) {
            if (error != nullptr) *error = "document not open: " + uri;
            return std::nullopt;
        }
        return platform::uri_to_path(uri);
    }
    const std::string file = jsonrpc::GetString(params, "file");
    if (!file.empty()) return platform::normalize_path(file);
    if (error != nullptr) *error = "document not open: " + uri;
    return std::nullopt;
}

// LSP 的 position（0-based）。MCP 侧 1-based 换算由 bridge 负责（B6）。
void ReadPosition(const Json& params, int* line, int* character) {
    *line = 0;
    *character = 0;
    const Json position = jsonrpc::GetObject(params, "position");
    if (position.is_object()) {
        *line = jsonrpc::GetInt(position, "line", 0);
        *character = jsonrpc::GetInt(position, "character", 0);
        return;
    }
    // 兼容 file + line/character（内部/测试用；与 LSP position 同为 0-based）
    *line = jsonrpc::GetInt(params, "line", 0);
    *character = jsonrpc::GetInt(params, "character", 0);
}

}  // namespace

// =============================================================================
//  fpgalsp.instantiate
// =============================================================================

Json LspServer::FeatureInstantiation(const Json& params) {
    try {
        // C1：只读工程 -> shared 访问器（shared_ptr 持有到 IndexForProject 返回）。
        const std::shared_ptr<const project::Project> project = CurrentProjectShared();
        if (project == nullptr) return Failure(".al project not found");
        const std::shared_ptr<const hdl::HdlIndex> index = IndexForProject(*project);

        // Python: indent = options.get("indent") if isinstance(..., str) else "    "
        std::string indent = "    ";
        if (const auto it = params.find("indent");
            it != params.end() && it->is_string()) {
            indent = it->get<std::string>();
        }
        return ide::Instantiation(*index, jsonrpc::GetString(params, "module"), indent);
    } catch (const std::exception& error) {
        return Failure(error.what());
    } catch (...) {
        return Failure("unexpected error in fpgalsp.instantiate");
    }
}

// =============================================================================
//  fpgalsp.copyInstantiation
// =============================================================================

Json LspServer::FeatureCopyInstantiation(const Json& params) {
    try {
        std::string error;
        const std::optional<std::string> path = DocumentPath(docs_, params, &error);
        if (!path.has_value()) return Failure(error);

        // C1：只读工程 -> shared 访问器。
        const std::shared_ptr<const project::Project> project = ProjectForPathShared(*path);
        const std::shared_ptr<const hdl::HdlIndex> index =
            project != nullptr ? IndexForProject(*project) : nullptr;
        if (!index) return Failure("no project/index for " + *path);

        return ide::CopyInstantiation(*index, *path);
    } catch (const std::exception& error) {
        return Failure(error.what());
    } catch (...) {
        return Failure("unexpected error in fpgalsp.copyInstantiation");
    }
}

// =============================================================================
//  fpgalsp.symbolPath
// =============================================================================

Json LspServer::FeatureSymbolPath(const Json& params) {
    try {
        std::string error;
        const std::optional<std::string> path = DocumentPath(docs_, params, &error);
        if (!path.has_value()) return Failure(error);

        // C1：只读工程 -> shared 访问器。
        const std::shared_ptr<const project::Project> project = ProjectForPathShared(*path);
        if (project == nullptr) return Failure("no project/index");
        const std::shared_ptr<const hdl::HdlIndex> index = IndexForProject(*project);

        int line = 0;
        int character = 0;
        ReadPosition(params, &line, &character);
        return ide::SymbolPath(*index, *project, *path, std::max(0, line),
                               std::max(0, character));
    } catch (const std::exception& error) {
        return Failure(error.what());
    } catch (...) {
        return Failure("unexpected error in fpgalsp.symbolPath");
    }
}

// =============================================================================
//  fpgalsp.duplicateModules
// =============================================================================

Json LspServer::FeatureDuplicateModules(const Json& /*params*/) {
    try {
        // C1：只读工程 -> shared 访问器。
        const std::shared_ptr<const project::Project> project = CurrentProjectShared();
        if (project == nullptr) return Failure("no project/index");
        const std::shared_ptr<const hdl::HdlIndex> index = IndexForProject(*project);
        return ide::DuplicateModules(*index);
    } catch (const std::exception& error) {
        return Failure(error.what());
    } catch (...) {
        return Failure("unexpected error in fpgalsp.duplicateModules");
    }
}

// =============================================================================
//  fpgalsp.searchHierarchy
// =============================================================================

Json LspServer::FeatureSearchHierarchy(const Json& params) {
    try {
        // C1：只读工程 -> shared 访问器。
        const std::shared_ptr<const project::Project> project = CurrentProjectShared();
        if (project == nullptr) return Failure("no project/index");
        const std::shared_ptr<const hdl::HdlIndex> index = IndexForProject(*project);

        // kind / limit 是 MCP 侧参数（Python mcp_bridge.search_hierarchy 有）；
        // LSP 侧 extension 只传 query，缺省 both / 200。
        std::string kind = "both";
        if (const auto it = params.find("kind"); it != params.end() && it->is_string()) {
            kind = it->get<std::string>();
            for (char& c : kind) {
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            }
        }
        int limit = 200;
        if (const auto it = params.find("limit");
            it != params.end() && it->is_number_integer()) {
            limit = std::clamp(it->get<int>(), 1, 2000);
        }

        Json result = ide::SearchHierarchy(*index, jsonrpc::GetString(params, "query"), limit);
        if (kind == "module" || kind == "instance") {
            Json filtered = Json::array();
            for (const Json& item : result["matches"]) {
                if (item.value("kind", std::string()) == kind) filtered.push_back(item);
            }
            result["matches"] = std::move(filtered);
        }
        return result;
    } catch (const std::exception& error) {
        return Failure(error.what());
    } catch (...) {
        return Failure("unexpected error in fpgalsp.searchHierarchy");
    }
}

// =============================================================================
//  fpgalsp.addSourceFile（写 .al 的 <Source_Files>）
// =============================================================================

Json LspServer::FeatureAddSourceFile(const Json& params) {
    try {
        const std::string file_value = jsonrpc::GetString(params, "file");
        if (file_value.empty()) {
            return Failure("No .al project found or the file path is empty");
        }
        // Python: project = self._project(source or self._root_path())
        const std::string source = platform::normalize_path(file_value);
        // C1：只读工程（只用到 al_path）-> shared 访问器，省掉整份深拷贝。
        const std::shared_ptr<const project::Project> project = ProjectForPathShared(source);
        if (project == nullptr) {
            return Failure("No .al project found or the file path is empty");
        }

        Json result = ide::AddSourceFile(project->al_path, source);
        if (result.value("ok", false)) InvalidateProjectCaches();
        return result;
    } catch (const std::exception& error) {
        return Failure(error.what());
    } catch (...) {
        return Failure("unexpected error in fpgalsp.addSourceFile");
    }
}

// =============================================================================
//  fpgalsp.setProjectName（写 .al 的 <Name>）
// =============================================================================

Json LspServer::FeatureSetProjectName(const Json& params) {
    try {
        // C1：只读工程（只用到 al_path）-> shared 访问器。
        const std::shared_ptr<const project::Project> project = CurrentProjectShared();
        if (project == nullptr) return Failure(".al project not found");

        Json result = ide::SetProjectName(project->al_path, jsonrpc::GetString(params, "name"));
        if (result.value("ok", false)) InvalidateProjectCaches();
        return result;
    } catch (const std::exception& error) {
        return Failure(error.what());
    } catch (...) {
        return Failure("unexpected error in fpgalsp.setProjectName");
    }
}

}  // namespace fpgalsp::server