// =============================================================================
//  tests/unit/code_actions_test.cpp
// =============================================================================
//  B1 建的骨架文件（target: unit_code_actions，链接 fpga_lsp::server + hdl）。
//  B1a（features-ide）在这里追加 **IDE 类命令纯逻辑** 的用例：
//    src/server/ide_features.{h,cpp}（instantiation / symbolPath / duplicate /
//    searchHierarchy / .al 写入），不依赖 LSP 传输层。
//  B3（parity-lsp）会继续在这个文件里加 codeAction 用例。
// =============================================================================

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "hdl/hdl_index.h"
#include "jsonrpc/transport.h"
#include "platform/path.h"
#include "project/al_project.h"
#include "server/ide_features.h"
#include "server/lsp_server.h"

namespace {

using fpgalsp::server::ide::AddSourceFile;
using fpgalsp::server::ide::BuildInstantiationText;
using fpgalsp::server::ide::CopyInstantiation;
using fpgalsp::server::ide::DuplicateModules;
using fpgalsp::server::ide::Instantiation;
using fpgalsp::server::ide::SearchHierarchy;
using fpgalsp::server::ide::SetProjectName;
using fpgalsp::server::ide::SymbolPath;
using fpgalsp::server::ide::WriteTextFileAtomic;

TEST(CodeActions, PlaceholderUntilB3) { EXPECT_TRUE(true); }

// =============================================================================
//  B1a：IDE 类命令的纯逻辑
// =============================================================================

class IdeFeaturesTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() / "fpgalsp_b1a_test";
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
        std::filesystem::create_directories(root_);
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

    // 索引里的路径必须是规范化的绝对路径（HdlIndex::File 会 normalize 后查表）
    std::string Path(const std::string& name) const {
        return fpgalsp::platform::normalize_path((root_ / name).string());
    }

    // 用内联 Verilog 建一个索引：{文件名 -> 源码}
    //  ⚠ B8 把 HdlIndex 的存储换成了 shared_ptr<const ParsedFile>（PERF-03），
    //     所以这里必须走 make_shared（AddFile 不再接受右值 ParsedFile）。
    fpgalsp::hdl::HdlIndex Index(const std::vector<std::pair<std::string, std::string>>& files) {
        fpgalsp::hdl::HdlIndex index;
        for (const auto& [name, text] : files) {
            index.AddFile(std::make_shared<const fpgalsp::hdl::ParsedFile>(
                fpgalsp::hdl::ParseVerilog(Path(name), text)));
        }
        return index;
    }

    std::string WriteFile(const std::string& name, const std::string& content) const {
        const auto p = root_ / name;
        std::filesystem::create_directories(p.parent_path());
        std::ofstream out(p, std::ios::binary);
        out << content;
        out.close();
        return p.string();
    }

    std::string ReadFile(const std::string& path) const {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    // .al 里如果含 CRLF，就必须**全是** CRLF（不允许混入裸 \n）
    static bool AllNewlinesAreCrlf(const std::string& text) {
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] != '\n') continue;
            if (i == 0 || text[i - 1] != '\r') return false;
        }
        return true;
    }

    std::filesystem::path root_;
};

// -----------------------------------------------------------------------------
//  instantiation 文本（Python server.py::_instantiation_text 的逐行对照）
// -----------------------------------------------------------------------------

TEST_F(IdeFeaturesTest, InstantiationTextMatchesPythonLayout) {
    fpgalsp::hdl::HdlIndex index = Index({{"leaf.v",
                                           "module leaf #(parameter W = 8)\n"
                                           "  (input wire clk, output wire [W-1:0] q);\n"
                                           "endmodule\n"}});
    const fpgalsp::hdl::Module* module = index.ModuleByName("leaf");
    ASSERT_NE(module, nullptr);

    const auto built = BuildInstantiationText(*module, "    ");
    EXPECT_EQ(built.instance, "u_leaf");
    // Python: indent + "\n".join(lines) + "\n"
    EXPECT_EQ(built.text,
              "    leaf #(\n"
              "        .W (W)\n"
              "    ) u_leaf (\n"
              "        .clk (clk),\n"
              "        .q (q)\n"
              "    );\n");

    const nlohmann::json result = Instantiation(index, "leaf", "    ");
    EXPECT_EQ(result.value("ok", false), true);
    EXPECT_EQ(result.value("module", std::string()), "leaf");
    EXPECT_EQ(result.value("instanceName", std::string()), "u_leaf");
    EXPECT_EQ(result.value("portCount", 0), 2);
    EXPECT_EQ(result.value("text", std::string()), built.text);
    // Python 不返回 ports / parameters 数组（那是 MCP 的形状）
    EXPECT_FALSE(result.contains("ports"));
    EXPECT_FALSE(result.contains("parameters"));
}

TEST_F(IdeFeaturesTest, InstantiationWithoutParameters) {
    fpgalsp::hdl::HdlIndex index = Index({{"m.v", "module m (input a);\nendmodule\n"}});
    const nlohmann::json result = Instantiation(index, "m", "  ");
    EXPECT_EQ(result.value("ok", false), true);
    EXPECT_EQ(result.value("text", std::string()),
              "  m u_m (\n"
              "      .a (a)\n"
              "  );\n");
}

