// =============================================================================
//  src/mcp/mcp_server.cpp
// =============================================================================
//  逐条移植 fpgalsp/mcp.py 的 McpServer。对照关系：
//    Handle()          <- handle()
//    WrapToolResult()  <- _tool_result()
//    WrapToolError()   <- _error_result()
//    CallTool()        <- _call()（含 required 校验与 ToolError）
//
//  ---- 与 Python 参考实现的差异（刻意的）----
//  1. `instructions` 是 C++ 侧的**增强版作战手册**（≤2000 字符），不是 Python 那
//     三句话的逐字复刻：Python 版没写"flow 是异步的""改了 RTL 必须重跑 syn"
//     "写 user.mdn 不改板子"这些实测坑，导致 agent 反复踩。协议形状不变，只有
//     这段文案加强。
//  2. 新增 `prompts/list` + `prompts/get`（4 个模板）与 capabilities.prompts：
//     Python 版返回空数组且没有 prompts/get，客户端只能看到"没有模板"。
//  3. 其余方法（tools/*、resources/*、initialize、取消、截断）逐字对齐。
// =============================================================================

#include "mcp/mcp_server.h"

#include "jsonrpc/json_util.h"
#include "jsonrpc/transport.h"

#include <algorithm>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mcp/tools.h"

namespace fpgalsp::mcp {
namespace {

// -----------------------------------------------------------------------------
//  instructions —— 只给"没有任何上下文的 agent"看的作战手册
// -----------------------------------------------------------------------------
//  预算：MCP 客户端会把 instructions 塞进系统提示，超过 2000 字符就有被截断/挤掉
//  其它上下文的风险。所以七条压成七段，每条 1–2 句，只写"必须知道、否则一定踩"
//  的东西；细节走 fpga_guide。
constexpr const char* kInstructions =
    "Tools for Anlogic TangDynasty (TD) FPGA projects. Line/column arguments are 1-based. "
    "Pass a project (.al path, directory or name) when the workspace has several projects. "
    "Call fpga_guide for a topic-level walkthrough "
    "(overview/project/index/flow/probe/program/troubleshoot).\n"
    "\n"
    "1. Three routes: read the project and its index (fpga_project_list, fpga_project_info, "
    "fpga_project_files, then fpga_document_symbols / fpga_search_text / fpga_symbol_path / "
    "fpga_diagnostics); build (fpga_flow_run -> fpga_flow_wait -> fpga_project_reports); "
    "on-board (fpga_probe_config -> flow_run stages:[\"all\"] -> fpga_probe_verify -> "
    "fpga_probe_capture). Probe budget: none imposed by this backend - each device has "
    "its own ChipWatcher limit (4 buses / 64 bits worked on PH1A100SFG676) and TD reports "
    "the real one at the phy stage; to keep a signal listed but unused, pass "
    "fpga_probe_config{disabled:[...],apply_build_config:true} (no debug resource, needs "
    "stages:[\"all\"]).\n"
    "2. Disk *.v files are NOT the build file set: call fpga_project_files first, and "
    "include_excluded:true to see what the .al excludes.\n"
    "3. flow_run is asynchronous: call fpga_flow_wait in the SAME MCP session. Exiting the "
    "process kills the TD children, so a later session sees \"ok\" with no flow.\n"
    "4. After an RTL edit rerun syn (stages:[\"all\"] is the safest); stages:[\"phy\"] alone "
    "reuses the old netlist.\n"
    "5. Writing chip_debugger/user.mdn changes nothing on the board. Run stages:[\"all\"] - "
    "the ChipWatcher instance is created right after Optimize Gate (in the syn run), so "
    "[\"phy\"] alone reuses the old watcher; the backend adds syn automatically when user.mdn "
    "is newer than the gate database. A successful flow does NOT prove the probes reached the "
    "bit: check fpga_probe_verify / fpga_probe_status.\n"
    "6. Sample clock and sample depth are compile-time values; MCP cannot change them (TD "
    "limitation). At capture time only trigger_position/window_*/trigger_* are adjustable.\n"
    "7. Permissions and cost: --allow-flow / --allow-write enable the gated tools; each new "
    "MCP process starts in ~0.1 s (TD discovery is cached); keep long runs in one session (exiting the process kills TD children) and close stdin after the `exit` notification; phy usually takes "
    "5-15 min.";

Json Response(const Json& id, Json result) {
    Json out = Json::object();
    out["jsonrpc"] = "2.0";
    out["id"] = id;
    out["result"] = std::move(result);
    return out;
}

Json ErrorResponse(const Json& id, int code, std::string message) {
    Json error = Json::object();
    error["code"] = code;
    error["message"] = std::move(message);
    Json out = Json::object();
    out["jsonrpc"] = "2.0";
    out["id"] = id;
    out["error"] = std::move(error);
    return out;
}

// Python: `message.get("id")` 为 None 时视为通知
bool HasId(const Json& message, Json* id) {
    if (!message.is_object()) return false;
    const auto it = message.find("id");
    if (it == message.end() || it->is_null()) return false;
    *id = *it;
    return true;
}

const Json* GetMember(const Json& json, const char* key) {
    if (!json.is_object()) return nullptr;
    const auto it = json.find(key);
    if (it == json.end()) return nullptr;
    return &(*it);
}

bool VersionSupported(const std::string& version) {
    return std::ranges::any_of(McpServer::kSupportedVersions, [&version](const char* supported) {
        return version == supported;
    });
}

// P-B1：协商档下 text 只留一行摘要，数据在 structuredContent 里。
// 摘要必须能区分"成功"与"工具自己报告的失败"（后者 isError 仍为 false）。
std::string SummaryOf(const Json& value) {
    if (value.is_object()) {
        const auto ok = value.find("ok");
        if (ok != value.end() && ok->is_boolean() && !ok->get<bool>()) {
            const auto error = value.find("error");
            if (error != value.end() && error->is_string()) {
                return "failed: " + error->get<std::string>();
            }
            return "failed";
        }
        return "ok";
    }
    if (value.is_array()) {
        return "ok: " + std::to_string(value.size()) + " items";
    }
    return "ok";
}

// Python: `arguments[key] in (None, "")`
bool MissingRequired(const Json* value) {
    if (value == nullptr) return true;
    if (value->is_null()) return true;
    if (value->is_string() && value->get<std::string>().empty()) return true;
    return false;
}

// -----------------------------------------------------------------------------
//  prompts —— 4 个模板（prompts/list + prompts/get）
// -----------------------------------------------------------------------------
//  为什么要有模板：`instructions` 只有 2000 字符，写不下"某个具体任务该怎么串"。
//  模板是**任务入口**（onboard / build / probe / debug），正文 2–5 行，只推 agent
//  到正确的工具序列 + 关键前提上，细节留给 fpga_guide 的对应 topic —— 两处都写
//  全套必然漂移。
//
//  ⚠ 缺省参数必须代入**可读的默认值**而不是留空：留空会让 agent 生成
//    `{"stages":[]}` / `{"job":""}` 这种非法调用，比不填模板更糟。
struct PromptArg {
    const char* name;
    const char* description;
    bool required;
};

struct PromptDef {
    const char* name;
    const char* description;
    std::vector<PromptArg> args;
};

const std::vector<PromptDef>& PromptTable() {
    // 顺序即 prompts/list 的输出顺序：先入门，再构建，再上板，最后排错。
    static const std::vector<PromptDef> kPrompts = {
        {.name="onboard-project",
         .description="Bootstrap an unfamiliar project: discover it, read the real build file set, "
         "then report structure, constraints and risks.",
         .args={{.name="project",
           .description="Optional .al path, project directory, or project name. Default: active project.",
           .required=false,},},},
        {.name="run-flow-to-bit",
         .description="Take the active project from RTL to a bitstream and report QoR, timing and any "
         "failure with its log evidence.",
         .args={{.name="stages",
           .description="Stages to run: all, syn, phy or a comma list such as syn,phy. Default: all.",
           .required=false,},},},
        {.name="probe-on-board",
         .description="Mark signals for on-chip capture, rebuild phy so they enter the bitstream, verify "
         "with probe_status, then capture on the board.",
         .args={{.name="signals",
           .description="Signals to mark, e.g. WHG/pwm_reg0_i[15:0], comma separated.",
           .required=true,},
          {.name="trigger",
           .description="Optional capture-time trigger arguments as a JSON fragment, e.g. "
           "\"trigger_position\":512 or \"bus_op\":\"equal\". Default: trigger_now.",
           .required=false,},},},
        {.name="debug-failed-flow",
         .description="Diagnose a failed or suspicious flow run from its issues, logs and artifacts before "
         "rerunning anything.",
         .args={{.name="run", .description="Flow run id (flow_xxxxxxxx). Default: the newest run.", .required=false}},},
    };
    return kPrompts;
}

std::string Trimmed(std::string_view value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && (value[begin] == ' ' || value[begin] == '\t' || value[begin] == ',')) {
        ++begin;
    }
    while (end > begin &&
           (value[end - 1] == ' ' || value[end - 1] == '\t' || value[end - 1] == ',')) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

std::string JsonEscaped(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 2);
    for (const char c : value) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

// "all" / "syn,phy" / '["all"]' -> JSON 数组**文本**（已含方括号）。
// 已经写成 JSON 数组的原样透传：agent 从 schema 抄过来的就是这种形态。
std::string JsonArrayText(std::string_view value, std::string_view fallback) {
    std::string trimmed = Trimmed(value);
    if (trimmed.empty()) trimmed = Trimmed(fallback);
    if (trimmed.empty()) return "[]";
    if (trimmed.front() == '[') return trimmed;
    std::string out = "[";
    std::size_t start = 0;
    bool first = true;
    while (start <= trimmed.size()) {
        const std::size_t comma = trimmed.find(',', start);
        const std::string item = Trimmed(comma == std::string::npos
                                             ? std::string_view(trimmed).substr(start)
                                             : std::string_view(trimmed).substr(start,
                                                                                comma - start));
        if (!item.empty()) {
            if (!first) out.push_back(',');
            out += "\"" + JsonEscaped(item) + "\"";
            first = false;
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    if (first) return "[]";
    out.push_back(']');
    return out;
}

void ReplaceAllInPlace(std::string* text, std::string_view needle, std::string_view value) {
    if (needle.empty()) return;
    std::size_t pos = 0;
    while ((pos = text->find(needle, pos)) != std::string::npos) {
        text->replace(pos, needle.size(), value);
        pos += value.size();
    }
}

// 取一个字符串参数（缺失/非字符串 -> 空串）
std::string StringArg(const Json& arguments, const char* key) {
    if (!arguments.is_object()) return {};
    const auto it = arguments.find(key);
    if (it == arguments.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

// 未知 name 返回空串（调用方转成 -32602）。
std::string PromptText(std::string_view name, const Json& arguments) {
    if (name == "onboard-project") {
        const std::string project = StringArg(arguments, "project");
        std::string text =
            "Onboard {{project}}: discover it, read the real build file set, then report.\n"
            "1. Call fpga_td_info, fpga_project_info and fpga_project_files with "
            "include_excluded:true - the .al build file set is what compiles, not every .v on "
            "disk.\n"
            "2. Call fpga_project_structure with include_ports:true, fpga_duplicate_modules {} "
            "and fpga_diagnostics {\"limit\":2000}: report top module, hierarchy depth and "
            "blocking issues.\n"
            "3. Call fpga_constraints_status and fpga_device_info: ADC pin coverage, SDC clock "
            "constraints, device/package.\n"
            "4. Report the risks (excluded files, duplicate modules, missing constraints) and "
            "the next step; fpga_guide {\"topic\":\"project\"} has the details.\n";
        ReplaceAllInPlace(&text, "{{project}}",
                          project.empty() ? "the active project" : "project \"" + project + "\"");
        return text;
    }
    if (name == "run-flow-to-bit") {
        const std::string stages = StringArg(arguments, "stages");
        const std::string stages_json = JsonArrayText(stages, "[\"all\"]");
        std::string text =
            "Take the active project from RTL to a bitstream with stages {{stages}}.\n"
            "1. Call fpga_project_files first: confirm the RTL you edited is in the build "
            "set.\n"
            "2. Call fpga_flow_run {\"stages\":{{stages_json}},\"jobs\":6}, then fpga_flow_wait "
            "in this SAME session - exiting the MCP process kills the TD children.\n"
            "3. Call fpga_project_reports {\"stage\":\"all\",\"section\":\"summary\"}; on "
            "failure call fpga_flow_issues and fpga_flow_log.\n"
            "4. ok:true is not a timing sign-off, and a phy-only run reuses the old netlist: "
            "after an RTL edit use stages [\"all\"]. Details: fpga_guide {\"topic\":\"flow\"}.\n";
        ReplaceAllInPlace(&text, "{{stages_json}}", stages_json);
        ReplaceAllInPlace(&text, "{{stages}}", Trimmed(stages).empty() ? "all" : Trimmed(stages));
        return text;
    }
    if (name == "probe-on-board") {
        const std::string signals = StringArg(arguments, "signals");
        const std::string trigger = Trimmed(StringArg(arguments, "trigger"));
        const std::string signals_json = JsonArrayText(signals, "[\"<signal>\"]");
        std::string text =
            "Mark and capture on-chip probes for {{signals}}.\n"
            "1. Call fpga_probe_status: record watcher.clock, watcher.sample_depth "
            "(compile-time, MCP cannot change them), ready and disabled.\n"
            "2. Call fpga_probe_config {\"signals\":{{signals_json}},\"write\":true} (needs "
            "--allow-write; dry-run without write first to see added/removed), then "
            "fpga_flow_run {\"stages\":[\"phy\"]} + fpga_flow_wait in this session - an "
            "incremental route does NOT re-insert the debugger.\n"
            "3. fpga_probe_status must show ready:true, watcher.stale:false, "
            "watcher.missing_in_bit:[] before "
            "fpga_probe_capture {\"download\":\"auto\",\"confirm\":true,{{trigger}}}.\n"
            "4. Only trigger_position/window_*/trigger_* are runtime; capture-time hint: "
            "{{trigger_hint}}. KIT-* failure shapes are in fpga_guide "
            "{\"topic\":\"probe\"}.\n";
        ReplaceAllInPlace(&text, "{{signals_json}}", signals_json);
        ReplaceAllInPlace(&text, "{{signals}}", Trimmed(signals).empty() ? "the signals you need" : signals);
        ReplaceAllInPlace(&text, "{{trigger}}", trigger.empty() ? "\"trigger_now\":true" : trigger);
        ReplaceAllInPlace(&text, "{{trigger_hint}}",
                          trigger.empty() ? "default trigger_now:true" : trigger);
        return text;
    }
    if (name == "debug-failed-flow") {
        const std::string run = Trimmed(StringArg(arguments, "run"));
        const bool has_run = !run.empty();
        std::string text =
            "Debug the flow run: {{run}}.\n"
            "1. Call fpga_flow_status and fpga_job_status {{job_args}}: is it running, failed, "
            "or done-but-unexpected?\n"
            "2. Call fpga_flow_issues {{issue_args}} and fpga_flow_log {{log_args}}; quote the "
            "exact *-ERROR / *-WARNING lines.\n"
            "3. Check the artifacts, not just ok:true - ok does NOT mean "
            "phy_1/cw/compiled.cwc exists or that the probes are in the bit; call "
            "fpga_probe_status.\n"
            "4. Classify before rerunning: PRJ-8806/PRJ-5303 are tolerated when artifacts "
            "exist, HDL-7225-style CRITICAL-WARNING is not a failure; see fpga_guide "
            "{\"topic\":\"troubleshoot\"}.\n";
        ReplaceAllInPlace(&text, "{{run}}", has_run ? run : "the newest one");
        ReplaceAllInPlace(&text, "{{job_args}}",
                          has_run ? R"({"job":")" + JsonEscaped(run) + "\"}" : "{}");
        ReplaceAllInPlace(&text, "{{issue_args}}",
                          has_run ? R"({"run_id":")" + JsonEscaped(run) + "\"}" : "{}");
        ReplaceAllInPlace(&text, "{{log_args}}",
                          has_run ? R"({"run_id":")" + JsonEscaped(run) +
                                        R"(","kind":"both","tail":400})"
                                  : R"({"kind":"both","tail":400})");
        return text;
    }
    return {};
}

Json PromptsListJson() {
    Json prompts = Json::array();
    for (const PromptDef& def : PromptTable()) {
        Json item = Json::object();
        item["name"] = def.name;
        item["description"] = def.description;
        Json args = Json::array();
        for (const PromptArg& arg : def.args) {
            Json arg_json = Json::object();
            arg_json["name"] = arg.name;
            arg_json["description"] = arg.description;
            arg_json["required"] = arg.required;
            args.push_back(std::move(arg_json));
        }
        item["arguments"] = std::move(args);
        prompts.push_back(std::move(item));
    }
    return prompts;
}

}  // namespace

