// =============================================================================
//  tests/unit/constraint_symbols_test.cpp —— B10：约束符号 / completion / moduleTree
// =============================================================================
//  对照参考实现 Y:\fpga-lsp @ b4891bb（server.py / adc.py / sdc.py）与
//  `audit/2026-11-18/parity-lsp/raw.json` 的 boundaries[adc:*, sdc:*]、matrix[*]。
//
//  覆盖三件事（对应 parity-matrix.md 的 LSP-11 / LSP-12 / LSP-16 / CAP-01）：
//    1. `.adc` / `.sdc` 的 documentSymbol / hover / completion
//       pin.adc -> 47 条 documentSymbol，top.sdc -> 2 条，hover 非 null。
//    2. completion 的候选集：745 条器件原语 + 48 条 Verilog 关键字（+ 索引里的模块）。
//    3. moduleTree：`includePrimitives=false` 时不得展开 IP 文件里的原语黑盒
//       （Python 34 节点 / 修复前 C++ 36 节点）。
//
//  ⚠ 这里的期望值全部是**从 Python 探针输出里抄出来的字面量**，不是"按实现反推"。
//     改实现时如果这些用例红了，先证明 Python 那边也变了，再改这里。
// =============================================================================

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "constraints/adc.h"
#include "constraints/sdc.h"
#include "hdl/hdl_index.h"
#include "hdl/primitives.h"
#include "project/al_project.h"
#include "server/constraints_features.h"
#include "server/features.h"

namespace {

using Json = nlohmann::json;

using fpgalsp::constraints::Port;
namespace adc = fpgalsp::constraints::adc;
namespace sdc = fpgalsp::constraints::sdc;
using fpgalsp::hdl::HdlIndex;

// 真实语料（与 Python 探针同一份文件：CorpusAlpha）
std::string CorpusPath(const std::string& relative) {
    return std::string(FPGA_LSP_SOURCE_DIR) + "/tests/gate-corpus/corpus_alpha/" + relative;
}

std::string ReadCorpus(const std::string& relative) {
    // 二进制读：列号是**字节列**语义（.adc/.sdc 全是 ASCII，等价于 UTF-16 列），
    // 且绝不能让文本模式把 CRLF 归一化掉 —— Python 侧读时是 universal newlines，
    // 但 documentSymbol 的 range 用的是"切行后的行长"，两侧必须都去掉 \r。
    std::ifstream input(std::filesystem::path(CorpusPath(relative)), std::ios::binary);
    if (!input) return {};
    std::string text;
    input.seekg(0, std::ios::end);
    text.resize(static_cast<std::size_t>(input.tellg()));
    input.seekg(0, std::ios::beg);
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    return text;
}

Json MakeRange(int line, int start, int end) {
    Json range = Json::object();
    range["start"] = {{"line", line}, {"character", start}};
    range["end"] = {{"line", line}, {"character", end}};
    return range;
}

// moduleTree 的节点总数（递归数 children）
int CountNodes(const Json& node) {
    if (!node.is_object()) return 0;
    int total = 1;
    for (const Json& child : node.value("children", Json::array())) total += CountNodes(child);
    return total;
}

// 收集黑盒节点（module 名 + 是否 blackbox）
void CollectBlackboxes(const Json& node, std::vector<std::string>* out) {
    if (!node.is_object()) return;
    if (node.value("blackbox", false)) out->push_back(node.value("module", std::string()));
    for (const Json& child : node.value("children", Json::array())) CollectBlackboxes(child, out);
}

}  // namespace

// =============================================================================
//  1. `.adc` 的 documentSymbol（Python: 47 条）
// =============================================================================

