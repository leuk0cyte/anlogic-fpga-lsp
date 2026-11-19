// =============================================================================
//  probe_mcp_bridge.cpp —— MCP 工具与传输层性能（perf-mcp / task-31）
// =============================================================================
//  进程内（in-process）探针：直接驱动 `fpgalsp::mcp::Bridge`，避免把 39 个工具的
//  延迟和 python 客户端/管道开销混在一起；协议层（序列化/帧）单独拆开测。
//
//  节：
//    tools     逐工具 p50/p95 + 响应字节 + µs/KB（默认全部只读工具）
//    struct    project_structure 的"两遍物化"（树构建 vs MCP 转换）归因
//    dispatch  Call() 39 分支字符串链 vs handler 真实成本
//    json      dump(-1) / dump(1) / AddPythonStyleSpaces 整份复制 / 请求帧解析
//    io        search_text 逐行物化、read_file 每次读盘、GetObject 按值拷贝
//
//  用法：
//    perf_mcp_bridge.exe --iters 30            # 默认跑 tools 节
//    perf_mcp_bridge.exe --iters 50 struct,dispatch,json,io
//    perf_mcp_bridge.exe --synthetic-only      # 只造合成工程，不测（调试用）
//
//  合成工程：%TEMP%\fpgalsp-perf\mcp\proj200（200 个 .v + top 例化 50 个模块）。
//  真实工程一律不碰（README 纪律 §4）。
// =============================================================================

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "jsonrpc/json_util.h"
#include "mcp/bridge.h"
#include "mcp/tools.h"
#include "perf_common.h"
#include "platform/encoding.h"
#include "platform/path.h"
#include "project/al_project.h"
#include "td/td_install.h"

// windows.h 把 GetObject 宏成 GetObjectA；这里要调 fpgalsp::jsonrpc::GetObject（按值拷贝那个）。
#ifdef GetObject
#undef GetObject
#endif

namespace {

namespace fs = std::filesystem;
using fpgalsp::jsonrpc::Json;
using fpgalsp::mcp::Bridge;
using fpgalsp::mcp::BridgeOptions;
using fpgalsp::mcp::Tool;

constexpr int kSyntheticFiles = 200;
constexpr int kTopInstances = 50;

std::string Slashes(std::string text) {
    for (char& c : text) {
        if (c == '\\') c = '/';
    }
    return text;
}

std::string TempRoot() {
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec) / "fpgalsp-perf" / "mcp";
    fs::create_directories(base, ec);
    return Slashes(base.string());
}