McpServer::McpServer(ToolDispatcher& dispatcher, McpServerOptions options)
    : dispatcher_(dispatcher), options_(std::move(options)) {
    StartWorker();
}

McpServer::~McpServer() { StopWorker(); }

void McpServer::StartWorker() {
    worker_ = std::thread([this]() { WorkerLoop(); });
}

void McpServer::StopWorker() {
    {
        std::scoped_lock lock(queue_mutex_);
        stopping_ = true;
    }
    queue_cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void McpServer::WorkerLoop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this]() { return stopping_ || !tasks_.empty(); });
            if (tasks_.empty()) {
                if (stopping_) return;
                continue;
            }
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        try {
            task();
        } catch (...) {
            // 工具层已经把自己的异常转成 isError；这里兜住"连转换都失败"的极端情况，
            // 不允许让 worker 线程死去（否则该连接后续所有调用都排队等死）。
        }
    }
}

void McpServer::CancelRequest(const Json& request_id) {
    const std::string key = request_id.dump();
    std::scoped_lock lock(inflight_mutex_);
    const auto it = inflight_.find(key);
    if (it != inflight_.end() && it->second != nullptr) {
        it->second->store(true);
    }
}

std::optional<Json> McpServer::HandleRouted(const Json& message,
                                            const std::function<void(const Json&)>& writer) {
    if (!message.is_object()) return Handle(message);
    const Json* method_member = GetMember(message, "method");
    const std::string method =
        (method_member != nullptr && method_member->is_string()) ? method_member->get<std::string>()
                                                                : std::string();

    // 取消通知：必须在这里吃掉 —— 它没有 id，Handle() 会当作"通知"直接返回 nullopt，
    // 但我们要在返回前置位令牌。
    if (method == "notifications/cancelled") {
        if (const Json* params = GetMember(message, "params"); params != nullptr) {
            if (const Json* request_id = GetMember(*params, "requestId");
                request_id != nullptr) {
                CancelRequest(*request_id);
            }
        }
        return std::nullopt;
    }

    if (method != "tools/call") return Handle(message);

    Json id;
    if (!HasId(message, &id)) return Handle(message);  // tools/call 的通知形式：同步跑

    const std::string key = id.dump();
    const auto token = std::make_shared<std::atomic<bool>>(false);
    {
        std::scoped_lock lock(inflight_mutex_);
        inflight_[key] = token;
    }
    {
        std::scoped_lock lock(queue_mutex_);
        tasks_.emplace_back([this, message, writer, key, token, id]() {
            dispatcher_.SetCancelToken(token);
            std::optional<Json> response;
            try {
                response = HandleSingle(message);
            } catch (...) {
                response = Response(id, WrapToolError("InternalError: worker crashed"));
            }
            dispatcher_.SetCancelToken(nullptr);
            {
                std::scoped_lock guard(inflight_mutex_);
                inflight_.erase(key);
            }
            if (response.has_value()) writer(*response);
        });
    }
    queue_cv_.notify_one();
    return std::nullopt;
}

