// =============================================================================
//  tests/unit/sdc_test.cpp —— SDC 时序约束解析 / 诊断 / 补全
// =============================================================================
//  语料与期望值来源：
//    · tests/gate-corpus/corpus_alpha/constrains/top.sdc（真实工程副本）
//    · %PYREF%\tests\test_sdc.py（Python 版自己的单测，行为契约）
//
//  ⚠ 本文件里有两条测试**故意钉死 Python 缺陷**（P-11 `-period nan`、
//    P-12 不校验时钟名）。修掉 Python 缺陷时它们会变红 —— 那是预期的。
// =============================================================================

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "constraints/sdc.h"

namespace {

using fpgalsp::constraints::Json;
using fpgalsp::constraints::Port;
using fpgalsp::constraints::sdc::Command;
using fpgalsp::constraints::sdc::TokenKind;
using fpgalsp::lsp::Diagnostic;

constexpr const char* kRealSdcPath =
    FPGA_LSP_SOURCE_DIR "/tests/gate-corpus/corpus_alpha/constrains/top.sdc";

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
Port MakePort(std::string name, int width = 1) {
    Port port;
    port.name = std::move(name);
    port.direction = "input";
    port.width = width;
    if (width > 1) {
        port.msb = width - 1;
        port.lsb = 0;
    }
    port.character = 9;
    port.file = "top.v";
    return port;
}

std::vector<Port> Ports() {
    return {MakePort("clk_input"), MakePort("w", 3), MakePort("relay", 4), MakePort("pwmG", 6)};
}

std::vector<Diagnostic> ByCode(const std::vector<Diagnostic>& diagnostics,
                               std::string_view code) {
    std::vector<Diagnostic> out;
    for (const Diagnostic& diagnostic : diagnostics) {
        if (diagnostic.code == code) out.push_back(diagnostic);
    }
    return out;
}

std::vector<Diagnostic> RunDiagnostics(const std::string& text,
                                       const std::vector<Port>& ports = Ports(),
                                       const std::set<std::string>* known = nullptr) {
    return fpgalsp::constraints::sdc::Diagnostics(fpgalsp::constraints::sdc::Parse(text), ports,
                                                  known);
}

std::string JoinCommands(const std::vector<Command>& commands) {
    std::string out;
    for (size_t i = 0; i < commands.size(); ++i) {
        if (i != 0) out.push_back('|');
        out += std::to_string(commands[i].line) + ":" + commands[i].command;
        for (const std::string& argument : commands[i].args) out += " " + argument;
    }
    return out;
}

const std::set<std::string> kKnownSubset = {"create_clock", "derive_pll_clocks"};

}  // namespace

// =============================================================================
//  解析
// =============================================================================

TEST(SdcParse, ExampleCommand) {
    const std::string text =
        "create_clock -name clk_input -period 31.25 -waveform {0 15.625} "
        "[get_ports {clk_input}]\n";
    const std::vector<Command> commands = fpgalsp::constraints::sdc::Parse(text);
    ASSERT_EQ(commands.size(), 1u);
    const Command& command = commands.front();
    EXPECT_EQ(command.line, 0);
    EXPECT_EQ(command.command, "create_clock");
    const std::vector<std::string> expected = {
        "-name",       "clk_input", "-period",     "31.25",  "-waveform",
        "0 15.625",    "[get_ports {clk_input}]",
    };
    EXPECT_EQ(command.args, expected);
    EXPECT_EQ(command.raw, text.substr(0, text.size() - 1));
    EXPECT_TRUE(command.syntax.empty());
    EXPECT_EQ(command.command_span.start, 0);
    EXPECT_EQ(command.command_span.end, 12);
}

TEST(SdcParse, ArgTokenKindsAndSpans) {
    const std::string text =
        "create_clock -name clk -period 10 -waveform {0 5} [get_ports {clk}]\n";
    const std::vector<Command> commands = fpgalsp::constraints::sdc::Parse(text);
    ASSERT_EQ(commands.size(), 1u);
    const Command& command = commands.front();
    ASSERT_EQ(command.arg_tokens.size(), 7u);
    EXPECT_EQ(command.arg_tokens[0].kind, TokenKind::kWord);
    EXPECT_EQ(command.arg_tokens[4].kind, TokenKind::kWord);   // -waveform
    EXPECT_EQ(command.arg_tokens[5].kind, TokenKind::kBrace);
    EXPECT_EQ(command.arg_tokens[5].text, "{0 5}");
    EXPECT_EQ(command.arg_tokens[5].inner_text, "0 5");
    EXPECT_EQ(command.arg_tokens[6].kind, TokenKind::kBracket);
    EXPECT_EQ(command.arg_tokens[6].inner_text, "get_ports {clk}");
    // brace 的 span 覆盖含花括号的原文
    EXPECT_EQ(command.arg_tokens[5].start, static_cast<int>(text.find("{0 5}")));
    EXPECT_EQ(command.arg_tokens[5].end, static_cast<int>(text.find("{0 5}")) + 5);
}

TEST(SdcParse, CommentsBlankLinesAndSemicolons) {
    const std::string text =
        "# a comment\n"
        "\n"
        "# create_clock -period 0 [get_ports {bogus}]\n"
        "create_clock -period 10 [get_ports {clk_input}] ; # trailing\n"
        "derive_pll_clocks\n";
    const std::vector<Command> commands = fpgalsp::constraints::sdc::Parse(text);
    ASSERT_EQ(commands.size(), 2u);
    EXPECT_EQ(commands[0].command, "create_clock");
    EXPECT_EQ(commands[0].line, 3);
    EXPECT_EQ(commands[1].command, "derive_pll_clocks");
    EXPECT_EQ(commands[1].line, 4);
}