TEST(AdcSymbols, DocumentSymbolsMatchPythonProbe) {
    const std::string text = ReadCorpus("constrains/pin.adc");
    ASSERT_FALSE(text.empty()) << "语料缺失: " << CorpusPath("constrains/pin.adc");

    const Json symbols = adc::DocumentSymbols(text);
    ASSERT_TRUE(symbols.is_array());
    ASSERT_EQ(symbols.size(), 47u);  // Python 探针 adc:documentSymbol = 47

    // 首条逐字段（raw.json boundaries[adc:documentSymbol].python.raw[0]）
    const Json& first = symbols[0];
    EXPECT_EQ(first["name"], "clk_input");
    EXPECT_EQ(first["detail"], "P87");
    EXPECT_EQ(first["kind"], 8);
    EXPECT_EQ(first["range"], MakeRange(0, 0, 92));
    EXPECT_EQ(first["selectionRange"], MakeRange(0, 0, 9));

    // 位选端口的 selectionRange 用**端口名整体**长度（`w[0]` -> 4）
    const Json& second = symbols[1];
    EXPECT_EQ(second["name"], "w[0]");
    EXPECT_EQ(second["detail"], "P76");
    EXPECT_EQ(second["selectionRange"], MakeRange(1, 0, 4));

    // 末条（raw.json 的 __tail__）
    const Json& last = symbols[symbols.size() - 1];
    EXPECT_EQ(last["name"], "relay[3]");
    EXPECT_EQ(last["detail"], "P43");
    EXPECT_EQ(last["range"], MakeRange(51, 0, 99));

    // 每一条都必须有 range/selectionRange/kind/name/detail 五个键且无 children
    for (const Json& symbol : symbols) {
        EXPECT_TRUE(symbol.contains("range"));
        EXPECT_TRUE(symbol.contains("selectionRange"));
        EXPECT_EQ(symbol["kind"], 8);
        EXPECT_FALSE(symbol.contains("children"));
    }

    // 空文档 = 没有符号（Python: adc.parse("") == []）
    EXPECT_TRUE(adc::DocumentSymbols("").empty());
}

TEST(AdcSymbols, CommentAndBlankLinesAreNotSymbols) {
    const std::string text =
        "# a comment\n"
        "\n"
        "set_pin_assignment\t{ clk } \t{ LOCATION = P1; }\n"
        "// another comment\n";
    const Json symbols = adc::DocumentSymbols(text);
    ASSERT_EQ(symbols.size(), 1u);
    EXPECT_EQ(symbols[0]["name"], "clk");
    EXPECT_EQ(symbols[0]["detail"], "P1");
}

// =============================================================================
//  2. `.adc` 的 hover（整行属性 markdown；无器件引脚表时的 Python 行为）
// =============================================================================

TEST(AdcSymbols, HoverShowsDecodedAttributes) {
    const std::string text = ReadCorpus("constrains/pin.adc");
    ASSERT_FALSE(text.empty());

    const Json hover = adc::Hover(text, 0, 0);
    ASSERT_TRUE(hover.is_object());
    EXPECT_EQ(hover["contents"]["kind"], "markdown");
    EXPECT_EQ(hover["contents"]["value"],
              "**clk_input**\n\n- `LOCATION` = `P87`\n- `IOSTANDARD` = `LVCMOS33`\n"
              "- `PULLTYPE` = `NONE`");
    EXPECT_EQ(hover["range"], MakeRange(0, 0, 92));

    // 越界/空行 -> null（绝不抛异常）
    EXPECT_TRUE(adc::Hover(text, 9999, 0).is_null());
    EXPECT_TRUE(adc::Hover("", 0, 0).is_null());
    EXPECT_TRUE(adc::Hover(text, -1, -1).is_null());
}

TEST(AdcSymbols, HoverAttributeOrderFollowsFirstAppearance) {
    // Python 的 attrs 是 dict：顺序 = **首次出现**，值 = **最后一次**赋值。
    const std::string text =
        "set_pin_assignment\t{ p } \t{ PULLTYPE = NONE; LOCATION = P5; PULLTYPE = PULLUP; }\n";
    const Json hover = adc::Hover(text, 0, 0);
    ASSERT_TRUE(hover.is_object());
    EXPECT_EQ(hover["contents"]["value"],
              "**p**\n\n- `PULLTYPE` = `PULLUP`\n- `LOCATION` = `P5`");
}

// =============================================================================
//  3. `.adc` 的 completion（光标在命令名上 = 2 条 snippet）
// =============================================================================

