// =============================================================================
//  tests/unit/features_optional_test.cpp —— A4–A12 的纯函数契约
// =============================================================================
//  覆盖（design/backend-features.md A 部分）：
//    A4 signatureHelp 端口定位 / activeParameter（命名端口 + 位置端口）
//    A5 documentLink 只在 .al 生效 / 相对路径 / 缺失文件无 target + tooltip
//    A6 codeLens 数量（"N instantiations"）与 command 形状
//    A7 documentHighlight 同文档同名 token，注释里的同名文本不命中
//    A8 selectionRange 层级（单词 -> 行 -> begin..end 块 -> module/整篇）
//    A9 pull diagnostics 形状 {kind:"full",items,resultId} / workspace 形状 / resultId 稳定
//    A12 completionItem/resolve 的 detail/documentation
//    A12 semanticTokens/range 只含区间内 token，且 5 元组**重新编码**正确
//
//  这些函数只依赖 hdl 索引 + 文本，不需要启动 LSP 传输层。
// =============================================================================

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hdl/hdl_index.h"
#include "platform/path.h"
#include "semantics/semantics.h"
#include "server/features_optional.h"

namespace {

using fpgalsp::hdl::HdlIndex;
using fpgalsp::hdl::ParseVerilog;
using fpgalsp::server::features_optional::BuildDocumentDiagnosticReport;
using fpgalsp::server::features_optional::BuildWorkspaceDiagnosticReport;
using fpgalsp::server::features_optional::CodeLenses;
using fpgalsp::server::features_optional::DiagnosticsResultId;
using fpgalsp::server::features_optional::DocumentHighlights;
using fpgalsp::server::features_optional::DocumentLinks;
using fpgalsp::server::features_optional::FilterSemanticTokens;
using fpgalsp::server::features_optional::ResolveCompletionItem;
using fpgalsp::server::features_optional::SelectionRanges;
using fpgalsp::server::features_optional::SemanticTokensRange;
using fpgalsp::server::features_optional::SignatureHelp;
using fpgalsp::semantics::EncodeTokens;
using fpgalsp::semantics::SemToken;
using fpgalsp::semantics::SemanticTokens;

using Json = nlohmann::json;

HdlIndex IndexFor(std::string_view path, const std::string& text) {
    HdlIndex index;
    index.AddFile(ParseVerilog(path, text));
    return index;
}

HdlIndex MultiFileIndex(const std::vector<std::pair<std::string, std::string>>& files) {
    HdlIndex index;
    for (const auto& [path, text] : files) index.AddFile(ParseVerilog(path, text));
    return index;
}

void WriteFile(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
}

// -----------------------------------------------------------------------------
// A4：signatureHelp
// -----------------------------------------------------------------------------
TEST(SignatureHelp, NamedPortInstanceGivesModuleSignatureAndActiveParameter) {
    const std::string text =
        "module child (\n"
        "  input  wire       clk,\n"
        "  input  wire [7:0] d,\n"
        "  output reg        q\n"
        ");\n"
        "endmodule\n"
        "\n"
        "module top;\n"
        "  child u0 (\n"
        "    .clk(clk),\n"
        "    .d(d),\n"
        "    .q(q)\n"
        "  );\n"
        "endmodule\n";
    const std::string path = "sig_child.v";
    const HdlIndex index = IndexFor(path, text);

    // 光标在第 10 行（`.d(d),`）的 `.d(` 之内：前面已经出现 1 个顶层逗号 -> activeParameter=1。
    const Json help = SignatureHelp(index, path, text, 10, 7);
    ASSERT_TRUE(help.is_object());
    ASSERT_EQ(help["signatures"].size(), 1u);
    EXPECT_EQ(help["activeSignature"].get<int>(), 0);
    EXPECT_EQ(help["activeParameter"].get<int>(), 1);

    const Json& signature = help["signatures"][0];
    EXPECT_EQ(signature["label"].get<std::string>(), "module child(clk, d, q)");
    ASSERT_EQ(signature["parameters"].size(), 3u);
    EXPECT_EQ(signature["parameters"][0]["label"].get<std::string>(), "clk");
    EXPECT_EQ(signature["parameters"][1]["label"].get<std::string>(), "d");
    EXPECT_NE(signature["parameters"][1]["documentation"].get<std::string>().find("input"),
              std::string::npos);
    EXPECT_NE(signature["parameters"][1]["documentation"].get<std::string>().find("[7:0]"),
              std::string::npos);
}

TEST(SignatureHelp, PositionalInstanceCountsTopLevelCommasOnly) {
    const std::string text =
        "module child (input wire clk, input wire [7:0] d, output reg q);\n"
        "endmodule\n"
        "module top;\n"
        "  wire [7:0] d;\n"
        "  child u0 (clk, d);\n"
        "endmodule\n";
    const std::string path = "sig_positional.v";
    const HdlIndex index = IndexFor(path, text);

    // `child u0 (clk, |d)` —— 光标在第二个实参前 -> activeParameter=1。
    const Json help = SignatureHelp(index, path, text, 4, 18);
    ASSERT_TRUE(help.is_object());
    ASSERT_EQ(help["signatures"].size(), 1u);
    EXPECT_EQ(help["activeParameter"].get<int>(), 1);
    EXPECT_EQ(help["signatures"][0]["label"].get<std::string>(),
              "module child(clk, d, q)");
}

TEST(SignatureHelp, OutsideParensOrConstraintDocReturnsLegalEmptyShape) {
    const std::string text = "module m;\n  assign x = 1;\nendmodule\n";
    const std::string path = "sig_empty.v";
    const HdlIndex index = IndexFor(path, text);

    const Json outside = SignatureHelp(index, path, text, 1, 2);
    ASSERT_TRUE(outside.is_object());
    EXPECT_TRUE(outside["signatures"].is_array());
    EXPECT_TRUE(outside["signatures"].empty());
    EXPECT_EQ(outside["activeSignature"].get<int>(), 0);
    EXPECT_EQ(outside["activeParameter"].get<int>(), 0);

    // `.adc` 不是 HDL：即使文本里有括号也不误触发。
    const HdlIndex empty_index;
    const Json adc = SignatureHelp(empty_index, "pin.adc", "(a, b)", 0, 1);
    ASSERT_TRUE(adc.is_object());
    EXPECT_TRUE(adc["signatures"].empty());
}

// -----------------------------------------------------------------------------
// A5：documentLink
// -----------------------------------------------------------------------------
TEST(DocumentLinks, OnlyAlAndExistingFilesGetTarget) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "fpgalsp_optional_test_links";
    fs::remove_all(root);
    fs::create_directories(root / "src");
    WriteFile(root / "src" / "a.v", "module a;\nendmodule\n");
    const fs::path al_path = root / "proj.al";
    const std::string al_text =
        "<?xml version=\"1.0\"?>\n"
        "<Project Path=\"Y:/project-root\">\n"
        "  <Verilog>\n"
        "    <File Path=\"src/a.v\"/>\n"
        "    <File Path=\"missing.v\"/>\n"
        "  </Verilog>\n"
        "</Project>\n";
    WriteFile(al_path, al_text);