void McpServer::SetNotificationWriter(std::function<void(const Json&)> writer) {
    notification_writer_ = std::move(writer);
}

Json McpServer::WrapToolError(const std::string& message) {
    Json structured = Json::object();
    structured["ok"] = false;
    structured["error"] = message;

    Json content = Json::array();
    Json text_item = Json::object();
    text_item["type"] = "text";
    text_item["text"] = "Error: " + message;
    content.push_back(std::move(text_item));

    Json out = Json::object();
    out["content"] = std::move(content);
    out["structuredContent"] = std::move(structured);
    out["isError"] = true;
    return out;
}

// P-B2：分页式裁剪。
// 旧行为是"超过 800000 字节就把整份结果换成 {truncated:true}"，agent 什么都拿不到。
// 现在按"最大的那个数组"裁剪：保留对象其余字段，数组只留能放下的前缀，并带上
// total / nextOffset，客户端可以继续取。
// 找不到数组（纯大字符串/大对象）时返回 null，调用方回退到旧的整份丢弃。
namespace {

std::size_t CompactSize(const Json& value) {
    return value.dump(-1, ' ', false, Json::error_handler_t::replace).size();
}

// 把 array 的前 count 项序列化后的字节数（含字段名开销，粗略但单调）。
std::size_t PrefixSize(const Json& array, std::size_t count) {
    Json slice = Json::array();
    for (std::size_t i = 0; i < count && i < array.size(); ++i) slice.push_back(array[i]);
    return CompactSize(slice);
}

}  // namespace

