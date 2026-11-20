// =============================================================================
//  tests/unit/td_output_test.cpp
// =============================================================================
//  语料全部来自 TD 2026.2 在本机 %WORKSPACE%\CorpusAlpha 真实工程的实测输出。
// =============================================================================

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "td/td_output.h"

namespace {

using namespace fpgalsp::td;

// ---- 真实 TD 输出片段（原样保留，含 \r\n）----
// read_hdl 在 %WORKSPACE%\CorpusAlpha 上的实际输出（经 run() 后处理后的形态）
const char* kRealReadHdl =
    "RUN-1002 : start command \"read_hdl -file {%WORKSPACE%/CorpusAlpha/src/trans/trans.v} -top top\"\r\n"
    "HDL-1008 : device name is not set, so there is no need to read *macro.v\r\n"
    "HDL-1007 : analyze verilog file %WORKSPACE%/CorpusAlpha/src/trans/trans.v\r\n"
    "HDL-1007 : elaborate module top in %WORKSPACE%/CorpusAlpha/src/top.v(15)\r\n"
    "HDL-7144 CRITICAL-WARNING: instantiate unknown module pll_gen in "
    "%WORKSPACE%/CorpusAlpha/src/top.v(66)\r\n"
    "HDL-5007 WARNING: could not find top-level module/unit; aborting hierarchy tree creation\r\n";

// ---- clean_lines ----

TEST(TdCleanLines, CrlfNormalized) {
    const auto lines = CleanLines("a\r\nb\r\nc");
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0], "a");
    EXPECT_EQ(lines[1], "b");
    EXPECT_EQ(lines[2], "c");
}

TEST(TdCleanLines, LoneCrIsNewline) {
    const auto lines = CleanLines("a\rb");
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0], "a");
    EXPECT_EQ(lines[1], "b");
}

TEST(TdCleanLines, DropsBlankLines) {
    const auto lines = CleanLines("a\r\n\r\n\r\nb\r\n");
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0], "a");
    EXPECT_EQ(lines[1], "b");
}

TEST(TdCleanLines, RstripsTrailingWhitespace) {
    const auto lines = CleanLines("abc   \t\r\nx  ");
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0], "abc");
    EXPECT_EQ(lines[1], "x");
}

TEST(TdCleanLines, KeepsLeadingWhitespace) {
    const auto lines = CleanLines("   indented");
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0], "   indented");  // 只 rstrip
}

TEST(TdCleanLines, EmptyInput) {
    EXPECT_TRUE(CleanLines("").empty());
}

// ---- 真实 body 形态（lead 要求的两层分离验证）----

TEST(TdRunPostProcess, RealBodyKeepsRawShapeThenCleanLinesYields42) {
    // 实测 body（探针抓取，原样）
    const std::string body = "\r\n\r\ntd > \r\ntd > 42\r\n\r\ntd > ";

    // 第一层：Run 的后处理只做 B/E 截取 + 全局去提示符，**不做行清理**
    const std::string stripped = StripSentinelsAndPrompt(body, "probe2");
    EXPECT_EQ(stripped, "\r\n\r\n\r\n42\r\n\r\n");  // 与 Python run() 输出逐字一致

    // 第二层：CleanLines 才负责清理
    const auto lines = CleanLines(stripped);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0], "42");
}

// ---- parse_message_lines：真实语料 ----