// ---------------------------------------------------------------------------
// 合成工程（200 文件；top 例化前 50 个模块，让 project_structure 有真实的树）
// ---------------------------------------------------------------------------
std::string EnsureSyntheticProject() {
    const std::string root = TempRoot() + "/proj200";
    const std::string src = root + "/src";
    std::error_code ec;
    const fs::path marker = fs::path(root + "/perf200.al");
    if (fs::exists(marker, ec)) {
        // 已存在：验证文件数够，够就直接复用（避免每次跑都重写 200 个文件）
        int count = 0;
        for (const auto& entry : fs::directory_iterator(fs::path(src), ec)) {
            if (entry.path().extension() == ".v") ++count;
        }
        if (count >= kSyntheticFiles) {
            std::printf("synthetic: reuse %s (%d files)\n", root.c_str(), count);
            return root;
        }
    }
    fs::remove_all(fs::path(root), ec);
    fs::create_directories(fs::path(src), ec);

    std::string al;
    al += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    al += "<!-- perf synthetic project (generated) -->\n";
    al += "<Project Version=\"3\" Minor=\"2\" Path=\"" + root + "\">\n";
    al += "    <Project_Created_Time></Project_Created_Time>\n";
    al += "    <TD_Version>6.2.3.210850</TD_Version>\n";
    al += "    <Name>perf200</Name>\n";
    al += "    <HardWare>\n        <Family>EF2</Family>\n";
    al += "        <Device>EF2L45LG144B</Device>\n        <Speed></Speed>\n    </HardWare>\n";
    al += "    <TOP_MODULE><MODULE>top</MODULE></TOP_MODULE>\n";
    al += "    <Source_Files>\n        <Verilog>\n";

    int order = 1;
    auto add_file = [&al, &order](const std::string& rel) {
        al += "            <File Path=\"" + rel + "\">\n                <FileInfo>\n";
        al += "                    <Attr Name=\"UsedInSyn\" Val=\"true\"/>\n";
        al += "                    <Attr Name=\"UsedInP&R\" Val=\"true\"/>\n";
        al += "                    <Attr Name=\"BelongTo\" Val=\"design_1\"/>\n";
        al += "                    <Attr Name=\"CompileOrder\" Val=\"" + std::to_string(order++) +
              "\"/>\n";
        al += "                </FileInfo>\n            </File>\n";
    };

    // 200 个叶子模块
    for (int index = 1; index <= kSyntheticFiles; ++index) {
        char name[16];
        std::snprintf(name, sizeof(name), "m%04d", index);
        std::string text;
        text += "// synthetic leaf module " + std::string(name) + "\n";
        text += "module " + std::string(name) + " (\n";
        text += "    input  wire        clk,\n";
        text += "    input  wire        rst_n,\n";
        text += "    input  wire [15:0] din,\n";
        text += "    output reg  [15:0] dout\n";
        text += ");\n";
        text += "  reg [15:0] acc;\n";
        text += "  always @(posedge clk or negedge rst_n) begin\n";
        text += "    if (!rst_n) begin\n      acc  <= 16'd0;\n      dout <= 16'd0;\n    end else begin\n";
        text += "      acc  <= acc + din;\n      dout <= acc ^ din;\n    end\n  end\n";
        text += "endmodule\n";
        const std::string rel = "src/" + std::string(name) + ".v";
        (void)fpgalsp::platform::write_file_utf8(root + "/" + rel, text);
        add_file(rel);
    }

    // top：例化前 kTopInstances 个模块（层次树 2 层）
    {
        std::string text;
        text += "module top (\n    input  wire        clk,\n    input  wire        rst_n,\n";
        text += "    input  wire [15:0] din,\n    output wire [15:0] dout\n);\n";
        for (int index = 1; index <= kTopInstances; ++index) {
            char name[16];
            std::snprintf(name, sizeof(name), "m%04d", index);
            text += "  " + std::string(name) + " u_" + std::string(name) + " (\n";
            text += "      .clk(clk), .rst_n(rst_n), .din(din), .dout()\n  );\n";
        }
        text += "  assign dout = din;\nendmodule\n";
        (void)fpgalsp::platform::write_file_utf8(root + "/src/top.v", text);
        add_file("src/top.v");
    }

    al += "        </Verilog>\n    </Source_Files>\n</Project>\n";
    (void)fpgalsp::platform::write_file_utf8(root + "/perf200.al", al);

    // 预热 page cache（README §6.4）：把每个文件读一遍
    std::size_t total = 0;
    for (const auto& entry : fs::directory_iterator(fs::path(src), ec)) {
        if (entry.path().extension() != ".v") continue;
        total += perf::ReadFile(entry.path()).size();
    }
    std::printf("synthetic: created %s (%d files, %.2f MB, warmed)\n", root.c_str(),
                kSyntheticFiles + 1, static_cast<double>(total) / (1024.0 * 1024.0));
    return root;
}

std::string ReadText(const std::string& path) {
    const auto text = fpgalsp::platform::try_read_file_utf8(path);
    return text.value_or(std::string());
}

struct Position {
    int line = 1;    // 1-based
    int column = 1;  // 1-based
};

Position ModulePosition(const std::string& path, std::string_view module) {
    const std::string text = ReadText(path);
    const std::string needle = "module " + std::string(module);
    const std::size_t at = text.find(needle);
    if (at == std::string::npos) return Position{};
    Position pos;
    pos.line = 1 + static_cast<int>(std::count(text.begin(), text.begin() + at, '\n'));
    const std::size_t line_start = text.rfind('\n', at);
    const std::size_t column0 = at - (line_start == std::string::npos ? 0 : line_start + 1);
    pos.column = static_cast<int>(column0) + 1;
    return pos;
}