    const Json links = DocumentLinks(al_path.generic_string(), al_text);
    ASSERT_EQ(links.size(), 2u);
    // 存在的文件 -> 有 target（file:// URI）
    EXPECT_TRUE(links[0].contains("target"));
    EXPECT_EQ(links[0]["target"].get<std::string>().rfind("file://", 0), 0u);
    EXPECT_EQ(links[0]["range"]["start"]["line"].get<int>(), 3);
    EXPECT_EQ(links[0]["range"]["start"]["character"].get<int>(), 16);  // `Path="` 之后
    // 缺失的文件 -> 没有 target，但有 tooltip 说明
    EXPECT_FALSE(links[1].contains("target"));
    EXPECT_NE(links[1]["tooltip"].get<std::string>().find("not found"), std::string::npos);
    EXPECT_EQ(links[1]["range"]["start"]["line"].get<int>(), 4);

    // 非 .al 文件：一行都不解析（HDL 的 `<File Path>` 不产生链接）。
    EXPECT_TRUE(DocumentLinks("top.v", al_text).empty());
    EXPECT_TRUE(DocumentLinks("pin.adc", al_text).empty());
    // `<Project Path=...>` 是工程根（目录），不是文件链接 —— 真实 .al 里它就在第 3 行，
    // 不排除会得到一条 "file not found: C:/work/CorpusAlpha"（2026-11-22 实测）。
    EXPECT_EQ(links.size(), 2u);
    fs::remove_all(root);
}

