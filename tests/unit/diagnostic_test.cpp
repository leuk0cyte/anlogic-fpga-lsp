// =============================================================================
//  tests/unit/diagnostic_test.cpp —— LSP Diagnostic 基础类型
// =============================================================================

#include <gtest/gtest.h>

#include <string>

#include "lsp/diagnostic.h"

namespace {

using namespace fpgalsp::lsp;

// -----------------------------------------------------------------------------
// 严重度常量
// -----------------------------------------------------------------------------

TEST(DiagnosticSeverity, MatchesLspSpec) {
    // LSP 规范：1=Error 2=Warning 3=Information 4=Hint
    EXPECT_EQ(kError, 1);
    EXPECT_EQ(kWarning, 2);
    EXPECT_EQ(kInfo, 3);
    EXPECT_EQ(kHint, 4);
}

TEST(DiagnosticSeverity, ValidRange) {
    EXPECT_TRUE(IsValidSeverity(1));
    EXPECT_TRUE(IsValidSeverity(4));
    EXPECT_FALSE(IsValidSeverity(0));
    EXPECT_FALSE(IsValidSeverity(5));
    EXPECT_FALSE(IsValidSeverity(-1));
}

// -----------------------------------------------------------------------------
// MakeRange（对应 Python make_range）
// -----------------------------------------------------------------------------

TEST(DiagnosticRange, OmittedEndsDefaultToStart) {
    const Range r = MakeRange(3, 5);
    EXPECT_EQ(r.start.line, 3);
    EXPECT_EQ(r.start.character, 5);
    EXPECT_EQ(r.end.line, 3);       // end_line 缺省 = line
    EXPECT_EQ(r.end.character, 5);  // end_char 缺省 = start_char
}

TEST(DiagnosticRange, ExplicitEnds) {
    const Range r = MakeRange(1, 2, 3, 4);
    EXPECT_EQ(r.start.line, 1);
    EXPECT_EQ(r.end.line, 3);
    EXPECT_EQ(r.end.character, 4);
}

// -----------------------------------------------------------------------------
// 序列化往返
// -----------------------------------------------------------------------------

TEST(DiagnosticJson, RoundTripWithData) {
    Diagnostic d = Diagnostic::AtLine(5, 0, 1, kWarning, "HDL-7144", "fpga-lsp:td", "boom");
    d.WithBackend("read_hdl");

    const Json j = ToJson(d);
    EXPECT_EQ(j["range"]["start"]["line"], 5);
    EXPECT_EQ(j["range"]["start"]["character"], 0);
    EXPECT_EQ(j["range"]["end"]["line"], 5);
    EXPECT_EQ(j["range"]["end"]["character"], 1);
    EXPECT_EQ(j["severity"], 2);
    EXPECT_EQ(j["code"], "HDL-7144");
    EXPECT_EQ(j["source"], "fpga-lsp:td");
    EXPECT_EQ(j["message"], "boom");
    EXPECT_EQ(j["data"]["backend"], "read_hdl");

    const Diagnostic back = FromJson(j);
    EXPECT_EQ(back.range.start.line, 5);
    EXPECT_EQ(back.range.end.character, 1);
    EXPECT_EQ(back.severity, kWarning);
    EXPECT_EQ(back.code, "HDL-7144");
    EXPECT_EQ(back.source, "fpga-lsp:td");
    EXPECT_EQ(back.message, "boom");
    EXPECT_EQ(back.data["backend"], "read_hdl");
}

TEST(DiagnosticJson, EmptyDataIsOmitted) {
    // 对应 Python _sanitize: `if diag.get("data")` —— 空则不输出该字段
    Diagnostic d = Diagnostic::AtLine(0, 0, 1, kInfo, "X-1", "fpga-lsp", "m");
    const Json j = ToJson(d);
    EXPECT_FALSE(j.contains("data")) << "null data 不应输出 data 字段";
}

TEST(DiagnosticJson, EmptyObjectDataOmitted) {
    Diagnostic d = Diagnostic::AtLine(0, 0, 1, kInfo, "X-1", "fpga-lsp", "m");
    d.data = Json::object();
    EXPECT_FALSE(ToJson(d).contains("data"));
}

TEST(DiagnosticJson, EmptyCodeRoundTrips) {
    Diagnostic d;
    const Json j = ToJson(d);
    // 未 Sanitize 时空 code 原样输出
    EXPECT_EQ(j["code"], "");
    EXPECT_EQ(FromJson(j).code, "");
}

TEST(DiagnosticJson, FromJsonToleratesMissingFields) {
    Diagnostic d;
    EXPECT_NO_THROW(d = FromJson(Json::object()));
    EXPECT_EQ(d.range.start.line, 0);
    EXPECT_EQ(d.message, "");

    // 非 object 输入也不抛
    EXPECT_NO_THROW(d = FromJson(Json("not an object")));
    EXPECT_NO_THROW(d = FromJson(Json()));
}

TEST(DiagnosticJson, FromJsonToleratesWrongTypes) {
    Json j = Json::object();
    j["severity"] = "not a number";
    j["code"] = 123;
    j["range"] = "not an object";
    Diagnostic d;
    EXPECT_NO_THROW(d = FromJson(j));
    EXPECT_EQ(d.code, "");  // 类型不符 -> 默认空
}

// -----------------------------------------------------------------------------
// 多字节字符位置
// -----------------------------------------------------------------------------

TEST(DiagnosticJson, MultiByteCharacterPositionPreserved) {
    // character 是 UTF-16 码元偏移，序列化不应做任何转码
    Diagnostic d = Diagnostic::AtLine(2, 7, 12, kError, "HDL-8007", "fpga-lsp:td",
                                      "中文😀 error");
    const Json j = ToJson(d);
    EXPECT_EQ(j["range"]["start"]["character"], 7);
    EXPECT_EQ(j["range"]["end"]["character"], 12);
    EXPECT_EQ(j["message"], "中文😀 error");
    EXPECT_EQ(FromJson(j).message, "中文😀 error");
}

TEST(DiagnosticJson, WideCharacterRangePreserved) {
    // 全角/emoji 场景下 character 可能较大
    Diagnostic d = Diagnostic::AtLine(0, 0, 40, kError, "X-1", "fpga-lsp:td", "m");
    EXPECT_EQ(ToJson(d)["range"]["end"]["character"], 40);
}

// -----------------------------------------------------------------------------
// Sanitize（对应 server.py _sanitize）
// -----------------------------------------------------------------------------

TEST(DiagnosticSanitize, FillsDefaultRange) {
    Diagnostic d;
    d.code = "X-1";
    const Diagnostic s = Sanitize(d);
    // 对应 Python: {"start":{0,0}, "end":{0,1}}
    EXPECT_EQ(s.range.start.line, 0);
    EXPECT_EQ(s.range.start.character, 0);
    EXPECT_EQ(s.range.end.line, 0);
    EXPECT_EQ(s.range.end.character, 1);
}

TEST(DiagnosticSanitize, ZeroSeverityBecomesWarning) {
    // 对应 Python: int(diag.get("severity") or 2)
    Diagnostic d;
    d.severity = 0;
    EXPECT_EQ(Sanitize(d).severity, kWarning);
}

TEST(DiagnosticSanitize, InvalidSeverityBecomesWarning) {
    Diagnostic d;
    d.severity = 99;
    EXPECT_EQ(Sanitize(d).severity, kWarning);
}

TEST(DiagnosticSanitize, EmptyCodeBecomesLsp) {
    // 对应 Python: str(diag.get("code") or "LSP")
    Diagnostic d;
    EXPECT_EQ(Sanitize(d).code, "LSP");
}

TEST(DiagnosticSanitize, DefaultSourceApplied) {
    Diagnostic d;
    d.code = "X-1";
    EXPECT_EQ(Sanitize(d).source, "fpga-lsp");
    EXPECT_EQ(Sanitize(d, "fpga-lsp:td").source, "fpga-lsp:td");
}

TEST(DiagnosticSanitize, PreservesValidFields) {
    Diagnostic d = Diagnostic::AtLine(9, 4, 8, kError, "HDL-8007", "fpga-lsp:td", "msg");
    const Diagnostic s = Sanitize(d);
    EXPECT_EQ(s.range.start.line, 9);
    EXPECT_EQ(s.range.start.character, 4);
    EXPECT_EQ(s.range.end.character, 8);
    EXPECT_EQ(s.severity, kError);
    EXPECT_EQ(s.code, "HDL-8007");
    EXPECT_EQ(s.source, "fpga-lsp:td");
    EXPECT_EQ(s.message, "msg");
}

TEST(DiagnosticSanitize, NeverThrows) {
    for (int sev : {-5, 0, 1, 4, 100}) {
        Diagnostic d;
        d.severity = sev;
        EXPECT_NO_THROW({
            const Diagnostic s = Sanitize(d);
            EXPECT_TRUE(IsValidSeverity(s.severity));
        });
    }
}

// -----------------------------------------------------------------------------
// 数组序列化
// -----------------------------------------------------------------------------

TEST(DiagnosticJson, ListSerialization) {
    DiagnosticList list;
    list.push_back(Diagnostic::AtLine(0, 0, 1, kError, "A-1", "fpga-lsp", "one"));
    list.push_back(Diagnostic::AtLine(1, 0, 1, kInfo, "B-2", "fpga-lsp", "two"));

    const Json arr = ToJson(list);
    ASSERT_TRUE(arr.is_array());
    ASSERT_EQ(arr.size(), 2u);
    EXPECT_EQ(arr[0]["code"], "A-1");
    EXPECT_EQ(arr[1]["code"], "B-2");
}

TEST(DiagnosticJson, EmptyListIsEmptyArray) {
    const Json arr = ToJson(DiagnosticList{});
    EXPECT_TRUE(arr.is_array());
    EXPECT_TRUE(arr.empty());
}

// -----------------------------------------------------------------------------
// WithData 链式调用
// -----------------------------------------------------------------------------

TEST(DiagnosticData, ChainedKeys) {
    Diagnostic d = Diagnostic::AtLine(0, 0, 1, kInfo, "X-1", "fpga-lsp", "m");
    d.WithBackend("read_hdl").WithData("origin", "td");

    const Json j = ToJson(d);
    EXPECT_EQ(j["data"]["backend"], "read_hdl");
    EXPECT_EQ(j["data"]["origin"], "td");
}

}  // namespace