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
//    已实现 / 占位的完整两栏清单见 doc/commands.md。
// =============================================================================

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "flow/flow_runner.h"
#include "flow/probe.h"
#include "flow/programmer.h"
#include "flow/strategy.h"
#include "hdl/hdl_index.h"
#include "reports/artifacts.h"
#include "reports/reports_aggregate.h"
#include "server/async_dispatcher.h"
#include "server/features.h"
#include "jsonrpc/json_util.h"
#include "platform/log.h"
#include "platform/path.h"
#include "constraints/adc.h"
#include "constraints/port.h"
#include "server/constraints_features.h"
#include "analysis/analysis.h"
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

// 占位命令的返回值现在由各 Feature stub 自己给（带 "not implemented: <缺什么>"），
// 这里不再保留统一的 NotImplemented()，避免出现"看起来实现了"的死代码。

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

HandlerResult LspServer::HandleModuleChildren(const Json& params) {
    return OkObject(FeatureModuleChildren(params));
}

Json LspServer::FeatureModuleChildren(const Json& params) {
    const std::string uri = jsonrpc::GetString(params, "uri");
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
    const std::string module = jsonrpc::GetString(params, "module");
    const int offset = jsonrpc::GetInt(params, "offset", 0);
    const int limit = jsonrpc::GetInt(params, "limit", 200);
    const bool include_primitives = jsonrpc::GetBool(params, "includePrimitives", false);
    return features::ModuleChildren(*index, *project, module, offset, limit,
                                    include_primitives);
}

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
//  fpgaLsp/reports —— 真实报告（src/reports）+ 策略/路由库装饰
// =============================================================================

namespace {

// 对应 Python analysis.newest_routed_db：<project>_Runs/phy_1 下最新的 *_pr.db。
std::string NewestRoutedDb(const project::Project& project) {
    const std::string phy =
        reports::RunsDirectory(project.dir, project.name) + "/phy_1";
    std::vector<std::string> candidates;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(phy, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const std::string name = entry.path().filename().string();
        if (name.size() >= 6 && name.compare(name.size() - 6, 6, "_pr.db") == 0) {
            candidates.push_back(platform::normalize_path(entry.path().string()));
        }
    }
    if (candidates.empty()) return "";
    std::sort(candidates.begin(), candidates.end(), [](const std::string& a, const std::string& b) {
        std::error_code ea;
        std::error_code eb;
        const auto ta = std::filesystem::last_write_time(std::filesystem::path(a), ea);
        const auto tb = std::filesystem::last_write_time(std::filesystem::path(b), eb);
        return !ea && !eb ? ta > tb : a > b;
    });
    return candidates.front();
}

}  // namespace

HandlerResult LspServer::HandleReports(const Json& params) {
    return OkObject(FeatureReports(params));
}

