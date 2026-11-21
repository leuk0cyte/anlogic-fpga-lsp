// =============================================================================
//  tests/unit/semantics_test.cpp —— B2：语义高亮 / 折叠 / token 统计
// =============================================================================
//  契约来自 Python 参考实现（fpgalsp/semantics.py）与 A2 的 wire 实测：
//    * top.v（tests/gate-corpus/corpus_alpha/src/top.v，与真实工程逐字节相同）
//        semanticTokens = 2094 个 token / 10470 个整数
//        counts = {comment:96, keyword:154, class:20, operator:1096, property:178,
//                  number:151, parameter:1, type:46, variable:331, function:21}
//        foldingRange = 24 条，首条 {111,114}
//    * 非 BMP（emoji）行必须按 UTF-16 列换算
//    * 空文档 -> 两类结果都是空
//  Python 侧数值由 A2 探针采集（audit/2026-11-18/parity-lsp/raw.json matrix[7][8]）
// =============================================================================

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "hdl/hdl_index.h"
#include "semantics/semantics.h"

namespace {

using fpgalsp::hdl::HdlIndex;
using fpgalsp::hdl::ParseVerilog;
using fpgalsp::semantics::EncodeTokens;
using fpgalsp::semantics::FoldingRange;
using fpgalsp::semantics::FoldingRanges;
using fpgalsp::semantics::SemToken;
using fpgalsp::semantics::SemanticTokens;
using fpgalsp::semantics::Summarize;
using fpgalsp::semantics::TokenStats;

const std::string kCorpusProject =
    std::string(FPGA_LSP_SOURCE_DIR) + "/tests/gate-corpus/corpus_alpha";
const std::string kCorpusTop = kCorpusProject + "/src/top.v";

std::string ReadFile(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.good()) return {};
    return std::string((std::istreambuf_iterator<char>(stream)),
                       std::istreambuf_iterator<char>());
}

// 全工程索引（top.v 的端口/例化重分类需要别的文件里的模块定义）
HdlIndex ProjectIndex(const std::string& root) {
    namespace fs = std::filesystem;
    std::vector<std::string> files;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) continue;
        const std::string extension = entry.path().extension().string();
        if (extension == ".v" || extension == ".sv" || extension == ".vh" ||
            extension == ".svh") {
            files.push_back(entry.path().generic_string());
        }
    }
    std::sort(files.begin(), files.end());
    HdlIndex index;
    for (const std::string& file : files) {
        index.AddFile(ParseVerilog(file, ReadFile(file)));
    }
    return index;
}

// FNV-1a 64：给 10470 个整数做稳定指纹（不引第三方哈希）
std::uint64_t Fnv1a64(const std::vector<int>& data) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const int value : data) {
        const auto bits = static_cast<std::uint32_t>(value);
        for (int byte = 0; byte < 4; ++byte) {
            hash ^= (bits >> (byte * 8)) & 0xffU;
            hash *= 0x100000001b3ULL;
        }
    }
    return hash;
}

// 用一份内联文本建"单文件索引"，方便断言
HdlIndex IndexFor(std::string_view path, const std::string& text) {
    HdlIndex index;
    index.AddFile(ParseVerilog(path, text));
    return index;
}

// 在 decoded token 里找 (line,start) 完全匹配的那条
const SemToken* Find(const std::vector<SemToken>& tokens, int line, int start) {
    for (const SemToken& token : tokens) {
        if (token.line == line && token.start == start) return &token;
    }
    return nullptr;
}

std::string TokensAsText(const std::vector<SemToken>& tokens, std::string_view text) {
    // 调试用：把 token 覆盖的原文片段拼出来
    std::string out;
    std::size_t offset = 0;
    std::vector<std::string_view> rows;
    for (;;) {
        const std::size_t newline = text.find('\n', offset);
        if (newline == std::string_view::npos) {
            rows.push_back(text.substr(offset));
            break;
        }
        rows.push_back(text.substr(offset, newline - offset));
        offset = newline + 1;
    }
    for (const SemToken& token : tokens) {
        if (token.line >= 0 && static_cast<std::size_t>(token.line) < rows.size()) {
            out += std::string(rows[token.line].substr(token.start, token.length));
            out += '|';
        }
    }
    return out;
}

