// =============================================================================
//  tests/unit/hdl_index_test.cpp —— HDL 索引单测
// =============================================================================
//  对照参考实现：Y:\fpga-lsp\fpgalsp\hdl.py
//
//  覆盖重点（任务要求 ≥40 例）：
//    * ANSI / 非 ANSI 端口列表
//    * 参数化模块、generate、函数/任务
//    * 注释与字符串里的伪代码不误判
//    * 未保存缓冲区覆盖磁盘内容（通过 BuildIndex 的 TextLoader 注入）
//    * VHDL entity 提取
//    * IP 生成 .v 纳入（且 .vhd 不走 Verilog —— P-1）
//    * 畸形输入不崩（never-raise）
//    * 位置必须 UTF-16 列（含中文的文件不错位）
// =============================================================================

#include "hdl/hdl_index.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {

using Json = nlohmann::json;

using fpgalsp::hdl::BuildIndex;
using fpgalsp::hdl::BuildOptions;
using fpgalsp::hdl::DocSymbol;
using fpgalsp::hdl::HdlIndex;
using fpgalsp::hdl::Instance;
using fpgalsp::hdl::Module;
using fpgalsp::hdl::ModuleInfo;
using fpgalsp::hdl::ParsedFile;
using fpgalsp::hdl::ParseCache;
using fpgalsp::hdl::ParseFile;
using fpgalsp::hdl::ParseVerilog;
using fpgalsp::hdl::ParseVhdlEntities;
using fpgalsp::hdl::Port;
using fpgalsp::hdl::Signal;

constexpr const char* kPath = "Y:/proj/src/a.v";

ParsedFile Parse(const std::string& text, const char* path = kPath) {
    return ParseVerilog(path, text);
}

const Port* FindPort(const Module& module, const std::string& name) {
    for (const Port& port : module.ports) {
        if (port.name == name) return &port;
    }
    return nullptr;
}

const Signal* FindSignal(const ModuleInfo& info, const std::string& name) {
    for (const Signal& signal : info.signals) {
        if (signal.name == name) return &signal;
    }
    return nullptr;
}

const Instance* FindInstance(const ParsedFile& file, const std::string& name) {
    for (const Instance& instance : file.instances) {
        if (instance.name == name) return &instance;
    }
    return nullptr;
}

// -----------------------------------------------------------------------------
// 1. 基本模块识别
// -----------------------------------------------------------------------------

TEST(HdlParse, SimpleAnsiModule) {
    const ParsedFile file = Parse("module top(input clk, output reg q);\nendmodule\n");
    ASSERT_EQ(file.modules.size(), 1u);
    const Module& module = file.modules[0].module;
    EXPECT_EQ(module.name, "top");
    EXPECT_EQ(module.line, 0);
    EXPECT_EQ(module.character, 7);  // "module " = 7
    EXPECT_EQ(module.end_line, 1);
    EXPECT_TRUE(module.ansi_ports);
    ASSERT_EQ(module.ports.size(), 2u);
    EXPECT_EQ(module.ports[0].name, "clk");
    EXPECT_EQ(module.ports[0].direction, "input");
    EXPECT_EQ(module.ports[1].name, "q");
    EXPECT_EQ(module.ports[1].direction, "output");
    EXPECT_EQ(module.ports[1].net, "reg");
}

TEST(HdlParse, EmptyModuleHasNoPorts) {
    const ParsedFile file = Parse("module foo;\nendmodule\n");
    ASSERT_EQ(file.modules.size(), 1u);
    EXPECT_TRUE(file.modules[0].module.ports.empty());
    // hdl.py: 头部没有端口名 -> 视为 ANSI 风格
    EXPECT_TRUE(file.modules[0].module.ansi_ports);
}

TEST(HdlParse, MultipleModulesInOneFile) {
    const ParsedFile file = Parse("module a;\nendmodule\nmodule b;\nendmodule\n");
    ASSERT_EQ(file.modules.size(), 2u);
    EXPECT_EQ(file.modules[0].module.name, "a");
    EXPECT_EQ(file.modules[1].module.name, "b");
    EXPECT_EQ(file.modules[1].module.line, 2);
}

TEST(HdlParse, ModuleNameMayBeEscaped) {
    const ParsedFile file = Parse("module \\top! (input clk);\nendmodule\n");
    ASSERT_EQ(file.modules.size(), 1u);
    EXPECT_EQ(file.modules[0].module.name, "\\top!");
}

TEST(HdlParse, ModuleKeywordMayBeMacromodule) {
    const ParsedFile file = Parse("macromodule m(input a);\nendmodule\n");
    ASSERT_EQ(file.modules.size(), 1u);
    EXPECT_EQ(file.modules[0].module.name, "m");
}

// -----------------------------------------------------------------------------
// 2. 端口：方向继承 / 位宽 / 非 ANSI
// -----------------------------------------------------------------------------

TEST(HdlParse, AnsiDirectionInheritance) {
    const ParsedFile file = Parse("module m(input a, b, output c, d);\nendmodule\n");
    const Module& module = file.modules[0].module;
    ASSERT_EQ(module.ports.size(), 4u);
    EXPECT_EQ(module.ports[0].direction, "input");
    EXPECT_EQ(module.ports[1].direction, "input");  // 继承
    EXPECT_EQ(module.ports[2].direction, "output");
    EXPECT_EQ(module.ports[3].direction, "output");  // 继承
}

TEST(HdlParse, AnsiPortBusRange) {
    const ParsedFile file = Parse("module m(input [7:0] a, output [3:0] b);\nendmodule\n");
    const Module& module = file.modules[0].module;
    const Port* a = FindPort(module, "a");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->msb, 7);
    EXPECT_EQ(a->lsb, 0);
    EXPECT_EQ(a->width, 8);
    const Port* b = FindPort(module, "b");
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->width, 4);
}

TEST(HdlParse, AnsiPortRangeWithParameter) {
    const ParsedFile file =
        Parse("module m #(parameter W = 8) (input [W-1:0] d);\nendmodule\n");
    const Module& module = file.modules[0].module;
    const Port* d = FindPort(module, "d");
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->msb, 7);
    EXPECT_EQ(d->lsb, 0);
    EXPECT_EQ(d->width, 8);
}

TEST(HdlParse, NonAnsiPortListGetsDirectionFromBody) {
    const std::string text =
        "module m(a, b);\n"
        "  input [7:0] a;\n"
        "  output b;\n"
        "endmodule\n";
    const ParsedFile file = Parse(text);
    const Module& module = file.modules[0].module;
    EXPECT_FALSE(module.ansi_ports);
    ASSERT_EQ(module.ports.size(), 2u);
    const Port* a = FindPort(module, "a");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->direction, "input");
    EXPECT_EQ(a->width, 8);
    const Port* b = FindPort(module, "b");
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->direction, "output");
}

TEST(HdlParse, PortDeclarationWinsOverHeaderPosition) {
    // 非 ANSI：body 里的 input 声明会把端口的位置更新到声明处（hdl.py 行为）
    const std::string text = "module m(a);\n  input a;\nendmodule\n";
    const ParsedFile file = Parse(text);
    const Module& module = file.modules[0].module;
    ASSERT_EQ(module.ports.size(), 1u);
    EXPECT_EQ(module.ports[0].line, 1);  // 声明行，不是 header 行
}

TEST(HdlParse, MultiLinePortList) {
    const std::string text =
        "module m(\n"
        "  input  wire clk,\n"
        "  input  wire rst_n,\n"
        "  output [15:0] data\n"
        ");\n"
        "endmodule\n";
    const ParsedFile file = Parse(text);
    const Module& module = file.modules[0].module;
    ASSERT_EQ(module.ports.size(), 3u);
    EXPECT_EQ(module.ports[0].name, "clk");
    EXPECT_EQ(module.ports[0].net, "wire");
    EXPECT_EQ(module.ports[2].name, "data");
    EXPECT_EQ(module.ports[2].width, 16);
}