Json McpServer::TruncatePayload(const Json& value, std::size_t limit,
                                std::size_t offset_hint) {
    constexpr std::size_t kSafety = 4096;  // 给信封/其他字段留余量
    const std::size_t budget = limit > kSafety ? limit - kSafety : limit / 2;

    // 1) 根数组：无法附加元数据，包成 {ok, items, ...}（旧行为下这种情况直接失败，
    //    所以形状变化不会破坏任何能工作的客户端）。
    if (value.is_array()) {
        if (value.empty()) return {};
        std::size_t keep = value.size();
        while (keep > 1 && PrefixSize(value, keep) > budget) {
            keep = keep * 9 / 10;
        }
        Json out = Json::object();
        out["ok"] = true;
        out["items"] = Json::array();
        for (std::size_t i = 0; i < keep; ++i) out["items"].push_back(value[i]);
        out["truncated"] = true;
        out["total"] = value.size();
        out["nextOffset"] = offset_hint + keep;
        return out;
    }

    if (!value.is_object()) return {};

    // 2) 对象：挑元素最多的一级数组字段。
    const std::string* best_key = nullptr;
    std::size_t best_count = 0;
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (!it->is_array()) continue;
        if (it->size() > best_count) {
            best_count = it->size();
            best_key = &it.key();
        }
    }
    if (best_key == nullptr || best_count == 0) return {};

    const Json& source = value[*best_key];
    std::size_t keep = source.size();
    while (keep > 1 && PrefixSize(source, keep) > budget) {
        keep = keep * 9 / 10;
    }

    Json out = value;
    Json kept = Json::array();
    for (std::size_t i = 0; i < keep; ++i) kept.push_back(source[i]);
    out[*best_key] = std::move(kept);
    out["truncated"] = true;
    out["total"] = source.size();
    out["nextOffset"] = offset_hint + keep;
    out["itemsField"] = *best_key;  // 告诉调用方被裁的是哪个字段
    return out;
}