TEST(SdcParse, MultipleCommandsOnOneLine) {
    const std::string text =
        "set_false_path -from [get_ports {w[0]}]; set_false_path -to [get_ports {relay[0]}]\n";
    const std::vector<Command> commands = fpgalsp::constraints::sdc::Parse(text);
    ASSERT_EQ(commands.size(), 2u);
    EXPECT_EQ(commands[0].args.at(1), "[get_ports {w[0]}]");
    EXPECT_EQ(commands[1].args.at(1), "[get_ports {relay[0]}]");
    EXPECT_EQ(commands[0].line, 0);
    EXPECT_EQ(commands[1].line, 0);
}

TEST(SdcParse, BackslashContinuationJoinsLogicalLine) {
    const std::string text =
        "# comment\n"
        "create_clock -period 10 \\\n"
        "    -name clk [get_ports {clk_input}]\n"
        "derive_pll_clocks ; # tail\n";
    const std::vector<Command> commands = fpgalsp::constraints::sdc::Parse(text);
    ASSERT_EQ(commands.size(), 2u);
    EXPECT_EQ(commands[0].command, "create_clock");
    EXPECT_EQ(commands[0].line, 1);  // 指向逻辑行的首行
    EXPECT_NE(std::find(commands[0].args.begin(), commands[0].args.end(), "-name"),
              commands[0].args.end());
    EXPECT_NE(std::find(commands[0].args.begin(), commands[0].args.end(), "clk"),
              commands[0].args.end());
    EXPECT_EQ(commands[1].command, "derive_pll_clocks");
    EXPECT_EQ(commands[1].line, 3);
}

TEST(SdcParse, HashInsideBracesIsNotAComment) {
    const std::string text = "set_false_path -from [get_ports {w[0]}] # real comment\n";
    const std::vector<Command> commands = fpgalsp::constraints::sdc::Parse(text);
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0].raw, "set_false_path -from [get_ports {w[0]}] ");
}

TEST(SdcParse, QuoteToken) {
    const std::string text = "set_clock_groups -asynchronous -group \"clk_a clk_b\"\n";
    const std::vector<Command> commands = fpgalsp::constraints::sdc::Parse(text);
    ASSERT_EQ(commands.size(), 1u);
    ASSERT_GE(commands[0].arg_tokens.size(), 3u);
    EXPECT_EQ(commands[0].arg_tokens.back().kind, TokenKind::kQuote);
    EXPECT_EQ(commands[0].args.back(), "clk_a clk_b");
}

TEST(SdcParse, UnclosedBraceProducesSyntaxError) {
    const std::string text = "create_clock -period 10 -waveform {0 5\n";
    const std::vector<Command> commands = fpgalsp::constraints::sdc::Parse(text);
    ASSERT_EQ(commands.size(), 1u);
    ASSERT_EQ(commands[0].syntax.size(), 1u);
    EXPECT_EQ(commands[0].syntax[0].message, "Unclosed '{'");
    EXPECT_EQ(commands[0].syntax[0].character, static_cast<int>(text.find('{')));
    EXPECT_EQ(commands[0].arg_tokens.back().closed, false);
}

TEST(SdcParse, UnclosedBracketProducesSyntaxError) {
    const std::string text = "create_clock -period 10 [get_ports {clk_input}\n";
    const std::vector<Command> commands = fpgalsp::constraints::sdc::Parse(text);
    ASSERT_EQ(commands.size(), 1u);
    ASSERT_EQ(commands[0].syntax.size(), 1u);
    EXPECT_EQ(commands[0].syntax[0].message, "Unclosed '['");
}

TEST(SdcParse, UnmatchedClosersProduceSyntaxErrors) {
    for (const std::string& text : {"create_clock -period 10 }\n", "]\n"}) {
        const std::vector<Command> commands = fpgalsp::constraints::sdc::Parse(text);
        ASSERT_EQ(commands.size(), 1u) << text;
        ASSERT_EQ(commands[0].syntax.size(), 1u) << text;
    }
    EXPECT_EQ(fpgalsp::constraints::sdc::Parse("]\n").front().syntax[0].message,
              "Unmatched ']'");
}

TEST(SdcParse, UnclosedQuoteIsNotAnError) {
    // Python 对未闭合引号**不报错**，只把 closed 置 False（照抄）
    const std::vector<Command> commands = fpgalsp::constraints::sdc::Parse("\"unterminated\n");
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_TRUE(commands[0].syntax.empty());
    EXPECT_EQ(commands[0].arg_tokens.size(), 0u);
    EXPECT_EQ(commands[0].command, "unterminated");
}

TEST(SdcParse, EmptyAndCommentOnlyText) {
    EXPECT_TRUE(fpgalsp::constraints::sdc::Parse("").empty());
    EXPECT_TRUE(fpgalsp::constraints::sdc::Parse("\n\n  \n").empty());
    EXPECT_TRUE(fpgalsp::constraints::sdc::Parse("# only a comment\n").empty());
}

TEST(SdcParse, GarbageNeverThrows) {
    const std::vector<std::string> inputs = {
        "",
        "# only a comment\n",
        "{",
        "[",
        "]",
        "}",
        "\"unterminated",
        "a { b } c",
        "create_clock -period 10 \\",
        std::string("\x00\xEF\xBF\xBD", 4),
        std::string(2048, '['),
        "{{{{{{}}}}}}",
    };
    for (const std::string& input : inputs) {
        const auto commands = fpgalsp::constraints::sdc::Parse(input);
        const auto json = fpgalsp::constraints::sdc::ToJson(commands);
        EXPECT_TRUE(json.is_array()) << input;
    }
}

TEST(SdcParse, CrlfLineEndings) {
    const std::vector<Command> commands = fpgalsp::constraints::sdc::Parse(
        "create_clock -period 10 [get_ports {clk_input}]\r\n"
        "derive_pll_clocks\r\n");
    ASSERT_EQ(commands.size(), 2u);
    EXPECT_EQ(commands[0].command, "create_clock");
    EXPECT_EQ(commands[1].line, 1);
    EXPECT_EQ(commands[1].args.size(), 0u);
}