TEST(TdParseReal, ReadHdlRealOutput) {
    const auto msgs = ParseMessageLines(kRealReadHdl);

    // RUN-1002 无位置且非 ignored -> 丢弃
    // HDL-1008 无位置且非 ignored -> 丢弃
    // "analyze verilog file ..." 无 (line) -> 丢弃
    // 剩下 3 条：elaborate top(INFO) / HDL-7144(CRITICAL-WARNING) / HDL-5007(WARNING)
    ASSERT_EQ(msgs.size(), 3u);

    // 1) elaborate module top -> INFO(3)，路径+行号
    EXPECT_EQ(msgs[0].code, "HDL-1007");
    EXPECT_EQ(msgs[0].severity, kSeverityInfo);
    EXPECT_EQ(msgs[0].message, "elaborate module top");
    ASSERT_TRUE(msgs[0].path.has_value());
    EXPECT_EQ(*msgs[0].path, "%WORKSPACE%/CorpusAlpha/src/top.v");
    ASSERT_TRUE(msgs[0].line.has_value());
    EXPECT_EQ(*msgs[0].line, 15);

    // 2) CRITICAL-WARNING -> 2
    EXPECT_EQ(msgs[1].code, "HDL-7144");
    EXPECT_EQ(msgs[1].severity, kSeverityWarning);
    EXPECT_EQ(msgs[1].message, "instantiate unknown module pll_gen");
    EXPECT_EQ(*msgs[1].path, "%WORKSPACE%/CorpusAlpha/src/top.v");
    EXPECT_EQ(*msgs[1].line, 66);

    // 3) WARNING -> 2，无位置
    EXPECT_EQ(msgs[2].code, "HDL-5007");
    EXPECT_EQ(msgs[2].severity, kSeverityWarning);
    EXPECT_FALSE(msgs[2].path.has_value());
}

TEST(TdParseReal, ElaborateWithParametersKeepsParams) {
    const std::string text =
        "HDL-1007 : elaborate module AD_ACQ(RAD_ACQ_CNT=5,AMB_ACQ_CNT=10) in "
        "%WORKSPACE%/CorpusAlpha/src/ad7924/AD_ACQ.v(30)\r\n";
    const auto msgs = ParseMessageLines(text);
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0].severity, kSeverityInfo);
    EXPECT_EQ(msgs[0].message, "elaborate module AD_ACQ(RAD_ACQ_CNT=5,AMB_ACQ_CNT=10)");
    EXPECT_EQ(*msgs[0].path, "%WORKSPACE%/CorpusAlpha/src/ad7924/AD_ACQ.v");
    EXPECT_EQ(*msgs[0].line, 30);
}

// ---- severity 规则 ----

TEST(TdParseSeverity, ExplicitError) {
    const auto msgs = ParseMessageLines("USR-8001 ERROR: Invalid device_db name");
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0].severity, kSeverityError);
    EXPECT_EQ(msgs[0].code, "USR-8001");
    EXPECT_EQ(msgs[0].message, "Invalid device_db name");
}

TEST(TdParseSeverity, CriticalWarningIsWarning) {
    const auto msgs = ParseMessageLines("HDL-7144 CRITICAL-WARNING: instantiate unknown module");
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0].severity, kSeverityWarning);
}

TEST(TdParseSeverity, PlainWarningIsWarning) {
    const auto msgs = ParseMessageLines("HDL-5007 WARNING: could not find top-level module");
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0].severity, kSeverityWarning);
}

TEST(TdParseSeverity, ExplicitInfo) {
    const auto msgs = ParseMessageLines("RUN-1001 INFO: Print Global Property");
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0].severity, kSeverityInfo);
}

TEST(TdParseSeverity, NoSeverityWithLocationIsInfo) {
    // ⚠ 0.10.23 的关键修复：无 severity -> INFO，不是 warning
    const auto msgs = ParseMessageLines("HDL-1007 : elaborate module top in Y:/a/top.v(15)");
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0].severity, kSeverityInfo);
}

TEST(TdParseSeverity, NoSeverityWithoutLocationDropped) {
    // 无 severity 且无位置的闲聊 -> 丢弃
    EXPECT_TRUE(ParseMessageLines("HDL-1007 : analyze verilog file Y:/a/b.v").empty());
    EXPECT_TRUE(ParseMessageLines("RUN-1002 : start command \"read_hdl\"").empty());
}

TEST(TdParseSeverity, SeverityColonVariants) {
    // "ERROR:" / "ERROR :" / "ERROR" 都应识别
    for (const char* text : {"PRJ-1000 ERROR: boom", "PRJ-1000 ERROR : boom",
                             "PRJ-1000 ERROR  boom"}) {
        const auto msgs = ParseMessageLines(text);
        ASSERT_EQ(msgs.size(), 1u) << text;
        EXPECT_EQ(msgs[0].severity, kSeverityError) << text;
    }
}