// -----------------------------------------------------------------------------
// 1. legend：12 项、顺序与 Python TOKEN_TYPES 完全一致
// -----------------------------------------------------------------------------
TEST(SemanticsLegend, MatchesPythonTokenTypes) {
    const std::vector<std::string> expected = {
        "keyword", "type", "class", "property", "variable", "parameter",
        "function", "number", "comment", "string", "operator", "macro",
    };
    ASSERT_EQ(static_cast<int>(expected.size()), fpgalsp::semantics::kTokenTypeCount);
    for (int type = 0; type < fpgalsp::semantics::kTokenTypeCount; ++type) {
        EXPECT_EQ(fpgalsp::semantics::TokenTypeName(type), expected[static_cast<std::size_t>(type)])
            << "token type index " << type;
    }
}

// -----------------------------------------------------------------------------
// 2. Verible y-symbol 名 -> 语义类型（Python _kind_to_type 的等价物）
// -----------------------------------------------------------------------------
TEST(SemanticsMapping, SymbolNamesToTokenTypes) {
    using namespace fpgalsp::semantics;
    // 带双引号的显示名 = 关键字 / 预处理指令 / 属性
    EXPECT_EQ(TokenTypeFromSymbolName("\"module\""), kKeyword);
    EXPECT_EQ(TokenTypeFromSymbolName("\"endmodule\""), kKeyword);
    EXPECT_EQ(TokenTypeFromSymbolName("\"`include\""), kKeyword);
    EXPECT_EQ(TokenTypeFromSymbolName("\"(*attribute*)\""), kKeyword);
    // 单引号 = 运算符/标点
    EXPECT_EQ(TokenTypeFromSymbolName("'('"), kOperator);
    EXPECT_EQ(TokenTypeFromSymbolName("';'"), kOperator);
    EXPECT_EQ(TokenTypeFromSymbolName("'<='"), kOperator);
    // 标识符 / 数字 / 注释 / 字符串 / 宏 / 系统函数
    EXPECT_EQ(TokenTypeFromSymbolName("SymbolIdentifier"), kVariable);
    EXPECT_EQ(TokenTypeFromSymbolName("EscapedIdentifier"), kVariable);
    EXPECT_EQ(TokenTypeFromSymbolName("TK_DecNumber"), kNumber);
    EXPECT_EQ(TokenTypeFromSymbolName("TK_BinBase"), kNumber);
    EXPECT_EQ(TokenTypeFromSymbolName("TK_HexDigits"), kNumber);
    EXPECT_EQ(TokenTypeFromSymbolName("TK_EOL_COMMENT"), -1);  // 名字里没有 comment 的形态
    EXPECT_EQ(TokenTypeFromSymbolName("\"// end of line comment\""), kComment);
    EXPECT_EQ(TokenTypeFromSymbolName("\"/&lowast;comment&lowast;/\""), kComment);
    EXPECT_EQ(TokenTypeFromSymbolName("TK_StringLiteral"), kString);
    EXPECT_EQ(TokenTypeFromSymbolName("PP_Identifier"), kMacro);
    EXPECT_EQ(TokenTypeFromSymbolName("SystemTFIdentifier"), kFunction);
    // 空白 / EOF / 未知 TK_ 一律丢弃
    EXPECT_EQ(TokenTypeFromSymbolName("\"<<space>>\""), -1);
    EXPECT_EQ(TokenTypeFromSymbolName("\"<<newline>>\""), -1);
    EXPECT_EQ(TokenTypeFromSymbolName("\"end of file\""), -1);
    EXPECT_EQ(TokenTypeFromSymbolName("TK_UNKNOWN_THING"), -1);
}

