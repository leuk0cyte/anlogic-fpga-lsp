// =============================================================================
//  src/mcp/tools.cpp —— 48 个工具的 schema 表（逐字来自 Python 参考实现）
// =============================================================================
//  ⚠ 本文件的 schema 常量**不要手改**：任何改动都必须先改 Python 版
//    (%PYREF%\fpgalsp\mcp.py 的 TOOLS) 并同步契约，否则
//    `tests/compare_mcp_schema.py` 会立刻变红 —— 那是故意的。
//
//  生成方式（开发期，一次性）：从 fpgalsp.mcp.TOOLS 导出后机械转写，
//  转写后由对比脚本逐字段校验（name/title/description/inputSchema/annotations）。
// =============================================================================

#include "mcp/tools.h"

#include <map>
#include <mutex>
#include <utility>

namespace fpgalsp::mcp {

// -----------------------------------------------------------------------------
// schema 构造器
// -----------------------------------------------------------------------------

Json Str(std::string description, std::vector<std::string> enum_values) {
    Json prop = Json::object();
    prop["type"] = "string";
    prop["description"] = std::move(description);
    if (!enum_values.empty()) prop["enum"] = std::move(enum_values);
    return prop;
}

Json Int(std::string description, std::optional<int> minimum, std::optional<int> maximum) {
    Json prop = Json::object();
    prop["type"] = "integer";
    prop["description"] = std::move(description);
    if (minimum.has_value()) prop["minimum"] = *minimum;
    if (maximum.has_value()) prop["maximum"] = *maximum;
    return prop;
}

Json Bool(std::string description) {
    Json prop = Json::object();
    prop["type"] = "boolean";
    prop["description"] = std::move(description);
    return prop;
}

Json StrNoDesc() {
    Json prop = Json::object();
    prop["type"] = "string";
    return prop;
}

Json Arr(std::string description, Json items) {
    Json prop = Json::object();
    prop["type"] = "array";
    prop["description"] = std::move(description);
    prop["items"] = std::move(items);
    return prop;
}

Json Schema(std::vector<std::pair<std::string, Json>> properties,
            std::vector<std::string> required) {
    Json schema = Json::object();
    Json props = Json::object();
    for (auto& [name, value] : properties) props[name] = std::move(value);
    schema["type"] = "object";
    schema["properties"] = std::move(props);
    schema["required"] = std::move(required);
    schema["additionalProperties"] = false;
    return schema;
}

// -----------------------------------------------------------------------------
// 工具表（顺序是契约的一部分：Python 对齐的 39 个在前，C++ 独有工具紧跟其后，
// 新增的 probe_verify 紧跟在 probe_capture 之后 —— 客户端可能依赖首次出现的顺序）
// -----------------------------------------------------------------------------

const std::vector<Tool>& AllTools() {
    static const std::vector<Tool> kTools = {
        // 放在表头：tools/list 按声明序输出，agent 第一眼就能看到"先读我"。
        // 只读（requires 为空 -> readOnlyHint=true），默认档也可见。
        Tool{.name="fpga_guide",
             .title="Usage guide",
             .description="Return the worked-through usage guide for one topic: the tool call sequence, the key arguments, the preconditions and the failure modes to check. Read this before improvising a multi-step task. Default topic: overview.",
             .input_schema=Schema({{"topic", Str("Guide topic.", {"overview", "project", "index", "flow", "probe", "program", "troubleshoot"})}}),
             .handler="guide", .requires_flag="",},
        Tool{.name="fpga_project_list",
             .title="List FPGA projects",
             .description="Discover Anlogic TangDynasty .al projects under a workspace root.",
             .input_schema=Schema({{"root", Str("Optional root directory. Default: MCP server root.")}}, {}),
             .handler="project_list", .requires_flag="",},
        Tool{.name="fpga_project_info",
             .title="Project information",
             .description="Return .al metadata, file counts, warnings, runs and TD installation summary.",
             .input_schema=Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")}}, {}),
             .handler="project_info", .requires_flag="",},
        Tool{.name="fpga_project_files",
             .title="Project build files",
             .description="List HDL/ADC/SDC/IP files recorded in the .al with build/exclusion state. `files` contains only the files that take part in the build; `counts` splits every (kind-filtered) file into in_build/excluded/missing and `excluded` says why each excluded or missing file is out (auto-excluded|disabled|missing).",
             .input_schema=Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")},
                                          {"kind", Str("Filter by file kind.", {"all", "hdl", "verilog", "vhdl", "adc", "sdc", "ip"})},
                                          {"include_excluded", Bool("Include files excluded from or missing in the build.")},
                                          {"limit", Int("Maximum entries in `excluded` (default 50).", 1, 2000)},}, {}),
             .handler="project_files", .requires_flag="",},
        Tool{.name="fpga_project_structure",
             .title="Module hierarchy",
             .description="Return the nested module/instance hierarchy rooted at the project top module. Line/column values are 1-based.",
             .input_schema=Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")},
                                          {"include_ports", Bool("Attach each module's ports and parameters.")},
                                          {"include_primitives", Bool("Include device primitive instances.")},
                                          {"max_depth", Int("Maximum hierarchy depth.", 1, 64)},
                                          {"max_nodes", Int("Approximate node budget.", 1, 50000)},}, {}),
             .handler="project_structure", .requires_flag="",},
        Tool{.name="fpga_project_reports",
             .title="Parsed FPGA reports",
             .description="Return QoR/timing/area/flow/DRC-CDC reports, optionally for the syn or phy stage. Raw report text is omitted unless include_raw is true.",
             .input_schema=Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")},
                                          {"stage", Str("Report stage.", {"all", "syn", "phy"})},
                                          {"section", Str("Report section.", {"summary", "qor", "timing", "area", "flow", "drc_cdc", "analysis", "strategies", "files", "all"})},
                                          {"limit", Int("Maximum rows/paths/metrics per section.", 1, 2000)},
                                          {"include_raw", Bool("Include raw report text (may be very large).")},}, {}),
             .handler="project_reports", .requires_flag="",},
        Tool{.name="fpga_flow_status",
             .title="Flow status",
             .description="Return the current flow state (running/stage/elapsed/lastMessage) and artifacts.",
             .input_schema=Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")}}, {}),
             .handler="flow_status", .requires_flag="",},
        Tool{.name="fpga_flow_run",
             .title="Run FPGA flow",
             // L4：前置条件 / 成本 / 失败判据 / 可照抄的参数对象。
             // 只加在 description 文本里 —— inputSchema 不动（单测要求 schema 里
             // 每个数组都声明 items，加 examples 会红）。
             .description="Start synthesis / place&route / full flow in the background and return a runId. Requires --allow-flow or fpgaLsp.mcp.allowFlow.\n"
             "Preconditions: --allow-flow; a detected TD install; a valid device and top module in the .al; no other flow running for this project.\n"
             "Cost: syn ~2-5 min, phy 5-15 min, all 8-20 min; returns immediately (async) - call fpga_flow_wait in the SAME MCP session.\n"
             "Failure modes: ok:true means only the requested stages finished, not that timing/DRC is clean or that probes are in the bit. A phy-only run reuses the old netlist.\n"
             "Example: {\"stages\":[\"all\"],\"jobs\":6}",
             .input_schema=Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")},
                                          {"stages", Arr("Stages to run.", Str("Stage.", {"syn", "phy", "all", "route"}))},
                                          {"jobs", Int("TD multirun concurrency: how many runs may run in parallel (1-6, TD PRJ-8802). Single-run launches are effectively 1 (single-step runs force 1, TD PRJ-8803). Default 6 = the TD GUI default.", 1, 6)},}, {}),
             .handler="flow_run", .requires_flag="flow",},
        Tool{.name="fpga_flow_run_step",
             .title="Run or reset one GUI flow step",
             .description="Per-step control that mirrors the TD GUI Flow view: right-click a step -> Run (skip when it already finished) or Reset and Run (rerun just that step). Steps map to the GUI list: read_design / opt_rtl / opt_gate (syn) and opt_place / opt_route / bitgen (phy).\n"
             "Preconditions: --allow-flow; no other flow running. Cost: one TD step (read_design seconds, opt_rtl/opt_gate ~1 min, place/route/bitgen minutes); returns immediately (async) - call fpga_flow_wait in the SAME session.\n"
             "Behaviour: force:false and the step already has its .end marker -> {ok:true, skipped:true}; force:true -> clears ONLY that step and the steps after it (earlier steps keep their products), then runs `launch_runs <run> -step <step>` - so exactly one step runs and the downstream steps show as not run. A syn step also invalidates the whole phy run (markers, watcher sidecar, bitstream).\n"
             "Example: {\"step\":\"opt_gate\",\"force\":false}",
             .input_schema=Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")},
                                     {"step", Str("GUI step name: read_design / opt_rtl / opt_gate / opt_place / opt_route / bitgen.")},
                                     {"force", Bool("true = Reset run (always rerun, clears downstream steps); false = Run (skip when already done).")},
                                     {"jobs", Int("TD multirun concurrency: how many runs may run in parallel (1-6, TD PRJ-8802). Single-run launches are effectively 1 (single-step runs force 1, TD PRJ-8803). Default 6 = the TD GUI default.", 1, 6)},}, {}),
             .handler="flow_run_step", .requires_flag="flow",},
        Tool{.name="fpga_flow_cancel",
             .title="Cancel FPGA flow",
             .description="Cancel the currently running flow. Requires --allow-flow.",
             .input_schema=Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")}}, {}),
             .handler="flow_cancel", .requires_flag="flow",},
        Tool{.name="fpga_analysis_run",
             .title="Run DRC/CDC analysis",
             .description="Run TD DRC/CDC analysis on the placed/routed database. Requires --allow-flow.",
             .input_schema=Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")}}, {}),
             .handler="analysis_run", .requires_flag="flow",},
        Tool{.name="fpga_strategy_compare",
             .title="Compare TD strategies",
             .description="Synthesize and place&route several TD strategies on a project copy and compare them. Long-running; requires --allow-flow.",
             .input_schema=Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")},
                                          {"strategies", Arr("Strategy names (default: all available).", StrNoDesc())},
                                          {"jobs", Int("Parallel jobs.", 1, 64)},}, {}),
             .handler="strategy_compare", .requires_flag="flow",},
        Tool{.name="fpga_project_update",
             .title="Update .al in TD",
             .description="Run check_project_version and open_project -update to resync the .al. Requires --allow-write.\n"
             "dry_run:true previews it: reports what TD work would run and writes nothing; no TD process is started.",
             .input_schema=Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")},
                      {"dry_run", Bool("preview only; nothing is written and no TD process is started")},}, {}),
             .handler="project_update", .requires_flag="write",},
        Tool{.name="fpga_project_add_source",
             .title="Add source to .al",
             .description="Append a Verilog/VHDL file to the .al Source_Files section. Requires --allow-write.\n"
             "dry_run:true previews the insertion (insertAtLine + the text that would be added) and writes nothing; no TD process is started.",
             .input_schema=Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")},
                      {"file", Str("Source file path (absolute or relative to the project/root).")},
                      {"update", Bool("Run open_project -update after editing the .al.")},
                      {"dry_run", Bool("preview only; nothing is written and no TD process is started")},}, {"file"}),
             .handler="project_add_source", .requires_flag="write",},
        Tool{.name="fpga_project_rename",
             .title="Rename .al project",
             .description="Change the project <Name> in the .al. Requires --allow-write.\n"
             "dry_run:true previews the rename (from/to) and writes nothing; no TD process is started.",
             .input_schema=Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")},
                      {"name", Str("New project name.")},
                      {"dry_run", Bool("preview only; nothing is written and no TD process is started")},}, {"name"}),
             .handler="project_rename", .requires_flag="write",},
        Tool{.name="fpga_document_symbols",
             .title="Document symbols",
             .description="Return the module/port/signal/parameter/instance symbol tree for a source file.",
             .input_schema=Schema({{"file", Str("Source file path (absolute or relative to the project/root).")}}, {"file"}),
             .handler="document_symbols", .requires_flag="",},
        Tool{.name="fpga_workspace_symbols",
             .title="Workspace symbol search",
             .description="Search project symbols by name.",
             .input_schema=Schema({{"query", Str("Search query.")},
                                          {"limit", Int("Maximum number of returned items.", 1, 2000)},
                                          {"offset", Int("Pagination offset (use nextOffset from a truncated result).", 0, 1000000)},}, {"query"}),
             .handler="workspace_symbols", .requires_flag="",},
        Tool{.name="fpga_search_hierarchy",
             .title="Search hierarchy",
             .description="Search module definitions and instance names in the project hierarchy.",
             .input_schema=Schema({{"query", Str("Module or instance name substring.")},
                                          {"kind", Str("Match kind.", {"both", "module", "instance"})},
                                          {"limit", Int("Maximum number of returned items.", 1, 2000)},}, {"query"}),
             .handler="search_hierarchy", .requires_flag="",},
        Tool{.name="fpga_definition",
             .title="Go to definition",
             .description="Return definition locations for the symbol at a 1-based position.",
             .input_schema=Schema({{"file", Str("Source file path (absolute or relative to the project/root).")},
                                          {"line", Int("1-based line number.", 1)},
                                          {"column", Int("1-based column number.", 1)},}, {"file", "line", "column"}),
             .handler="definition", .requires_flag="",},
        Tool{.name="fpga_references",
             .title="Find references",
             .description="Return all references for the symbol at a 1-based position.",
             .input_schema=Schema({{"file", Str("Source file path (absolute or relative to the project/root).")},
                                          {"line", Int("1-based line number.", 1)},
                                          {"column", Int("1-based column number.", 1)},
                                          {"include_declaration", Bool("Include the declaration itself (default true).")},}, {"file", "line", "column"}),
             .handler="references", .requires_flag="",},
        Tool{.name="fpga_hover",
             .title="Hover information",
             .description="Return hover markdown for the symbol at a 1-based position.",
             .input_schema=Schema({{"file", Str("Source file path (absolute or relative to the project/root).")},
                                          {"line", Int("1-based line number.", 1)},
                                          {"column", Int("1-based column number.", 1)},}, {"file", "line", "column"}),
             .handler="hover", .requires_flag="",},
        Tool{.name="fpga_symbol_path",
             .title="Hierarchical symbol path",
             .description="Return the instance path of a wire/reg signal, e.g. U_OC_CHK/U_DATA_ABS/data_o_r.",
             .input_schema=Schema({{"file", Str("Source file path (absolute or relative to the project/root).")},
                                          {"line", Int("1-based line number.", 1)},
                                          {"column", Int("1-based column number.", 1)},}, {"file", "line", "column"}),
             .handler="symbol_path", .requires_flag="",},
        Tool{.name="fpga_call_hierarchy",
             .title="Call hierarchy",
             .description="Return caller/callee modules for a module, or for the module/instance at a position.",
             .input_schema=Schema({{"file", Str("Source file path (absolute or relative to the project/root).")},
                                          {"line", Int("1-based line number.", 1)},
                                          {"column", Int("1-based column number.", 1)},
                                          {"module", Str("Module name (alternative to file/line/column).")},
                                          {"direction", Str("Direction.", {"both", "incoming", "outgoing"})},
                                          {"depth", Int("Reserved depth hint.", 1, 16)},}, {}),
             .handler="call_hierarchy", .requires_flag="",},
        Tool{.name="fpga_completion",
             .title="Code completion",
             .description="Return LSP completion items at a 1-based position (including instantiation snippets).",
             .input_schema=Schema({{"file", Str("Source file path (absolute or relative to the project/root).")},
                                          {"line", Int("1-based line number.", 1)},
                                          {"column", Int("1-based column number.", 1)},
                                          {"prefix", Str("Optional label prefix filter.")},
                                          {"limit", Int("Maximum items.", 1, 500)},
                                          {"include_snippets", Bool("Include snippet completion items (default true).")},}, {"file", "line", "column"}),
             .handler="completion", .requires_flag="",},
        Tool{.name="fpga_code_actions",
             .title="Code actions",
             .description="List LSP code actions for a 1-based range (read-only unless include_edits is requested).",
             .input_schema=Schema({{"file", Str("Source file path (absolute or relative to the project/root).")},
                                          {"start_line", Int("1-based line number.", 1)},
                                          {"start_column", Int("1-based column number.", 1)},
                                          {"end_line", Int("1-based end line.", 1)},
                                          {"end_column", Int("1-based end column.", 1)},
                                          {"include_edits", Bool("Include edit payloads.")},}, {"file", "start_line", "start_column"}),
             .handler="code_actions", .requires_flag="",},
        Tool{.name="fpga_rename_edits",
             .title="Rename edits",
             .description="Return rename workspace edits for a symbol (does not write files).",
             .input_schema=Schema({{"file", Str("Source file path (absolute or relative to the project/root).")},
                                          {"line", Int("1-based line number.", 1)},
                                          {"column", Int("1-based column number.", 1)},
                                          {"new_name", Str("New identifier.")},}, {"file", "line", "column", "new_name"}),
             .handler="rename_edits", .requires_flag="",},
        Tool{.name="fpga_format",
             .title="Format source",
             .description="Format Verilog/SystemVerilog text or a file and return the result (does not write files).",
             .input_schema=Schema({{"file", Str("Source file path (absolute or relative to the project/root).")},
                                          {"text", Str("Source text (used when file is omitted).")},
                                          {"start_line", Int("Optional 1-based range start.", 1)},
                                          {"end_line", Int("Optional 1-based range end.", 1)},
                                          {"tab_size", Int("Indent width (default 4).", 1, 16)},
                                          {"insert_spaces", Bool("Use spaces (default true).")},}, {}),
             .handler="format", .requires_flag="",},
        Tool{.name="fpga_instantiation",
             .title="Generate instantiation",
             .description="Return a named-port instantiation template for a project module.",
             .input_schema=Schema({{"module", Str("Module name.")},
                                          {"instance_name", Str("Optional instance name (default u_<module>).")},
                                          {"indent", Str("Indent unit (default four spaces).")},}, {"module"}),
             .handler="instantiation", .requires_flag="",},
        Tool{.name="fpga_module_info",
             .title="Module information",
             .description="Return module ports, parameters, definition and instantiation sites.",
             .input_schema=Schema({{"module", Str("Module name.")},
                                          {"project", Str("Optional .al path, project directory, or project name. Default: active project.")},}, {"module"}),
             .handler="module_info", .requires_flag="",},
        Tool{.name="fpga_duplicate_modules",
             .title="Duplicate modules",
             .description="Find modules defined in more than one build file.",
             .input_schema=Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")}}, {}),
             .handler="duplicate_modules", .requires_flag="",},
        Tool{.name="fpga_diagnostics",
             .title="Diagnostics",
             .description="Return plugin diagnostics (HDL/ADC/SDC by default; optional Verible and TD tiers) for a file or the whole project.",
             .input_schema=Schema({{"file", Str("Source file path (absolute or relative to the project/root).")},
                                          {"project", Str("Optional .al path, project directory, or project name. Default: active project.")},
                                          {"sources", Arr("Diagnostic sources.", Str("Source.", {"hdl", "adc", "sdc", "verible", "td"}))},
                                          {"limit", Int("Maximum diagnostics.", 1, 2000)},}, {}),
             .handler="diagnostics", .requires_flag="",},
        Tool{.name="fpga_device_info",
             .title="Device information",
             .description="Return the project device/package and optionally filtered pin capabilities. May start a TD session to load the device database.",
             .input_schema=Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")},
                                          {"include_pins", Bool("Include pin rows.")},
                                          {"pin_filter", Str("Optional pin/location substring filter.")},
                                          {"limit", Int("Maximum pins.", 1, 5000)},
                                          {"refresh", Bool("Reload the device database.")},}, {}),
             .handler="device_info", .requires_flag="",},
        Tool{.name="fpga_constraints_status",
             .title="Constraint coverage",
             .description="Return ADC port coverage and SDC clock-constraint summary.",
             .input_schema=Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")}}, {}),
             .handler="constraints_status", .requires_flag="",},
        Tool{.name="fpga_td_info",
             .title="TD installation info",
             .description="Return detected TangDynasty installations and the selected version.",
             .input_schema=Schema({}, {}),
             .handler="td_info", .requires_flag="",},
        Tool{.name="fpga_testbench",
             .title="Generate a testbench skeleton",
             .description="Generate a compilable testbench skeleton for a module (default: the top module), with port declarations, clock/reset guess and a parse self-check.",
             .input_schema=Schema({{"module", Str("Module name (default: project top).")},
                     {"clock", Str("Clock port name (default: guessed from clk/clock).")},
                     {"reset", Str("Reset port name (default: guessed from rst/reset).")},
                     {"project", Str("Optional .al path, project directory, or project name.")},},
                    {}),
             .handler="testbench", .requires_flag="",},
        Tool{.name="fpga_run_diff",
             .title="Compare two flow runs",
             .description="Compare QoR/timing/area metrics between two runs under <project>_Runs (default: the last two).",
             .input_schema=Schema({{"from", Str("Run name to compare from (default: second newest).")},
                     {"to", Str("Run name to compare to (default: newest).")},
                     {"project", Str("Optional .al path, project directory, or project name.")},},
                    {}),
             .handler="run_diff", .requires_flag="",},
        Tool{.name="fpga_job_status",
             .title="Job status",
             .description="Status of a long-running flow job (job = the runId returned by fpga_flow_run).",
             .input_schema=Schema({{"job", Str("Job id returned by fpga_flow_run.")},
                     {"project", Str("Optional .al path, project directory, or project name.")},},
                    {}),
             .handler="job_status", .requires_flag="",},
        Tool{.name="fpga_job_cancel",
             .title="Cancel job",
             .description="Cancel a running flow job by id (no-op when it already finished).",
             .input_schema=Schema({{"job", Str("Job id returned by fpga_flow_run.")},
                     {"project", Str("Optional .al path, project directory, or project name.")},},
                    {}),
             .handler="job_cancel", .requires_flag="flow",},
        Tool{.name="fpga_cables",
             .title="Cable / online device",
             .description="Read-only list_cable/online-device query (never programs the FPGA).",
             .input_schema=Schema({}, {}),
             .handler="cables", .requires_flag="",},
        // ---- 0.16.0：片上探针（Anlogic ChipWatcher）----
        //   status 纯读文件；config 改 chip_debugger/user.mdn；capture 碰真实硬件。
        Tool{.name="fpga_probe_status",
             .title="On-chip probe status",
             .description="Report the ChipWatcher probe setup of the active project: the signals marked in chip_debugger/user.mdn and the compiled watcher (phy_1/cw/compiled.cwc): clock, sample depth, buses, and whether a probe-enabled bitstream exists. The `sampling` object answers whether the sample clock/depth can be changed at capture time: `editable` is always false because they are compile-time (TD only writes them from the GUI ChipWatcher instance before the Gate stage). Disabled entries are reported separately: `disabled` (requested, from the state dir's probe/config.txt), `disabled_count`, and watcher.disabled_count/disabled_bus_count/disabled_net_count vs enabled_net_count (what the compiled cwc actually marks as disabled; those entries consume no debug resource). Read-only; no TD session, no hardware access.",
             .input_schema=Schema({}, {}),
             .handler="probe_status", .requires_flag="",},
        Tool{.name="fpga_probe_config",
             .title="Configure on-chip probes",
             .description="Read or replace the probe signal list in chip_debugger/user.mdn (the file Anlogic TangDynasty insert_debugger reads). Without write:true this is a dry run that only reports added/removed signals. Writing changes what the next phy flow embeds into the bitstream; it does not touch the board.\n"
             "Preconditions: a readable project; write:true needs --allow-write and a non-empty signals list.\n"
             "Cost: milliseconds. It only edits chip_debugger/user.mdn - no TD session, no hardware, no bitstream.\n"
             "Failure modes: ok:true with written:true does NOT mean the probes are in the bitstream; rerun phy and confirm with fpga_probe_status (watcher.stale / watcher.missing_in_bit). An empty list is refused.\n"
             "Disabled entries: `disabled` disables a watched bus or a single bit WITHOUT removing it from the list. TD's ChipWatcher list can enable/disable entries and a disabled entry consumes no debug resource (the compiled cwc carries bus is_enabled=\"no\" and net data=\"disable\" trig=\"disable\"). chip_debugger/user.mdn has no disable syntax, so the backend expresses it only through the authored cwc it hands to insert_debugger -cwc: set apply_build_config:true and rerun stages:[\"all\"], otherwise the entries stay enabled in the bitstream. The response has disabled/disabled_count/unresolved_disabled plus enabled_bus_count/enabled_bit_count (disabled bits excluded) and requires_apply_build_config.\n"
             "Example: {\"signals\":[\"WHG/e2e_cnt2l[15:0]\",\"WHG/e2e_hstat[4:0]\"],\"disabled\":[\"WHG/e2e_hstat\"],\"write\":true,\"apply_build_config\":true}\n"
             "dry_run:true is the same preview as write:false: it reports before/after/added/removed and writes nothing (user.mdn and the state dir's probe config are left untouched); no TD process is started.",
             .input_schema=Schema({{"clock", Str("Sample clock net for the ChipWatcher, e.g. reg_init/clk. TD treats it as a compile-time value: it is only honoured at the Optimize Gate step when the backend hands TD an authored cwc, so it takes effect on the next stages:[\"all\"] run, not at capture time.")},
                      {"cwc_bus_order", Arr("Bus order (one entry per watched bus, e.g. [\"WHG/e2e_cnt2l\",\"WHG/e2e_hstat\",...]) used when this backend regenerates phy_1/cw/compiled.cwc after a phy run. TD reorders buses and its watcher artifacts carry only widths, so a mixed-width list (16/5/16/16) cannot be mapped automatically; give the order here once and every later flow writes a correct cwc. Persisted in the state dir (default %TEMP%\fpgalsp\<project>-<hash>) under probe/config.txt; the project directory is never written.", StrNoDesc())},
                      {"write_sidecar_cwc", Bool("Write a generated phy_1/cw/compiled.cwc after the phy run (default false). The generated file is not yet equivalent to the one TD itself writes - TD reports a different Chipwatcher code and capture then hangs - so only enable it to experiment.")},
                      {"apply_build_config", Bool("Actually hand TD an authored cwc at the Optimize Gate step so `clock`/`depth` take effect (default false). Measured: TD honours the depth, but a phy place after that crashed twice with 'Run phy_1 has exited unexpectedly' - the authored cwc still disagrees with the bitstream resources. Enable only when you are prepared to experiment.")},
                      {"depth", Int("Sample depth for the ChipWatcher (e.g. 2048). Same rule as clock: compile-time in TD, takes effect on the next stages:[\"all\"] run. The backend writes it into the cwc it feeds to insert_debugger - measured: depth 2048 is honoured by TD 2026.2.", 16, 65536)},
                      {"signals", Arr("Replacement probe signals, e.g. WHG/pwm_reg0_i[0]. Omit to read the current list.",
                                      StrNoDesc()),},
                      {"disabled", Arr(R"(Entries to disable without removing them from the watch list. A whole bus ("WHG/e2e_hstat", all of its bits) or a single bit ("WHG/e2e_hstat[2]"); ranges ("WHG/x[4:0]") are expanded. They stay in chip_debugger/user.mdn and in the generated cwc (bus is_enabled="no", net data="disable" trig="disable", signal_vec cond="dont_care") but consume no ChipWatcher debug resource, so they are excluded from enabled_bus_count/enabled_bit_count and from the cwc DIN/bit order. Persisted as disabled=... in the state dir's probe/config.txt (write:true). Omitting it keeps the persisted list; send [] to clear. Entries not present in signals come back in unresolved_disabled plus a warning (not a failure).)",
                                       StrNoDesc()),},
                      {"write", Bool("Actually rewrite chip_debugger/user.mdn (default false = dry run).")},
                      {"dry_run", Bool("preview only; nothing is written and no TD process is started")},},
                     {}),
             .handler="probe_config", .requires_flag="write",},
        Tool{.name="fpga_probe_capture",
             .title="Capture on-chip probe data",
             .description="Read the live values of the embedded probes over JTAG and return them per sample. Runs a TD session: import_device, open_project, config_chipwatcher, get_hw_cws, connect_cable, download (mode jtag_burst), run_hw_cw -trigger_now, wait_on_hw_cw, write_hw_cw_data. Requires a board and a probe-enabled bitstream. Refuses when chip_debugger/user.mdn no longer matches the probes embedded in the bitstream. download:true reprograms SRAM and therefore needs confirm:true.\n"
             "Trigger values: global trigger_type/bus_op/window_* plus per-bus `buses:[{name,op,value}]` (value is a binary bit string, the same format as the capture CSV, e.g. \"0000000000000000\" = trigger when the counter wraps to 0). A per-bus condition only takes effect when bus_op (the global comparator) is also given AND trigger_enabled:true; in TD a bus op=dont_care disables the per-net conditions.\n"
             "Preconditions: phy_1/cw/compiled.cwc and compiled.bit from a phy run; a board on a cable; no other hardware operation. download auto/true needs confirm:true.\n"
             "Cost: ~20-60 s (17.4 s measured with the bit already loaded; a download adds the SRAM load); phy 5-15 min if a rebuild is needed first.\n"
             "Failure modes: ok:true means samples were read, not that they are the signals you marked - stale/missing_in_bit means the bit is older than user.mdn. Sample clock and depth are compile-time; only trigger_*/window_* are runtime.\n"
             "Example: {\"download\":\"auto\",\"confirm\":true,\"trigger_now\":false,\"bus_op\":\"equal\",\"trigger_enabled\":true,\"buses\":[{\"name\":\"WHG/e2e_cnt2l\",\"op\":\"equal\",\"value\":\"0000000000000000\"}]}",
             .input_schema=Schema({{"project", Str("Optional .al path, project directory, or project name.")},
                      {"instance", Str("Watcher instance name (default: read from compiled.cwc).")},
                      {"cable", Str("Cable index for -cable (default 0).")},
                      {"download", Str("Whether to load phy_1/cw/compiled.bit first. auto: capture on the bitstream already in the device and only download when that fails (default). true: always reload. false: never reload (pure read, no confirm needed).",
                                      {"auto", "true", "false"}),},
                      {"speed", Int("Download clock step for -spd: 2=200kHz 3=500kHz 4=1MHz 5=2MHz 6=3MHz 7=5MHz (GUI default) 8=8MHz 9=16MHz.", 2, 9)},
                      {"trigger_now", Bool("Trigger immediately instead of waiting for the configured trigger condition (default true).")},
                      {"confirm", Bool("Required when download is true: acknowledges that the FPGA SRAM is reprogrammed.")},
                      {"allow_stale", Bool("Capture even when user.mdn no longer matches the probes embedded in the bitstream (default false).")},
                      {"trigger_position", Int("Trigger position inside the sample window (0..depth-1). Applied at capture time, no rebuild needed.", 0, 65535)},
                      {"window_num", Int("Number of trigger windows (1..sample depth).", 1, 1024)},
                      {"window_depth", Int("Samples per window (<= sample depth).", 1, 65536)},
                      {"trigger_type", Str("Condition combinator.", {"basic_and", "basic_or"})},
                      {"trigger_enabled", Bool("Enable or disable the trigger condition (required for per-bus conditions to take effect).")},
                      {"bus_op", Str("Comparator applied to every watched bus (required for per-bus conditions; dont_care disables them in TD).", {"dont_care", "equal", "not_equal", "less", "less_equal", "greater", "greater_equal", "bit_trigger"})},
                      {"allow_disabled_capture", Bool("Try to capture even when the bitstream was built with disabled probes (default false). TD 2026.2 cannot read such a bitstream back in batch mode: readback reports KIT-8445 (sample depth shorter than the set value, e.g. 54/48) and any width-matched cwc turns into a Chipwatcher-code mismatch that hangs the session. Clear disabled[] and rerun stages:[\"all\"] instead.")},
                      {"generate_cwc", Bool("Allow this backend to write phy_1/cw/compiled.cwc from the TD watcher artifacts when the run produced none (default false). Every <phy_eram> row is copied from phy_1/<prj>_inst.bid - including each RAM's own working-mode depth, which TD's runtime trigger needs - so the sidecar matches the bitstream (verified end-to-end on PH1A100, 1024 samples). A flow run writes this sidecar automatically after phy, so this is only needed for bitstreams built before that.")},
                      {"cwc_bus_order", Arr("Bus order to use when this backend has to rebuild phy_1/cw/compiled.cwc from TD's own artifacts (batch phy runs do not write it). TD reorders buses and its watcher artifacts carry only widths, so a mixed-width list (e.g. 16/5/16/16) cannot be mapped automatically - pass the real order here, names as in user.mdn (`WHG/e2e_hstat` or `e2e_hstat`).", StrNoDesc())},
                      {"buses", Arr("Per-bus trigger values. value is a binary bit string (left = msb), e.g. \"0000000000000000\" for 'counter wrapped to 0'. Empty array = no per-bus condition. Requires bus_op and trigger_enabled:true to take effect in TD.",
                                    Schema({{"name", Str("Watched bus name as it appears in the watcher, e.g. WHG/e2e_cnt2l or WHG/pwm_reg0_i[15:0].")},
                                            {"op", Str("Per-bus comparator (same vocabulary as bus_op, without dont_care).", {"equal", "not_equal", "less", "less_equal", "greater", "greater_equal", "bit_trigger"})},
                                            {"value", Str("Expected value as a binary bit string, left = msb, same format as the capture CSV.")},},
                                           {"name"})),},
                      {"timeout", Int("wait_on_hw_cw timeout in seconds (default 30).", 1, 600)},
                      {"csv", Str("Optional absolute CSV output path (default phy_1/cw/capture_<timestamp>.csv).")},},
                     {}),
             .handler="probe_capture", .requires_flag="flow",},
        Tool{.name="fpga_probe_clocks",
             .title="List ChipWatcher sample-clock candidates",
             .description="The same list the TD GUI offers in the ChipWatcher sample-clock drop-down: the clock nets TD recorded in the clock tree (phy_1/route_clock_utilization.txt or place_clock_utilization.txt), plus the clock currently in use. Read-only - no TD session, no hardware.\n"
             "Use it before fpga_probe_config{clock}: pick a name from `clocks[].name` instead of typing one. Empty list = no clock tree report yet; run stages:[\"all\"] once first.\n"
             "Example: {}",
             .input_schema=Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")}}, {}),
             .handler="probe_clocks", .requires_flag="flow",},
        Tool{.name="fpga_probe_verify",
             .title="Verify probe coverage",
             .description="Explain which requested probes made it into the bitstream and which were dropped, by comparing the mark_debug list in chip_debugger/user.mdn with phy_1/cw/compiled.cwc bit by bit. `buses` gives per-bus width/embedded_bits/missing_bits/status with a reason (no-bitstream | dropped-by-td | not-in-build | partial), `missing_nets` lists the exact net names to fix, `stale` says whether the bit still matches the mdn, and `hints` carries the one actionable next step. Read-only: no TD session, no hardware, no bitstream rewrite.\n"
             "Preconditions: a readable .al project. Works without a compiled watcher (then every requested net is reported as no-bitstream).\n"
             "Cost: milliseconds - it reads two text files; the HDL index is built lazily only when a requested net is actually missing.\n"
             "Failure modes: ok:true means the comparison ran, not that the probes are in the bit - read signals.missing/stale. A reason of not-in-build means the name was not found in the project HDL (typo or file not in the build), not that TD dropped it.\n"
             "Example: {\"include_bits\":true}",
             .input_schema=Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")},
                      {"include_bits", Bool("Return every missing net bit (default false: first 32 + truncated:true).")},},
                     {}),
             .handler="probe_verify", .requires_flag="",},
        Tool{.name="fpga_flow_wait",
             .title="Wait for flow progress",
             .description="Wait for the active flow to finish and emit MCP progress notifications when the client supplies a progressToken. Read-only; does not start a flow.",
             .input_schema=Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")},
                                          {"timeout", Int("Maximum wait in seconds (default 3600).", 1, 7200)},
                                          {"interval", Int("Poll interval in seconds (default 1).", 1, 30)},}, {}),
             .handler="flow_wait", .requires_flag="",},
        Tool{.name="fpga_flow_log",
             .title="Flow logs",
             .description="Read TD session logs and TD run.log files for the latest or a specific flow run. Returns tailed text and file paths.",
             .input_schema=Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")},
                                          {"run_id", Str("Optional flow run id (flow_xxxxxxxx).")},
                                          {"kind", Str("Log kind.", {"td", "run", "both"})},
                                          {"tail", Int("Lines to return per file (default 200).", 1, 5000)},
                                          {"max_chars", Int("Maximum characters read per file (default 200000).", 1000, 1000000)},}, {}),
             .handler="flow_log", .requires_flag="",},
        Tool{.name="fpga_flow_issues",
             .title="Flow issues",
             .description="Return structured flow errors/warnings, failed step markers and involved log paths.",
             .input_schema=Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")},
                                          {"run_id", Str("Optional flow run id (flow_xxxxxxxx).")},
                                          {"tail", Int("Lines scanned per log (default 2000).", 1, 10000)},
                                          {"limit", Int("Maximum issues returned (default 500).", 1, 2000)},}, {}),
             .handler="flow_issues", .requires_flag="",},
        Tool{.name="fpga_read_file",
             .title="Read project file",
             .description="Read a text file inside the project with line range and size limits. Read-only.",
             .input_schema=Schema({{"file", Str("Source file path (absolute or relative to the project/root).")},
                                          {"project", Str("Optional .al path, project directory, or project name. Default: active project.")},
                                          {"offset", Int("1-based first line (default 1).", 1)},
                                          {"limit", Int("Number of lines (default 400).", 1, 5000)},
                                          {"max_chars", Int("Maximum characters returned (default 200000).", 1000, 1000000)},}, {"file"}),
             .handler="read_file", .requires_flag="",},
        Tool{.name="fpga_search_text",
             .title="Search project text",
             .description="Search text/regex in project HDL, ADC, SDC and .al files. Read-only.",
             .input_schema=Schema({{"query", Str("Text or regex to find.")},
                                          {"project", Str("Optional .al path, project directory, or project name. Default: active project.")},
                                          {"path", Str("Optional file or directory inside the project.")},
                                          {"glob", Str("Optional basename glob, e.g. *.v.")},
                                          {"regex", Bool("Treat query as a regular expression.")},
                                          {"case_sensitive", Bool("Case-sensitive search (default false).")},
                                          {"limit", Int("Maximum matches (default 100).", 1, 1000)},
                                          {"context", Int("Context lines around each match (default 0).", 0, 5)},}, {"query"}),
             .handler="search_text", .requires_flag="",},
        Tool{.name="fpga_constraint_audit",
             .title="Constraint audit",
             .description="Audit ADC pin coverage/conflicts and SDC clock constraints, including device pin checks.",
             .input_schema=Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")},
                                          {"include_sdc", Bool("Include SDC clock constraint counts (default true).")},
                                          {"include_device", Bool("Load the device database for pin existence/dedicated checks.")},}, {}),
             .handler="constraint_audit", .requires_flag="",},

    };
    return kTools;
}