TEST(HdlParse, PortCountMatchesRealTopModule) {
    // 与 tests/gate-corpus/corpus_alpha/src/top.v 的真实端口数一致（25）
    const std::string text =
        "module top(\n"
        " input    clk_input,\n"
        " input   [2:0] w,\n"
        " output  GATE_LED,\n"
        " output  GATE_ERR,\n"
        " output  ERR_LED,\n"
        " output ad_cs,\n"
        " output ad_sck,\n"
        " input ad_dout,\n"
        " output  ad_din,\n"
        " output [5:0] pwmG,\n"
        " input  [5:0] IGBT_in,\n"
        " input  [5:0] ERR_in,\n"
        " input PowerDown33,\n"
        " input PowerDown150,\n"
        " input OH_CHK,\n"
        " input DCOV_CHK,\n"
        " input SUPSTS,\n"
        " output ERRCLR,\n"
        " input GR ,\n"
        " input sin,\n"
        " output sout,\n"
        " input [1:0] fan_i,\n"
        " output [1:0] fan_o,\n"
        " output [3:0] relay,\n"
        " output test_led\n"
        ");\n"
        "endmodule\n";
    const ParsedFile file = Parse(text);
    ASSERT_EQ(file.modules.size(), 1u);
    EXPECT_EQ(file.modules[0].module.ports.size(), 25u);
}

// -----------------------------------------------------------------------------
// 3. 参数与信号声明
// -----------------------------------------------------------------------------

TEST(HdlParse, HeaderParameters) {
    const ParsedFile file =
        Parse("module m #(parameter A = 1, parameter B = 2, localparam C = 3) ();\n"
              "endmodule\n");
    const ModuleInfo& info = file.modules[0];
    ASSERT_EQ(info.params.size(), 3u);
    EXPECT_EQ(info.params[0].name, "A");
    EXPECT_EQ(info.params[0].value, "1");
    EXPECT_FALSE(info.params[0].local);
    EXPECT_EQ(info.params[2].name, "C");
    EXPECT_TRUE(info.params[2].local);
    ASSERT_EQ(info.module.parameters.size(), 3u);
    EXPECT_EQ(info.module.parameters[1], "B");
}

TEST(HdlParse, BodyParameterAndLocalparam) {
    const std::string text =
        "module m;\n"
        "  parameter WIDTH = 16;\n"
        "  localparam DEPTH = WIDTH * 2;\n"
        "endmodule\n";
    const ParsedFile file = Parse(text);
    const ModuleInfo& info = file.modules[0];
    ASSERT_EQ(info.params.size(), 2u);
    EXPECT_EQ(info.params[0].name, "WIDTH");
    EXPECT_EQ(info.params[0].value, "16");
    EXPECT_TRUE(info.params[1].local);
    EXPECT_EQ(info.params[1].line, 2);
}

TEST(HdlParse, SignalDeclarations) {
    const std::string text =
        "module m;\n"
        "  wire a;\n"
        "  reg [7:0] b, c;\n"
        "  logic [3:0] d;\n"
        "endmodule\n";
    const ParsedFile file = Parse(text);
    const ModuleInfo& info = file.modules[0];
    ASSERT_EQ(info.signals.size(), 4u);
    const Signal* b = FindSignal(info, "b");
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->type, "reg");
    EXPECT_EQ(b->width, 8);
    const Signal* c = FindSignal(info, "c");
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->type, "reg");
    EXPECT_EQ(FindSignal(info, "d")->type, "logic");
    EXPECT_EQ(FindSignal(info, "a")->type, "wire");
}

TEST(HdlParse, SignalNotDuplicatedWhenAlsoPort) {
    const std::string text =
        "module m(a);\n"
        "  input a;\n"
        "  wire a;\n"
        "endmodule\n";
    const ParsedFile file = Parse(text);
    const ModuleInfo& info = file.modules[0];
    EXPECT_EQ(FindSignal(info, "a"), nullptr);  // 端口名不进 signals
    ASSERT_EQ(info.module.ports.size(), 1u);
}

TEST(HdlParse, SignalWithAttribute) {
    const std::string text =
        "module m;\n"
        "  (* keep = \"true\" *) wire x;\n"
        "endmodule\n";
    const ParsedFile file = Parse(text);
    const ModuleInfo& info = file.modules[0];
    ASSERT_EQ(info.signals.size(), 1u);
    EXPECT_EQ(info.signals[0].name, "x");
}

// -----------------------------------------------------------------------------
// 4. 例化
// -----------------------------------------------------------------------------

TEST(HdlParse, SimpleInstance) {
    const std::string text =
        "module child(input a);\nendmodule\n"
        "module top;\n"
        "  child u_child(.a(w));\n"
        "  wire w;\n"
        "endmodule\n";
    const ParsedFile file = Parse(text);
    const Instance* instance = FindInstance(file, "u_child");
    ASSERT_NE(instance, nullptr);
    EXPECT_EQ(instance->module, "child");
    EXPECT_EQ(instance->line, 3);
    ASSERT_EQ(instance->connections.size(), 1u);
    EXPECT_EQ(instance->connections.at("a"), "w");
}

TEST(HdlParse, InstanceWithParameterOverride) {
    const std::string text =
        "module top;\n"
        "  child #(.W(16)) u0 (.a(x));\n"
        "endmodule\n";
    const ParsedFile file = Parse(text);
    const Instance* instance = FindInstance(file, "u0");
    ASSERT_NE(instance, nullptr);
    EXPECT_EQ(instance->params.at("W"), "16");
}

TEST(HdlParse, MultipleInstancesOnOneStatement) {
    const std::string text =
        "module top;\n"
        "  inv a1 (.y(x)), a2 (.y(y));\n"
        "endmodule\n";
    const ParsedFile file = Parse(text);
    ASSERT_EQ(file.instances.size(), 2u);
    EXPECT_EQ(file.instances[0].name, "a1");
    EXPECT_EQ(file.instances[1].name, "a2");
    EXPECT_EQ(file.instances[0].module, "inv");
}

TEST(HdlParse, UnnamedPrimitiveInstanceIsAccepted) {
    const std::string text =
        "module top;\n"
        "  and (o, a, b);\n"
        "endmodule\n";
    const ParsedFile file = Parse(text);
    ASSERT_EQ(file.instances.size(), 1u);
    EXPECT_EQ(file.instances[0].module, "and");
    EXPECT_TRUE(file.instances[0].name.empty());
}

TEST(HdlParse, UnnamedNonPrimitiveIsRejected) {
    const std::string text =
        "module top;\n"
        "  child (a, b);\n"
        "endmodule\n";
    const ParsedFile file = Parse(text);
    EXPECT_TRUE(file.instances.empty());
}

TEST(HdlParse, InstanceInsideGenerate) {
    const std::string text =
        "module top;\n"
        "  generate\n"
        "    for (i = 0; i < 4; i = i + 1) begin : g\n"
        "      leaf u_leaf (.a(x));\n"
        "    end\n"
        "  endgenerate\n"
        "endmodule\n";
    const ParsedFile file = Parse(text);
    const Instance* instance = FindInstance(file, "u_leaf");
    ASSERT_NE(instance, nullptr);
    EXPECT_EQ(instance->module, "leaf");
}

TEST(HdlParse, InstanceConnectionPortPositions) {
    const std::string text =
        "module top;\n"
        "  child u0 (\n"
        "    .clk(clk),\n"
        "    .rst(rst)\n"
        "  );\n"
        "endmodule\n";
    const ParsedFile file = Parse(text);
    const Instance* instance = FindInstance(file, "u0");
    ASSERT_NE(instance, nullptr);
    ASSERT_EQ(instance->connection_ports.size(), 2u);
    EXPECT_EQ(instance->connection_ports[0].second, "clk");
    EXPECT_EQ(instance->connection_ports[1].second, "rst");
    EXPECT_EQ(instance->connection_ports[0].first.first, 2);  // 第 3 行
}

// -----------------------------------------------------------------------------
// 5. 注释 / 字符串 / 预处理 不误判
// -----------------------------------------------------------------------------

TEST(HdlParse, CommentIsNotCode) {
    const std::string text =
        "// module fake(input a);\n"
        "/* module fake2; endmodule */\n"
        "module real_m;\n"
        "endmodule\n";
    const ParsedFile file = Parse(text);
    ASSERT_EQ(file.modules.size(), 1u);
    EXPECT_EQ(file.modules[0].module.name, "real_m");
}

TEST(HdlParse, StringLiteralIsNotCode) {
    const std::string text =
        "module m;\n"
        "  initial $display(\"module fake; endmodule\");\n"
        "endmodule\n";
    const ParsedFile file = Parse(text);
    ASSERT_EQ(file.modules.size(), 1u);
    EXPECT_EQ(file.modules[0].module.name, "m");
}