TEST(AdcSymbols, CompletionOnCommandKeyword) {
    const std::string text = ReadCorpus("constrains/pin.adc");
    ASSERT_FALSE(text.empty());

    const Json result = adc::Completion(text, 0, 0);
    ASSERT_TRUE(result.is_object());
    EXPECT_EQ(result["isIncomplete"], false);
    const Json& items = result["items"];
    ASSERT_EQ(items.size(), 2u);  // Python 探针 adc:completion = 2
    EXPECT_EQ(items[0]["label"], "set_pin_assignment");
    EXPECT_EQ(items[0]["kind"], 3);
    EXPECT_EQ(items[0]["detail"], "TD ADC pin constraints");
    EXPECT_EQ(items[0]["insertText"],
              "set_pin_assignment\t{ ${1:port} }\t{ LOCATION = ${2:pin}; IOSTANDARD = "
              "${3:LVCMOS33}; }");
    EXPECT_EQ(items[0]["insertTextFormat"], 2);
    EXPECT_EQ(items[1]["label"], "set_ippin_assignment");
}

TEST(AdcSymbols, CompletionAttributeKeysAndIoStandards) {
    const std::string text = "set_pin_assignment\t{ clk } \t{ LOC =  }\n";
    // 光标落在 `LOC` 上：kind = attr_key -> 7 条 ADC_ATTRS，带 " = " 插入文本
    const Json attr = adc::Completion(text, 0, static_cast<int>(text.find("LOC")));
    const Json& attr_items = attr["items"];
    ASSERT_EQ(attr_items.size(), 7u);
    EXPECT_EQ(attr_items[0]["label"], "LOCATION");
    EXPECT_EQ(attr_items[0]["kind"], 10);
    EXPECT_EQ(attr_items[0]["insertText"], "LOCATION = ");

    // 光标落在 IOSTANDARD 的值上 -> 器件实测 IO 标准表（37 条，kind 12）
    const std::string io = "set_pin_assignment\t{ clk } \t{ IOSTANDARD = LV }\n";
    const Json io_items = adc::Completion(io, 0, static_cast<int>(io.find("LV")) + 1)["items"];
    ASSERT_EQ(io_items.size(), 37u);
    EXPECT_EQ(io_items[0]["label"], "BLVDS25");
    EXPECT_EQ(io_items[0]["kind"], 12);
    EXPECT_EQ(io_items[0]["detail"], "IOSTANDARD");
}

// =============================================================================
//  4. `.sdc` 的 documentSymbol（Python: 2 条）
// =============================================================================

TEST(SdcSymbols, DocumentSymbolsMatchPythonProbe) {
    const std::string text = ReadCorpus("constrains/top.sdc");
    ASSERT_FALSE(text.empty());

    const Json symbols = sdc::DocumentSymbols(text);
    ASSERT_TRUE(symbols.is_array());
    ASSERT_EQ(symbols.size(), 2u);  // Python 探针 sdc:documentSymbol = 2

    EXPECT_EQ(symbols[0]["name"], "create_clock");
    // Python: `" ".join(args)[:60]` —— 注意是在 60 个字符处**硬截断**（会切断 {clk_input}）
    EXPECT_EQ(symbols[0]["detail"],
              "-name clk_input -period 40 -waveform 0 20 [get_ports {clk_in");
    EXPECT_EQ(symbols[0]["kind"], 3);
    EXPECT_EQ(symbols[0]["range"], MakeRange(0, 0, 80));
    EXPECT_EQ(symbols[0]["selectionRange"], MakeRange(0, 0, 12));

    EXPECT_EQ(symbols[1]["name"], "derive_pll_clocks");
    EXPECT_EQ(symbols[1]["detail"], "");
    EXPECT_EQ(symbols[1]["range"], MakeRange(1, 0, 17));
    EXPECT_EQ(symbols[1]["selectionRange"], MakeRange(1, 0, 17));

    EXPECT_TRUE(sdc::DocumentSymbols("").empty());
    EXPECT_TRUE(sdc::DocumentSymbols("# only a comment\n").empty());
}

// =============================================================================
//  5. `.sdc` 的 hover（命令 hover = TD help 的 usage 原文）
// =============================================================================

