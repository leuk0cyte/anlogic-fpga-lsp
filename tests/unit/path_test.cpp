// =============================================================================
//  tests/unit/path_test.cpp
// =============================================================================

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "platform/command_line.h"
#include "platform/path.h"

namespace {

using namespace fpgalsp::platform;

// -----------------------------------------------------------------------------
// normalize_path（复现 project.py 的 norm()）
// -----------------------------------------------------------------------------

TEST(PathNormalize, BackslashesBecomeForwardSlashes) {
    const std::string p = normalize_path("Y:\\proj\\sub\\a.sv");
    EXPECT_EQ(p, "Y:/proj/sub/a.sv");
    EXPECT_EQ(p.find('\\'), std::string::npos);
}

TEST(PathNormalize, DriveLetterUppercased) {
    EXPECT_EQ(normalize_path("y:/proj/a.sv"), "Y:/proj/a.sv");
    EXPECT_EQ(normalize_path("y:\\proj\\a.sv"), "Y:/proj/a.sv");
}

TEST(PathNormalize, TrailingSeparatorRemoved) {
    EXPECT_EQ(normalize_path("Y:/proj/"), "Y:/proj");
    EXPECT_EQ(normalize_path("Y:\\proj\\"), "Y:/proj");
    // 盘符根保留
    EXPECT_EQ(normalize_path("Y:/"), "Y:/");
}

TEST(PathNormalize, DotAndDotDotCollapsed) {
    EXPECT_EQ(normalize_path("Y:/proj/./sub/../a.sv"), "Y:/proj/a.sv");
}

TEST(PathNormalize, DuplicateSeparatorsCollapsed) {
    EXPECT_EQ(normalize_path("Y://proj///a.sv"), "Y:/proj/a.sv");
}

TEST(PathNormalize, EmptyStaysEmpty) {
    // 对应 Python: if not path: return path
    EXPECT_EQ(normalize_path(""), "");
}

TEST(PathNormalize, RelativePathBecomesAbsolute) {
    const std::string p = normalize_path("a.sv");
    EXPECT_TRUE(is_absolute_path(p));
    EXPECT_EQ(p.find("a.sv"), p.size() - 4);
}

// -----------------------------------------------------------------------------
// 大小写
// -----------------------------------------------------------------------------

TEST(PathCase, EqualsIgnoreCase) {
    EXPECT_TRUE(path_equals_ignore_case("Y:/Proj/A.sv", "y:/proj/a.sv"));
    EXPECT_FALSE(path_equals_ignore_case("Y:/Proj/A.sv", "Y:/Proj/B.sv"));
    EXPECT_FALSE(path_equals_ignore_case("Y:/a", "Y:/ab"));
}

TEST(PathCase, KeyIsLowercase) {
    EXPECT_EQ(path_key("Y:/Proj/A.SV"), "y:/proj/a.sv");
}

TEST(PathCase, OriginalCasePreservedAfterNormalize) {
    // 大小写不敏感只用于比较，显示用原始大小写
    EXPECT_EQ(normalize_path("Y:/MixedCase/Dir/File.sv"), "Y:/MixedCase/Dir/File.sv");
}

TEST(PathCase, LessIgnoreCaseIsOrdering) {
    EXPECT_TRUE(path_less_ignore_case("Y:/a", "Y:/b"));
    EXPECT_FALSE(path_less_ignore_case("Y:/B", "Y:/a"));
}

// -----------------------------------------------------------------------------
// 组成
// -----------------------------------------------------------------------------

TEST(PathParts, Absolute) {
    EXPECT_TRUE(is_absolute_path("Y:/a"));
    EXPECT_TRUE(is_absolute_path("\\\\server\\share"));
    EXPECT_FALSE(is_absolute_path("a/b"));
    EXPECT_FALSE(is_absolute_path(""));
}

TEST(PathParts, DirnameFilenameExtension) {
    EXPECT_EQ(path_dirname("Y:/proj/a.sv"), "Y:/proj");
    EXPECT_EQ(path_filename("Y:/proj/a.sv"), "a.sv");
    EXPECT_EQ(path_extension("Y:/proj/a.sv"), ".sv");
    EXPECT_EQ(path_extension("Y:/proj/a"), "");
}

TEST(PathParts, Join) {
    EXPECT_EQ(path_join("Y:/proj", "sub/a.sv"), "Y:/proj/sub/a.sv");
    // 绝对路径的第二个参数直接生效
    EXPECT_EQ(path_join("Y:/proj", "Z:/other/b.sv"), "Z:/other/b.sv");
}

// -----------------------------------------------------------------------------
// URI 往返
// -----------------------------------------------------------------------------

TEST(PathUri, BasicDrivePath) {
    const std::string uri = path_to_uri("Y:/proj/a.sv");
    EXPECT_EQ(uri, "file:///Y:/proj/a.sv");
}

TEST(PathUri, SpaceIsPercentEncoded) {
    const std::string uri = path_to_uri("Y:/proj/a b.sv");
    EXPECT_EQ(uri, "file:///Y:/proj/a%20b.sv");
    EXPECT_EQ(uri_to_path(uri), "Y:/proj/a b.sv");
}

TEST(PathUri, RoundTripPreservesPath) {
    for (const char* p : {"Y:/proj/a.sv", "Y:/proj/sub dir/x y.sv", "Y:/a/b/c.sv"}) {
        const std::string uri = path_to_uri(p);
        EXPECT_EQ(uri_to_path(uri), std::string(p)) << "uri=" << uri;
    }
}

TEST(PathUri, RoundTripWithNonAscii) {
    const std::string path = "Y:/proj/\xE4\xB8\xAD\xE6\x96\x87.sv";  // 中文.sv
    const std::string uri = path_to_uri(path);
    EXPECT_EQ(uri_to_path(uri), path);
}

TEST(PathUri, NonFileSchemeRejected) {
    EXPECT_EQ(uri_to_path("http://example.com/a.sv"), "");
    EXPECT_FALSE(uri_is_file("http://x"));
    EXPECT_TRUE(uri_is_file("file:///Y:/a.sv"));
    EXPECT_TRUE(uri_is_file("FILE:///Y:/a.sv"));
}

TEST(PathUri, MalformedUriDoesNotThrow) {
    EXPECT_NO_THROW({
        EXPECT_EQ(uri_to_path("file:"), "");
        EXPECT_EQ(uri_to_path(""), "");
        // 畸形的百分号转义（%zz）原样保留，不整体丢弃、不抛
        EXPECT_EQ(uri_to_path("file:///%zz"), "/%zz");
    });
}

// =============================================================================
//  B7 / S-02：Windows 命令行引用（platform/command_line.h）
// =============================================================================
//  回归目标：CreateProcessW 必须拿到"加过引号"的 argv[0]。
//  修复前 td_session/programmer 把 exe 原样塞进 lpCommandLine 且
//  lpApplicationName=nullptr，Windows 会按空白切分去猜 exe ->
//  "C:\Program Files\..." 变成 "C:\Program.exe"（CWE-428）。
// =============================================================================

TEST(PlatformCommandLine, QuotesPathsWithSpaces) {
    EXPECT_EQ(QuoteWindowsArgument(L"C:\\Program Files\\Anlogic\\td.exe"),
              L"\"C:\\Program Files\\Anlogic\\td.exe\"");
}

TEST(PlatformCommandLine, LeavesSimpleArgumentsBare) {
    EXPECT_EQ(QuoteWindowsArgument(L"C:\\td\\td.exe"), L"C:\\td\\td.exe");
    EXPECT_EQ(QuoteWindowsArgument(L"prog_sram"), L"prog_sram");
}

TEST(PlatformCommandLine, EmptyArgumentBecomesEmptyQuotes) {
    EXPECT_EQ(QuoteWindowsArgument(L""), L"\"\"");
}

TEST(PlatformCommandLine, DoublesBackslashesBeforeClosingQuote) {
    // 末尾反斜杠必须翻倍，否则结尾的引号会被当成转义
    EXPECT_EQ(QuoteWindowsArgument(L"C:\\dir with space\\"), L"\"C:\\dir with space\\\\\"");
}

TEST(PlatformCommandLine, EscapesEmbeddedQuotePerCrtRules) {
    // a"b -> "a\"b"
    EXPECT_EQ(QuoteWindowsArgument(L"a\"b"), L"\"a\\\"b\"");
    // a\"b（反斜杠 + 引号）-> "a\\\"b"
    EXPECT_EQ(QuoteWindowsArgument(L"a\\\"b"), L"\"a\\\\\\\"b\"");
}

TEST(PlatformCommandLine, BuildsExeAndArguments) {
    const std::wstring line = BuildWindowsCommandLine(L"C:\\Program Files\\TD\\td.exe",
                                                      {L"-x", L"a b"});
    EXPECT_EQ(line, L"\"C:\\Program Files\\TD\\td.exe\" -x \"a b\"");
    // argv[0] 一定以引号开头，这是 S-02 的核心断言
    EXPECT_EQ(line.front(), L'"');
}

// =============================================================================
//  B7 / S-04：物理包含判定（canonical_path / path_is_within_canonical）
// =============================================================================
//  normalize_path 是纯词法的，工程目录里一个指向外部的 junction 会通过纯词法
//  包含判定（实测能读出 C:\Windows\...\hosts）。这里用一个真实 junction 回归。
// =============================================================================

TEST(PlatformPathCanonical, ResolvesDotDotAndTrailingSeparator) {
    const std::string p = normalize_path("Y:\\proj\\sub\\..\\a.sv");
    EXPECT_EQ(p, "Y:/proj/a.sv");
    EXPECT_EQ(canonical_path(p), canonical_path("Y:/proj/a.sv"));
}

TEST(PlatformPathCanonical, LexicalContainmentIsNotPhysical) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec) / "fpgalsp-b7-canon";
    if (ec) {
        GTEST_SKIP() << "no temp dir";
    }
    fs::remove_all(base, ec);
    const fs::path proj = base / "proj";
    const fs::path outside = base / "outside";
    fs::create_directories(proj, ec);
    fs::create_directories(outside, ec);
    {
        std::ofstream(outside / "secret.txt") << "OUTSIDE\n";
    }
    const fs::path link = proj / "jlink";
    // mklink /J 不需要管理员权限（符号链接需要，所以这里用 junction）
    const std::string cmd = "cmd /c mklink /J \"" + link.string() + "\" \"" + outside.string() +
                            "\" >nul 2>&1";
    const int rc = std::system(cmd.c_str());
    if (rc != 0 || !fs::exists(link / "secret.txt", ec)) {
        fs::remove_all(base, ec);
        GTEST_SKIP() << "cannot create a directory junction here";
    }

    const std::string target = (link / "secret.txt").string();
    const std::string root = proj.string();

    // 1) 词法判定：通过（这就是 S-04 指出的缺口）
    const std::string lexical = normalize_path(target);
    EXPECT_EQ(lexical.rfind(normalize_path(root), 0), 0u)
        << "lexical containment is expected to be fooled; got " << lexical;

    // 2) 物理判定：被拒（canonical 展开 junction 后落在 root 之外）
    EXPECT_FALSE(path_is_within_canonical(target, root));

    // 3) 工程内真实文件仍然通过
    {
        std::ofstream(proj / "real.txt") << "INSIDE\n";
    }
    EXPECT_TRUE(path_is_within_canonical((proj / "real.txt").string(), root));

    fs::remove_all(base, ec);
}

}  // namespace
// -----------------------------------------------------------------------------
// state_dir（Wave 3 / W1）：状态目录必须在系统临时目录里，**绝不能**落回工程目录。
// 2026-11-22 审计的真实现象：每次 LSP 保存触发 TD 检查，工程根就重新长出
// `.fpgalsp/run` —— 根因是 td_service 里一处硬编码 `<project>/.fpgalsp/run`。
// 这个用例钉住"默认路径不含 .fpgalsp、且不落在工程目录下"。
// -----------------------------------------------------------------------------
TEST(PathStateDir, NeverReturnsProjectLocalFpgalsp) {
    const std::string project = "Y:/_fpgalsp_state_dir_unit_test_project";
    const std::string dir = state_dir(project);
    ASSERT_FALSE(dir.empty());
    // 1) 不带 `.fpgalsp` 这个名字（临时目录布局是 <tmp>/fpgalsp/<name>-<hash8>）
    EXPECT_EQ(dir.find(".fpgalsp"), std::string::npos) << dir;
    // 2) 不在工程目录之下
    EXPECT_EQ(dir.find(project), std::string::npos) << dir;
    // 3) 仍然是按工程唯一化的目录（name-hash8）
    EXPECT_NE(dir.find("fpgalsp"), std::string::npos) << dir;
}

TEST(PathStateDir, HonorsFpgalspStateDirEnv) {
#ifdef _WIN32
    const char* previous = std::getenv("FPGALSP_STATE_DIR");
    const std::string saved = previous == nullptr ? std::string() : std::string(previous);
    ASSERT_EQ(_putenv_s("FPGALSP_STATE_DIR", "Y:/_fpgalsp_state_dir_env_test"), 0);
    EXPECT_EQ(state_dir("Y:/some/project"), "Y:/_fpgalsp_state_dir_env_test");
    ASSERT_EQ(_putenv_s("FPGALSP_STATE_DIR", saved.c_str()), 0);
#else
    GTEST_SKIP() << "setenv-based override is Windows-only in this test";
#endif
}
