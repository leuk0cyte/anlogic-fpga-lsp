// =============================================================================
//  tests/unit/adc_test.cpp —— ADC 引脚约束解析 / 诊断 / 覆盖率
// =============================================================================
//  语料与期望值来源：
//    · tests/gate-corpus/corpus_alpha/constrains/pin.adc（真实工程副本，47 条约束）
//    · 金标准（golden）由 Python 参考实现生成：
//          python -c "from fpgalsp import adc; ..."  见文件末尾注释
//      金标准逐字段覆盖 line/character/port/base/bit/attrs/三类 span，
//      任何一处与 Python 不一致都会在这里变红。
//    · 合成用例照搬 Y:\fpga-lsp\tests\test_adc.py（Python 版自己的单测），
//      保证"Python 认为对的行为"在 C++ 里也成立。
//
//  ⚠ 本文件里有几条测试是**故意钉死 Python 缺陷**的（P-8/P-9/P-10），
//    注释里写了机理。修掉 Python 缺陷时它们会变红 —— 那是预期的。
// =============================================================================

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <set>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "constraints/adc.h"

namespace {

using fpgalsp::constraints::Json;
using fpgalsp::constraints::Port;
using fpgalsp::constraints::Span;
using fpgalsp::constraints::adc::Entry;
using fpgalsp::lsp::Diagnostic;

constexpr const char* kRealAdcPath =
    FPGA_LSP_SOURCE_DIR "/tests/gate-corpus/corpus_alpha/constrains/pin.adc";

std::optional<std::string> ReadTextFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

// 环境变量读取（MSVC 把 getenv 标为不安全，/W4 + /WX 会失败 -> 用 _dupenv_s）
std::optional<std::string> ReadEnv(const char* name) {
#if defined(_MSC_VER)
    char* buffer = nullptr;
    size_t size = 0;
    if (_dupenv_s(&buffer, &size, name) != 0 || buffer == nullptr) return std::nullopt;
    std::string value(buffer);
    std::free(buffer);
    if (value.empty()) return std::nullopt;
    return value;
#else
    const char* raw = std::getenv(name);
    if (raw == nullptr || *raw == '\0') return std::nullopt;
    return std::string(raw);
#endif
}

// ---- 端口构造（与 Python test_adc.py 的 make_port 同义）----

Port MakePort(std::string name, int width = 1, int line = 0) {
    Port port;
    port.name = std::move(name);
    port.direction = "input";
    port.width = width;
    if (width > 1) {
        port.msb = width - 1;
        port.lsb = 0;
    }
    port.line = line;
    port.character = 9;
    port.file = "top.v";
    return port;
}

// CorpusAlpha/src/top.v 的顶层端口（25 个端口 / 47 bit，声明行号与真实文件一致）
std::vector<Port> TopPorts() {
    return {
        MakePort("clk_input", 1, 15),   MakePort("w", 3, 16),
        MakePort("GATE_LED", 1, 18),    MakePort("GATE_ERR", 1, 19),
        MakePort("ERR_LED", 1, 20),     MakePort("ad_cs", 1, 23),
        MakePort("ad_sck", 1, 24),      MakePort("ad_dout", 1, 25),
        MakePort("ad_din", 1, 26),      MakePort("pwmG", 6, 29),
        MakePort("IGBT_in", 6, 30),     MakePort("ERR_in", 6, 31),
        MakePort("PowerDown33", 1, 34), MakePort("PowerDown150", 1, 35),
        MakePort("OH_CHK", 1, 37),      MakePort("DCOV_CHK", 1, 38),
        MakePort("SUPSTS", 1, 40),      MakePort("ERRCLR", 1, 41),
        MakePort("GR", 1, 42),          MakePort("sin", 1, 45),
        MakePort("sout", 1, 46),        MakePort("fan_i", 2, 48),
        MakePort("fan_o", 2, 49),       MakePort("relay", 4, 51),
        MakePort("test_led", 1, 52),
    };
}

std::vector<Port> FivePorts() {
    return {MakePort("clk_input", 1, 0), MakePort("w", 3, 1), MakePort("pwmG", 6, 2),
            MakePort("relay", 4, 10),    MakePort("sin", 1, 3)};
}

// ---- .adc 行构造 ----

struct LineAttrs {
    std::optional<std::string> location = std::string("P1");
    std::optional<std::string> iostandard = std::string("LVCMOS33");
    std::optional<std::string> pulltype = std::string("NONE");
    std::string extra;
};

std::string AdcLine(const std::string& port, LineAttrs attrs = {}) {
    std::string body;
    if (attrs.location) body += "LOCATION = " + *attrs.location + "; ";
    if (attrs.iostandard) body += "IOSTANDARD = " + *attrs.iostandard + "; ";
    if (attrs.pulltype) body += "PULLTYPE = " + *attrs.pulltype + "; ";
    body += attrs.extra;
    return "set_pin_assignment\t{ " + port + " }\t{ " + body + "}";
}

std::string JoinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i != 0) out.push_back('\n');
        out += lines[i];
    }
    return out;
}

std::vector<Diagnostic> ByCode(const std::vector<Diagnostic>& diagnostics,
                               std::string_view code) {
    std::vector<Diagnostic> out;
    for (const Diagnostic& diagnostic : diagnostics) {
        if (diagnostic.code == code) out.push_back(diagnostic);
    }
    return out;
}

// 设备引脚表夹具（形状与 td.py 的 device dict 一致）
Json DeviceFixture() {
    Json device = Json::object();
    device["device"] = "EF2L45LG144B";
    device["package"] = "ef2l45lg144b";
    device["pins"] = Json::object();
    device["pins"]["p87"] = Json{{"pe", "p87"}, {"location", "pt35a"}, {"type", "fio"},
                                 {"int_bank", 2}};
    device["pins"]["p76"] = Json{{"pe", "p76"}, {"location", "pt32a"}, {"type", "fio"},
                                 {"int_bank", 2}};
    device["dedicated"] = Json::array();
    device["dedicated"].push_back(Json{{"pe", "p119"}, {"role", "programn"}});
    return device;
}

// 把一条 Entry 还原成与 golden 相同的 "|" 拼接行，便于逐字段对比与定位差异
std::string RowOf(const Entry& entry) {
    const auto span_text = [&entry](const char* key) -> std::string {
        const auto it = entry.attr_spans.find(key);
        if (it == entry.attr_spans.end()) return "-";
        return std::to_string(it->second.start) + ":" + std::to_string(it->second.end);
    };
    const auto attr_text = [&entry](const char* key) -> std::string {
        const auto it = entry.attrs.find(key);
        return it == entry.attrs.end() ? std::string() : it->second;
    };
    std::string row;
    row += std::to_string(entry.line);
    row += "|" + std::to_string(entry.character);
    row += "|" + entry.port;
    row += "|" + entry.base;
    row += "|" + (entry.bit.has_value() ? std::to_string(*entry.bit) : std::string("-"));
    row += "|" + attr_text("LOCATION");
    row += "|" + attr_text("IOSTANDARD");
    row += "|" + attr_text("PULLTYPE");
    row += "|" + std::to_string(entry.port_span.start) + ":" +
           std::to_string(entry.port_span.end);
    row += "|" + span_text("LOCATION");
    row += "|" + span_text("IOSTANDARD");
    row += "|" + span_text("PULLTYPE");
    row += "|" + std::to_string(entry.command_span.start) + ":" +
           std::to_string(entry.command_span.end);
    return row;
}