TEST(SdcSymbols, HoverCommandUsesTdUsage) {
    const std::string text = ReadCorpus("constrains/top.sdc");
    ASSERT_FALSE(text.empty());

    const Json hover = sdc::Hover(text, 0, 0);
    ASSERT_TRUE(hover.is_object());
    EXPECT_EQ(hover["contents"]["kind"], "markdown");
    EXPECT_EQ(hover["contents"]["value"],
              "**create_clock**\n\n```\ncreate_clock -help -add -name <string> -period "
              "<double> -waveform <string> target\n```");
    EXPECT_EQ(hover["range"], MakeRange(0, 0, 12));

    // 第二条命令同样有 hover（derive_pll_clocks 在表里）
    const Json second = sdc::Hover(text, 1, 0);
    ASSERT_TRUE(second.is_object());
    EXPECT_NE(second["contents"]["value"].get<std::string>().find("derive_pll_clocks"),
              std::string::npos);

    // 不认识的名字 -> null
    EXPECT_TRUE(sdc::Hover("no_such_command_xyz arg\n", 0, 0).is_null());
    EXPECT_TRUE(sdc::Hover(text, 9999, 0).is_null());
}

// =============================================================================
//  6. `.sdc` 的 completion
// =============================================================================

TEST(SdcSymbols, CompletionOnCommandKeyword) {
    const std::string text = ReadCorpus("constrains/top.sdc");
    ASSERT_FALSE(text.empty());

    const Json result = sdc::LspCompletion(text, 0, 0);
    ASSERT_TRUE(result.is_object());
    EXPECT_EQ(result["isIncomplete"], false);
    const Json& items = result["items"];
    // 行首前缀 "create_clock" 过滤后只剩 create_clock 自己（Python 探针 = 1 条）
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0]["label"], "create_clock");
    EXPECT_EQ(items[0]["kind"], 3);
    EXPECT_EQ(items[0]["detail"],
              "create_clock -help -add -name <string> -period <double> -waveform <string> target");
}

TEST(SdcSymbols, CommandTableMatchesPythonData) {
    // Python `_complete_sdc` 的候选集大小 = td_commands.json 的命令数（864）
    EXPECT_EQ(sdc::CommandUsages().size(), 864u);

    const auto* create_clock = [&]() -> const sdc::CommandUsage* {
        for (const sdc::CommandUsage& row : sdc::CommandUsages()) {
            if (row.name == "create_clock") return &row;
        }
        return nullptr;
    }();
    ASSERT_NE(create_clock, nullptr);
    EXPECT_EQ(create_clock->usage,
              "create_clock -help -add -name <string> -period <double> -waveform <string> target");
    // 选项原顺序（Python 的 info["options"]）
    EXPECT_NE(create_clock->options.find("-help -add -name -period -waveform"),
              std::string_view::npos);

    // 空行上光标不属于任何上下文 -> 空 items（Python: sdc.context 返回 None）
    const Json empty = sdc::LspCompletion("\n\n", 0, 0);
    EXPECT_EQ(empty["items"].size(), 0u);
}

// =============================================================================
//  7. 器件原语 / Verilog 关键字表（completion 覆盖 & moduleTree 黑盒判定）
// =============================================================================

TEST(HdlPrimitives, CountsMatchPythonProbe) {
    // Python 探针：同一位置 814 项 = 21 模块 + 745 器件原语 + 48 关键字
    EXPECT_EQ(fpgalsp::hdl::DevicePrimitives().size(), 745u);
    EXPECT_EQ(fpgalsp::hdl::VerilogKeywords().size(), 48u);

    // 排序（Python `for name in sorted(...)`）
    const auto& primitives = fpgalsp::hdl::DevicePrimitives();
    EXPECT_EQ(*primitives.begin(), "DR1_LOGIC_BUF");
    EXPECT_EQ(*primitives.rbegin(), "xor");

    // 关键字顺序与内容照抄 Python（含重复的 casex —— 探针按 kind 计数 48）
    std::size_t casex = 0;
    for (const std::string& keyword : fpgalsp::hdl::VerilogKeywords()) {
        if (keyword == "casex") ++casex;
    }
    EXPECT_EQ(casex, 2u);
    EXPECT_EQ(fpgalsp::hdl::VerilogKeywords().front(), "module");
    EXPECT_EQ(fpgalsp::hdl::VerilogKeywords().back(), "endspecify");
}