TEST(SdcParse, RealCorpusMatchesPythonGolden) {
    const std::optional<std::string> text = ReadTextFile(kRealSdcPath);
    if (!text.has_value()) GTEST_SKIP() << "语料需自备，缺少 " << kRealSdcPath;
    const std::vector<Command> commands = fpgalsp::constraints::sdc::Parse(*text);
    ASSERT_EQ(commands.size(), 2u);

    // Python: [(0, 'create_clock', ['-name','clk_input','-period','40','-waveform','0 20',
    //                               '[get_ports {clk_input}]'], []),
    //          (1, 'derive_pll_clocks', [], [])]
    EXPECT_EQ(commands[0].line, 0);
    EXPECT_EQ(commands[0].command, "create_clock");
    const std::vector<std::string> expected_args = {
        "-name", "clk_input", "-period", "40", "-waveform", "0 20", "[get_ports {clk_input}]"};
    EXPECT_EQ(commands[0].args, expected_args);
    EXPECT_EQ(commands[0].command_span.start, 0);
    EXPECT_EQ(commands[0].command_span.end, 12);
    EXPECT_TRUE(commands[0].syntax.empty());

    EXPECT_EQ(commands[1].line, 1);
    EXPECT_EQ(commands[1].command, "derive_pll_clocks");
    EXPECT_TRUE(commands[1].args.empty());
    EXPECT_EQ(commands[1].command_span.start, 0);
    EXPECT_EQ(commands[1].command_span.end, 17);
    EXPECT_TRUE(commands[1].syntax.empty());
}

// =============================================================================
//  诊断
// =============================================================================

TEST(SdcDiagnostics, UnknownPortInGetPorts) {
    const std::string text = "create_clock -name clk -period 10 [get_ports {nope}]\n";
    const auto diagnostics = RunDiagnostics(text);
    const auto found = ByCode(diagnostics, "SDC-PORT");
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].severity, fpgalsp::lsp::kError);
    EXPECT_EQ(found[0].range.start.line, 0);
    EXPECT_EQ(found[0].range.start.character, static_cast<int>(text.find("nope")));
}

TEST(SdcDiagnostics, GetPortsWithoutBraces) {
    const std::string text =
        "create_clock -period 10 [get_ports clk_input]\n"
        "set_false_path -from [get_ports nope]\n";
    const auto found = ByCode(RunDiagnostics(text), "SDC-PORT");
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].range.start.line, 1);
    // 列号是**逻辑行内**的 0-based 偏移（LSP 语义，Python 的 token.start 同样如此）
    const std::string second_line = "set_false_path -from [get_ports nope]";
    EXPECT_EQ(found[0].range.start.character, static_cast<int>(second_line.find("nope")));
}

TEST(SdcDiagnostics, BusPortPatterns) {
    const std::string valid =
        "create_clock -period 10 [get_ports {clk_input}]\n"
        "set_false_path -from [get_ports {pwmG[5:0] relay[3:0] w[*]}]\n";
    EXPECT_TRUE(ByCode(RunDiagnostics(valid), "SDC-PORT").empty());

    const std::string invalid =
        "create_clock -period 10 [get_ports {clk_input}]\n"
        "set_false_path -from [get_ports {pwmG[7] w[3:0] relay[0:2]}]\n";
    const auto found = ByCode(RunDiagnostics(invalid), "SDC-PORT");
    // pwmG[7] 越界、w[3:0] 超过 3 bit 端口；relay[0:2] 合法
    ASSERT_EQ(found.size(), 2u);
    const std::string second_line =
        "set_false_path -from [get_ports {pwmG[7] w[3:0] relay[0:2]}]";
    std::set<int> starts;
    for (const Diagnostic& diagnostic : found) {
        EXPECT_EQ(diagnostic.range.start.line, 1);
        starts.insert(diagnostic.range.start.character);
    }
    const std::set<int> expected = {static_cast<int>(second_line.find("pwmG[7]")),
                                    static_cast<int>(second_line.find("w[3:0]"))};
    EXPECT_EQ(starts, expected);
}

TEST(SdcDiagnostics, FilterAndRegexpAreNotValidated) {
    const std::string text =
        "create_clock -period 10 [get_ports {clk_input}]\n"
        "set_false_path -from [get_ports -filter {name =~ clk*}]\n"
        "set_false_path -to [get_ports -regexp {.*_clk}]\n";
    EXPECT_TRUE(ByCode(RunDiagnostics(text), "SDC-PORT").empty());
}

TEST(SdcDiagnostics, MissingPeriod) {
    const std::string text = "create_clock -name clk [get_ports {clk_input}]\n";
    const auto found = ByCode(RunDiagnostics(text), "SDC-PERIOD");
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].severity, fpgalsp::lsp::kError);
    EXPECT_NE(found[0].message.find("missing -period"), std::string::npos);
}

TEST(SdcDiagnostics, BadPeriodValues) {
    const std::string text =
        "create_clock -period 0 [get_ports {clk_input}]\n"
        "create_clock -period abc [get_ports {clk_input}]\n"
        "create_clock -period -5 [get_ports {clk_input}]\n";
    const auto diagnostics = RunDiagnostics(text);
    const auto found = ByCode(diagnostics, "SDC-PERIOD");
    ASSERT_EQ(found.size(), 3u);
    EXPECT_EQ(found[0].range.start.line, 0);
    EXPECT_EQ(found[1].range.start.line, 1);
    EXPECT_EQ(found[2].range.start.line, 2);
    for (const Diagnostic& diagnostic : found) {
        EXPECT_EQ(diagnostic.severity, fpgalsp::lsp::kError);
    }
    // "-5" 不能被杀成"未知选项"
    EXPECT_TRUE(ByCode(diagnostics, "SDC-OPT").empty());
    // Python 的 repr 用单引号
    EXPECT_NE(found[1].message.find("'abc'"), std::string::npos) << found[1].message;
}

