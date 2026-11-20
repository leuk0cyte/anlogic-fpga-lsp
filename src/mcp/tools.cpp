// =============================================================================
//  src/mcp/tools.cpp —— 39 个工具的 schema 表（逐字来自 Python 参考实现）
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
// 39 个工具（顺序与 Python TOOLS 完全一致 —— 客户端可能依赖首次出现的顺序）
// -----------------------------------------------------------------------------

const std::vector<Tool>& AllTools() {
    static const std::vector<Tool> kTools = {        Tool{"fpga_project_list",
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
             "List HDL/ADC/SDC/IP files recorded in the .al with build/exclusion state.",
             Schema({{"project", Str("Optional .al path, project directory, or project name. Default: active project.")},
                                          {"kind", Str("Filter by file kind.", {"all", "hdl", "verilog", "vhdl", "adc", "sdc", "ip"})},
                                          {"include_excluded", Bool("Include files excluded from or missing in the build.")}}, {}),
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
             "Start synthesis / place&route / full flow in the background and return a runId. Requires --allow-flow or fpgaLsp.mcp.allowFlow.",
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
             "Report the ChipWatcher probe setup of the active project: the signals marked in chip_debugger/user.mdn and the compiled watcher (phy_1/cw/compiled.cwc): clock, sample depth, buses, and whether a probe-enabled bitstream exists. Read-only; no TD session, no hardware access.",
             Schema({}, {}),
             "probe_status", ""},
        Tool{"fpga_probe_config",
             "Configure on-chip probes",
             "Read or replace the probe signal list in chip_debugger/user.mdn (the file Anlogic TangDynasty insert_debugger reads). Without write:true this is a dry run that only reports added/removed signals. Writing changes what the next phy flow embeds into the bitstream; it does not touch the board.",
             Schema({{"signals", Arr("Replacement probe signals, e.g. WHG/pwm_reg0_i[0]. Omit to read the current list.",
                                      StrNoDesc())},
                      {"write", Bool("Actually rewrite chip_debugger/user.mdn (default false = dry run).")}},
                     {}),
             "probe_config", "write"},
        Tool{"fpga_probe_capture",
             "Capture on-chip probe data",
             "Read the live values of the embedded probes over JTAG and return them per sample. Runs a TD session: import_device, open_project, config_chipwatcher, get_hw_cws, connect_cable, download (mode jtag_burst), run_hw_cw -trigger_now, wait_on_hw_cw, write_hw_cw_data. Requires a board and a probe-enabled bitstream. Refuses when chip_debugger/user.mdn no longer matches the probes embedded in the bitstream. download:true reprograms SRAM and therefore needs confirm:true.",
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
                      {"trigger_enabled", Bool("Enable or disable the trigger condition.")},
                      {"bus_op", Str("Comparator applied to every watched bus.", {"dont_care", "equal", "not_equal", "less", "less_equal", "greater", "greater_equal", "bit_trigger"})},
                      {"timeout", Int("wait_on_hw_cw timeout in seconds (default 30).", 1, 600)},
                      {"csv", Str("Optional absolute CSV output path (default phy_1/cw/capture_<timestamp>.csv).")}},
                     {}),
             "probe_capture", "flow"},
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