// 按 schema 的 required 生成一组"固定且可复现"的参数（README §6.4 的思路）。
Json BuildArguments(const Tool& tool, const std::string& root, const std::string& file,
                    const Position& pos) {
    Json args = Json::object();
    const Json& schema = tool.input_schema;
    const Json required = schema.contains("required") ? schema["required"] : Json::array();
    for (const Json& item : required) {
        if (!item.is_string()) continue;
        const std::string key = item.get<std::string>();
        if (key == "file") {
            args["file"] = file;
        } else if (key == "line") {
            args["line"] = pos.line;
        } else if (key == "column") {
            args["column"] = pos.column;
        } else if (key == "start_line") {
            args["start_line"] = pos.line;
        } else if (key == "start_column") {
            args["start_column"] = pos.column;
        } else if (key == "end_line") {
            args["end_line"] = pos.line + 1;
        } else if (key == "end_column") {
            args["end_column"] = 1;
        } else if (key == "query") {
            args["query"] = "m00";
        } else if (key == "module") {
            args["module"] = "m0001";
        } else if (key == "new_name") {
            args["new_name"] = "m0001_renamed";
        } else if (key == "name") {
            args["name"] = "perf200_renamed";
        } else if (key == "root") {
            args["root"] = root;
        } else {
            return Json();  // 不认识的 required -> 交给调用方跳过
        }
    }
    return args;
}

// ---------------------------------------------------------------------------
// tools 节
// ---------------------------------------------------------------------------
struct ToolRow {
    std::string tool;
    std::string handler;
    size_t bytes = 0;
    double p50 = 0.0;
    double p95 = 0.0;
    double max = 0.0;
    bool ok = false;
    std::string note;
};

std::vector<ToolRow> RunToolLatency(Bridge& bridge, const std::string& root, int iters,
                                    int warmup) {
    std::vector<ToolRow> rows;
    const std::string file = root + "/src/top.v";
    const Position pos = ModulePosition(file, "top");

    std::printf("%-34s %8s %8s %8s %9s %8s %s\n", "tool", "p50/ms", "p95/ms", "max/ms",
                "bytes", "us/KB", "note");
    for (const Tool& tool : fpgalsp::mcp::AllTools()) {
        if (!tool.requires_flag.empty()) continue;  // 只读工具（写/flow 类另测）
        ToolRow row;
        row.tool = tool.name;
        row.handler = tool.handler;
        if (tool.name == "fpga_device_info") {
            row.note = "needs TD session (excluded: would spawn td_commands_prompt)";
            rows.push_back(row);
            continue;
        }
        Json args = BuildArguments(tool, root, file, pos);
        if (tool.name == "fpga_diagnostics") args = Json{{"sources", Json::array({"adc", "sdc"})}};
        if (tool.name == "fpga_diagnostics_placeholder") args = Json::object();
        if (args.is_null()) {
            row.note = "no fixed arguments for required keys";
            rows.push_back(row);
            continue;
        }
        if (tool.name == "fpga_format" && args.empty()) {
            args["text"] = "module m(input a);endmodule\n";
        }
        if (tool.name == "fpga_instantiation" && !args.contains("module")) {
            args["module"] = "m0001";
        }
        // call_hierarchy 没有 required 键（file/line/column 与 module 二选一），显式给 module
        if (tool.name == "fpga_call_hierarchy" && args.empty()) {
            args["module"] = "m0001";
        }

        Json last = Json();
        bool threw = false;
        std::string error;
        try {
            // 预热一次，确保懒初始化的索引/缓存都已建好（否则 p50 混入冷启动）
            last = bridge.Call(tool.handler, args);
            const perf::Stats stats =
                perf::Bench(tool.name, iters,
                            [&bridge, &tool, &args, &last] {
                                last = bridge.Call(tool.handler, args);
                            },
                            warmup);
            row.ok = true;
            row.p50 = stats.P50();
            row.p95 = stats.P95();
            row.max = stats.Max();
            row.bytes = last.is_discarded() ? 0 : last.dump().size();
        } catch (const std::exception& exc) {
            threw = true;
            error = exc.what();
            row.note = std::string("error: ") + (error.size() > 60 ? error.substr(0, 60) : error);
        }
        if (!threw && row.ok) {
            const double us_per_kb =
                row.bytes == 0 ? 0.0 : (row.p50 * 1000.0) / (static_cast<double>(row.bytes) / 1024.0);
            std::printf("%-34s %8.3f %8.3f %8.3f %9zu %8.2f\n", row.tool.c_str(), row.p50,
                        row.p95, row.max, row.bytes, us_per_kb);
            std::printf("csv,%s,%zu,%.4f,%.4f,%.4f,%zu,%.2f\n", row.tool.c_str(),
                        static_cast<size_t>(iters), row.p50, row.p95, row.max, row.bytes,
                        us_per_kb);
        } else {
            std::printf("%-34s %8s %8s %8s %9s %8s %s\n", row.tool.c_str(), "-", "-", "-", "-",
                        "-", row.note.c_str());
        }
        rows.push_back(row);
    }
    return rows;
}