Json LspServer::FeatureReports(const Json& params) {
    const auto project = ProjectFromParams(params);
    if (!project.has_value()) {
        Json out = Json::object();
        out["ok"] = false;
        out["error"] = "No Anlogic .al project found";
        out["flow"] = CurrentFlowObject(flow_runner());
        return out;
    }

    // Phase 2 / A3：按 section 走 ReportStore 缓存；默认 kAll = Phase 1 全量行为。
    std::uint32_t mask = reports::sections::kAll;
    std::string section_text;
    if (const auto it = params.find("section"); it != params.end()) {
        if (it->is_string()) {
            section_text = it->get<std::string>();
            bool ok = false;
            mask = reports::sections::Parse(section_text, &ok);
            if (!ok) mask = reports::sections::kAll;
        } else if (it->is_array()) {
            std::vector<std::string> names;
            for (const Json& item : *it) {
                if (item.is_string()) names.push_back(item.get<std::string>());
            }
            bool ok = false;
            mask = reports::sections::MaskFromList(names, &ok);
            if (!ok) mask = reports::sections::kAll;
        }
    }
    // 可选 limit：只裁剪副本（快照共享不可变），用于 timing/area 的大表。
    // F3：limit 现在是**构建期**生效（进 ReportStore 的快照键），所以要在 Get 之前解析；
    //     原来在返回的副本上裁剪是 no-op（只找节点自己的 paths，而真实形状是
    //     data.timing.paths / groups[].paths），见 doc/audit/2026-11-19/fix/F3.md。
    const int limit = jsonrpc::GetInt(params, "limit", 0);
    const std::uint32_t trim_limit =
        limit > 0 ? static_cast<std::uint32_t>(std::min(limit, 100000)) : 0u;
    // 分节请求默认不带 raw 正文（timing/area 的原始报告可达 10 MB+）；
    // 需要时显式 include_raw=true。全量请求（无 section）保持 Phase 1 行为。
    const bool include_raw = jsonrpc::GetBool(params, "include_raw", false);

    // P-C3：revision 条件请求。客户端带上"我看到的那一版"，服务端没变就直接回
    // `{unchanged:true}` —— 连快照都不用取（报告面板一次 flow 完成后的 4 次
    // section 重取会退化成 1 次真取 + 3 次空应答）。
    // 用全局 revision（ReportStore::revision()）：任何工程任何报告失效都会 +1，
    // 是"是否需要重发"的粗粒度但正确的判据。
    const std::uint64_t revision_before = report_store_->revision();
    if (const auto it = params.find("ifRevision");
        it != params.end() && it->is_number_integer() &&
        static_cast<std::uint64_t>(it->get<std::int64_t>()) == revision_before) {
        Json unchanged = Json::object();
        unchanged["ok"] = true;
        unchanged["unchanged"] = true;
        unchanged["revision"] = revision_before;
        return unchanged;
    }

    const auto snapshot = report_store_->Get(*project, mask, /*force=*/false, trim_limit);
    // ⚠ C2（性能审计）：这里原来是「整份克隆快照 -> 再 erase("raw")」。分节请求下
    //   raw 正文恰恰是最贵的那 10 MB，先拷一遍再立刻丢掉 = 246-267 ms/请求
    //   （doc/audit/2026-11-19/perf/CPU-PROFILE.md §0.6）。
    //   改成「拷贝时就不带 raw」：遍历路径与旧的 drop_raw 完全一致（根一层、
    //   timing/area/qor 各一层、syn/phy 递归），所以输出 JSON 逐字节不变，只是
    //   不再为即将丢弃的字段付出拷贝代价。快照本身仍是不可变的共享对象，不动。
    Json data = [&]() -> Json {
        if (include_raw || section_text.empty() || !snapshot->data.is_object()) {
            return snapshot->data;
        }
        // 只去掉这一层自己的 "raw"，与 drop_raw 的 node.erase("raw") 等价。
        const auto shallow = [](const Json& object) {
            Json out = Json::object();
            for (auto it = object.begin(); it != object.end(); ++it) {
                if (it.key() != "raw") out[it.key()] = it.value();
            }
            return out;
        };
        const auto deep = [&shallow](auto&& self, const Json& node) -> Json {
            Json out = Json::object();
            for (auto it = node.begin(); it != node.end(); ++it) {
                const std::string& key = it.key();
                if (key == "raw") continue;
                if (it->is_object() && (key == "timing" || key == "area" || key == "qor")) {
                    out[key] = shallow(*it);
                } else if (it->is_object() && (key == "syn" || key == "phy")) {
                    out[key] = self(self, *it);
                } else {
                    out[key] = *it;
                }
            }
            return out;
        };
        return deep(deep, snapshot->data);
    }();
    // B-B2：把时序端点解析到源码。reports 层拿不到索引（`timing.cpp` 因此恒
    // resolved=false / reason="hdl-index-unavailable"），这里在服务端注入索引后重算。
    // ⚠ 只改这份 `data` 副本：快照本身是共享不可变的，绝不能就地改。
    if (!project->top.empty()) {
        bool ignored_truncation = false;
        if (const std::shared_ptr<const hdl::HdlIndex> index =
                IndexForProject(*project, &ignored_truncation, nullptr);
            index != nullptr) {
            features::ResolveTimingEndpoints(*index, *project, data);
            // B-B7：面积热点 -> 源码（area.modules 里的模块定义位置）
            features::ResolveAreaModules(*index, *project, data);
        }
    }

    if (!section_text.empty()) data["section"] = section_text;
    data["project"] = project->name;
    data["al"] = project->al_path;
    // 冷构建会让 revision 前进，所以数据路径要报**构建之后**的值，否则客户端
    // 拿到的 revision 立刻就是旧的、条件请求永远命不中。
    data["revision"] = report_store_->revision();
    data["flow"] = CurrentFlowObject(flow_runner());
    if (data["flow"].is_object() && !data["flow"].contains("ok")) {
        data["flow"]["ok"] = nullptr;  // Python 的 _flow_state() 始终带 ok（未跑 flow 时为 null）
    }

    // Python _decorate_reports：路由库 + TD 策略清单 + 上次 DRC/CDC 缓存
    data["routed_db"] = NewestRoutedDb(*project);
    Json strategies = Json::object();
    Json available = Json::array();
    const std::string configured =
        settings_.td_path.empty() ? td_explicit_ : settings_.td_path;
    if (const std::optional<td::TdChoice> td = FindTdCached(configured); td.has_value()) {
        for (const std::string& name :
             flow::AvailableStrategies(td->root, project->family)) {
            available.push_back(name);
        }
    }
    strategies["available"] = std::move(available);
    data["strategies"] = std::move(strategies);

    // 上次 analysis 缓存（B5 会写；没有就保持缺省，与 Python 一致）
    const std::string cache = platform::path_join(
        platform::path_join(platform::path_join(project->dir, ".fpgalsp"), "cache"),
        "analysis.json");
    std::error_code ec;
    if (std::filesystem::is_regular_file(cache, ec)) {
        try {
            std::ifstream input(cache, std::ios::binary);
            if (input) data["analysis"] = Json::parse(input);
        } catch (...) {
            // 缓存损坏不影响报告主体
        }
    }
    return data;
}

