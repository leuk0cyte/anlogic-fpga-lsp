// =============================================================================
//  tests/unit/rename_test.cpp
// =============================================================================
//  B3（parity-lsp）：textDocument/prepareRename + textDocument/rename
//  对应矩阵 LSP-04 / LSP-05。
//
//  契约来自 A2 的 wire 实测（audit/2026-11-18/parity-lsp/raw.json matrix[10..11]）：
//    prepareRename(clk_50M @58:6) -> {range: 58:5-58:12, placeholder:"clk_50M"}
//    rename(clk_50M -> clk_50M_probe)
//        -> {changes: {file:///…/src/top.v: [4 条编辑]}}
//           行 58:5 / 62:13 / 68:13 / 73:6
//
//  这些测试直接驱动 LspServer::Feature*（不经子进程），语料
//  tests/gate-corpus/corpus_alpha 与真实工程 C:/work/CorpusAlpha 逐字节相同。
// =============================================================================

#include <cstdio>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "jsonrpc/transport.h"
#include "platform/path.h"
#include "server/features_rename_util.h"
#include "server/lsp_server.h"

namespace {

using fpgalsp::server::Json;
using fpgalsp::server::LspServer;

const std::string kProject = std::string(FPGA_LSP_SOURCE_DIR) + "/tests/gate-corpus/corpus_alpha";
const std::string kTop = kProject + "/src/top.v";
const std::string kAdc = kProject + "/constrains/pin.adc";
const std::string kSdc = kProject + "/constrains/top.sdc";

std::string ReadTextFile(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.good()) return {};
    return std::string((std::istreambuf_iterator<char>(stream)),
                       std::istreambuf_iterator<char>());
}

int FindLine(const std::string& text, const std::string& needle) {
    std::istringstream stream(text);
    std::string row;
    int line = 0;
    while (std::getline(stream, row)) {
        if (row.find(needle) != std::string::npos) return line;
        ++line;
    }
    return -1;
}

// 该行里 token 的 0-based 列；找不到返回 -1
int Column0(const std::string& text, int line, const std::string& token) {
    std::istringstream stream(text);
    std::string row;
    for (int index = 0;; ++index) {
        if (!std::getline(stream, row)) return -1;
        if (index != line) continue;
        const std::size_t at = row.find(token);
        return at == std::string::npos ? -1 : static_cast<int>(at);
    }
}

// 该行长度（0-based 行；找不到返回 -1）
int LineLength(const std::string& text, int line) {
    std::istringstream stream(text);
    std::string row;
    for (int index = 0;; ++index) {
        if (!std::getline(stream, row)) return -1;
        if (index == line) return static_cast<int>(row.size());
    }
}

// 最小 LspServer 夹具（与 call_hierarchy_test 同一形状）
class RenameFixture {
public:
    explicit RenameFixture(const std::string& root = kProject)
        : transport_(input_, output_), server_(transport_) {
        Json params = Json::object();
        params["rootUri"] = fpgalsp::platform::path_to_uri(root);
        server_.FeatureInitialize(params);
    }

    LspServer& server() { return server_; }

private:
    std::istringstream input_;
    std::ostringstream output_;
    fpgalsp::jsonrpc::LspTransport transport_;
    LspServer server_;
};

// 临时工程：root/<name>.al + 任意 HDL 文件（用于边界用例，不碰真实工程）
class TempProject {
public:
    explicit TempProject(std::string_view top_module) : top_(top_module) {
        static int counter = 0;
        std::error_code error;
        root_ = std::filesystem::temp_directory_path(error) /
                ("fpgalsp_b3_rename_" + std::to_string(++counter));
        std::filesystem::remove_all(root_, error);
        std::filesystem::create_directories(root_, error);
    }
    ~TempProject() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    std::string Write(std::string_view relative, const std::string& content) {
        const std::filesystem::path path = root_ / std::filesystem::path(std::string(relative));
        std::filesystem::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary);
        stream << content;
        return fpgalsp::platform::normalize_path(path.generic_string());
    }

    // 建 .al：files 是相对 root 的 Verilog 路径
    void WriteAl(const std::vector<std::string>& files) {
        std::string text =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Project Version=\"3\" Minor=\"2\">\n"
            "  <Name>b3rename</Name>\n"
            "  <TOP_MODULE><MODULE>" + top_ + "</MODULE></TOP_MODULE>\n"
            "  <Source_Files>\n"
            "    <Verilog>\n";
        for (const std::string& file : files) {
            text += "      <File Path=\"" + file + "\"><FileInfo>\n"
                    "        <Attr Name=\"CompileOrder\" Val=\"1\"/>\n"
                    "      </FileInfo></File>\n";
        }
        text +=
            "    </Verilog>\n"
            "  </Source_Files>\n"
            "</Project>\n";
        std::ofstream stream(root_ / "b3rename.al", std::ios::binary);
        stream << text;
    }

    std::string root() const { return fpgalsp::platform::normalize_path(root_.generic_string()); }