// -----------------------------------------------------------------------------
// 3. 注释/字符串掩码（Python refs.code_mask）
// -----------------------------------------------------------------------------
TEST(SemanticsMask, CommentsAndStringsAreMasked) {
    const std::string text =
        "wire a; // 行注释 wire\n"
        "/* 块注释\n"
        "   仍然注释 */ wire b;\n"
        "assign c = \"string with wire and // not a comment\";\n";
    const std::vector<char> mask = fpgalsp::semantics::CodeMask(text);
    ASSERT_EQ(mask.size(), text.size());

    // 1 = 代码，0 = 注释/字符串（与 Python refs.code_mask 逐字节一致）
    const auto is_code = [&](std::size_t offset) { return mask[offset] == 1; };
    const std::size_t line_comment = text.find("// 行注释");
    const std::size_t block_comment = text.find("/* 块注释");
    const std::size_t block_close = text.find("*/ wire b");
    const std::size_t string_literal = text.find("\"string with");
    const std::size_t string_close = text.find("\";");
    const std::size_t second_wire = text.find("wire b");
    const std::size_t third_assign = text.find("assign c");

    ASSERT_NE(line_comment, std::string::npos);
    ASSERT_NE(block_comment, std::string::npos);
    ASSERT_NE(string_literal, std::string::npos);

    EXPECT_TRUE(is_code(text.find("wire a"))) << "第一行的 wire 是代码";
    EXPECT_FALSE(is_code(line_comment)) << "// 本身也在注释里";
    EXPECT_FALSE(is_code(line_comment + 2)) << "行注释内容应被掩码";
    EXPECT_FALSE(is_code(block_comment)) << "/* 本身在注释里";
    EXPECT_FALSE(is_code(block_comment + 3)) << "块注释内容应被掩码";
    EXPECT_FALSE(is_code(block_close)) << "*/ 的 '*' 仍属于注释";
    EXPECT_FALSE(is_code(block_close + 1)) << "*/ 的 '/' 仍属于注释";
    EXPECT_TRUE(is_code(block_close + 2)) << "*/ 之后回到代码（空格）";
    EXPECT_TRUE(is_code(second_wire)) << "块注释之后的 wire 是代码";
    EXPECT_FALSE(is_code(string_literal)) << "字符串的引号也在掩码内";
    EXPECT_FALSE(is_code(string_literal + 1)) << "字符串内容应被掩码";
    EXPECT_FALSE(is_code(string_close - 2)) << "字符串里的 // 不是注释（仍在字符串内）";
    EXPECT_TRUE(is_code(third_assign)) << "第二行的 assign 是代码";
}
// -----------------------------------------------------------------------------
// 4. 折叠：module/begin/end + 注释块（Python folding_ranges 同序）
// -----------------------------------------------------------------------------
TEST(SemanticsFolding, ModuleBeginEndAndComments) {
    const std::string text =
        "// 文件头注释\n"
        "// 第二行\n"
        "// 第三行\n"
        "module demo;\n"
        "  always @(posedge clk) begin\n"
        "    // 内部注释\n"
        "    x <= y;\n"
        "  end\n"
        "endmodule\n";
    const std::vector<FoldingRange> ranges = FoldingRanges(text);
    // 与 Python semantics.folding_ranges() 逐条一致：
    //   begin..end 先出栈 -> {4,7}，module..endmodule -> {3,8}，最后是连续 // 块 {0,2}
    ASSERT_EQ(ranges.size(), 3u);
    EXPECT_EQ(ranges[0].start_line, 4);
    EXPECT_EQ(ranges[0].end_line, 7);
    EXPECT_FALSE(ranges[0].comment);
    EXPECT_EQ(ranges[1].start_line, 3);
    EXPECT_EQ(ranges[1].end_line, 8);
    EXPECT_FALSE(ranges[1].comment);
    EXPECT_EQ(ranges[2].start_line, 0);
    EXPECT_EQ(ranges[2].end_line, 2);
    EXPECT_TRUE(ranges[2].comment);
}
TEST(SemanticsFolding, UnclosedBlockCommentFoldsToEnd) {
    const std::string text =
        "module m;\n"
        "/* 未闭合\n"
        "   一直到底\n";
    // Python: 未闭合块注释一直折到**最后一行**（含末尾空行 -> endLine = len(rows)-1 = 3）；
    //         module 没有 endmodule，所以不产生括号区间。
    const std::vector<FoldingRange> ranges = FoldingRanges(text);
    ASSERT_EQ(ranges.size(), 1u);
    EXPECT_EQ(ranges[0].start_line, 1);
    EXPECT_EQ(ranges[0].end_line, 3);
    EXPECT_TRUE(ranges[0].comment);
}
// -----------------------------------------------------------------------------
// 5. 小文件：类型/位置/掩码/数字/参数
// -----------------------------------------------------------------------------
TEST(SemanticsTokens, InlineModuleClassification) {
    // 期望值逐项对齐 Python server 的 merged token（primary + verible）
    const std::string text =
        "// 头部注释\n"
        "module demo #(parameter WIDTH = 8) (\n"
        "  input  wire clk,\n"
        "  output reg  [WIDTH-1:0] q\n"
        ");\n"
        "  localparam DEPTH = 4;\n"
        "  assign q = \"wire clk\"; // 字符串里的关键字不算\n"
        "endmodule\n";
    const std::string path = "inline_demo.v";
    HdlIndex index = IndexFor(path, text);
    const std::vector<SemToken> tokens = SemanticTokens(&index, path, text);
    ASSERT_FALSE(tokens.empty());

    struct Expect {
        int line;
        int start;
        int length;
        int type;
    };
    const Expect expected[] = {
        {0, 0, 7, fpgalsp::semantics::kComment},    // "// 头部注释"
        {1, 0, 6, fpgalsp::semantics::kKeyword},    // module
        {1, 7, 4, fpgalsp::semantics::kClass},      // demo（来自索引）
        {1, 14, 9, fpgalsp::semantics::kKeyword},   // parameter
        {1, 24, 5, fpgalsp::semantics::kParameter}, // WIDTH
        {1, 32, 1, fpgalsp::semantics::kNumber},    // 8
        {2, 2, 5, fpgalsp::semantics::kKeyword},    // input
        {2, 9, 4, fpgalsp::semantics::kType},       // wire
        {2, 14, 3, fpgalsp::semantics::kProperty},  // clk（端口）
        {3, 2, 6, fpgalsp::semantics::kKeyword},    // output
        {3, 9, 3, fpgalsp::semantics::kType},       // reg
        {3, 15, 5, fpgalsp::semantics::kParameter}, // WIDTH 的引用（重分类）
        {3, 21, 1, fpgalsp::semantics::kNumber},    // 1
        {3, 23, 1, fpgalsp::semantics::kNumber},    // 0
        {3, 26, 1, fpgalsp::semantics::kProperty},  // q（端口）
        {5, 2, 10, fpgalsp::semantics::kKeyword},   // localparam
        {5, 13, 5, fpgalsp::semantics::kParameter}, // DEPTH
        {5, 21, 1, fpgalsp::semantics::kNumber},    // 4
        {6, 2, 6, fpgalsp::semantics::kKeyword},    // assign
        {6, 13, 10, fpgalsp::semantics::kString},   // "wire clk"
        {6, 25, 13, fpgalsp::semantics::kComment},  // // 字符串里的...
        {7, 0, 9, fpgalsp::semantics::kKeyword},    // endmodule
    };
    for (const Expect& item : expected) {
        const SemToken* token = Find(tokens, item.line, item.start);
        ASSERT_NE(token, nullptr) << "缺少 token L" << item.line << " C" << item.start;
        EXPECT_EQ(token->length, item.length) << "L" << item.line << " C" << item.start;
        EXPECT_EQ(token->type, item.type) << "L" << item.line << " C" << item.start;
    }

    // 字符串内部不得出现 keyword/type（掩码生效）
    const int string_column = 13;
    for (const SemToken& token : tokens) {
        if (token.line != 6) continue;
        if (token.start > string_column && token.start < string_column + 10) {
            ADD_FAILURE() << "字符串内部出现了 token: type=" << token.type
                          << " start=" << token.start;
        }
    }

    // 每行严格递增、不重叠
    for (std::size_t i = 1; i < tokens.size(); ++i) {
        if (tokens[i].line == tokens[i - 1].line) {
            EXPECT_GT(tokens[i].start, tokens[i - 1].start) << "token 应严格递增且不重叠";
        } else {
            EXPECT_LT(tokens[i - 1].line, tokens[i].line);
        }
    }
}
// -----------------------------------------------------------------------------
// 6. 非 BMP（emoji）按 UTF-16 列换算
// -----------------------------------------------------------------------------
TEST(SemanticsTokens, Utf16ColumnsAfterEmoji) {
    // "/* 🚀 中文 */ wire emoji_sig;" —— emoji 是 2 个 UTF-16 码元
    //  列：0..1 "/*"，3 空格，4 emoji(2)，6 空格，7 中，8 文，9 空格，10 "*/"...
    const std::string text =
        "module m(input wire clk);\n"
        "/* 🚀 中文 */ wire emoji_sig;\n"
        "endmodule\n";
    const std::string path = "emoji.v";
    HdlIndex index = IndexFor(path, text);
    const std::vector<SemToken> tokens = SemanticTokens(&index, path, text);
    ASSERT_FALSE(tokens.empty());

    // `wire` 的 UTF-16 列 = "/* " (3) + emoji (2) + " 中文 " (4) + "*/" (2) + " " (1) = 12
    const SemToken* wire_type = Find(tokens, 1, 12);
    ASSERT_NE(wire_type, nullptr) << "emoji 行的 wire 应在 UTF-16 列 12；实际 token 见下";
    EXPECT_EQ(wire_type->type, fpgalsp::semantics::kType);
    const SemToken* signal = Find(tokens, 1, 17);  // "wire " 之后
    ASSERT_NE(signal, nullptr);
    EXPECT_EQ(signal->type, fpgalsp::semantics::kVariable);
    EXPECT_EQ(signal->length, static_cast<int>(std::string("emoji_sig").size()));
}

