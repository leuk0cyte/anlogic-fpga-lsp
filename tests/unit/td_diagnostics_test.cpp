// =============================================================================
//  tests/unit/td_diagnostics_test.cpp —— TdMessage → Diagnostic 映射
// =============================================================================
//  基准行为对照 Y:\fpga-lsp\fpgalsp\td.py 的 run_read_hdl 与 _resolve_file。
// =============================================================================

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "td/td_diagnostics.h"
#include "td/td_output.h"

namespace {

using namespace fpgalsp;
using namespace fpgalsp::td;

// 一个最小的 test Project
project::Project MakeProject(const std::string& dir = "Y:/proj",
                             const std::string& declared = "") {
    project::Project p;
    p.dir = dir;
    p.al_path = dir + "/demo.al";
    p.name = "demo";
    p.declared_path = declared;
    p.top = "top";
    p.device = "EF2L45LG144B";
    p.family = "EF2";
    return p;
}

project::AlFile AddFile(project::Project& p, const std::string& rel,
                        project::FileKind kind = project::FileKind::Verilog) {
    project::AlFile f;
    f.rel = rel;
    f.path = p.dir + "/" + rel;
    f.kind = kind;
    f.exists = true;
    p.files.push_back(f);
    return f;
}

TdMessage MakeMsg(std::string code, int severity, std::string message,
                  std::optional<std::string> path = std::nullopt,
                  std::optional<int> line = std::nullopt) {
    TdMessage m;
    m.code = std::move(code);
    m.severity = severity;
    m.message = std::move(message);
    m.path = std::move(path);
    m.line = std::move(line);
    return m;
}

class TdDiagFsTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() / "fpgalsp_tddiag_test";
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
        std::filesystem::create_directories(root_);
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }
    std::string Touch(const std::string& rel) const {
        const auto p = root_ / rel;
        std::filesystem::create_directories(p.parent_path());
        std::ofstream(p) << "x";
        return p.string();
    }
    std::string Dir() const { return root_.string(); }
    std::filesystem::path root_;
};

// -----------------------------------------------------------------------------
// 路径解析：绝对路径
// -----------------------------------------------------------------------------

TEST(TdResolveFile, AbsolutePathUsedDirectly) {
    const auto prj = MakeProject("Y:/proj");
    EXPECT_EQ(ResolveTdFile(std::string("Y:/other/a.v"), prj), "Y:/other/a.v");
}

TEST(TdResolveFile, AbsoluteBackslashesConverted) {
    const auto prj = MakeProject("Y:/proj");
    EXPECT_EQ(ResolveTdFile(std::string("C:\\proj\\src\\a.v"), prj), "C:/proj/src/a.v");
}

TEST(TdResolveFile, EmptyPathFallsBackToAl) {
    // 对应 Python: if not path: return project.al_path
    const auto prj = MakeProject("Y:/proj");
    EXPECT_EQ(ResolveTdFile(std::nullopt, prj), "Y:/proj/demo.al");
    EXPECT_EQ(ResolveTdFile(std::string(""), prj), "Y:/proj/demo.al");
}

// -----------------------------------------------------------------------------
// 路径解析：相对路径（需要真实文件系统判断 isfile）
// -----------------------------------------------------------------------------

TEST_F(TdDiagFsTest, RelativeResolvesAgainstProjectDir) {
    Touch("proj/src/a.v");
    const auto prj = MakeProject(Dir() + "/proj");

    const std::string got = ResolveTdFile(std::string("src/a.v"), prj);
    EXPECT_NE(got.find("src/a.v"), std::string::npos);
    EXPECT_TRUE(std::filesystem::exists(got)) << got;
    EXPECT_EQ(got.find('\\'), std::string::npos);
}

TEST_F(TdDiagFsTest, RelativePrefersProjectDirOverDeclared) {
    // 两边都存在时应优先 project.dir
    Touch("proj/src/a.v");
    Touch("declared/src/a.v");
    const auto prj = MakeProject(Dir() + "/proj", Dir() + "/declared");

    const std::string got = ResolveTdFile(std::string("src/a.v"), prj);
    EXPECT_NE(got.find("proj/src/a.v"), std::string::npos) << got;
}