TEST(HdlPrimitives, IsDevicePrimitiveIsCaseInsensitive) {
    // CorpusAlpha 的 IP 文件（al_ip/pll_gen.v）里实例化的两个原语
    EXPECT_TRUE(fpgalsp::hdl::IsDevicePrimitive("EF2_PHY_PLL"));
    EXPECT_TRUE(fpgalsp::hdl::IsDevicePrimitive("ef2_phy_pll"));
    EXPECT_TRUE(fpgalsp::hdl::IsDevicePrimitive("EF2_LOGIC_BUFG"));
    EXPECT_TRUE(fpgalsp::hdl::IsDevicePrimitive("ef2_logic_bufg"));
    // 通用门也在 745 表里（Python 的 primitives 集合 = GENERIC_GATES ∪ cells）
    EXPECT_TRUE(fpgalsp::hdl::IsDevicePrimitive("and"));
    EXPECT_TRUE(fpgalsp::hdl::IsDevicePrimitive("BUF"));
    // 普通模块名不是原语
    EXPECT_FALSE(fpgalsp::hdl::IsDevicePrimitive("top"));
    EXPECT_FALSE(fpgalsp::hdl::IsDevicePrimitive("pll_gen"));
    EXPECT_FALSE(fpgalsp::hdl::IsDevicePrimitive(""));
}

// =============================================================================
//  8. moduleTree：includePrimitives=false 不展开 IP 里的原语黑盒（LSP-16）
// =============================================================================

namespace {

// 造一个"合成工程"：top -> pll_gen（IP 文件里的模块）-> EF2_PHY_PLL / EF2_LOGIC_BUFG
fpgalsp::hdl::BuildResult BuildSyntheticIpProject() {
    fpgalsp::project::Project project;
    project.dir = "Y:/b10_synthetic";
    project.al_path = "Y:/b10_synthetic/p.al";
    project.name = "b10_synthetic";
    project.family = "EF2";
    project.device = "EF2L45LG144B";
    project.top = "top";

    const std::string top_source =
        "module top (input clk, output q);\n"
        "  pll_gen u_pll (.clk(clk), .q(q));\n"
        "endmodule\n";
    const std::string pll_source =
        "module pll_gen (input clk, output q);\n"
        "  EF2_PHY_PLL u_pll_core ();\n"
        "  EF2_LOGIC_BUFG u_bufg ();\n"
        "endmodule\n";

    for (const char* path : {"Y:/b10_synthetic/src/top.v", "Y:/b10_synthetic/al_ip/pll_gen.v"}) {
        fpgalsp::project::AlFile file;
        file.path = path;
        file.rel = path;
        file.kind = fpgalsp::project::FileKind::Verilog;
        file.exists = true;
        file.enabled = true;
        file.excluded = false;
        project.files.push_back(file);
    }

    const auto load = [&](const std::string& path) -> std::optional<std::string> {
        if (path.find("top.v") != std::string::npos) return top_source;
        if (path.find("pll_gen.v") != std::string::npos) return pll_source;
        return std::nullopt;
    };
    return fpgalsp::hdl::BuildIndex(project, load);
}

}  // namespace

TEST(ModuleTree, IncludePrimitivesFalseHidesIpPrimitiveBlackboxes) {
    fpgalsp::project::Project project;
    project.dir = "Y:/b10_synthetic";
    project.al_path = "Y:/b10_synthetic/p.al";
    project.name = "b10_synthetic";
    project.top = "top";

    const auto build = BuildSyntheticIpProject();
    const int without = CountNodes(fpgalsp::server::features::ModuleTree(
        build.index, project, /*include_primitives=*/false)["root"]);
    const int with = CountNodes(fpgalsp::server::features::ModuleTree(
        build.index, project, /*include_primitives=*/true)["root"]);

    // top + pll_gen = 2；两个器件原语在默认参数下**不得**出现
    EXPECT_EQ(without, 2);
    EXPECT_EQ(with, 4);

    std::vector<std::string> blackboxes;
    CollectBlackboxes(fpgalsp::server::features::ModuleTree(build.index, project, false)["root"],
                      &blackboxes);
    EXPECT_TRUE(blackboxes.empty()) << "默认参数下不应有黑盒节点";

    // includePrimitives=true 时两个原语以黑盒节点出现（这是显式请求的行为）
    blackboxes.clear();
    CollectBlackboxes(fpgalsp::server::features::ModuleTree(build.index, project, true)["root"],
                      &blackboxes);
    EXPECT_EQ(blackboxes.size(), 2u);
}