// -----------------------------------------------------------------------------
// A6：codeLens
// -----------------------------------------------------------------------------
TEST(CodeLens, CountsInstantiationsPerModuleDefinition) {
    const std::string child_text = "module child(input wire clk);\nendmodule\n";
    const std::string top_text =
        "module top;\n"
        "  wire clk;\n"
        "  child u0 (.clk(clk));\n"
        "  child u1 (.clk(clk));\n"
        "endmodule\n";
    const std::string child_path = "child.v";
    const std::string top_path = "top.v";
    const HdlIndex index = MultiFileIndex({{child_path, child_text}, {top_path, top_text}});

    const Json child_lenses = CodeLenses(index, child_path);
    ASSERT_EQ(child_lenses.size(), 1u);
    EXPECT_EQ(child_lenses[0]["command"]["title"].get<std::string>(), "2 instantiations");
    EXPECT_EQ(child_lenses[0]["command"]["command"].get<std::string>(),
              "fpgalsp.searchHierarchy");
    ASSERT_EQ(child_lenses[0]["command"]["arguments"].size(), 1u);
    EXPECT_EQ(child_lenses[0]["command"]["arguments"][0]["query"].get<std::string>(), "child");
    EXPECT_EQ(child_lenses[0]["range"]["start"]["line"].get<int>(), 0);

    // top 没有被例化 -> 0 条（仍然是真实计数，不是固定空值）。
    const Json top_lenses = CodeLenses(index, top_path);
    ASSERT_EQ(top_lenses.size(), 1u);
    EXPECT_EQ(top_lenses[0]["command"]["title"].get<std::string>(), "0 instantiations");

    // 非 HDL 文件 -> []
    EXPECT_TRUE(CodeLenses(index, "proj.al").empty());
}

// -----------------------------------------------------------------------------
// A7：documentHighlight
// -----------------------------------------------------------------------------
TEST(DocumentHighlight, SameDocumentTokensOnlyAndCommentsIgnored) {
    const std::string text =
        "module demo(input wire clk);\n"
        "  wire sig;\n"
        "  assign sig = clk;\n"
        "  // sig in a comment is not a token\n"
        "endmodule\n";
    const std::string path = "highlight.v";
    const HdlIndex index = IndexFor(path, text);

    const Json highlights = DocumentHighlights(index, path, 2, 9);
    ASSERT_EQ(highlights.size(), 2u) << "声明 + 赋值处 = 2 条";
    for (const Json& highlight : highlights) {
        EXPECT_EQ(highlight["kind"].get<int>(), 1) << "token 流判不出读写 -> Text";
        EXPECT_NE(highlight["range"]["start"]["line"].get<int>(), 3)
            << "注释里的同名文本不能命中";
    }
    // 两处的行：声明（line 1）与赋值（line 2）
    const int first_line = highlights[0]["range"]["start"]["line"].get<int>();
    const int second_line = highlights[1]["range"]["start"]["line"].get<int>();
    EXPECT_EQ(std::min(first_line, second_line), 1);
    EXPECT_EQ(std::max(first_line, second_line), 2);

    // 约束文档没有 HDL token -> []
    EXPECT_TRUE(DocumentHighlights(index, "pin.adc", 0, 0).empty());
}