TEST_F(IdeFeaturesTest, InstantiationWithoutModuleListsNames) {
    fpgalsp::hdl::HdlIndex index = Index({{"z.v", "module zeta; endmodule\n"},
                                          {"a.v", "module alpha; endmodule\n"}});
    const nlohmann::json result = Instantiation(index, "", "    ");
    ASSERT_TRUE(result.value("ok", false));
    ASSERT_TRUE(result.contains("modules"));
    const std::vector<std::string> names = result["modules"];
    EXPECT_EQ(names, (std::vector<std::string>{"alpha", "zeta"}));  // sorted，同 Python
    EXPECT_FALSE(result.contains("text"));
}

TEST_F(IdeFeaturesTest, InstantiationUnknownModule) {
    fpgalsp::hdl::HdlIndex index = Index({{"a.v", "module alpha; endmodule\n"}});
    const nlohmann::json result = Instantiation(index, "nope", "    ");
    EXPECT_EQ(result.value("ok", true), false);
    EXPECT_EQ(result.value("error", std::string()), "Module not found: nope");
}

// -----------------------------------------------------------------------------
//  copyInstantiation：当前文件的**第一个** module
// -----------------------------------------------------------------------------

TEST_F(IdeFeaturesTest, CopyInstantiationUsesFirstModuleInFile) {
    const std::string path = Path("multi.v");
    fpgalsp::hdl::HdlIndex index = Index({{"multi.v",
                                           "module first (input a);\nendmodule\n"
                                           "module second (input b);\nendmodule\n"}});
    const nlohmann::json result = CopyInstantiation(index, path);
    ASSERT_TRUE(result.value("ok", false)) << result.dump();
    EXPECT_EQ(result.value("module", std::string()), "first");
    EXPECT_EQ(result.value("portCount", 0), 1);
    // ⚠ Python 的 copyInstantiation **没有** instanceName 字段
    EXPECT_FALSE(result.contains("instanceName"));
}

TEST_F(IdeFeaturesTest, CopyInstantiationOnFileWithoutModules) {
    fpgalsp::hdl::HdlIndex index = Index({{"empty.v", "\n"}});
    const nlohmann::json result = CopyInstantiation(index, Path("empty.v"));
    EXPECT_EQ(result.value("ok", true), false);
    EXPECT_EQ(result.value("error", std::string()), "The current file has no module definition");
}

// -----------------------------------------------------------------------------
//  duplicateModules：0-based line/end_line（Python 原样透传）
// -----------------------------------------------------------------------------

TEST_F(IdeFeaturesTest, DuplicateModulesReportsZeroBasedLines) {
    fpgalsp::hdl::HdlIndex index = Index({{"a.v", "module dup (input a);\nendmodule\n"},
                                          {"b.v", "\n\nmodule dup (input b);\nendmodule\n"},
                                          {"c.v", "module unique; endmodule\n"}});
    const nlohmann::json result = DuplicateModules(index);
    ASSERT_TRUE(result.value("ok", false));
    const auto& duplicates = result["duplicates"];
    ASSERT_EQ(duplicates.size(), 1u);
    EXPECT_EQ(duplicates[0].value("name", std::string()), "dup");
    const auto& definitions = duplicates[0]["definitions"];
    ASSERT_EQ(definitions.size(), 2u);
    EXPECT_EQ(definitions[0].value("line", -1), 0);   // 0-based
    EXPECT_EQ(definitions[1].value("line", -1), 2);   // 0-based，不是 3
    EXPECT_EQ(definitions[0].value("file", std::string()), Path("a.v"));
}

// -----------------------------------------------------------------------------
//  searchHierarchy：module + instance（含 parent），0-based line
// -----------------------------------------------------------------------------

TEST_F(IdeFeaturesTest, SearchHierarchyFindsModuleAndInstance) {
    const std::string top = Path("top.v");
    fpgalsp::hdl::HdlIndex index = Index({
        {"sys_rstn.v", "module sys_rstn (input clk, output rst_n);\nendmodule\n"},
        {"top.v", "module top (input clk);\n  sys_rstn sys_rstn (.clk(clk));\nendmodule\n"},
    });
    const nlohmann::json result = SearchHierarchy(index, "sys_rstn");
    ASSERT_TRUE(result.value("ok", false));
    const auto& matches = result["matches"];
    ASSERT_EQ(matches.size(), 2u);
    EXPECT_EQ(matches[0].value("kind", std::string()), "module");
    EXPECT_EQ(matches[0].value("file", std::string()), Path("sys_rstn.v"));
    EXPECT_EQ(matches[0].value("line", -1), 0);
    EXPECT_EQ(matches[0].value("instance", std::string()), "");

    EXPECT_EQ(matches[1].value("kind", std::string()), "instance");
    EXPECT_EQ(matches[1].value("name", std::string()), "sys_rstn");
    EXPECT_EQ(matches[1].value("file", std::string()), top);
    EXPECT_EQ(matches[1].value("line", -1), 1);
    EXPECT_EQ(matches[1].value("parent", std::string()), "top");
}