TEST(ModuleTree, RealCorpusMatchesPythonNodeCount) {
    // Python 探针（PROBE.md §3.5）：完整 payload 递归计数 = 34 节点，其中 0 个 blackbox；
    // 修复前 C++ 是 36（多出 pll_gen 里的 EF2_LOGIC_BUFG / EF2_PHY_PLL 两个黑盒）。
    const fpgalsp::project::Project project =
        fpgalsp::project::ParseAl(CorpusPath("demo_project.al"));
    ASSERT_EQ(project.top, "top") << "语料工程解析失败: " << CorpusPath("demo_project.al");

    const auto load = [](const std::string& path) -> std::optional<std::string> {
        std::ifstream input(std::filesystem::path(path), std::ios::binary);
        if (!input) return std::nullopt;
        std::string text;
        input.seekg(0, std::ios::end);
        text.resize(static_cast<std::size_t>(input.tellg()));
        input.seekg(0, std::ios::beg);
        input.read(text.data(), static_cast<std::streamsize>(text.size()));
        return text;
    };
    const auto build = fpgalsp::hdl::BuildIndex(project, load);
    ASSERT_GT(build.index.ModuleCount(), 0u);

    const Json tree = fpgalsp::server::features::ModuleTree(build.index, project, false);
    ASSERT_TRUE(tree["ok"].get<bool>()) << tree.dump();
    EXPECT_EQ(CountNodes(tree["root"]), 34);

    std::vector<std::string> blackboxes;
    CollectBlackboxes(tree["root"], &blackboxes);
    EXPECT_TRUE(blackboxes.empty());
}

// =============================================================================
//  9. `features::Completion` 的候选池是 LSP 与 MCP 的**同一份**（B10b）
// =============================================================================
//  B10 曾把 745 原语 + 48 关键字只加在 LSP 的 hdl_handlers 分支，于是：
//    · LSP `textDocument/completion`        -> 814 项 ✅
//    · MCP `fpga_completion`（features 层） ->  21 项 ❌
//  现在候选池在 `features::Completion` 里，两边都走它。这两个用例就是防线：
//  任何一边"自己再加一次"会变成 1607，任何一边"漏掉"会变成 21，都会被钉住。

namespace {

// 真语料工程 + 索引（重复建索引的代价 ~16ms，值得换"用例之间互不影响"）
struct CorpusIndex {
    fpgalsp::project::Project project;
    fpgalsp::hdl::BuildResult build;
};

CorpusIndex LoadCorpusIndex() {
    CorpusIndex out;
    out.project = fpgalsp::project::ParseAl(CorpusPath("demo_project.al"));
    const auto load = [](const std::string& path) -> std::optional<std::string> {
        std::ifstream input(std::filesystem::path(path), std::ios::binary);
        if (!input) return std::nullopt;
        std::string text;
        input.seekg(0, std::ios::end);
        text.resize(static_cast<std::size_t>(input.tellg()));
        input.seekg(0, std::ios::beg);
        input.read(text.data(), static_cast<std::streamsize>(text.size()));
        return text;
    };
    out.build = fpgalsp::hdl::BuildIndex(out.project, load);
    return out;
}

// probe_lsp 的 completion(port-list) 锚点：top.v 里 `.refclk` 那一行的 `.` 之后
void FindCompletionAnchor(const std::string& text, int* line, int* character) {
    std::size_t begin = 0;
    int index = 0;
    while (begin <= text.size()) {
        const std::size_t end = text.find('\n', begin);
        const std::string row = text.substr(begin, (end == std::string::npos ? text.size() : end) - begin);
        if (row.find(".refclk") != std::string::npos) {
            *line = index;
            *character = static_cast<int>(row.find('.')) + 1;
            return;
        }
        if (end == std::string::npos) break;
        begin = end + 1;
        ++index;
    }
    *line = 0;
    *character = 0;
}

}  // namespace