TEST(HdlParse, PreprocessorLineIsDropped) {
    const std::string text =
        "`timescale 1ns / 1ps\n"
        "`define WIDTH 8\n"
        "module m;\n"
        "  wire [`WIDTH-1:0] d;\n"
        "endmodule\n";
    const ParsedFile file = Parse(text);
    ASSERT_EQ(file.modules.size(), 1u);
    const ModuleInfo& info = file.modules[0];
    EXPECT_EQ(info.module.name, "m");
    // `define 那行的 token 不应变成例化/信号
    EXPECT_TRUE(info.signals.size() <= 1u);
    for (const Instance& instance : file.instances) {
        EXPECT_NE(instance.module, "define");
    }
}

TEST(HdlParse, FunctionBodyIsSkipped) {
    const std::string text =
        "module m;\n"
        "  function integer f(input integer x);\n"
        "    wire should_not_be_indexed;\n"
        "    f = x;\n"
        "  endfunction\n"
        "  wire real_signal;\n"
        "endmodule\n";
    const ParsedFile file = Parse(text);
    const ModuleInfo& info = file.modules[0];
    EXPECT_EQ(FindSignal(info, "should_not_be_indexed"), nullptr);
    EXPECT_NE(FindSignal(info, "real_signal"), nullptr);
}

// -----------------------------------------------------------------------------
// 6. 畸形输入：never-raise
// -----------------------------------------------------------------------------