// ---------------------------------------------------------------------------
// struct 节：project_structure 的两遍物化归因
// ---------------------------------------------------------------------------
//  第一遍 = features::ModuleTree（LSP 形状树，Bridge 内部调用）
//  第二遍 = bridge.cpp 的 convert()（把树搬到 MCP 形状）
//  我们无法从外部调 features::ModuleTree（Bridge 的 index 是私有的），但可以：
//    a) 测 Call("project_structure") 总时间；
//    b) 对返回的 root 子树复刻一遍 convert 形状的遍历（每节点同样的 value()/拷贝），
//       得到"第二遍"的量级。
std::size_t CountNodes(const Json& node) {
    if (!node.is_object()) return 0;
    std::size_t count = 1;
    if (node.contains("children") && node["children"].is_array()) {
        for (const Json& child : node["children"]) count += CountNodes(child);
    }
    return count;
}

Json ConvertLikeBridge(const Json& node) {
    // 复刻 src/mcp/bridge.cpp:973-1024 的字段搬运（不含 children 递归细节的省略项）
    Json out = Json::object();
    out["module"] = node.value("module", std::string());
    out["instance"] = node.value("instance", std::string());
    out["file"] = node.value("file", std::string());
    out["line"] = node.value("line", 0);
    out["column"] = node.value("column", 0);
    out["portCount"] = node.value("portCount", 0);
    out["blackbox"] = node.value("blackbox", false);
    out["defined"] = node.value("defined", false);
    out["recursive"] = node.value("recursive", false);
    Json range = Json::object();
    const Json rng = node.value("range", Json::object());
    const Json start = rng.value("start", Json::object());
    const Json end = rng.value("end", Json::object());
    range["start"] = Json{{"line", start.value("line", 0) + 1},
                          {"column", start.value("column", 0) + 1}};
    range["end"] = Json{{"line", end.value("line", 0) + 1}, {"column", end.value("column", 0) + 1}};
    out["range"] = range;
    if (node.contains("usedAt") && node["usedAt"].is_object()) {
        out["usedAt"] = node["usedAt"];
    }
    if (node.contains("children") && node["children"].is_array()) {
        Json children = Json::array();
        for (const Json& child : node["children"]) children.push_back(ConvertLikeBridge(child));
        out["children"] = std::move(children);
    }
    return out;
}