// -----------------------------------------------------------------------------
// A8：selectionRange
// -----------------------------------------------------------------------------
TEST(SelectionRange, ChainIsWordLineBlockModule) {
    const std::string text =
        "module demo;\n"
        "  initial begin\n"
        "    x = 1;\n"
        "  end\n"
        "endmodule\n";
    const std::string path = "select.v";
    const HdlIndex index = IndexFor(path, text);

    const Json ranges =
        SelectionRanges(index, path, text, std::vector<std::pair<int, int>>{{2, 4}});
    ASSERT_EQ(ranges.size(), 1u);

    // 由内向外走 parent 链，逐层断言。
    std::vector<Json> chain;
    for (Json node = ranges[0]; node.is_object() && !node.is_null(); node = node["parent"]) {
        chain.push_back(node["range"]);
        if (!node.contains("parent") || node["parent"].is_null()) break;
    }
    ASSERT_GE(chain.size(), 4u);
    // 1) 单词 `x`
    EXPECT_EQ(chain[0]["start"]["line"].get<int>(), 2);
    EXPECT_EQ(chain[0]["start"]["character"].get<int>(), 4);
    EXPECT_EQ(chain[0]["end"]["character"].get<int>(), 5);
    // 2) 整行
    EXPECT_EQ(chain[1]["start"]["character"].get<int>(), 0);
    EXPECT_EQ(chain[1]["start"]["line"].get<int>(), 2);
    // 3) begin..end 块
    EXPECT_EQ(chain[2]["start"]["line"].get<int>(), 1);
    EXPECT_EQ(chain[2]["end"]["line"].get<int>(), 3);
    // 4) module 体
    EXPECT_EQ(chain[3]["start"]["line"].get<int>(), 0);
    EXPECT_EQ(chain[3]["end"]["line"].get<int>(), 4);
    // 5) 整篇（末尾换行算一个空行：endLine = 5）
    EXPECT_EQ(chain.back()["start"]["line"].get<int>(), 0);
    EXPECT_EQ(chain.back()["end"]["line"].get<int>(), 5);

    // 每一层 parent 都必须包含 child（LSP 硬要求）。
    for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
        const Json& inner = chain[i];
        const Json& outer = chain[i + 1];
        EXPECT_LE(outer["start"]["line"].get<int>(), inner["start"]["line"].get<int>());
        EXPECT_GE(outer["end"]["line"].get<int>(), inner["end"]["line"].get<int>());
    }
}

// -----------------------------------------------------------------------------
// A9：pull diagnostics 形状
// -----------------------------------------------------------------------------
TEST(PullDiagnostics, DocumentAndWorkspaceReportsHaveExpectedShape) {
    Json items = Json::array();
    Json diagnostic = Json::object();
    diagnostic["range"]["start"]["line"] = 3;
    diagnostic["range"]["start"]["character"] = 1;
    diagnostic["range"]["end"]["line"] = 3;
    diagnostic["range"]["end"]["character"] = 5;
    diagnostic["severity"] = 1;
    diagnostic["source"] = "fpga-lsp";
    diagnostic["message"] = "boom";
    items.push_back(diagnostic);

    const std::string uri = "file:///work/top.v";
    const std::string id = DiagnosticsResultId(uri, items);
    EXPECT_EQ(id.size(), 16u);
    // 同样的内容 -> 同样的 resultId（客户端据此回 "unchanged"）；内容变 -> id 变。
    EXPECT_EQ(id, DiagnosticsResultId(uri, items));
    Json changed = items;
    changed[0]["message"] = "different";
    EXPECT_NE(id, DiagnosticsResultId(uri, changed));

    const Json report = BuildDocumentDiagnosticReport(items, id);
    EXPECT_EQ(report["kind"].get<std::string>(), "full");
    ASSERT_EQ(report["items"].size(), 1u);
    EXPECT_EQ(report["resultId"].get<std::string>(), id);
    EXPECT_EQ(report["items"][0]["message"].get<std::string>(), "boom");

    std::map<std::string, Json> latest;
    latest[uri] = items;
    latest["file:///work/other.v"] = Json::array();  // 曾发布过、当前已清空
    const Json workspace = BuildWorkspaceDiagnosticReport(latest);
    ASSERT_EQ(workspace["items"].size(), 2u);
    for (const Json& entry : workspace["items"]) {
        EXPECT_EQ(entry["kind"].get<std::string>(), "full");
        EXPECT_TRUE(entry.contains("uri"));
        EXPECT_TRUE(entry.contains("resultId"));
        EXPECT_TRUE(entry["items"].is_array());
    }
}