TEST(SdcDiagnostics, PeriodNaNIsSilentlyAccepted) {
    // ⚠ P-11（见 PYTHON-BUGS.md）：Python 用 `number <= 0` 判断合法性，
    //   而 NaN 与任何数比较都是 False -> `-period nan` 静默通过。
    //   忠实复现；修掉后本测试会变红（那时应改为报 SDC-PERIOD）。
    for (const std::string& value : {"nan", "NaN", "inf", "infinity"}) {
        const std::string text =
            "create_clock -period " + value + " [get_ports {clk_input}]\n";
        const auto diagnostics = RunDiagnostics(text);
        EXPECT_TRUE(ByCode(diagnostics, "SDC-PERIOD").empty())
            << "-period " << value << " 在 Python 版里不会报错（-period 的值是 float）";
    }
}

TEST(SdcDiagnostics, UnbalancedBracesAndBrackets) {
    const std::vector<std::pair<std::string, int>> cases = {
        {"create_clock -period 10 [get_ports {clk_input}\n", 0},
        {"create_clock -period 10 -waveform {0 5\n", 0},
        {"create_clock -period 10 }\n", 0},
        {"derive_pll_clocks\ncreate_clock -period 10 [get_ports {clk_input}\n", 1},
    };
    for (const auto& [text, expected_line] : cases) {
        const auto found = ByCode(RunDiagnostics(text), "SDC-SYNTAX");
        ASSERT_FALSE(found.empty()) << text;
        EXPECT_EQ(found[0].severity, fpgalsp::lsp::kError);
        EXPECT_EQ(found[0].range.start.line, expected_line) << text;
    }
}

TEST(SdcDiagnostics, EmptyFileHasNoClock) {
    for (const std::string& text : {"", "\n\n", "# only comments\n"}) {
        const auto found = ByCode(RunDiagnostics(text), "SDC-NOCLOCK");
        ASSERT_EQ(found.size(), 1u) << text;
        EXPECT_EQ(found[0].severity, fpgalsp::lsp::kWarning);
        EXPECT_EQ(found[0].range.start.line, 0);
    }
}

TEST(SdcDiagnostics, ClockPresentSuppressesNoClock) {
    for (const std::string& text :
         {"derive_pll_clocks\n",
          "create_clock -period 10 [get_ports {clk_input}]\n",
          "create_generated_clock -name g -source [get_ports {clk_input}] -divide_by 2\n"}) {
        EXPECT_TRUE(ByCode(RunDiagnostics(text), "SDC-NOCLOCK").empty()) << text;
    }
}

TEST(SdcDiagnostics, DelayWithoutClockOption) {
    const std::string without = "set_input_delay 1.0 [get_ports {clk_input}]\n";
    const auto diagnostics = RunDiagnostics(without);
    const auto found = ByCode(diagnostics, "SDC-DELAY");
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].severity, fpgalsp::lsp::kWarning);
    // 顺带钉死：没有 create_clock 的文件同时还有 SDC-NOCLOCK（Python 同）
    EXPECT_EQ(ByCode(diagnostics, "SDC-NOCLOCK").size(), 1u);

    const std::string good =
        "create_clock -period 10 [get_ports {clk_input}]\n"
        "set_input_delay 1.0 -clock clk_input [get_ports {clk_input}]\n"
        "set_output_delay 1.0 -clock clk_input [get_ports {relay[0]}]\n";
    EXPECT_TRUE(RunDiagnostics(good).empty());
}

TEST(SdcDiagnostics, UnknownOptionIsHint) {
    const std::string text = "create_clock -period 10 -name clk -bogus 1\n";
    const auto found = ByCode(RunDiagnostics(text), "SDC-OPT");
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].severity, fpgalsp::lsp::kInfo);
    EXPECT_EQ(found[0].range.start.character, static_cast<int>(text.find("-bogus")));
}

TEST(SdcDiagnostics, UnknownCommandRequiresKnownList) {
    const std::string text = "set_bogus 1\n";
    EXPECT_TRUE(ByCode(RunDiagnostics(text), "SDC-CMD").empty());
    const auto diagnostics = RunDiagnostics(text, Ports(), &kKnownSubset);
    const auto found = ByCode(diagnostics, "SDC-CMD");
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].severity, fpgalsp::lsp::kWarning);
    EXPECT_EQ(found[0].range.start.line, 0);
}

TEST(SdcDiagnostics, StandaloneWithoutTopPortsIsSilent) {
    const std::string text = "create_clock -period 10 [get_ports {nope}]\n";
    EXPECT_TRUE(fpgalsp::constraints::sdc::Diagnostics(
                    fpgalsp::constraints::sdc::Parse(text), {})
                    .empty());
}

TEST(SdcDiagnostics, HandBuiltCommandsAreAccepted) {
    // diagnostics() 必须能吃"只有契约字段"的手工命令（Python 有同名用例）
    std::vector<Command> commands;
    {
        Command command;
        command.line = 0;
        command.command = "create_clock";
        command.args = {"-name", "clk", "-bogus", "1"};
        command.raw = "create_clock -name clk -bogus 1 [get_ports {nope}]";
        commands.push_back(command);
    }
    {
        Command command;
        command.line = 1;
        command.command = "create_clock";
        command.args = {"-period", "10", "[get_ports {nope}]"};
        command.raw = "create_clock -period 10 [get_ports {nope}]";
        commands.push_back(command);
    }
    const auto diagnostics = fpgalsp::constraints::sdc::Diagnostics(commands, Ports());
    EXPECT_EQ(ByCode(diagnostics, "SDC-PERIOD").size(), 1u);
    ASSERT_EQ(ByCode(diagnostics, "SDC-PERIOD").size(), 1u);
    EXPECT_EQ(ByCode(diagnostics, "SDC-PERIOD")[0].range.start.line, 0);
    EXPECT_EQ(ByCode(diagnostics, "SDC-OPT").size(), 1u);
    ASSERT_EQ(ByCode(diagnostics, "SDC-PORT").size(), 1u);
    EXPECT_EQ(ByCode(diagnostics, "SDC-PORT")[0].range.start.line, 1);
}