void RunStructAttribution(Bridge& bridge, int iters, int warmup) {
    std::printf("\n--- project_structure attribution ---\n");
    Json structure;
    try {
        structure = bridge.Call("project_structure", Json::object());
    } catch (const std::exception& exc) {
        std::printf("project_structure failed: %s\n", exc.what());
        return;
    }
    const Json root = structure.contains("root") ? structure["root"] : Json();
    const std::size_t nodes = CountNodes(root);
    std::printf("nodes=%zu bytes=%zu\n", nodes, structure.dump().size());

    auto total = perf::Bench("project_structure(total)", iters,
                             [&bridge] { (void)bridge.Call("project_structure", Json::object()); },
                             warmup);
    total.Print();
    total.PrintCsv();

    auto convert = perf::Bench(
        "convert_only(mcp copy pass)", iters, [&root] { (void)ConvertLikeBridge(root); }, warmup);
    convert.Print();
    convert.PrintCsv();
}

// ---------------------------------------------------------------------------
// dispatch 节：39 分支字符串链 vs 真实 handler
// ---------------------------------------------------------------------------
void RunDispatchAttribution(Bridge& bridge, int iters, int warmup) {
    std::printf("\n--- dispatch ---\n");
    // 复刻 bridge.cpp:1477+ 的形态：std::string 拷贝 + 顺序 if (name == "...")
    static const char* kHandlers[] = {
        "project_list",  "project_info",     "project_files",   "project_reports",
        "diagnostics",   "device_info",      "constraints_status", "constraint_audit",
        "td_info",       "read_file",        "search_text",     "flow_status",
        "flow_log",      "flow_issues",      "flow_wait",       "flow_run",
        "flow_cancel",   "strategy_compare", "cables",          "completion",
        "instantiation", "project_rename",   "project_add_source", "project_structure",
        "document_symbols", "workspace_symbols", "search_hierarchy", "definition",
        "references",    "hover",            "symbol_path",     "call_hierarchy",
        "module_info",   "duplicate_modules", "format",         "code_actions",
        "rename_edits",  "analysis_run",     "project_update",
    };
    auto chain = perf::Bench(
        "dispatch_chain(39 strcmp, 'td_info')", iters * 1000,
        [] {
            const std::string name = "td_info";  // 15th 分支：平均要比较 15 次
            int hit = -1;
            for (int index = 0; index < 39; ++index) {
                if (name == kHandlers[index]) {
                    hit = index;
                    break;
                }
            }
            if (hit < 0) std::printf("unreachable\n");
        },
        3);
    chain.Print();
    chain.PrintCsv();
    std::printf("note: 上面是 %d 次/样本，单次 = %.3f us\n", iters * 1000,
                chain.P50() * 1000.0 / (iters * 1000.0));

    // 真实 handler 的上界：flow_status 是"几乎只做 dispatch + 读一个进程内状态"
    auto cheap = perf::Bench(
        "bridge_Call(flow_status) total", iters,
        [&bridge] { (void)bridge.Call("flow_status", Json::object()); }, warmup);
    cheap.Print();
    cheap.PrintCsv();
}