TEST_F(IdeFeaturesTest, SearchHierarchyEmptyQueryAndLimit) {
    fpgalsp::hdl::HdlIndex index = Index({{"top.v",
                                           "module top (input clk);\n"
                                           "  abc_x inst_a (.clk(clk));\n"
                                           "  abc_y inst_b (.clk(clk));\n"
                                           "endmodule\n"}});
    const nlohmann::json empty = SearchHierarchy(index, "");
    EXPECT_TRUE(empty.value("ok", false));
    EXPECT_TRUE(empty["matches"].empty());

    const nlohmann::json limited = SearchHierarchy(index, "abc", 1);
    ASSERT_TRUE(limited.value("ok", false));
    EXPECT_EQ(limited["matches"].size(), 1u);  // Python: results[:limit]
}

// -----------------------------------------------------------------------------
//  symbolPath：层次路径（照 Python ide.symbol_path 的 while 循环）
// -----------------------------------------------------------------------------

TEST_F(IdeFeaturesTest, SymbolPathWalksInstanceChain) {
    const std::string leaf = Path("leaf.v");
    fpgalsp::hdl::HdlIndex index = Index({
        {"leaf.v", "module leaf (input clk);\n  wire sig;\nendmodule\n"},
        {"sub.v", "module sub (input clk);\n  leaf u_leaf (.clk(clk));\nendmodule\n"},
        {"top.v", "module top (input clk);\n  sub u_sub (.clk(clk));\nendmodule\n"},
    });
    fpgalsp::project::Project project;
    project.top = "top";
    project.dir = fpgalsp::platform::normalize_path(root_.string());

    // 光标落在 leaf 里的 `sig` 上（line 1，character 8）
    const nlohmann::json result = SymbolPath(index, project, leaf, 1, 8);
    ASSERT_TRUE(result.value("ok", false)) << result.dump();
    EXPECT_EQ(result.value("name", std::string()), "sig");
    EXPECT_EQ(result.value("module", std::string()), "leaf");
    EXPECT_EQ(result.value("instancePath", std::string()), "u_sub/u_leaf");
    EXPECT_EQ(result.value("path", std::string()), "u_sub/u_leaf/sig");
}

TEST_F(IdeFeaturesTest, SymbolPathAtTopLevelModuleKeepsBareName) {
    const std::string top = Path("top.v");
    fpgalsp::hdl::HdlIndex index = Index({{"top.v",
                                           "module top (input clk);\n  wire clk_50M;\n"
                                           "endmodule\n"}});
    fpgalsp::project::Project project;
    project.top = "top";
    const nlohmann::json result = SymbolPath(index, project, top, 1, 8);
    ASSERT_TRUE(result.value("ok", false)) << result.dump();
    EXPECT_EQ(result.value("name", std::string()), "clk_50M");
    EXPECT_EQ(result.value("path", std::string()), "clk_50M");
    EXPECT_EQ(result.value("instancePath", std::string()), "");
    EXPECT_EQ(result.value("module", std::string()), "top");
}

TEST_F(IdeFeaturesTest, SymbolPathWithoutSymbol) {
    const std::string empty = Path("blank.v");
    fpgalsp::hdl::HdlIndex index = Index({{"blank.v", "\n\n\n"}});
    fpgalsp::project::Project project;
    project.top = "top";
    const nlohmann::json result = SymbolPath(index, project, empty, 1, 0);
    EXPECT_EQ(result.value("ok", true), false);
    EXPECT_EQ(result.value("error", std::string()), "No copyable symbol under the cursor");
}

// -----------------------------------------------------------------------------
//  addSourceFile：写 .al 的 <Source_Files>（原子写 + 验证）
// -----------------------------------------------------------------------------

namespace {

std::string MakeAlText(const std::string& extra = "", const std::string& eol = "\n") {
    std::string text =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<Project Version=\"3\" Minor=\"2\">\n"
        "  <Name>demo</Name>\n"
        "  <TOP_MODULE><MODULE>top</MODULE></TOP_MODULE>\n"
        "  <Source_Files>\n"
        "    <Verilog>\n"
        "      <File Path=\"src/a.v\"><FileInfo>\n"
        "        <Attr Name=\"CompileOrder\" Val=\"2\"/>\n"
        "      </FileInfo></File>\n"
        "    </Verilog>\n" + extra +
        "  </Source_Files>\n"
        "</Project>\n";
    if (eol == "\n") return text;
    std::string crlf;
    for (const char c : text) {
        if (c == '\n') crlf += eol;
        else crlf.push_back(c);
    }
    return crlf;
}

}  // namespace

