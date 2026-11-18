// =============================================================================
//  tests/unit/path_test.cpp
// =============================================================================

#include <gtest/gtest.h>

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

}  // namespace