Json McpServer::WrapToolResult(const Json& value_in, std::size_t offset_hint) const {
    Json value = value_in;
    // Python: 非 dict/list 的结果包成 {"ok": true, "result": value}
    if (!value.is_object() && !value.is_array()) {
        Json wrapped = Json::object();
        wrapped["ok"] = true;
        wrapped["result"] = std::move(value);
        value = std::move(wrapped);
    }

    // P-B1：只发一份数据。
    //   默认档：紧凑 JSON 放进 text（旧客户端照旧读 text；JSON 空白无语义）。
    //   协商档：**对象**结果进 structuredContent，text 只留一行摘要。
    //   数组结果在任何档位都只能走 text（structuredContent 我们的约束是对象），
    //   否则它会变成一句 "ok: N items" 而数据丢失。
    // 以前的实现两档都发全量 → 实测占响应 58–64%。
    const std::string compact = value.dump(-1, ' ', false, Json::error_handler_t::replace);
    const bool structured_payload = options_.client_supports_structured && value.is_object();
    std::string text = structured_payload ? SummaryOf(value) : compact;
    // 上限按**载荷**算，不能按 text 算：协商档的 text 是一行摘要，
    // 按它算会让超大 structuredContent 直接漏过去（客户端解析器会崩）。
    if (compact.size() > kMaxToolResultBytes) {
        // P-B2：先尝试按数组分页裁剪（agent 还能继续取）；实在没有可裁的数组
        // （纯大字符串/大对象）才回退到旧的"整份丢弃"。
        Json truncated = TruncatePayload(value, kMaxToolResultBytes, offset_hint);
        if (!truncated.is_null()) {
            value = std::move(truncated);
        } else {
            Json too_large = Json::object();
            too_large["ok"] = false;
            too_large["error"] = "Tool result too large; pass a smaller limit";
            too_large["truncated"] = true;
            value = std::move(too_large);
        }
        text = value.dump(-1, ' ', false, Json::error_handler_t::replace);
        // 裁剪后的载荷仍可能偏大：再走一次协商档判断（结构化时 text 只是摘要）。
        if (structured_payload && value.is_object()) text = SummaryOf(value);
    }

    Json content = Json::array();
    Json text_item = Json::object();
    text_item["type"] = "text";
    text_item["text"] = std::move(text);
    content.push_back(std::move(text_item));

    Json out = Json::object();
    out["content"] = std::move(content);
    out["isError"] = false;
    if (structured_payload && value.is_object()) {
        out["structuredContent"] = std::move(value);
    }
    return out;
}