TEST_F(TdDiagFsTest, RelativeFallsBackToDeclaredWhenProjectMissing) {
    // ⚠ PRJ-6619 那条逻辑：工程被拷走后文件只在 declared_path 下存在
    Touch("declared/moved.v");
    const auto prj = MakeProject(Dir() + "/proj", Dir() + "/declared");

    const std::string got = ResolveTdFile(std::string("moved.v"), prj);
    EXPECT_NE(got.find("declared"), std::string::npos) << got;
    EXPECT_TRUE(std::filesystem::exists(got)) << got;
}

TEST_F(TdDiagFsTest, RelativeMissingFallsBackToProjectDirJoin) {
    // 都不存在 -> 回落到 join(project.dir, clean)
    const auto prj = MakeProject(Dir() + "/proj");
    const std::string got = ResolveTdFile(std::string("no/such/file.v"), prj);
    EXPECT_NE(got.find("proj"), std::string::npos) << got;
    EXPECT_NE(got.find("no/such/file.v"), std::string::npos) << got;
}

TEST(TdResolveFile, NoProjectDirReturnsCleanPath) {
    auto prj = MakeProject("");
    prj.al_path = "";
    EXPECT_EQ(ResolveTdFile(std::string("rel/a.v"), prj), "rel/a.v");
}

TEST(TdResolveFile, NeverThrowsOnWeirdInput) {
    const auto prj = MakeProject("Y:/proj");
    for (const char* weird : {"", "/", "//", "a", "..", "../..", "C:", "Z:\\"}) {
        EXPECT_NO_THROW(ResolveTdFile(std::string(weird), prj)) << weird;
    }
}

// -----------------------------------------------------------------------------
// 行号转换（TD 1-based -> LSP 0-based）
// -----------------------------------------------------------------------------

TEST(TdMapMessage, LineOneBecomesZero) {
    const auto prj = MakeProject();
    // TD 第 1 行 -> LSP 第 0 行
    const auto d = MapMessage(MakeMsg("HDL-1007", 3, "m", std::string("Y:/a.v"), 1), prj);
    EXPECT_EQ(d.range.start.line, 0);
    EXPECT_EQ(d.range.end.line, 0);
}

TEST(TdMapMessage, LineFifteenBecomesFourteen) {
    const auto prj = MakeProject();
    const auto d = MapMessage(MakeMsg("HDL-1007", 3, "m", std::string("Y:/a.v"), 15), prj);
    EXPECT_EQ(d.range.start.line, 14);
}

TEST(TdMapMessage, NoLineBecomesZero) {
    const auto prj = MakeProject();
    const auto d = MapMessage(MakeMsg("HDL-1007", 3, "m", std::nullopt, std::nullopt), prj);
    EXPECT_EQ(d.range.start.line, 0);
}

TEST(TdMapMessage, CharacterIsAlwaysZero) {
    // TD 无列号
    const auto prj = MakeProject();
    const auto d = MapMessage(MakeMsg("HDL-7144", 2, "m", std::string("Y:/a.v"), 66), prj);
    EXPECT_EQ(d.range.start.character, 0);
    EXPECT_EQ(d.range.end.character, 1) << "range 应为 1 列宽（对应 _diag 的 char+1）";
}

TEST(TdMapMessage, ZeroLineDoesNotProduceNegative) {
    // 畸形输入 line=0：钳到 0，不产生 -1
    const auto prj = MakeProject();
    const auto d = MapMessage(MakeMsg("X-1", 3, "m", std::string("Y:/a.v"), 0), prj);
    EXPECT_EQ(d.range.start.line, 0);
}

// -----------------------------------------------------------------------------
// 严重度映射
// -----------------------------------------------------------------------------

TEST(TdMapMessage, ErrorSeverity) {
    const auto prj = MakeProject();
    const auto d = MapMessage(MakeMsg("USR-8001", 1, "boom"), prj);
    EXPECT_EQ(d.severity, lsp::kError);
}