// 金标准：Python 参考实现（adc.py）对真实语料的解析结果，逐字段
//   line|character|port|base|bit|LOCATION|IOSTANDARD|PULLTYPE|
//   port_span|LOCATION span|IOSTANDARD span|PULLTYPE span|command_span
// 重新生成方式见文件末尾注释。
constexpr const char* kRealGolden = R"GOLDEN(0|0|clk_input|clk_input|-|P87|LVCMOS33|NONE|21:30|46:49|64:72|85:89|0:18
1|0|w[0]|w|0|P76|LVCMOS33|NONE|21:25|41:44|59:67|80:84|0:18
2|0|w[1]|w|1|P77|LVCMOS33|NONE|21:25|41:44|59:67|80:84|0:18
3|0|w[2]|w|2|P78|LVCMOS33|NONE|21:25|41:44|59:67|80:84|0:18
4|0|GATE_LED|GATE_LED|-|P35|LVCMOS33|NONE|21:29|45:48|63:71|84:88|0:18
5|0|ERR_LED|ERR_LED|-|P82|LVCMOS33|NONE|21:28|44:47|62:70|83:87|0:18
6|0|ad_cs|ad_cs|-|P68|LVCMOS33|NONE|21:26|42:45|60:68|81:85|0:18
7|0|ad_sck|ad_sck|-|P70|LVCMOS33|NONE|21:27|43:46|61:69|82:86|0:18
8|0|ad_dout|ad_dout|-|P71|LVCMOS33|NONE|21:28|44:47|62:70|83:87|0:18
9|0|ad_din|ad_din|-|P69|LVCMOS33|NONE|21:27|43:46|61:69|82:86|0:18
10|0|pwmG[0]|pwmG|0|P15|LVCMOS33|NONE|21:28|44:47|62:70|83:87|0:18
11|0|pwmG[1]|pwmG|1|P12|LVCMOS33|NONE|21:28|44:47|62:70|83:87|0:18
12|0|pwmG[2]|pwmG|2|P14|LVCMOS33|NONE|21:28|44:47|62:70|83:87|0:18
13|0|pwmG[3]|pwmG|3|P11|LVCMOS33|NONE|21:28|44:47|62:70|83:87|0:18
14|0|pwmG[4]|pwmG|4|P13|LVCMOS33|NONE|21:28|44:47|62:70|83:87|0:18
15|0|pwmG[5]|pwmG|5|P10|LVCMOS33|NONE|21:28|44:47|62:70|83:87|0:18
16|0|IGBT_in[0]|IGBT_in|0|P1|LVCMOS33|NONE|21:31|47:49|64:72|85:89|0:18
17|0|IGBT_in[1]|IGBT_in|1|P5|LVCMOS33|NONE|21:31|47:49|64:72|85:89|0:18
18|0|IGBT_in[2]|IGBT_in|2|P3|LVCMOS33|NONE|21:31|47:49|64:72|85:89|0:18
19|0|IGBT_in[3]|IGBT_in|3|P23|LVCMOS33|NONE|21:31|47:50|65:73|86:90|0:18
20|0|IGBT_in[4]|IGBT_in|4|P22|LVCMOS33|NONE|21:31|47:50|65:73|86:90|0:18
21|0|IGBT_in[5]|IGBT_in|5|P26|LVCMOS33|NONE|21:31|47:50|65:73|86:90|0:18
22|0|ERR_in[0]|ERR_in|0|P4|LVCMOS33|NONE|21:30|46:48|63:71|84:88|0:18
23|0|ERR_in[1]|ERR_in|1|P2|LVCMOS33|NONE|21:30|46:48|63:71|84:88|0:18
24|0|ERR_in[2]|ERR_in|2|P6|LVCMOS33|NONE|21:30|46:48|63:71|84:88|0:18
25|0|ERR_in[3]|ERR_in|3|P24|LVCMOS33|NONE|21:30|46:49|64:72|85:89|0:18
26|0|ERR_in[4]|ERR_in|4|P21|LVCMOS33|NONE|21:30|46:49|64:72|85:89|0:18
27|0|ERR_in[5]|ERR_in|5|P25|LVCMOS33|NONE|21:30|46:49|64:72|85:89|0:18
30|0|PowerDown33|PowerDown33|-|P28|LVCMOS33|NONE|21:32|48:51|66:74|87:91|0:18
31|0|PowerDown150|PowerDown150|-|P39|LVCMOS33|NONE|21:33|49:52|67:75|88:92|0:18
32|0|OH_CHK|OH_CHK|-|P38|LVCMOS33|NONE|21:27|43:46|61:69|82:86|0:18
33|0|GATE_ERR|GATE_ERR|-|P40|LVCMOS33|NONE|21:29|45:48|63:71|84:88|0:18
34|0|DCOV_CHK|DCOV_CHK|-|P54|LVCMOS33|NONE|21:29|45:48|63:71|84:88|0:18
35|0|SUPSTS|SUPSTS|-|P31|LVCMOS33|NONE|21:27|43:46|61:69|82:86|0:18
36|0|ERRCLR|ERRCLR|-|P32|LVCMOS33|NONE|21:27|43:46|61:69|82:86|0:18
37|0|GR|GR|-|P34|LVCMOS33|NONE|21:23|39:42|57:65|78:82|0:18
38|0|sin|sin|-|P73|LVCMOS33|NONE|21:24|40:43|58:66|79:83|0:18
39|0|sout|sout|-|P74|LVCMOS33|NONE|21:25|41:44|59:67|80:84|0:18
42|0|test_led|test_led|-|P81|LVCMOS33|NONE|21:29|45:48|63:71|84:88|0:18
43|0|fan_i[0]|fan_i|0|P142|LVCMOS33|NONE|21:29|45:49|64:72|85:89|0:18
44|0|fan_i[1]|fan_i|1|P139|LVCMOS33|NONE|21:29|45:49|64:72|85:89|0:18
45|0|fan_o[0]|fan_o|0|P141|LVCMOS33|NONE|21:29|45:49|64:72|85:89|0:18
46|0|fan_o[1]|fan_o|1|P140|LVCMOS33|NONE|21:29|45:49|64:72|85:89|0:18
48|1|relay[0]|relay|0|P41|LVCMOS33|PULLDOWN|23:31|50:53|68:76|89:97|1:19
49|0|relay[1]|relay|1|P44|LVCMOS33|PULLDOWN|22:30|49:52|67:75|88:96|0:18
50|0|relay[2]|relay|2|P42|LVCMOS33|PULLDOWN|22:30|49:52|67:75|88:96|0:18
51|0|relay[3]|relay|3|P43|LVCMOS33|PULLDOWN|22:30|49:52|67:75|88:96|0:18)GOLDEN";


}  // namespace

// =============================================================================
//  解析
// =============================================================================

TEST(AdcParse, BasicEntryFieldsAndSpans) {
    const std::string text =
        "# a comment\n"
        "// another comment\n"
        "\n"
        "set_pin_assignment\t{ w[0] }\t{ LOCATION = P76; IOSTANDARD = LVCMOS33; PULLTYPE = NONE; }\n";
    const std::vector<Entry> entries = fpgalsp::constraints::adc::Parse(text);
    ASSERT_EQ(entries.size(), 1u);
    const Entry& entry = entries.front();
    EXPECT_EQ(entry.line, 3);
    EXPECT_EQ(entry.character, 0);
    EXPECT_EQ(entry.command, "set_pin_assignment");
    EXPECT_EQ(entry.port, "w[0]");
    EXPECT_EQ(entry.base, "w");
    ASSERT_TRUE(entry.bit.has_value());
    EXPECT_EQ(*entry.bit, 0);
    ASSERT_EQ(entry.attrs.size(), 3u);
    EXPECT_EQ(entry.attrs.at("LOCATION"), "P76");
    EXPECT_EQ(entry.attrs.at("IOSTANDARD"), "LVCMOS33");
    EXPECT_EQ(entry.attrs.at("PULLTYPE"), "NONE");

    const size_t port_at = entry.raw.find("w[0]");
    EXPECT_EQ(entry.port_span.start, static_cast<int>(port_at));
    EXPECT_EQ(entry.port_span.end, static_cast<int>(port_at + 4));
    const size_t location_at = entry.raw.find("P76");
    EXPECT_EQ(entry.attr_spans.at("LOCATION").start, static_cast<int>(location_at));
    EXPECT_EQ(entry.attr_spans.at("LOCATION").end, static_cast<int>(location_at + 3));
}

TEST(AdcParse, PaddingAndScalarPort) {
    const std::string text =
        " set_pin_assignment  {  clamp  }  { LOCATION = P87; IOSTANDARD = LVCMOS33; }\n"
        "set_pin_assignment { di[0] }\t{ LOCATION = P5; IOSTANDARD = LVCMOS33; }\n";
    const std::vector<Entry> entries = fpgalsp::constraints::adc::Parse(text);
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].port, "clamp");
    EXPECT_EQ(entries[0].base, "clamp");
    EXPECT_FALSE(entries[0].bit.has_value());
    EXPECT_EQ(entries[0].character, 1);
    EXPECT_EQ(entries[0].attrs.size(), 2u);  // 没有 PULLTYPE
    EXPECT_EQ(entries[1].port, "di[0]");
    ASSERT_TRUE(entries[1].bit.has_value());
    EXPECT_EQ(*entries[1].bit, 0);
}

TEST(AdcParse, OnlyFirstTwoBraceGroupsAreUsed) {
    // 第三个 {} 组不参与解析（Python 只看 groups[0] 与 groups[1]）
    const std::string text =
        "set_pin_assignment\t{ w[1] }\t{ LOCATION = P77; }\t{ IOSTANDARD = LVCMOS18; }\n";
    const std::vector<Entry> entries = fpgalsp::constraints::adc::Parse(text);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].port, "w[1]");
    EXPECT_EQ(entries[0].attrs.at("LOCATION"), "P77");
    EXPECT_EQ(entries[0].attrs.count("IOSTANDARD"), 0u);
}

TEST(AdcParse, SingleGroupWithEqualsIsAttrGroup) {
    // 只有一个 {} 组且含 '=' -> 当属性组，端口为空
    const std::vector<Entry> entries =
        fpgalsp::constraints::adc::Parse("set_pin_assignment { LOCATION = P1; }");
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_TRUE(entries[0].port.empty());
    EXPECT_EQ(entries[0].attrs.at("LOCATION"), "P1");
}

TEST(AdcParse, SingleGroupWithoutEqualsIsPortGroup) {
    const std::vector<Entry> entries = fpgalsp::constraints::adc::Parse("set_ippin { clk }");
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].port, "clk");
    EXPECT_TRUE(entries[0].attrs.empty());
}

TEST(AdcParse, NestedBracesAreTreatedAsOneGroup) {
    const std::string text =
        "set_pin_assignment { w[0] } { LOCATION = P1; } { nested { x } }\n";
    const std::vector<Entry> entries = fpgalsp::constraints::adc::Parse(text);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].port, "w[0]");
    EXPECT_EQ(entries[0].attrs.at("LOCATION"), "P1");
}