TEST(HdlParse, MissingEndmoduleIsReported) {
    const ParsedFile file = Parse("module m(input a);\n  wire x;\n");
    ASSERT_EQ(file.modules.size(), 1u);  // 仍然部分可用
    EXPECT_FALSE(file.parse_errors.empty());
    bool found = false;
    for (const auto& error : file.parse_errors) {
        if (error.message.find("unclosed module") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(HdlParse, UnbalancedPortListIsReported) {
    const ParsedFile file = Parse("module m(input a, b;\nendmodule\n");
    EXPECT_FALSE(file.parse_errors.empty());
}

TEST(HdlParse, GarbageDoesNotThrow) {
    const ParsedFile file = Parse("@@@ ~~~ %%% \x01\x02 \xff\xfe unstructured ###");
    EXPECT_TRUE(file.modules.empty());
    // 只要没崩就算过（never-raise）
    SUCCEED();
}

TEST(HdlParse, EmptyTextIsFine) {
    const ParsedFile file = Parse("");
    EXPECT_TRUE(file.modules.empty());
    EXPECT_TRUE(file.tokens.empty());
    EXPECT_TRUE(file.parse_errors.empty());
}

TEST(HdlParse, ModuleWithoutNameIsReported) {
    const ParsedFile file = Parse("module ;\nendmodule\n");
    EXPECT_FALSE(file.parse_errors.empty());
}

TEST(HdlParse, DeeplyNestedExpressionDoesNotThrow) {
    std::string text = "module m;\n wire [";
    for (int i = 0; i < 200; ++i) text += "(";
    text += "1";
    for (int i = 0; i < 200; ++i) text += ")";
    text += "-1:0] d;\nendmodule\n";
    const ParsedFile file = Parse(text);
    EXPECT_EQ(file.modules.size(), 1u);  // 不崩即可
}

// -----------------------------------------------------------------------------
// 7. VHDL entity
// -----------------------------------------------------------------------------

TEST(HdlVhdl, ExtractsEntity) {
    const std::string text =
        "library ieee;\n"
        "entity my_ent is\n"
        "  port (clk : in std_logic);\n"
        "end entity my_ent;\n"
        "architecture rtl of my_ent is\n"
        "begin\n"
        "end rtl;\n";
    const ParsedFile file = ParseVhdlEntities("Y:/proj/src/x.vhd", text);
    ASSERT_EQ(file.modules.size(), 1u);
    EXPECT_EQ(file.modules[0].module.name, "my_ent");
    EXPECT_TRUE(file.modules[0].module.vhdl);
    EXPECT_TRUE(file.modules[0].module.ports.empty());
    EXPECT_EQ(file.modules[0].module.line, 1);
    EXPECT_EQ(file.modules[0].module.character, 7);
    EXPECT_EQ(file.modules[0].module.end_line, 3);
}

TEST(HdlVhdl, TwoEntitiesAndCaseInsensitive) {
    const std::string text =
        "ENTITY a IS END ENTITY;\n"
        "entity work.b is\nend;\n";
    const ParsedFile file = ParseVhdlEntities("Y:/proj/src/y.vhdl", text);
    ASSERT_EQ(file.modules.size(), 2u);
    EXPECT_EQ(file.modules[0].module.name, "a");
    EXPECT_EQ(file.modules[1].module.name, "b");
}

TEST(HdlVhdl, NoEntityMeansNoModules) {
    const ParsedFile file = ParseVhdlEntities("Y:/proj/src/z.vhd", "-- nothing here\n");
    EXPECT_TRUE(file.modules.empty());
}

TEST(HdlVhdl, ExtensionRouting) {
    EXPECT_TRUE(fpgalsp::hdl::IsVhdlPath("a.vhd"));
    EXPECT_TRUE(fpgalsp::hdl::IsVhdlPath("A.VHDL"));
    EXPECT_FALSE(fpgalsp::hdl::IsVerilogPath("a.vhd"));
    EXPECT_TRUE(fpgalsp::hdl::IsVerilogPath("a.sv"));
    // ParseFile 按扩展名分流：.vhd 里的 module 关键字不会被识别
    const ParsedFile file = ParseFile("x.vhd", "module not_vhdl; endmodule\n");
    EXPECT_TRUE(file.modules.empty());
}

// -----------------------------------------------------------------------------
// 8. UTF-16 列（含中文的文件不错位）
// -----------------------------------------------------------------------------

TEST(HdlUtf16, ColumnsAreUtf16NotBytes) {
    // `y` 前面有中文串："  initial $display(\"中文\"); wire y;\n"
    //   字节列   = 35（"中文" 占 6 字节）
    //   UTF-16 列 = 31（"中文" 占 2 码元）
    const std::string text =
        "module top;\n"
        "  initial $display(\"中文\"); wire y;\n"
        "endmodule\n";
    const ParsedFile file = Parse(text);
    const ModuleInfo& info = file.modules[0];
    const Signal* signal = FindSignal(info, "y");
    ASSERT_NE(signal, nullptr);
    EXPECT_EQ(signal->line, 1);
    EXPECT_EQ(signal->character, 31) << "必须是 UTF-16 列（35 就是字节列，说明没换算）";
}

TEST(HdlUtf16, ChineseCommentBeforeCode) {
    // 中文注释在上一行：位置换算不能把注释的字节数算进来
    const std::string text =
        "// 这是一行中文注释\n"
        "module top(input a);\n"
        "endmodule\n";
    const ParsedFile file = Parse(text);
    ASSERT_EQ(file.modules.size(), 1u);
    EXPECT_EQ(file.modules[0].module.name, "top");
    EXPECT_EQ(file.modules[0].module.line, 1);
    EXPECT_EQ(file.modules[0].module.character, 7);  // "module " 都是 ASCII
    const Port* a = FindPort(file.modules[0].module, "a");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->character, 17);  // "module top(input " = 17
}

TEST(HdlUtf16, NonAsciiIdentifierIsNotAcceptedAsVerilog) {
    // ⚠ 与 Python 的**已知差异**（PYTHON-BUGS P-9）：
    //   hdl.py 的 tokenizer 用 str.isalpha()，会把「中文模块」当成合法标识符；
    //   但 Verilog 的 identifier 只允许 ASCII，Verible 会拒绝。
    //   这里跟随规范（不认非 ASCII 标识符），并保证 never-raise。
    const ParsedFile file = Parse("module 中文模块(input a);\nendmodule\n");
    EXPECT_TRUE(file.modules.empty());   // 没有识别出模块
    // 关键：不能崩。Verible 会给出词法错误，我们如实记录。
    SUCCEED();
}

TEST(HdlUtf16, VhdlEntityColumnIsUtf16) {
    const std::string text = "-- 中文注释行\nentity e is\nend e;\n";
    const ParsedFile file = ParseVhdlEntities("x.vhd", text);
    ASSERT_EQ(file.modules.size(), 1u);
    EXPECT_EQ(file.modules[0].module.line, 1);
}

// -----------------------------------------------------------------------------
// 9. 缓存与哈希
// -----------------------------------------------------------------------------

TEST(HdlCache, ContentHashIsStableAndContentSensitive) {
    const auto a = fpgalsp::hdl::ContentHash("module m; endmodule");
    const auto b = fpgalsp::hdl::ContentHash("module m; endmodule");
    const auto c = fpgalsp::hdl::ContentHash("module m;  endmodule");
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}

TEST(HdlCache, ParseCacheHitsOnSameContent) {
    ParseCache cache;
    const std::string text = "module m; endmodule";
    EXPECT_EQ(cache.Get(kPath, text), nullptr);
    auto parsed = std::make_shared<ParsedFile>(Parse(text));
    cache.Put(kPath, text, parsed);
    const auto hit = cache.Get(kPath, text);
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(cache.misses(), 1u);
    EXPECT_EQ(cache.hits(), 1u);
    // 内容变了就 miss
    EXPECT_EQ(cache.Get(kPath, "module other; endmodule"), nullptr);
}

// -----------------------------------------------------------------------------
// 10. HdlIndex 查询
// -----------------------------------------------------------------------------

namespace {

// 一个含两个模块、一个例化的小工程
struct SampleProject {
    HdlIndex index;

    SampleProject() {
        index.AddFile(Parse(
            "module child #(parameter W = 8) (\n"
            "  input  [W-1:0] d,\n"
            "  output [W-1:0] q\n"
            ");\n"
            "endmodule\n",
            "Y:/proj/src/child.v"));
        index.AddFile(Parse(
            "module top(input clk, output [7:0] q);\n"
            "  wire [7:0] w;\n"
            "  child #(.W(8)) u_child (.d(w), .q(q));\n"
            "  assign w = {8{clk}};\n"
            "endmodule\n",
            "Y:/proj/src/top.v"));
    }
};

}  // namespace

TEST(HdlIndexQuery, ModuleLookup) {
    SampleProject project;
    ASSERT_EQ(project.index.ModuleCount(), 2u);
    const Module* top = project.index.ModuleByName("top");
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(top->name, "top");
    EXPECT_EQ(project.index.FileCount(), 2u);
    const auto names = project.index.ModuleNames();
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "child");
    EXPECT_EQ(names[1], "top");
}

TEST(HdlIndexQuery, FileLookupIsCaseInsensitive) {
    SampleProject project;
    EXPECT_NE(project.index.File("Y:/PROJ/SRC/TOP.V"), nullptr);
    EXPECT_EQ(project.index.File("Y:/proj/src/missing.v"), nullptr);
}

TEST(HdlIndexQuery, TokenAtFindsIdentifier) {
    SampleProject project;
    const ParsedFile* file = project.index.File("Y:/proj/src/top.v");
    ASSERT_NE(file, nullptr);
    const auto* token = HdlIndex::TokenAt(*file, 0, 7);  // "top"
    ASSERT_NE(token, nullptr);
    EXPECT_EQ(token->text, "top");
    EXPECT_EQ(token->kind, 'i');
}

TEST(HdlIndexQuery, KindAtDetectsPortAndSignalAndInstance) {
    SampleProject project;
    // 列号来自 cst_dump --tokens 的实测（不要靠数格猜）
    EXPECT_EQ(project.index.KindAt("Y:/proj/src/top.v", 0, 7), "module");     // top
    EXPECT_EQ(project.index.KindAt("Y:/proj/src/top.v", 0, 17), "port");      // clk
    EXPECT_EQ(project.index.KindAt("Y:/proj/src/top.v", 1, 13), "signal");    // w
    EXPECT_EQ(project.index.KindAt("Y:/proj/src/top.v", 2, 17), "instance");  // u_child
    // ⚠ `module` 关键字本身**不是**符号（与 hdl.py symbol_at 一致：它返回 None）
    EXPECT_EQ(project.index.KindAt("Y:/proj/src/top.v", 0, 0), "");
}

TEST(HdlIndexQuery, SymbolAtPortDetail) {
    SampleProject project;
    const auto symbol = project.index.SymbolAt("Y:/proj/src/top.v", 0, 17);
    ASSERT_TRUE(symbol.has_value());
    EXPECT_EQ(symbol->kind, "port");
    EXPECT_EQ(symbol->name, "clk");
    EXPECT_EQ(symbol->detail, "input");
    EXPECT_EQ(symbol->module, "top");
}

TEST(HdlIndexQuery, DefinitionOfInstanceNameJumpsToModule) {
    SampleProject project;
    // u_child 在第 2 行第 17 列
    const auto location = project.index.DefinitionOf("Y:/proj/src/top.v", 2, 17);
    ASSERT_TRUE(location.has_value());
    EXPECT_EQ(location->path, "Y:/proj/src/child.v");
    EXPECT_EQ(location->line, 0);
    EXPECT_EQ(location->character, 7);
}

TEST(HdlIndexQuery, DefinitionOfModuleReference) {
    SampleProject project;
    // 模块名 "child" 在第 2 行第 2 列
    const auto location = project.index.DefinitionOf("Y:/proj/src/top.v", 2, 3);
    ASSERT_TRUE(location.has_value());
    EXPECT_EQ(location->path, "Y:/proj/src/child.v");
}

TEST(HdlIndexQuery, DefinitionOfConnectedPortJumpsToChildPort) {
    SampleProject project;
    // .d(w) 里的端口名 d：第 2 行，列需要算
    const ParsedFile* file = project.index.File("Y:/proj/src/top.v");
    ASSERT_NE(file, nullptr);
    const Instance* instance = FindInstance(*file, "u_child");
    ASSERT_NE(instance, nullptr);
    ASSERT_FALSE(instance->connection_ports.empty());
    const auto& position = instance->connection_ports[0].first;
    const auto location = project.index.DefinitionOf("Y:/proj/src/top.v", position.first,
                                                     position.second);
    ASSERT_TRUE(location.has_value());
    EXPECT_EQ(location->path, "Y:/proj/src/child.v");
    EXPECT_EQ(location->line, 1);   // child.v 的 `  input  [W-1:0] d,`
    EXPECT_EQ(location->character, 17);
}

TEST(HdlIndexQuery, DocumentSymbolsShape) {
    SampleProject project;
    const auto symbols = project.index.DocumentSymbols("Y:/proj/src/top.v");
    ASSERT_EQ(symbols.size(), 1u);
    const DocSymbol& module = symbols[0];
    EXPECT_EQ(module.name, "top");
    EXPECT_EQ(module.kind, 2);
    EXPECT_EQ(module.range_start_line, 0);
    EXPECT_EQ(module.range_end_line, 4);
    ASSERT_EQ(module.children.size(), 3u);  // 2 端口 + 1 信号
    EXPECT_EQ(module.children[0].name, "clk");
    EXPECT_EQ(module.children[0].kind, 8);
    EXPECT_EQ(module.children[2].name, "w");
    EXPECT_EQ(module.children[2].kind, 13);
}

TEST(HdlIndexQuery, DocumentSymbolsIncludesParameters) {
    SampleProject project;
    const auto symbols = project.index.DocumentSymbols("Y:/proj/src/child.v");
    ASSERT_EQ(symbols.size(), 1u);
    bool has_parameter = false;
    for (const DocSymbol& child : symbols[0].children) {
        if (child.kind == 14 && child.name == "W") has_parameter = true;
    }
    EXPECT_TRUE(has_parameter);
}

TEST(HdlIndexQuery, SearchSymbolsFiltersByName) {
    SampleProject project;
    const auto all = project.index.SearchSymbols("");
    EXPECT_GT(all.size(), 3u);

    // workspace/symbol 的契约（PROTOCOL v7）：模块 + 端口 + 信号/参数。
    // **实例名不是 workspace symbol**（那是 callHierarchy / searchHierarchy 的活）。
    const auto clk = project.index.SearchSymbols("clk");
    ASSERT_EQ(clk.size(), 1u);
    EXPECT_EQ(clk[0].name, "clk");
    EXPECT_EQ(clk[0].container, "top");
    EXPECT_EQ(clk[0].kind, 8);
    EXPECT_EQ(project.index.SearchSymbols("u_child").size(), 0u);

    // 大小写不敏感
    EXPECT_EQ(project.index.SearchSymbols("CLK").size(), 1u);
    EXPECT_EQ(project.index.SearchSymbols("no_such_symbol_xyz").size(), 0u);
}

// P2-A：匹配是**子串**（不是前缀）—— 分页索引若按前缀二分会静默漏掉这些名字，
// 所以这条用例既是语义钉子，也是"为什么不做前缀二分"的回归护栏。
TEST(HdlIndexQuery, SearchSymbolsMatchesInfixNotJustPrefix) {
    SampleProject project;
    const auto lk = project.index.SearchSymbols("lk");  // "clk" 的中缀
    ASSERT_EQ(lk.size(), 1u);
    EXPECT_EQ(lk[0].name, "clk");
    EXPECT_EQ(lk[0].container, "top");

    const auto hild = project.index.SearchSymbols("hild");  // "child" 的中缀
    ASSERT_EQ(hild.size(), 1u);
    EXPECT_EQ(hild[0].name, "child");
    EXPECT_EQ(hild[0].kind, 2);

    // 大小写不敏感对中缀同样成立（ToLower 只折叠 ASCII，与旧实现一致）
    ASSERT_EQ(project.index.SearchSymbols("LK").size(), 1u);

    // query 为空 = 全部（顺序即全序：文件顺序 → 模块 → 模块/端口/信号/参数）
    const auto all = project.index.SearchSymbols("");
    ASSERT_GE(all.size(), 6u);
    EXPECT_EQ(all[0].name, "child");
    EXPECT_EQ(all[0].kind, 2);
    bool has_param = false;
    for (const fpgalsp::hdl::WorkspaceSymbol& symbol : all) {
        if (symbol.kind == 14 && symbol.name == "W") has_param = true;
    }
    EXPECT_TRUE(has_param) << "参数必须留在 workspace symbol 里（kind 14）";
}

// P2-A：分页窗口必须与全量结果**逐字段**一致（拼接无重不漏、顺序不变、total 是命中总数）
TEST(HdlIndexQuery, SearchSymbolsWindowMatchesFullList) {
    SampleProject project;
    const std::vector<std::string> queries = {"", "o", "lk", "q", "no_such_symbol_xyz"};

    for (const std::string& query : queries) {
        const auto full = project.index.SearchSymbols(query);

        std::size_t total = 0;
        const auto whole = project.index.SearchSymbols(query, 0, 0, &total);
        EXPECT_EQ(total, full.size()) << "query=" << query;
        ASSERT_EQ(whole.size(), full.size()) << "query=" << query;

        // 窗口切成 2 条一页，拼起来 == 全量
        std::vector<fpgalsp::hdl::WorkspaceSymbol> stitched;
        for (std::size_t offset = 0; offset < full.size() + 2; offset += 2) {
            std::size_t page_total = 12345;
            const auto page = project.index.SearchSymbols(query, offset, 2, &page_total);
            EXPECT_EQ(page_total, full.size()) << "query=" << query << " offset=" << offset;
            EXPECT_LE(page.size(), 2u) << "query=" << query << " offset=" << offset;
            stitched.insert(stitched.end(), page.begin(), page.end());
        }
        ASSERT_EQ(stitched.size(), full.size()) << "query=" << query;
        for (std::size_t i = 0; i < full.size(); ++i) {
            EXPECT_EQ(stitched[i].name, full[i].name) << "query=" << query << " i=" << i;
            EXPECT_EQ(stitched[i].container, full[i].container) << "query=" << query;
            EXPECT_EQ(stitched[i].detail, full[i].detail) << "query=" << query;
            EXPECT_EQ(stitched[i].kind, full[i].kind) << "query=" << query;
            EXPECT_EQ(stitched[i].path, full[i].path) << "query=" << query;
            EXPECT_EQ(stitched[i].span.line, full[i].span.line) << "query=" << query;
            EXPECT_EQ(stitched[i].span.start_character, full[i].span.start_character);
            EXPECT_EQ(stitched[i].span.end_character, full[i].span.end_character);
        }

        // offset 越界：空窗口，但 total 仍是命中总数（MCP 的 nextOffset 依赖它）
        std::size_t beyond = 0;
        EXPECT_TRUE(project.index.SearchSymbols(query, full.size() + 10, 5, &beyond).empty());
        EXPECT_EQ(beyond, full.size()) << "query=" << query;

        // limit 恰好等于剩余条数：取到结尾
        if (!full.empty()) {
            const auto tail = project.index.SearchSymbols(query, full.size() - 1, 1);
            ASSERT_EQ(tail.size(), 1u);
            EXPECT_EQ(tail[0].name, full.back().name);
        }
    }
}

// P2-A in-process 预算（打印实测值，不设阈值：CI 机器差异大；最终数字由 lead 构建后测）。
// 200 个模块 × (1 模块 + 9 端口 + 6 信号) ≈ 3200 个符号，与 audit/perf2 的 200 文件
// 工程同量级；query="w" 命中 1200 个（够大，能看出"只物化窗口"的差别）。
TEST(HdlIndexQuery, SearchSymbolsWindowIsCheap) {
    HdlIndex index;
    for (int i = 0; i < 200; ++i) {
        std::string text = "module m" + std::to_string(i) + " (input clk, ";
        for (int p = 0; p < 7; ++p) text += "input a" + std::to_string(p) + ", ";
        text += "output q);\n";
        for (int s = 0; s < 6; ++s) text += "  wire w" + std::to_string(s) + ";\n";
        text += "  assign w0 = clk;\nendmodule\n";
        const std::string path = "Y:/bench/m" + std::to_string(i) + ".v";
        index.AddFile(Parse(text, path.c_str()));
    }
    const auto full = index.SearchSymbols("w");
    ASSERT_GT(full.size(), 200u);

    // 旧路径的等价成本 = 全量物化；新路径 = 只物化窗口。
    const auto measure = [&index](const std::string& query, std::size_t offset,
                                  std::size_t limit) {
        constexpr int kIters = 50;
        double best = 1e9;
        for (int i = 0; i < kIters; ++i) {
            const auto start = std::chrono::steady_clock::now();
            const auto window = index.SearchSymbols(query, offset, limit);
            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - start)
                                  .count();
            best = std::min(best, ms);
            EXPECT_FALSE(window.empty());
        }
        return best;
    };

    std::size_t total = 0;
    const auto window = index.SearchSymbols("w", 0, 50, &total);
    EXPECT_EQ(window.size(), 50u);
    EXPECT_EQ(total, full.size());
    const auto window200 = index.SearchSymbols("w", 0, 200, &total);
    EXPECT_EQ(window200.size(), 200u);

    const double full_ms = measure("w", 0, 0);
    const double w50_ms = measure("w", 0, 50);
    const double w200_ms = measure("w", 0, 200);
    const auto empty_all = index.SearchSymbols("");
    EXPECT_GT(empty_all.size(), 200u);
    std::printf(
        "[bench] SearchSymbols(%zu symbols): full=%.3f ms  (0,50)=%.3f ms  (0,200)=%.3f ms\n"
        "        budget: limit=50 <= 1.5 ms, limit=200 <= 4 ms\n",
        full.size(), full_ms, w50_ms, w200_ms);
}