TEST(SdcDiagnostics, RealCorpusIsClean) {
    const std::optional<std::string> text = ReadTextFile(kRealSdcPath);
    if (!text.has_value()) GTEST_SKIP() << "语料需自备，缺少 " << kRealSdcPath;
    const auto diagnostics = fpgalsp::constraints::sdc::Diagnostics(
        fpgalsp::constraints::sdc::Parse(*text), Ports(), &kKnownSubset);
    EXPECT_TRUE(diagnostics.empty())
        << "真实语料不应有诊断；第一条是 " << (diagnostics.empty() ? "" : diagnostics.front().code);
}

TEST(SdcDiagnostics, NeverThrows) {
    for (const std::string& text : std::vector<std::string>{
             "", "{", "[", "]", "}", "create_clock", "garbage {[}]",
             std::string("\x00", 1)}) {
        const auto commands = fpgalsp::constraints::sdc::Parse(text);
        EXPECT_TRUE(fpgalsp::lsp::ToJson(fpgalsp::constraints::sdc::Diagnostics(commands,
                                                                               Ports()))
                        .is_array());
        EXPECT_TRUE(fpgalsp::lsp::ToJson(fpgalsp::constraints::sdc::Diagnostics(commands, {}))
                        .is_array());
        EXPECT_TRUE(fpgalsp::constraints::sdc::UndefinedClockDiagnostics(commands).size() <= 8u);
    }
}

TEST(SdcDiagnostics, SortedByLineThenCharacter) {
    const std::string text =
        "create_clock -period 0 [get_ports {nope}]\n"
        "set_input_delay 1.0 [get_ports {clk_input}]\n";
    const auto diagnostics = RunDiagnostics(text);
    ASSERT_GE(diagnostics.size(), 3u);
    for (size_t index = 1; index < diagnostics.size(); ++index) {
        const auto& previous = diagnostics[index - 1].range.start;
        const auto& current = diagnostics[index].range.start;
        EXPECT_TRUE(previous.line < current.line ||
                    (previous.line == current.line && previous.character <= current.character));
    }
}

// =============================================================================
//  附加诊断：未定义的时钟名（SDC-CLOCK，Python 版没有）
// =============================================================================

TEST(SdcUndefinedClocks, FlagsUnknownClockReference) {
    const std::string text =
        "create_clock -name clk_input -period 10 [get_ports {clk_input}]\n"
        "set_input_delay 1.0 -clock clk_typo [get_ports {clk_input}]\n";
    const auto diagnostics = fpgalsp::constraints::sdc::UndefinedClockDiagnostics(
        fpgalsp::constraints::sdc::Parse(text));
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].code, "SDC-CLOCK");
    EXPECT_EQ(diagnostics[0].severity, fpgalsp::lsp::kWarning);
    EXPECT_EQ(diagnostics[0].range.start.line, 1);
    EXPECT_NE(diagnostics[0].message.find("clk_typo"), std::string::npos);
}

TEST(SdcUndefinedClocks, SilentWhenNoClockIsCreated) {
    // 时钟可能来自 derive_pll_clocks 或被 include 的其它 sdc —— 不做判断（避免误报）
    const std::string text =
        "derive_pll_clocks\n"
        "set_input_delay 1.0 -clock clk_50m [get_ports {clk_input}]\n";
    EXPECT_TRUE(fpgalsp::constraints::sdc::UndefinedClockDiagnostics(
                    fpgalsp::constraints::sdc::Parse(text))
                    .empty());
}

TEST(SdcUndefinedClocks, SkipsBracketValuesAndWildcards) {
    const std::string text =
        "create_clock -name clk_input -period 10 [get_ports {clk_input}]\n"
        "set_false_path -from [get_clocks {clk_*}] -to [get_clocks {clk_input}]\n";
    const auto diagnostics = fpgalsp::constraints::sdc::UndefinedClockDiagnostics(
        fpgalsp::constraints::sdc::Parse(text));
    EXPECT_TRUE(diagnostics.empty()) << "通配符与已定义时钟都不该报";
}

TEST(SdcUndefinedClocks, FlagsUnknownGetClocksName) {
    const std::string text =
        "create_clock -name clk_input -period 10 [get_ports {clk_input}]\n"
        "set_clock_groups -asynchronous -group [get_clocks {clk_input}] "
        "-group [get_clocks {clk_other}]\n";
    const auto diagnostics = fpgalsp::constraints::sdc::UndefinedClockDiagnostics(
        fpgalsp::constraints::sdc::Parse(text));
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_NE(diagnostics[0].message.find("clk_other"), std::string::npos);
}

TEST(SdcUndefinedClocks, GeneratedClockNameCountsAsDefined) {
    const std::string text =
        "create_generated_clock -name clk_div2 -source [get_ports {clk_input}] -divide_by 2\n"
        "set_output_delay 2.0 -clock clk_div2 [get_ports {relay[0]}]\n";
    EXPECT_TRUE(fpgalsp::constraints::sdc::UndefinedClockDiagnostics(
                    fpgalsp::constraints::sdc::Parse(text))
                    .empty());
}

TEST(SdcUndefinedClocks, PythonReferenceHasNoSuchCheck) {
    // ⚠ P-12（见 PYTHON-BUGS.md）：Python 的 sdc.diagnostics() 完全不校验时钟名，
    //   `-clock bogus` 静默通过。为保持与 Python 逐字段等价，Diagnostics() 也不报；
    //   这条检查单独放在 UndefinedClockDiagnostics()。修掉 Python 缺陷后，
    //   应把该检查并入 Diagnostics()，届时本测试要改。
    const std::string text =
        "create_clock -name clk_input -period 10 [get_ports {clk_input}]\n"
        "set_input_delay 1.0 -clock bogus [get_ports {clk_input}]\n";
    const auto commands = fpgalsp::constraints::sdc::Parse(text);
    EXPECT_TRUE(ByCode(fpgalsp::constraints::sdc::Diagnostics(commands, Ports()), "SDC-CLOCK")
                    .empty());
    EXPECT_EQ(fpgalsp::constraints::sdc::UndefinedClockDiagnostics(commands).size(), 1u);
}