// ---------------------------------------------------------------------------
// json 节：序列化 / 帧
// ---------------------------------------------------------------------------
void RunJsonAttribution(Bridge& bridge, int iters, int warmup) {
    std::printf("\n--- json / serialization ---\n");
    Json completion;
    Json structure;
    try {
        completion = bridge.Call(
            "completion", Json{{"file", "src/top.v"}, {"line", 1}, {"column", 1}, {"limit", 200}});
        structure = bridge.Call("project_structure", Json::object());
    } catch (const std::exception& exc) {
        std::printf("payload build failed: %s\n", exc.what());
        return;
    }
    struct Case {
        const char* name;
        Json payload;
    };
    const std::vector<Case> cases = {{"completion", completion}, {"project_structure", structure}};
    for (const Case& item : cases) {
        const size_t bytes = item.payload.dump().size();
        std::printf("[%s] bytes=%zu\n", item.name, bytes);
        auto compact = perf::Bench(
            std::string(item.name) + ":dump(-1)", iters,
            [&item] { (void)item.payload.dump(-1, ' ', false, Json::error_handler_t::replace); },
            warmup);
        compact.Print();
        compact.PrintCsv();
        auto indent1 = perf::Bench(
            std::string(item.name) + ":dump(1) [mcp_server 路径]", iters,
            [&item] { (void)item.payload.dump(1, ' ', false, Json::error_handler_t::replace); },
            warmup);
        indent1.Print();
        indent1.PrintCsv();
        const std::string compact_text =
            item.payload.dump(-1, ' ', false, Json::error_handler_t::replace);
        // AddPythonStyleSpaces 是 json_util.cpp 的内部函数；这里用公开的
        // Serialize(value, false) - Serialize(value, true) 的差值给"整份复制"定系数（E）。
        auto spaces = perf::Bench(
            std::string(item.name) + ":Serialize(compact=false) 含整份复制", iters,
            [&item] { (void)fpgalsp::jsonrpc::Serialize(item.payload, /*compact=*/false); },
            warmup);
        spaces.Print();
        spaces.PrintCsv();
        auto compact_serialize = perf::Bench(
            std::string(item.name) + ":Serialize(compact=true) 基线", iters,
            [&item] { (void)fpgalsp::jsonrpc::Serialize(item.payload, /*compact=*/true); },
            warmup);
        compact_serialize.Print();
        compact_serialize.PrintCsv();
        auto parse = perf::Bench(
            std::string(item.name) + ":parse(响应体)", iters,
            [&compact_text] { (void)Json::parse(compact_text); }, warmup);
        parse.Print();
        parse.PrintCsv();
    }
}

// ---------------------------------------------------------------------------
// io 节：search_text 逐行物化 / read_file / GetObject 按值拷贝
// ---------------------------------------------------------------------------
std::vector<std::string> SplitLinesLikeBridge(std::string_view text) {
    // 复刻 src/mcp/bridge.cpp:169 SplitLines 的形态（每行一个 std::string）
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t pos = text.find('\n', start);
        if (pos == std::string_view::npos) {
            lines.emplace_back(text.substr(start));
            break;
        }
        lines.emplace_back(text.substr(start, pos - start));
        start = pos + 1;
    }
    return lines;
}

void RunIoAttribution(Bridge& bridge, const std::string& root, int iters, int warmup) {
    std::printf("\n--- io ---\n");
    const std::string top = root + "/src/top.v";
    const std::string text = ReadText(top);
    std::printf("top.v bytes=%zu lines=%d\n", text.size(),
                static_cast<int>(std::count(text.begin(), text.end(), '\n') + 1));

    auto split = perf::Bench(
        "SplitLines(top.v) 逐行物化", iters * 50, [&text] { (void)SplitLinesLikeBridge(text); }, 3);
    split.Print();
    split.PrintCsv();

    auto read = perf::Bench(
        "ReadTextFile(top.v) 每次读盘", iters * 20, [&top] { (void)ReadText(top); }, 3);
    read.Print();
    read.PrintCsv();

    // GetObject 按值返回 vs find 指针
    Json params = Json::object();
    Json doc = Json::object();
    doc["uri"] = "file:///" + top;
    doc["text"] = std::string(100 * 1024, 'x');  // 100 KB 文档
    params["textDocument"] = doc;
    auto get_object = perf::Bench(
        "GetObject(100KB textDocument) 按值拷贝", iters * 20,
        [&params] { (void)fpgalsp::jsonrpc::GetObject(params, "textDocument"); }, 3);
    get_object.Print();
    get_object.PrintCsv();
    auto find_ptr = perf::Bench(
        "params.find(textDocument) 指针", iters * 20,
        [&params] {
            const auto it = params.find("textDocument");
            const Json* found = it == params.end() ? nullptr : &(*it);
            if (found == nullptr) std::printf("unreachable\n");
        },
        3);
    find_ptr.Print();
    find_ptr.PrintCsv();

    // search_text 端到端（200 文件）
    Json args = Json::object();
    args["query"] = "always";
    args["limit"] = 200;
    auto search = perf::Bench(
        "search_text(200 files)", iters,
        [&bridge, &args] { (void)bridge.Call("search_text", args); }, warmup);
    search.Print();
    search.PrintCsv();
}

