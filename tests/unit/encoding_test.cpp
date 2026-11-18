// =============================================================================
//  tests/unit/encoding_test.cpp —— 文件读写与 UTF-8 清洗
// =============================================================================

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "platform/encoding.h"
#include "platform/utf16.h"

namespace {

using namespace fpgalsp::platform;

class EncodingFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() / "fpgalsp_encoding_test";
        std::filesystem::create_directories(dir_);
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    std::string Path(const std::string& name) const {
        return (dir_ / name).string();
    }

    std::filesystem::path dir_;
};

TEST_F(EncodingFileTest, WriteThenReadRoundTrip) {
    const std::string file = Path("a.sv");
    const std::string content = "module m;\n  // \xE4\xB8\xAD\xE6\x96\x87\nendmodule\n";

    ASSERT_TRUE(write_file_utf8(file, content));
    auto r = read_file_utf8(file);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.content, content);
    EXPECT_FALSE(r.had_bom);
    EXPECT_FALSE(r.had_invalid_bytes);
}

TEST_F(EncodingFileTest, WriteProducesNoBom) {
    const std::string file = Path("nobom.sv");
    ASSERT_TRUE(write_file_utf8(file, "abc"));

    std::ifstream in(file, std::ios::binary);
    char buf[3] = {};
    in.read(buf, 3);
    // 不能是 EF BB BF
    EXPECT_FALSE(buf[0] == '\xEF' && buf[1] == '\xBB' && buf[2] == '\xBF');
}

TEST_F(EncodingFileTest, ReadStripsBom) {
    const std::string file = Path("bom.sv");
    {
        std::ofstream out(file, std::ios::binary);
        out << "\xEF\xBB\xBF" << "module m;\n";
    }
    auto r = read_file_utf8(file);
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.had_bom);
    // BOM 被剥掉，第一行不残留 U+FEFF
    EXPECT_EQ(r.content, "module m;\n");
}

TEST_F(EncodingFileTest, ReadInvalidBytesReplacedNotThrown) {
    const std::string file = Path("bad.sv");
    {
        std::ofstream out(file, std::ios::binary);
        out << "a\xFF" << "z";
    }
    ReadFileResult r;
    EXPECT_NO_THROW(r = read_file_utf8(file));
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.had_invalid_bytes);
    // 非法字节被替换成合法 UTF-8（U+FFFD = EF BF BD）
    EXPECT_EQ(r.content, "a\xEF\xBF\xBDz");
}

TEST_F(EncodingFileTest, ReadMissingFileReturnsNotOk) {
    auto r = read_file_utf8(Path("does-not-exist.sv"));
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.content.empty());
}

TEST_F(EncodingFileTest, EmptyPathHandled) {
    auto r = read_file_utf8("");
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(write_file_utf8("", "x"));
}

TEST_F(EncodingFileTest, TryReadReturnsNulloptOnFailure) {
    EXPECT_FALSE(try_read_file_utf8(Path("nope.sv")).has_value());
}

TEST_F(EncodingFileTest, EnsureDirectoryCreatesNested) {
    const std::string nested = Path("a/b/c");
    EXPECT_TRUE(ensure_directory(nested));
    EXPECT_TRUE(std::filesystem::exists(nested));
    // 重复调用不报错
    EXPECT_TRUE(ensure_directory(nested));
}

TEST_F(EncodingFileTest, WriteDoesNotConvertLineEndings) {
    const std::string file = Path("lf.sv");
    ASSERT_TRUE(write_file_utf8(file, "a\nb\n"));

    // 二进制读取确认为 \n 而不是 \r\n
    std::ifstream in(file, std::ios::binary);
    std::string raw{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    EXPECT_EQ(raw, "a\nb\n");
    EXPECT_EQ(raw.find('\r'), std::string::npos);
}

// -----------------------------------------------------------------------------
// 纯函数
// -----------------------------------------------------------------------------

TEST(EncodingPure, BomDetection) {
    EXPECT_TRUE(has_utf8_bom("\xEF\xBB\xBFx"));
    EXPECT_FALSE(has_utf8_bom("x"));
    EXPECT_FALSE(has_utf8_bom(""));
    EXPECT_EQ(strip_utf8_bom("\xEF\xBB\xBFx"), "x");
    EXPECT_EQ(strip_utf8_bom("x"), "x");
}

TEST(EncodingPure, SanitizeLeavesValidInputAlone) {
    const std::string sv = "module m;\n  // \xE4\xB8\xAD\nendmodule\n";
    EXPECT_EQ(sanitize_utf8(sv), sv);
}

TEST(EncodingPure, SanitizeReplacesInvalid) {
    const std::string bad = "a\xFFz";
    const std::string fixed = sanitize_utf8(bad);
    EXPECT_TRUE(is_valid_utf8(fixed));
}

}  // namespace