TEST(HdlIndexQuery, IterateInstances) {
    SampleProject project;
    const auto instances = project.index.AllInstances();
    ASSERT_EQ(instances.size(), 1u);
    EXPECT_EQ(instances[0]->module, "child");
    EXPECT_EQ(project.index.InstancesOfFile("Y:/proj/src/child.v").size(), 0u);
}

// -----------------------------------------------------------------------------
// 10b. 引用：端口名 / 内部 net 名（P2-2）
// -----------------------------------------------------------------------------
//  之前怀疑"引用只认 module/instance"，实测不是：References 是**文本级 token 搜索**，
//  光标处只要是标识符就能命中。这里把这条语义钉死 —— 端口 / 网 / 端口连接三种位置
//  都必须有引用，且 include_declaration 只影响"声明本身"那一条。

TEST(HdlIndexQuery, ReferencesOfPortNameAndIncludeDeclaration) {
    SampleProject project;
    // top.v 第 0 行第 17 列 = 端口 clk 的声明
    const std::vector<fpgalsp::hdl::Location> all =
        project.index.References("Y:/proj/src/top.v", 0, 17);
    ASSERT_GE(all.size(), 2u) << "端口名应该同时命中声明与使用（assign w = {8{clk}}）";
    int declarations = 0;
    for (const auto& location : all) {
        if (location.path == "Y:/proj/src/top.v" && location.line == 0 &&
            location.character == 17) {
            ++declarations;
        }
    }
    EXPECT_EQ(declarations, 1) << "声明本身也应算一条引用（include_declaration 默认 true）";

    const std::vector<fpgalsp::hdl::Location> uses =
        project.index.References("Y:/proj/src/top.v", 0, 17, /*include_declaration=*/false);
    EXPECT_EQ(uses.size() + 1, all.size()) << "false 只应掉掉声明那一条";
    for (const auto& location : uses) {
        EXPECT_FALSE(location.path == "Y:/proj/src/top.v" && location.line == 0 &&
                     location.character == 17)
            << "include_declaration=false 时声明不能出现在结果里";
    }
}