private:
    std::filesystem::path root_;
    std::string top_;
};

int StartLine(const Json& edit) { return edit["range"]["start"]["line"].get<int>(); }
int StartCharacter(const Json& edit) { return edit["range"]["start"]["character"].get<int>(); }
int EndCharacter(const Json& edit) { return edit["range"]["end"]["character"].get<int>(); }

}  // namespace

// =============================================================================
//  纯函数（features_rename_util.h）
// =============================================================================



TEST(RenameUtil, TextTokenAtFindsIdentifierUnderCursor) {
    if (!std::filesystem::exists(FPGA_LSP_SOURCE_DIR "/tests/gate-corpus/corpus_alpha")) {
        GTEST_SKIP() << "corpus missing (release tree)";
    }
    const std::string text = "  wire clk_50M;\n";
    const auto token = fpgalsp::server::edit::TextTokenAt(text, 0, 7);
    ASSERT_TRUE(token.found);
    EXPECT_EQ(token.text, "clk_50M");
    EXPECT_EQ(token.start, 7);
    EXPECT_EQ(token.end, 14);

    // 光标停在标识符中间 / 结尾也算命中
    EXPECT_EQ(fpgalsp::server::edit::TextTokenAt(text, 0, 10).text, "clk_50M");
    EXPECT_EQ(fpgalsp::server::edit::TextTokenAt(text, 0, 14).text, "clk_50M");
    // 不在标识符上（分号 / 空格）与越界行 -> 没有 token
    EXPECT_FALSE(fpgalsp::server::edit::TextTokenAt(text, 0, 0).found);
    EXPECT_FALSE(fpgalsp::server::edit::TextTokenAt(text, 0, 15).found);
    EXPECT_FALSE(fpgalsp::server::edit::TextTokenAt(text, 99, 0).found);
    // 列超出行长度时 clamp 到行尾（Python: character > len(row) -> len(row)），
    // 行尾是 ';' -> 不是标识符 -> 没有 token
    EXPECT_FALSE(fpgalsp::server::edit::TextTokenAt(text, 0, 999).found);
}

TEST(RenameUtil, ValidVerilogIdentifierMatchesPythonBlacklist) {
    if (!std::filesystem::exists(FPGA_LSP_SOURCE_DIR "/tests/gate-corpus/corpus_alpha")) {
        GTEST_SKIP() << "corpus missing (release tree)";
    }
    using fpgalsp::server::edit::IsValidVerilogIdentifier;
    EXPECT_TRUE(IsValidVerilogIdentifier("clk_50M"));
    EXPECT_TRUE(IsValidVerilogIdentifier("_tmp"));
    EXPECT_TRUE(IsValidVerilogIdentifier("data$1"));
    EXPECT_FALSE(IsValidVerilogIdentifier(""));
    EXPECT_FALSE(IsValidVerilogIdentifier("1abc"));
    EXPECT_FALSE(IsValidVerilogIdentifier("a-b"));
    EXPECT_FALSE(IsValidVerilogIdentifier("a b"));
    // Python 的关键字黑名单只有这 4 个（不是完整关键字表）
    EXPECT_FALSE(IsValidVerilogIdentifier("module"));
    EXPECT_FALSE(IsValidVerilogIdentifier("endmodule"));
    EXPECT_FALSE(IsValidVerilogIdentifier("input"));
    EXPECT_FALSE(IsValidVerilogIdentifier("output"));
    EXPECT_TRUE(IsValidVerilogIdentifier("always"));  // Python 允许
}