// ---- ignored due to errors（合成的 warning）----

TEST(TdParseIgnored, SynthesizesWarning) {
    // ⚠ 必须以 <CODE> 开头，否则 MSG_RE 不匹配（实测 Python: code=None -> 整行丢弃）
    const auto msgs = ParseMessageLines(
        "HDL-9000 file \"src/bad.v\" ignored due to errors");
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0].code, "HDL-9000");
    EXPECT_EQ(msgs[0].severity, kSeverityWarning);
    ASSERT_TRUE(msgs[0].path.has_value());
    EXPECT_EQ(*msgs[0].path, "src/bad.v");
    EXPECT_EQ(*msgs[0].line, 1);
    EXPECT_NE(msgs[0].message.find("black box"), std::string::npos);
}

TEST(TdParseIgnored, LineWithoutCodePrefixIsDropped) {
    // 实测 Python：没有 <CODE> 前缀的行连 MSG_RE 都匹配不上 -> 不产生消息
    EXPECT_TRUE(ParseMessageLines("something file \"src/bad.v\" ignored due to errors").empty());
}

TEST(TdParseIgnored, SingleQuotesAlsoMatch) {
    const auto msgs = ParseMessageLines("HDL-9000 file 'lib/x.sv' ignored due to errors");
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(*msgs[0].path, "lib/x.sv");
}

TEST(TdParseIgnored, CaseInsensitive) {
    const auto msgs = ParseMessageLines("HDL-9000 FILE 'a.v' IGNORED DUE TO ERRORS");
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0].severity, kSeverityWarning);
}

// ---- 路径形态 ----

TEST(TdParsePath, AbsoluteWindowsPath) {
    const auto msgs = ParseMessageLines("HDL-8007 ERROR: bad syntax in C:/proj/src/a.v(42)");
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(*msgs[0].path, "C:/proj/src/a.v");
    EXPECT_EQ(*msgs[0].line, 42);
    EXPECT_EQ(msgs[0].message, "bad syntax");
}

TEST(TdParsePath, BackslashPathNormalizedToForwardSlash) {
    const auto msgs =
        ParseMessageLines("HDL-8007 ERROR: bad syntax in C:\\proj\\src\\a.v(42)");
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(*msgs[0].path, "C:/proj/src/a.v");  // 反斜杠已转换
}

TEST(TdParsePath, RelativePathMatched) {
    const auto msgs = ParseMessageLines("HDL-8007 ERROR: bad syntax in src/sub/a.v(7)");
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(*msgs[0].path, "src/sub/a.v");
    EXPECT_EQ(*msgs[0].line, 7);
}

TEST(TdParsePath, StripsTrailingPreposition) {
    // "in <path>(N)" -> 去掉尾部 "in"
    const auto msgs = ParseMessageLines("HDL-8007 ERROR: foo bar in Y:/a/b.v(3)");
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0].message, "foo bar");
}

TEST(TdParsePath, VariousPrepositions) {
    for (const char* prep : {"in", "at", "from", "of"}) {
        const std::string text =
            std::string("HDL-8007 ERROR: msg ") + prep + " Y:/a/b.v(1)";
        const auto msgs = ParseMessageLines(text);
        ASSERT_EQ(msgs.size(), 1u) << text;
        EXPECT_EQ(msgs[0].message, "msg") << text;
    }
}

TEST(TdParsePath, AdcAndSdcExtensionsMatched) {
    const auto msgs = ParseMessageLines("ADC-2000 ERROR: bad pin in constrains/a.adc(3)");
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(*msgs[0].path, "constrains/a.adc");
}

// ---- code 形态 ----