TEST(HdlIndexQuery, ReferencesOfInternalNetAndPortConnection) {
    SampleProject project;
    // top.v 第 1 行第 13 列 = `wire [7:0] w;` 的内部 net 声明
    const std::vector<fpgalsp::hdl::Location> all =
        project.index.References("Y:/proj/src/top.v", 1, 13);
    ASSERT_GE(all.size(), 3u) << "net 应命中 声明 + .d(w) 连接 + assign w";
    const std::vector<fpgalsp::hdl::Location> uses =
        project.index.References("Y:/proj/src/top.v", 1, 13, /*include_declaration=*/false);
    EXPECT_EQ(uses.size() + 1, all.size());

    // 光标停在例化的端口连接 `.d(` 上（端口连接位置），也要拿得到引用
    const ParsedFile* file = project.index.File("Y:/proj/src/top.v");
    ASSERT_NE(file, nullptr);
    const Instance* instance = FindInstance(*file, "u_child");
    ASSERT_NE(instance, nullptr);
    ASSERT_FALSE(instance->connection_ports.empty());
    const auto& position = instance->connection_ports[0].first;
    const std::vector<fpgalsp::hdl::Location> connection =
        project.index.References("Y:/proj/src/top.v", position.first, position.second);
    EXPECT_GE(connection.size(), 1u) << "端口连接位置必须能拿到引用";
}

// -----------------------------------------------------------------------------
// 10c. 引用：net 的 scope-aware 语义（wire 引用不得混入同名例化端口标签）
// -----------------------------------------------------------------------------
//  用户报的 bug：wire `oc_chk_lock` 的"转到引用"把例化里 `.oc_chk_lock(...)` 的
//  **端口标签**也算成引用 —— 那是被例化模块的端口名，与 wire 只是同名。
//  修法（HdlIndex::References）：kind=="signal" 时只在**同文件同 module** 范围内找
//  同名 token，并排除命名端口标签 / 端口·参数·实例·模块名等其它符号位置；
//  非 signal（module/port/parameter/instance）保持文本级。

namespace {

constexpr const char* kRefsPath = "Y:/proj/src/refs.v";

// module child 的端口名与 top 的 wire 同名；other_mod 里还有一个同名 wire。
constexpr const char* kRefsText =
    "module child(input oc_chk_lock, input other);\n"                     // 0
    "endmodule\n"                                                         // 1
    "module top(input a, input b);\n"                                     // 2
    "  wire oc_chk_lock;\n"                                               // 3
    "  assign oc_chk_lock = a & b;\n"                                     // 4
    "  child u0 (.oc_chk_lock(oc_chk_lock), .other(oc_chk_lock));\n"      // 5
    "endmodule\n"                                                         // 6
    "module other_mod(input c, input d);\n"                               // 7
    "  wire oc_chk_lock;\n"                                               // 8
    "  assign oc_chk_lock = c & d;\n"                                     // 9
    "endmodule\n";                                                        // 10

std::vector<std::pair<int, int>> TokenPositions(const ParsedFile& file,
                                                const std::string& name) {
    std::vector<std::pair<int, int>> out;
    for (const fpgalsp::hdl::Token& token : file.tokens) {
        if (token.kind != 'i' || token.text != name) continue;
        out.push_back({token.line, token.character});
    }
    return out;
}

// 所有例化命名端口标签的位置（instances[].connection_ports 的 key）
std::vector<std::pair<int, int>> PortLabelPositions(const ParsedFile& file) {
    std::vector<std::pair<int, int>> out;
    for (const Instance& instance : file.instances) {
        for (const auto& entry : instance.connection_ports) out.push_back(entry.first);
    }
    return out;
}

bool Contains(const std::vector<fpgalsp::hdl::Location>& locations, int line,
              int character) {
    for (const fpgalsp::hdl::Location& location : locations) {
        if (location.line == line && location.character == character) return true;
    }
    return false;
}

const ModuleInfo* ModuleInfoByName(const HdlIndex& index, std::string_view path,
                                   const std::string& name) {
    for (const ModuleInfo* info : index.ModulesOfFile(path)) {
        if (info->module.name == name) return info;
    }
    return nullptr;
}

}  // namespace

TEST(HdlIndexQuery, ReferencesOfWireExcludeInstancePortLabels) {
    HdlIndex index;
    index.AddFile(Parse(kRefsText, kRefsPath));
    const ParsedFile* file = index.File(kRefsPath);
    ASSERT_NE(file, nullptr);

    const ModuleInfo* top = ModuleInfoByName(index, kRefsPath, "top");
    ASSERT_NE(top, nullptr);
    const Signal* wire = FindSignal(*top, "oc_chk_lock");
    ASSERT_NE(wire, nullptr);

    // 例化里有两个命名端口标签（.oc_chk_lock / .other），其中与本 wire 同名的是前者
    const auto labels = PortLabelPositions(*file);
    ASSERT_EQ(labels.size(), 2u);
    const auto same_name = TokenPositions(*file, "oc_chk_lock");
    std::vector<std::pair<int, int>> same_name_labels;
    for (const auto& position : same_name) {
        if (std::find(labels.begin(), labels.end(), position) != labels.end()) {
            same_name_labels.push_back(position);
        }
    }
    ASSERT_EQ(same_name_labels.size(), 1u) << "例化里只有一个同名端口标签 .oc_chk_lock";
    const auto label = same_name_labels[0];

    // 例化行上另外两个同名 token 是 .oc_chk_lock(...) 与 .other(...) 的**实参**
    std::vector<std::pair<int, int>> instance_arguments;
    for (const auto& position : same_name) {
        if (position.first == label.first && position != label) {
            instance_arguments.push_back(position);
        }
    }
    ASSERT_EQ(instance_arguments.size(), 2u);

    // assign 行的 LHS（本行唯一同名 token）
    std::vector<std::pair<int, int>> assign_lhs;
    for (const auto& position : same_name) {
        if (position.first == wire->line + 1) assign_lhs.push_back(position);
    }
    ASSERT_EQ(assign_lhs.size(), 1u);

    const auto refs = index.References(kRefsPath, wire->line, wire->character);
    // 声明 + assign LHS + 两个实参；端口标签不算引用
    EXPECT_EQ(refs.size(), 4u);
    for (const auto& location : refs) EXPECT_EQ(location.path, kRefsPath);

    EXPECT_FALSE(Contains(refs, label.first, label.second))
        << "`.oc_chk_lock` 端口标签不是 wire 的引用（用户报的误报）";
    EXPECT_TRUE(Contains(refs, wire->line, wire->character)) << "include_declaration=true 含声明";
    EXPECT_TRUE(Contains(refs, assign_lhs[0].first, assign_lhs[0].second)) << "assign LHS";
    for (const auto& position : instance_arguments) {
        EXPECT_TRUE(Contains(refs, position.first, position.second))
            << "`.port(wire)` 的实参必须保留";
    }

    // include_declaration=false：只掉声明那一条，其余语义不变
    const auto uses = index.References(kRefsPath, wire->line, wire->character, false);
    EXPECT_EQ(uses.size(), 3u);
    EXPECT_FALSE(Contains(uses, wire->line, wire->character));
    EXPECT_FALSE(Contains(uses, label.first, label.second));
    for (const auto& position : instance_arguments) {
        EXPECT_TRUE(Contains(uses, position.first, position.second));
    }

    // 光标停在 assign 里的引用上 -> 与停在声明处等价（都解析成同一个 signal）
    const auto from_use =
        index.References(kRefsPath, assign_lhs[0].first, assign_lhs[0].second);
    EXPECT_EQ(from_use.size(), refs.size());
    const auto use_symbol =
        index.SymbolAt(kRefsPath, assign_lhs[0].first, assign_lhs[0].second);
    ASSERT_TRUE(use_symbol.has_value());
    EXPECT_EQ(use_symbol->kind, "signal");
}

