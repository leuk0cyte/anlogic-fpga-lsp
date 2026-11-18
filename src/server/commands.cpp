// =============================================================================
//  src/server/commands.cpp —— 自定义 v2 方法 + workspace/executeCommand 分发
// =============================================================================
//  权威协议：Y:\fpga-lsp\PROTOCOL-V2.md（§1 moduleTree / §2 reports / §3 flowStatus
//  / §5 命令表 / v6 工程发现 & 多 TD）
//  参考实现：Y:\fpga-lsp\fpgalsp\server.py
//      on_fpgaLsp_moduleTree / on_fpgaLsp_reports / on_fpgaLsp_flowStatus
//      on_workspace_executeCommand
//
//  ⚠ 占位命令必须**显式返回** `{"ok": false, "error": "not implemented"}`。
//    静默不返回会让 LSP 客户端永远等这条响应（表现为命令"卡住"）。
//    已实现 / 占位的完整两栏清单见 COMMANDS.md。
// =============================================================================

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "flow/flow_runner.h"
#include "flow/programmer.h"
#include "flow/strategy.h"
#include "hdl/hdl_index.h"
#include "server/features.h"
#include "jsonrpc/json_util.h"
#include "platform/log.h"
#include "platform/path.h"
#include "server/lsp_server.h"
#include "td/td_device.h"
#include "td/td_diagnostics.h"
#include "td/td_install.h"
#include "td/td_output.h"
#include "td/td_session.h"