const Tool* FindTool(std::string_view name) {
    for (const Tool& tool : AllTools()) {
        if (tool.name == name) return &tool;
    }
    return nullptr;
}

Json ToolToJson(const Tool& tool) {
    Json out = Json::object();
    out["name"] = tool.name;
    out["title"] = tool.title;
    out["description"] = tool.description;
    out["inputSchema"] = tool.input_schema;
    // Python `_tool()`：
    //   annotations = {"readOnlyHint": requires is None,
    //                  "destructiveHint": bool(requires)}
    Json annotations = Json::object();
    annotations["readOnlyHint"] = tool.requires_flag.empty();
    annotations["destructiveHint"] = !tool.requires_flag.empty();
    out["annotations"] = std::move(annotations);
    return out;
}

const std::vector<Json>& VisibleTools(bool allow_flow, bool allow_write) {
    // P2：每进程每个 (allow_flow, allow_write) 组合只构建一次（options 在进程内不变）。
    // 返回只读引用；调用方若把数组放进响应 Json，仍会拷贝一次（HandlerResult 只装
    // Json），但省掉了每次"为 48 个工具重新 ToolToJson + 深拷贝 schema"的构造开销。
    struct Cache {
        std::mutex mutex;
        std::map<std::pair<bool, bool>, std::vector<Json>> entries;
    };
    static Cache cache;
    const std::pair<bool, bool> key{allow_flow, allow_write};
    std::scoped_lock lock(cache.mutex);
    if (const auto it = cache.entries.find(key); it != cache.entries.end()) return it->second;
    std::vector<Json> out;
    out.reserve(AllTools().size());
    for (const Tool& tool : AllTools()) {
        if (tool.requires_flag == "flow" && !allow_flow) continue;
        if (tool.requires_flag == "write" && !allow_write) continue;
        out.push_back(ToolToJson(tool));
    }
    return cache.entries.emplace(key, std::move(out)).first->second;
}

}  // namespace fpgalsp::mcp