TEST(AdcParse, LastAttributeAssignmentWins) {
    const std::vector<Entry> entries = fpgalsp::constraints::adc::Parse(
        "set_pin_assignment { w[0] } { LOCATION = P76; LOCATION = P77; }");
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].attrs.at("LOCATION"), "P77");
    ASSERT_EQ(entries[0].attr_items.size(), 2u);  // 两个 item 都保留，供 ADC-ATTR 使用
}

TEST(AdcParse, AttributeWithoutTrailingSemicolon) {
    const std::vector<Entry> entries = fpgalsp::constraints::adc::Parse(
        "set_pin_assignment { w[0] } { LOCATION = P76; IOSTANDARD = LVCMOS33 }");
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].attrs.at("IOSTANDARD"), "LVCMOS33");
}

TEST(AdcParse, LowercaseAttributeKeyIsUppercased) {
    const std::vector<Entry> entries = fpgalsp::constraints::adc::Parse(
        "set_pin_assignment { w[0] } { location = P76; iostandard = LVCMOS33; }");
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].attrs.at("LOCATION"), "P76");
    EXPECT_EQ(entries[0].attr_items.at(0).key, "location");  // 原文保留供 hover
}

TEST(AdcParse, EmptyAttributeValueYieldsEmptyStringAndFlatSpan) {
    // `LOCATION = ;` -> 值为空串，span 退化成 start == end。
    // 诊断侧据此走 USR-8056（缺 LOCATION），不会把这个空 span 画成波浪线。
    const std::vector<Entry> entries = fpgalsp::constraints::adc::Parse(
        "set_pin_assignment { w[0] } { LOCATION = ; IOSTANDARD = LVCMOS33; }");
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].attrs.at("LOCATION"), "");
    EXPECT_EQ(entries[0].attr_spans.at("LOCATION").start,
              entries[0].attr_spans.at("LOCATION").end);
}

TEST(AdcParse, CrlfLineEndings) {
    const std::vector<Entry> entries = fpgalsp::constraints::adc::Parse(
        "set_pin_assignment\t{ w[0] }\t{ LOCATION = P1; }\r\n"
        "set_pin_assignment\t{ w[1] }\t{ LOCATION = P2; }\r\n");
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[1].line, 1);
    EXPECT_EQ(entries[1].attrs.at("LOCATION"), "P2");
}

TEST(AdcParse, TrailingLineCommentIsIgnored) {
    const std::vector<Entry> entries = fpgalsp::constraints::adc::Parse(
        "set_pin_assignment { w[0] } { LOCATION = P1; } // pin for w0\n");
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].attrs.at("LOCATION"), "P1");
}

TEST(AdcParse, EmptyAndCommentOnlyText) {
    EXPECT_TRUE(fpgalsp::constraints::adc::Parse("").empty());
    EXPECT_TRUE(fpgalsp::constraints::adc::Parse("\n\n   \n").empty());
    EXPECT_TRUE(fpgalsp::constraints::adc::Parse("# only\n// comments\n").empty());
}

TEST(AdcParse, GarbageNeverThrows) {
    const std::vector<std::string> inputs = {
        "",
        "set_pin_assignment",
        "set_pin_assignment {",
        "set_pin_assignment { } { }",
        "set_pin_assignment { w[0] }",
        "}}}",
        "garbage\t\tstuff",
        "{ LOCATION = P1; }",
        std::string("\x00\xEF\xBF\xBD", 4),
        "set_pin_assignment {{{{{",
        "set_pin_assignment {}{}{}{}",
        std::string(4096, '{'),
    };
    for (const std::string& input : inputs) {
        const auto entries = fpgalsp::constraints::adc::Parse(input);
        // 只要求"能返回、不崩、JSON 可序列化"
        const Json json = fpgalsp::constraints::adc::ToJson(entries);
        EXPECT_TRUE(json.is_array());
    }
}

TEST(AdcParse, PortWithSpacesInsideBraces) {
    const std::vector<Entry> entries =
        fpgalsp::constraints::adc::Parse("set_pin_assignment {   w[3]   } { LOCATION = P1; }");
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].port, "w[3]");
    EXPECT_EQ(entries[0].base, "w");
    ASSERT_TRUE(entries[0].bit.has_value());
    EXPECT_EQ(*entries[0].bit, 3);
    // span 指向去空白后的真实位置
    EXPECT_EQ(entries[0].raw.substr(static_cast<size_t>(entries[0].port_span.start), 4), "w[3]");
}

TEST(AdcParse, VhdlStylePortNameWithUnderscores) {
    // VHDL 端口名在 .adc 里同样只能出现基名 + 可选位下标
    const std::vector<Entry> entries = fpgalsp::constraints::adc::Parse(
        "set_pin_assignment { clk_in_p } { LOCATION = P1; IOSTANDARD = LVCMOS33; }");
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].base, "clk_in_p");
    EXPECT_FALSE(entries[0].bit.has_value());
}

TEST(AdcParse, RealCorpusMatchesPythonGolden) {
    const std::optional<std::string> text = ReadTextFile(kRealAdcPath);
    ASSERT_TRUE(text.has_value()) << "缺少语料文件 " << kRealAdcPath;
    const std::vector<Entry> entries = fpgalsp::constraints::adc::Parse(*text);

    std::vector<std::string> golden;
    {
        std::istringstream stream(kRealGolden);
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) golden.push_back(line);
        }
    }
    ASSERT_EQ(entries.size(), golden.size())
        << "条数与 Python 版不一致（Python 解析出 " << golden.size() << " 条）";
    for (size_t index = 0; index < entries.size(); ++index) {
        SCOPED_TRACE("entry #" + std::to_string(index));
        EXPECT_EQ(RowOf(entries[index]), golden[index]);
    }
}

TEST(AdcParse, RealCorpusEntryCounts) {
    const std::optional<std::string> text = ReadTextFile(kRealAdcPath);
    ASSERT_TRUE(text.has_value());
    const std::vector<Entry> entries = fpgalsp::constraints::adc::Parse(*text);
    ASSERT_EQ(entries.size(), 47u);  // 53 行里有 6 行空行/纯空白
    for (const Entry& entry : entries) {
        EXPECT_EQ(entry.command, "set_pin_assignment");
        EXPECT_EQ(entry.attrs.at("IOSTANDARD"), "LVCMOS33");
        EXPECT_TRUE(entry.attrs.count("LOCATION") == 1u);
    }
    // 末尾 relay[0] 那行有前导空格
    EXPECT_EQ(entries.back().port, "relay[3]");
    EXPECT_EQ(entries[entries.size() - 4].character, 1);
}

TEST(AdcSplitPort, BusScalarAndMalformed) {
    using fpgalsp::constraints::adc::SplitPort;
    {
        const auto split = SplitPort("pwmG[5]");
        EXPECT_EQ(split.base, "pwmG");
        ASSERT_TRUE(split.bit.has_value());
        EXPECT_EQ(*split.bit, 5);
    }
    {
        const auto split = SplitPort("pwmG[ 5 ]");  // 允许括号内空白
        EXPECT_EQ(split.base, "pwmG");
        EXPECT_EQ(split.bit.value_or(-1), 5);
    }
    {
        const auto split = SplitPort("clk");
        EXPECT_EQ(split.base, "clk");
        EXPECT_FALSE(split.bit.has_value());
    }
    {
        const auto split = SplitPort("di[7:0]");  // 位范围：best-effort 取基名，无 bit
        EXPECT_EQ(split.base, "di");
        EXPECT_FALSE(split.bit.has_value());
    }
    {
        const auto split = SplitPort("");  // 空串
        EXPECT_TRUE(split.base.empty());
        EXPECT_FALSE(split.bit.has_value());
    }
    {
        const auto split = SplitPort("9abc");  // 不以标识符开头 -> 原样返回
        EXPECT_EQ(split.base, "9abc");
        EXPECT_FALSE(split.bit.has_value());
    }
}

// =============================================================================
//  IO 标准白名单
// =============================================================================

// 与 Python adc.KNOWN_IO_STANDARDS 完全一致的 71 项（逐字复制，不要"精简"）
const char* const kExpectedStandards[] = {
    "BLVDS25", "DIFF_HSTL15", "DIFF_HSTL18_I", "DIFF_POD12", "DIFF_POD12_PZQ", "DIFF_SSTL12",
    "DIFF_SSTL12_PZQ", "DIFF_SSTL135", "DIFF_SSTL135_PZQ", "DIFF_SSTL15", "DIFF_SSTL15_PZQ",
    "DIFF_SSTL18_I", "HSTL12_I", "HSTL15", "HSTL18", "HSTL18_I", "HSTL18_II", "HSUL12",
    "LVCMOS10", "LVCMOS10V", "LVCMOS10V18", "LVCMOS10V25", "LVCMOS10V33", "LVCMOS12",
    "LVCMOS12L9", "LVCMOS12V18", "LVCMOS12V25", "LVCMOS12V33", "LVCMOS15", "LVCMOS15V18",
    "LVCMOS15V25", "LVCMOS15V33", "LVCMOS16", "LVCMOS18", "LVCMOS18L9", "LVCMOS18V",
    "LVCMOS18V25", "LVCMOS18V33", "LVCMOS25", "LVCMOS25V33", "LVCMOS33", "LVCMOS4", "LVCMOS8",
    "LVDS18", "LVDS18_E", "LVDS18_EH9", "LVDS25", "LVDS25E", "LVDS25_E", "LVDS33", "LVDS33_E",
    "LVDS71", "LVDS7_1", "LVPECL33", "LVTTL33", "MINI_LVDS25", "MINI_LVDS33", "PCI33", "POD12",
    "POD12_PZQ", "SSTL12", "SSTL12_PZQ", "SSTL135", "SSTL135_PZQ", "SSTL15", "SSTL15_PZQ",
    "SSTL15_R", "SSTL18", "SSTL18_I", "SSTL18_II", "SSTL25_I",
};