// -----------------------------------------------------------------------------
// 7. 空文档 / 无索引
// -----------------------------------------------------------------------------
TEST(SemanticsTokens, EmptyInputIsEmpty) {
    HdlIndex index;
    EXPECT_TRUE(SemanticTokens(&index, "empty.v", "").empty());
    EXPECT_TRUE(SemanticTokens(nullptr, "empty.v", "module m;\nendmodule\n").empty());
    EXPECT_TRUE(FoldingRanges("").empty());
    // 只有空行的文本也没有折叠区间
    EXPECT_TRUE(FoldingRanges("\n\n\n").empty());
}

// -----------------------------------------------------------------------------
// 8. LSP delta 编码
// -----------------------------------------------------------------------------
TEST(SemanticsEncode, DeltaEncoding) {
    const std::vector<SemToken> tokens = {
        {0, 0, 3, fpgalsp::semantics::kKeyword},
        {0, 4, 5, fpgalsp::semantics::kClass},
        {2, 1, 2, fpgalsp::semantics::kVariable},
    };
    const std::vector<int> data = EncodeTokens(tokens);
    const std::vector<int> expected = {
        0, 0, 3, 0, 0,
        0, 4, 5, 2, 0,
        2, 1, 2, 4, 0,
    };
    EXPECT_EQ(data, expected);
    EXPECT_EQ(data.size(), tokens.size() * 5);
}