Json McpServer::CallTool(const Json& params) {
    const Json* name_member = GetMember(params, "name");
    const std::string name = (name_member != nullptr && name_member->is_string())
                                 ? name_member->get<std::string>()
                                 : std::string();

    const Tool* tool = FindTool(name);
    if (tool == nullptr) throw ToolError("Unknown tool: " + name);

    if (tool->requires_flag == "flow" && !options_.allow_flow) {
        throw ToolError("Tool " + name + " is disabled; start the MCP server with --allow-flow");
    }
    if (tool->requires_flag == "write" && !options_.allow_write) {
        throw ToolError("Tool " + name + " is disabled; start the MCP server with --allow-write");
    }

    Json arguments = Json::object();
    if (const Json* args = GetMember(params, "arguments"); args != nullptr && args->is_object()) {
        arguments = *args;
    }

    // required 校验（Python 在调用 handler 之前做，这里保持一致）
    if (const Json* required = GetMember(tool->input_schema, "required");
        required != nullptr && required->is_array()) {
        for (const Json& key : *required) {
            if (!key.is_string()) continue;
            const std::string key_name = key.get<std::string>();
            if (MissingRequired(GetMember(arguments, key_name.c_str()))) {
                throw ToolError("Missing required argument: " + key_name);
            }
        }
    }

    // 进度回调：只有带 progressToken 的调用才接（对应 Python _progress_callback）
    bool has_progress_token = false;
    if (const Json* meta = GetMember(params, "_meta"); meta != nullptr && meta->is_object()) {
        const Json* token = GetMember(*meta, "progressToken");
        has_progress_token = token != nullptr && !token->is_null();
        if (has_progress_token && notification_writer_ && token->is_string()) {
            const std::string token_text = token->get<std::string>();
            dispatcher_.SetProgressEmitter([this, token_text](double progress,
                                                              const std::string& message) {
                Json params_json = Json::object();
                params_json["progressToken"] = token_text;
                params_json["progress"] = progress;
                if (!message.empty()) params_json["message"] = message;
                Json note = Json::object();
                note["jsonrpc"] = "2.0";
                note["method"] = "notifications/progress";
                note["params"] = std::move(params_json);
                notification_writer_(note);
            });
        } else if (has_progress_token && notification_writer_ && token->is_number()) {
            const double token_number = token->get<double>();
            dispatcher_.SetProgressEmitter([this, token_number](double progress,
                                                                const std::string& message) {
                Json params_json = Json::object();
                params_json["progressToken"] = token_number;
                params_json["progress"] = progress;
                if (!message.empty()) params_json["message"] = message;
                Json note = Json::object();
                note["jsonrpc"] = "2.0";
                note["method"] = "notifications/progress";
                note["params"] = std::move(params_json);
                notification_writer_(note);
            });
        } else {
            dispatcher_.SetProgressEmitter({});
        }
    } else {
        dispatcher_.SetProgressEmitter({});
    }

    return dispatcher_.Call(tool->handler, arguments);
}