TEST(AdcIoStandards, MatchesPythonWhitelistExactly) {
    // A-5：查表类型改成 constexpr std::array<std::string_view>（零动态初始化），
    // 内容必须逐项不变 —— 这里把 span 收成 set 再与 Python 表比对。
    const std::span<const std::string_view> standards =
        fpgalsp::constraints::adc::KnownIoStandards();
    std::set<std::string> actual;
    for (const std::string_view value : standards) actual.emplace(value);
    std::set<std::string> expected(std::begin(kExpectedStandards), std::end(kExpectedStandards));
    EXPECT_EQ(actual.size(), 71u);
    EXPECT_EQ(expected.size(), 71u) << "期望表本身有重复项";
    EXPECT_EQ(actual, expected) << "白名单与 Python 版（adc.KNOWN_IO_STANDARDS）不一致";
}

TEST(AdcIoStandards, ContractMinimumIsPresent) {
    // CONTRACT.md 明确要求"至少"包含这 17 项
    for (const char* value :
         {"LVCMOS33", "LVCMOS25", "LVCMOS18", "LVCMOS15", "LVCMOS12", "LVTTL33", "LVDS25",
          "LVDS33", "LVDS25E", "SSTL15", "SSTL135", "SSTL18", "HSTL15", "PCI33",
          "DIFF_SSTL135", "DIFF_SSTL15", "DIFF_HSTL15"}) {
        EXPECT_TRUE(fpgalsp::constraints::adc::IsKnownIoStandard(value)) << value;
    }
}

TEST(AdcIoStandards, TdReportedValuesAreAcceptedWithoutDiagnostics) {
    // 白名单漏项会让用户的**正确**工程报 warning —— 逐项跑一遍诊断确认无误报。
    const std::vector<Port> ports = {MakePort("w", 3, 0)};
    for (const char* value : kExpectedStandards) {
        const std::string text =
            AdcLine("w[0]", LineAttrs{std::string("P1"), std::string(value), std::nullopt});
        const auto diagnostics = fpgalsp::constraints::adc::Diagnostics(
            fpgalsp::constraints::adc::Parse(text), ports, nullptr, text);
        EXPECT_TRUE(ByCode(diagnostics, "ADC-IOSTD").empty()) << value;
    }
}

TEST(AdcIoStandards, LookupIsCaseInsensitive) {
    EXPECT_TRUE(fpgalsp::constraints::adc::IsKnownIoStandard("lvcmos33"));
    EXPECT_TRUE(fpgalsp::constraints::adc::IsKnownIoStandard("LvCmos18"));
    EXPECT_FALSE(fpgalsp::constraints::adc::IsKnownIoStandard("LVCMOS99"));
    EXPECT_FALSE(fpgalsp::constraints::adc::IsKnownIoStandard(""));
}

TEST(AdcIoStandards, PulltypesAndAttrKeys) {
    const std::span<const std::string_view> pulltypes =
        fpgalsp::constraints::adc::KnownPulltypes();
    EXPECT_EQ(pulltypes.size(), 6u);
    EXPECT_NE(std::ranges::find(pulltypes, std::string_view("WEAKPULLUP")), pulltypes.end());
    EXPECT_NE(std::ranges::find(pulltypes, std::string_view("NONE")), pulltypes.end());

    const std::span<const std::string_view> keys = fpgalsp::constraints::adc::KnownAttrKeys();
    EXPECT_NE(std::ranges::find(keys, std::string_view("LOCATION")), keys.end());
    EXPECT_NE(std::ranges::find(keys, std::string_view("IOSTANDARD")), keys.end());
    EXPECT_NE(std::ranges::find(keys, std::string_view("CLOCK_DEDICATED_ROUTE")), keys.end());
    EXPECT_EQ(std::ranges::find(keys, std::string_view("BOGUS")), keys.end());
}

TEST(AdcIoStandards, OnlySetPinAssignmentIsKnown) {
    const std::span<const std::string_view> commands =
        fpgalsp::constraints::adc::KnownCommands();
    EXPECT_EQ(commands.size(), 1u);
    EXPECT_TRUE(fpgalsp::constraints::adc::IsKnownCommand("set_pin_assignment"));
    // ⚠ P-8：Python 的**补全**列表里会推荐 set_ippin_assignment，
    //   但白名单里没有它 -> 用户照补全写就会收到 ADC-CMD。见 PYTHON-BUGS.md。
    EXPECT_FALSE(fpgalsp::constraints::adc::IsKnownCommand("set_ippin_assignment"));
}

// =============================================================================
//  覆盖率
// =============================================================================

TEST(AdcCoverage, ScalarUnconstrainedUsesEmptyBitList) {
    const std::vector<Port> ports = {MakePort("sin", 1, 0), MakePort("w", 3, 5)};
    const auto entries =
        fpgalsp::constraints::adc::Parse(AdcLine("w[0]", LineAttrs{std::string("P1")}));
    const auto coverage = fpgalsp::constraints::adc::Coverage(entries, ports);

    EXPECT_EQ(coverage.unconstrained_ports, std::vector<std::string>{"sin"});
    ASSERT_TRUE(coverage.missing.count("sin") == 1u);
    EXPECT_TRUE(coverage.missing.at("sin").empty());
    ASSERT_TRUE(coverage.covered.count("w") == 1u);
    EXPECT_EQ(coverage.covered.at("w"), std::vector<int>({0}));
    EXPECT_EQ(coverage.missing.at("w"), std::vector<int>({1, 2}));
    EXPECT_EQ(coverage.total_bits, 4);
    EXPECT_EQ(coverage.constrained_bits, 1);
}

TEST(AdcCoverage, ExtraPortsDuplicatePortsAndPins) {
    const std::vector<Port> ports = {MakePort("w", 3)};
    const std::string text = JoinLines({
        AdcLine("w[0]", LineAttrs{std::string("P1")}),
        AdcLine("nope", LineAttrs{std::string("P2")}),
        AdcLine("w[0]", LineAttrs{std::string("P3")}),
        AdcLine("w[1]", LineAttrs{std::string("P1")}),
    });
    const auto entries = fpgalsp::constraints::adc::Parse(text);
    const auto coverage = fpgalsp::constraints::adc::Coverage(entries, ports);

    ASSERT_EQ(coverage.extra_ports.size(), 1u);
    EXPECT_EQ(coverage.extra_ports.front().port, "nope");
    ASSERT_EQ(coverage.duplicate_ports.size(), 1u);
    EXPECT_EQ(coverage.duplicate_ports.front().port, "w[0]");
    EXPECT_EQ(coverage.duplicate_ports.front().lines, std::vector<int>({0, 2}));
    ASSERT_EQ(coverage.duplicate_pins.size(), 1u);
    EXPECT_EQ(coverage.duplicate_pins.front().pin, "P1");
    EXPECT_EQ(coverage.duplicate_pins.front().lines, std::vector<int>({0, 3}));
}

TEST(AdcCoverage, RealCorpusIsFullyCovered) {
    const std::optional<std::string> text = ReadTextFile(kRealAdcPath);
    ASSERT_TRUE(text.has_value());
    const auto entries = fpgalsp::constraints::adc::Parse(*text);
    const auto coverage = fpgalsp::constraints::adc::Coverage(entries, TopPorts(), "top");

    EXPECT_EQ(coverage.total_bits, 47);
    EXPECT_EQ(coverage.constrained_bits, 47);
    EXPECT_TRUE(coverage.missing.empty());
    EXPECT_TRUE(coverage.unconstrained_ports.empty());
    EXPECT_TRUE(coverage.extra_ports.empty());
    EXPECT_TRUE(coverage.duplicate_ports.empty());
    EXPECT_TRUE(coverage.duplicate_pins.empty());
    EXPECT_EQ(coverage.covered.at("pwmG"), std::vector<int>({0, 1, 2, 3, 4, 5}));
    EXPECT_EQ(coverage.covered.at("relay"), std::vector<int>({0, 1, 2, 3}));
    EXPECT_TRUE(coverage.covered.at("clk_input").empty());  // 标量：有约束但无 bit 列表
}

TEST(AdcCoverage, EmptyEntriesMakesEverythingMissing) {
    const auto coverage = fpgalsp::constraints::adc::Coverage({}, TopPorts());
    EXPECT_EQ(coverage.constrained_bits, 0);
    EXPECT_EQ(coverage.total_bits, 47);
    EXPECT_EQ(coverage.unconstrained_ports.size(), 25u);
    EXPECT_EQ(coverage.missing.at("pwmG"), std::vector<int>({0, 1, 2, 3, 4, 5}));
}