// =============================================================================
//  fpgaLsp/flowStatus —— 真实 flow 状态 + 最后 200 行日志（协议 §3）
// =============================================================================

// P-D2：TD 安装根探测缓存（5 s TTL，负结果同样缓存）。
std::optional<td::TdChoice> LspServer::FindTdCached(const std::string& configured) {
    constexpr auto kTtl = std::chrono::seconds(5);
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(td_cache_mutex_);
    if (td_cache_valid_ && td_cache_key_ == configured &&
        now - td_cache_at_ < kTtl) {
        return td_cache_value_;
    }
    std::optional<td::TdChoice> found = td::FindTd(configured);
    td_cache_key_ = configured;
    td_cache_value_ = found;
    td_cache_valid_ = true;
    td_cache_at_ = now;
    return found;
}

HandlerResult LspServer::HandleFlowStatus(const Json& params) {    return OkObject(FeatureFlowStatus(params));
}

// P-C4：纯函数，便于单测覆盖窗口/落后/全量三种情况。
jsonrpc::Json SliceFlowLog(const flow::FlowState& state, std::optional<std::uint64_t> since,
                           std::uint64_t* log_start_seq) {
    const std::uint64_t total = state.log_seq;
    const std::uint64_t lines = static_cast<std::uint64_t>(state.log.size());
    const std::uint64_t start_seq = total > lines ? total - lines : 0u;
    if (log_start_seq != nullptr) *log_start_seq = start_seq;

    Json log = Json::array();
    std::size_t first = 0;
    if (since.has_value()) {
        if (*since < start_seq) {
            // 客户端落后于窗口：明确告诉它丢了行，别让它拼出错误的时间线。
            log.push_back("<log truncated: " + std::to_string(start_seq - *since) +
                          " lines dropped>");
        }
        // sinceSeq 是"已看到的最后一行序号"，所以下一行是 since - start_seq + 1。
        const std::uint64_t skip = *since >= start_seq ? *since - start_seq + 1 : 0u;
        first = static_cast<std::size_t>(std::min<std::uint64_t>(skip, lines));
    }
    for (std::size_t i = first; i < state.log.size(); ++i) log.push_back(state.log[i]);
    return log;
}

Json LspServer::FeatureFlowStatus(const Json& params) {
    const std::shared_ptr<flow::FlowRunner> runner = flow_runner();

    Json result = Json::object();
    result["ok"] = true;
    Json flow = CurrentFlowObject(runner);
    flow::FlowState state;
    bool have_state = false;
    if (runner) {
        state = runner->Status();
        have_state = true;
    }

    // P-C4：日志增量。`sinceSeq` = 客户端已看到的最后一行序号；只回新增行。
    // 旧客户端（不带 sinceSeq）仍然拿完整的最后 200 行，行为不变。
    std::optional<std::uint64_t> since;
    if (const auto it = params.find("sinceSeq");
        it != params.end() && it->is_number_integer()) {
        const auto value = it->get<std::int64_t>();
        since = value > 0 ? static_cast<std::uint64_t>(value) : 0u;
    }
    Json log = Json::array();
    if (have_state) {
        std::uint64_t start_seq = 0;
        log = SliceFlowLog(state, since, &start_seq);
        flow["logSeq"] = state.log_seq;
        flow["logStartSeq"] = start_seq;
    }
    flow["log"] = std::move(log);
    if (!flow.contains("ok")) flow["ok"] = nullptr;
    result["flow"] = std::move(flow);

    // Python 的 flowStatus 顶层还带 artifacts（runs[]），扩展的 FlowTree 会读
    Json artifacts = Json::object();
    Json runs = Json::array();
    if (const auto project = CurrentProject(); project.has_value()) {
        const std::string runs_dir = reports::RunsDirectory(project->dir, project->name);
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(runs_dir, ec)) {
            if (ec) break;
            if (!entry.is_directory(ec)) continue;
            Json item = Json::object();
            item["name"] = entry.path().filename().string();
            item["dir"] = platform::normalize_path(entry.path().string());
            item["exists"] = true;
            runs.push_back(std::move(item));
        }
    }
    artifacts["runs"] = std::move(runs);
    result["artifacts"] = std::move(artifacts);
    return result;
}

// =============================================================================
//  TD 摘要（fpgalsp.tdInfo / fpgalsp.info.td 共用）
// =============================================================================