TEST_F(IdeFeaturesTest, AddSourceFileAppendsEntryWithNextCompileOrder) {
    const std::string al = WriteFile("proj/demo.al", MakeAlText());
    WriteFile("proj/src/b.v", "module b; endmodule\n");

    const nlohmann::json result =
        AddSourceFile(al, (root_ / "proj" / "src" / "b.v").string());
    ASSERT_TRUE(result.value("ok", false)) << result.dump();
    EXPECT_EQ(result.value("added", false), true);
    EXPECT_EQ(result.value("rel", std::string()), "src/b.v");
    EXPECT_EQ(result.value("compileOrder", 0), 3);  // max(2)+1
    EXPECT_FALSE(result.contains("fileCount"));     // Python 不返回 fileCount

    const std::string updated = ReadFile(al);
    EXPECT_NE(updated.find("<File Path=\"src/b.v\">"), std::string::npos) << updated;
    EXPECT_NE(updated.find("Name=\"CompileOrder\" Val=\"3\""), std::string::npos) << updated;
    // 插入后仍能被解析，且文件数 +1
    const fpgalsp::project::Project parsed = fpgalsp::project::ParseAl(al);
    EXPECT_EQ(parsed.files.size(), 2u);
    // 不留下临时文件
    EXPECT_FALSE(std::filesystem::exists(al + ".fpgalsp-tmp"));
}

TEST_F(IdeFeaturesTest, AddSourceFileIsIdempotent) {
    const std::string al = WriteFile("proj/demo.al", MakeAlText());
    WriteFile("proj/src/b.v", "module b; endmodule\n");
    const std::string before = ReadFile(al);

    const nlohmann::json first = AddSourceFile(al, (root_ / "proj" / "src" / "b.v").string());
    ASSERT_TRUE(first.value("ok", false));
    const std::string after_first = ReadFile(al);

    const nlohmann::json second = AddSourceFile(al, (root_ / "proj" / "src" / "b.v").string());
    ASSERT_TRUE(second.value("ok", false));
    EXPECT_EQ(second.value("added", true), false);
    EXPECT_TRUE(second["compileOrder"].is_null());
    EXPECT_EQ(second.value("rel", std::string()), "src/b.v");
    EXPECT_EQ(ReadFile(al), after_first);  // 第二次不写盘
    EXPECT_NE(before, after_first);
}

TEST_F(IdeFeaturesTest, AddSourceFileKeepsCrlfLineEndings) {
    const std::string al = WriteFile("proj/demo.al", MakeAlText("", "\r\n"));
    WriteFile("proj/src/b.v", "module b; endmodule\n");

    const nlohmann::json result = AddSourceFile(al, (root_ / "proj" / "src" / "b.v").string());
    ASSERT_TRUE(result.value("ok", false)) << result.dump();
    const std::string updated = ReadFile(al);
    // 有意与 Python 不同：不把整文件改写成 LF（见 ide_features.cpp 文件头）
    EXPECT_TRUE(AllNewlinesAreCrlf(updated));
    EXPECT_NE(updated.find("<File Path=\"src/b.v\">"), std::string::npos);
}

TEST_F(IdeFeaturesTest, AddSourceFileWithoutSourceFilesSectionFails) {
    const std::string al = WriteFile("proj/demo.al",
                                     "<Project Version=\"3\"><Name>demo</Name></Project>\n");
    const std::string before = ReadFile(al);
    const nlohmann::json result = AddSourceFile(al, (root_ / "proj" / "src" / "b.v").string());
    EXPECT_EQ(result.value("ok", true), false);
    EXPECT_EQ(result.value("error", std::string()), "No <Source_Files> section in the .al");
    EXPECT_EQ(ReadFile(al), before);  // 失败保持原文件
}

TEST_F(IdeFeaturesTest, AddSourceFileUsesVhdlTagForVhdlExtensions) {
    const std::string al = WriteFile("proj/demo.al", MakeAlText());
    WriteFile("proj/src/c.vhd", "entity c is end;\n");
    const nlohmann::json result = AddSourceFile(al, (root_ / "proj" / "src" / "c.vhd").string());
    ASSERT_TRUE(result.value("ok", false)) << result.dump();
    EXPECT_NE(ReadFile(al).find("<VHDL>"), std::string::npos);
}

// -----------------------------------------------------------------------------
//  setProjectName：写 .al 的 <Name>
// -----------------------------------------------------------------------------

TEST_F(IdeFeaturesTest, SetProjectNameRewritesNameOnly) {
    const std::string al = WriteFile("proj/demo.al", MakeAlText());
    const nlohmann::json result = SetProjectName(al, " demo_probe ");
    ASSERT_TRUE(result.value("ok", false)) << result.dump();
    EXPECT_EQ(result.value("changed", false), true);
    EXPECT_EQ(result.value("name", std::string()), "demo_probe");  // 两端空白被 strip
    // Python 的 setProjectName **不返回** al 字段
    EXPECT_FALSE(result.contains("al"));

    const std::string updated = ReadFile(al);
    EXPECT_NE(updated.find("<Name>demo_probe</Name>"), std::string::npos);
    const fpgalsp::project::Project parsed = fpgalsp::project::ParseAl(al);
    EXPECT_EQ(parsed.name, "demo_probe");
    EXPECT_EQ(parsed.files.size(), 1u);
}