TEST(RenameUtil, BlockScopesCoverGenerateForOnly) {
    if (!std::filesystem::exists(FPGA_LSP_SOURCE_DIR "/tests/gate-corpus/corpus_alpha")) {
        GTEST_SKIP() << "corpus missing (release tree)";
    }
    const std::string text =
        "module m;\n"                        // 0
        "    generate\n"                     // 1
        "        for (genvar i = 0; i < 2; i = i + 1) begin : g_a\n"  // 2
        "            wire a0;\n"             // 3
        "        end\n"                      // 4
        "    endgenerate\n"                  // 5
        "    always @(posedge clk) begin\n"  // 6
        "        q <= 0;\n"                  // 7
        "    end\n"                          // 8
        "endmodule\n";                       // 9
    const std::vector<fpgalsp::server::edit::BlockScope> scopes =
        fpgalsp::server::edit::BlockScopes(text);
    // Python 只把 generate / for-begin 当作 rename 边界（裸 begin/end 不算）
    ASSERT_GE(scopes.size(), 1u);
    bool found_for_begin = false;
    for (const auto& scope : scopes) {
        if (scope.for_begin) {
            found_for_begin = true;
            EXPECT_EQ(scope.start, 2);
            EXPECT_EQ(scope.end, 4);
        }
    }
    EXPECT_TRUE(found_for_begin);
    const auto* enclosing = fpgalsp::server::edit::EnclosingScope(scopes, 3);
    ASSERT_NE(enclosing, nullptr);
    EXPECT_TRUE(enclosing->for_begin);
    // always 块里的行不在任何 generate 作用域内
    EXPECT_EQ(fpgalsp::server::edit::EnclosingScope(scopes, 7), nullptr);
}

TEST(RenameUtil, ExpandCommentTabsOnlyTouchesComments) {
    if (!std::filesystem::exists(FPGA_LSP_SOURCE_DIR "/tests/gate-corpus/corpus_alpha")) {
        GTEST_SKIP() << "corpus missing (release tree)";
    }
    const std::string text = "wire\tx; // a\tb\n/* c\td */\n";
    const std::string fixed = fpgalsp::server::edit::ExpandCommentTabs(text);
    // 代码里的 tab 原样保留（Verible 的 no-tabs 规则对代码有效）
    EXPECT_NE(fixed.find("wire\tx;"), std::string::npos) << fixed;
    // 注释里的 tab 展开成空格
    EXPECT_EQ(fixed.find("a\tb"), std::string::npos) << fixed;
    EXPECT_EQ(fixed.find("// a b"), std::string::npos) << fixed;  // tab 末尾对齐，不一定是单个空格
}

// =============================================================================
//  prepareRename：语料 top.v
// =============================================================================

TEST(RenameB3, PrepareRenameOnSignalYieldsRangeAndPlaceholder) {
    if (!std::filesystem::exists(FPGA_LSP_SOURCE_DIR "/tests/gate-corpus/corpus_alpha")) {
        GTEST_SKIP() << "corpus missing (release tree)";
    }
    const std::string text = ReadTextFile(kTop);
    if (text.empty()) GTEST_SKIP() << "语料需自备：语料缺失（设置 FPGA_LSP_TEST_PROJECT 指向自备工程）";
    const int line = FindLine(text, "wire clk_50M;");
    ASSERT_GE(line, 0);
    const int column = Column0(text, line, "clk_50M");
    ASSERT_GE(column, 0);

    RenameFixture fixture;
    const Json info = fixture.server().FeaturePrepareRename(kTop, line, column + 1);
    ASSERT_TRUE(info.is_object()) << "Python: {range, placeholder}";
    EXPECT_EQ(info["placeholder"].get<std::string>(), "clk_50M");
    EXPECT_EQ(info["range"]["start"]["line"].get<int>(), line);
    EXPECT_EQ(info["range"]["start"]["character"].get<int>(), column);
    EXPECT_EQ(info["range"]["end"]["line"].get<int>(), line);
    EXPECT_EQ(info["range"]["end"]["character"].get<int>(), column + 7);
}