TEST(HdlIndexQuery, ReferencesOfWireAreScopedToOwnModule) {
    HdlIndex index;
    index.AddFile(Parse(kRefsText, kRefsPath));
    const ParsedFile* file = index.File(kRefsPath);
    ASSERT_NE(file, nullptr);

    const ModuleInfo* top = ModuleInfoByName(index, kRefsPath, "top");
    const ModuleInfo* other = ModuleInfoByName(index, kRefsPath, "other_mod");
    ASSERT_NE(top, nullptr);
    ASSERT_NE(other, nullptr);
    const Signal* top_wire = FindSignal(*top, "oc_chk_lock");
    const Signal* other_wire = FindSignal(*other, "oc_chk_lock");
    ASSERT_NE(top_wire, nullptr);
    ASSERT_NE(other_wire, nullptr);

    const auto top_refs = index.References(kRefsPath, top_wire->line, top_wire->character);
    const auto other_refs =
        index.References(kRefsPath, other_wire->line, other_wire->character);

    // other_mod：只有声明 + assign LHS
    ASSERT_EQ(other_refs.size(), 2u);
    for (const auto& location : other_refs) {
        EXPECT_GE(location.line, other->module.keyword_line);
        EXPECT_LE(location.line, other->module.end_line);
    }
    // top 的引用里不能出现 other_mod 里的同名 token（跨 module 同名不混入）
    for (const auto& location : top_refs) {
        EXPECT_GE(location.line, top->module.keyword_line);
        EXPECT_LE(location.line, top->module.end_line);
        EXPECT_FALSE(location.line == other_wire->line &&
                     location.character == other_wire->character);
    }
    EXPECT_EQ(top_refs.size(), 4u);

    // 两个 module 的引用集合不相交
    for (const auto& location : other_refs) {
        EXPECT_FALSE(Contains(top_refs, location.line, location.character));
    }
}

TEST(HdlIndexQuery, ReferencesOfWireExcludeSameNameModuleAndInstancePositions) {
    // wire 与"例化的模块名/实例名"同名：那两个 token 也不是 wire 的引用
    HdlIndex index;
    const char* text =
        "module child(input d);\n"           // 0
        "endmodule\n"                         // 1
        "module top(input a);\n"              // 2
        "  wire child;\n"                     // 3
        "  assign child = a;\n"               // 4
        "  child child (.d(child));\n"        // 5
        "endmodule\n";                        // 6
    index.AddFile(Parse(text, kRefsPath));

    const ModuleInfo* top = ModuleInfoByName(index, kRefsPath, "top");
    ASSERT_NE(top, nullptr);
    const Signal* wire = FindSignal(*top, "child");
    ASSERT_NE(wire, nullptr);

    const ParsedFile* file = index.File(kRefsPath);
    ASSERT_NE(file, nullptr);
    const auto same_name = TokenPositions(*file, "child");
    std::vector<std::pair<int, int>> instance_line;
    for (const auto& position : same_name) {
        if (position.first == wire->line + 2) instance_line.push_back(position);
    }
    ASSERT_EQ(instance_line.size(), 3u) << "模块名 + 实例名 + .d(child) 实参";
    const auto modref = instance_line[0];
    const auto instance_name = instance_line[1];
    const auto argument = instance_line[2];

    const auto refs = index.References(kRefsPath, wire->line, wire->character);
    EXPECT_EQ(refs.size(), 3u) << "声明 + assign LHS + .d(child) 实参";
    EXPECT_FALSE(Contains(refs, modref.first, modref.second)) << "例化的模块名不是 wire 引用";
    EXPECT_FALSE(Contains(refs, instance_name.first, instance_name.second))
        << "实例名不是 wire 引用";
    EXPECT_TRUE(Contains(refs, argument.first, argument.second));
}

TEST(HdlIndexQuery, ReferencesOfInstancePortLabelKeepsTextLevel) {
    // 光标本来就停在 `.oc_chk_lock` 端口标签上：那是被例化模块 child 的端口符号，
    // 不是 net；保持旧的文本级语义（跨文件全量同名 token），不要误伤。
    HdlIndex index;
    index.AddFile(Parse(kRefsText, kRefsPath));
    const ParsedFile* file = index.File(kRefsPath);
    ASSERT_NE(file, nullptr);

    std::pair<int, int> label{};
    bool found = false;
    for (const Instance& instance : file->instances) {
        for (const auto& entry : instance.connection_ports) {
            if (entry.second == "oc_chk_lock") {
                label = entry.first;
                found = true;
            }
        }
    }
    ASSERT_TRUE(found) << "fixture 里应存在 .oc_chk_lock 端口标签";
    const auto symbol = index.SymbolAt(kRefsPath, label.first, label.second);
    ASSERT_TRUE(symbol.has_value());
    EXPECT_EQ(symbol->kind, "port");
    EXPECT_EQ(symbol->module, "child");

    const auto all = index.References(kRefsPath, label.first, label.second);
    // 只索引了一个文件，文本级 = 该文件里全部同名 token
    const auto same_name = TokenPositions(*file, "oc_chk_lock");
    ASSERT_EQ(all.size(), same_name.size()) << "端口标签上的引用保持文本级旧语义";
    for (const auto& position : same_name) {
        EXPECT_TRUE(Contains(all, position.first, position.second));
    }

    // include_declaration=false：只掉被例化模块的端口声明那一条
    const auto uses = index.References(kRefsPath, label.first, label.second, false);
    EXPECT_EQ(uses.size() + 1, all.size());
    EXPECT_FALSE(Contains(uses, symbol->span.line, symbol->span.start_character));
}

// -----------------------------------------------------------------------------
// 11. BuildIndex（工程级：缓冲区优先 / VHDL / IP）
// -----------------------------------------------------------------------------

namespace {

fpgalsp::project::Project MakeProject(const std::string& dir) {
    fpgalsp::project::Project project;
    project.dir = dir;
    project.al_path = dir + "/p.al";
    project.name = "p";
    project.family = "EF2";
    project.device = "EF2L45LG144B";
    project.top = "top";
    return project;
}

fpgalsp::project::AlFile MakeFile(const std::string& path,
                                 fpgalsp::project::FileKind kind) {
    fpgalsp::project::AlFile file;
    file.path = path;
    file.rel = path.substr(path.find_last_of('/') + 1);
    file.kind = kind;
    file.exists = true;
    file.enabled = true;
    file.excluded = false;
    return file;
}

}  // namespace

TEST(HdlBuild, BuffersOverrideDisk) {
    fpgalsp::project::Project project = MakeProject("Y:/build_test");
    project.files.push_back(MakeFile("Y:/build_test/a.v", fpgalsp::project::FileKind::Verilog));

    // loader 返回"未保存缓冲区"内容：模块名是 buffer_mod
    const auto load = [](const std::string&) -> std::optional<std::string> {
        return std::string("module buffer_mod; endmodule\n");
    };
    const auto result = BuildIndex(project, load);
    ASSERT_EQ(result.index.FileCount(), 1u);
    EXPECT_EQ(result.index.ModuleByName("buffer_mod") != nullptr, true);
    EXPECT_EQ(result.index.ModuleByName("disk_mod"), nullptr);
}

TEST(HdlBuild, MissingFileIsSkipped) {
    fpgalsp::project::Project project = MakeProject("Y:/build_test");
    project.files.push_back(MakeFile("Y:/build_test/a.v", fpgalsp::project::FileKind::Verilog));
    project.files.push_back(MakeFile("Y:/build_test/b.v", fpgalsp::project::FileKind::Verilog));
    const auto load = [](const std::string& path) -> std::optional<std::string> {
        if (path.find("a.v") != std::string::npos) {
            return std::string("module a; endmodule\n");
        }
        return std::nullopt;  // b.v 读不到
    };
    const auto result = BuildIndex(project, load);
    EXPECT_EQ(result.index.FileCount(), 1u);
    EXPECT_EQ(result.index.ModuleByName("a") != nullptr, true);
}

TEST(HdlBuild, VhdlEntityIsSyntheticModule) {
    fpgalsp::project::Project project = MakeProject("Y:/build_test");
    project.files.push_back(MakeFile("Y:/build_test/a.v", fpgalsp::project::FileKind::Verilog));
    project.files.push_back(MakeFile("Y:/build_test/e.vhd", fpgalsp::project::FileKind::Vhdl));
    const auto load = [](const std::string& path) -> std::optional<std::string> {
        if (path.find(".vhd") != std::string::npos) {
            return std::string("entity vhdl_mod is\nend vhdl_mod;\n");
        }
        return std::string("module verilog_mod; endmodule\n");
    };
    const auto result = BuildIndex(project, load);
    ASSERT_EQ(result.index.FileCount(), 2u);
    const Module* module = result.index.ModuleByName("vhdl_mod");
    ASSERT_NE(module, nullptr);
    EXPECT_TRUE(module->vhdl);
    EXPECT_TRUE(module->ports.empty());
}