// =============================================================================
//  光标上下文
// =============================================================================

namespace {

const char* kContextText =
    "create_clock -name clk_input -period 31.25 -waveform {0 15.625} "
    "[get_ports {clk_input}]\n"
    "derive_pll_clocks\n";

}  // namespace

TEST(SdcContext, CommandOptionClockNameAndPort) {
    const std::string text = kContextText;
    const std::string raw = text.substr(0, text.find('\n'));
    {
        const auto context = fpgalsp::constraints::sdc::Context(text, 0, 2);
        ASSERT_TRUE(context.has_value());
        EXPECT_EQ(context->kind, "command");
        EXPECT_EQ(context->span.start, 0);
        EXPECT_EQ(context->span.end, 12);
    }
    {
        const auto context =
            fpgalsp::constraints::sdc::Context(text, 0, static_cast<int>(raw.find("-period")) + 1);
        ASSERT_TRUE(context.has_value());
        EXPECT_EQ(context->kind, "option");
        EXPECT_EQ(context->span.start, static_cast<int>(raw.find("-period")));
        EXPECT_EQ(context->command, "create_clock");
    }
    {
        const auto context = fpgalsp::constraints::sdc::Context(
            text, 0, static_cast<int>(raw.find("clk_input")) + 1);
        ASSERT_TRUE(context.has_value());
        EXPECT_EQ(context->kind, "clock_name");
        EXPECT_EQ(context->span.start, static_cast<int>(raw.find("clk_input")));
        EXPECT_EQ(context->span.end, static_cast<int>(raw.find("clk_input")) + 9);
    }
    {
        // get_ports 本身是一个 Tcl 命令
        const auto context = fpgalsp::constraints::sdc::Context(
            text, 0, static_cast<int>(raw.find("get_ports")) + 1);
        ASSERT_TRUE(context.has_value());
        EXPECT_EQ(context->kind, "command");
    }
    {
        // 括号里的 clk_input 是端口
        const int at = static_cast<int>(raw.rfind("clk_input"));
        const auto context = fpgalsp::constraints::sdc::Context(text, 0, at + 1);
        ASSERT_TRUE(context.has_value());
        EXPECT_EQ(context->kind, "port");
        EXPECT_EQ(context->span.start, at);
        EXPECT_EQ(context->span.end, at + 9);
    }
}

TEST(SdcContext, PortSpanIsBaseNameForBus) {
    const std::string text = "set_false_path -from [get_ports {pwmG[3:0]}]\n";
    const std::string raw = text.substr(0, text.size() - 1);
    const auto context =
        fpgalsp::constraints::sdc::Context(text, 0, static_cast<int>(raw.find("pwmG")));
    ASSERT_TRUE(context.has_value());
    EXPECT_EQ(context->kind, "port");
    EXPECT_EQ(context->span.start, static_cast<int>(raw.find("pwmG")));
    EXPECT_EQ(context->span.end, static_cast<int>(raw.find("pwmG")) + 4);
}

TEST(SdcContext, SecondLineAndNonePlaces) {
    using fpgalsp::constraints::sdc::Context;
    const auto context = Context(kContextText, 1, 2);
    ASSERT_TRUE(context.has_value());
    EXPECT_EQ(context->kind, "command");
    EXPECT_FALSE(Context(kContextText, 5, 0).has_value());
    EXPECT_FALSE(Context("", 0, 0).has_value());
    EXPECT_FALSE(Context(kContextText, 0, -1).has_value());
    EXPECT_FALSE(Context(kContextText, -1, 0).has_value());
}

TEST(SdcContext, RealCorpusPositions) {
    const std::optional<std::string> text = ReadTextFile(kRealSdcPath);
    if (!text.has_value()) GTEST_SKIP() << "语料需自备（设置 FPGA_LSP_TEST_PROJECT 指向自备工程）: " << kRealSdcPath;
    const std::string& content = *text;
    const std::string first = content.substr(0, content.find('\n'));
    const int name_at = static_cast<int>(first.find("clk_input"));
    const auto clock = fpgalsp::constraints::sdc::Context(content, 0, name_at + 1);
    ASSERT_TRUE(clock.has_value());
    EXPECT_EQ(clock->kind, "clock_name");
    const int port_at = static_cast<int>(first.rfind("clk_input"));
    const auto port = fpgalsp::constraints::sdc::Context(content, 0, port_at + 1);
    ASSERT_TRUE(port.has_value());
    EXPECT_EQ(port->kind, "port");
    const auto second = fpgalsp::constraints::sdc::Context(content, 1, 1);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->kind, "command");
}

// =============================================================================
//  补全
// =============================================================================

TEST(SdcCompletion, CommandNamesFilteredByPrefix) {
    const std::string text = "creat";
    const auto items = fpgalsp::constraints::sdc::Completion(
        text, 0, 5, fpgalsp::constraints::sdc::KnownCommands(), Ports());
    ASSERT_FALSE(items.empty());
    for (const auto& item : items) {
        EXPECT_EQ(item.label.rfind("creat", 0), 0u) << item.label;
        EXPECT_EQ(item.kind, "command");
    }
    const auto all = fpgalsp::constraints::sdc::Completion(
        "set_", 0, 4, fpgalsp::constraints::sdc::KnownCommands(), Ports());
    EXPECT_GE(all.size(), 8u);
}