// -----------------------------------------------------------------------------
// 9. 真实语料：与 Python 逐项对齐（A2 探针的数字）
// -----------------------------------------------------------------------------
TEST(SemanticsCorpus, RealTopModuleMatchesPython) {
    const std::string text = ReadFile(kCorpusTop);
    ASSERT_FALSE(text.empty()) << "语料缺失: " << kCorpusTop;
    // ⚠ 必须用全工程索引：端口/例化模块的重分类要靠别的文件里的模块定义
    const HdlIndex index = ProjectIndex(kCorpusProject);

    const std::vector<SemToken> tokens = SemanticTokens(&index, kCorpusTop, text);
    EXPECT_EQ(tokens.size(), 2094u) << "Python semanticTokens = 2094 个 token";

    const TokenStats stats = Summarize(tokens);
    EXPECT_EQ(stats.total, 2094);
    const std::map<std::string, int, std::less<>> expected = {
        {"class", 20},    {"comment", 96},  {"function", 21}, {"keyword", 154},
        {"number", 151},  {"operator", 1096}, {"parameter", 1}, {"property", 178},
        {"type", 46},     {"variable", 331},
    };
    EXPECT_EQ(stats.counts, expected);

    // 首尾 token：第一行是 79 字符的长注释；最后一条是 endmodule（行 500，9 字符）
    ASSERT_FALSE(tokens.empty());
    EXPECT_EQ(tokens.front().line, 0);
    EXPECT_EQ(tokens.front().start, 0);
    EXPECT_EQ(tokens.front().length, 79);
    EXPECT_EQ(tokens.front().type, fpgalsp::semantics::kComment);
    EXPECT_EQ(tokens.back().type, fpgalsp::semantics::kKeyword);
    EXPECT_EQ(tokens.back().line, 500);
    EXPECT_EQ(tokens.back().start, 0);
    EXPECT_EQ(tokens.back().length, 9);

    // 10470 个整数 = 2094 * 5；FNV 指纹逐元素锁定（Python 同一份 data 算出来相同）
    const std::vector<int> data = EncodeTokens(tokens);
    EXPECT_EQ(data.size(), 10470u);
    EXPECT_EQ(data.size(), tokens.size() * 5);
    EXPECT_EQ(Fnv1a64(data), 0x10fb3965587cc85cULL)
        << "与 Python 的 data 逐元素不一致（FNV-1a 64 指纹不同）";
}

