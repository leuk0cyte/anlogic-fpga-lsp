// =============================================================================
//  tests/unit/utf16_test.cpp
// =============================================================================
//  重点：utf16_column_to_byte_index 的 clamp 语义必须与
//        %PYREF%\fpgalsp\lsp.py :: utf16_to_py 逐字一致。
//
//  Python 参考实现：
//      def utf16_to_py(text, col):
//          if col <= 0: return 0
//          used = 0
//          for i, ch in enumerate(text):
//              w = 2 if ord(ch) > 0xFFFF else 1
//              if used + w > col: return i      # 落在代理对中间
//              used += w
//              if used == col:    return i + 1
//          return len(text)
// =============================================================================

#include <gtest/gtest.h>

#include "platform/utf16.h"

namespace {

using namespace fpgalsp::platform;

// 常用测试串
constexpr const char* kAscii = "hello";
constexpr const char* kChinese = "\xE4\xB8\xAD\xE6\x96\x87";  // 中文（2 个 BMP 字符）
constexpr const char* kEmoji = "\xF0\x9F\x98\x80";            // 😀 1 个非 BMP 字符

// -----------------------------------------------------------------------------
// utf16_len
// -----------------------------------------------------------------------------

TEST(Utf16Len, EmptyString) {
    EXPECT_EQ(utf16_len(""), 0u);
}

TEST(Utf16Len, PureAscii) {
    EXPECT_EQ(utf16_len("hello"), 5u);  // 5 码元
}

TEST(Utf16Len, ChineseBmpCharsOneCodeUnitEach) {
    // 中文在 BMP 内，每个字符 1 个 UTF-16 码元，但 UTF-8 占 3 字节
    EXPECT_EQ(std::string(kChinese).size(), 6u);
    EXPECT_EQ(utf16_len(kChinese), 2u);
}

TEST(Utf16Len, EmojiIsSurrogatePair) {
    // emoji 是非 BMP，UTF-8 4 字节，UTF-16 2 码元
    EXPECT_EQ(std::string(kEmoji).size(), 4u);
    EXPECT_EQ(utf16_len(kEmoji), 2u);
}

TEST(Utf16Len, MixedChineseEmoji) {
    const std::string mixed = std::string(kChinese) + kEmoji;  // 中 + 文 + 😀
    EXPECT_EQ(utf16_len(mixed), 4u);  // 1 + 1 + 2
}

TEST(Utf16Len, SingleCodePointEmojiOneChar) {
    EXPECT_EQ(utf16_len("\xF0\x9F\x91\x8D"), 2u);  // 👍
}

// -----------------------------------------------------------------------------
// utf16_column_to_byte_index —— clamp 语义（核心）
// -----------------------------------------------------------------------------

TEST(Utf16ColumnToByteIndex, ColumnZero) {
    EXPECT_EQ(utf16_column_to_byte_index(kAscii, 0), 0u);
    EXPECT_EQ(utf16_column_to_byte_index(kChinese, 0), 0u);
    EXPECT_EQ(utf16_column_to_byte_index(kEmoji, 0), 0u);
}

TEST(Utf16ColumnToByteIndex, AsciiExact) {
    // "hello": 第 3 列 -> 字节 3
    EXPECT_EQ(utf16_column_to_byte_index(kAscii, 3), 3u);
    EXPECT_EQ(utf16_column_to_byte_index(kAscii, 5), 5u);
}

TEST(Utf16ColumnToByteIndex, ChineseColumnsAreCodeUnitsNotBytes) {
    // "中文"：UTF-8 6 字节，UTF-16 2 码元
    // 第 1 列 -> 字节 3（第一个中文之后）
    // 第 2 列 -> 字节 6（末尾）
    EXPECT_EQ(utf16_column_to_byte_index(kChinese, 1), 3u);
    EXPECT_EQ(utf16_column_to_byte_index(kChinese, 2), 6u);
}

TEST(Utf16ColumnToByteIndex, EmojiExactBoundaries) {
    // "😀"：UTF-16 2 码元，UTF-8 4 字节
    // 列 0 -> 0，列 2 -> 4（末尾）
    EXPECT_EQ(utf16_column_to_byte_index(kEmoji, 0), 0u);
    EXPECT_EQ(utf16_column_to_byte_index(kEmoji, 2), 4u);
}

TEST(Utf16ColumnToByteIndex, EmojiMidSurrogatePairReturnsPreviousBoundary) {
    // ⚠ 最关键的一条：列号落在代理对中间（列 1）时，
    // Python 的 `if used + w > col: return i` 返回**该字符的起始索引**，
    // 不是四舍五入到下一个字符。
    //
    // Python: utf16_to_py("😀", 1) == 0
    EXPECT_EQ(utf16_column_to_byte_index(kEmoji, 1), 0u);
}

TEST(Utf16ColumnToByteIndex, MixedMidSurrogatePairClampsBack) {
    // "中文😀"：UTF-16 列分布 = [0,1) 中, [1,2) 文, [2,4) 😀
    // 列 3 落在 emoji 的代理对中间 -> 回到 emoji 起始处。
    //
    // 已用 Python 参考实现逐值核对（utf16_to_py -> 码点索引 -> UTF-8 字节偏移）：
    //     col 0 -> cp 0 -> byte 0
    //     col 1 -> cp 1 -> byte 3
    //     col 2 -> cp 2 -> byte 6
    //     col 3 -> cp 2 -> byte 6   (= emoji 起始，代理对中间钳回)
    //     col 4 -> cp 3 -> byte 10  (末尾)
    const std::string mixed = std::string(kChinese) + kEmoji;
    EXPECT_EQ(utf16_column_to_byte_index(mixed, 0), 0u);
    EXPECT_EQ(utf16_column_to_byte_index(mixed, 1), 3u);
    EXPECT_EQ(utf16_column_to_byte_index(mixed, 2), 6u);   // emoji 起始
    EXPECT_EQ(utf16_column_to_byte_index(mixed, 3), 6u);   // 代理对中间 -> 仍是 emoji 起始
    EXPECT_EQ(utf16_column_to_byte_index(mixed, 4), 10u);  // 末尾
    // 列超过文本长度 -> clamp 到末尾（Python: return len(text)）
    EXPECT_EQ(utf16_column_to_byte_index(mixed, 5), 10u);
}

TEST(Utf16ColumnToByteIndex, ColumnBeyondEndClampsToSize) {
    EXPECT_EQ(utf16_column_to_byte_index(kAscii, 999), 5u);
    EXPECT_EQ(utf16_column_to_byte_index(kChinese, 999), 6u);
    EXPECT_EQ(utf16_column_to_byte_index(kEmoji, 999), 4u);
}

TEST(Utf16ColumnToByteIndex, NegativeLikeColumnTreatedAsZero) {
    // 无符号参数下 0 已在上面覆盖；这里确认「极大值」不会崩
    EXPECT_EQ(utf16_column_to_byte_index("", 0), 0u);
    EXPECT_EQ(utf16_column_to_byte_index("", 5), 0u);
}

// -----------------------------------------------------------------------------
// 反向换算
// -----------------------------------------------------------------------------

TEST(ByteIndexToUtf16Column, AsciiAndChinese) {
    EXPECT_EQ(byte_index_to_utf16_column(kAscii, 3), 3u);
    EXPECT_EQ(byte_index_to_utf16_column(kChinese, 3), 1u);
    EXPECT_EQ(byte_index_to_utf16_column(kChinese, 6), 2u);
}

TEST(ByteIndexToUtf16Column, MidCharacterRoundsDown) {
    // 字节 1 落在第一个中文字符（3 字节）中间 -> 向下取整到 0 码元
    EXPECT_EQ(byte_index_to_utf16_column(kChinese, 1), 0u);
    EXPECT_EQ(byte_index_to_utf16_column(kChinese, 2), 0u);
}

TEST(ByteIndexToUtf16Column, Emoji) {
    EXPECT_EQ(byte_index_to_utf16_column(kEmoji, 0), 0u);
    EXPECT_EQ(byte_index_to_utf16_column(kEmoji, 2), 0u);  // 字符中间
    EXPECT_EQ(byte_index_to_utf16_column(kEmoji, 4), 2u);
}

TEST(ByteIndexToUtf16Column, ClampsBeyondSize) {
    EXPECT_EQ(byte_index_to_utf16_column(kEmoji, 999), 2u);
}

// -----------------------------------------------------------------------------
// 非法 UTF-8（never-raise 契约）
// -----------------------------------------------------------------------------

TEST(Utf16Invalid, InvalidByteDoesNotThrow) {
    // 0xFF 是非法起始字节
    const std::string bad = "a\xFF\xFFz";
    EXPECT_NO_THROW({
        const auto n = utf16_len(bad);
        EXPECT_EQ(n, 4u);  // a + FFFD + FFFD + z
    });
}

TEST(Utf16Invalid, TruncatedSequenceDoesNotThrow) {
    // 只有 2 字节的 3 字节序列前缀
    const std::string bad = "\xE4\xB8";
    EXPECT_NO_THROW({
        const auto n = utf16_len(bad);
        EXPECT_EQ(n, 2u);  // 两个 U+FFFD
    });
}

TEST(Utf16Invalid, InvalidUtf8ColumnConversionDoesNotThrow) {
    const std::string bad = "a\xFFz";
    EXPECT_NO_THROW({
        const auto c = utf16_column_to_byte_index(bad, 2);
        EXPECT_LE(c, bad.size());
    });
}

TEST(Utf16Invalid, SurrogateEncodedInUtf8IsRejected) {
    // UTF-8 里编码代理项（ED A0 80）是非法的
    const std::string bad = "\xED\xA0\x80";
    EXPECT_FALSE(is_valid_utf8(bad));
    EXPECT_EQ(utf16_len(bad), 3u);  // 3 个替换字符
}

TEST(Utf16Valid, ValidStringsReportedValid) {
    EXPECT_TRUE(is_valid_utf8(""));
    EXPECT_TRUE(is_valid_utf8(kAscii));
    EXPECT_TRUE(is_valid_utf8(kChinese));
    EXPECT_TRUE(is_valid_utf8(kEmoji));
}

// -----------------------------------------------------------------------------
// CRLF 与制表符
// -----------------------------------------------------------------------------

TEST(Utf16Lines, CrLfLinesKeepCarriageReturn) {
    // line_at 只按 '\n' 切分，行尾的 '\r' 保留（与 Python split("\n") 一致）
    const std::string text = "abc\r\ndef\r\n";
    EXPECT_EQ(line_at(text, 0), "abc\r");
    EXPECT_EQ(line_at(text, 1), "def\r");
    EXPECT_EQ(line_at(text, 2), "");
}

TEST(Utf16Lines, TabCountsAsOneCodeUnit) {
    EXPECT_EQ(utf16_len("\tx"), 2u);
    const std::string text = "\tabc";
    EXPECT_EQ(utf16_column_to_byte_index(text, 1), 1u);
}

TEST(Utf16Lines, LineCount) {
    EXPECT_EQ(line_count(""), 1u);
    EXPECT_EQ(line_count("abc"), 1u);
    EXPECT_EQ(line_count("a\nb"), 2u);
    EXPECT_EQ(line_count("a\nb\n"), 3u);
}

TEST(Utf16Lines, LineAtOutOfRange) {
    EXPECT_EQ(line_at("a\nb", 5), "");
}

TEST(Utf16Lines, PositionToByteIndexAcrossLines) {
    const std::string text = "abc\ndef\nghi";
    // 第 1 行第 2 列 -> 字节 4 + 2 = 6
    EXPECT_EQ(position_to_byte_index(text, 1, 2), 6u);
    // 第 2 行第 0 列 -> 字节 8
    EXPECT_EQ(position_to_byte_index(text, 2, 0), 8u);
}

TEST(Utf16Lines, PositionToByteIndexClampsOutOfRange) {
    const std::string text = "abc";
    EXPECT_EQ(position_to_byte_index(text, 99, 0), 3u);
    EXPECT_EQ(position_to_byte_index(text, 0, 99), 3u);
}

// -----------------------------------------------------------------------------
// 长字符串走 simdutf 路径（>= 64 字节）也要与短串一致
// -----------------------------------------------------------------------------

TEST(Utf16Long, LongMixedStringConsistentWithHandScan) {
    std::string long_text;
    for (int i = 0; i < 30; ++i) {
        long_text += kChinese;
        long_text += kEmoji;
    }
    ASSERT_GT(long_text.size(), 64u);

    // 30 * (2 BMP + 2 surrogate) = 120 码元
    EXPECT_EQ(utf16_len(long_text), 120u);

    // 往返一致性：UTF-8 -> UTF-16 -> UTF-8
    const auto u16 = utf8_to_utf16(long_text);
    EXPECT_EQ(u16.size(), 120u);
    EXPECT_EQ(utf16_to_utf8(u16), long_text);
}

TEST(Utf16RoundTrip, ShortStrings) {
    for (const char* s : {kAscii, kChinese, kEmoji}) {
        EXPECT_EQ(utf16_to_utf8(utf8_to_utf16(s)), std::string(s));
    }
}

TEST(Utf16RoundTrip, MixedString) {
    const std::string mixed = std::string(kChinese) + kEmoji + kAscii;
    EXPECT_EQ(utf16_to_utf8(utf8_to_utf16(mixed)), mixed);
}

// -----------------------------------------------------------------------------
// 码点索引版本（直接对应 Python 的 py_to_utf16）
// -----------------------------------------------------------------------------

TEST(Utf16CodepointIndex, MatchesPythonSemantics) {
    // py_to_utf16("😀", 1) == 2  （1 个码点 -> 2 码元）
    EXPECT_EQ(codepoint_index_to_utf16_column(kEmoji, 1), 2u);
    // py_to_utf16("中文", 1) == 1
    EXPECT_EQ(codepoint_index_to_utf16_column(kChinese, 1), 1u);
    // py_to_utf16("中文😀", 3) == 4
    const std::string mixed = std::string(kChinese) + kEmoji;
    EXPECT_EQ(codepoint_index_to_utf16_column(mixed, 3), 4u);
}

}  // namespace