TEST(TdParseCode, ShortAndLongCodesAccepted) {
    EXPECT_EQ(ParseMessageLines("A-123 INFO: x").size(), 0u);  // 无位置无 ignored -> 丢
    const auto m1 = ParseMessageLines("AB-12345 INFO: x");
    ASSERT_EQ(m1.size(), 1u);
    EXPECT_EQ(m1[0].code, "AB-12345");
}

TEST(TdParseCode, LowercaseCodeRejected) {
    // 正则要求 [A-Z] 开头
    EXPECT_TRUE(ParseMessageLines("hdl-1007 INFO: x").empty());
}

TEST(TdParseCode, MalformedLinesIgnored) {
    EXPECT_TRUE(ParseMessageLines("just some text").empty());
    EXPECT_TRUE(ParseMessageLines("").empty());
    EXPECT_TRUE(ParseMessageLines("td > ").empty());
}

// ---- has_error / has_fatal ----

TEST(TdHasError, DetectsErrorAndCriticalWarning) {
    EXPECT_TRUE(HasError("USR-8001 ERROR: boom"));
    EXPECT_TRUE(HasError("HDL-7144 CRITICAL-WARNING: x"));
    EXPECT_TRUE(HasError("prefix HDL-7144 CRITICAL-WARNING: x suffix"));
}

TEST(TdHasError, IgnoresPlainWarningAndInfo) {
    EXPECT_FALSE(HasError("HDL-5007 WARNING: x"));
    EXPECT_FALSE(HasError("HDL-1007 : analyze file"));
    EXPECT_FALSE(HasError("no codes here"));
}

TEST(TdHasFatal, OnlyHardError) {
    EXPECT_TRUE(HasFatal("USR-8001 ERROR: boom"));
    // CRITICAL-WARNING 不算 fatal（flow 步骤里 USR-6136 是噪音）
    EXPECT_FALSE(HasFatal("HDL-7144 CRITICAL-WARNING: x"));
    EXPECT_FALSE(HasFatal("HDL-5007 WARNING: x"));
}

TEST(TdHasFatal, WordBoundaryRequired) {
    // ⚠ 实测 Python：\b 在 "-" 与数字之间也成立，所以 "XUSR-8001 ERROR" **仍然匹配**。
    //   真正不匹配的是「数字紧跟」的形态，因为 [A-Z] 首字符前需要边界。
    EXPECT_TRUE(HasFatal("XUSR-8001 ERROR: boom"));   // Python: True
    EXPECT_FALSE(HasFatal("1USR-8001 ERROR: boom"));  // 前面是数字，无 \b
}

// ---- has_notice / notice_lines（P0-2：没有 severity 词的 TD 消息）----
//  实测（TD 2026.2，agent 改完探针清单后跑 flow）：
//      KIT-2501 : No mark_debug exists in the netlist.
//  这一行里没有 ERROR / CRITICAL-WARNING，HasError / HasFatal 都看不见，
//  所以 flow 会"自信地成功"。Notice 专门捞这种行。

TEST(TdHasNotice, Kit2501IsNotice) {
    const std::string text = "KIT-2501 : No mark_debug exists in the netlist.";
    EXPECT_TRUE(HasNotice(text));
    EXPECT_EQ(NoticeLines(text), "KIT-2501 : No mark_debug exists in the netlist.");
}

TEST(TdHasNotice, OtherKitCodesAreNoticesToo) {
    // 实测同形态的其它信息码（KIT-1004 / KIT-8414）
    EXPECT_TRUE(HasNotice("KIT-1004 : no ChipWatcher instance"));
    EXPECT_TRUE(HasNotice("KIT-8414 : missing instance name"));
    EXPECT_EQ(NoticeLines("KIT-1004 : a\r\nKIT-8414 : b"), "KIT-1004 : a / KIT-8414 : b");
}

TEST(TdHasNotice, SeverityLinesAreNotNotices) {
    // 同一行已经带 severity -> 只算 error/warning 行，不能算 notice（否则重复计数）
    EXPECT_FALSE(HasNotice("PRJ-8001 ERROR: Invalid device_db name"));
    EXPECT_FALSE(HasNotice("USR-8128 CRITICAL-WARNING: some warning"));
    EXPECT_FALSE(HasNotice("HDL-5007 WARNING: could not find top-level module"));
}