TEST(SemanticsCorpus, RealTopModuleFoldingMatchesPython) {
    const std::string text = ReadFile(kCorpusTop);
    ASSERT_FALSE(text.empty());
    const std::vector<FoldingRange> ranges = FoldingRanges(text);
    // Python foldingRange 的 24 条区间，**连顺序一起**逐条对齐
    const std::vector<FoldingRange> expected = {
        {111, 114, false}, {127, 135, false}, {135, 143, false}, {126, 144, false},
        {152, 155, false}, {158, 161, false}, {168, 178, false}, {178, 188, false},
        {167, 189, false}, {194, 197, false}, {268, 287, false}, {267, 288, false},
        {14, 500, false},
        {0, 12, true},     {97, 103, true},   {117, 119, true},   {146, 150, true},
        {191, 192, true},  {291, 304, true},  {343, 344, true},   {400, 402, true},
        {412, 414, true},  {468, 469, true},  {495, 498, true},
    };
    ASSERT_EQ(ranges.size(), expected.size()) << "Python foldingRange = 24 条";
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(ranges[i].start_line, expected[i].start_line) << "range #" << i;
        EXPECT_EQ(ranges[i].end_line, expected[i].end_line) << "range #" << i;
        EXPECT_EQ(ranges[i].comment, expected[i].comment) << "range #" << i;
    }
    std::size_t comment_ranges = 0;
    for (const FoldingRange& range : ranges) {
        EXPECT_LT(range.start_line, range.end_line);
        if (range.comment) ++comment_ranges;
    }
    EXPECT_EQ(comment_ranges, 11u);
}

// -----------------------------------------------------------------------------
// 10. 语料自检：真实工程副本与探针口径一致（CRLF 不影响结果）
// -----------------------------------------------------------------------------
TEST(SemanticsCorpus, CrlfTextProducesSameTokens) {
    std::string text = ReadFile(kCorpusTop);
    ASSERT_FALSE(text.empty());
    std::string lf;
    lf.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue;
        lf.push_back(text[i]);
    }
    ASSERT_NE(lf, text) << "语料应是 CRLF（与真实工程一致）";

    HdlIndex index_crlf;
    index_crlf.AddFile(ParseVerilog(kCorpusTop, text));
    HdlIndex index_lf;
    index_lf.AddFile(ParseVerilog(kCorpusTop, lf));

    const std::vector<SemToken> from_crlf = SemanticTokens(&index_crlf, kCorpusTop, text);
    const std::vector<SemToken> from_lf = SemanticTokens(&index_lf, kCorpusTop, lf);
    ASSERT_EQ(from_crlf.size(), from_lf.size());
    for (std::size_t i = 0; i < from_crlf.size(); ++i) {
        EXPECT_EQ(from_crlf[i].line, from_lf[i].line) << "token #" << i;
        EXPECT_EQ(from_crlf[i].start, from_lf[i].start) << "token #" << i;
        EXPECT_EQ(from_crlf[i].length, from_lf[i].length) << "token #" << i;
        EXPECT_EQ(from_crlf[i].type, from_lf[i].type) << "token #" << i;
    }
    EXPECT_EQ(FoldingRanges(text).size(), FoldingRanges(lf).size());
}
}  // namespace

// 索引比缓冲区旧时（典型：格式化把行号整体挪动），从索引拿的 module/port/instance
// token 必须被丢弃，而不是画到错误的位置上。用户现象：格式化后高亮全乱，切走再回来又好了。
TEST(SemanticsTokens, StaleIndexDoesNotMisplaceTokens) {
    const std::string original =
        "module demo (\n"
        "  input  wire clk\n"
        ");\n"
        "endmodule\n";
    const std::string path = "stale_demo.v";
    HdlIndex index = IndexFor(path, original);

    // 模拟格式化：在文件开头插入几行注释，所有行号 +3
    const std::string formatted =
        "// header 1\n"
        "// header 2\n"
        "// header 3\n" + original;
    const std::vector<SemToken> tokens = SemanticTokens(&index, path, formatted);

    // 不允许出现"索引位置"(旧行 0/1) 上的 class/property token —— 那里现在是注释
    for (const SemToken& token : tokens) {
        if (token.line < 3 && (token.type == fpgalsp::semantics::kClass ||
                               token.type == fpgalsp::semantics::kProperty)) {
            ADD_FAILURE() << "stale index token at line " << token.line << " start "
                          << token.start << " type " << token.type;
        }
    }
    // 新位置上 module 名仍应由行内规则/Verible 上色（至少有 keyword "module"）
    bool keyword_module = false;
    for (const SemToken& token : tokens) {
        if (token.line == 3 && token.type == fpgalsp::semantics::kKeyword) keyword_module = true;
    }
    EXPECT_TRUE(keyword_module);
}