namespace fpgalsp::server {
namespace {

// -----------------------------------------------------------------------------
// 小工具
// -----------------------------------------------------------------------------

Json FlowObject() {
    Json flow = Json::object();
    flow["running"] = false;
    flow["stage"] = "idle";
    flow["elapsed"] = 0;
    flow["lastMessage"] = "";
    return flow;
}

// 真实 flow 状态 -> 协议 §2 的 flow 段。
// 未接入 / 没跑过时由调用方退回 FlowObject() 的 idle 形状。
Json FlowObjectFrom(const flow::FlowState& state) {
    Json flow = Json::object();
    flow["running"] = state.running;
    flow["stage"] = state.stage;
    flow["elapsed"] = state.elapsed;
    flow["lastMessage"] = state.last_message;
    // 以下字段是 v3+ 追加的，客户端会忽略不认识的键
    flow["stages"] = state.stages;
    flow["runId"] = state.run_id;
    flow["jobs"] = state.jobs;
    flow["ok"] = state.ok.has_value() ? Json(*state.ok) : Json(nullptr);
    if (!state.error.empty()) flow["error"] = state.error;
    return flow;
}

// flow 段的统一入口：runner 不存在时给 idle
Json CurrentFlowObject(const std::shared_ptr<flow::FlowRunner>& runner) {
    if (!runner) return FlowObject();
    return FlowObjectFrom(runner->Status());
}

HandlerResult OkObject(Json value) { return HandlerResult::Ok(std::move(value)); }

HandlerResult Failure(std::string error) {
    Json result = Json::object();
    result["ok"] = false;
    result["error"] = std::move(error);
    return OkObject(std::move(result));
}

// 占位命令的统一返回（参见文件头注释）
HandlerResult NotImplemented() { return Failure("not implemented"); }

std::string ToLower(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string LowerExtension(std::string_view path) {
    return ToLower(platform::path_extension(path));
}

// -----------------------------------------------------------------------------
// moduleTree 的树构造（基于 src/hdl 的真实符号索引）
// -----------------------------------------------------------------------------
//  语义逐条对齐 server.py 的 on_fpgaLsp_moduleTree：
//    * children 按源码例化顺序（hdl 的 instances 就是解析顺序）
//    * defined=false -> blackbox=true，range 指向**例化位置**，portCount=0
//    * 环路保护：同名模块已在栈上，或 depth >= 24 -> recursive=true + children=[]
//    * 全局 5000 节点预算 -> truncated；单节点 500 个子项 -> 该节点 truncated
//    * VHDL entity 与 IP 生成的 .v 都在索引里，因此**不会**被误判成黑盒
//      （对应 hdl.py 的 _vhdl_module_defs / ip_generated_hdl）
// -----------------------------------------------------------------------------

Json MakeRange(int line, int start_character, int end_character) {
    Json range = Json::object();
    Json start = Json::object();
    start["line"] = line;
    start["character"] = start_character;
    Json end = Json::object();
    end["line"] = line;
    end["character"] = end_character;
    range["start"] = std::move(start);
    range["end"] = std::move(end);
    return range;
}

// usedAt：例化处的位置（根节点为 null）
Json MakeUsedAt(const hdl::Instance& instance) {
    Json used = Json::object();
    used["uri"] = platform::path_to_uri(instance.file);
    const std::string& name = instance.name;
    used["range"] = MakeRange(instance.line, instance.character,
                              instance.character + static_cast<int>(name.size()));
    return used;
}

struct TreeBudget {
    int left = 5000;
    bool truncated = false;
};

Json BuildNode(const hdl::HdlIndex& index, const std::string& module_name,
               const hdl::Instance* instance, std::vector<std::string> stack, int depth,
               bool include_primitives, TreeBudget& budget) {
    if (budget.left <= 0) {
        budget.truncated = true;
        return Json(nullptr);
    }
    --budget.left;

    const hdl::Module* definition = index.ModuleByName(module_name);
    if (definition == nullptr) {
        // defined=false（IP / 原语 / 拼写错误）：客户端显示成黑盒，点击跳到 usedAt
        Json node = Json::object();
        node["module"] = module_name;
        node["instance"] = (instance != nullptr && !instance->name.empty()) ? instance->name
                                                                            : module_name;
        node["file"] = instance != nullptr ? instance->file : std::string();
        const int line = instance != nullptr ? instance->line : 0;
        const int character = instance != nullptr ? instance->character : 0;
        node["range"] =
            MakeRange(line, character, character + static_cast<int>(module_name.size()));
        node["defined"] = false;
        node["blackbox"] = true;
        node["recursive"] = false;
        node["portCount"] = 0;
        node["usedAt"] = instance != nullptr ? MakeUsedAt(*instance) : Json(nullptr);
        node["children"] = Json::array();
        return node;
    }

    Json node = Json::object();
    node["module"] = definition->name;
    node["instance"] = instance != nullptr ? instance->name : std::string();
    node["file"] = definition->file;
    node["range"] = MakeRange(definition->line, definition->character,
                              definition->character +
                                  static_cast<int>(definition->name.size()));
    node["defined"] = true;
    node["blackbox"] = false;
    node["recursive"] = false;
    node["portCount"] = definition->ports.size();
    node["usedAt"] = instance != nullptr ? MakeUsedAt(*instance) : Json(nullptr);

    const std::string lowered = ToLower(definition->name);
    if (std::find(stack.begin(), stack.end(), lowered) != stack.end() || depth >= 24) {
        // 环路保护：recursive=true + children=[]（PROTOCOL-V2.md §1）
        node["recursive"] = true;
        node["children"] = Json::array();
        return node;
    }

    Json children = Json::array();
    stack.push_back(lowered);
    for (const hdl::Instance* child : index.InstancesOfFile(definition->file)) {
        if (child->module.empty()) continue;
        // 只取"定义体行范围内"的例化（对应 hdl.py instances_of）
        if (child->line < definition->line || child->line > definition->end_line) continue;
        if (!include_primitives && index.ModuleByName(child->module) == nullptr &&
            hdl::IsPrimitiveGate(child->module)) {
            continue;
        }
        Json built = BuildNode(index, child->module, child, stack, depth + 1,
                              include_primitives, budget);
        if (built.is_null()) break;
        children.push_back(std::move(built));
        if (children.size() >= 500) {
            node["truncated"] = true;
            break;
        }
    }
    node["children"] = std::move(children);
    return node;
}

}  // namespace

// =============================================================================
//  fpgaLsp/moduleTree
// =============================================================================

HandlerResult LspServer::HandleModuleTree(const Json& params) {
    return OkObject(FeatureModuleTree(jsonrpc::GetString(params, "uri"),
                                 jsonrpc::GetBool(params, "includePrimitives", false)));
}

Json LspServer::FeatureModuleTree(std::string_view uri, bool include_primitives) {
    const auto project = uri.empty() ? CurrentProject()
                                     : ProjectForPath(platform::uri_to_path(std::string(uri)));
    if (!project.has_value()) {
        Json out = Json::object();
        out["ok"] = false;
        out["error"] = "No Anlogic .al project found";
        return out;
    }

    bool index_truncated = false;
    const std::shared_ptr<const hdl::HdlIndex> index =
        IndexForProject(*project, &index_truncated, nullptr);
    if (!index) {
        Json out = Json::object();
        out["ok"] = false;
        out["error"] = "No HDL index";
        return out;
    }

    // 真正的树构造在 features（LSP 与 MCP 共用），这里只补 index 级截断标志
    Json result = features::ModuleTree(*index, *project, include_primitives);
    if (jsonrpc::GetBool(result, "ok", false) && index_truncated) {
        result["truncated"] = true;
    }
    return result;
}
// =============================================================================
//  fpgaLsp/reports（占位：reports 模块未实现，字段结构按协议预留）
// =============================================================================

HandlerResult LspServer::HandleReports(const Json& params) {
    Json result = Json::object();
    result["ok"] = false;
    result["error"] = "reports module not implemented";

    const auto project = ProjectFromParams(params);
    if (!project.has_value()) {
        result["error"] = "No Anlogic .al project found";
    } else {
        result["project"] = project->name;
        result["al"] = project->al_path;
    }

    // PROTOCOL-V2.md §2 的全部字段都预留（客户端要能容忍缺字段，但结构先给全）
    result["generatedAt"] = nullptr;
    Json artifacts = Json::object();
    artifacts["bit"] = nullptr;
    artifacts["syn_db"] = nullptr;
    artifacts["route_db"] = nullptr;
    artifacts["runs"] = Json::array();
    result["artifacts"] = std::move(artifacts);
    result["flow"] = CurrentFlowObject(flow_runner());
    result["qor"] = nullptr;
    result["timing"] = nullptr;
    result["area"] = nullptr;
    result["flowStatus"] = Json::array();
    result["files"] = Json::object();
    return OkObject(std::move(result));
}

// =============================================================================
//  fpgaLsp/flowStatus —— 真实 flow 状态 + 最后 200 行日志（协议 §3）
// =============================================================================

HandlerResult LspServer::HandleFlowStatus(const Json& params) {
    (void)params;
    const std::shared_ptr<flow::FlowRunner> runner = flow_runner();

    Json result = Json::object();
    result["ok"] = true;
    Json flow = CurrentFlowObject(runner);
    Json log = Json::array();
    if (runner) {
        for (const std::string& line : runner->Status().log) log.push_back(line);
    }
    flow["log"] = std::move(log);
    result["flow"] = std::move(flow);
    return OkObject(std::move(result));
}

// =============================================================================
//  TD 摘要（fpgalsp.tdInfo / fpgalsp.info.td 共用）
// =============================================================================

Json LspServer::TdSummary() {
    const std::string configured =
        settings_.td_path.empty() ? td_explicit_ : settings_.td_path;
    const std::optional<td::TdChoice> choice = td::FindTd(configured);

    Json result = Json::object();
    if (!choice.has_value()) {
        result["ok"] = false;
        result["error"] = "No TangDynasty installation found";
        return result;
    }
    result["ok"] = true;
    result["root"] = choice->root;
    result["version"] = choice->version;
    result["source"] = choice->source;
    Json alternatives = Json::array();
    for (const auto& item : choice->alternatives) {
        Json alt = Json::object();
        alt["root"] = item.root;
        alt["version"] = item.version;
        alternatives.push_back(std::move(alt));
    }
    result["alternatives"] = std::move(alternatives);
    return result;
}

// =============================================================================
//  器件摘要
// =============================================================================

std::optional<DeviceSummary> LspServer::CachedDevice(const project::Project& project) {
    const std::string key = project.device + "|" + project.family + "|" +
                            (project.speed.has_value() ? *project.speed : std::string());
    {
        std::lock_guard<std::mutex> lock(device_mutex_);
        if (const auto it = device_mem_.find(key); it != device_mem_.end()) return it->second;
    }

    // 对应 Python _device_cached：先看工程目录下的磁盘缓存（不启动 TD）
    if (project.dir.empty()) return std::nullopt;
    const std::string cache_path = platform::path_join(
        platform::path_join(project.dir, ".fpgalsp/cache"),
        td::DeviceCacheKey(project.device, project.family, project.speed));
    const std::optional<std::string> text = docs_.GetText(platform::path_to_uri(cache_path));
    if (!text.has_value()) return std::nullopt;
    const Json root = Json::parse(*text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) return std::nullopt;
    const Json data = jsonrpc::GetObject(root, "data");
    if (!data.is_object()) return std::nullopt;

    DeviceSummary summary;
    summary.device = jsonrpc::GetString(data, "device", project.device);
    summary.family = jsonrpc::GetString(data, "family", project.family);
    summary.speed = project.speed;
    summary.db = jsonrpc::GetString(data, "db");
    summary.package = jsonrpc::GetString(data, "package");
    summary.pins = jsonrpc::GetObject(data, "pins").size();

    std::lock_guard<std::mutex> lock(device_mutex_);
    device_mem_[key] = summary;
    return summary;
}

namespace {

// 对应 Python: `Package\s+([A-Za-z0-9_]+)` 从 import_device 的输出里取封装名
std::string ExtractPackage(std::string_view text, std::string_view fallback) {
    constexpr std::string_view kNeedle = "Package ";
    const std::size_t at = text.find(kNeedle);
    if (at == std::string_view::npos) return ToLower(fallback);
    std::size_t pos = at + kNeedle.size();
    std::string name;
    while (pos < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[pos]);
        if (!std::isalnum(c) && c != '_') break;
        name.push_back(static_cast<char>(c));
        ++pos;
    }
    if (name.empty()) return ToLower(fallback);
    return ToLower(name);
}

std::string TdRootOf(const td::TdSession& session) {
    const std::string exe = session.td().exe;
    const std::size_t bin = exe.rfind("/bin/");
    return bin == std::string::npos ? std::string() : exe.substr(0, bin);
}

}  // namespace

std::optional<DeviceSummary> LspServer::LoadDeviceSummary(const project::Project& project,
                                                          td::TdSession& session,
                                                          std::string* error) {
    if (project.device.empty()) {
        if (error) *error = "project has no device";
        return std::nullopt;
    }

    const std::string root = TdRootOf(session);
    const std::string speed_suffix = td::SpeedArgSuffix(project.speed);

    std::string chosen;
    std::string import_output;
    for (const auto& candidate : td::DbCandidates(root, project.family)) {
        try {
            const std::string out = session.Run("import_device " + candidate + " -package " +
                                                    project.device + speed_suffix,
                                                90.0);
            if (!td::HasError(out)) {
                chosen = candidate;
                import_output = out;
                break;
            }
        } catch (const std::exception& exc) {
            if (error) *error = exc.what();
            return std::nullopt;
        }
    }
    if (chosen.empty()) {
        if (error) *error = "No device library supports " + project.device;
        return std::nullopt;
    }

    std::string fio;
    try {
        fio = session.Run("query_current_package -fio");
    } catch (const std::exception& exc) {
        if (error) *error = exc.what();
        return std::nullopt;
    }
    std::map<std::string, td::Pin> pins = td::ParseFio(fio);
    if (pins.empty()) {
        if (error) *error = "query_current_package -fio returned no pins (device " +
                            project.device + ")";
        return std::nullopt;
    }
    try {
        td::ParseClockIo(session.Run("query_current_package -clock_io"), pins);
        td::ParseLvds(session.Run("query_current_package -lvds"), pins);
        (void)td::ParseCfgIo(session.Run("query_current_package -cfg_io"));
    } catch (const std::exception& exc) {
        // 时钟/LVDS/专用脚只是增强信息，失败不算致命
        platform::default_logger().warn(std::string("device pin extras failed: ") + exc.what());
    }

    DeviceSummary summary;
    summary.device = project.device;
    summary.family = project.family;
    summary.speed = project.speed;
    summary.db = chosen;
    summary.package = ExtractPackage(import_output, project.device);
    summary.pins = pins.size();

    {
        std::lock_guard<std::mutex> lock(device_mutex_);
        const std::string key = project.device + "|" + project.family + "|" +
                                (project.speed.has_value() ? *project.speed : std::string());
        device_mem_[key] = summary;
    }
    return summary;
}

// =============================================================================
//  FeatureInfo —— 工程摘要（fpgalsp.info 与 MCP 工具共用同一实现）
// =============================================================================

Json LspServer::FeatureInfo() {
    const auto project = CurrentProject();
    if (!project.has_value()) {
        Json out = Json::object();
        out["ok"] = false;
        out["error"] = ".al project not found";
        return out;
    }
    const std::optional<DeviceSummary> device = CachedDevice(*project);

    Json files = Json::object();
    files["hdl"] = project->HdlFiles().size();
    files["adc"] = project->AdcFiles().size();
    files["sdc"] = project->SdcFiles().size();
    files["ip"] = project->IpFiles().size();

    Json warnings = Json::array();
    for (const auto& warning : project->warnings) warnings.push_back(warning);

    // 工作区根递归发现的工程摘要（深度 4、最多 20 个；对应 Python 的 discover_projects[:20]）
    Json projects = Json::array();
    const std::string base = RootPath().empty() ? project->dir : RootPath();
    const auto found = ::fpgalsp::project::DiscoverProjects(base, 4);
    for (std::size_t i = 0; i < found.size() && i < 20; ++i) {
        projects.push_back(ProjectSummaryOf(found[i]));
    }

    Json result = Json::object();
    result["ok"] = true;
    result["project"] = project->name;
    result["dir"] = project->dir;
    result["al"] = project->al_path;
    result["family"] = project->family;
    result["device"] = project->device;
    result["top"] = project->top;
    result["td_version"] = project->td_version;
    result["files"] = std::move(files);
    result["warnings"] = std::move(warnings);
    // ⚠ device_loaded / td_session 是非确定性实时状态位（PYTHON-BUGS.md P-3）。
    //   这里是忠实复现，不要"改进"成确定性字段 —— 那是独立契约修正任务。
    result["device_loaded"] = device.has_value();
    result["pins"] = device.has_value() ? device->pins : 0;
    result["td_session"] = SessionAlive();
    result["td"] = TdSummary();
    result["projects"] = std::move(projects);
    return result;
}

// =============================================================================
//  workspace/executeCommand
// =============================================================================
//  已实现：fpgalsp.info / projects / setProject / tdInfo / device / check / restart
//          fpgalsp.cancelFlow（flow 未接入，如实回答"没在跑"）
//  占位  ：其余命令统一返回 {"ok": false, "error": "not implemented"}
//          —— 必须显式返回，否则客户端会挂住等响应。
// =============================================================================

namespace {

// 占位命令白名单（COMMANDS.md 的两栏清单以此为准）
const std::set<std::string>& PlaceholderCommands() {
    static const std::set<std::string> commands = {
        // flow / 烧录（阶段 4-δ）已实现，不在白名单里：
        //   fpgalsp.runFlow / cancelFlow / compareStrategies / cables / program / svfgen
        "fpgalsp.reports",
        "fpgalsp.runAnalysis",
        "fpgalsp.addSourceFile",
        "fpgalsp.setProjectName",
        "fpgalsp.updateProject",
        "fpgalsp.instantiate",
        "fpgalsp.copyInstantiation",
        "fpgalsp.symbolPath",
        "fpgalsp.duplicateModules",
        "fpgalsp.searchHierarchy",
        "fpgalsp.debugHighlight",
    };
    return commands;
}

}  // namespace

HandlerResult LspServer::HandleExecuteCommand(const Json& params) {
    const std::string command = jsonrpc::GetString(params, "command");
    const Json args = jsonrpc::GetArray(params, "arguments");

    if (command.empty()) {
        return Failure("unknown command ");
    }

    // ---- 已实现 ----

    if (command == "fpgalsp.info") {
        // 实现在 FeatureInfo()（MCP 工具也直接调它 —— 同一实现）
        return OkObject(FeatureInfo());
    }

    if (command == "fpgalsp.projects") {
        const std::string root = RootPath();
        std::string base = root;
        std::error_code ec;
        if (base.empty() || !std::filesystem::is_directory(base, ec)) {
            if (const auto project = CurrentProject(); project.has_value()) {
                base = project->dir;
            } else {
                base = root;
            }
        }
        const auto found = ::fpgalsp::project::DiscoverProjects(base, 6);
        Json projects = Json::array();
        for (const auto& al : found) projects.push_back(ProjectSummaryOf(al));
        Json result = Json::object();
        result["ok"] = true;
        result["root"] = base;
        result["count"] = found.size();
        result["projects"] = std::move(projects);
        result["selected"] = !settings_.project_file.empty() ? settings_.project_file
                                                             : project_override_;
        return OkObject(std::move(result));
    }

    if (command == "fpgalsp.setProject") {
        const Json options = args.empty() || !args.front().is_object() ? Json::object()
                                                                       : args.front();
        const std::string al =
            jsonrpc::GetString(options, "al");  // trim 见下（.strip() 语义）
        std::string trimmed = al;
        while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) {
            trimmed.erase(trimmed.begin());
        }
        while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) {
            trimmed.pop_back();
        }
        if (trimmed.empty()) {
            // 空串 = 恢复自动发现（对应 Python：只清 override，不清诊断表）
            project_override_.clear();
            Json result = Json::object();
            result["ok"] = true;
            result["selected"] = "";
            return OkObject(std::move(result));
        }
        const std::string normalized = platform::normalize_path(trimmed);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(normalized, ec) ||
            LowerExtension(normalized) != ".al") {
            return Failure("Not a valid .al file: " + normalized);
        }
        project_override_ = normalized;
        InvalidateProjectCaches();
        platform::default_logger().info("project override -> " + normalized);
        Json result = Json::object();
        result["ok"] = true;
        result["selected"] = normalized;
        result["summary"] = ProjectSummaryOf(normalized);
        return OkObject(std::move(result));
    }

    if (command == "fpgalsp.tdInfo") {
        Json result = Json::object();
        result["ok"] = true;
        result["td"] = TdSummary();
        return OkObject(std::move(result));
    }

    if (command == "fpgalsp.device") {
        return CommandDevice();
    }

    if (command == "fpgalsp.check") {
        std::optional<::fpgalsp::project::Project> project = CurrentProject();
        if (!project.has_value() && !args.empty()) {
            const Json first = args.front();
            if (first.is_string()) {
                project = ProjectForPath(platform::uri_to_path(first.get<std::string>()));
            } else if (first.is_object()) {
                const std::string uri = jsonrpc::GetString(first, "uri");
                if (!uri.empty()) project = ProjectForPath(platform::uri_to_path(uri));
            }
        }
        if (!project.has_value()) {
            return Failure(".al project not found");
        }
        // 触发一次权威 TD 检查（异步），响应立刻返回 —— 诊断随后通过
        // textDocument/publishDiagnostics 推送。
        //
        // ⚠ Python 版在这之前还有一行 `self.td_running = False`（server.py:2083），
        //   那会让并发的第二次 check 再起一个 TD 线程（同一个会话管道被两个线程写）。
        //   这里**故意不复现**：并发保护完全交给 state_mutex_ 保护的 td_running_。
        //   详见 ../tests/PYTHON-BUGS.md P-8。
        ScheduleTdCheck(*project);
        Json result = Json::object();
        result["ok"] = true;
        result["project"] = project->name;
        result["queued"] = true;
        return OkObject(std::move(result));
    }

    if (command == "fpgalsp.restart") {
        // 关掉当前 TD 会话（下一次检查会重新建）。每次检查本来就是新会话，
        // 所以这里主要作用是"立刻释放可能还在跑的 TD"。
        KillSession();
        Json result = Json::object();
        result["ok"] = true;
        return OkObject(std::move(result));
    }

    if (command == "fpgalsp.cancelFlow") {
        // runner 为 null 或没在跑时保持 {ok:true, note:"not running"}（客户端契约）
        const std::shared_ptr<flow::FlowRunner> runner = flow_runner();
        if (!runner) {
            Json result = Json::object();
            result["ok"] = true;
            result["note"] = "not running";
            return OkObject(std::move(result));
        }
        return OkObject(runner->Cancel());
    }

    // ---- Flow / 烧录（阶段 4-δ）----
    //  ⚠ 这里的每个分支都必须**自己 return**：下面 PlaceholderCommands() 白名单里
    //    已经把这些命令删掉了，走到末尾会变成 "unknown command"。

    if (command == "fpgalsp.runFlow") {
        const auto project = CurrentProject();
        if (!project.has_value()) return Failure(".al project not found");
        const std::string configured =
            settings_.td_path.empty() ? td_explicit_ : settings_.td_path;
        const std::optional<td::TdChoice> td = td::FindTd(configured);
        if (!td.has_value()) return Failure("No TangDynasty installation found");

        const Json options =
            args.empty() || !args.front().is_object() ? Json::object() : args.front();

        std::vector<std::string> stages;
        if (const Json stage_value = jsonrpc::GetArray(options, "stages"); !stage_value.empty()) {
            for (const auto& item : stage_value) {
                if (item.is_string()) stages.push_back(item.get<std::string>());
            }
        } else if (const std::string single = jsonrpc::GetString(options, "stage");
                   !single.empty()) {
            stages.push_back(single);
        }
        // 缺省等价于 GUI 的 "Generate Bitstream"
        if (stages.empty()) stages.push_back("all");

        const int jobs = jsonrpc::GetInt(options, "jobs", 6);
        const std::shared_ptr<flow::FlowRunner> runner = EnsureFlowRunner(*project, *td);
        return OkObject(runner->Start(stages, jobs));
    }

    if (command == "fpgalsp.compareStrategies") {
        const auto project = CurrentProject();
        if (!project.has_value()) return Failure(".al project not found");
        const std::string configured =
            settings_.td_path.empty() ? td_explicit_ : settings_.td_path;
        const std::optional<td::TdChoice> td = td::FindTd(configured);
        if (!td.has_value()) return Failure("No TangDynasty installation found");

        const Json options =
            args.empty() || !args.front().is_object() ? Json::object() : args.front();
        std::vector<std::string> wanted;
        if (const Json list = jsonrpc::GetArray(options, "strategies"); !list.empty()) {
            for (const auto& item : list) {
                if (item.is_string()) wanted.push_back(item.get<std::string>());
            }
        }
        flow::CompareOptions compare_options;
        compare_options.jobs = jsonrpc::GetInt(options, "jobs", 2);
        return OkObject(flow::CompareStrategies(*project, *td, wanted, compare_options));
    }

    if (command == "fpgalsp.cables") {
        const auto project = CurrentProject();
        if (!project.has_value()) return Failure(".al project not found");
        const std::string configured =
            settings_.td_path.empty() ? td_explicit_ : settings_.td_path;
        const std::optional<td::TdChoice> td = td::FindTd(configured);
        if (!td.has_value()) return Failure("No TangDynasty installation found");

        const Json options =
            args.empty() || !args.front().is_object() ? Json::object() : args.front();
        flow::ProgramOptions program_options;
        program_options.bit_path = jsonrpc::GetString(options, "bit");
        return OkObject(flow::Inspect(*td, *project, program_options));
    }

    if (command == "fpgalsp.program") {
        const auto project = CurrentProject();
        if (!project.has_value()) return Failure(".al project not found");
        const std::string configured =
            settings_.td_path.empty() ? td_explicit_ : settings_.td_path;
        const std::optional<td::TdChoice> td = td::FindTd(configured);
        if (!td.has_value()) return Failure("No TangDynasty installation found");

        const Json options =
            args.empty() || !args.front().is_object() ? Json::object() : args.front();
        flow::ProgramOptions program_options;
        program_options.bit_path = jsonrpc::GetString(options, "bit");
        program_options.mode = jsonrpc::GetString(options, "mode", "sram");
        // ⚠ 三条安全规则都写死在 flow::Program() 里：这里只负责**如实透传**，
        //   不做任何"缺 confirm 就当 true"之类的便利处理。
        program_options.confirm = jsonrpc::GetBool(options, "confirm", false);
        program_options.allow_mismatch = jsonrpc::GetBool(options, "allowMismatch", false);
        return OkObject(flow::Program(*td, *project, program_options));
    }

    if (command == "fpgalsp.svfgen") {
        const auto project = CurrentProject();
        if (!project.has_value()) return Failure(".al project not found");
        const std::string configured =
            settings_.td_path.empty() ? td_explicit_ : settings_.td_path;
        const std::optional<td::TdChoice> td = td::FindTd(configured);
        if (!td.has_value()) return Failure("No TangDynasty installation found");

        const Json options =
            args.empty() || !args.front().is_object() ? Json::object() : args.front();
        flow::ProgramOptions program_options;
        program_options.bit_path = jsonrpc::GetString(options, "bit");
        program_options.svf_path = jsonrpc::GetString(options, "svf");
        program_options.scan_mode = jsonrpc::GetString(options, "scanMode", "prog_sram");
        return OkObject(flow::GenerateSvf(*td, *project, program_options));
    }

    // ---- 占位（必须显式返回 not implemented）----
    if (PlaceholderCommands().count(command) != 0) {
        return NotImplemented();
    }

    return Failure("unknown command " + command);
}

// =============================================================================
//  fpgalsp.device
// =============================================================================

HandlerResult LspServer::CommandDevice() {
    const auto project = CurrentProject();
    if (!project.has_value()) {
        return Failure(".al project not found");
    }

    std::string error;
    std::shared_ptr<td::TdSession> session = CreateSession(*project, &error);
    if (!session) {
        return Failure(error);
    }

    const std::optional<DeviceSummary> summary = LoadDeviceSummary(*project, *session, &error);
    session->Kill();  // import_device 之后本会话可复用，但保持"用完即杀"的简单策略
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        if (session_ == session) session_.reset();
    }
    if (!summary.has_value()) {
        return Failure(error);
    }

    Json result = Json::object();
    result["ok"] = true;
    result["device"] = summary->device;
    result["db"] = summary->db;
    result["pins"] = summary->pins;
    result["package"] = summary->package;
    return OkObject(std::move(result));
}

}  // namespace fpgalsp::server