TEST(TdHasNotice, NoticeLinesSkipsSeverityLines) {
    const std::string text =
        "PRJ-8001 ERROR: Invalid device_db name\r\n"
        "KIT-2501 : No mark_debug exists in the netlist.\r\n"
        "USR-8128 CRITICAL-WARNING: some warning\r\n";
    EXPECT_TRUE(HasNotice(text));
    EXPECT_EQ(NoticeLines(text), "KIT-2501 : No mark_debug exists in the netlist.");
}

TEST(TdHasNotice, PlainInfoCodeWithColonIsNotice) {
    // 实测普通信息行（analyze verilog file …）也是 `<CODE> :` 形态：
    // 如实断言"算 notice"，不要猜成 warning/error。
    EXPECT_TRUE(HasNotice("HDL-1007 : analyze verilog file Y:/a/b.v"));
    EXPECT_TRUE(HasNotice("HDL-1007 : b"));
    EXPECT_EQ(NoticeLines("HDL-1007 : b"), "HDL-1007 : b");
}

TEST(TdHasNotice, LineWithBareColonlessCodeIsNotNotice) {
    // 没有冒号（`HDL-7144 CRITICAL-WARNING:` 已在上面覆盖；这里是没有冒号的闲聊）
    EXPECT_FALSE(HasNotice("just some text"));
    EXPECT_FALSE(HasNotice(""));
    EXPECT_FALSE(HasNotice("td > "));
}

TEST(TdHasNotice, LimitKeepsFirstN) {
    const std::string text =
        "KIT-2501 : one\r\n"
        "KIT-2502 : two\r\n"
        "KIT-2503 : three\r\n";
    EXPECT_EQ(NoticeLines(text), "KIT-2501 : one / KIT-2502 : two / KIT-2503 : three");
    EXPECT_EQ(NoticeLines(text, 2), "KIT-2501 : one / KIT-2502 : two");
    EXPECT_EQ(NoticeLines(text, 0), "");
}

TEST(TdHasNotice, TrimsEachLine) {
    const std::string text = "  KIT-2501 : No mark_debug exists in the netlist.  \r\n";
    EXPECT_TRUE(HasNotice(text));
    EXPECT_EQ(NoticeLines(text, 1), "KIT-2501 : No mark_debug exists in the netlist.");
}

// ---- ⚠ 已知缺陷：全局替换 "td > " ----
// 这是 Python 版 td.py 第 237 行 `text.replace("td > ", "")` 的既有行为。
// 本实现保持一致；若将来要修正，本测试会变红并提醒。
TEST(TdOutputKnownDefect, GlobalPromptReplaceEatsLiteralOccurrences) {
    // TD 执行 puts "a td > b" 时，输出里会真的含 "td > " 字面。
    // Python 版全局替换后变成 "a b" —— C++ 版行为相同。
    const std::string raw = "__Bprobe__\r\na td > b\r\n__Eprobe__";
    const std::string out = StripSentinelsAndPrompt(raw, "probe");

    // ⚠ 期望值 = "a b"（内容被吃掉了），这是**有意保持**的兼容行为
    EXPECT_EQ(CleanLines(out), std::vector<std::string>{"a b"});

    // 说明：正确行为本应是 "a td > b"。若某天修了这个缺陷，请把上面改成
    //   EXPECT_EQ(CleanLines(out), std::vector<std::string>{"a td > b"});
}

TEST(TdOutputKnownDefect, MultiplePromptResiduesRemoved) {
    // 真实 body 里提示符出现多次，全部被清掉（这是全局替换的正向作用）
    const std::string raw = "td > __Bx__\r\n\r\ntd > \r\ntd > RUN-1002 : start\r\n__Ex__";
    const std::string out = StripSentinelsAndPrompt(raw, "x");
    const auto lines = CleanLines(out);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0], "RUN-1002 : start");
}

}  // namespace