TEST(RenameB3, PrepareRenameOnKeywordCommentOrOutOfRangeIsNull) {
    if (!std::filesystem::exists(FPGA_LSP_SOURCE_DIR "/tests/gate-corpus/corpus_alpha")) {
        GTEST_SKIP() << "corpus missing (release tree)";
    }
    RenameFixture fixture;
    const std::string text = ReadTextFile(kTop);
    const int signal_line = FindLine(text, "wire clk_50M;");
    // 行首是关键字 `wire`（不在符号索引里）
    EXPECT_TRUE(fixture.server().FeaturePrepareRename(kTop, signal_line, 0).is_null());
    // 注释行
    int comment_line = -1;
    {
        std::istringstream stream(text);
        std::string row;
        int line = 0;
        while (std::getline(stream, row)) {
            if (row.find("//") != std::string::npos) {
                comment_line = line;
                break;
            }
            ++line;
        }
    }
    ASSERT_GE(comment_line, 0);
    EXPECT_TRUE(fixture.server().FeaturePrepareRename(kTop, comment_line, 3).is_null());
    // 越界位置与空路径都不崩
    EXPECT_TRUE(fixture.server().FeaturePrepareRename(kTop, 100000, 0).is_null());
    EXPECT_TRUE(fixture.server().FeaturePrepareRename("", 0, 0).is_null());
}

TEST(RenameB3, PrepareRenameOnAdcAndSdcIsNull) {
    if (!std::filesystem::exists(FPGA_LSP_SOURCE_DIR "/tests/gate-corpus/corpus_alpha")) {
        GTEST_SKIP() << "corpus missing (release tree)";
    }
    // Python 的 HDL 索引只有 .al 的 Verilog + VHDL，symbol_at 对 .adc/.sdc 恒为 None。
    // C++ 的索引在 Phase B 之后也装了约束符号（B10），这里必须挡掉，否则契约漂移。
    RenameFixture fixture;
    EXPECT_TRUE(fixture.server().FeaturePrepareRename(kAdc, 0, 0).is_null());
    EXPECT_TRUE(fixture.server().FeaturePrepareRename(kSdc, 0, 0).is_null());
}

// =============================================================================
//  rename：语料 top.v（4 处编辑）
// =============================================================================

TEST(RenameB3, RenameSignalHasFourEditsInTop) {
    if (!std::filesystem::exists(FPGA_LSP_SOURCE_DIR "/tests/gate-corpus/corpus_alpha")) {
        GTEST_SKIP() << "corpus missing (release tree)";
    }
    const std::string text = ReadTextFile(kTop);
    if (text.empty()) GTEST_SKIP() << "语料需自备：语料缺失（设置 FPGA_LSP_TEST_PROJECT 指向自备工程）";
    const int line = FindLine(text, "wire clk_50M;");
    const int column = Column0(text, line, "clk_50M");
    ASSERT_GE(line, 0);

    RenameFixture fixture;
    const Json result = fixture.server().FeatureRename(kTop, line, column + 1, "clk_50M_probe");
    ASSERT_TRUE(result.is_object()) << "Python: {changes:{uri:[4 edtis]}}";
    ASSERT_TRUE(result.contains("changes"));
    const Json& changes = result["changes"];
    ASSERT_EQ(changes.size(), 1u) << "clk_50M 只在 top.v 里出现";

    const std::string uri = fpgalsp::platform::path_to_uri(kTop);
    ASSERT_TRUE(changes.contains(uri));
    const Json& edits = changes[uri];
    ASSERT_EQ(edits.size(), 4u) << "raw.json matrix[11]: 4 处";

    struct Expected {
        int line;
        int character;
    };
    const std::vector<Expected> expected = {
        {line, column},
        {FindLine(text, "assign clk = clk_50M;"),
         Column0(text, FindLine(text, "assign clk = clk_50M;"), "clk_50M")},
        {FindLine(text, "  .clk0_out (clk_50M),"),
         Column0(text, FindLine(text, "  .clk0_out (clk_50M),"), "clk_50M")},
        {FindLine(text, " .clk(clk_50M),"),
         Column0(text, FindLine(text, " .clk(clk_50M),"), "clk_50M")},
    };
    for (std::size_t index = 0; index < expected.size(); ++index) {
        ASSERT_GE(expected[index].line, 0);
        ASSERT_GE(expected[index].character, 0);
        EXPECT_EQ(StartLine(edits[index]), expected[index].line) << "编辑 #" << index;
        EXPECT_EQ(StartCharacter(edits[index]), expected[index].character) << "编辑 #" << index;
        EXPECT_EQ(EndCharacter(edits[index]), expected[index].character + 7) << "编辑 #" << index;
        EXPECT_EQ(edits[index]["newText"].get<std::string>(), "clk_50M_probe");
    }
}