Json LspServer::TdSummary() {
    const std::string configured =
        settings_.td_path.empty() ? td_explicit_ : settings_.td_path;
    const std::optional<td::TdChoice> choice = FindTdCached(configured);

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

    std::string chosen;
    std::string import_output;
    for (const auto& candidate : td::DbCandidates(root, project.family)) {
        // ⚠ SEC-01（B7 / S-01 的漏网点）：project.device 直接来自 .al 的
        //   <HardWare><Device>（al_project.cpp:318 只做 Trim，无字符集校验），
        //   不是可信内容。这里原来是"手抄"了 td_diagnostics 那份候选循环却没抄
        //   引用处理，于是 `<Device>EF2L45LG144B; exec calc</Device>` 会被裸拼成
        //   两条 Tcl 命令。统一走 MakeImportDeviceCommand（内部 TclQuote +
        //   IsTclToken）：合法器件名输出与旧实现逐字节相同（裸写），非法值变成
        //   一个被引用的单词，不再成为新命令。
        //   同一份数据在 MCP 路径（td_diagnostics.cpp:394 / td_device.cpp:355）
        //   一直是安全的，这里补齐即可，行为对合法输入零变化。
        const std::string command =
            td::MakeImportDeviceCommand(candidate, project.device, project.speed);
        try {
            const std::string out = session.Run(command, 90.0);
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
//  Phase B 起所有命令都走 LspServer::Feature*（LSP 与 MCP 同一实现）；
//  未完成的 Feature 由各自的 stub 返回 {ok:false,error:"not implemented: …"}。
// =============================================================================

// 已实现的命令名（顺序 = capability 里对外的展示顺序）。
// ⚠ 与下面 HandleExecuteCommand 的分支表必须保持一致：新增命令两处都要加，
//   `tests/unit/commands_test.cpp` 会逐个断言"名字能被分发"。
const std::vector<std::string>& ExecuteCommandNames() {
    static const std::vector<std::string> kNames = {
        "fpgalsp.info",         "fpgalsp.projects",        "fpgalsp.setProject",
        "fpgalsp.tdInfo",       "fpgalsp.device",          "fpgalsp.check",
        "fpgalsp.restart",      "fpgalsp.cancelFlow",      "fpgalsp.runFlow",
        "fpgalsp.compareStrategies", "fpgalsp.cables",     "fpgalsp.program",
        "fpgalsp.svfgen",       "fpgalsp.reports",         "fpgalsp.runAnalysis",
        "fpgalsp.instantiate",  "fpgalsp.copyInstantiation", "fpgalsp.symbolPath",
        "fpgalsp.duplicateModules", "fpgalsp.searchHierarchy", "fpgalsp.addSourceFile",
        "fpgalsp.setProjectName", "fpgalsp.updateProject", "fpgalsp.debugHighlight",
        "fpgalsp.metrics",      "fpgalsp.compareRuns", "fpgalsp.runs",      "fpgalsp.testbench", "fpgalsp.constraintTable",
        // 0.16.0 片上探针（ChipWatcher）
        "fpgalsp.probeStatus",  "fpgalsp.probeConfig", "fpgalsp.probeCapture",
    };
    return kNames;
}

// capability 里**广播**的命令名 = 全表减去扩展自己 contributes 的 3 条历史命令。
//
// 为什么必须减：vscode-languageclient 会把 executeCommandProvider.commands 里的
// **每一条**都注册成 VS Code 命令；扩展的 package.json 已经 contributes 了
// `fpgalsp.check` / `fpgalsp.info` / `fpgalsp.restart`，重复注册会让 initialize 抛
// "command 'fpgalsp.check' already exists" 并断开连接（doc/commands.md 有记录）。
// 这 3 条仍然可以被 workspace/executeCommand 调用，只是不对外广播。
const std::vector<std::string>& AdvertisedCommandNames() {
    static const std::vector<std::string> kNames = [] {
        std::vector<std::string> out;
        for (const std::string& name : ExecuteCommandNames()) {
            if (name == "fpgalsp.check" || name == "fpgalsp.info" ||
                name == "fpgalsp.restart") {
                continue;  // 扩展 contributes.commands 已占用
            }
            out.push_back(name);
        }
        return out;
    }();
    return kNames;
}

// =============================================================================
//  B-A3：workspace/willRenameFiles + didRenameFiles
// =============================================================================
//  为什么必须有：`.al` 用路径记录参与构建的文件，而 `AlFile::exists` 为假时
//  `InBuild()` 就是 false —— 改个文件名，工程**静默少喂一个文件**，综合还能"成功"，
//  用户看到 0 error。这是本仓库唯一会破坏工程数据的一类缺陷。
namespace {

std::string ReadTextFileOrEmpty(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::string LowerAscii(std::string_view text) {
    std::string out(text);
    for (char& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return out;
}

// `.al` 里记录的路径可能是相对（最常见）或绝对；新值保持同样的风格。
std::string RebasedPath(const project::Project& project, const std::string& old_rel,
                        const std::string& new_abs) {
    const bool absolute = old_rel.size() > 1 &&
                          (old_rel[0] == '/' || old_rel[1] == ':');
    if (absolute) return platform::normalize_path(new_abs);
    const std::string dir = LowerAscii(platform::normalize_path(project.dir)) + "/";
    const std::string normal = platform::normalize_path(new_abs);
    if (LowerAscii(normal).rfind(dir, 0) == 0) {
        return normal.substr(dir.size());  // 相对 .al 目录
    }
    return normal;  // 移到工程外了：只能写绝对路径
}

struct AlEdit {
    std::string al_path;
    int line = 0;
    int character = 0;
    int length = 0;
    std::string new_text;
};

// 为"old_abs 被改名为 new_abs"规划 .al 里的文本替换（只改第一处，避免误伤同名字符串）。
std::vector<AlEdit> PlanRenameEdits(const project::Project& project,
                                    const std::string& old_abs,
                                    const std::string& new_abs) {
    const project::AlFile* entry = nullptr;
    for (const project::AlFile& file : project.files) {
        if (LowerAscii(platform::normalize_path(file.path)) ==
            LowerAscii(platform::normalize_path(old_abs))) {
            entry = &file;
            break;
        }
    }
    if (entry == nullptr) return {};

    const std::string recorded = entry->rel.empty() ? entry->path : entry->rel;
    const std::string text = ReadTextFileOrEmpty(project.al_path);
    if (text.empty()) return {};
    const std::size_t pos = text.find(recorded);
    if (pos == std::string::npos) return {};

    // 位置换算：路径是 ASCII，字节偏移 = UTF-16 列偏移。
    int line = 0;
    int character = 0;
    for (std::size_t i = 0; i < pos; ++i) {
        if (text[i] == '\n') {
            ++line;
            character = 0;
        } else {
            ++character;
        }
    }

    AlEdit edit;
    edit.al_path = project.al_path;
    edit.line = line;
    edit.character = character;
    edit.length = static_cast<int>(recorded.size());
    edit.new_text = RebasedPath(project, recorded, new_abs);
    return {edit};
}

}  // namespace

Json LspServer::FeatureWillRenameFiles(const Json& params) {
    Json changes = Json::object();
    for (const Json& item : jsonrpc::GetArray(params, "files")) {
        const std::string old_uri = jsonrpc::GetString(item, "oldUri");
        const std::string new_uri = jsonrpc::GetString(item, "newUri");
        if (old_uri.empty() || new_uri.empty()) continue;
        const std::string old_path = platform::uri_to_path(old_uri);
        const std::string new_path = platform::uri_to_path(new_uri);
        const auto project = ProjectForPath(old_path);
        if (!project.has_value()) continue;
        for (const AlEdit& edit : PlanRenameEdits(*project, old_path, new_path)) {
            Json text_edit = Json::object();
            Json range = Json::object();
            Json start = Json::object();
            start["line"] = edit.line;
            start["character"] = edit.character;
            Json end = Json::object();
            end["line"] = edit.line;
            end["character"] = edit.character + edit.length;
            range["start"] = std::move(start);
            range["end"] = std::move(end);
            text_edit["range"] = std::move(range);
            text_edit["newText"] = edit.new_text;
            const std::string al_uri = platform::path_to_uri(edit.al_path);
            if (!changes.contains(al_uri)) changes[al_uri] = Json::array();
            changes[al_uri].push_back(std::move(text_edit));
        }
    }
    Json result = Json::object();
    if (!changes.empty()) result["changes"] = std::move(changes);
    return result;
}

HandlerResult LspServer::HandleWillRenameFiles(const Json& params) {
    return OkObject(FeatureWillRenameFiles(params));
}

HandlerResult LspServer::HandleDidRenameFiles(const Json& params) {
    // 客户端已经把 .al 改好了（或用户手工改的）。这里让工程模型/索引/诊断跟上，
    // 否则下一次 hover/诊断仍然用旧文件集合 —— 又是"静默少文件"。
    bool touched = false;
    for (const Json& item : jsonrpc::GetArray(params, "files")) {
        const std::string new_uri = jsonrpc::GetString(item, "newUri");
        if (new_uri.empty()) continue;
        touched = true;
    }
    if (!touched) return HandlerResult::Ok(Json());

    InvalidateProjectCaches();
    if (const auto project = CurrentProject(); project.has_value()) {
        if (index_service_) {
            index_service_->SetProject(*project);
            index_service_->OnWatchedFiles({project->al_path}, {});
        }
        PublishMerged(*project, nullptr);
        MaybeNotifyIndexChanged({project->al_path});
    }
    return HandlerResult::Ok(Json());
}

// B-B6：两次 run 的差异（QoR/时序/面积）。数据来自 <project>_Runs/<name>/。
// B-B6/X-B2：run 列表 + 指标（趋势图的数据源）。与 CompareRuns 同源。
// B-B10：testbench 骨架（只生成文本，不落盘；写文件交给调用方/agent）。
// X-B3：约束表格数据源（.adc 逐行 -> 结构化行 + 该行的诊断数）。
// 只读：编辑交给调用方用 WorkspaceEdit 回写，服务端不直接改用户文件。
// B-B4：把分析缓存（<state>/cache/analysis.json）里的 DRC/CDC 违例发布成诊断。
// CDC 行的 cell 文本用 features::ResolveHdlPath 解析到源码；解析不到不发布
// （宁可少一条波浪线，也不要把它钉到一个猜出来的位置上）。
// DRC 明细是纯文本（没有结构化 cell），只在 .al 上给一条计数摘要。
void LspServer::PublishAnalysisDiagnostics(const project::Project& project) {
    std::optional<Json> cache = analysis::Cached(project);
    if (!cache.has_value()) return;

    bool truncated = false;
    const std::shared_ptr<const hdl::HdlIndex> index =
        IndexForProject(project, &truncated, nullptr);

    std::map<std::string, lsp::DiagnosticList> table;
    if (index != nullptr && cache->contains("cdc")) {
        const Json& cdc = (*cache)["cdc"];
        if (cdc.is_object() && cdc.contains("rows") && cdc["rows"].is_array()) {
            for (const Json& row : cdc["rows"]) {
                if (!row.is_array()) continue;
                for (const Json& cell : row) {
                    if (!cell.is_string()) continue;
                    const std::string text = cell.get<std::string>();
                    if (text.size() < 3) continue;
                    const auto location = features::ResolveHdlPath(*index, project, text);
                    if (!location.has_value()) continue;
                    lsp::Diagnostic diagnostic;
                    diagnostic.severity = lsp::kWarning;
                    diagnostic.code = "CDC-UNSAFE";
                    diagnostic.source = "fpga-lsp:analysis";
                    diagnostic.message = "CDC: asynchronous crossing at " + text;
                    diagnostic.range.start = lsp::Position{location->line, location->character};
                    diagnostic.range.end =
                        lsp::Position{location->line, location->character + 1};
                    Json data = Json::object();
                    data["origin"] = "analysis";
                    diagnostic.data = std::move(data);
                    table[platform::uri_to_path(location->file)].push_back(
                        std::move(diagnostic));
                }
            }
        }
    }

    if (cache->contains("drc")) {
        const Json& drc = (*cache)["drc"];
        const int violations = drc.is_object() ? drc.value("violations", 0) : 0;
        if (violations > 0) {
            lsp::Diagnostic diagnostic;
            diagnostic.severity = lsp::kWarning;
            diagnostic.code = "DRC-SUMMARY";
            diagnostic.source = "fpga-lsp:analysis";
            diagnostic.message = "DRC: " + std::to_string(violations) +
                                 " violation(s); see the DRC report table";
            diagnostic.range.start = lsp::Position{0, 0};
            diagnostic.range.end = lsp::Position{0, 1};
            table[project.al_path].push_back(std::move(diagnostic));
        }
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        diag_analysis_[project.al_path] = std::move(table);
    }
    PublishMerged(project, nullptr);
}

Json LspServer::FeatureConstraintTable(const Json& params) {
    const auto project = ProjectFromParams(params);
    Json out = Json::object();
    if (!project.has_value()) {
        out["ok"] = false;
        out["error"] = "No Anlogic .al project found";
        return out;
    }
    bool truncated = false;
    const std::shared_ptr<const hdl::HdlIndex> index = IndexForProject(*project, &truncated, nullptr);

    // 顶层端口表（与 Problems 里的约束诊断同源）
    std::vector<constraints::Port> ports;
    if (index != nullptr && !project->top.empty()) {
        if (const hdl::Module* module = index->ModuleByName(project->top); module != nullptr) {
            for (const hdl::Port& port : module->ports) {
                constraints::Port converted;
                converted.name = port.name;
                converted.direction = port.direction;
                converted.width = port.width;
                converted.msb = port.msb;
                converted.lsb = port.lsb;
                converted.line = port.line;
                converted.character = port.character;
                converted.file = port.file;
                ports.push_back(std::move(converted));
            }
        }
    }

    Json files = Json::array();
    for (const project::AlFile* file : project->AdcFiles()) {
        const std::optional<std::string> text = LoadText(file->path);
        if (!text.has_value()) continue;
        const std::vector<constraints::adc::Entry> entries = constraints::adc::Parse(*text);

        // 行号 -> 诊断数（表格里给一个可见的"有问题"标记）
        std::map<int, int> issues_by_line;
        for (const lsp::Diagnostic& diagnostic : ConstraintDiagnostics(file->path, *text, ports)) {
            ++issues_by_line[diagnostic.range.start.line];
        }

        Json rows = Json::array();
        for (const constraints::adc::Entry& entry : entries) {
            Json row = Json::object();
            row["line"] = entry.line;
            row["character"] = entry.character;
            row["command"] = entry.command;
            row["port"] = entry.port;
            row["base"] = entry.base;
            row["bit"] = entry.bit.has_value() ? Json(*entry.bit) : Json(nullptr);
            const auto attr = [&entry](const char* key) {
                const auto it = entry.attrs.find(key);
                return it != entry.attrs.end() ? Json(it->second) : Json(nullptr);
            };
            row["location"] = attr("LOCATION");
            row["iostandard"] = attr("IOSTANDARD");
            row["pulltype"] = attr("PULLTYPE");
            const auto issue_count = issues_by_line.find(entry.line);
            row["issues"] = issue_count != issues_by_line.end() ? issue_count->second : 0;
            rows.push_back(std::move(row));
        }

        Json item = Json::object();
        item["file"] = file->path;
        item["rows"] = std::move(rows);
        files.push_back(std::move(item));
    }
    out["ok"] = true;
    out["project"] = project->name;
    out["files"] = std::move(files);
    return out;
}

Json LspServer::FeatureTestbench(const Json& params) {
    const auto project = ProjectFromParams(params);
    Json out = Json::object();
    if (!project.has_value()) {
        out["ok"] = false;
        out["error"] = "No Anlogic .al project found";
        return out;
    }
    bool truncated = false;
    const std::shared_ptr<const hdl::HdlIndex> index = IndexForProject(*project, &truncated, nullptr);
    if (index == nullptr) {
        out["ok"] = false;
        out["error"] = "No HDL index";
        return out;
    }
    return features::TestbenchSkeleton(*index, *project, jsonrpc::GetString(params, "module"),
                                       jsonrpc::GetString(params, "clock"),
                                       jsonrpc::GetString(params, "reset"));
}

Json LspServer::FeatureListRuns(const Json& params) {
    const auto project = ProjectFromParams(params);
    Json out = Json::object();
    if (!project.has_value()) {
        out["ok"] = false;
        out["error"] = "No Anlogic .al project found";
        return out;
    }
    const std::vector<reports::RunMetrics> runs = reports::ListRuns(*project);
    out["ok"] = true;
    out["project"] = project->name;
    Json list = Json::array();
    for (const reports::RunMetrics& run : runs) {
        Json item = Json::object();
        item["name"] = run.name;
        item["dir"] = run.dir;
        Json files = Json::array();
        for (const std::string& file : run.files) files.push_back(file);
        item["files"] = std::move(files);
        Json values = Json::object();
        for (const auto& [key, value] : run.values) values[key] = value;
        item["values"] = std::move(values);
        list.push_back(std::move(item));
    }
    out["runs"] = std::move(list);
    return out;
}

Json LspServer::FeatureCompareRuns(const Json& params) {
    const auto project = ProjectFromParams(params);
    if (!project.has_value()) {
        Json out = Json::object();
        out["ok"] = false;
        out["error"] = "No Anlogic .al project found";
        return out;
    }
    return reports::CompareRuns(*project, jsonrpc::GetString(params, "from"),
                                jsonrpc::GetString(params, "to"));
}

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
        const std::optional<td::TdChoice> td = FindTdCached(configured);
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
        const std::optional<td::TdChoice> td = FindTdCached(configured);
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
        const std::optional<td::TdChoice> td = FindTdCached(configured);
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
        const std::optional<td::TdChoice> td = FindTdCached(configured);
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
        const std::optional<td::TdChoice> td = FindTdCached(configured);
        if (!td.has_value()) return Failure("No TangDynasty installation found");

        const Json options =
            args.empty() || !args.front().is_object() ? Json::object() : args.front();
        flow::ProgramOptions program_options;
        program_options.bit_path = jsonrpc::GetString(options, "bit");
        program_options.svf_path = jsonrpc::GetString(options, "svf");
        program_options.scan_mode = jsonrpc::GetString(options, "scanMode", "prog_sram");
        return OkObject(flow::GenerateSvf(*td, *project, program_options));
    }

    // ---- 0.16.0：片上探针（ChipWatcher）----
    //   与 program/svfgen 同一形状：只解析参数，逻辑在 probe::，TD 由这里解析后传进去。
    if (command == "fpgalsp.probeStatus") {
        const auto project = CurrentProject();
        if (!project.has_value()) return Failure(".al project not found");
        return OkObject(probe::Status(*project));
    }

    if (command == "fpgalsp.probeConfig") {
        const auto project = CurrentProject();
        if (!project.has_value()) return Failure(".al project not found");
        const Json options =
            args.empty() || !args.front().is_object() ? Json::object() : args.front();
        std::vector<std::string> signals;
        if (const Json list = jsonrpc::GetArray(options, "signals"); !list.empty()) {
            for (const auto& item : list) {
                if (item.is_string()) signals.push_back(item.get<std::string>());
            }
        }
        // ⚠ 写 user.mdn 会改变下一次综合/布局要嵌进 bit 的探针集合，必须显式 write:true
        return OkObject(probe::Configure(*project, signals,
                                        jsonrpc::GetBool(options, "write", false)));
    }

    if (command == "fpgalsp.probeCapture") {
        const auto project = CurrentProject();
        if (!project.has_value()) return Failure(".al project not found");
        const std::string configured =
            settings_.td_path.empty() ? td_explicit_ : settings_.td_path;
        const std::optional<td::TdChoice> td = FindTdCached(configured);
        if (!td.has_value()) return Failure("No TangDynasty installation found");

        const Json options =
            args.empty() || !args.front().is_object() ? Json::object() : args.front();
        probe::CaptureOptions capture_options;
        capture_options.download = jsonrpc::GetBool(options, "download", true);
        capture_options.trigger_now = jsonrpc::GetBool(options, "triggerNow", true);
        capture_options.instance = jsonrpc::GetString(options, "instance");
        capture_options.cable = jsonrpc::GetString(options, "cable", "0");
        capture_options.csv_path = jsonrpc::GetString(options, "csv");
        const int timeout = jsonrpc::GetInt(options, "timeout", 30);
        capture_options.wait_timeout = timeout > 0 ? static_cast<double>(timeout) : 30.0;
        // `download` 会把带探针的 bit 重新灌进 SRAM（改板子状态）；采集本身只读。
        // 和 fpgalsp.program 一样把确认下沉到后端，避免任何调用路径绕过。
        if (capture_options.download && !jsonrpc::GetBool(options, "confirm", false)) {
            Json refusal = flow::Refusal(
                "confirm",
                "capture with download:true reprograms the FPGA SRAM; pass confirm:true "
                "(or download:false to only read the already-loaded design)");
            return OkObject(refusal);
        }
        return OkObject(probe::Capture(*td, *project, capture_options));
    }

    // ---- Phase B：IDE / 分析 / 报告类命令（统一走 Feature 层）----
    const Json options =
        args.empty() || !args.front().is_object() ? Json::object() : args.front();

    if (command == "fpgalsp.reports") {
        return OkObject(FeatureReports(params));
    }
    if (command == "fpgalsp.runAnalysis") {
        return OkObject(FeatureRunAnalysis(options));
    }
    if (command == "fpgalsp.instantiate") {
        return OkObject(FeatureInstantiation(options));
    }
    if (command == "fpgalsp.copyInstantiation") {
        return OkObject(FeatureCopyInstantiation(options));
    }
    if (command == "fpgalsp.symbolPath") {
        return OkObject(FeatureSymbolPath(options));
    }
    if (command == "fpgalsp.duplicateModules") {
        return OkObject(FeatureDuplicateModules(options));
    }
    if (command == "fpgalsp.searchHierarchy") {
        return OkObject(FeatureSearchHierarchy(options));
    }
    if (command == "fpgalsp.addSourceFile") {
        return OkObject(FeatureAddSourceFile(options));
    }
    if (command == "fpgalsp.setProjectName") {
        return OkObject(FeatureSetProjectName(options));
    }
    if (command == "fpgalsp.updateProject") {
        return OkObject(FeatureUpdateProject(options));
    }
    if (command == "fpgalsp.debugHighlight") {
        return OkObject(FeatureDebugHighlight(params));
    }
    if (command == "fpgalsp.constraintTable") {
        return OkObject(FeatureConstraintTable(params));
    }
    if (command == "fpgalsp.testbench") {
        return OkObject(FeatureTestbench(params));
    }
    if (command == "fpgalsp.runs") {
        return OkObject(FeatureListRuns(params));
    }
    if (command == "fpgalsp.compareRuns") {
        return OkObject(FeatureCompareRuns(params));
    }
    if (command == "fpgalsp.metrics") {
        const AsyncMetricsSnapshot metrics = GetAsyncMetrics();
        Json result = Json::object();
        result["ok"] = true;
        result["queued"] = metrics.queued;
        result["completed"] = metrics.completed;
        result["cancelled"] = metrics.cancelled;
        result["failed"] = metrics.failed;
        result["slow"] = metrics.slow;
        result["inflight"] = metrics.inflight;
        result["maxQueueWaitMs"] = metrics.max_queue_wait_ms;
        result["maxRunMs"] = metrics.max_run_ms;
        if (index_service_) {
            const IndexService::Stats stats = index_service_->stats();
            Json index = Json::object();
            index["generation"] = stats.generation;
            index["rebuilds"] = stats.rebuilds;
            index["fileUpdates"] = stats.file_updates;
            index["files"] = index_service_->tracked_files();
            result["index"] = std::move(index);
        }
        if (report_store_) {
            const ReportStoreStats stats = report_store_->stats();
            Json reports = Json::object();
            reports["gets"] = stats.gets;
            reports["snapshotHits"] = stats.snapshot_hits;
            reports["builds"] = stats.builds;
            reports["revisions"] = stats.revisions;
            reports["lastBuildMs"] = stats.last_build_ms;
            result["reports"] = std::move(reports);
        }
        return OkObject(std::move(result));
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