TEST(TdMapMessage, CriticalWarningSeverity) {
    const auto prj = MakeProject();
    const auto d = MapMessage(MakeMsg("HDL-7144", 2, "instantiate unknown module"), prj);
    EXPECT_EQ(d.severity, lsp::kWarning);
}

TEST(TdMapMessage, NoSeverityIsInfo) {
    // ⚠ 最关键：无 severity 的 HDL-1007 必须是 Info，不是 warning
    const auto prj = MakeProject();
    const auto d = MapMessage(MakeMsg("HDL-1007", 3, "elaborate module top"), prj);
    EXPECT_EQ(d.severity, lsp::kInfo);
}

TEST(TdMapMessage, BlackBoxUpgradedToWarning) {
    // 对应 Python: "black box" in message.lower() -> severity = 2 + 追加说明
    const auto prj = MakeProject();
    const auto d = MapMessage(MakeMsg("HDL-1007", 3, "treating module as Black Box"), prj);
    EXPECT_EQ(d.severity, lsp::kWarning);
    EXPECT_NE(d.message.find("(IP/module not in the build"), std::string::npos);
}

TEST(TdMapMessage, BlackBoxCaseInsensitive) {
    const auto prj = MakeProject();
    const auto d = MapMessage(MakeMsg("X-1", 3, "BLACK BOX here"), prj);
    EXPECT_EQ(d.severity, lsp::kWarning);
}

TEST(TdMapMessage, SyntheticIgnoredMessageNotDoubleUpgraded) {
    // 合成消息含 "black box" 但也含 "TD ignored this file" -> 不追加说明
    const auto prj = MakeProject();
    const std::string text =
        "TD ignored this file due to errors; the module is treated as a black box";
    const auto d = MapMessage(MakeMsg("X-1", 2, text), prj);
    EXPECT_EQ(d.severity, lsp::kWarning);
    EXPECT_EQ(d.message, text) << "含 'TD ignored this file' 时不应追加说明";
}

// -----------------------------------------------------------------------------
// HDL-8007 无行号回落 .al
// -----------------------------------------------------------------------------

TEST(TdMapMessage, Hdl8007WithoutLineFallsBackToAlPath) {
    const auto prj = MakeProject("Y:/proj");
    const auto d = MapMessage(MakeMsg("HDL-8007", 1, "bad syntax",
                                      std::string("Y:/some/file.v"), std::nullopt),
                              prj);
    const std::string path = d.data["path"].get<std::string>();
    EXPECT_EQ(path, "Y:/proj/demo.al");
}

TEST(TdMapMessage, Hdl8007WithLineKeepsFile) {
    const auto prj = MakeProject("Y:/proj");
    const auto d = MapMessage(MakeMsg("HDL-8007", 1, "bad syntax",
                                      std::string("Y:/some/file.v"), 42),
                              prj);
    EXPECT_EQ(d.data["path"], "Y:/some/file.v");
}

TEST(TdMapMessage, OtherCodesWithoutLineKeepResolvedPath) {
    const auto prj = MakeProject("Y:/proj");
    const auto d = MapMessage(MakeMsg("HDL-5107", 2, "no top", std::string("Y:/a.v"),
                                      std::nullopt),
                              prj);
    // 非 HDL-8007 不做 .al 回落
    EXPECT_EQ(d.data["path"], "Y:/a.v");
}

// -----------------------------------------------------------------------------
// 元数据
// -----------------------------------------------------------------------------

TEST(TdMapMessage, SourceAndBackend) {
    const auto prj = MakeProject();
    const auto d = MapMessage(MakeMsg("HDL-1007", 3, "m"), prj);
    EXPECT_EQ(d.source, "fpga-lsp:td");
    EXPECT_EQ(d.data["backend"], "read_hdl");
    EXPECT_EQ(d.code, "HDL-1007");
}

TEST(TdMapMessage, CustomBackend) {
    const auto prj = MakeProject();
    const auto d = MapMessage(MakeMsg("ADC-2000", 1, "m"), prj, "read_adc");
    EXPECT_EQ(d.data["backend"], "read_adc");
}