TEST(FeaturesCompletion, CorpusAnchorIs814ForLspAndMcp) {
    CorpusIndex corpus = LoadCorpusIndex();
    ASSERT_GT(corpus.build.index.ModuleCount(), 0u);

    const std::string text = ReadCorpus("src/top.v");
    ASSERT_FALSE(text.empty());
    int line = 0;
    int character = 0;
    FindCompletionAnchor(text, &line, &character);

    const Json result = fpgalsp::server::features::Completion(
        corpus.build.index, CorpusPath("src/top.v"), text, line, character);
    const Json& items = result["items"];
    ASSERT_TRUE(items.is_array());

    // Python 探针 matrix[4]：814 = 21 模块 + 745 器件原语 + 48 关键字
    EXPECT_EQ(items.size(), 814u);

    int modules = 0;
    int primitives = 0;
    int keywords = 0;
    int snippets = 0;
    for (const Json& item : items) {
        const int kind = item.value("kind", 0);
        if (kind == 9) ++modules;
        else if (kind == 3) ++primitives;
        else if (kind == 14) ++keywords;
        else if (kind == 15) ++snippets;
    }
    EXPECT_EQ(modules, 21);
    EXPECT_EQ(primitives, 745);
    EXPECT_EQ(keywords, 48);
    EXPECT_EQ(snippets, 0) << "该锚点不在行首，不应出现实例化模板";

    // 模块候选按 Python 的 `sorted(names)` 顺序（B10b 起排序，保证截断窗口一致）
    EXPECT_EQ(items[0]["label"], "AD_ACQ");
    // 尾部与 Python 同序：... specify, endspecify
    EXPECT_EQ(items[items.size() - 1]["label"], "endspecify");
    EXPECT_EQ(items[items.size() - 2]["label"], "specify");

    // 每一项都必须有 label/kind（MCP 的 bridge 按这两个字段做过滤/命名）
    for (const Json& item : items) {
        EXPECT_TRUE(item.contains("label"));
        EXPECT_TRUE(item.contains("kind"));
    }
}

TEST(FeaturesCompletion, PrimitiveCandidateCarriesDetailLikePython) {
    // Python: {"label": "EF2_PHY_PLL", "kind": 3, "detail": "Device primitive"}
    const Json result = fpgalsp::server::features::Completion(
        LoadCorpusIndex().build.index, CorpusPath("src/top.v"), "", 0, 0);
    const Json& items = result["items"];
    bool found = false;
    for (const Json& item : items) {
        if (item.value("label", std::string()) != "EF2_PHY_PLL") continue;
        found = true;
        EXPECT_EQ(item.value("kind", 0), 3);
        EXPECT_EQ(item.value("detail", std::string()), "Device primitive");
        break;
    }
    EXPECT_TRUE(found) << "器件原语必须出现在候选池里（MCP 也要拿到）";
}
// =============================================================================
//  B-B1：约束诊断接入 LSP（编辑器 Problems 与 MCP fpga_diagnostics 同源）
// =============================================================================
TEST(ConstraintDiagnosticsTest, AdcProblemsAreExposedThroughTheServerHelper) {
    std::vector<fpgalsp::constraints::Port> ports;
    fpgalsp::constraints::Port w;
    w.name = "w";
    w.direction = "input";
    w.width = 8;
    w.msb = 7;
    w.lsb = 0;
    ports.push_back(w);

    // 缺 LOCATION -> USR-8056（error），与 adc_test 用同一条规则
    const std::string text =
        "set_pin_assignment\t{ w[0] }\t{ IOSTANDARD = LVCMOS33; }";
    const auto diagnostics =
        fpgalsp::server::ConstraintDiagnostics("x.adc", text, ports);
    ASSERT_FALSE(diagnostics.empty());
    bool found = false;
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.code == "USR-8056") found = true;
        EXPECT_FALSE(diagnostic.source.empty()) << "诊断必须带来源，Problems 才能分组";
    }
    EXPECT_TRUE(found) << "缺 LOCATION 必须产出 USR-8056";

    // 非约束文档 -> 空（不能把 HDL 文件当成 .adc 解析）
    EXPECT_TRUE(fpgalsp::server::ConstraintDiagnostics("x.v", text, ports).empty());
}
