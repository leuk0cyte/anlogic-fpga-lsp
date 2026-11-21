// =============================================================================
//  src/mcp/tools.cpp —— 48 个工具的 schema 表（逐字来自 Python 参考实现）
// =============================================================================
//  ⚠ 本文件的 schema 常量**不要手改**：任何改动都必须先改 Python 版
//    (Y:\fpga-lsp\fpgalsp\mcp.py 的 TOOLS) 并同步契约，否则
//    `tests/compare_mcp_schema.py` 会立刻变红 —— 那是故意的。
//
//  生成方式（开发期，一次性）：从 fpgalsp.mcp.TOOLS 导出后机械转写，
//  转写后由对比脚本逐字段校验（name/title/description/inputSchema/annotations）。
// =============================================================================

#include "mcp/tools.h"

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
        Tool{"fpga_guide",
             "Usage guide",
             "Return the worked-through usage guide for one topic: the tool call sequence, the key arguments, the preconditions and the failure modes to check. Read this before improvising a multi-step task. Default topic: overview.",
             Schema({{"topic", Str("Guide topic.", {"overview", "project", "index", "flow", "probe", "program", "troubleshoot"})}}),
             "guide", ""},
        Tool{"fpga_project_list",
             "List FPGA projects",
             "Discover Anlogic TangDynasty .al projects under a workspace root.",
             Schema({{"root", Str("Optional root directory. Default: MCP server root.")}}, {}),
             "project_list", ""},
        Tool{"fpga_project_info",
             "Project information",
             "Return .al metadata, file counts, warnings, runs and TD installation summary.",
             Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")}}, {}),
             "project_info", ""},
        Tool{"fpga_project_files",
             "Project build files",
             "List HDL/ADC/SDC/IP files recorded in the .al with build/exclusion state. `files` contains only the files that take part in the build; `counts` splits every (kind-filtered) file into in_build/excluded/missing and `excluded` says why each excluded or missing file is out (auto-excluded|disabled|missing).",
             Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")},
                                          {"kind", Str("Filter by file kind.", {"all", "hdl", "verilog", "vhdl", "adc", "sdc", "ip"})},
                                          {"include_excluded", Bool("Include files excluded from or missing in the build.")},
                                          {"limit", Int("Maximum entries in `excluded` (default 50).", 1, 2000)}}, {}),
             "project_files", ""},
        Tool{"fpga_project_structure",
             "Module hierarchy",
             "Return the nested module/instance hierarchy rooted at the project top module. Line/column values are 1-based.",
             Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")},
                                          {"include_ports", Bool("Attach each module's ports and parameters.")},
                                          {"include_primitives", Bool("Include device primitive instances.")},
                                          {"max_depth", Int("Maximum hierarchy depth.", 1, 64)},
                                          {"max_nodes", Int("Approximate node budget.", 1, 50000)}}, {}),
             "project_structure", ""},
        Tool{"fpga_project_reports",
             "Parsed FPGA reports",
             "Return QoR/timing/area/flow/DRC-CDC reports, optionally for the syn or phy stage. Raw report text is omitted unless include_raw is true.",
             Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")},
                                          {"stage", Str("Report stage.", {"all", "syn", "phy"})},
                                          {"section", Str("Report section.", {"summary", "qor", "timing", "area", "flow", "drc_cdc", "analysis", "strategies", "files", "all"})},
                                          {"limit", Int("Maximum rows/paths/metrics per section.", 1, 2000)},
                                          {"include_raw", Bool("Include raw report text (may be very large).")}}, {}),
             "project_reports", ""},
        Tool{"fpga_flow_status",
             "Flow status",
             "Return the current flow state (running/stage/elapsed/lastMessage) and artifacts.",
             Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")}}, {}),
             "flow_status", ""},
        Tool{"fpga_flow_run",
             "Run FPGA flow",
             // L4：前置条件 / 成本 / 失败判据 / 可照抄的参数对象。
             // 只加在 description 文本里 —— inputSchema 不动（单测要求 schema 里
             // 每个数组都声明 items，加 examples 会红）。
             "Start synthesis / place&route / full flow in the background and return a runId. Requires --allow-flow or fpgaLsp.mcp.allowFlow.\n"
             "Preconditions: --allow-flow; a detected TD install; a valid device and top module in the .al; no other flow running for this project.\n"
             "Cost: syn ~2-5 min, phy 5-15 min, all 8-20 min; returns immediately (async) - call fpga_flow_wait in the SAME MCP session.\n"
             "Failure modes: ok:true means only the requested stages finished, not that timing/DRC is clean or that probes are in the bit. A phy-only run reuses the old netlist.\n"
             "Example: {\"stages\":[\"all\"],\"jobs\":6}",
             Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")},
                                          {"stages", Arr("Stages to run.", Str("Stage.", {"syn", "phy", "all", "route"}))},
                                          {"jobs", Int("Parallel TD jobs (default 6).", 1, 64)}}, {}),
             "flow_run", "flow"},
        Tool{"fpga_flow_cancel",
             "Cancel FPGA flow",
             "Cancel the currently running flow. Requires --allow-flow.",
             Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")}}, {}),
             "flow_cancel", "flow"},
        Tool{"fpga_analysis_run",
             "Run DRC/CDC analysis",
             "Run TD DRC/CDC analysis on the placed/routed database. Requires --allow-flow.",
             Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")}}, {}),
             "analysis_run", "flow"},
        Tool{"fpga_strategy_compare",
             "Compare TD strategies",
             "Synthesize and place&route several TD strategies on a project copy and compare them. Long-running; requires --allow-flow.",
             Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")},
                                          {"strategies", Arr("Strategy names (default: all available).", StrNoDesc())},
                                          {"jobs", Int("Parallel jobs.", 1, 64)}}, {}),
             "strategy_compare", "flow"},
        Tool{"fpga_project_update",
             "Update .al in TD",
             "Run check_project_version and open_project -update to resync the .al. Requires --allow-write.",
             Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")}}, {}),
             "project_update", "write"},
        Tool{"fpga_project_add_source",
             "Add source to .al",
             "Append a Verilog/VHDL file to the .al Source_Files section. Requires --allow-write.",
             Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")},
                                          {"file", Str("Source file path (absolute or relative to the project/root).")},
                                          {"update", Bool("Run open_project -update after editing the .al.")}}, {"file"}),
             "project_add_source", "write"},
        Tool{"fpga_project_rename",
             "Rename .al project",
             "Change the project <Name> in the .al. Requires --allow-write.",
             Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")},
                                          {"name", Str("New project name.")}}, {"name"}),
             "project_rename", "write"},
        Tool{"fpga_document_symbols",
             "Document symbols",
             "Return the module/port/signal/parameter/instance symbol tree for a source file.",
             Schema({{"file", Str("Source file path (absolute or relative to the project/root).")}}, {"file"}),
             "document_symbols", ""},
        Tool{"fpga_workspace_symbols",
             "Workspace symbol search",
             "Search project symbols by name.",
             Schema({{"query", Str("Search query.")},
                                          {"limit", Int("Maximum number of returned items.", 1, 2000)},
                                          {"offset", Int("Pagination offset (use nextOffset from a truncated result).", 0, 1000000)}}, {"query"}),
             "workspace_symbols", ""},
        Tool{"fpga_search_hierarchy",
             "Search hierarchy",
             "Search module definitions and instance names in the project hierarchy.",
             Schema({{"query", Str("Module or instance name substring.")},
                                          {"kind", Str("Match kind.", {"both", "module", "instance"})},
                                          {"limit", Int("Maximum number of returned items.", 1, 2000)}}, {"query"}),
             "search_hierarchy", ""},
        Tool{"fpga_definition",
             "Go to definition",
             "Return definition locations for the symbol at a 1-based position.",
             Schema({{"file", Str("Source file path (absolute or relative to the project/root).")},
                                          {"line", Int("1-based line number.", 1)},
                                          {"column", Int("1-based column number.", 1)}}, {"file", "line", "column"}),
             "definition", ""},
        Tool{"fpga_references",
             "Find references",
             "Return all references for the symbol at a 1-based position.",
             Schema({{"file", Str("Source file path (absolute or relative to the project/root).")},
                                          {"line", Int("1-based line number.", 1)},
                                          {"column", Int("1-based column number.", 1)},
                                          {"include_declaration", Bool("Include the declaration itself (default true).")}}, {"file", "line", "column"}),
             "references", ""},
        Tool{"fpga_hover",
             "Hover information",
             "Return hover markdown for the symbol at a 1-based position.",
             Schema({{"file", Str("Source file path (absolute or relative to the project/root).")},
                                          {"line", Int("1-based line number.", 1)},
                                          {"column", Int("1-based column number.", 1)}}, {"file", "line", "column"}),
             "hover", ""},
        Tool{"fpga_symbol_path",
             "Hierarchical symbol path",
             "Return the instance path of a wire/reg signal, e.g. U_OC_CHK/U_DATA_ABS/data_o_r.",
             Schema({{"file", Str("Source file path (absolute or relative to the project/root).")},
                                          {"line", Int("1-based line number.", 1)},
                                          {"column", Int("1-based column number.", 1)}}, {"file", "line", "column"}),
             "symbol_path", ""},
        Tool{"fpga_call_hierarchy",
             "Call hierarchy",
             "Return caller/callee modules for a module, or for the module/instance at a position.",
             Schema({{"file", Str("Source file path (absolute or relative to the project/root).")},
                                          {"line", Int("1-based line number.", 1)},
                                          {"column", Int("1-based column number.", 1)},
                                          {"module", Str("Module name (alternative to file/line/column).")},
                                          {"direction", Str("Direction.", {"both", "incoming", "outgoing"})},
                                          {"depth", Int("Reserved depth hint.", 1, 16)}}, {}),
             "call_hierarchy", ""},
        Tool{"fpga_completion",
             "Code completion",
             "Return LSP completion items at a 1-based position (including instantiation snippets).",
             Schema({{"file", Str("Source file path (absolute or relative to the project/root).")},
                                          {"line", Int("1-based line number.", 1)},
                                          {"column", Int("1-based column number.", 1)},
                                          {"prefix", Str("Optional label prefix filter.")},
                                          {"limit", Int("Maximum items.", 1, 500)},
                                          {"include_snippets", Bool("Include snippet completion items (default true).")}}, {"file", "line", "column"}),
             "completion", ""},
        Tool{"fpga_code_actions",
             "Code actions",
             "List LSP code actions for a 1-based range (read-only unless include_edits is requested).",
             Schema({{"file", Str("Source file path (absolute or relative to the project/root).")},
                                          {"start_line", Int("1-based line number.", 1)},
                                          {"start_column", Int("1-based column number.", 1)},
                                          {"end_line", Int("1-based end line.", 1)},
                                          {"end_column", Int("1-based end column.", 1)},
                                          {"include_edits", Bool("Include edit payloads.")}}, {"file", "start_line", "start_column"}),
             "code_actions", ""},
        Tool{"fpga_rename_edits",
             "Rename edits",
             "Return rename workspace edits for a symbol (does not write files).",
             Schema({{"file", Str("Source file path (absolute or relative to the project/root).")},
                                          {"line", Int("1-based line number.", 1)},
                                          {"column", Int("1-based column number.", 1)},
                                          {"new_name", Str("New identifier.")}}, {"file", "line", "column", "new_name"}),
             "rename_edits", ""},
        Tool{"fpga_format",
             "Format source",
             "Format Verilog/SystemVerilog text or a file and return the result (does not write files).",
             Schema({{"file", Str("Source file path (absolute or relative to the project/root).")},
                                          {"text", Str("Source text (used when file is omitted).")},
                                          {"start_line", Int("Optional 1-based range start.", 1)},
                                          {"end_line", Int("Optional 1-based range end.", 1)},
                                          {"tab_size", Int("Indent width (default 4).", 1, 16)},
                                          {"insert_spaces", Bool("Use spaces (default true).")}}, {}),
             "format", ""},
        Tool{"fpga_instantiation",
             "Generate instantiation",
             "Return a named-port instantiation template for a project module.",
             Schema({{"module", Str("Module name.")},
                                          {"instance_name", Str("Optional instance name (default u_<module>).")},
                                          {"indent", Str("Indent unit (default four spaces).")}}, {"module"}),
             "instantiation", ""},
        Tool{"fpga_module_info",
             "Module information",
             "Return module ports, parameters, definition and instantiation sites.",
             Schema({{"module", Str("Module name.")},
                                          {"project", Str("Optional .al path, project directory, or project name. Default: active project.")}}, {"module"}),
             "module_info", ""},
        Tool{"fpga_duplicate_modules",
             "Duplicate modules",
             "Find modules defined in more than one build file.",
             Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")}}, {}),
             "duplicate_modules", ""},
        Tool{"fpga_diagnostics",
             "Diagnostics",
             "Return plugin diagnostics (HDL/ADC/SDC by default; optional Verible and TD tiers) for a file or the whole project.",
             Schema({{"file", Str("Source file path (absolute or relative to the project/root).")},
                                          {"project", Str("Optional .al path, project directory, or project name. Default: active project.")},
                                          {"sources", Arr("Diagnostic sources.", Str("Source.", {"hdl", "adc", "sdc", "verible", "td"}))},
                                          {"limit", Int("Maximum diagnostics.", 1, 2000)}}, {}),
             "diagnostics", ""},
        Tool{"fpga_device_info",
             "Device information",
             "Return the project device/package and optionally filtered pin capabilities. May start a TD session to load the device database.",
             Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")},
                                          {"include_pins", Bool("Include pin rows.")},
                                          {"pin_filter", Str("Optional pin/location substring filter.")},
                                          {"limit", Int("Maximum pins.", 1, 5000)},
                                          {"refresh", Bool("Reload the device database.")}}, {}),
             "device_info", ""},
        Tool{"fpga_constraints_status",
             "Constraint coverage",
             "Return ADC port coverage and SDC clock-constraint summary.",
             Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")}}, {}),
             "constraints_status", ""},
        Tool{"fpga_td_info",
             "TD installation info",
             "Return detected TangDynasty installations and the selected version.",
             Schema({}, {}),
             "td_info", ""},
        Tool{"fpga_testbench",
             "Generate a testbench skeleton",
             "Generate a compilable testbench skeleton for a module (default: the top module), with port declarations, clock/reset guess and a parse self-check.",
             Schema({{"module", Str("Module name (default: project top).")},
                     {"clock", Str("Clock port name (default: guessed from clk/clock).")},
                     {"reset", Str("Reset port name (default: guessed from rst/reset).")},
                     {"project", Str("Optional .al path, project directory, or project name.")}},
                    {}),
             "testbench", ""},
        Tool{"fpga_run_diff",
             "Compare two flow runs",
             "Compare QoR/timing/area metrics between two runs under <project>_Runs (default: the last two).",
             Schema({{"from", Str("Run name to compare from (default: second newest).")},
                     {"to", Str("Run name to compare to (default: newest).")},
                     {"project", Str("Optional .al path, project directory, or project name.")}},
                    {}),
             "run_diff", ""},
        Tool{"fpga_job_status",
             "Job status",
             "Status of a long-running flow job (job = the runId returned by fpga_flow_run).",
             Schema({{"job", Str("Job id returned by fpga_flow_run.")},
                     {"project", Str("Optional .al path, project directory, or project name.")}},
                    {}),
             "job_status", ""},
        Tool{"fpga_job_cancel",
             "Cancel job",
             "Cancel a running flow job by id (no-op when it already finished).",
             Schema({{"job", Str("Job id returned by fpga_flow_run.")},
                     {"project", Str("Optional .al path, project directory, or project name.")}},
                    {}),
             "job_cancel", "flow"},
        Tool{"fpga_cables",
             "Cable / online device",
             "Read-only list_cable/online-device query (never programs the FPGA).",
             Schema({}, {}),
             "cables", ""},
        // ---- 0.16.0：片上探针（Anlogic ChipWatcher）----
        //   status 纯读文件；config 改 chip_debugger/user.mdn；capture 碰真实硬件。
        Tool{"fpga_probe_status",
             "On-chip probe status",
             "Report the ChipWatcher probe setup of the active project: the signals marked in chip_debugger/user.mdn and the compiled watcher (phy_1/cw/compiled.cwc): clock, sample depth, buses, and whether a probe-enabled bitstream exists. The `sampling` object answers whether the sample clock/depth can be changed at capture time: `editable` is always false because they are compile-time (TD only writes them from the GUI ChipWatcher instance before the Gate stage). Disabled entries are reported separately: `disabled` (requested, from .fpgalsp/probe/config.txt), `disabled_count`, and watcher.disabled_count/disabled_bus_count/disabled_net_count vs enabled_net_count (what the compiled cwc actually marks as disabled; those entries consume no debug resource). Read-only; no TD session, no hardware access.",
             Schema({}, {}),
             "probe_status", ""},
        Tool{"fpga_probe_config",
             "Configure on-chip probes",
             "Read or replace the probe signal list in chip_debugger/user.mdn (the file Anlogic TangDynasty insert_debugger reads). Without write:true this is a dry run that only reports added/removed signals. Writing changes what the next phy flow embeds into the bitstream; it does not touch the board.\n"
             "Preconditions: a readable project; write:true needs --allow-write and a non-empty signals list.\n"
             "Cost: milliseconds. It only edits chip_debugger/user.mdn - no TD session, no hardware, no bitstream.\n"
             "Failure modes: ok:true with written:true does NOT mean the probes are in the bitstream; rerun phy and confirm with fpga_probe_status (watcher.stale / watcher.missing_in_bit). An empty list is refused.\n"
             "Disabled entries: `disabled` disables a watched bus or a single bit WITHOUT removing it from the list. TD's ChipWatcher list can enable/disable entries and a disabled entry consumes no debug resource (the compiled cwc carries bus is_enabled=\"no\" and net data=\"disable\" trig=\"disable\"). chip_debugger/user.mdn has no disable syntax, so the backend expresses it only through the authored cwc it hands to insert_debugger -cwc: set apply_build_config:true and rerun stages:[\"all\"], otherwise the entries stay enabled in the bitstream. The response has disabled/disabled_count/unresolved_disabled plus enabled_bus_count/enabled_bit_count (disabled bits excluded) and requires_apply_build_config.\n"
             "Example: {\"signals\":[\"WHG/e2e_cnt2l[15:0]\",\"WHG/e2e_hstat[4:0]\"],\"disabled\":[\"WHG/e2e_hstat\"],\"write\":true,\"apply_build_config\":true}",
             Schema({{"clock", Str("Sample clock net for the ChipWatcher, e.g. reg_init/clk. TD treats it as a compile-time value: it is only honoured at the Optimize Gate step when the backend hands TD an authored cwc, so it takes effect on the next stages:[\"all\"] run, not at capture time.")},
                      {"cwc_bus_order", Arr("Bus order (one entry per watched bus, e.g. [\"WHG/e2e_cnt2l\",\"WHG/e2e_hstat\",...]) used when this backend regenerates phy_1/cw/compiled.cwc after a phy run. TD reorders buses and its watcher artifacts carry only widths, so a mixed-width list (16/5/16/16) cannot be mapped automatically; give the order here once and every later flow writes a correct cwc. Persisted in .fpgalsp/probe/config.txt.", StrNoDesc())},
                      {"write_sidecar_cwc", Bool("Write a generated phy_1/cw/compiled.cwc after the phy run (default false). The generated file is not yet equivalent to the one TD itself writes - TD reports a different Chipwatcher code and capture then hangs - so only enable it to experiment.")},
                      {"apply_build_config", Bool("Actually hand TD an authored cwc at the Optimize Gate step so `clock`/`depth` take effect (default false). Measured: TD honours the depth, but a phy place after that crashed twice with 'Run phy_1 has exited unexpectedly' - the authored cwc still disagrees with the bitstream resources. Enable only when you are prepared to experiment.")},
                      {"depth", Int("Sample depth for the ChipWatcher (e.g. 2048). Same rule as clock: compile-time in TD, takes effect on the next stages:[\"all\"] run. The backend writes it into the cwc it feeds to insert_debugger - measured: depth 2048 is honoured by TD 2026.2.", 16, 65536)},
                      {"signals", Arr("Replacement probe signals, e.g. WHG/pwm_reg0_i[0]. Omit to read the current list.",
                                      StrNoDesc())},
                      {"disabled", Arr("Entries to disable without removing them from the watch list. A whole bus (\"WHG/e2e_hstat\", all of its bits) or a single bit (\"WHG/e2e_hstat[2]\"); ranges (\"WHG/x[4:0]\") are expanded. They stay in chip_debugger/user.mdn and in the generated cwc (bus is_enabled=\"no\", net data=\"disable\" trig=\"disable\", signal_vec cond=\"dont_care\") but consume no ChipWatcher debug resource, so they are excluded from enabled_bus_count/enabled_bit_count and from the cwc DIN/bit order. Persisted as disabled=... in .fpgalsp/probe/config.txt (write:true). Omitting it keeps the persisted list; send [] to clear. Entries not present in signals come back in unresolved_disabled plus a warning (not a failure).",
                                       StrNoDesc())},
                      {"write", Bool("Actually rewrite chip_debugger/user.mdn (default false = dry run).")}},
                     {}),
             "probe_config", "write"},
        Tool{"fpga_probe_capture",
             "Capture on-chip probe data",
             "Read the live values of the embedded probes over JTAG and return them per sample. Runs a TD session: import_device, open_project, config_chipwatcher, get_hw_cws, connect_cable, download (mode jtag_burst), run_hw_cw -trigger_now, wait_on_hw_cw, write_hw_cw_data. Requires a board and a probe-enabled bitstream. Refuses when chip_debugger/user.mdn no longer matches the probes embedded in the bitstream. download:true reprograms SRAM and therefore needs confirm:true.\n"
             "Trigger values: global trigger_type/bus_op/window_* plus per-bus `buses:[{name,op,value}]` (value is a binary bit string, the same format as the capture CSV, e.g. \"0000000000000000\" = trigger when the counter wraps to 0). A per-bus condition only takes effect when bus_op (the global comparator) is also given AND trigger_enabled:true; in TD a bus op=dont_care disables the per-net conditions.\n"
             "Preconditions: phy_1/cw/compiled.cwc and compiled.bit from a phy run; a board on a cable; no other hardware operation. download auto/true needs confirm:true.\n"
             "Cost: ~20-60 s (17.4 s measured with the bit already loaded; a download adds the SRAM load); phy 5-15 min if a rebuild is needed first.\n"
             "Failure modes: ok:true means samples were read, not that they are the signals you marked - stale/missing_in_bit means the bit is older than user.mdn. Sample clock and depth are compile-time; only trigger_*/window_* are runtime.\n"
             "Example: {\"download\":\"auto\",\"confirm\":true,\"trigger_now\":false,\"bus_op\":\"equal\",\"trigger_enabled\":true,\"buses\":[{\"name\":\"WHG/e2e_cnt2l\",\"op\":\"equal\",\"value\":\"0000000000000000\"}]}",
             Schema({{"project", Str("Optional .al path, project directory, or project name.")},
                      {"instance", Str("Watcher instance name (default: read from compiled.cwc).")},
                      {"cable", Str("Cable index for -cable (default 0).")},
                      {"download", Str("Whether to load phy_1/cw/compiled.bit first. auto: capture on the bitstream already in the device and only download when that fails (default). true: always reload. false: never reload (pure read, no confirm needed).",
                                      {"auto", "true", "false"})},
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
                      {"generate_cwc", Bool("Allow this backend to generate phy_1/cw/compiled.cwc from the TD watcher artifacts when a batch phy run did not write it (default false). The generated ERAM section is not verified against the bitstream - prefer a cwc produced by the TD GUI and only use this when you accept the risk.")},
                      {"cwc_bus_order", Arr("Bus order to use when this backend has to rebuild phy_1/cw/compiled.cwc from TD's own artifacts (batch phy runs do not write it). TD reorders buses and its watcher artifacts carry only widths, so a mixed-width list (e.g. 16/5/16/16) cannot be mapped automatically - pass the real order here, names as in user.mdn (`WHG/e2e_hstat` or `e2e_hstat`).", StrNoDesc())},
                      {"buses", Arr("Per-bus trigger values. value is a binary bit string (left = msb), e.g. \"0000000000000000\" for 'counter wrapped to 0'. Empty array = no per-bus condition. Requires bus_op and trigger_enabled:true to take effect in TD.",
                                    Schema({{"name", Str("Watched bus name as it appears in the watcher, e.g. WHG/e2e_cnt2l or WHG/pwm_reg0_i[15:0].")},
                                            {"op", Str("Per-bus comparator (same vocabulary as bus_op, without dont_care).", {"equal", "not_equal", "less", "less_equal", "greater", "greater_equal", "bit_trigger"})},
                                            {"value", Str("Expected value as a binary bit string, left = msb, same format as the capture CSV.")}},
                                           {"name"}))},
                      {"timeout", Int("wait_on_hw_cw timeout in seconds (default 30).", 1, 600)},
                      {"csv", Str("Optional absolute CSV output path (default phy_1/cw/capture_<timestamp>.csv).")}},
                     {}),
             "probe_capture", "flow"},
        Tool{"fpga_probe_verify",
             "Verify probe coverage",
             "Explain which requested probes made it into the bitstream and which were dropped, by comparing the mark_debug list in chip_debugger/user.mdn with phy_1/cw/compiled.cwc bit by bit. `buses` gives per-bus width/embedded_bits/missing_bits/status with a reason (no-bitstream | dropped-by-td | not-in-build | partial), `missing_nets` lists the exact net names to fix, `stale` says whether the bit still matches the mdn, and `hints` carries the one actionable next step. Read-only: no TD session, no hardware, no bitstream rewrite.\n"
             "Preconditions: a readable .al project. Works without a compiled watcher (then every requested net is reported as no-bitstream).\n"
             "Cost: milliseconds - it reads two text files; the HDL index is built lazily only when a requested net is actually missing.\n"
             "Failure modes: ok:true means the comparison ran, not that the probes are in the bit - read signals.missing/stale. A reason of not-in-build means the name was not found in the project HDL (typo or file not in the build), not that TD dropped it.\n"
             "Example: {\"include_bits\":true}",
             Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")},
                      {"include_bits", Bool("Return every missing net bit (default false: first 32 + truncated:true).")}},
                     {}),
             "probe_verify", ""},
        Tool{"fpga_flow_wait",
             "Wait for flow progress",
             "Wait for the active flow to finish and emit MCP progress notifications when the client supplies a progressToken. Read-only; does not start a flow.",
             Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")},
                                          {"timeout", Int("Maximum wait in seconds (default 3600).", 1, 7200)},
                                          {"interval", Int("Poll interval in seconds (default 1).", 1, 30)}}, {}),
             "flow_wait", ""},
        Tool{"fpga_flow_log",
             "Flow logs",
             "Read TD session logs and TD run.log files for the latest or a specific flow run. Returns tailed text and file paths.",
             Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")},
                                          {"run_id", Str("Optional flow run id (flow_xxxxxxxx).")},
                                          {"kind", Str("Log kind.", {"td", "run", "both"})},
                                          {"tail", Int("Lines to return per file (default 200).", 1, 5000)},
                                          {"max_chars", Int("Maximum characters read per file (default 200000).", 1000, 1000000)}}, {}),
             "flow_log", ""},
        Tool{"fpga_flow_issues",
             "Flow issues",
             "Return structured flow errors/warnings, failed step markers and involved log paths.",
             Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")},
                                          {"run_id", Str("Optional flow run id (flow_xxxxxxxx).")},
                                          {"tail", Int("Lines scanned per log (default 2000).", 1, 10000)},
                                          {"limit", Int("Maximum issues returned (default 500).", 1, 2000)}}, {}),
             "flow_issues", ""},
        Tool{"fpga_read_file",
             "Read project file",
             "Read a text file inside the project with line range and size limits. Read-only.",
             Schema({{"file", Str("Source file path (absolute or relative to the project/root).")},
                                          {"project", Str("Optional .al path, project directory, or project name. Default: active project.")},
                                          {"offset", Int("1-based first line (default 1).", 1)},
                                          {"limit", Int("Number of lines (default 400).", 1, 5000)},
                                          {"max_chars", Int("Maximum characters returned (default 200000).", 1000, 1000000)}}, {"file"}),
             "read_file", ""},
        Tool{"fpga_search_text",
             "Search project text",
             "Search text/regex in project HDL, ADC, SDC and .al files. Read-only.",
             Schema({{"query", Str("Text or regex to find.")},
                                          {"project", Str("Optional .al path, project directory, or project name. Default: active project.")},
                                          {"path", Str("Optional file or directory inside the project.")},
                                          {"glob", Str("Optional basename glob, e.g. *.v.")},
                                          {"regex", Bool("Treat query as a regular expression.")},
                                          {"case_sensitive", Bool("Case-sensitive search (default false).")},
                                          {"limit", Int("Maximum matches (default 100).", 1, 1000)},
                                          {"context", Int("Context lines around each match (default 0).", 0, 5)}}, {"query"}),
             "search_text", ""},
        Tool{"fpga_constraint_audit",
             "Constraint audit",
             "Audit ADC pin coverage/conflicts and SDC clock constraints, including device pin checks.",
             Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")},
                                          {"include_sdc", Bool("Include SDC clock constraint counts (default true).")},
                                          {"include_device", Bool("Load the device database for pin existence/dedicated checks.")}}, {}),
             "constraint_audit", ""},

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

std::vector<Json> VisibleTools(bool allow_flow, bool allow_write) {
    std::vector<Json> out;
    out.reserve(AllTools().size());
    for (const Tool& tool : AllTools()) {
        if (tool.requires_flag == "flow" && !allow_flow) continue;
        if (tool.requires_flag == "write" && !allow_write) continue;
        out.push_back(ToolToJson(tool));
    }
    return out;
}

}  // namespace fpgalsp::mcp