TEST(RenameB3, RenameToSameNameYieldsEmptyChanges) {
    if (!std::filesystem::exists(FPGA_LSP_SOURCE_DIR "/tests/gate-corpus/corpus_alpha")) {
        GTEST_SKIP() << "corpus missing (release tree)";
    }
    const std::string text = ReadTextFile(kTop);
    const int line = FindLine(text, "wire clk_50M;");
    const int column = Column0(text, line, "clk_50M");

    RenameFixture fixture;
    const Json result = fixture.server().FeatureRename(kTop, line, column + 1, "clk_50M");
    ASSERT_TRUE(result.is_object());
    // Python: new_name == info["name"] -> {"edits": {}} -> {"changes": {}}
    ASSERT_TRUE(result.contains("changes"));
    EXPECT_TRUE(result["changes"].is_object());
    EXPECT_TRUE(result["changes"].empty());
}

TEST(RenameB3, RenameRejectsInvalidOrUnrenameableTargets) {
    if (!std::filesystem::exists(FPGA_LSP_SOURCE_DIR "/tests/gate-corpus/corpus_alpha")) {
        GTEST_SKIP() << "corpus missing (release tree)";
    }
    const std::string text = ReadTextFile(kTop);
    const int signal_line = FindLine(text, "wire clk_50M;");
    const int column = Column0(text, signal_line, "clk_50M");

    RenameFixture fixture;
    // 非法标识符（Python 抛 ValueError；这里返回合法 null）
    EXPECT_TRUE(fixture.server().FeatureRename(kTop, signal_line, column + 1, "1abc").is_null());
    EXPECT_TRUE(fixture.server().FeatureRename(kTop, signal_line, column + 1, "module").is_null());
    EXPECT_TRUE(fixture.server().FeatureRename(kTop, signal_line, column + 1, "").is_null());
    // 光标处没有可重命名符号：行尾 `;` **之后**（Python：token_span 返回 None ->
// "No renameable symbol under the cursor"，这里返回合法 null）
    EXPECT_TRUE(fixture.server()
                    .FeatureRename(kTop, signal_line, LineLength(text, signal_line), "x")
                    .is_null());
    EXPECT_TRUE(fixture.server().FeatureRename(kTop, 100000, 0, "x").is_null());
}

TEST(RenameB3, RenameOnKeywordFallsBackToTokenSearch) {
    if (!std::filesystem::exists(FPGA_LSP_SOURCE_DIR "/tests/gate-corpus/corpus_alpha")) {
        GTEST_SKIP() << "corpus missing (release tree)";
    }
    // Python 的行为（probe 实测）：`wire` 不在符号索引里、也不是黑名单关键字，
    // 于是 rename 用 token_span 兜底 -> 把全工程所有 `wire` 改名。
    // 这里只校验"C++ 同样走兜底并且形状合法"，条数与 Python 对齐（33）。
    const std::string text = ReadTextFile(kTop);
    const int signal_line = FindLine(text, "wire clk_50M;");
    ASSERT_GE(signal_line, 0);

    RenameFixture fixture;
    const Json result = fixture.server().FeatureRename(kTop, signal_line, 0, "wire_x");
    ASSERT_TRUE(result.is_object()) << "Python 同样返回 changes（不是 null）";
    ASSERT_TRUE(result.contains("changes"));
    const std::string uri = fpgalsp::platform::path_to_uri(kTop);
    ASSERT_TRUE(result["changes"].contains(uri));
    const Json& edits = result["changes"][uri];
    ASSERT_EQ(edits.size(), 33u) << "Python 侧 top.v 里 `wire` 出现 33 次";
    for (const Json& edit : edits) {
        EXPECT_EQ(edit["newText"].get<std::string>(), "wire_x");
    }
}