TEST(HdlBuild, IpGeneratedVerilogIsIndexedFromDisk) {
    // 真建目录：.ipc 旁边放同名 .v 和 .vhd，验证只有 .v 进 Verilog 索引（P-1）
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "fpgalsp_hdl_ip_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "al_ip", ec);
    {
        std::ofstream(root / "al_ip" / "pll.ipc") << "<ipc/>\n";
        std::ofstream(root / "al_ip" / "pll.v") << "module pll_gen; endmodule\n";
        std::ofstream(root / "al_ip" / "pll.vhd") << "entity pll_ent is\nend;\n";
    }

    fpgalsp::project::Project project = MakeProject(root.generic_string());
    project.files.push_back(
        MakeFile((root / "al_ip" / "pll.ipc").generic_string(), fpgalsp::project::FileKind::Ip));

    const auto generated = fpgalsp::hdl::IpGeneratedHdlFor(project);
    EXPECT_EQ(generated.verilog.size(), 1u);
    EXPECT_EQ(generated.vhdl.size(), 1u);

    const auto verilog_files = fpgalsp::hdl::VerilogFilesFor(project);
    ASSERT_EQ(verilog_files.size(), 1u);
    EXPECT_NE(verilog_files[0].find("pll.v"), std::string::npos);
    EXPECT_EQ(verilog_files[0].find(".vhd"), std::string::npos);

    const auto vhdl_files = fpgalsp::hdl::VhdlFilesFor(project);
    ASSERT_EQ(vhdl_files.size(), 1u);
    EXPECT_NE(vhdl_files[0].find("pll.vhd"), std::string::npos);

    fs::remove_all(root, ec);
}

TEST(HdlBuild, ParseCacheIsConsulted) {
    fpgalsp::project::Project project = MakeProject("Y:/build_test");
    project.files.push_back(MakeFile("Y:/build_test/a.v", fpgalsp::project::FileKind::Verilog));
    const auto load = [](const std::string&) -> std::optional<std::string> {
        return std::string("module a; endmodule\n");
    };
    ParseCache cache;
    BuildOptions options;
    options.cache = &cache;
    BuildIndex(project, load, options);
    BuildIndex(project, load, options);
    EXPECT_EQ(cache.hits(), 1u);
    EXPECT_EQ(cache.misses(), 1u);
    EXPECT_EQ(cache.size(), 1u);
}

TEST(HdlBuild, TruncationByFileBudget) {
    fpgalsp::project::Project project = MakeProject("Y:/build_test");
    for (int i = 0; i < 5; ++i) {
        project.files.push_back(MakeFile("Y:/build_test/f" + std::to_string(i) + ".v",
                                         fpgalsp::project::FileKind::Verilog));
    }
    const auto load = [](const std::string& path) -> std::optional<std::string> {
        return "module m_" + path + "; endmodule\n";
    };
    BuildOptions options;
    options.max_files = 2;
    const auto result = BuildIndex(project, load, options);
    EXPECT_TRUE(result.truncated);
    EXPECT_EQ(result.files_indexed, 2u);
}

// -----------------------------------------------------------------------------
// 12. 与真实语料对照（gate-corpus/corpus_alpha 的 top.v）
// -----------------------------------------------------------------------------

TEST(HdlCorpus, RealTopModule) {
    const std::string path = std::string(FPGA_LSP_SOURCE_DIR) +
                             "/tests/gate-corpus/corpus_alpha/src/top.v";
    std::ifstream stream(path, std::ios::binary);
    ASSERT_TRUE(stream.good()) << "语料缺失: " << path;
    const std::string text((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
    const ParsedFile file = ParseVerilog(path, text);
    ASSERT_EQ(file.modules.size(), 1u);
    const Module& module = file.modules[0].module;
    EXPECT_EQ(module.name, "top");
    EXPECT_EQ(module.ports.size(), 25u);
    EXPECT_TRUE(module.ansi_ports);
    // top.v 的例化：pll_gen / sys_rstn / trans / ad_7924 / ON_DEL x7 / err_lock x2 ...
    EXPECT_GE(file.instances.size(), 10u);
    bool has_pll = false;
    for (const Instance& instance : file.instances) {
        if (instance.module == "pll_gen") {
            has_pll = true;
            EXPECT_EQ(instance.line, 65);  // 0-based
        }
    }
    EXPECT_TRUE(has_pll);
}

TEST(HdlCorpus, RealTopModuleProducesSymbols) {
    const std::string path = std::string(FPGA_LSP_SOURCE_DIR) +
                             "/tests/gate-corpus/corpus_alpha/src/top.v";
    std::ifstream stream(path, std::ios::binary);
    ASSERT_TRUE(stream.good());
    const std::string text((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
    HdlIndex index;
    index.AddFile(ParseVerilog(path, text));
    const auto symbols = index.DocumentSymbols(path);
    ASSERT_EQ(symbols.size(), 1u);
    EXPECT_EQ(symbols[0].name, "top");
    EXPECT_GE(symbols[0].children.size(), 25u);
}

// -----------------------------------------------------------------------------
// 13. 与 Python 参考实现逐字段对比的"导出"入口
// -----------------------------------------------------------------------------
//  为什么做成测试而不是单独的 CLI 工具：
//    src/tools/ 不在本任务写作用域；而 gtest 二进制本来就链着 hdl 库。
//  用法（tests/e2e/compare_hdl_python.py 会这样调）：
//    FPGALSP_HDL_DUMP=<out.json> FPGALSP_HDL_DUMP_DIR=<dir> unit_hdl_index --gtest_filter=HdlDump.*
//  未设置环境变量时自动 SKIP，不影响正常 ctest。
namespace {

Json DumpFile(const std::string& path, const std::string& text) {
    const ParsedFile file = ParseVerilog(path, text);
    Json out = Json::object();
    out["path"] = path;
    Json modules = Json::array();
    for (const ModuleInfo& info : file.modules) {
        const Module& module = info.module;
        Json entry = Json::object();
        entry["name"] = module.name;
        entry["line"] = module.line;
        entry["character"] = module.character;
        entry["end_line"] = module.end_line;
        entry["ansi_ports"] = module.ansi_ports;
        Json ports = Json::array();
        for (const Port& port : module.ports) {
            Json p = Json::object();
            p["name"] = port.name;
            p["direction"] = port.direction;
            p["msb"] = port.msb.has_value() ? Json(*port.msb) : Json(nullptr);
            p["lsb"] = port.lsb.has_value() ? Json(*port.lsb) : Json(nullptr);
            p["width"] = port.width;
            p["line"] = port.line;
            p["character"] = port.character;
            ports.push_back(std::move(p));
        }
        entry["ports"] = std::move(ports);
        entry["parameters"] = module.parameters;
        modules.push_back(std::move(entry));
    }
    out["modules"] = std::move(modules);
    Json instances = Json::array();
    for (const Instance& instance : file.instances) {
        Json entry = Json::object();
        entry["module"] = instance.module;
        entry["name"] = instance.name;
        entry["line"] = instance.line;
        entry["character"] = instance.character;
        instances.push_back(std::move(entry));
    }
    out["instances"] = std::move(instances);
    return out;
}

}  // namespace

TEST(HdlDump, DumpDirectoryJson) {
    const char* out_path = std::getenv("FPGALSP_HDL_DUMP");
    const char* dir_path = std::getenv("FPGALSP_HDL_DUMP_DIR");
    if (out_path == nullptr || dir_path == nullptr) {
        GTEST_SKIP() << "FPGALSP_HDL_DUMP / FPGALSP_HDL_DUMP_DIR 未设置（正常的 ctest 运行不需要导出）";
    }
    namespace fs = std::filesystem;
    std::vector<std::string> files;
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(dir_path, ec)) {
        if (!entry.is_regular_file()) continue;
        const std::string path = entry.path().generic_string();
        if (!fpgalsp::hdl::IsVerilogPath(path)) continue;
        files.push_back(path);
    }
    std::sort(files.begin(), files.end());

    Json root = Json::object();
    Json list = Json::array();
    for (const std::string& path : files) {
        std::ifstream stream(path, std::ios::binary);
        if (!stream.good()) continue;
        const std::string text((std::istreambuf_iterator<char>(stream)),
                               std::istreambuf_iterator<char>());
        list.push_back(DumpFile(path, text));
    }
    root["files"] = std::move(list);
    std::ofstream out(out_path, std::ios::binary);
    ASSERT_TRUE(out.good());
    out << root.dump(1, ' ', false, Json::error_handler_t::replace);
}

}  // namespace