TEST_F(IdeFeaturesTest, SetProjectNameUnchangedKeepsFileUntouched) {
    const std::string al = WriteFile("proj/demo.al", MakeAlText());
    const std::string before = ReadFile(al);
    const nlohmann::json result = SetProjectName(al, "demo");
    ASSERT_TRUE(result.value("ok", false));
    EXPECT_EQ(result.value("changed", true), false);
    EXPECT_EQ(result.value("name", std::string()), "demo");
    EXPECT_EQ(ReadFile(al), before);
}

TEST_F(IdeFeaturesTest, SetProjectNameRejectsBadInput) {
    const std::string al = WriteFile("proj/demo.al", MakeAlText());
    EXPECT_EQ(SetProjectName(al, "  ").value("error", std::string()),
              "Project name cannot be empty");
    EXPECT_EQ(SetProjectName(al, "a<b").value("error", std::string()),
              "Project name cannot contain < > \" &");
    EXPECT_EQ(ReadFile(al), MakeAlText());  // 原文件没被碰过
}

TEST_F(IdeFeaturesTest, SetProjectNameInsertsNameWhenMissing) {
    const std::string al = WriteFile(
        "proj/demo.al",
        "<Project Version=\"3\">\n  <TOP_MODULE><MODULE>top</MODULE></TOP_MODULE>\n</Project>\n");
    const nlohmann::json result = SetProjectName(al, "inserted");
    ASSERT_TRUE(result.value("ok", false)) << result.dump();
    EXPECT_EQ(result.value("changed", false), true);
    EXPECT_NE(ReadFile(al).find("<Name>inserted</Name>"), std::string::npos);
}

TEST_F(IdeFeaturesTest, SetProjectNameKeepsCrlfLineEndings) {
    const std::string al = WriteFile("proj/demo.al", MakeAlText("", "\r\n"));
    const nlohmann::json result = SetProjectName(al, "demo2");
    ASSERT_TRUE(result.value("ok", false)) << result.dump();
    EXPECT_TRUE(AllNewlinesAreCrlf(ReadFile(al)));
}

TEST_F(IdeFeaturesTest, SetProjectNameWithoutProjectTagFails) {
    const std::string al = WriteFile("proj/demo.al", "<Config><Name>demo</Name></Config>\n");
    const std::string before = ReadFile(al);
    // 有 <Name> 时会被替换（Python 也一样），这里测的是"完全没有 <Project>"的分支
    const std::string al2 = WriteFile("proj/broken.al", "<Config/>\n");
    const std::string before2 = ReadFile(al2);
    const nlohmann::json result = SetProjectName(al2, "x");
    EXPECT_EQ(result.value("ok", true), false);
    EXPECT_EQ(result.value("error", std::string()), "No <Project> tag in the .al");
    EXPECT_EQ(ReadFile(al2), before2);
    EXPECT_EQ(ReadFile(al), before);
}

// -----------------------------------------------------------------------------
//  原子写：成功替换、不残留临时文件
// -----------------------------------------------------------------------------

TEST_F(IdeFeaturesTest, WriteTextFileAtomicReplacesContent) {
    const std::string path = WriteFile("proj/atomic.al", "old");
    EXPECT_TRUE(WriteTextFileAtomic(path, "new-content"));
    EXPECT_EQ(ReadFile(path), "new-content");
    EXPECT_FALSE(std::filesystem::exists(path + ".fpgalsp-tmp"));
    EXPECT_FALSE(std::filesystem::exists(path + ".fpgalsp-bak"));
}

TEST_F(IdeFeaturesTest, WriteTextFileAtomicFailsWithoutTouchingTarget) {
    // 目标目录不存在 -> 临时文件都写不出来，目标不会有任何变化
    const std::string missing = (root_ / "no_such_dir" / "x.al").string();
    EXPECT_FALSE(WriteTextFileAtomic(missing, "data"));
    EXPECT_FALSE(std::filesystem::exists(missing));
}

}  // namespace

// =============================================================================
//  B3（parity-lsp）：textDocument/codeAction（矩阵 LSP-03）
// =============================================================================
//  语料 tests/gate-corpus/corpus_alpha 与真实工程 %WORKSPACE%\CorpusAlpha 逐字节相同，
//  锚点与 audit/2026-11-18/parity-lsp/raw.json matrix[9] 一致：
//    module top 在 14:7（光标 14:8）、实例 sys_rstn 在 72:11、wire clk_50M 在 58:5。
//
//  Python 契约（server.py:1555-1609）：
//    [ {title:"Insert instantiation of top", kind:"refactor", isPreferred:true, edit{...}},
//      {title:"Verible: apply lint autofixes", kind:"quickfix", edit{...}} ]
//  顺序固定（实例化在前），且 quickfix **没有** isPreferred 字段。
// =============================================================================