TEST(AdcCoverage, BitsFromMsbLsbWhenWidthMissing) {
    Port port;
    port.name = "bus";
    port.msb = 5;
    port.lsb = 2;
    port.width = std::nullopt;  // 只有 msb/lsb
    const auto coverage =
        fpgalsp::constraints::adc::Coverage(fpgalsp::constraints::adc::Parse(""), {port});
    EXPECT_EQ(coverage.total_bits, 4);
    EXPECT_EQ(coverage.missing.at("bus"), std::vector<int>({2, 3, 4, 5}));
}

TEST(AdcCoverage, TopParameterIsIgnoredLikePython) {
    // 文档化行为：Python 的 coverage(entries, ports, top) 里 top 从未被使用。
    // 传什么 top 结果都一样；保留参数只为签名对照。
    const std::optional<std::string> text = ReadTextFile(kRealAdcPath);
    ASSERT_TRUE(text.has_value());
    const auto entries = fpgalsp::constraints::adc::Parse(*text);
    const auto with_top = fpgalsp::constraints::adc::Coverage(entries, TopPorts(), "top");
    const auto without = fpgalsp::constraints::adc::Coverage(entries, TopPorts());
    EXPECT_EQ(fpgalsp::constraints::adc::ToJson(with_top),
              fpgalsp::constraints::adc::ToJson(without));
}

// =============================================================================
//  诊断
// =============================================================================

TEST(AdcDiagnostics, UnknownPortIsError) {
    const std::string text = AdcLine("nope[0]", LineAttrs{std::string("P1")});
    const auto diagnostics = fpgalsp::constraints::adc::Diagnostics(
        fpgalsp::constraints::adc::Parse(text), FivePorts(), nullptr, text);
    const auto found = ByCode(diagnostics, "USR-8052");
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].severity, fpgalsp::lsp::kError);
    EXPECT_EQ(found[0].range.start.line, 0);
    EXPECT_EQ(found[0].range.start.character, static_cast<int>(text.find("nope[0]")));
}

TEST(AdcDiagnostics, UnknownCommandIsInformationNotWarning) {
    // CONTRACT.md / Python：ADC-CMD 的 severity 是 3(Information)，不是 warning。
    // task 描述里写的"未知命令 -> warning"是概要说法，以代码契约表为准。
    const std::string text =
        "set_pin_assignment_v2\t{ w[0] }\t{ LOCATION = P76; IOSTANDARD = LVCMOS33; }\n";
    const auto diagnostics = fpgalsp::constraints::adc::Diagnostics(
        fpgalsp::constraints::adc::Parse(text), FivePorts(), nullptr, text);
    const auto found = ByCode(diagnostics, "ADC-CMD");
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].severity, fpgalsp::lsp::kInfo);
    EXPECT_NE(found[0].severity, fpgalsp::lsp::kWarning);
    // Python repr 用单引号
    EXPECT_NE(found[0].message.find("'set_pin_assignment_v2'"), std::string::npos)
        << found[0].message;
}

TEST(AdcDiagnostics, MissingLocationIsError) {
    const std::string text =
        AdcLine("w[0]", LineAttrs{std::nullopt, std::string("LVCMOS33"), std::nullopt});
    const auto diagnostics = fpgalsp::constraints::adc::Diagnostics(
        fpgalsp::constraints::adc::Parse(text), FivePorts(), nullptr, text);
    const auto found = ByCode(diagnostics, "USR-8056");
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].severity, fpgalsp::lsp::kError);
}

TEST(AdcDiagnostics, MissingIoStandardIsWarning) {
    const std::string text =
        AdcLine("w[0]", LineAttrs{std::string("P76"), std::nullopt, std::nullopt});
    const auto diagnostics = fpgalsp::constraints::adc::Diagnostics(
        fpgalsp::constraints::adc::Parse(text), FivePorts(), nullptr, text);
    const auto found = ByCode(diagnostics, "ADC-NO-IOSTD");
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].severity, fpgalsp::lsp::kWarning);
}

TEST(AdcDiagnostics, UnknownIoStandardPointsAtTheValue) {
    const std::string text =
        AdcLine("w[0]", LineAttrs{std::string("P76"), std::string("LVCMOS99"), std::nullopt});
    const auto diagnostics = fpgalsp::constraints::adc::Diagnostics(
        fpgalsp::constraints::adc::Parse(text), FivePorts(), nullptr, text);
    const auto found = ByCode(diagnostics, "ADC-IOSTD");
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].severity, fpgalsp::lsp::kWarning);
    const int at = static_cast<int>(text.find("LVCMOS99"));
    EXPECT_EQ(found[0].range.start.character, at);
    EXPECT_EQ(found[0].range.end.character, at + 8);
}

TEST(AdcDiagnostics, DuplicatePortBitAndDuplicatePin) {
    const std::string text = JoinLines({
        AdcLine("w[0]", LineAttrs{std::string("P76")}),
        AdcLine("w[0]", LineAttrs{std::string("P77")}),
        AdcLine("w[1]", LineAttrs{std::string("P76")}),
    });
    const auto diagnostics = fpgalsp::constraints::adc::Diagnostics(
        fpgalsp::constraints::adc::Parse(text), FivePorts(), nullptr, text);

    const auto duplicate_ports = ByCode(diagnostics, "ADC-DUP-PORT");
    ASSERT_EQ(duplicate_ports.size(), 1u);
    EXPECT_EQ(duplicate_ports[0].severity, fpgalsp::lsp::kError);
    EXPECT_EQ(duplicate_ports[0].range.start.line, 1);
    EXPECT_NE(duplicate_ports[0].message.find("first at line 1"), std::string::npos);

    const auto duplicate_pins = ByCode(diagnostics, "ADC-DUP-PIN");
    ASSERT_EQ(duplicate_pins.size(), 1u);
    EXPECT_EQ(duplicate_pins[0].severity, fpgalsp::lsp::kError);
    EXPECT_EQ(duplicate_pins[0].range.start.line, 2);
    EXPECT_NE(duplicate_pins[0].message.find("already assigned to w[0] at line 1"),
              std::string::npos)
        << duplicate_pins[0].message;
}

TEST(AdcDiagnostics, DuplicateAttributeWarnsAndUnknownAttributeHints) {
    const std::string text =
        "set_pin_assignment\t{ w[0] }\t{ LOCATION = P76; LOCATION = P77; "
        "IOSTANDARD = LVCMOS33; BOGUS = 1; }\n";
    const auto diagnostics = fpgalsp::constraints::adc::Diagnostics(
        fpgalsp::constraints::adc::Parse(text), FivePorts(), nullptr, text);
    const auto attribute_diagnostics = ByCode(diagnostics, "ADC-ATTR");
    ASSERT_EQ(attribute_diagnostics.size(), 2u);

    int warnings = 0;
    int hints = 0;
    for (const Diagnostic& diagnostic : attribute_diagnostics) {
        if (diagnostic.severity == fpgalsp::lsp::kWarning) {
            ++warnings;
            EXPECT_EQ(diagnostic.range.start.character, static_cast<int>(text.find("P77")));
        } else if (diagnostic.severity == fpgalsp::lsp::kInfo) {
            ++hints;
            EXPECT_EQ(diagnostic.range.start.character, static_cast<int>(text.find("BOGUS")));
        }
    }
    EXPECT_EQ(warnings, 1);
    EXPECT_EQ(hints, 1);
}

TEST(AdcDiagnostics, MissingPortArgument) {
    const std::string text = "set_pin_assignment\t{ LOCATION = P1; }\n";
    const auto diagnostics = fpgalsp::constraints::adc::Diagnostics(
        fpgalsp::constraints::adc::Parse(text), FivePorts(), nullptr, text);
    const auto found = ByCode(diagnostics, "ADC-ATTR");
    ASSERT_EQ(found.size(), 1u);
    EXPECT_NE(found[0].message.find("{ port }"), std::string::npos);
}

TEST(AdcDiagnostics, BusBitMissingProducesBit8123OnPortDeclaration) {
    const std::string text = JoinLines({
        AdcLine("relay[0]", LineAttrs{std::string("P40")}),
        AdcLine("relay[1]", LineAttrs{std::string("P41")}),
        AdcLine("relay[2]", LineAttrs{std::string("P42")}),
    });
    const auto diagnostics = fpgalsp::constraints::adc::Diagnostics(
        fpgalsp::constraints::adc::Parse(text), FivePorts(), nullptr, text);
    const auto all = ByCode(diagnostics, "BIT-8123");
    std::vector<Diagnostic> relay_only;
    for (const Diagnostic& diagnostic : all) {
        if (diagnostic.data.is_object() && diagnostic.data.value("port", "") == "relay") {
            relay_only.push_back(diagnostic);
        }
    }
    ASSERT_EQ(relay_only.size(), 1u);
    EXPECT_EQ(relay_only[0].severity, fpgalsp::lsp::kError);
    // 落在顶层模块的端口声明行（relay 在 10 行、字符 9），而不是 .adc 文件里
    EXPECT_EQ(relay_only[0].range.start.line, 10);
    EXPECT_EQ(relay_only[0].range.start.character, 9);
    EXPECT_NE(relay_only[0].message.find("relay[3]"), std::string::npos);
    EXPECT_EQ(relay_only[0].data.value("bit", -1), 3);
}