std::optional<Json> McpServer::Handle(const Json& message) {
    // MCP 允许批量数组
    if (message.is_array()) {
        Json responses = Json::array();
        for (const Json& item : message) {
            if (auto response = HandleSingle(item)) responses.push_back(std::move(*response));
        }
        if (responses.empty()) return std::nullopt;
        return responses;
    }
    return HandleSingle(message);
}

std::optional<Json> McpServer::HandleSingle(const Json& message) {
    if (!message.is_object()) {
        return ErrorResponse(Json(nullptr), -32600, "Invalid Request");
    }

    const Json* method_member = GetMember(message, "method");
    if (method_member == nullptr || !method_member->is_string()) {
        return std::nullopt;  // 不是请求（Python: method is None -> None）
    }
    const std::string method = method_member->get<std::string>();

    Json id = Json(nullptr);
    if (!HasId(message, &id)) {
        // 通知（initialized / cancelled / progress / exit 等）一律不应答
        return std::nullopt;
    }

    Json params = Json::object();
    if (const Json* params_member = GetMember(message, "params");
        params_member != nullptr && params_member->is_object()) {
        params = *params_member;
    }

    if (method == "initialize") {
        std::string requested = "2024-11-05";
        if (const Json* version = GetMember(params, "protocolVersion");
            version != nullptr && version->is_string() && !version->get<std::string>().empty()) {
            requested = version->get<std::string>();
        }
        if (!VersionSupported(requested)) requested = kSupportedVersions[0];

        Json tools_cap = Json::object();
        tools_cap["listChanged"] = false;
        // L3：声明 prompts 能力，否则客户端不会去调 prompts/list（模板等于不存在）。
        Json prompts_cap = Json::object();
        prompts_cap["listChanged"] = false;
        Json capabilities = Json::object();
        capabilities["tools"] = std::move(tools_cap);
        capabilities["prompts"] = std::move(prompts_cap);

        Json server_info = Json::object();
        server_info["name"] = "fpga-lsp-mcp";
        server_info["version"] = options_.server_version;

        Json result = Json::object();
        result["protocolVersion"] = requested;
        result["capabilities"] = std::move(capabilities);
        result["serverInfo"] = std::move(server_info);
        result["instructions"] = kInstructions;

        // P-B1：只有客户端显式声明才走 structuredContent 档。
        // 不按协议版本自动开启 —— 说 2025-06-18 但不声明能力的客户端仍然只读 text。
        const Json* client_caps = GetMember(params, "capabilities");
        const Json* experimental =
            client_caps != nullptr ? GetMember(*client_caps, "experimental") : nullptr;
        const Json* structured =
            experimental != nullptr ? GetMember(*experimental, "structuredContent") : nullptr;
        options_.client_supports_structured =
            structured != nullptr && structured->is_boolean() && structured->get<bool>();
        return Response(id, std::move(result));
    }

    if (method == "ping") return Response(id, Json::object());

    if (method == "tools/list") {
        Json result = Json::object();
        result["tools"] = VisibleTools(options_.allow_flow, options_.allow_write);
        return Response(id, std::move(result));
    }

    if (method == "resources/list") {
        Json result = Json::object();
        // P-B6：由 dispatcher 提供（Bridge 给出工程/报告/flow 三类只读资源）。
        Json resources = dispatcher_.ListResources();
        result["resources"] = resources.is_array() ? std::move(resources) : Json::array();
        return Response(id, std::move(result));
    }
    if (method == "resources/read") {
        const std::string uri = jsonrpc::GetString(params, "uri");
        std::optional<std::string> text;
        if (!uri.empty()) text = dispatcher_.ReadResource(uri);
        if (!text.has_value()) {
            return ErrorResponse(id, -32602, "Unknown resource: " + uri);
        }
        Json content = Json::object();
        content["uri"] = uri;
        content["mimeType"] = "application/json";
        content["text"] = std::move(*text);
        Json contents = Json::array();
        contents.push_back(std::move(content));
        Json result = Json::object();
        result["contents"] = std::move(contents);
        return Response(id, std::move(result));
    }
    if (method == "resources/templates/list") {
        Json result = Json::object();
        result["resourceTemplates"] = Json::array();
        return Response(id, std::move(result));
    }
    if (method == "prompts/list") {
        Json result = Json::object();
        result["prompts"] = PromptsListJson();
        return Response(id, std::move(result));
    }
    // L3：prompts/get —— 按 name 取模板并把 arguments 代入。
    // 未知 name 是**协议级错误**（不是工具失败）：参数非法，回 -32602。
    if (method == "prompts/get") {
        const std::string name = jsonrpc::GetString(params, "name");
        Json arguments = Json::object();
        if (const Json* args = GetMember(params, "arguments");
            args != nullptr && args->is_object()) {
            arguments = *args;
        }
        const std::string text = PromptText(name, arguments);
        if (text.empty()) {
            return ErrorResponse(id, jsonrpc::kInvalidParams, "Unknown prompt: " + name);
        }
        std::string description;
        for (const PromptDef& def : PromptTable()) {
            if (name == def.name) {
                description = def.description;
                break;
            }
        }
        Json content = Json::object();
        content["type"] = "text";
        content["text"] = text;
        Json message = Json::object();
        message["role"] = "user";
        message["content"] = std::move(content);
        Json messages = Json::array();
        messages.push_back(std::move(message));
        Json result = Json::object();
        result["description"] = std::move(description);
        result["messages"] = std::move(messages);
        return Response(id, std::move(result));
    }
    if (method == "logging/setLevel") return Response(id, Json::object());

    if (method == "tools/call") {
        try {
            // P-B2：让裁剪后的 nextOffset 是绝对偏移。
            std::size_t offset = 0;
            if (const Json* args = GetMember(params, "arguments");
                args != nullptr && args->is_object()) {
                if (const Json* value = GetMember(*args, "offset");
                    value != nullptr && value->is_number_integer()) {
                    const auto parsed = value->get<std::int64_t>();
                    if (parsed > 0) offset = static_cast<std::size_t>(parsed);
                }
            }
            return Response(id, WrapToolResult(CallTool(params), offset));
        } catch (const ToolError& error) {
            return Response(id, WrapToolError(error.what()));
        } catch (const std::exception& error) {
            // Python: `"%s: %s" % (type(exc).__name__, exc)` —— C++ 没有稳定的类型名，
            // 用 InternalError 前缀，保证客户端看到的是"工具失败"而不是协议错误。
            return Response(id, WrapToolError("InternalError: " + std::string(error.what())));
        } catch (...) {
            return Response(id, WrapToolError("InternalError: unknown exception"));
        }
    }

    if (method == "shutdown") return Response(id, Json(nullptr));

    return ErrorResponse(id, -32601, "Method not found: " + method);
}

}  // namespace fpgalsp::mcp