// =============================================================================
//  rename：注释 / 字符串 / generate 作用域（临时工程）
// =============================================================================

TEST(RenameB3, RenameExcludesCommentsAndStrings) {
    if (!std::filesystem::exists(FPGA_LSP_SOURCE_DIR "/tests/gate-corpus/corpus_alpha")) {
        GTEST_SKIP() << "corpus missing (release tree)";
    }
    TempProject project("m");
    const std::string source =
        "module m (\n"                              // 0
        "    input  wire clk_50M,\n"                // 1
        "    output wire out\n"                     // 2
        ");\n"                                      // 3
        "    // clk_50M in a line comment\n"        // 4
        "    /* clk_50M in a block comment */\n"    // 5
        "    wire clk_50M_q;\n"                     // 6
        "    assign out = clk_50M;\n"               // 7
        "    assign clk_50M_q = clk_50M;\n"         // 8
        "endmodule\n";                              // 9
    const std::string path = project.Write("src/a.v", source);
    project.WriteAl({"src/a.v"});

    RenameFixture fixture(project.root());
    const int column = Column0(source, 1, "clk_50M");
    const Json result = fixture.server().FeatureRename(path, 1, column + 1, "sig");
    ASSERT_TRUE(result.is_object()) << result.dump();
    const std::string uri = fpgalsp::platform::path_to_uri(path);
    ASSERT_TRUE(result["changes"].contains(uri));
    const Json& edits = result["changes"][uri];

    // 只改"代码里完整标识符"的出现：第 1、7、8 行；注释（4/5）与 clk_50M_q 都不动
    ASSERT_EQ(edits.size(), 3u) << result.dump();
    std::vector<int> lines;
    for (const Json& edit : edits) {
        lines.push_back(StartLine(edit));
        EXPECT_EQ(edit["newText"].get<std::string>(), "sig");
    }
    EXPECT_EQ(lines, (std::vector<int>{1, 7, 8}));
}

TEST(RenameB3, RenameGenerateForVariableStaysInItsScope) {
    if (!std::filesystem::exists(FPGA_LSP_SOURCE_DIR "/tests/gate-corpus/corpus_alpha")) {
        GTEST_SKIP() << "corpus missing (release tree)";
    }
    TempProject project("m");
    const std::string source =
        "module m (input wire clk);\n"                                              // 0
        "    generate\n"                                                           // 1
        "        for (genvar i = 0; i < 2; i = i + 1) begin : g_a\n"                // 2
        "            wire a0;\n"                                                   // 3
        "            assign a0 = clk;\n"                                           // 4
        "        end\n"                                                            // 5
        "    endgenerate\n"                                                        // 6
        "    generate\n"                                                           // 7
        "        for (genvar i = 0; i < 2; i = i + 1) begin : g_b\n"                // 8
        "            wire b0;\n"                                                   // 9
        "        end\n"                                                            // 10
        "    endgenerate\n"                                                        // 11
        "endmodule\n";                                                             // 12
    const std::string path = project.Write("src/gen.v", source);
    project.WriteAl({"src/gen.v"});

    RenameFixture fixture(project.root());
    // `genvar i` 不在 HDL 符号索引里 -> 走"光标 token"回退（Python nav.rename_edits:86-91）
    const int column = Column0(source, 2, "i = 0");
    ASSERT_GE(column, 0);
    const Json result = fixture.server().FeatureRename(path, 2, column, "k");
    ASSERT_TRUE(result.is_object()) << result.dump();
    const std::string uri = fpgalsp::platform::path_to_uri(path);
    ASSERT_TRUE(result["changes"].contains(uri));
    const Json& edits = result["changes"][uri];

    // 第 2 行有 4 个 `i`（genvar 声明 + 条件 + 初值/增量），第 8 行的 4 个 **不能**被改
    ASSERT_EQ(edits.size(), 4u) << result.dump();
    for (const Json& edit : edits) {
        EXPECT_EQ(StartLine(edit), 2) << "generate 作用域外的同名 i 不该被重命名";
        EXPECT_EQ(edit["newText"].get<std::string>(), "k");
    }
}