namespace {

using fpgalsp::server::Json;

const std::string kCodeActionProject =
    std::string(FPGA_LSP_SOURCE_DIR) + "/tests/gate-corpus/corpus_alpha";
const std::string kCodeActionTop = kCodeActionProject + "/src/top.v";
const std::string kCodeActionAdc = kCodeActionProject + "/constrains/pin.adc";
const std::string kCodeActionSdc = kCodeActionProject + "/constrains/top.sdc";

std::string ReadTextFile(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.good()) return {};
    return std::string((std::istreambuf_iterator<char>(stream)),
                       std::istreambuf_iterator<char>());
}

// 最小 LspServer 夹具（与 call_hierarchy_test 同一形状）：本地 stringstream 充当传输
class CodeActionFixture {
public:
    explicit CodeActionFixture(const std::string& root = kCodeActionProject)
        : transport_(input_, output_), server_(transport_) {
        Json params = Json::object();
        params["rootUri"] = fpgalsp::platform::path_to_uri(root);
        server_.FeatureInitialize(params);
    }

    fpgalsp::server::LspServer& server() { return server_; }

private:
    std::istringstream input_;
    std::ostringstream output_;
    fpgalsp::jsonrpc::LspTransport transport_;
    fpgalsp::server::LspServer server_;
};

// 第一处包含 needle 的行号（0-based）；找不到返回 -1
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

// 该行里 token 的 0-based 列 + 1（落在标识符内部，与 probe 的锚点做法一致）
int ColumnOf(const std::string& text, int line, const std::string& token) {
    std::istringstream stream(text);
    std::string row;
    for (int index = 0;; ++index) {
        if (!std::getline(stream, row)) return -1;
        if (index != line) continue;
        const std::size_t at = row.find(token);
        return at == std::string::npos ? -1 : static_cast<int>(at) + 1;
    }
}

Json PointRange(int line, int character) {
    Json position = Json::object();
    position["line"] = line;
    position["character"] = character;
    Json range = Json::object();
    range["start"] = position;
    range["end"] = position;
    return range;
}

const Json* FindAction(const Json& actions, const std::string& title) {
    for (const Json& action : actions) {
        if (action.is_object() && action.contains("title") &&
            action["title"].get<std::string>() == title) {
            return &action;
        }
    }
    return nullptr;
}

// 与 features_code_actions.cpp 的 FindVeribleLinter 等价的**测试侧**探测。
// 找不到外部 verible-verilog-lint 时跳过 autofix 用例（Python 侧同样不产生该动作）。
bool VeribleLinterAvailable() {
    const std::vector<std::string> candidates = {
        "C:/verible/bin/verible-verilog-lint.exe",
        "C:/tools/verible/bin/verible-verilog-lint.exe",
        "C:/Program Files/verible/bin/verible-verilog-lint.exe",
    };
    for (const std::string& candidate : candidates) {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error)) return true;
    }
    const char* home = std::getenv("USERPROFILE");
    if (home != nullptr) {
        const std::string base(home);
        for (const std::string& relative : {"/verible/verible-verilog-lint.exe",
                                            "/verible/bin/verible-verilog-lint.exe",
                                            "/tools/verible/verible-verilog-lint.exe",
                                            "/tools/verible/bin/verible-verilog-lint.exe",
                                            "/scoop/apps/verible/current/bin/verible-verilog-lint.exe"}) {
            std::error_code error;
            if (std::filesystem::is_regular_file(base + relative, error)) return true;
        }
    }
    const char* path_env = std::getenv("PATH");
    if (path_env != nullptr) {
        const std::string paths(path_env);
        std::size_t position = 0;
        while (position <= paths.size()) {
            const std::size_t separator = paths.find(';', position);
            const std::size_t end = separator == std::string::npos ? paths.size() : separator;
            const std::string directory = paths.substr(position, end - position);
            if (!directory.empty()) {
                std::error_code error;
                if (std::filesystem::is_regular_file(
                        directory + "\\verible-verilog-lint.exe", error)) {
                    return true;
                }
            }
            if (separator == std::string::npos) break;
            position = separator + 1;
        }
    }
    return false;
}

}  // namespace