TEST(SdcCompletion, ClockNamesComesFromCreateClock) {
    const std::string text =
        "create_clock -name clk_input -period 10 [get_ports {clk_input}]\n"
        "create_generated_clock -name clk_div2 -source [get_ports {clk_input}] -divide_by 2\n"
        "set_input_delay 1.0 -clock clk_in";
    const auto names = fpgalsp::constraints::sdc::DefinedClockNames(text);
    EXPECT_EQ(names, std::vector<std::string>({"clk_input", "clk_div2"}));

    // 重复定义只出现一次
    const std::string duplicated =
        "create_clock -name clk_a -period 10 [get_ports {clk_input}]\n"
        "create_clock -name clk_a -period 20 [get_ports {clk_input}]\n";
    EXPECT_EQ(fpgalsp::constraints::sdc::DefinedClockNames(duplicated),
              std::vector<std::string>({"clk_a"}));

    // 光标在 `-clock clk_in` 的末字符上 -> 给出已定义时钟名
    const std::string last_line = text.substr(text.rfind('\n') + 1);
    const int character = static_cast<int>(last_line.size() - 1);
    const auto items = fpgalsp::constraints::sdc::Completion(
        text, 2, character, fpgalsp::constraints::sdc::KnownCommands(), Ports());
    ASSERT_FALSE(items.empty());
    for (const auto& item : items) EXPECT_EQ(item.kind, "clock");
    EXPECT_EQ(items.front().label, "clk_input");
}

TEST(SdcCompletion, PortNamesInsideGetPorts) {
    const std::string text = "set_false_path -from [get_ports {wpwm";
    const auto items = fpgalsp::constraints::sdc::Completion(
        text, 0, static_cast<int>(text.size()), fpgalsp::constraints::sdc::KnownCommands(),
        Ports());
    ASSERT_FALSE(items.empty());
    bool has_clk = false;
    for (const auto& item : items) {
        EXPECT_EQ(item.kind, "port");
        if (item.label == "clk_input") has_clk = true;
    }
    EXPECT_TRUE(has_clk);
}

TEST(SdcCompletion, OptionsForCommand) {
    // 光标停在 `-c` 的末字符上：`-` 开头的裸词 -> 选项上下文
    const std::string text = "create_clock -period 10 -c";
    const auto items = fpgalsp::constraints::sdc::Completion(
        text, 0, static_cast<int>(text.size()) - 1,
        fpgalsp::constraints::sdc::KnownCommands(), Ports());
    ASSERT_FALSE(items.empty());
    bool has_period = false;
    for (const auto& item : items) {
        EXPECT_EQ(item.kind, "option");
        if (item.label == "-period") has_period = true;
    }
    EXPECT_TRUE(has_period);
}

TEST(SdcCompletion, KnownCommandsCoversModelledSet) {
    const std::set<std::string>& commands = fpgalsp::constraints::sdc::KnownCommands();
    EXPECT_EQ(commands.size(), 15u);
    for (const char* name :
         {"create_clock", "create_generated_clock", "derive_pll_clocks", "set_input_delay",
          "set_output_delay", "set_false_path", "set_multicycle_path", "set_max_delay",
          "set_min_delay", "set_clock_groups", "set_clock_uncertainty", "set_clock_latency",
          "set_clock_transition", "set_propagated_clock", "set_disable_timing"}) {
        EXPECT_TRUE(commands.count(name) == 1u) << name;
    }
    EXPECT_TRUE(fpgalsp::constraints::sdc::OptionsFor("set_bogus") == nullptr);
    ASSERT_TRUE(fpgalsp::constraints::sdc::OptionsFor("create_clock") != nullptr);
    EXPECT_TRUE(fpgalsp::constraints::sdc::OptionsFor("create_clock")->count("-period") == 1u);
}

TEST(SdcCompletion, CommandSubstitutionIsOpaque) {
    const std::string text = "set_false_path -from [get_ports {w[0]}] -to [get_ports {relay";
    const std::vector<Command> commands = fpgalsp::constraints::sdc::Parse(text);
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0].args.at(1), "[get_ports {w[0]}]");
    // 未闭合的 [ 会带一条语法诊断（不是崩溃）
    EXPECT_FALSE(commands[0].syntax.empty() || commands[0].arg_tokens.back().closed);
}

// =============================================================================
//  嵌套深度加固（SEC-03）
// =============================================================================
//  Flatten() 逐层展开 `[...]`。它现在有 64 层上限：超限时停止展开、把该
//  kBracket token 原样收进结果（有意降级），而不是继续递归把栈打爆。
//  两条测试各钉住一半语义：上限内的嵌套照常展开；超深嵌套不死循环、不崩溃。
TEST(SdcContext, NestedBracketsWithinLimitStillFlatten) {
    // 三层嵌套，远小于上限：里面的 get_ports 仍应被展开成端口上下文
    const std::string text = "set_false_path -from [foo [get_ports {wpwm}]]";
    const int at = static_cast<int>(text.find("wpwm"));
    const auto context = fpgalsp::constraints::sdc::Context(text, 0, at);
    ASSERT_TRUE(context.has_value());
    EXPECT_EQ(context->kind, "port");
    EXPECT_EQ(context->span.start, at);
    EXPECT_EQ(context->span.end, at + 4);

    // 同一位置走补全入口也应给出端口候选（入口不崩、结果可预期）
    const auto items = fpgalsp::constraints::sdc::Completion(
        text, 0, at, fpgalsp::constraints::sdc::KnownCommands(), Ports());
    ASSERT_FALSE(items.empty());
    for (const auto& item : items) EXPECT_EQ(item.kind, "port");
}