TEST(AdcDiagnostics, OutOfRangeBitAndScalarWithBitIndex) {
    {
        const std::string text = AdcLine("w[7]", LineAttrs{std::string("P1")});
        const auto diagnostics = fpgalsp::constraints::adc::Diagnostics(
            fpgalsp::constraints::adc::Parse(text), FivePorts(), nullptr, text);
        const auto found = ByCode(diagnostics, "USR-8052");
        ASSERT_EQ(found.size(), 1u);
        EXPECT_NE(found[0].message.find("outside the range"), std::string::npos);
    }
    {
        const std::string text = AdcLine("sin[0]", LineAttrs{std::string("P1")});
        const auto diagnostics = fpgalsp::constraints::adc::Diagnostics(
            fpgalsp::constraints::adc::Parse(text), FivePorts(), nullptr, text);
        const auto found = ByCode(diagnostics, "USR-8052");
        ASSERT_EQ(found.size(), 1u);
        EXPECT_NE(found[0].message.find("must not have a bit index"), std::string::npos);
    }
}

TEST(AdcDiagnostics, BusPortWithoutBitIndex) {
    const std::string text = AdcLine("w", LineAttrs{std::string("P1")});
    const auto diagnostics = fpgalsp::constraints::adc::Diagnostics(
        fpgalsp::constraints::adc::Parse(text), FivePorts(), nullptr, text);
    const auto found = ByCode(diagnostics, "USR-8052");
    ASSERT_EQ(found.size(), 1u);
    EXPECT_NE(found[0].message.find("missing a bit index"), std::string::npos);
}

TEST(AdcDiagnostics, DeviceNoneSkipsPinExistenceChecks) {
    const std::string text = AdcLine("sin", LineAttrs{std::string("P999")});
    const auto diagnostics = fpgalsp::constraints::adc::Diagnostics(
        fpgalsp::constraints::adc::Parse(text), FivePorts(), nullptr, text);
    EXPECT_TRUE(ByCode(diagnostics, "USR-8026").empty());
    EXPECT_TRUE(ByCode(diagnostics, "USR-8027").empty());
}

TEST(AdcDiagnostics, DevicePinLookupIsCaseInsensitiveAndAcceptsLocationName) {
    const Json device = DeviceFixture();
    const std::string text = JoinLines({
        AdcLine("clk_input", LineAttrs{std::string("P87")}),   // 存在的引脚（大小写不敏感）
        AdcLine("w[0]", LineAttrs{std::string("pt35a")}),      // TD 的 location 名
    });
    const auto diagnostics = fpgalsp::constraints::adc::Diagnostics(
        fpgalsp::constraints::adc::Parse(text), FivePorts(), &device, text);
    EXPECT_TRUE(ByCode(diagnostics, "USR-8026").empty());
    EXPECT_TRUE(ByCode(diagnostics, "USR-8027").empty());
}

TEST(AdcDiagnostics, DeviceMissingPinAndDedicatedPin) {
    const Json device = DeviceFixture();
    const std::string text = JoinLines({
        AdcLine("clk_input", LineAttrs{std::string("P87")}),
        AdcLine("w[0]", LineAttrs{std::string("P119")}),  // dedicated/config 引脚
        AdcLine("w[1]", LineAttrs{std::string("P999")}),  // 器件上不存在
    });
    const auto diagnostics = fpgalsp::constraints::adc::Diagnostics(
        fpgalsp::constraints::adc::Parse(text), FivePorts(), &device, text);

    const auto missing = ByCode(diagnostics, "USR-8026");
    ASSERT_EQ(missing.size(), 1u);
    EXPECT_EQ(missing[0].severity, fpgalsp::lsp::kError);
    EXPECT_EQ(missing[0].range.start.line, 2);

    const auto dedicated = ByCode(diagnostics, "USR-8027");
    ASSERT_EQ(dedicated.size(), 1u);
    EXPECT_EQ(dedicated[0].severity, fpgalsp::lsp::kError);
    EXPECT_EQ(dedicated[0].range.start.line, 1);
}

TEST(AdcDiagnostics, DeviceWithEmptyPinTableFlagsEveryPin) {
    // ⚠ P-9（见 PYTHON-BUGS.md）：Python 只看 `device is not None`，不看引脚表是否
    //   真的加载成功。device 有值但 pins 为空时，**每一条**约束都会报
    //   USR-8026 "Pin ... does not exist on this device"。
    //   这与"覆盖率刷屏"同一类误报。忠实复现；修掉 Python 缺陷后本测试会变红。
    Json device = Json::object();
    device["device"] = "EF2L45LG144B";
    device["pins"] = Json::object();          // 空引脚表
    device["dedicated"] = Json::array();

    const std::string text = JoinLines({
        AdcLine("clk_input", LineAttrs{std::string("P87")}),
        AdcLine("w[0]", LineAttrs{std::string("P76")}),
        AdcLine("w[1]", LineAttrs{std::string("P77")}),
    });
    const auto diagnostics = fpgalsp::constraints::adc::Diagnostics(
        fpgalsp::constraints::adc::Parse(text), FivePorts(), &device, text);
    EXPECT_EQ(ByCode(diagnostics, "USR-8026").size(), 3u);
}

TEST(AdcDiagnostics, StandaloneAdcWithoutPortListIsSilent) {
    // 单独打开一个 .adc（没有工程/顶层端口表）时不能把每个端口都报成"不存在"
    const std::string text = AdcLine("nope[0]", LineAttrs{std::string("P1")});
    const auto diagnostics = fpgalsp::constraints::adc::Diagnostics(
        fpgalsp::constraints::adc::Parse(text), {}, nullptr, text);
    EXPECT_TRUE(diagnostics.empty());
}

TEST(AdcDiagnostics, PulltypeValueIsNeverValidated) {
    // ⚠ P-10（见 PYTHON-BUGS.md）：adc.py 定义了 KNOWN_PULLTYPES 却从不校验，
    //   `PULLTYPE = PULLUPX` 静默通过，直到 bitgen 才报错。忠实复现。
    const std::string text =
        AdcLine("w[0]", LineAttrs{std::string("P1"), std::string("LVCMOS33"),
                                  std::string("PULLUPX")});
    const auto diagnostics = fpgalsp::constraints::adc::Diagnostics(
        fpgalsp::constraints::adc::Parse(text), FivePorts(), nullptr, text);
    // 这里只关心 PULLTYPE：不能出现任何与属性值相关的诊断。
    // （BIT-8123 会因为 FivePorts 里其它总线没约束而出现，与本用例无关。）
    EXPECT_TRUE(ByCode(diagnostics, "ADC-ATTR").empty());
    EXPECT_TRUE(ByCode(diagnostics, "ADC-IOSTD").empty());
    for (const Diagnostic& diagnostic : diagnostics) {
        EXPECT_EQ(diagnostic.message.find("PULLUPX"), std::string::npos) << diagnostic.message;
    }
}

TEST(AdcDiagnostics, RealCorpusIsClean) {
    const std::optional<std::string> text = ReadTextFile(kRealAdcPath);
    ASSERT_TRUE(text.has_value());
    const auto diagnostics = fpgalsp::constraints::adc::Diagnostics(
        fpgalsp::constraints::adc::Parse(*text), TopPorts(), nullptr, *text);
    EXPECT_TRUE(diagnostics.empty()) << "真实语料不应有任何诊断；第一条是 "
                                     << diagnostics.front().code;
}

TEST(AdcDiagnostics, NeverThrowsOnGarbage) {
    const std::vector<Port> ports = FivePorts();
    const Json device = DeviceFixture();
    for (const std::string& text : std::vector<std::string>{
             "", "set_pin_assignment", "{", "}}}", "garbage\t\tstuff",
             std::string("\x00\xFF", 2)}) {
        const auto entries = fpgalsp::constraints::adc::Parse(text);
        const auto diagnostics =
            fpgalsp::constraints::adc::Diagnostics(entries, ports, &device, text);
        const Json json = fpgalsp::lsp::ToJson(diagnostics);
        EXPECT_TRUE(json.is_array());
        // ports 为空 + device 为空 的组合也不能崩
        const auto bare = fpgalsp::constraints::adc::Diagnostics(entries, {}, nullptr, text);
        EXPECT_TRUE(fpgalsp::lsp::ToJson(bare).is_array());
    }
}

TEST(AdcDiagnostics, SortedByLineThenCharacterThenCode) {
    const std::string text = JoinLines({
        AdcLine("nope[0]", LineAttrs{std::string("P1")}),
        AdcLine("w[0]", LineAttrs{std::nullopt, std::string("LVCMOS99"), std::nullopt}),
        AdcLine("w[1]", LineAttrs{std::string("P2")}),
    });
    const auto diagnostics = fpgalsp::constraints::adc::Diagnostics(
        fpgalsp::constraints::adc::Parse(text), FivePorts(), nullptr, text);
    ASSERT_GE(diagnostics.size(), 3u);
    for (size_t index = 1; index < diagnostics.size(); ++index) {
        const auto& previous = diagnostics[index - 1].range.start;
        const auto& current = diagnostics[index].range.start;
        const bool ordered = (previous.line < current.line) ||
                             (previous.line == current.line &&
                              previous.character <= current.character);
        EXPECT_TRUE(ordered);
    }
}