TEST(CodeActionsB3, ModuleHeaderYieldsInstantiationRefactor) {
    const std::string text = ReadTextFile(kCodeActionTop);
    ASSERT_FALSE(text.empty());
    const int line = FindLine(text, "module top");
    ASSERT_GE(line, 0);
    const int character = ColumnOf(text, line, "top");
    ASSERT_GE(character, 0);

    CodeActionFixture fixture;
    const Json actions =
        fixture.server().FeatureCodeActions(kCodeActionTop, PointRange(line, character),
                                            Json::object());
    ASSERT_TRUE(actions.is_array());
    ASSERT_FALSE(actions.empty()) << "Python: 实例化 refactor + Verible autofix";

    const Json& action = actions[0];
    EXPECT_EQ(action["title"].get<std::string>(), "Insert instantiation of top");
    EXPECT_EQ(action["kind"].get<std::string>(), "refactor");
    EXPECT_TRUE(action.value("isPreferred", false));

    const std::string uri = fpgalsp::platform::path_to_uri(kCodeActionTop);
    ASSERT_TRUE(action["edit"]["changes"].contains(uri));
    const Json& edits = action["edit"]["changes"][uri];
    ASSERT_EQ(edits.size(), 1u);
    // 插入点 = 当前行行首（Python: make_range(line, 0, line, 0)）
    EXPECT_EQ(edits[0]["range"]["start"]["line"].get<int>(), line);
    EXPECT_EQ(edits[0]["range"]["start"]["character"].get<int>(), 0);
    EXPECT_EQ(edits[0]["range"]["end"]["line"].get<int>(), line);
    EXPECT_EQ(edits[0]["range"]["end"]["character"].get<int>(), 0);

    // 文本与 Python _instantiation_text 同形（缩进取行首空白，此处是 0）
    const std::string new_text = edits[0]["newText"].get<std::string>();
    EXPECT_EQ(new_text.rfind("top #(\n", 0), 0u) << new_text.substr(0, 120);
    EXPECT_NE(new_text.find("    .DEY_CNT (DEY_CNT)\n) u_top (\n"), std::string::npos)
        << new_text.substr(0, 200);
    EXPECT_NE(new_text.find("    .clk_input (clk_input),\n"), std::string::npos);
    ASSERT_GE(new_text.size(), 3u);
    EXPECT_EQ(new_text.substr(new_text.size() - 3), ");\n");
}

TEST(CodeActionsB3, InstanceNameYieldsThatModulesInstantiation) {
    const std::string text = ReadTextFile(kCodeActionTop);
    ASSERT_FALSE(text.empty());
    const int line = FindLine(text, "sys_rstn  sys_rstn");
    ASSERT_GE(line, 0);
    const int character = ColumnOf(text, line, "sys_rstn(");
    ASSERT_GE(character, 0);

    CodeActionFixture fixture;
    const Json actions =
        fixture.server().FeatureCodeActions(kCodeActionTop, PointRange(line, character),
                                            Json::object());
    ASSERT_FALSE(actions.empty());
    EXPECT_EQ(actions[0]["title"].get<std::string>(), "Insert instantiation of sys_rstn");
    const std::string uri = fpgalsp::platform::path_to_uri(kCodeActionTop);
    const std::string new_text =
        actions[0]["edit"]["changes"][uri][0]["newText"].get<std::string>();
    // 行首有一个空格 -> 缩进 " "（Python: row[: len(row)-len(row.lstrip())]）；
    // sys_rstn 有 1 个参数（cnt_set），所以走 "#(" 分支
    EXPECT_EQ(new_text.rfind(" sys_rstn #(\n", 0), 0u) << new_text.substr(0, 120);
    EXPECT_NE(new_text.find("     .cnt_set (cnt_set)\n ) u_sys_rstn (\n"), std::string::npos)
        << new_text.substr(0, 200);
    EXPECT_NE(new_text.find("     .clk (clk),\n"), std::string::npos);
    ASSERT_GE(new_text.size(), 4u);
    // Python: lines.append("%s);" % indent) -> 收尾行带行首缩进
    EXPECT_EQ(new_text.substr(new_text.size() - 4), " );\n");
}

TEST(CodeActionsB3, SignalPositionHasNoInstantiationAction) {
    const std::string text = ReadTextFile(kCodeActionTop);
    const int line = FindLine(text, "wire clk_50M;");
    ASSERT_GE(line, 0);
    const int character = ColumnOf(text, line, "clk_50M");
    ASSERT_GE(character, 0);

    CodeActionFixture fixture;
    const Json actions =
        fixture.server().FeatureCodeActions(kCodeActionTop, PointRange(line, character),
                                            Json::object());
    // 信号名不是模块 -> 没有 refactor；autofix 若可用则只剩 quickfix
    for (const Json& action : actions) {
        EXPECT_EQ(action["title"].get<std::string>().find("Insert instantiation"),
                  std::string::npos);
    }
}

TEST(CodeActionsB3, VeribleAutofixReplacesWholeDocument) {
    if (!VeribleLinterAvailable()) {
        GTEST_SKIP() << "verible-verilog-lint not found (Python 侧同样不产生该动作)";
    }
    const std::string text = ReadTextFile(kCodeActionTop);
    const int line = FindLine(text, "module top");
    const int character = ColumnOf(text, line, "top");

    CodeActionFixture fixture;
    const Json actions =
        fixture.server().FeatureCodeActions(kCodeActionTop, PointRange(line, character),
                                            Json::object());
    const Json* autofix = FindAction(actions, "Verible: apply lint autofixes");
    ASSERT_NE(autofix, nullptr) << "Python: 第 2 条是 lint autofix quickfix";
    EXPECT_EQ((*autofix)["kind"].get<std::string>(), "quickfix");
    // Python 的 quickfix **没有** isPreferred
    EXPECT_FALSE(autofix->contains("isPreferred"));

    const std::string uri = fpgalsp::platform::path_to_uri(kCodeActionTop);
    const Json& edits = (*autofix)["edit"]["changes"][uri];
    ASSERT_EQ(edits.size(), 1u);
    EXPECT_EQ(edits[0]["range"]["start"]["line"].get<int>(), 0);
    EXPECT_EQ(edits[0]["range"]["start"]["character"].get<int>(), 0);
    // 覆盖全文：末行 = 原文换行数（top.v 以换行结尾 -> 最后一个空行第 0 列）
    const std::size_t newlines = static_cast<std::size_t>(
        std::count(text.begin(), text.end(), '\n'));
    EXPECT_EQ(edits[0]["range"]["end"]["line"].get<int>(), static_cast<int>(newlines));
    EXPECT_EQ(edits[0]["range"]["end"]["character"].get<int>(), 0);

    const std::string new_text = edits[0]["newText"].get<std::string>();
    EXPECT_NE(new_text, text);
    EXPECT_EQ(new_text.find('\r'), std::string::npos) << "Python 的 newText 是 LF 文本";
    ASSERT_GE(new_text.size(), 1u);
    EXPECT_EQ(new_text.back(), '\n');
}