// -----------------------------------------------------------------------------
// LSP-PATH-SPACE
// -----------------------------------------------------------------------------

TEST(TdPathSpace, DiagnosticForHdlFileWithSpace) {
    auto prj = MakeProject("Y:/proj");
    AddFile(prj, "src/has space.v");

    const auto diags = MakePathSpaceDiagnostics(prj);
    ASSERT_EQ(diags.size(), 1u);
    EXPECT_EQ(diags[0].code, "LSP-PATH-SPACE");
    EXPECT_EQ(diags[0].severity, lsp::kWarning);
    EXPECT_EQ(diags[0].source, "fpga-lsp:td");
    EXPECT_EQ(diags[0].range.start.line, 0);
    EXPECT_EQ(diags[0].data["path"], "Y:/proj/demo.al");
    // 消息里用的是 rel（相对路径）
    EXPECT_NE(diags[0].message.find("src/has space.v"), std::string::npos);
}

TEST(TdPathSpace, NoDiagnosticWhenNoSpaces) {
    auto prj = MakeProject("Y:/proj");
    AddFile(prj, "src/a.v");
    EXPECT_TRUE(MakePathSpaceDiagnostics(prj).empty());
}

TEST(TdPathSpace, OnlyHdlFilesConsidered) {
    auto prj = MakeProject("Y:/proj");
    AddFile(prj, "src/a.v");
    AddFile(prj, "constrains/pin space.adc", project::FileKind::Adc);
    // ADC 文件不在 hdl_files() 里，不应触发
    EXPECT_TRUE(MakePathSpaceDiagnostics(prj).empty());
}

TEST(TdPathSpace, SkippedFilesListed) {
    auto prj = MakeProject("Y:/proj");
    AddFile(prj, "a.v");
    AddFile(prj, "b c.v");
    AddFile(prj, "d e.v");
    EXPECT_EQ(SkippedSpaceHdlFiles(prj).size(), 2u);
}

// -----------------------------------------------------------------------------
// Tcl 命令行
// -----------------------------------------------------------------------------

TEST(TdFileList, SkipsPathsWithSpaces) {
    const std::vector<std::string> files = {"Y:/a.v", "Y:/b c.v", "Y:/d.v"};
    EXPECT_EQ(FileListTcl(files), "Y:/a.v Y:/d.v");
}

TEST(TdFileList, ConvertsBackslashes) {
    EXPECT_EQ(FileListTcl({"C:\\a\\b.v"}), "C:/a/b.v");
}

TEST(TdReadHdlCommand, Shape) {
    EXPECT_EQ(MakeReadHdlCommand({"Y:/a.v", "Y:/b.v"}, "top"),
              "read_hdl -file {Y:/a.v Y:/b.v} -top top");
}

TEST(TdReadHdlCommand, EmptyTopDefaultsToTop) {
    EXPECT_EQ(MakeReadHdlCommand({"Y:/a.v"}, ""), "read_hdl -file {Y:/a.v} -top top");
}

// -----------------------------------------------------------------------------
// 从真实输出文本产出诊断（离线）
// -----------------------------------------------------------------------------