TEST(SdcContext, ThousandNestedBracketsDegradesInsteadOfCrashing) {
    // 1000 层 `[`：没上限时这会让 Flatten 递归 1000 层并每层重新 Tokenize 整段，
    // 真实攻击输入是 20 万层 -> 栈溢出。上限 64 之后这里只展开 64 层。
    constexpr int kDepth = 1000;
    std::string text = "set_false_path -from ";
    text.append(static_cast<std::size_t>(kDepth), '[');
    text += "wpwm";
    text.append(static_cast<std::size_t>(kDepth), ']');
    const int at = static_cast<int>(text.find("wpwm"));
    ASSERT_GT(at, 0);

    // 能正常解析（不抛、不崩）
    const std::vector<Command> commands = fpgalsp::constraints::sdc::Parse(text);
    ASSERT_EQ(commands.size(), 1u);

    // 降级语义：超限的 kBracket token 原样留在结果里，不再向内展开，
    // 因此光标落在超深层嵌套内部时拿不到端口上下文（而不是崩溃）。
    EXPECT_FALSE(fpgalsp::constraints::sdc::Context(text, 0, at).has_value());
    EXPECT_TRUE(fpgalsp::constraints::sdc::Completion(
                    text, 0, at, fpgalsp::constraints::sdc::KnownCommands(), Ports())
                    .empty());
}

// =============================================================================
//  JSON 形状
// =============================================================================

TEST(SdcJson, CommandShapeMatchesContract) {
    const auto commands = fpgalsp::constraints::sdc::Parse(
        "create_clock -period 10 [get_ports {clk_input}]\n");
    ASSERT_EQ(commands.size(), 1u);
    const auto json = fpgalsp::constraints::sdc::ToJson(commands.front());
    EXPECT_EQ(json.at("line").get<int>(), 0);
    EXPECT_EQ(json.at("command").get<std::string>(), "create_clock");
    EXPECT_EQ(json.at("args").size(), 3u);
    EXPECT_TRUE(json.contains("raw"));
    EXPECT_TRUE(json.at("command_span").contains("start"));
    EXPECT_TRUE(json.contains("arg_tokens"));
    EXPECT_TRUE(json.at("syntax").is_array());
}

TEST(SdcJson, SyntaxErrorsAreSerialised) {
    const auto commands = fpgalsp::constraints::sdc::Parse("create_clock -period 10 }\n");
    ASSERT_EQ(commands.size(), 1u);
    const auto json = fpgalsp::constraints::sdc::ToJson(commands);
    ASSERT_TRUE(json.is_array());
    ASSERT_EQ(json.at(0).at("syntax").size(), 1u);
}

//  与 Python 参考实现逐字段对照的导出通道（默认跳过）
// =============================================================================
//  用法：$env:FPGA_LSP_SDC_DUMP = "<path>"; .\unit_sdc.exe --gtest_filter=SdcParity.*
//  导出内容 = {commands, diagnostics}，字段与 Python 的 sdc.parse / sdc.diagnostics 一一对应。
// =============================================================================

TEST(SdcParity, DumpCorpusJsonWhenRequested) {
    const std::optional<std::string> target = ReadEnv("FPGA_LSP_SDC_DUMP");
    if (!target.has_value()) {
        GTEST_SKIP() << "设置 FPGA_LSP_SDC_DUMP=<path> 可导出与 Python 对照的 JSON";
    }
    const std::optional<std::string> text = ReadTextFile(kRealSdcPath);
    if (!text.has_value()) GTEST_SKIP() << "语料需自备（设置 FPGA_LSP_TEST_PROJECT 指向自备工程）: " << kRealSdcPath;
    const auto commands = fpgalsp::constraints::sdc::Parse(*text);

    Json out = Json::object();
    // 合成边界输入：畸形/半畸形 SDC 的解析与诊断也要与 Python 逐字段一致
    const std::vector<std::pair<std::string, std::string>> synthetic = {
        {"empty", ""},
        {"comment_only", "# nothing but a comment\n"},
        {"plain_clock", "create_clock -name clk -period 10 [get_ports {clk_input}]\n"},
        {"missing_period", "create_clock -name clk [get_ports {clk_input}]\n"},
        {"period_zero", "create_clock -period 0 [get_ports {clk_input}]\n"},
        {"period_negative", "create_clock -period -5 [get_ports {clk_input}]\n"},
        {"period_text", "create_clock -period abc [get_ports {clk_input}]\n"},
        {"unknown_port", "create_clock -period 10 [get_ports {nope}]\n"},
        {"bus_patterns",
         "create_clock -period 10 [get_ports {clk_input}]\n"
         "set_false_path -from [get_ports {pwmG[7] w[3:0] relay[0:2]}]\n"},
        {"unclosed_bracket", "create_clock -period 10 [get_ports {clk_input}\n"},
        {"unclosed_brace", "create_clock -period 10 -waveform {0 5\n"},
        {"unmatched_brace", "create_clock -period 10 }\n"},
        {"unknown_option", "create_clock -period 10 -name clk -bogus 1\n"},
        {"delay_without_clock", "set_input_delay 1.0 [get_ports {clk_input}]\n"},
        {"comment_and_semicolon",
         "create_clock -period 10 [get_ports {clk_input}] ; # trailing\n"
         "derive_pll_clocks ; set_false_path -to [get_ports {relay[0]}]\n"},
        {"continuation",
         "create_clock -period 10 \\\n    -name clk [get_ports {clk_input}]\n"},
        {"garbage", "garbage {[}]\n"},
    };
    Json synthetic_json = Json::array();
    for (const auto& [name, case_text] : synthetic) {
        const auto case_commands = fpgalsp::constraints::sdc::Parse(case_text);
        Json item = Json::object();
        item["name"] = name;
        item["text"] = case_text;
        item["commands"] = fpgalsp::constraints::sdc::ToJson(case_commands);
        item["diagnostics"] = fpgalsp::lsp::ToJson(
            fpgalsp::constraints::sdc::Diagnostics(case_commands, Ports(), &kKnownSubset));
        synthetic_json.push_back(std::move(item));
    }
    out["synthetic"] = std::move(synthetic_json);

    out["commands"] = fpgalsp::constraints::sdc::ToJson(commands);
    out["diagnostics"] = fpgalsp::lsp::ToJson(
        fpgalsp::constraints::sdc::Diagnostics(commands, Ports(), &kKnownSubset));

    std::ofstream file(*target, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(file.good()) << "无法写入 " << *target;
    file << out.dump(2) << "\n";
}