TEST(CodeActionsB3, NonVerilogPathYieldsNoActions) {
    CodeActionFixture fixture;
    // Python: 只有 .v/.sv/.vh/.svh 与 .adc 有 codeAction，.sdc 恒为空
    EXPECT_TRUE(fixture.server()
                    .FeatureCodeActions(kCodeActionSdc, PointRange(0, 0), Json::object())
                    .empty());
    // 不存在的路径也不该崩，返回合法空结果
    EXPECT_TRUE(fixture.server()
                    .FeatureCodeActions(kCodeActionProject + "/src/no_such_file.v",
                                        PointRange(0, 0), Json::object())
                    .empty());
}

TEST(CodeActionsB3, AdcPinAssignmentLineOffersRemove) {
    const std::string text = ReadTextFile(kCodeActionAdc);
    ASSERT_FALSE(text.empty());
    const int line = FindLine(text, "set_pin_assignment");
    ASSERT_GE(line, 0);

    CodeActionFixture fixture;
    const Json actions = fixture.server().FeatureCodeActions(
        kCodeActionAdc, PointRange(line, 0), Json::object());
    ASSERT_EQ(actions.size(), 1u) << "该行已有 IOSTANDARD -> 只有 Remove";
    EXPECT_EQ(actions[0]["title"].get<std::string>(), "Remove this pin assignment");
    EXPECT_EQ(actions[0]["kind"].get<std::string>(), "quickfix");
    EXPECT_FALSE(actions[0].contains("isPreferred"));

    const std::string uri = fpgalsp::platform::path_to_uri(kCodeActionAdc);
    const Json& edit = actions[0]["edit"]["changes"][uri][0];
    // 删整行：line,0 -> line+1,0（Python 的同一条规则）
    EXPECT_EQ(edit["range"]["start"]["line"].get<int>(), line);
    EXPECT_EQ(edit["range"]["start"]["character"].get<int>(), 0);
    EXPECT_EQ(edit["range"]["end"]["line"].get<int>(), line + 1);
    EXPECT_EQ(edit["range"]["end"]["character"].get<int>(), 0);
    EXPECT_EQ(edit["newText"].get<std::string>(), "");
}

TEST(CodeActionsB3, AdcMissingIostandardOffersInsert) {
    // .adc 分支在"工程查找"之前（Python 同样如此），所以临时文件不需要 .al
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "fpgalsp_b3_code_actions";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    const std::filesystem::path path = directory / "pin.adc";
    const std::string line0 = "set_pin_assignment\t{ clk_input }\t{ LOCATION = P87; }";
    const std::string content =
        line0 + "\n" + "set_pin_assignment\t{ x }\t{ LOCATION = P1; IOSTANDARD = LVCMOS33; }\n";
    {
        std::ofstream stream(path, std::ios::binary);
        stream << content;
    }

    CodeActionFixture fixture;
    const Json actions = fixture.server().FeatureCodeActions(
        path.string(), PointRange(0, 0), Json::object());
    ASSERT_EQ(actions.size(), 2u) << "缺 IOSTANDARD -> Remove + Add IOSTANDARD";
    EXPECT_EQ(actions[0]["title"].get<std::string>(), "Remove this pin assignment");

    const Json& insert = actions[1];
    EXPECT_EQ(insert["title"].get<std::string>(), "Add IOSTANDARD = LVCMOS33");
    EXPECT_TRUE(insert.value("isPreferred", false));
    const std::string uri = fpgalsp::platform::path_to_uri(path.string());
    const Json& edit = insert["edit"]["changes"][uri][0];
    const std::size_t brace = line0.rfind('}');
    ASSERT_NE(brace, std::string::npos);
    EXPECT_EQ(edit["range"]["start"]["line"].get<int>(), 0);
    EXPECT_EQ(edit["range"]["start"]["character"].get<int>(), static_cast<int>(brace));
    EXPECT_EQ(edit["range"]["end"]["character"].get<int>(), static_cast<int>(brace));
    EXPECT_EQ(edit["newText"].get<std::string>(), " IOSTANDARD = LVCMOS33;");

    std::filesystem::remove_all(directory, error);
}