TEST(TdFromOutput, RealFixtureProducesDiagnostics) {
    const std::string path = std::string(FPGA_LSP_SOURCE_DIR) +
                             "/tests/td/fixtures/read_hdl_mb252.txt";
    if (!std::filesystem::exists(path)) {
        GTEST_SKIP() << "fixture missing";
    }
    std::ifstream in(path, std::ios::binary);
    const std::string output{std::istreambuf_iterator<char>(in),
                             std::istreambuf_iterator<char>()};

    // 用真实工程的 al 作为 project
    const std::string al = std::string(FPGA_LSP_SOURCE_DIR) +
                           "/tests/gate-corpus/corpus_alpha/demo_project.al";
    if (!std::filesystem::exists(al)) GTEST_SKIP() << "corpus missing";
    const auto prj = project::ParseAl(al);

    const auto diags = DiagnosticsFromReadHdlOutput(output, prj);

    // 夹具（真实 TD 输出，20 个 HDL 文件）产出 22 条：
    //   20 x HDL-1007 elaborate（Info）
    //    1 x HDL-7144 CRITICAL-WARNING（Warning）
    //    1 x HDL-8007 "pll_gen is a black box"（ERROR，但因含 "black box" 升级为 Warning）
    // 另有 0 条 LSP-PATH-SPACE（该工程无含空格路径）
    ASSERT_EQ(diags.size(), 22u);

    // 第一条：HDL-1007 elaborate module top，TD 第 15 行 -> LSP 第 14 行
    EXPECT_EQ(diags[0].code, "HDL-1007");
    EXPECT_EQ(diags[0].severity, lsp::kInfo);
    EXPECT_EQ(diags[0].range.start.line, 14);
    EXPECT_EQ(diags[0].message, "elaborate module top");

    // 第二条：HDL-7144 -> warning
    EXPECT_EQ(diags[1].code, "HDL-7144");
    EXPECT_EQ(diags[1].severity, lsp::kWarning);
    EXPECT_EQ(diags[1].range.start.line, 65);

    // 所有诊断 character 恒为 0
    for (const auto& d : diags) {
        EXPECT_EQ(d.range.start.character, 0);
        EXPECT_EQ(d.range.end.character, 1);
        EXPECT_EQ(d.source, "fpga-lsp:td");
    }

    // warning 恰好 2 条：HDL-7144 + HDL-8007(black box 升级)
    // ⚠ 关键：20 条 HDL-1007 全部是 Info，没有一条被误判成 warning
    int warnings = 0;
    int infos = 0;
    for (const auto& d : diags) {
        if (d.severity == lsp::kWarning) ++warnings;
        if (d.severity == lsp::kInfo) ++infos;
    }
    EXPECT_EQ(warnings, 2) << "应为 HDL-7144 + HDL-8007(black box)";
    EXPECT_EQ(infos, 20) << "20 条 elaborate 都应是 Info";
}

TEST(TdFromOutput, BlackBoxMessageUpgradedToWarning) {
    // 真实输出末尾那条：HDL-8007 ERROR: ... is a black box
    // Python 会把它从 ERROR(1) 升级为 Warning(2) 并追加说明
    const std::string path = std::string(FPGA_LSP_SOURCE_DIR) +
                             "/tests/td/fixtures/read_hdl_mb252.txt";
    if (!std::filesystem::exists(path)) GTEST_SKIP() << "fixture missing";
    std::ifstream in(path, std::ios::binary);
    const std::string output{std::istreambuf_iterator<char>(in),
                             std::istreambuf_iterator<char>()};

    const auto prj = MakeProject("Y:/proj");
    const auto diags = DiagnosticsFromReadHdlOutput(output, prj);

    const lsp::Diagnostic* black_box = nullptr;
    for (const auto& d : diags) {
        if (d.code == "HDL-8007" && d.message.find("black box") != std::string::npos) {
            black_box = &d;
        }
    }
    ASSERT_NE(black_box, nullptr) << "夹具里应有 black box 消息";
    EXPECT_EQ(black_box->severity, lsp::kWarning) << "black box 应升级为 warning";
    EXPECT_NE(black_box->message.find("(IP/module not in the build"),
              std::string::npos);
    // TD 第 66 行 -> LSP 第 65 行
    EXPECT_EQ(black_box->range.start.line, 65);
}

TEST(TdFromOutput, EmptyOutputGivesNoDiagnostics) {
    const auto prj = MakeProject();
    EXPECT_TRUE(DiagnosticsFromReadHdlOutput("", prj).empty());
}

TEST(TdFromOutput, NeverThrowsOnGarbage) {
    const auto prj = MakeProject();
    for (const char* junk : {"garbage", "\x00\x01", "HDL-", "| | |"}) {
        EXPECT_NO_THROW(DiagnosticsFromReadHdlOutput(junk, prj)) << junk;
    }
}

}  // namespace