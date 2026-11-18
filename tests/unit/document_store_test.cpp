// =============================================================================
//  tests/unit/document_store_test.cpp
// =============================================================================
//  核心：LSP 增量同步的 UTF-16 语义。
//  最关键的回归是「range 端点落在 emoji 代理对中间」——客户端与服务器理解不一致
//  时，编辑器里删一个中文字符会连带后面的字。
// =============================================================================

#include <gtest/gtest.h>

#include <string>

#include "lsp/document_store.h"

namespace {

using namespace fpgalsp::lsp;

constexpr const char* kEmoji = "\xF0\x9F\x98\x80";               // 😀 2 码元 / 4 字节
constexpr const char* kChinese = "\xE4\xB8\xAD\xE6\x96\x87";      // 中文 2 码元 / 6 字节

// 构造一个 range 变更（列都是 UTF-16）
ContentChange MkChange(std::size_t sl, std::size_t sc, std::size_t el, std::size_t ec,
                       std::string text) {
    ContentChange c;
    c.has_range = true;
    c.start_line = sl;
    c.start_character = sc;
    c.end_line = el;
    c.end_character = ec;
    c.text = std::move(text);
    return c;
}

ContentChange MkFull(std::string text) {
    ContentChange c;
    c.has_range = false;
    c.text = std::move(text);
    return c;
}

// -----------------------------------------------------------------------------
// 全文替换 / 基本替换
// -----------------------------------------------------------------------------

TEST(DocumentStoreApply, FullReplaceWhenNoRange) {
    EXPECT_EQ(DocumentStore::ApplyChange("old text", MkFull("new text")), "new text");
}

TEST(DocumentStoreApply, SingleCharReplaceAscii) {
    // "hello" 把 [1,3) 换成 "XY" -> "hXYlo"
    EXPECT_EQ(DocumentStore::ApplyChange("hello", MkChange(0, 1, 0, 3, "XY")), "hXYlo");
}

TEST(DocumentStoreApply, PureDelete) {
    // [1,4) 删除 -> "ho"
    EXPECT_EQ(DocumentStore::ApplyChange("hello", MkChange(0, 1, 0, 4, "")), "ho");
}

TEST(DocumentStoreApply, PureInsert) {
    // 在 2 处插入（零宽 range）
    EXPECT_EQ(DocumentStore::ApplyChange("hello", MkChange(0, 2, 0, 2, "XY")), "heXYllo");
}

// -----------------------------------------------------------------------------
// UTF-16 列：中文
// -----------------------------------------------------------------------------

TEST(DocumentStoreApply, ChineseColumnsAreCodeUnitsNotBytes) {
    // "中文"：UTF-16 列 [0,1) 中, [1,2) 文；字节 [0,3) 中, [3,6) 文
    // 把第 1 列起（第二个字）删掉 -> 只剩 "中"
    EXPECT_EQ(DocumentStore::ApplyChange(kChinese, MkChange(0, 1, 0, 2, "")), "\xE4\xB8\xAD");
    // 把第一个中文替换成 "X"
    EXPECT_EQ(DocumentStore::ApplyChange(kChinese, MkChange(0, 0, 0, 1, "X")), "X\xE6\x96\x87");
}

// -----------------------------------------------------------------------------
// ⚠ 关键回归：range 落在 emoji 代理对中间
// -----------------------------------------------------------------------------

TEST(DocumentStoreApply, EmojiRangeEndingInsideSurrogatePairClampsBack) {
    // "a😀b"：UTF-16 列 a[0,1) 😀[1,3) b[3,4)
    // range = [1, 2)：end 落在 emoji 的代理对**中间**。
    // utf16_column_to_byte_index(col=2) 与 col=1 同样钳回 emoji 起始（byte 1），
    // 因此这个 range 是零宽的 -> 等于在 emoji 前插入 "X"。
    const std::string input = std::string("a") + kEmoji + "b";
    const std::string out = DocumentStore::ApplyChange(input, MkChange(0, 1, 0, 2, "X"));
    EXPECT_EQ(out, std::string("aX") + kEmoji + "b");
}

TEST(DocumentStoreApply, EmojiFullRangeDeletesWholeChar) {
    // 正确的做法是 range = [1,3) 完整覆盖 emoji
    const std::string input = std::string("a") + kEmoji + "b";
    EXPECT_EQ(DocumentStore::ApplyChange(input, MkChange(0, 1, 0, 3, "")), "ab");
}

TEST(DocumentStoreApply, EmojiDoesNotEatNeighbour) {
    // 删除 emoji 后，后面的 'b' 必须还在（这正是"删一个中文字符会连带后面的字"的场景）
    const std::string input = std::string(kChinese) + kEmoji + "\xE4\xB8\xAD";  // 中文😀中
    // 删掉中间的 emoji：[2,4)
    const std::string out = DocumentStore::ApplyChange(input, MkChange(0, 2, 0, 4, ""));
    EXPECT_EQ(out, std::string(kChinese) + "\xE4\xB8\xAD");
}

TEST(DocumentStoreApply, InsertAtEmojiEndBoundaryGoesAfter) {
    // 列 2 是 emoji 的**结束**边界（emoji 占 [0,2)），Python utf16_to_py("😀", 2)
    // 返回码点索引 1（emoji 之后），所以零宽 range 在列 2 插入 = 插到 emoji 之后。
    const std::string input = kEmoji;
    const std::string out = DocumentStore::ApplyChange(input, MkChange(0, 2, 0, 2, "X"));
    EXPECT_EQ(out, std::string(kEmoji) + "X");

    // 对照：在列 1（代理对中间）插入会被钳回 emoji 起始，即插在 emoji 之前
    const std::string out2 = DocumentStore::ApplyChange(input, MkChange(0, 1, 0, 1, "X"));
    EXPECT_EQ(out2, std::string("X") + kEmoji);
}

// -----------------------------------------------------------------------------
// 跨行
// -----------------------------------------------------------------------------

TEST(DocumentStoreApply, MultiLineReplace) {
    const std::string text = "l0\nl1\nl2\nl3";
    // 从 (1,0) 到 (2,2) 替换 -> "l0\n" + "R" + "3"...  实际 l2 只剩 'l' 之后的 "2"？不对
    // (2,2) 是 "l2" 的末尾，所以删掉 "l1\nl2" 整段
    EXPECT_EQ(DocumentStore::ApplyChange(text, MkChange(1, 0, 2, 2, "R")), "l0\nR\nl3");
}

TEST(DocumentStoreApply, CrossLineWithUtf16Columns) {
    const std::string text = std::string("中\n") + kEmoji;
    // 从 (0,1)（第一个中文之后）到 (1,2)（emoji 之后）替换成 "X"
    EXPECT_EQ(DocumentStore::ApplyChange(text, MkChange(0, 1, 1, 2, "X")),
              std::string("\xE4\xB8\xAD") + "X");
}

// -----------------------------------------------------------------------------
// 越界 clamp（never-throw）
// -----------------------------------------------------------------------------

TEST(DocumentStoreApply, OutOfRangeLineClamps) {
    const std::string text = "abc\ndef";
    EXPECT_NO_THROW({
        const auto out = DocumentStore::ApplyChange(text, MkChange(99, 0, 99, 0, "X"));
        EXPECT_EQ(out, "abc\ndefX");  // 钳到末尾
    });
}

TEST(DocumentStoreApply, OutOfRangeColumnClamps) {
    EXPECT_EQ(DocumentStore::ApplyChange("abc", MkChange(0, 99, 0, 99, "X")), "abcX");
}

TEST(DocumentStoreApply, ReversedRangeDoesNotThrow) {
    // end 在 start 之前：防御性地当作不删除
    std::string out;
    EXPECT_NO_THROW(out = DocumentStore::ApplyChange("abcdef", MkChange(0, 4, 0, 2, "X")));
    EXPECT_EQ(out, "abcdXef");
}

TEST(DocumentStoreApply, EmptyTextAndEmptyRange) {
    EXPECT_EQ(DocumentStore::ApplyChange("", MkChange(0, 0, 0, 0, "X")), "X");
    EXPECT_EQ(DocumentStore::ApplyChange("", MkFull("")), "");
}

// -----------------------------------------------------------------------------
// 多条变更顺序应用
// -----------------------------------------------------------------------------

TEST(DocumentStoreApply, MultipleChangesAppliedInOrder) {
    // LSP：同一批 contentChanges 依次应用，后一个的坐标基于前一个的结果
    std::vector<ContentChange> changes;
    changes.push_back(MkChange(0, 5, 0, 5, " world"));  // "hello" -> "hello world"
    changes.push_back(MkChange(0, 0, 0, 5, "goodbye"));  // -> "goodbye world"
    EXPECT_EQ(DocumentStore::ApplyChanges("hello", changes), "goodbye world");
}

TEST(DocumentStoreApply, SecondChangeSeesResultOfFirst) {
    // 第一步插入中文，第二步按 UTF-16 列删除第一个中文
    std::vector<ContentChange> changes;
    changes.push_back(MkChange(0, 0, 0, 0, kChinese));  // -> "中文abc"
    changes.push_back(MkChange(0, 0, 0, 1, ""));        // -> "文abc"
    EXPECT_EQ(DocumentStore::ApplyChanges("abc", changes), std::string("\xE6\x96\x87") + "abc");
}

TEST(DocumentStoreApply, FullReplaceResetsSubsequent) {
    std::vector<ContentChange> changes;
    changes.push_back(MkFull("base"));
    changes.push_back(MkChange(0, 4, 0, 4, "!"));
    EXPECT_EQ(DocumentStore::ApplyChanges("whatever", changes), "base!");
}

// -----------------------------------------------------------------------------
// DocumentStore 生命周期
// -----------------------------------------------------------------------------

TEST(DocumentStore, OpenAndGetText) {
    DocumentStore store;
    store.Open("file:///Y:/a.sv", "module a;", 1);
    EXPECT_TRUE(store.IsOpen("file:///Y:/a.sv"));
    auto t = store.GetText("file:///Y:/a.sv");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(*t, "module a;");
    EXPECT_EQ(store.Size(), 1u);
}

TEST(DocumentStore, ChangeAppliesIncremental) {
    DocumentStore store;
    store.Open("file:///Y:/a.sv", "hello", 1);
    std::vector<ContentChange> changes{MkChange(0, 0, 0, 5, "world")};
    ASSERT_TRUE(store.Change("file:///Y:/a.sv", changes, 2));
    EXPECT_EQ(*store.GetText("file:///Y:/a.sv"), "world");
    EXPECT_EQ(*store.GetVersion("file:///Y:/a.sv"), 2);
}

TEST(DocumentStore, ChangeOnUnknownDocReturnsFalse) {
    DocumentStore store;
    std::vector<ContentChange> changes{MkFull("x")};
    EXPECT_FALSE(store.Change("file:///Y:/nope.sv", changes, 1));
}

TEST(DocumentStore, VersionMonotonicAndOutOfOrderTolerated) {
    DocumentStore store;
    store.Open("file:///Y:/a.sv", "a", 5);

    // 乱序（更小的版本）：版本不倒退，但内容照样应用
    std::vector<ContentChange> c1{MkChange(0, 1, 0, 1, "b")};
    ASSERT_TRUE(store.Change("file:///Y:/a.sv", c1, 3));
    EXPECT_EQ(*store.GetVersion("file:///Y:/a.sv"), 5);  // 未被降级
    EXPECT_EQ(*store.GetText("file:///Y:/a.sv"), "ab");  // 内容仍然生效

    // 正常递增
    std::vector<ContentChange> c2{MkChange(0, 2, 0, 2, "c")};
    ASSERT_TRUE(store.Change("file:///Y:/a.sv", c2, 6));
    EXPECT_EQ(*store.GetVersion("file:///Y:/a.sv"), 6);
}

TEST(DocumentStore, SaveReplacesText) {
    DocumentStore store;
    store.Open("file:///Y:/a.sv", "old", 1);
    ASSERT_TRUE(store.Save("file:///Y:/a.sv", std::string("saved")));
    EXPECT_EQ(*store.GetText("file:///Y:/a.sv"), "saved");
}

TEST(DocumentStore, SaveWithoutTextKeepsBuffer) {
    DocumentStore store;
    store.Open("file:///Y:/a.sv", "keep", 1);
    ASSERT_TRUE(store.Save("file:///Y:/a.sv", std::nullopt));
    EXPECT_EQ(*store.GetText("file:///Y:/a.sv"), "keep");
}

TEST(DocumentStore, CloseRemovesDocument) {
    DocumentStore store;
    store.Open("file:///Y:/a.sv", "x", 1);
    EXPECT_TRUE(store.Close("file:///Y:/a.sv"));
    EXPECT_FALSE(store.IsOpen("file:///Y:/a.sv"));
    EXPECT_FALSE(store.Close("file:///Y:/a.sv"));  // 再关一次返回 false
}

TEST(DocumentStore, BufferTakesPrecedenceOverDisk) {
    DocumentStore store;
    store.SetDiskReader([](std::string_view) -> std::optional<std::string> {
        return std::string("from disk");
    });
    store.Open("file:///Y:/a.sv", "from buffer", 1);
    EXPECT_EQ(*store.GetText("file:///Y:/a.sv"), "from buffer");

    store.Close("file:///Y:/a.sv");
    EXPECT_EQ(*store.GetText("file:///Y:/a.sv"), "from disk");
}

TEST(DocumentStore, UnopenedReadsDisk) {
    DocumentStore store;
    store.SetDiskReader([](std::string_view path) -> std::optional<std::string> {
        if (path == "Y:/proj/a.sv") return std::string("disk content");
        return std::nullopt;
    });
    auto t = store.GetText("file:///Y:/proj/a.sv");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(*t, "disk content");
}

TEST(DocumentStore, MissingFileReturnsNullopt) {
    DocumentStore store;
    store.SetDiskReader([](std::string_view) -> std::optional<std::string> {
        return std::nullopt;
    });
    EXPECT_FALSE(store.GetText("file:///Y:/nothere.sv").has_value());
}

TEST(DocumentStore, SnapshotIsIsolatedFromLaterEdits) {
    DocumentStore store;
    store.Open("file:///Y:/a.sv", "before", 1);
    DocumentSnapshot snap = store.Snapshot("file:///Y:/a.sv");
    ASSERT_TRUE(snap.valid());
    EXPECT_EQ(snap.text, "before");

    std::vector<ContentChange> changes{MkFull("after")};
    ASSERT_TRUE(store.Change("file:///Y:/a.sv", changes, 2));

    // 快照不受影响
    EXPECT_EQ(snap.text, "before");
    EXPECT_EQ(*snap.version, 1);
}

TEST(DocumentStore, SnapshotOfUnknownDocIsInvalid) {
    DocumentStore store;
    EXPECT_FALSE(store.Snapshot("file:///Y:/nope.sv").valid());
}

// -----------------------------------------------------------------------------
// contentChanges JSON 解析
// -----------------------------------------------------------------------------

TEST(DocumentStoreJson, ParsesRangeChanges) {
    Json changes = Json::parse(R"([
        {"range":{"start":{"line":0,"character":1},"end":{"line":0,"character":3}},"text":"XY"}
    ])");
    const auto parsed = ParseContentChanges(changes);
    ASSERT_EQ(parsed.size(), 1u);
    EXPECT_TRUE(parsed[0].has_range);
    EXPECT_EQ(parsed[0].start_character, 1u);
    EXPECT_EQ(parsed[0].end_character, 3u);
    EXPECT_EQ(parsed[0].text, "XY");
}