// =============================================================================
//  覆盖率摘要诊断（ADC-COVERAGE）—— 必须是 info 级，且一个文档只有一条
// =============================================================================

TEST(AdcCoverageDiagnostic, SeverityIsInformationNeverWarning) {
    // 这条是硬要求：覆盖率做成 warning 会让一个未约束完的工程在 Problems 里
    // 刷几十条噪音，用户会直接关掉这个功能。
    const std::string text = AdcLine("w[0]", LineAttrs{std::string("P1")});
    const auto entries = fpgalsp::constraints::adc::Parse(text);
    const auto coverage = fpgalsp::constraints::adc::Coverage(entries, FivePorts());
    const auto diagnostic = fpgalsp::constraints::adc::CoverageDiagnostic(coverage, "top");
    ASSERT_TRUE(diagnostic.has_value());
    EXPECT_EQ(diagnostic->severity, fpgalsp::lsp::kInfo);
    EXPECT_NE(diagnostic->severity, fpgalsp::lsp::kWarning);
    EXPECT_NE(diagnostic->severity, fpgalsp::lsp::kError);
    EXPECT_EQ(diagnostic->code, "ADC-COVERAGE");
}

TEST(AdcCoverageDiagnostic, ExactlyOneSummaryForUnconstrainedProject) {
    // 完全空白的 .adc：25 个端口 / 47 bit 全未约束，但只能有**一条**诊断
    const auto entries = fpgalsp::constraints::adc::Parse("");
    const auto diagnostics = fpgalsp::constraints::adc::Diagnostics(
        entries, TopPorts(),
        fpgalsp::constraints::adc::DiagnosticsOptions{nullptr, {}, true, "top"});
    EXPECT_EQ(ByCode(diagnostics, "ADC-COVERAGE").size(), 1u)
        << "覆盖率必须是单条摘要，不能按端口/按 bit 刷屏";
    // 逐 bit 的 BIT-8123 是 Python 的既有行为（error 级，落点在顶层端口声明行），
    // 这里保持等价：7 条总线共 29 bit 全部缺失 -> 29 条。
    EXPECT_EQ(ByCode(diagnostics, "BIT-8123").size(), 29u);
}

TEST(AdcCoverageDiagnostic, SilentWhenFullyCovered) {
    const std::optional<std::string> text = ReadTextFile(kRealAdcPath);
    ASSERT_TRUE(text.has_value());
    const auto entries = fpgalsp::constraints::adc::Parse(*text);
    const auto coverage = fpgalsp::constraints::adc::Coverage(entries, TopPorts());
    EXPECT_FALSE(fpgalsp::constraints::adc::CoverageDiagnostic(coverage, "top").has_value());
}

TEST(AdcCoverageDiagnostic, SilentWithoutTopPortList) {
    const auto coverage = fpgalsp::constraints::adc::Coverage({}, {});
    EXPECT_FALSE(fpgalsp::constraints::adc::CoverageDiagnostic(coverage).has_value());
}

TEST(AdcCoverageDiagnostic, MessageCarriesStatisticsAndIsCapped) {
    const auto entries = fpgalsp::constraints::adc::Parse("");
    const auto coverage = fpgalsp::constraints::adc::Coverage(entries, TopPorts());
    const auto diagnostic = fpgalsp::constraints::adc::CoverageDiagnostic(coverage, "top");
    ASSERT_TRUE(diagnostic.has_value());
    EXPECT_NE(diagnostic->message.find("0/47"), std::string::npos) << diagnostic->message;
    EXPECT_NE(diagnostic->message.find("0.0%"), std::string::npos) << diagnostic->message;
    EXPECT_NE(diagnostic->message.find("top module top"), std::string::npos);
    EXPECT_NE(diagnostic->message.find("and 17 more"), std::string::npos)
        << "列表最多列 8 项，其余折叠；实际：" << diagnostic->message;
    EXPECT_LT(diagnostic->message.size(), 400u);
    ASSERT_TRUE(diagnostic->data.is_object());
    EXPECT_EQ(diagnostic->data.value("total_bits", -1), 47);
    EXPECT_EQ(diagnostic->data.value("constrained_bits", -1), 0);
}

TEST(AdcCoverageDiagnostic, OptionsFlagAppendsOneInfoDiagnostic) {
    const std::string text = JoinLines({
        AdcLine("w[0]", LineAttrs{std::string("P1")}),
        AdcLine("nope[0]", LineAttrs{std::string("P2")}),
    });
    const auto entries = fpgalsp::constraints::adc::Parse(text);

    fpgalsp::constraints::adc::DiagnosticsOptions options;
    options.include_coverage = true;
    options.top = "top";
    const auto with_coverage = fpgalsp::constraints::adc::Diagnostics(entries, FivePorts(), options);

    const auto without = fpgalsp::constraints::adc::Diagnostics(entries, FivePorts(), nullptr, text);
    EXPECT_EQ(ByCode(without, "ADC-COVERAGE").size(), 0u) << "默认与 Python 等价，不产出覆盖率";
    EXPECT_EQ(ByCode(with_coverage, "ADC-COVERAGE").size(), 1u);
    EXPECT_EQ(with_coverage.size(), without.size() + 1);
}

// =============================================================================
//  光标辅助
// =============================================================================

namespace {

const char* kCursorText =
    "set_pin_assignment\t{ w[0] }\t{ LOCATION = P76; IOSTANDARD = LVCMOS33; PULLTYPE = NONE; }\n";

int IndexOf(const std::string& haystack, const std::string& needle) {
    return static_cast<int>(haystack.find(needle));
}

}  // namespace

TEST(AdcContext, PinValue) {
    const std::string text = kCursorText;
    const std::string raw = text.substr(0, text.size() - 1);
    const auto context =
        fpgalsp::constraints::adc::Context(text, 0, IndexOf(raw, "P76") + 1);
    ASSERT_TRUE(context.has_value());
    EXPECT_EQ(context->kind, "pin");
    EXPECT_EQ(context->span.start, IndexOf(raw, "P76"));
    EXPECT_EQ(context->span.end, IndexOf(raw, "P76") + 3);
}

TEST(AdcContext, IoStandardPulltypeCommandAndAttrKey) {
    const std::string text = kCursorText;
    const std::string raw = text.substr(0, text.size() - 1);
    const auto kind_at = [&text](int character) {
        const auto context = fpgalsp::constraints::adc::Context(text, 0, character);
        return context.has_value() ? context->kind : std::string("<none>");
    };
    EXPECT_EQ(kind_at(IndexOf(raw, "LVCMOS33") + 2), "iostandard");
    EXPECT_EQ(kind_at(IndexOf(raw, "NONE") + 1), "pulltype");
    EXPECT_EQ(kind_at(3), "command");
    EXPECT_EQ(kind_at(IndexOf(raw, "LOCATION")), "attr_key");
    // 属性组内部、但不在键/值上 -> 无意义（server 会回落到通用补全）
    EXPECT_EQ(kind_at(IndexOf(raw, "LOCATION") - 2), "<none>");
}

TEST(AdcContext, PortSpanIsTheBaseNameNotTheBitIndex) {
    const std::string text = kCursorText;
    const std::string raw = text.substr(0, text.size() - 1);
    const int port_at = IndexOf(raw, "w[0]");
    {
        const auto context = fpgalsp::constraints::adc::Context(text, 0, port_at);
        ASSERT_TRUE(context.has_value());
        EXPECT_EQ(context->kind, "port");
        EXPECT_EQ(context->span.start, port_at);
        EXPECT_EQ(context->span.end, port_at + 1);  // 只覆盖 "w"
    }
    {
        const auto context = fpgalsp::constraints::adc::Context(text, 0, port_at + 2);
        ASSERT_TRUE(context.has_value());
        EXPECT_EQ(context->kind, "port");
        EXPECT_EQ(context->span.end, port_at + 1);  // 光标在 [0] 上也解析到基名
    }
}

TEST(AdcContext, NonePlaces) {
    using fpgalsp::constraints::adc::Context;
    EXPECT_FALSE(Context(kCursorText, 2, 0).has_value());
    EXPECT_FALSE(Context("", 0, 0).has_value());
    EXPECT_FALSE(Context("# comment\n", 0, 3).has_value());
    EXPECT_FALSE(Context("set_pin_assignment\n", 0, 100).has_value());
    EXPECT_FALSE(Context(kCursorText, -1, 0).has_value());
    EXPECT_FALSE(Context(kCursorText, 0, -1).has_value());
}

TEST(AdcContext, RealCorpusCursorPositions) {
    const std::optional<std::string> text = ReadTextFile(kRealAdcPath);
    ASSERT_TRUE(text.has_value());
    const std::string& content = *text;
    const int pin_at = IndexOf(content, "P87") + 1;
    const auto pin = fpgalsp::constraints::adc::Context(content, 0, pin_at);
    ASSERT_TRUE(pin.has_value());
    EXPECT_EQ(pin->kind, "pin");
    EXPECT_EQ(pin->span.start, IndexOf(content, "P87"));
}