// ---------------------------------------------------------------------------
// scan 节：哪些 handler 每次调用都做"全量扫描/全量索引"
// ---------------------------------------------------------------------------
void RunScanAttribution(Bridge& bridge, const std::string& root, int iters, int warmup) {
    std::printf("\n--- scan / repeated full work ---\n");
    const std::string al = root + "/perf200.al";

    auto discover = perf::Bench(
        "project::DiscoverProjects(root,6) 每次全扫", iters,
        [&root] { (void)fpgalsp::project::DiscoverProjects(root, 6); }, warmup);
    discover.Print();
    discover.PrintCsv();

    auto parse_al = perf::Bench(
        "project::ParseAl(201 entries)", iters, [&al] { (void)fpgalsp::project::ParseAl(al); },
        warmup);
    parse_al.Print();
    parse_al.PrintCsv();

    auto find_td = perf::Bench(
        "td::FindTd() 安装发现", iters, [] { (void)fpgalsp::td::FindTd(); }, warmup);
    find_td.Print();
    find_td.PrintCsv();

    auto list_tool = perf::Bench(
        "Call(project_list) 每次重扫", iters,
        [&bridge] { (void)bridge.Call("project_list", Json::object()); }, warmup);
    list_tool.Print();
    list_tool.PrintCsv();

    // N 次调用是否 N 次全索引：第一次（冷）与后续（热）分别看
    Json ws_args = Json::object();
    ws_args["query"] = "m0";
    auto ws_first = perf::Clock::now();
    (void)bridge.Call("workspace_symbols", ws_args);
    const double ws_cold = perf::MsSince(ws_first);
    auto ws_warm = perf::Bench(
        "Call(workspace_symbols) warm", iters,
        [&bridge, &ws_args] { (void)bridge.Call("workspace_symbols", ws_args); }, warmup);
    std::printf("workspace_symbols cold=%.3f ms\n", ws_cold);
    ws_warm.Print();
    ws_warm.PrintCsv();
}

}  // namespace

int main(int argc, char** argv) {
    const perf::Cli cli = perf::ParseCli(argc, argv);
    perf::Banner("mcp_bridge");

    const std::string root = EnsureSyntheticProject();
    if (cli.extra == "synthetic-only") return 0;

    // 解析要跑哪些节：位置参数可以是 "struct,dispatch" 这类逗号列表
    std::set<std::string> sections;
    if (cli.extra.empty()) {
        sections.insert("tools");
    } else {
        std::string item;
        for (char ch : cli.extra) {
            if (ch == ',') {
                if (!item.empty()) sections.insert(item);
                item.clear();
            } else {
                item.push_back(ch);
            }
        }
        if (!item.empty()) sections.insert(item);
    }
    const bool all = sections.count("all") != 0;

    BridgeOptions options;
    options.root = root;
    const auto init_start = perf::Clock::now();
    Bridge bridge(options);
    const double init_ms = perf::MsSince(init_start);
    std::printf("bridge_init: %.3f ms (parse .al + project discovery, no index)\n", init_ms);

    if (all || sections.count("tools")) RunToolLatency(bridge, root, cli.iters, cli.warmup);
    if (all || sections.count("struct")) RunStructAttribution(bridge, cli.iters, cli.warmup);
    if (all || sections.count("dispatch")) RunDispatchAttribution(bridge, cli.iters, cli.warmup);
    if (all || sections.count("json")) RunJsonAttribution(bridge, cli.iters, cli.warmup);
    if (all || sections.count("io")) RunIoAttribution(bridge, root, cli.iters, cli.warmup);
    if (all || sections.count("scan")) RunScanAttribution(bridge, root, cli.iters, cli.warmup);

    std::printf("\nworking_set=%.2f MB peak_working_set=%.2f MB\n",
                perf::Mb(perf::CurrentWorkingSetBytes()), perf::Mb(perf::PeakWorkingSetBytes()));
    bridge.Shutdown();
    return 0;
}