// -----------------------------------------------------------------------------
// A12：semanticTokens/range（核心：重新编码，不能截数组）
// -----------------------------------------------------------------------------
TEST(SemanticTokensRange, ContainsOnlyTokensInsideRangeAndReencodes) {
    const std::string text =
        "module demo (\n"
        "  input  wire clk,\n"
        "  output reg  q\n"
        ");\n"
        "  assign q = clk;\n"
        "endmodule\n";
    const std::string path = "range_tokens.v";
    const HdlIndex index = IndexFor(path, text);

    const std::vector<SemToken> all = SemanticTokens(&index, path, text);
    ASSERT_FALSE(all.empty());

    // 只请求第 4 行（assign 行）。
    const Json result = SemanticTokensRange(&index, path, text, 4, 0, 5, 0);
    const Json data = result["data"];
    ASSERT_TRUE(data.is_array());
    ASSERT_EQ(data.size() % 5, 0);
    ASSERT_FALSE(data.empty());

    // 期望值 = 过滤后的 decoded token 重新 EncodeTokens（逐元素相同）。
    std::vector<SemToken> expected;
    for (const SemToken& token : all) {
        if (token.line == 4) expected.push_back(token);
    }
    ASSERT_FALSE(expected.empty());
    const std::vector<int> encoded = EncodeTokens(expected);
    ASSERT_EQ(data.size(), encoded.size());
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        EXPECT_EQ(data[i].get<int>(), encoded[i]) << "index " << i;
    }

    // 解码 5 元组：所有 token 必须落在第 4 行；第一条的 deltaLine 是绝对值 4
    // （相对 (0,0)），这正是"不能直接截全量数组"的证明。
    EXPECT_EQ(data[0].get<int>(), 4) << "第一条 deltaLine 必须是绝对行号";
    int line = 0;
    int character = 0;
    // 触发一次 FilterSemanticTokens 的直接契约检查（区间外的 token 一条不剩）。
    const std::vector<SemToken> filtered = FilterSemanticTokens(all, 4, 0, 5, 0);
    std::size_t offset = 0;
    for (std::size_t i = 0; i < filtered.size(); ++i) {
        line += data[offset].get<int>();
        character = data[offset].get<int>();
        EXPECT_EQ(line, 4);
        EXPECT_GE(character, 0);
        offset += 5;
    }
    EXPECT_EQ(offset, data.size());
}

// -----------------------------------------------------------------------------
// A12：completionItem/resolve
// -----------------------------------------------------------------------------
TEST(CompletionResolve, ModuleItemGetsDetailDocumentationAndDefinition) {
    const std::string text =
        "module child (\n"
        "  input  wire       clk,\n"
        "  input  wire [7:0] d,\n"
        "  output reg        q\n"
        ");\n"
        "endmodule\n";
    const std::string path = "resolve_child.v";
    const HdlIndex index = IndexFor(path, text);

    Json item = Json::object();
    item["label"] = "child";
    item["kind"] = 9;
    const Json resolved = ResolveCompletionItem(&index, item);
    EXPECT_EQ(resolved["label"].get<std::string>(), "child");
    EXPECT_EQ(resolved["detail"].get<std::string>(), "module (3 ports)");
    ASSERT_TRUE(resolved.contains("documentation"));
    EXPECT_EQ(resolved["documentation"]["kind"].get<std::string>(), "markdown");
    const std::string documentation = resolved["documentation"]["value"].get<std::string>();
    EXPECT_NE(documentation.find("clk"), std::string::npos);
    EXPECT_NE(documentation.find("Defined at"), std::string::npos);
    EXPECT_EQ(resolved["data"]["module"].get<std::string>(), "child");
    EXPECT_NE(resolved["data"]["uri"].get<std::string>().rfind("file://", 0), std::string::npos);

    // 非模块候选：原样返回（不伪造 documentation）。
    Json keyword = Json::object();
    keyword["label"] = "always";
    keyword["kind"] = 14;
    const Json untouched = ResolveCompletionItem(&index, keyword);
    EXPECT_FALSE(untouched.contains("documentation"));
    EXPECT_EQ(untouched["label"].get<std::string>(), "always");

    // 无索引：原样返回。
    const Json bare = ResolveCompletionItem(nullptr, item);
    EXPECT_FALSE(bare.contains("documentation"));
}

}  // namespace