TEST(AdcWordAt, PinAndPortWords) {
    const std::string text = kCursorText;
    const std::string raw = text.substr(0, text.size() - 1);
    const int pin_at = IndexOf(raw, "P76");
    const auto word = fpgalsp::constraints::adc::WordAt(text, 0, pin_at + 1);
    ASSERT_TRUE(word.has_value());
    EXPECT_EQ(word->word, "P76");
    EXPECT_EQ(word->start, pin_at);
    EXPECT_EQ(word->end, pin_at + 3);
    EXPECT_EQ(word->line, 0);

    const auto port_word =
        fpgalsp::constraints::adc::WordAt(text, 0, IndexOf(raw, "w[0]"));
    ASSERT_TRUE(port_word.has_value());
    EXPECT_EQ(port_word->word, "w[0]");
}

TEST(AdcWordAt, OutOfRangeAndCommentLines) {
    using fpgalsp::constraints::adc::WordAt;
    EXPECT_FALSE(WordAt(kCursorText, 5, 0).has_value());
    EXPECT_FALSE(WordAt("# comment\n", 0, 3).has_value());
    EXPECT_FALSE(WordAt("  \n", 0, 1).has_value());
    EXPECT_FALSE(WordAt(kCursorText, -2, 0).has_value());
}

// =============================================================================
//  JSON 形状（与 Python dict 对齐，供 server / golden 使用）
// =============================================================================

TEST(AdcJson, EntryShapeMatchesContract) {
    const auto entries = fpgalsp::constraints::adc::Parse(
        AdcLine("w[0]", LineAttrs{std::string("P76")}));
    ASSERT_EQ(entries.size(), 1u);
    const Json json = fpgalsp::constraints::adc::ToJson(entries.front());
    EXPECT_EQ(json.at("line").get<int>(), 0);
    EXPECT_EQ(json.at("command").get<std::string>(), "set_pin_assignment");
    EXPECT_EQ(json.at("port").get<std::string>(), "w[0]");
    EXPECT_EQ(json.at("base").get<std::string>(), "w");
    EXPECT_EQ(json.at("bit").get<int>(), 0);
    EXPECT_EQ(json.at("attrs").at("LOCATION").get<std::string>(), "P76");
    EXPECT_TRUE(json.at("attr_spans").contains("LOCATION"));
    EXPECT_TRUE(json.at("port_span").contains("start"));
    EXPECT_EQ(json.at("attr_items").size(), 3u);
}

TEST(AdcJson, CoverageShapeMatchesContract) {
    const auto coverage = fpgalsp::constraints::adc::Coverage({}, TopPorts());
    const Json json = fpgalsp::constraints::adc::ToJson(coverage);
    EXPECT_EQ(json.at("total_bits").get<int>(), 47);
    EXPECT_EQ(json.at("constrained_bits").get<int>(), 0);
    EXPECT_TRUE(json.contains("covered"));
    EXPECT_TRUE(json.contains("missing"));
    EXPECT_TRUE(json.contains("unconstrained_ports"));
    EXPECT_TRUE(json.contains("extra_ports"));
    EXPECT_TRUE(json.contains("duplicate_ports"));
    EXPECT_TRUE(json.contains("duplicate_pins"));
    EXPECT_EQ(json.at("unconstrained_ports").size(), 25u);
}

// =============================================================================
//  金标准重新生成方式（与 Python 参考实现逐字段对照）
// =============================================================================
//  在 Y:\fpga-lsp 目录下执行：
//
//    python - <<'PY'
//    import sys; sys.path.insert(0, r"Y:\fpga-lsp")
//    from fpgalsp import adc
//    text = open(r"Y:\fpga-lsp-cpp\tests\gate-corpus\corpus_alpha\constrains\pin.adc",
//                encoding="utf-8").read()
//    for e in adc.parse(text):
//        a = e["attrs"]; bit = "-" if e["bit"] is None else str(e["bit"])
//        sp = lambda k: (lambda s: "-" if s is None else "%d:%d" % (s["start"], s["end"])
//                        )(e["attr_spans"].get(k))
//        print("|".join([str(e["line"]), str(e["character"]), e["port"], e["base"], bit,
//                        a.get("LOCATION",""), a.get("IOSTANDARD",""), a.get("PULLTYPE",""),
//                        "%d:%d" % (e["port_span"]["start"], e["port_span"]["end"]),
//                        sp("LOCATION"), sp("IOSTANDARD"), sp("PULLTYPE"),
//                        "%d:%d" % (e["command_span"]["start"], e["command_span"]["end"])]))
//    PY
// =============================================================================
//  与 Python 参考实现逐字段对照的**导出通道**（默认跳过，不产生噪音）
// =============================================================================
//  用法（在 Y:\fpga-lsp-cpp\build\bin 下）：
//    $env:FPGA_LSP_ADC_DUMP = "C:\temp\adc_cpp.json"; .\unit_adc.exe --gtest_filter=AdcParity.*
//  然后与 Python 输出对比：
//    python tests/unit/parity_check.py   （或按文件末尾注释自己拼一条 python -c）
//  导出内容 = {entries, coverage, diagnostics}，字段与 Python 的
//  adc.parse / adc.coverage / adc.diagnostics 返回值一一对应。
// =============================================================================

TEST(AdcParity, DumpCorpusJsonWhenRequested) {
    const std::optional<std::string> target = ReadEnv("FPGA_LSP_ADC_DUMP");
    if (!target.has_value()) {
        GTEST_SKIP() << "设置 FPGA_LSP_ADC_DUMP=<path> 可导出与 Python 对照的 JSON";
    }
    const std::optional<std::string> text = ReadTextFile(kRealAdcPath);
    ASSERT_TRUE(text.has_value());
    const auto entries = fpgalsp::constraints::adc::Parse(*text);

    Json out = Json::object();
    // 合成的边界输入：每个用例同时导出 entries / coverage / diagnostics，
    // Python 侧按同一份文本重算，做到"畸形输入也逐字段一致"。
    const std::vector<std::pair<std::string, std::string>> synthetic = {
        {"empty", ""},
        {"comment_only", "# just a comment\n"},
        {"scalar_port", AdcLine("clk_input", LineAttrs{std::string("P87")})},
        {"bus_bit", AdcLine("pwmG[3]", LineAttrs{std::string("P11")})},
        {"no_semicolon",
         "set_pin_assignment\t{ w[0] }\t{ LOCATION = P76; IOSTANDARD = LVCMOS33 }"},
        {"dup_location",
         AdcLine("w[0]", LineAttrs{std::string("P76")}) + "\n" +
             AdcLine("w[1]", LineAttrs{std::string("P76")})},
        {"unknown_iostd", AdcLine("w[0]", LineAttrs{std::string("P1"), std::string("LVCMOS99")})},
        {"missing_location",
         AdcLine("w[0]", LineAttrs{std::nullopt, std::string("LVCMOS33")})},
        {"missing_iostd", AdcLine("w[0]", LineAttrs{std::string("P1"), std::nullopt})},
        {"unknown_port", AdcLine("nope[0]", LineAttrs{std::string("P1")})},
        {"bit_out_of_range", AdcLine("w[7]", LineAttrs{std::string("P1")})},
        {"scalar_with_bit", AdcLine("sin[0]", LineAttrs{std::string("P1")})},
        {"unknown_command",
         "set_pin_assignment_v2\t{ w[0] }\t{ LOCATION = P1; IOSTANDARD = LVCMOS33; }"},
        {"missing_port_argument", "set_pin_assignment\t{ LOCATION = P1; }"},
        {"unknown_attr", "set_pin_assignment\t{ w[0] }\t{ LOCATION = P1; BOGUS = 1; }"},
        {"dup_attr",
         "set_pin_assignment\t{ w[0] }\t{ LOCATION = P1; LOCATION = P2; "
         "IOSTANDARD = LVCMOS33; }"},
        {"nested_braces", "set_pin_assignment { w[0] } { LOCATION = P1; } { x { y } }"},
        {"garbage", "set_pin_assignment {\n}}}"},
    };
    Json synthetic_json = Json::array();
    for (const auto& [name, case_text] : synthetic) {
        const auto case_entries = fpgalsp::constraints::adc::Parse(case_text);
        Json item = Json::object();
        item["name"] = name;
        item["text"] = case_text;
        item["entries"] = fpgalsp::constraints::adc::ToJson(case_entries);
        item["coverage"] = fpgalsp::constraints::adc::ToJson(
            fpgalsp::constraints::adc::Coverage(case_entries, TopPorts()));
        item["diagnostics"] = fpgalsp::lsp::ToJson(
            fpgalsp::constraints::adc::Diagnostics(case_entries, TopPorts(), nullptr, case_text));
        synthetic_json.push_back(std::move(item));
    }
    out["synthetic"] = std::move(synthetic_json);

    out["entries"] = fpgalsp::constraints::adc::ToJson(entries);
    out["coverage"] = fpgalsp::constraints::adc::ToJson(
        fpgalsp::constraints::adc::Coverage(entries, TopPorts(), "top"));
    out["diagnostics"] = fpgalsp::lsp::ToJson(
        fpgalsp::constraints::adc::Diagnostics(entries, TopPorts(), nullptr, *text));

    std::ofstream file(*target, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(file.good()) << "无法写入 " << *target;
    file << out.dump(2) << "\n";
}