TEST(DocumentStoreJson, MissingRangeMeansFullReplace) {
    Json changes = Json::parse(R"([{"text":"whole"}])");
    const auto parsed = ParseContentChanges(changes);
    ASSERT_EQ(parsed.size(), 1u);
    EXPECT_FALSE(parsed[0].has_range);
    EXPECT_EQ(parsed[0].text, "whole");
}

TEST(DocumentStoreJson, NullRangeMeansFullReplace) {
    Json changes = Json::parse(R"([{"range":null,"text":"whole"}])");
    const auto parsed = ParseContentChanges(changes);
    ASSERT_EQ(parsed.size(), 1u);
    EXPECT_FALSE(parsed[0].has_range);
}

TEST(DocumentStoreJson, MalformedEntriesSkipped) {
    Json changes = Json::parse(R"([1,"str",{"text":"ok"},null])");
    const auto parsed = ParseContentChanges(changes);
    ASSERT_EQ(parsed.size(), 1u);
    EXPECT_EQ(parsed[0].text, "ok");
}

TEST(DocumentStoreJson, NegativeAndMissingFieldsClampToZero) {
    Json changes = Json::parse(
        R"([{"range":{"start":{"line":-1,"character":-5},"end":{"line":0}},"text":""}])");
    const auto parsed = ParseContentChanges(changes);
    ASSERT_EQ(parsed.size(), 1u);
    EXPECT_EQ(parsed[0].start_line, 0u);
    EXPECT_EQ(parsed[0].start_character, 0u);
    EXPECT_EQ(parsed[0].end_character, 0u);
}

TEST(DocumentStoreJson, NonObjectJsonReturnsEmpty) {
    EXPECT_TRUE(ParseContentChanges(Json("x")).empty());
    EXPECT_TRUE(ParseContentChanges(Json()).empty());
}

TEST(DocumentStoreJson, Utf16ColumnsFromJsonApplied) {
    // 端到端：JSON 里的 UTF-16 列直接作用到含中文的文本
    DocumentStore store;
    store.Open("file:///Y:/a.sv", std::string(kChinese) + " tail", 1);
    const auto changes = ParseContentChanges(Json::parse(R"([
        {"range":{"start":{"line":0,"character":1},"end":{"line":0,"character":2}},"text":"B"}
    ])"));
    ASSERT_TRUE(store.Change("file:///Y:/a.sv", changes, 2));
    EXPECT_EQ(*store.GetText("file:///Y:/a.sv"), std::string("\xE4\xB8\xAD") + "B tail");
}

}  // namespace