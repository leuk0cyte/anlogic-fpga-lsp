// =============================================================================
//  src/semantics/semantics.cpp —— 语义高亮 / 折叠 / token 统计的实现
// =============================================================================
//  对应参考实现：Y:\fpga-lsp\fpgalsp\semantics.py（368 行）+ refs.py:code_mask
//
//  流水线（顺序与 Python 完全一致，任何一步换顺序都会改变结果）：
//    1. CodeMask(text)                     -> 每字节 1=代码 0=注释/字符串
//    2. 每行把掩码处换成空格（**字节长度不变**，这样匹配位置能直接映射回原行）
//    3. HdlSideTokens：
//         a) index 里的 module（class,9）/ port（property,8）/ instance（function,7）
//         b) 行内正则：类型词（type,6）/ 关键字（keyword,2）/ parameter_re（5）/
//            declaration_re（variable,5）/ literal_re（number,4）/ number_re（number,3）
//         c) (line,start) 去重（高优先级胜，同优先级保留先到的）+ 排序 + 去同行重叠
//    4. VeribleSideTokens：Verible 原始词法流（带 byte offset）-> 每行切分 ->
//       y-symbol 名映射 -> UTF-16 列；再按 hdl 索引把标识符重分类
//    5. 合并（primary 优先）+ 排序 + 去同行重叠 -> EncodeTokens
//
//  ⚠ 性能：单文件、线性；500 行 top.v 约 2000 token，都是栈上/vector 操作。
// =============================================================================

#include "semantics/semantics.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "platform/utf16.h"
#include "verible/common/text/token-info.h"
#include "verible/verilog/parser/verilog-lexer.h"
#include <span>
#include "verible/verilog/parser/verilog-parser.h"  // verilog_symbol_name()

namespace fpgalsp::semantics {
namespace {

// -----------------------------------------------------------------------------
//  Python 侧的三张词表（逐项照抄，顺序无关但内容不能改）
// -----------------------------------------------------------------------------

bool InSet(std::span<const std::string_view> table, std::string_view word) {
    return std::ranges::find(table, word) != table.end();
}

constexpr auto kTypeWords = std::to_array<std::string_view>({
    "wire", "reg", "logic", "integer", "signed", "unsigned",
    "tri", "genvar", "real", "time", "supply0", "supply1",
});

constexpr auto kKeywords = std::to_array<std::string_view>({
    "module", "endmodule", "input", "output", "inout", "wire", "reg", "logic", "assign",
    "always", "always_ff", "always_comb", "always_latch", "begin", "end", "if", "else",
    "case", "casez", "casex", "endcase", "default", "for", "while", "repeat", "forever",
    "posedge", "negedge", "parameter", "localparam", "generate", "endgenerate",
    "function", "endfunction", "task", "endtask", "initial", "defparam",
    "specify", "endspecify", "and", "or", "nand", "nor", "xor", "xnor", "not", "buf",
});

constexpr auto kOpenWords = std::to_array<std::string_view>({
    "module", "begin", "case", "casez", "casex", "generate", "function", "task", "fork",
});

constexpr auto kCloseWords = std::to_array<std::string_view>({
    "endmodule", "end", "endcase", "endfunction", "endtask", "endgenerate", "join",
});

// declaration_re 的关键字顺序（Python 交替式的书写顺序）
constexpr auto kDeclKeywords = std::to_array<std::string_view>({
    "input", "output", "inout", "wire", "reg", "logic", "tri", "integer",
    "genvar", "real", "time", "parameter", "localparam",
});

// -----------------------------------------------------------------------------
//  字符分类（全部按 ASCII，与 Python 的正则一致）
// -----------------------------------------------------------------------------

bool IsWordChar(unsigned char c) {
    return std::isalnum(c) != 0 || c == '_' || c == '$';
}
bool IsIdentStart(unsigned char c) { return std::isalpha(c) != 0 || c == '_'; }
bool IsSpaceChar(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}
bool IsDigit(char c) { return c >= '0' && c <= '9'; }

// -----------------------------------------------------------------------------
//  掩码扫描（Python refs.code_mask 的逐字节状态机）
// -----------------------------------------------------------------------------
// mask_out != nullptr 时把"代码字节"写成 1（注释/字符串字节保持 0，调用方负责
// 清零）。返回扫描到 `end` 处的状态（代码 / 块注释 / 字符串）。
//
// ⚠ lookahead 始终看**整段文本**（text[index + 1]，不 clamp 到 end）：这样把
//    文本按行切成若干段、状态逐段传递时，结果与整篇一次扫描逐字节相同 ——
//    连 "/" 落在行尾、"*" 落在下一行行首这种跨行 `/*` 也一致（状态机在行尾
//    就已经跳过 "*" 进入块注释状态）。行级增量依赖这个性质。
enum MaskState : int { kMaskCode = 0, kMaskBlock = 1, kMaskString = 2 };

int ScanMask(std::string_view text, std::size_t begin, std::size_t end, int initial_state,
             char* mask_out) {
    enum class State { kCode, kLine, kBlock, kString };
    State state = static_cast<State>(initial_state);
    const std::size_t size = text.size();
    std::size_t index = begin;
    const auto at = [&](std::size_t position) -> char {
        return position < size ? text[position] : '\0';
    };
    while (index < end) {
        const char ch = text[index];
        const char next = at(index + 1);
        switch (state) {
            case State::kCode:
                if (ch == '/' && next == '/') {
                    state = State::kLine;
                    index += 2;
                    continue;
                }
                if (ch == '/' && next == '*') {
                    state = State::kBlock;
                    index += 2;
                    continue;
                }
                if (ch == '"') {
                    state = State::kString;
                    ++index;
                    continue;
                }
                if (mask_out != nullptr) mask_out[index - begin] = 1;
                ++index;
                break;
            case State::kLine:
                if (ch == '\n') {
                    state = State::kCode;
                    ++index;
                    continue;
                }
                ++index;
                break;
            case State::kBlock:
                if (ch == '*' && next == '/') {
                    state = State::kCode;
                    index += 2;
                    continue;
                }
                ++index;
                break;
            case State::kString:
                if (ch == '\\') {
                    index += 2;
                    continue;
                }
                if (ch == '"') state = State::kCode;
                ++index;
                break;
        }
    }
    return static_cast<int>(state);
}

// 行内扫描的辅助判断（增量安全条件）
bool RowEndsWithSlash(std::string_view row) {
    // 行尾是 '/' 时，该行扫描的 lookahead 会是下一行的首字节 —— 下一行变了就可能
    // 让本行的 '/' 变成 '/*'，所以这种行不能当作增量锚点的"上一行"。
    return !row.empty() && row.back() == '/';
}
bool RowEndsWithBackslash(std::string_view row) {
    // Verilog 宏续行：反斜杠紧邻换行时，下一行属于宏体（Verible 词法状态会延续）。
    if (!row.empty() && row.back() == '\r') row.remove_suffix(1);
    return !row.empty() && row.back() == '\\';
}

// -----------------------------------------------------------------------------
//  行表：text.split("\n") + 每行起始字节偏移
// -----------------------------------------------------------------------------

struct LineTable {
    std::string_view full;
    std::vector<std::string_view> rows;
    std::vector<std::size_t> starts;
};

LineTable BuildLines(std::string_view text) {
    LineTable table;
    table.full = text;
    std::size_t position = 0;
    for (;;) {
        const std::size_t newline = text.find('\n', position);
        if (newline == std::string_view::npos) {
            table.rows.push_back(text.substr(position));
            table.starts.push_back(position);
            break;
        }
        table.rows.push_back(text.substr(position, newline - position));
        table.starts.push_back(position);
        position = newline + 1;
    }
    return table;
}

// 行宽（UTF-16 码元数），不含行尾的 '\r'（Python: row.rstrip("\r") 之后再量）。
int RowWidth(std::string_view row) {
    if (!row.empty() && row.back() == '\r') row.remove_suffix(1);
    return static_cast<int>(platform::utf16_len(row));
}

// 行内字节偏移 -> UTF-16 列（clamp 到行尾）
int ByteToColumn(std::string_view row, std::size_t byte_offset) {
    if (!row.empty() && row.back() == '\r') row.remove_suffix(1);
    return static_cast<int>(platform::byte_index_to_utf16_column(row, byte_offset));
}

// -----------------------------------------------------------------------------
//  token 候选表：同一 (line,start) 只留优先级最高的一条
// -----------------------------------------------------------------------------

struct Candidate {
    int length = 0;
    int type = 0;
    int priority = 0;
};

class CandidateTable {
public:
    CandidateTable(const LineTable& lines) : lines_(lines) {}
    // 行级增量：只保留 [first, last) 行（全量路径 first=0, last=rows.size()）。
    CandidateTable(const LineTable& lines, std::size_t first, std::size_t last)
        : lines_(lines), first_(first), last_(last) {}

    void Add(int line, int start, int length, int type, int priority) {
        if (length <= 0 || start < 0) return;
        if (line < 0 || static_cast<std::size_t>(line) >= lines_.rows.size()) return;
        if (static_cast<std::size_t>(line) < first_ ||
            static_cast<std::size_t>(line) >= last_) {
            return;
        }
        const int width = RowWidth(lines_.rows[static_cast<std::size_t>(line)]);
        if (start + length > width) return;
        const auto key = std::make_pair(line, start);
        const auto it = found_.find(key);
        if (it != found_.end() && it->second.priority >= priority) return;  // 同优先级先到先得
        found_[key] = Candidate{.length=length, .type=type, .priority=priority};
    }

    // 排序 + 去同行重叠（Python 的 ordered / last_line,last_end 逻辑）
    std::vector<SemToken> Finish() const {
        std::vector<SemToken> ordered;
        ordered.reserve(found_.size());
        for (const auto& [position, candidate] : found_) {
            ordered.push_back(SemToken{.line=position.first, .start=position.second,
                                       .length=candidate.length, .type=candidate.type,});
        }
        std::ranges::sort(ordered, [](const SemToken& a, const SemToken& b) {
            return a.line != b.line ? a.line < b.line : a.start < b.start;
        });
        std::vector<SemToken> result;
        result.reserve(ordered.size());
        int last_line = -1;
        int last_end = -1;
        for (const SemToken& token : ordered) {
            if (token.line == last_line && token.start < last_end) continue;
            result.push_back(token);
            last_line = token.line;
            last_end = token.start + token.length;
        }
        return result;
    }

private:
    const LineTable& lines_;
    std::size_t first_ = 0;
    std::size_t last_ = static_cast<std::size_t>(-1);
    std::map<std::pair<int, int>, Candidate> found_;
};

// -----------------------------------------------------------------------------
//  行内扫描（Python 的四个 re 的等价物，全部在"掩码成空格"的行上跑）
// -----------------------------------------------------------------------------

// 一段字节区间 -> (UTF-16 列, 长度) 后入表
void AddSpan(CandidateTable* sink, int line, std::string_view row, std::size_t begin,
             std::size_t end, int type, int priority) {
    const int start = ByteToColumn(row, begin);
    const int stop = ByteToColumn(row, end);
    sink->Add(line, start, stop - start, type, priority);
}

// word_re: [A-Za-z_][A-Za-z0-9_$]*
void ScanWords(int line, std::string_view row, std::string_view code, CandidateTable* sink) {
    std::size_t index = 0;
    while (index < code.size()) {
        if (!IsIdentStart(static_cast<unsigned char>(code[index]))) {
            ++index;
            continue;
        }
        const std::size_t begin = index;
        while (index < code.size() && IsWordChar(static_cast<unsigned char>(code[index]))) {
            ++index;
        }
        const std::string_view word = code.substr(begin, index - begin);
        if (InSet(kTypeWords, word)) {
            AddSpan(sink, line, row, begin, index, kType, 6);
        } else if (InSet(kKeywords, word)) {
            AddSpan(sink, line, row, begin, index, kKeyword, 2);
        }
    }
}

// parameter_re: \b(?:parameter|localparam)\s+(?:signed\s+)?(?:\[[^\]]*\]\s*)?
//              ([A-Za-z_][A-Za-z0-9_$]*)\s*=
bool MatchParameter(std::string_view code, std::size_t at, std::size_t* name_begin,
                    std::size_t* name_end, std::size_t* match_end) {
    if (at > 0 && IsWordChar(static_cast<unsigned char>(code[at - 1]))) return false;
    std::size_t length = 0;
    if (code.compare(at, 9, "parameter") == 0) {
        length = 9;
    } else if (code.compare(at, 10, "localparam") == 0) {
        length = 10;
    } else {
        return false;
    }
    std::size_t index = at + length;
    if (index >= code.size() || !IsSpaceChar(code[index])) return false;  // \s+
    while (index < code.size() && IsSpaceChar(code[index])) ++index;
    if (code.compare(index, 6, "signed") == 0 &&
        (index + 6 >= code.size() || IsSpaceChar(code[index + 6]))) {
        index += 6;
        while (index < code.size() && IsSpaceChar(code[index])) ++index;
    }
    if (index < code.size() && code[index] == '[') {  // (?:\[[^\]]*\]\s*)?
        const std::size_t close = code.find(']', index + 1);
        if (close != std::string_view::npos) {
            index = close + 1;
            while (index < code.size() && IsSpaceChar(code[index])) ++index;
        }
    }
    if (index >= code.size() || !IsIdentStart(static_cast<unsigned char>(code[index]))) {
        return false;
    }
    const std::size_t begin = index;
    while (index < code.size() && IsWordChar(static_cast<unsigned char>(code[index]))) ++index;
    const std::size_t end = index;
    while (index < code.size() && IsSpaceChar(code[index])) ++index;
    if (index >= code.size() || code[index] != '=') return false;
    *name_begin = begin;
    *name_end = end;
    *match_end = index + 1;
    return true;
}

// declaration_re: \b(?:input|output|...)\b\s*(?:signed|unsigned)?\s*(?:\[[^\]]*\]\s*)?
//                 ([A-Za-z_][A-Za-z0-9_$]*)
bool MatchDeclaration(std::string_view code, std::size_t at, std::size_t* name_begin,
                      std::size_t* match_end) {
    if (at > 0 && IsWordChar(static_cast<unsigned char>(code[at - 1]))) return false;
    std::size_t after = 0;
    for (auto keyword : kDeclKeywords) {
        if (code.compare(at, keyword.size(), keyword) != 0) continue;
        const std::size_t end = at + keyword.size();
        if (end < code.size() && IsWordChar(static_cast<unsigned char>(code[end]))) continue;
        after = end;  // \b 通过
        break;
    }
    if (after == 0) return false;
    std::size_t index = after;
    while (index < code.size() && IsSpaceChar(code[index])) ++index;  // \s*
    // (?:signed|unsigned)?
    for (const std::string_view qualifier : {std::string_view("signed"),
                                             std::string_view("unsigned"),}) {
        if (code.compare(index, qualifier.size(), qualifier) == 0) {
            const std::size_t end = index + qualifier.size();
            if (end >= code.size() || !IsWordChar(static_cast<unsigned char>(code[end]))) {
                index = end;
                while (index < code.size() && IsSpaceChar(code[index])) ++index;
                break;
            }
        }
    }
    if (index < code.size() && code[index] == '[') {  // (?:\[[^\]]*\]\s*)?
        const std::size_t close = code.find(']', index + 1);
        if (close != std::string_view::npos) {
            index = close + 1;
            while (index < code.size() && IsSpaceChar(code[index])) ++index;
        }
    }
    if (index >= code.size() || !IsIdentStart(static_cast<unsigned char>(code[index]))) {
        return false;
    }
    const std::size_t begin = index;
    while (index < code.size() && IsWordChar(static_cast<unsigned char>(code[index]))) ++index;
    *name_begin = begin;
    *match_end = index;
    return true;
}

// literal_re: \d*'[sS]?[bdhoBDHO][0-9a-fA-FxXzZ_]+
bool MatchLiteral(std::string_view code, std::size_t at, std::size_t* match_end) {
    std::size_t index = at;
    while (index < code.size() && IsDigit(code[index])) ++index;  // \d*
    if (index >= code.size() || code[index] != '\'') return false;
    ++index;
    if (index < code.size() && (code[index] == 's' || code[index] == 'S')) ++index;
    if (index >= code.size()) return false;
    const char base = code[index];
    if (base != 'b' && base != 'd' && base != 'h' && base != 'o' &&
        base != 'B' && base != 'D' && base != 'H' && base != 'O') {
        return false;
    }
    ++index;
    const std::size_t digits_begin = index;
    while (index < code.size()) {
        const char c = code[index];
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                         (c >= 'A' && c <= 'F') || c == 'x' || c == 'X' || c == 'z' ||
                         c == 'Z' || c == '_';
        if (!hex) break;
        ++index;
    }
    if (index == digits_begin) return false;
    *match_end = index;
    return true;
}

// number_re: (?<![A-Za-z0-9_$'\]])\d+(?![A-Za-z0-9_$'])
bool PreviousAllowsNumber(char c) {
    return !IsWordChar(static_cast<unsigned char>(c)) && c != '\'' && c != ']';
}
bool NextAllowsNumber(char c) {
    return !IsWordChar(static_cast<unsigned char>(c)) && c != '\'';
}

// -----------------------------------------------------------------------------
//  Verible 原始词法流
// -----------------------------------------------------------------------------

struct RawToken {
    int type = 0;
    std::size_t begin = 0;
    std::size_t end = 0;
};

struct LexResult {
    std::vector<RawToken> tokens;
    // 词法器是否"吃完整段输入"（正常 EOF）。false = 中途遇到不前进的 token
    // （词法错误）；此时整篇词法流也一定在同一位置中断，所以区域增量必须回退，
    // 不能把后面的行当作"没受影响"。
    bool complete = true;
};

LexResult LexVerible(std::string_view text) {
    LexResult result;
    if (text.empty()) return result;
    verilog::VerilogLexer lexer(text);
    for (;;) {
        const verible::TokenInfo& token = lexer.DoNextToken();
        const int code = token.token_enum();
        if (code == 0) break;  // EOF
        const int left = token.left(text);
        const int right = token.right(text);
        if (left < 0 || right <= left) {  // 位置不前进（词法错误）就退出，绝不空转
            result.complete = false;
            break;
        }
        const std::string_view name =
            verilog::verilog_symbol_name(static_cast<std::size_t>(code));
        const int type = TokenTypeFromSymbolName(name);
        if (type < 0) continue;
        result.tokens.push_back(RawToken{.type=type, .begin=static_cast<std::size_t>(left),
                                         .end=static_cast<std::size_t>(right),});
    }
    return result;
}

// -----------------------------------------------------------------------------
//  合并（Python merge_tokens）
// -----------------------------------------------------------------------------
std::vector<SemToken> Merge(const std::vector<SemToken>& primary,
                            const std::vector<SemToken>& extra) {
    std::map<std::pair<int, int>, SemToken> merged;
    for (const SemToken& token : primary) merged[{token.line, token.start}] = token;
    for (const SemToken& token : extra) merged.emplace(std::make_pair(token.line, token.start), token);
    std::vector<SemToken> ordered;
    ordered.reserve(merged.size());
    for (const auto& [position, token] : merged) ordered.push_back(token);
    std::ranges::sort(ordered, [](const SemToken& a, const SemToken& b) {
        return a.line != b.line ? a.line < b.line : a.start < b.start;
    });
    std::vector<SemToken> result;
    result.reserve(ordered.size());
    int last_line = -1;
    int last_end = -1;
    for (const SemToken& token : ordered) {
        if (token.line == last_line && token.start < last_end) continue;
        result.push_back(token);
        last_line = token.line;
        last_end = token.start + token.length;
    }
    return result;
}

}  // namespace

// -----------------------------------------------------------------------------
//  legend / 名字映射
// -----------------------------------------------------------------------------

std::string_view TokenTypeName(int type) {
    switch (type) {
        case kKeyword: return "keyword";
        case kType: return "type";
        case kClass: return "class";
        case kProperty: return "property";
        case kVariable: return "variable";
        case kParameter: return "parameter";
        case kFunction: return "function";
        case kNumber: return "number";
        case kComment: return "comment";
        case kString: return "string";
        case kOperator: return "operator";
        case kMacro: return "macro";
        default: return "";
    }
}

int TokenTypeFromSymbolName(std::string_view kind) {
    // Python _SKIP_KINDS = ("<<", "end of file", "space", "newline", "define-tokens")
    const auto contains = [kind](std::string_view needle) {
        return kind.contains(needle);
    };
    if (contains("<<") || contains("end of file") || contains("space") ||
        contains("newline") || contains("define-tokens")) {
        return -1;
    }
    if (kind.size() >= 2 && kind.front() == '"') {
        const std::string_view inner = kind.substr(1, kind.size() - 2);
        if (inner.starts_with("//") || inner.starts_with("/&lowast;") ||
            inner.contains("comment")) {
            return kComment;
        }
        if (!inner.empty() && inner.front() == '`') return kKeyword;  // 预处理指令
        return kKeyword;                                             // 关键字 / 属性
    }
    if (contains("Comment")) return kComment;
    if (contains("String")) return kString;
    if (kind == "PP_Identifier" || kind == "MacroIdentifier" || kind == "MacroCallId") {
        return kMacro;
    }
    if (kind == "SystemTFIdentifier") return kFunction;
    if (kind == "SymbolIdentifier" || kind == "EscapedIdentifier") return kVariable;
    if (contains("Number") || contains("Digits") || contains("Base") ||
        contains("TimeLiteral")) {
        return kNumber;
    }
    if (!kind.empty() && kind.front() == '\'') return kOperator;  // ';' '(' '+' ...
    return -1;                                                   // TK_xxx / 其它
}

// -----------------------------------------------------------------------------
//  掩码（Python refs.code_mask，逐字节版本）
// -----------------------------------------------------------------------------

std::vector<char> CodeMask(std::string_view text) {
    std::vector<char> mask(text.size(), 0);
    (void)ScanMask(text, 0, text.size(), kMaskCode, mask.data());
    return mask;
}

// -----------------------------------------------------------------------------
//  primary：hdl 索引 + 行内正则
// -----------------------------------------------------------------------------

// 计算 [first, last) 行的 primary token（全量路径 = [0, rows.size())）。
// `mask` 是"代码掩码"，mask[0] 对应文本的 mask_offset 字节处；区域调用时只覆盖
// 区域字节即可（见 ScanMask 的逐行一致性说明）。
std::vector<SemToken> HdlSideRange(const hdl::HdlIndex* index, std::string_view path,
                                   const LineTable& lines, std::string_view mask,
                                   std::size_t mask_offset, std::size_t first,
                                   std::size_t last) {
    if (index == nullptr || lines.rows.empty() || first >= last) return {};
    if (last > lines.rows.size()) last = lines.rows.size();
    CandidateTable sink(lines, first, last);

    // a) 索引里的 module / port / instance
    //
    // ⚠ 索引可能比缓冲区**旧**：格式化会把整篇代码的行号改掉，而索引要等下一次
    //   重新解析才更新。此时若照抄索引里的 (line, character)，token 会落到错误的
    //   位置上 —— 用户看到的就是"格式化后语法高亮全乱，切走再切回来又好了"。
    //   这里逐条做**文本校验**：该位置上的实际文本必须等于索引里的名字，否则丢弃。
    //   丢弃是安全的：行内正则（b）仍会给关键字/类型/数字上色，只是少了 9/8/7 那三类。
    const auto position_matches = [&lines](int line, int character, std::string_view name) {
        if (line < 0 || static_cast<std::size_t>(line) >= lines.rows.size()) return false;
        const std::string_view row = lines.rows[static_cast<std::size_t>(line)];
        if (character < 0 || static_cast<std::size_t>(character) + name.size() > row.size()) {
            return false;
        }
        return row.substr(static_cast<std::size_t>(character), name.size()) == name;
    };
    if (const hdl::ParsedFile* parsed = index->File(path); parsed != nullptr) {
        for (const hdl::ModuleInfo& info : parsed->modules) {
            if (position_matches(info.module.line, info.module.character, info.module.name)) {
                sink.Add(info.module.line, info.module.character,
                         static_cast<int>(info.module.name.size()), kClass, 9);
            }
            for (const hdl::Port& port : info.module.ports) {
                if (position_matches(port.line, port.character, port.name)) {
                    sink.Add(port.line, port.character, static_cast<int>(port.name.size()),
                             kProperty, 8);
                }
            }
        }
    }
    for (const hdl::Instance* instance : index->InstancesOfFile(path)) {
        if (position_matches(instance->line, instance->character, instance->name)) {
            sink.Add(instance->line, instance->character,
                     static_cast<int>(instance->name.size()), kFunction, 7);
        }
    }

    // b) 行内正则（注释/字符串已掩码成空格）
    std::string code;
    for (std::size_t line = first; line < last; ++line) {
        const std::string_view row = lines.rows[line];
        const std::size_t base = lines.starts[line];
        code.assign(row);
        for (std::size_t i = 0; i < row.size(); ++i) {
            const std::size_t position = base + i - mask_offset;
            if (position >= mask.size() || mask[position] == 0) code[i] = ' ';
        }
        const std::string_view view(code);
        const int line_number = static_cast<int>(line);

        ScanWords(line_number, row, view, &sink);

        // parameter_re（注意：Python 的 finditer 语义 = 非重叠、从左到右）
        for (std::size_t i = 0; i < view.size();) {
            std::size_t name_begin = 0;
            std::size_t name_end = 0;
            std::size_t match_end = 0;
            if (MatchParameter(view, i, &name_begin, &name_end, &match_end)) {
                AddSpan(&sink, line_number, row, name_begin, name_end, kParameter, 6);
                i = match_end;
                continue;
            }
            ++i;
        }
        // declaration_re
        for (std::size_t i = 0; i < view.size();) {
            std::size_t name_begin = 0;
            std::size_t match_end = 0;
            if (MatchDeclaration(view, i, &name_begin, &match_end)) {
                AddSpan(&sink, line_number, row, name_begin, match_end, kVariable, 5);
                i = match_end;
                continue;
            }
            ++i;
        }
        // literal_re
        for (std::size_t i = 0; i < view.size();) {
            std::size_t match_end = 0;
            if (MatchLiteral(view, i, &match_end)) {
                AddSpan(&sink, line_number, row, i, match_end, kNumber, 4);
                i = match_end;
                continue;
            }
            ++i;
        }
        // number_re
        for (std::size_t i = 0; i < view.size();) {
            if (!IsDigit(view[i]) || (i > 0 && !PreviousAllowsNumber(view[i - 1]))) {
                ++i;
                continue;
            }
            std::size_t end = i;
            while (end < view.size() && IsDigit(view[end])) ++end;
            if (end < view.size() && !NextAllowsNumber(view[end])) {
                i = end;
                continue;
            }
            AddSpan(&sink, line_number, row, i, end, kNumber, 3);
            i = end;
        }
    }

    return sink.Finish();
}

// 公开 API：整篇 primary（= HdlSideRange 覆盖全部行）
std::vector<SemToken> HdlSideTokens(const hdl::HdlIndex* index, std::string_view path,
                                    std::string_view text) {
    if (index == nullptr || text.empty()) return {};
    const LineTable lines = BuildLines(text);
    const std::vector<char> mask = CodeMask(text);
    return HdlSideRange(index, path, lines,
                        std::string_view(mask.data(), mask.size()), 0, 0,
                        lines.rows.size());
}

// 计算 [first, last) 行的 extra token。词法只跑 `text` 的这段字节（区域增量），
// 再按**绝对**行号解码。`complete` 传出词法是否吃完整段输入（见 LexResult）。
std::vector<SemToken> VeribleSideRange(const hdl::HdlIndex* index, std::string_view path,
                                       const LineTable& lines, std::string_view text,
                                       std::size_t first, std::size_t last,
                                       bool* complete,
                                       std::vector<char>* inside_out = nullptr,
                                       bool* ends_at_region_end = nullptr) {
    std::vector<SemToken> decoded;
    if (complete != nullptr) *complete = true;
    if (text.empty() || first >= last) return decoded;
    if (last > lines.rows.size()) last = lines.rows.size();
    const std::size_t base = lines.starts[first];
    const std::size_t end = last < lines.rows.size() ? lines.starts[last] : text.size();
    if (end <= base) return decoded;

    const LexResult lexed = LexVerible(text.substr(base, end - base));
    if (complete != nullptr) *complete = lexed.complete;
    decoded.reserve(lexed.tokens.size());

    for (const RawToken& relative : lexed.tokens) {
        const std::size_t token_begin = base + relative.begin;
        const std::size_t token_end = base + relative.end;
        // 吃到区域末尾的 token = 跨行构造没有在本区域闭合（未闭合块注释等）-> 不确定
        if (ends_at_region_end != nullptr && relative.end == end - base) {
            *ends_at_region_end = true;
        }
        // first = 最后一个 start <= token.begin 的行
        std::size_t token_first = static_cast<std::size_t>(
            std::ranges::upper_bound(lines.starts, token_begin) -
            lines.starts.begin());
        if (token_first == 0) continue;
        --token_first;
        std::size_t token_last = static_cast<std::size_t>(
            std::ranges::upper_bound(lines.starts, token_end) -
            lines.starts.begin());
        if (token_last == 0) continue;
        --token_last;
        if (token_last < token_first) token_last = token_first;
        if (token_first >= lines.rows.size()) continue;
        // 跨行 token（块注释/未闭合字符串）把中间那些行标记为"行首就在 token 里"：
        // 这些行的 token 依赖于上一行的 token 起点，不能跨区域复用。
        if (inside_out != nullptr && token_last > token_first) {
            for (std::size_t line = token_first + 1;
                 line <= token_last && line < inside_out->size(); ++line) {
                (*inside_out)[line] = 1;
            }
        }

        const std::string_view first_row = lines.rows[token_first];
        const int start_column =
            ByteToColumn(first_row, token_begin - lines.starts[token_first]);
        if (token_first == token_last) {
            const int end_column =
                ByteToColumn(first_row, token_end - lines.starts[token_first]);
            if (end_column > start_column) {
                decoded.push_back(SemToken{.line=static_cast<int>(token_first),
                                           .start=start_column,
                                           .length=end_column - start_column,
                                           .type=relative.type,});
            }
            continue;
        }
        // 跨行 token（块注释）：每行一段
        const int first_length = RowWidth(first_row) - start_column;
        if (first_length > 0) {
            decoded.push_back(SemToken{.line=static_cast<int>(token_first),
                                       .start=start_column, .length=first_length,
                                       .type=relative.type,});
        }
        for (std::size_t line = token_first + 1;
             line < token_last && line < lines.rows.size(); ++line) {
            const int width = RowWidth(lines.rows[line]);
            if (width > 0) {
                decoded.push_back(SemToken{.line=static_cast<int>(line), .start=0,
                                           .length=width, .type=relative.type});
            }
        }
        if (token_last < lines.rows.size()) {
            const int end_column =
                ByteToColumn(lines.rows[token_last], token_end - lines.starts[token_last]);
            if (end_column > 0) {
                decoded.push_back(SemToken{.line=static_cast<int>(token_last), .start=0,
                                           .length=end_column, .type=relative.type});
            }
        }
    }

    // 标识符重分类（Python _reclassify_verible_identifiers）
    if (index != nullptr && !path.empty()) {
        for (SemToken& token : decoded) {
            if (token.type != kVariable) continue;
            const int probe = token.start + (token.length > 1 ? 1 : 0);
            const std::optional<hdl::SymbolAtResult> symbol =
                index->SymbolAt(path, token.line, probe);
            if (!symbol.has_value()) continue;
            const std::string& kind = symbol->kind;
            if (kind == "module") {
                token.type = kClass;
            } else if (kind == "port") {
                token.type = kProperty;
            } else if (kind == "instance") {
                token.type = kFunction;
            } else if (kind == "signal") {
                token.type = kVariable;
            } else if (kind == "parameter" || kind == "genvar") {
                token.type = kParameter;
            }
        }
    }
    return decoded;
}

// 公开 API：整篇 extra
std::vector<SemToken> VeribleSideTokens(const hdl::HdlIndex* index, std::string_view path,
                                        std::string_view text) {
    if (text.empty()) return {};
    const LineTable lines = BuildLines(text);
    return VeribleSideRange(index, path, lines, text, 0, lines.rows.size(), nullptr);
}

// -----------------------------------------------------------------------------
//  合并后的完整流水线
// -----------------------------------------------------------------------------

std::vector<SemToken> SemanticTokens(const hdl::HdlIndex* index, std::string_view path,
                                     std::string_view text) {
    if (index == nullptr || text.empty()) return {};
    std::vector<SemToken> primary = HdlSideTokens(index, path, text);
    // 与 Python 一致：只有 Verilog 文件才叠加 Verible 词法流（.adc/.sdc 只有 primary）
    std::vector<SemToken> extra;
    if (hdl::IsVerilogPath(path)) {
        extra = VeribleSideTokens(index, path, text);
    }
    return Merge(primary, extra);
}

// =============================================================================
//  P2-A：行级增量缓存
// =============================================================================
//  正确性主张：对任意文本 T，`SemanticTokensCached(...)` 的返回值与
//  `SemanticTokens(index, path, T)` 逐 token 相同。依据是流水线的**逐行可分离性**：
//    * primary：CandidateTable 的去重/去重叠只比较同一行（`line == last_line`）；
//    * extra：Verible 的块注释跨行段在解码时也是逐行产出的；
//    * Merge：同样只按 (line,start) 合并、只去同行重叠；
//    * 行内掩码/词法状态只在"块注释 / 字符串 / 宏续行"三种情形下跨行。
//  因此"未变化行"的 token 只依赖于：该行字节 + 行入口状态 + 索引(generation)。
//  增量只有在三个锚点条件全满足时才用，否则全量重算。
// =============================================================================
namespace {

// 索引内容指纹的载体（定义见下方 IndexSignature；行号可平移时逐条比较）
struct IndexRecord {
    int line = -1;  // -1 = 不含行号（全局模块/端口名记录）
    int character = 0;
    std::string payload;
};

struct IndexSignatureData {
    std::string flat;                  // 含行号的逐字段拼接：完全相等可跳过逐条比较
    std::vector<IndexRecord> records;  // 行号可平移时逐条比较
};

// 缓存条目（不可变快照；shared_ptr 在缓存与调用方之间共享）
struct CacheEntry {
    std::string path;
    const hdl::HdlIndex* index = nullptr;
    std::uint64_t generation = 0;
    // 索引**内容**指纹（逐字段精确比较，无哈希碰撞）。generation 每次 didChange 都会
    // +1（IndexService 重装快照），不能作为缓存键；这个指纹只变于"索引真的变了"。
    IndexSignatureData index_sig;
    std::shared_ptr<const std::string> text;
    std::vector<std::size_t> line_starts;
    std::vector<int> line_states;                    // 每行开始扫描时的掩码状态
    std::vector<char> verible_inside;                // 1 = 行首已在一个跨行 Verible token 里
    std::vector<std::vector<SemToken>> line_tokens;  // 每行最终 token（已合并/去重）
    std::shared_ptr<const std::vector<SemToken>> all;
    std::uint64_t stamp = 0;
};

constexpr std::size_t kMaxCacheEntries = 8;

// ⚠ 语义 token 缓存的全局状态**必须是函数内 static**：
//   命名空间级 std::map 的构造在静态初始化期，抛异常无法被捕获
//   （bugprone-throwing-static-initialization），且会引入静态初始化顺序问题。
struct CacheState {
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<const CacheEntry>> entries;
    std::uint64_t stamp = 0;
    SemanticTokenCacheStats stats;
};

CacheState& Cache() {
    static CacheState state;  // C++11 起线程安全的一次性初始化
    return state;
}

// 最后一个 start <= offset 的行号
std::size_t LineContaining(const std::vector<std::size_t>& starts, std::size_t offset) {
    const std::size_t index = static_cast<std::size_t>(
        std::ranges::upper_bound(starts, offset) - starts.begin());
    return index == 0 ? 0 : index - 1;
}

// 第一个 start >= offset 的下标（没有则 = starts.size()）
std::size_t LineAtOrAfter(const std::vector<std::size_t>& starts, std::size_t offset) {
    return static_cast<std::size_t>(
        std::ranges::lower_bound(starts, offset) - starts.begin());
}

// -----------------------------------------------------------------------------
//  索引内容指纹（**行号平移感知**）
// -----------------------------------------------------------------------------
// 语义 token 对索引的依赖只有两处：
//   * HdlSideRange 的 module/port/instance 候选（位置 + 名字）；
//   * VeribleSideRange 的标识符重分类（HdlIndex::SymbolAt 的 kind）。
// SymbolAt 的 kind 判定完全由这些字段决定（见 hdl_index.cpp:2202-2283）：
// 本文件的 modules/ports/signals/params/instances 位置与名字，加上全局模块名与
// 它们的端口名（ModuleByName / conn_port_pos 的解析目标）。
//
// 每条记录都带行号：这样"整行插入/删除"（行号整体 ±delta）可以逐条按映射比较，
// 而不是一有行号变化就整篇作废。不用哈希 —— 精确比较，不存在碰撞导致的错误复用。
void AppendInt(std::string* out, std::int64_t value) {
    out->append(reinterpret_cast<const char*>(&value), sizeof(value));
}
void AppendText(std::string* out, std::string_view text) {
    AppendInt(out, static_cast<std::int64_t>(text.size()));
    out->append(text.data(), text.size());
}

void PushRecord(IndexSignatureData* signature, int line, int character, std::string payload) {
    AppendInt(&signature->flat, line);
    AppendInt(&signature->flat, character);
    AppendText(&signature->flat, payload);
    signature->records.push_back(
        IndexRecord{.line=line, .character=character, .payload=std::move(payload)});
}

IndexSignatureData IndexSignature(const hdl::HdlIndex* index, std::string_view path) {
    IndexSignatureData signature;
    if (index == nullptr) return signature;
    const hdl::ParsedFile* file = index->File(path);
    if (file != nullptr) {
        signature.flat.reserve(8192);
        for (const hdl::ModuleInfo& info : file->modules) {
            PushRecord(&signature, info.module.line, info.module.character, info.module.name);
            for (const hdl::Port& port : info.module.ports) {
                std::string payload = "p";
                payload += port.name;
                payload += '\x1f';
                payload += port.direction;
                payload += '\x1f';
                payload += port.net;
                PushRecord(&signature, port.line, port.character, std::move(payload));
            }
            for (const hdl::Signal& signal : info.signals) {
                PushRecord(&signature, signal.line, signal.character, "s" + signal.name);
            }
            for (const hdl::Param& param : info.params) {
                PushRecord(&signature, param.line, param.character, "q" + param.name);
            }
        }
        for (const hdl::Instance& instance : file->instances) {
            std::string payload = "i" + instance.module;
            payload += '\x1f';
            payload += instance.name;
            for (const auto& [name, value] : instance.connections) {
                payload += '\x1e';
                payload += name;
            }
            PushRecord(&signature, instance.line, instance.character, std::move(payload));
            for (const auto& [position, name] : instance.connection_ports) {
                PushRecord(&signature, position.first, position.second, "c" + name);
            }
        }
    }
    // 全局：模块名 + 各自端口名（跨文件解析目标）
    std::vector<std::string> names = index->ModuleNames();
    std::ranges::sort(names);
    AppendInt(&signature.flat, -1000000);
    AppendInt(&signature.flat, static_cast<std::int64_t>(names.size()));
    for (const std::string& name : names) {
        std::string payload = "g" + name;
        const hdl::Module* module = index->ModuleByName(name);
        if (module != nullptr) {
            for (const hdl::Port& port : module->ports) {
                payload += '\x1e';
                payload += port.name;
            }
        }
        PushRecord(&signature, -1, 0, std::move(payload));
    }
    return signature;
}

// 索引记录是否等于"旧记录 + 行号整体平移"。
//   * 旧记录的行在 [anchor, old_end)（变化区）内：新位置不约束（这些行会重算）；
//   * 旧行 < anchor：行号不变；>= old_end：行号 +delta；
//   * 记录条数/顺序/payload 必须完全一致（多一个声明就回退）。
// 索引记录里"该位置应当出现的文本"（用于校验索引是否与当前文本同源）
std::string_view IndexRecordText(const IndexRecord& record) {
    const std::string& payload = record.payload;
    if (payload.empty()) return std::string_view();
    const std::size_t first_sep = payload.find('\x1f', 1);
    if (payload[0] == 'i') {
        if (first_sep == std::string::npos) return std::string_view();
        // payload = "i" + module + '\x1f' + name (+ '\x1e' + 连接名 ...)
        std::size_t stop = payload.find('\x1e', first_sep + 1);
        if (stop == std::string::npos) stop = payload.size();
        const std::string_view name(payload.data() + first_sep + 1, stop - first_sep - 1);
        if (!name.empty()) return name;
        // 无名例化：位置退化为模块名位置
        return std::string_view(payload).substr(1, first_sep - 1);
    }
    if (payload[0] == 'p' || payload[0] == 's' || payload[0] == 'q' || payload[0] == 'c') {
        if (first_sep == std::string::npos) return std::string_view(payload).substr(1);
        return std::string_view(payload).substr(1, first_sep - 1);
    }
    return std::string_view(payload);  // module 名
}

// **索引-文本同源校验**：索引里的每条声明记录都必须在当前文本的对应位置找到同名
// 文本。满足时，索引对这些行给出的 module/port/instance 候选与 `SymbolAt` 的 kind
// 就与该文本的一次全新解析一致 —— 行号平移才安全。
// 代价 O(索引记录数)（CorpusAlpha top.v ≈ 数百条，几十 µs）。
bool IndexRecordsMatchText(const std::vector<IndexRecord>& records, const LineTable& lines) {
    for (const IndexRecord& record : records) {
        if (record.line < 0) continue;
        const std::size_t line = static_cast<std::size_t>(record.line);
        if (line >= lines.rows.size()) return false;
        const std::string_view row = lines.rows[line];
        const std::string_view expected = IndexRecordText(record);
        if (expected.empty()) return false;
        const std::size_t column = static_cast<std::size_t>(record.character);
        if (column + expected.size() > row.size()) return false;
        if (row.substr(column, expected.size()) != expected) return false;
    }
    return true;
}

bool RecordsMatchUnderShift(const std::vector<IndexRecord>& before,
                            const std::vector<IndexRecord>& after, bool index_identical,
                            std::size_t anchor, std::size_t old_end, std::int64_t delta) {
    if (index_identical) return true;  // flat 完全相等（行号也相等）
    if (before.size() != after.size()) return false;
    for (std::size_t i = 0; i < before.size(); ++i) {
        const IndexRecord& a = before[i];
        const IndexRecord& b = after[i];
        if (a.payload != b.payload) return false;
        if (a.line < 0) {
            if (b.line >= 0) return false;
            continue;
        }
        const auto old_line = static_cast<std::size_t>(a.line);
        if (old_line >= anchor && old_line < old_end) continue;  // 变化区内：位置不约束
        const std::int64_t mapped = old_line < anchor ? a.line : a.line + delta;
        if (b.line != mapped || b.character != a.character) return false;
    }
    return true;
}

// 全量计算 + 建条目
std::shared_ptr<CacheEntry> BuildFullEntry(const hdl::HdlIndex* index, std::string_view path,
                                           const std::shared_ptr<const std::string>& text,
                                           std::uint64_t generation,
                                           const IndexSignatureData& index_signature) {
    auto entry = std::make_shared<CacheEntry>();
    entry->path = std::string(path);
    entry->index = index;
    entry->generation = generation;
    entry->index_sig = index_signature;
    entry->text = text;

    const std::string& source = *text;
    const LineTable lines = BuildLines(source);
    const std::size_t line_count = lines.rows.size();
    entry->line_starts = lines.starts;
    entry->line_states.assign(line_count, kMaskCode);
    entry->verible_inside.assign(line_count, 0);
    entry->line_tokens.resize(line_count);

    // 逐行扫掩码：与整篇一次 ScanMask 逐字节相同（见 ScanMask 注释）
    std::vector<char> mask(source.size(), 0);
    int state = kMaskCode;
    for (std::size_t line = 0; line < line_count; ++line) {
        entry->line_states[line] = state;
        const std::size_t begin = lines.starts[line];
        const std::size_t end = line + 1 < line_count ? lines.starts[line + 1] : source.size();
        state = ScanMask(source, begin, end, state, mask.data() + begin);
    }

    std::vector<SemToken> primary =
        HdlSideRange(index, path, lines, std::string_view(mask.data(), mask.size()), 0, 0,
                     line_count);
    std::vector<SemToken> extra;
    if (hdl::IsVerilogPath(path)) {
        extra = VeribleSideRange(index, path, lines, source, 0, line_count, nullptr,
                                &entry->verible_inside);
    }
    std::vector<SemToken> merged = Merge(primary, extra);
    for (const SemToken& token : merged) {
        if (token.line < 0 || static_cast<std::size_t>(token.line) >= line_count) continue;
        entry->line_tokens[static_cast<std::size_t>(token.line)].push_back(token);
    }
    entry->all = std::make_shared<const std::vector<SemToken>>(std::move(merged));
    return entry;
}

// 行级增量：成功返回新条目，任何"不确定"都返回 nullptr（调用方全量重算）。
std::shared_ptr<CacheEntry> TryIncremental(const CacheEntry& old, const hdl::HdlIndex* index,
                                           std::string_view path,
                                           const std::shared_ptr<const std::string>& text,
                                           std::uint64_t generation,
                                           const IndexSignatureData& index_signature,
                                           bool index_matches_text) {
    const std::string& old_text = *old.text;
    const std::string& new_text = *text;
    const std::size_t old_size = old_text.size();
    const std::size_t new_size = new_text.size();
    const std::size_t old_line_count = old.line_starts.size();
    if (old_size == 0 || new_size == 0 || old_line_count == 0) { return nullptr; }

    // 1) 字节级公共前缀 / 后缀（O(n) 比较，无哈希 -> 无碰撞风险）
    const std::size_t common = std::min(old_size, new_size);
    std::size_t prefix = 0;
    while (prefix < common && old_text[prefix] == new_text[prefix]) ++prefix;
    if (prefix == old_size && prefix == new_size) { return nullptr; }  // 完全没变
    std::size_t suffix = 0;
    while (suffix < common - prefix &&
           old_text[old_size - 1 - suffix] == new_text[new_size - 1 - suffix]) {
        ++suffix;
    }

    const LineTable new_lines = BuildLines(new_text);
    const std::size_t new_line_count = new_lines.rows.size();
    if (new_line_count == 0) { return nullptr; }
    // 行数**允许**变化：行号平移由下面的 old_end/new_end + delta 处理，
    // 索引侧的 module/port/signal/param/instance 记录按同一 delta 逐条校验
    // （RecordsMatchUnderShift）；平移映射不成立就回退全量。

    // 2) 变化区间的行边界
    std::size_t anchor = LineContaining(old.line_starts, std::min(prefix, old_size - 1));
    if (anchor >= old_line_count) anchor = old_line_count - 1;
    // 2a) 锚点必须是"干净行首"：不在块注释/字符串里，上一行不以 '\' 结尾（宏续行）
    //     也不以 '/' 结尾（后者只是保守防御：行扫描的 lookahead 不跨 '\n'）。
    while (anchor > 0) {
        const std::size_t old_prev_begin = old.line_starts[anchor - 1];
        const std::size_t old_prev_end =
            anchor < old_line_count ? old.line_starts[anchor] : old_size;
        const std::string_view old_prev =
            std::string_view(old_text).substr(old_prev_begin, old_prev_end - old_prev_begin);
        bool clean = old.line_states[anchor] == kMaskCode && !RowEndsWithSlash(old_prev) &&
                     !RowEndsWithBackslash(old_prev);
        if (clean) {
            // ⚠ **新文本也要看**：插入可能让"新文本里 anchor 的上一行"以 '\' 结尾
            //   （即 anchor 变成了宏体），而旧文本的上一行还是普通行 —— 只查旧侧会漏掉，
            //   区域就会从宏体中间开始，Verible 的词法与全量不同（多/少 comment token）。
            if (anchor >= new_line_count) {
                clean = false;
            } else {
                const std::size_t new_prev_begin = new_lines.starts[anchor - 1];
                const std::size_t new_prev_end =
                    anchor < new_line_count ? new_lines.starts[anchor] : new_size;
                const std::string_view new_prev = std::string_view(new_text).substr(
                    new_prev_begin, new_prev_end - new_prev_begin);
                clean = !RowEndsWithSlash(new_prev) && !RowEndsWithBackslash(new_prev);
            }
        }
        if (clean) break;
        --anchor;
    }

    std::size_t old_end = LineAtOrAfter(old.line_starts, old_size - suffix);
    std::size_t new_end = LineAtOrAfter(new_lines.starts, new_size - suffix);
    if (old_end > old_line_count) old_end = old_line_count;
    if (new_end > new_line_count) new_end = new_line_count;
    if (new_end < anchor) { return nullptr; }

    // 2b) 宏续行：区域最后一行以 '\' 结尾时，下一行仍属于宏体，必须一起重算。
    //     新旧两侧同步外扩（这些行内容逐字节相同）。
    while (new_end > 0 && new_end < new_line_count) {
        const std::size_t row_begin = new_lines.starts[new_end - 1];
        const std::size_t row_end = new_lines.starts[new_end];
        if (!RowEndsWithBackslash(new_text.substr(row_begin, row_end - row_begin))) break;
        ++new_end;
        ++old_end;
    }
    if (old_end > old_line_count) { return nullptr; }
    // 后缀行数必须对齐（字节相同 => 行数必然相同；不一致就说明推导出错，回退）
    if (new_line_count - new_end != old_line_count - old_end) { return nullptr; }

    // 2c) 宏体边界：区域的两端都不能落在"宏体"里。
    //     宏体是纯文本局部性质：某行在宏体里 <=> 上一行以 '\' 结尾（续行）。
    //     * 起点：锚点扩展循环已经保证"上一行不以 '\' 结尾"；
    //     * 终点：如果**旧**区域最后一行以 '\' 结尾，说明旧后缀的第一行属于宏体，
    //       而新区域最后一行不是 —— 后缀两边的词法状态不一致，回退。
    //       （新侧由上面的外扩循环保证：最后一行不以 '\' 结尾。）
    // 2c) 宏相关编辑一律回退（运行时判别，最后一道门禁）：
    //     区域（含锚点行与其上一行）里出现 "`define"，或任一行以 '\' 结尾（宏续行）
    //     ⇒ 宏体词法上下文会跨行/随编辑变化，逐行复用无法保证与全量等价。
    //     旧/新两侧都看：编辑可能让"原本普通的行"变成宏体的一部分。
    {
        const std::size_t first = anchor > 0 ? anchor - 1 : 0;
        const std::size_t last = std::max(new_end, old_end);
        for (std::size_t line = first; line < last; ++line) {
            if (line < new_line_count) {
                const std::size_t begin = new_lines.starts[line];
                const std::size_t end =
                    line + 1 < new_line_count ? new_lines.starts[line + 1] : new_size;
                const std::string_view row =
                    std::string_view(new_text).substr(begin, end - begin);
                if (row.find("`define") != std::string_view::npos ||
                    RowEndsWithBackslash(row)) {
                    return nullptr;
                }
            }
            if (line < old_line_count) {
                const std::size_t begin = old.line_starts[line];
                const std::size_t end =
                    line + 1 < old_line_count ? old.line_starts[line + 1] : old_size;
                const std::string_view row =
                    std::string_view(old_text).substr(begin, end - begin);
                if (row.find("`define") != std::string_view::npos ||
                    RowEndsWithBackslash(row)) {
                    return nullptr;
                }
            }
        }
    }
    if (old_end > 0 && old_end <= old_line_count) {
        const std::size_t last_old = old_end - 1;
        const std::size_t begin = old.line_starts[last_old];
        const std::size_t end =
            last_old + 1 < old_line_count ? old.line_starts[last_old + 1] : old_size;
        if (RowEndsWithBackslash(std::string_view(old_text).substr(begin, end - begin))) {
            return nullptr;
        }
    }

    // 行号平移量：新行 = 旧行 + delta（只对后缀成立；前缀是恒等映射）
    const std::int64_t delta =
        static_cast<std::int64_t>(new_end) - static_cast<std::int64_t>(old_end);

    // 2d) 行号平移（delta != 0）需要"索引-文本同源"契约：只有索引确实是按当前文本
    //     构建的（每条声明记录都能在当前文本对应位置找到同名文本），平移后的
    //     `SymbolAt` 才会与全量路径一致。调用方若能用更强的手段保证同源
    //     （例如服务端记录"索引构建时该文件的文本哈希"），可传
    //     index_matches_text=true 跳过这次 O(记录数) 校验。
    if (delta != 0 && !index_matches_text &&
        !IndexRecordsMatchText(index_signature.records, new_lines)) {
        return nullptr;
    }
    // ⚠ 行号平移仍**未启用**：本轮修好了"同源契约"（instance payload 解析 bug）与
    //   后缀 line_states 重算，7 类行数变化用例里 3 类（删除行/粘贴/块注释回车）已经
    //   等价且命中增量；但插入行 / 跨行删除 / `\` 后回车仍与全量不等价（多一个 token），
    //   根因是"某行在编辑后才变成宏体"这类宏体边界，`\` 字符串中间回车则全部回退。
    //   正确性优先：先保持全量，等宏体状态位做完再放开。
    // ⚠ 行号平移仍**未放开**：7 类行数变化用例中 4 类（删除行/粘贴多行/块注释回车/
    //   字符串回车）已证明与全量逐 token 等价且能命中增量；但插入行 / 跨行删除 /
    //   `\` 续行后回车仍有 1 个 token 的差异，卡在"复用行的宏体性质在旧/新两侧不同"
    //   （详见 task-49 报告）。正确性优先：先全量，等该门禁补完再按类别放开。
    // 3) 索引内容必须等于"旧索引 + 行号整体平移"：
    //    * 前缀里的声明：行号不变；后缀里的声明：行号 +delta；变化区里的不约束；
    //    * 记录条数/顺序/payload 必须完全一致（多一个端口就回退）。
    //    flat 完全相等时（连行号都相同）直接跳过逐条比较。
    const bool index_identical = old.index_sig.flat == index_signature.flat;
    if (!RecordsMatchUnderShift(old.index_sig.records, index_signature.records, index_identical,
                                anchor, old_end, delta)) {
        return nullptr;
    }

    // 3b) 宏续行（`define ... \`）：Verible 的宏体词法状态跨行，而且**编辑前不明显的
    //     行可能在编辑后变成宏体**（插到 '\' 之后），逐行复用无法保证等价。
    //     因此只要变化区"紧邻"任一以 '\' 结尾的行（旧/新两侧都算），就整篇回退。
    //     这是本轮唯一没能证明安全的形态，按"允许部分放开"保留回退。
    {
        const std::size_t first = anchor > 0 ? anchor - 1 : 0;
        const std::size_t last = std::max(new_end, old_end);
        for (std::size_t line = first; line < last && line < new_line_count; ++line) {
            const std::size_t begin = new_lines.starts[line];
            const std::size_t end =
                line + 1 < new_line_count ? new_lines.starts[line + 1] : new_size;
            if (RowEndsWithBackslash(std::string_view(new_text).substr(begin, end - begin))) {
                return nullptr;
            }
        }
        for (std::size_t line = first; line < last && line < old_line_count; ++line) {
            const std::size_t begin = old.line_starts[line];
            const std::size_t end =
                line + 1 < old_line_count ? old.line_starts[line + 1] : old_size;
            if (RowEndsWithBackslash(std::string_view(old_text).substr(begin, end - begin))) {
                return nullptr;
            }
        }
    }

    // ⚠ 行号平移（delta != 0）仍未放开：7 类用例里 4 类（删除行/粘贴多行/块注释回车/
    //   字符串回车）已证明逐 token 等价且能命中增量，另外 3 类（插入行/跨行删除/
    //   `\` 续行后回车）仍有 1 个 token 差异 —— 卡在"复用行的宏体/跨行词法状态在
    //   旧/新两侧不同"，宏邻接门禁没能完全覆盖（详见 task-49 报告）。
    //   正确性优先：行数变化整体回退全量；等该状态位补完再按类别放开。

    // ⚠ 行号平移（delta != 0）仍**不放开**：7 类里 4 类（删除行/粘贴多行/块注释回车/
    //   字符串回车）已证明逐 token 等价且能命中增量；插入行 / 跨行删除 / `\` 续行回车
    //   仍有 1 个 token 差异，根因是"区域起点处 Verible 的 `define 宏上下文"（区域
    //   子串从 `define 行开始重词法时，宏体续行语义与全量不同）。正确性优先。

    // 4) 规模上限：改动太大就直接全量，避免"增量"比重算还贵
    {
        const std::size_t region_lines_now = new_end - anchor;
        if (region_lines_now > std::max<std::size_t>(64, new_line_count / 2)) return nullptr;
    }

    // 5) 区域重算：区域终点必须回到"代码状态"，否则后缀的行入口状态不可复用。
    //    终点落在块注释/字符串里时，**沿该跨行构造把区域扩到闭合**（最多到 EOF），
    //    而不是直接回退 —— "字符串/块注释中间回车"就是这一类。
    const std::size_t region_begin = new_lines.starts[anchor];
    std::size_t region_stop =
        new_end < new_line_count ? new_lines.starts[new_end] : new_size;
    if (region_stop < region_begin) return nullptr;
    std::vector<char> mask(region_stop - region_begin, 0);
    std::vector<int> states;
    int state = kMaskCode;  // 每次循环开头都会从锚点重新取；这里只是给个确定的初值
    for (;;) {
        std::fill(mask.begin(), mask.end(), 0);
        states.clear();
        states.reserve(new_end - anchor);
        state = old.line_states[anchor];
        for (std::size_t line = anchor; line < new_end; ++line) {
            states.push_back(state);
            const std::size_t begin = new_lines.starts[line];
            const std::size_t end =
                line + 1 < new_line_count ? new_lines.starts[line + 1] : new_size;
            state = ScanMask(new_text, begin, end, state, mask.data() + (begin - region_begin));
        }
        if (new_end >= new_line_count || state == kMaskCode) break;
        // 未闭合：把下一行纳入区域后重扫（后缀两侧同步外扩；它们内容逐字节相同）
        ++new_end;
        ++old_end;
        if (old_end > old_line_count) return nullptr;
        region_stop = new_end < new_line_count ? new_lines.starts[new_end] : new_size;
        mask.assign(region_stop - region_begin, 0);
    }
    if (new_end < new_line_count) {
        if (old.line_states[old_end] != kMaskCode) return nullptr;  // 旧后缀不是干净行首
    }

    // 3b) 宏体（上一行以 '\' 结尾）在"锚点"和"后缀首行"两侧必须一致：
    //     * 锚点：anchor == 0 或 旧上一行不以 '\' 结尾（锚点扩展循环已保证）；
    //     * 后缀首行：新/旧两侧的 in_macro 必须相等 —— 否则新后缀会用旧文本的
    //       词法状态（Verilog 宏体里 `//`/标识符的 token 化与代码不同）。
    if (new_end < new_line_count) {
        const auto new_prev_begin = new_lines.starts[new_end - 1];
        const auto new_prev_end =
            new_end < new_line_count ? new_lines.starts[new_end] : new_size;
        const bool new_in_macro = RowEndsWithBackslash(
            std::string_view(new_text).substr(new_prev_begin, new_prev_end - new_prev_begin));
        bool old_in_macro = false;
        if (old_end > 0) {
            const auto old_prev_begin = old.line_starts[old_end - 1];
            const auto old_prev_end = old.line_starts[old_end];
            old_in_macro = RowEndsWithBackslash(
                std::string_view(old_text).substr(old_prev_begin, old_prev_end - old_prev_begin));
        }
        if (new_in_macro != old_in_macro) return nullptr;
    }


    std::vector<SemToken> primary =
        HdlSideRange(index, path, new_lines, std::string_view(mask.data(), mask.size()),
                     region_begin, anchor, new_end);
    std::vector<SemToken> extra;
    std::vector<char> region_inside(new_line_count, 0);
    if (hdl::IsVerilogPath(path)) {
        bool complete = true;
        bool reaches_end = false;
        extra = VeribleSideRange(index, path, new_lines, new_text, anchor, new_end, &complete,
                                 &region_inside, &reaches_end);
        if (!complete) { return nullptr; }  // 区域内有词法错误 -> 不确定，全量
        // 区域末尾的跨行构造没闭合（或区域起点已在一个跨行 token 里）-> 后缀不可复用
        if (new_end < new_line_count && reaches_end) { return nullptr; }
        if (old.verible_inside[anchor] != 0) { return nullptr; }
        if (new_end < new_line_count && old.verible_inside[old_end] != 0) { return nullptr; }
    }
    const std::vector<SemToken> merged = Merge(primary, extra);

    // 5a) 行对齐安全网：被复用的行必须**逐字节相同**。前后缀的字节 diff 在"插入内容
    //     与相邻行重复"时会产生歧义（同一段字节既可能算前缀也可能算后缀），导致行映射
    //     错位（实测：把上一行 `// inserted line` 的 token 复用到了新的 `define` 行）。
    //     这里按行逐字节核对映射，代价 O(文件字节)，只有几 µs。
    {
        auto row_of = [](std::string_view text, const std::vector<std::size_t>& starts,
                         std::size_t line, std::size_t line_count) -> std::string_view {
            const std::size_t begin = starts[line];
            const std::size_t end = line + 1 < line_count ? starts[line + 1] : text.size();
            return text.substr(begin, end - begin);
        };
        for (std::size_t line = 0; line < anchor; ++line) {
            if (row_of(old_text, old.line_starts, line, old_line_count) !=
                row_of(new_text, new_lines.starts, line, new_line_count)) {
                return nullptr;
            }
        }
        for (std::size_t line = new_end; line < new_line_count; ++line) {
            const std::ptrdiff_t old_line =
                static_cast<std::ptrdiff_t>(line) - static_cast<std::ptrdiff_t>(new_end) +
                static_cast<std::ptrdiff_t>(old_end);
            if (old_line < 0 || static_cast<std::size_t>(old_line) >= old_line_count) {
                return nullptr;
            }
            if (row_of(old_text, old.line_starts, static_cast<std::size_t>(old_line),
                       old_line_count) !=
                row_of(new_text, new_lines.starts, line, new_line_count)) {
                return nullptr;
            }
        }
    }

    // 5) 组装：前缀复用 + 区域重算 + 后缀复用（行号整体平移）
    auto entry = std::make_shared<CacheEntry>();
    entry->path = std::string(path);
    entry->index = index;
    entry->generation = generation;
    entry->index_sig = index_signature;
    entry->text = text;
    entry->line_starts = new_lines.starts;
    entry->line_states.assign(new_line_count, kMaskCode);
    entry->verible_inside.assign(new_line_count, 0);
    entry->line_tokens.resize(new_line_count);

    for (std::size_t line = 0; line < anchor; ++line) {
        entry->line_states[line] = old.line_states[line];
        entry->verible_inside[line] = old.verible_inside[line];
        entry->line_tokens[line] = old.line_tokens[line];
    }
    for (std::size_t line = anchor; line < new_end; ++line) {
        entry->line_states[line] = states[line - anchor];
        entry->verible_inside[line] = region_inside[line];
    }
    for (const SemToken& token : merged) {
        if (token.line < 0 || static_cast<std::size_t>(token.line) >= new_line_count) continue;
        entry->line_tokens[static_cast<std::size_t>(token.line)].push_back(token);
    }
    // 后缀行的入口状态**必须重算**，不能照搬旧值；并且要与旧状态**逐行比较**。
    // 「行入口词法状态」= 掩码状态(Code/Block/String) **+ in_macro（上一行以 '\' 结尾）**。
    // 两者正交：宏体里的 `//` 不是注释，但掩码状态仍然是 Code —— 只比掩码状态会漏掉
    // 这类差异（实测：插入行让某行从"普通注释行"变成"宏体行"，Verible 少一个 comment
    // token）。这里逐行比较新旧状态，不等就从该行起重扫（本实现选择整篇回退，正确优先）。
    {
        int suffix_state = state;
        for (std::size_t line = new_end; line < new_line_count; ++line) {
            const std::size_t begin = new_lines.starts[line];
            const std::size_t end =
                line + 1 < new_line_count ? new_lines.starts[line + 1] : new_size;
            entry->line_states[line] = suffix_state;
            suffix_state = ScanMask(new_text, begin, end, suffix_state, nullptr);

            const std::ptrdiff_t old_line =
                static_cast<std::ptrdiff_t>(line) - static_cast<std::ptrdiff_t>(new_end) +
                static_cast<std::ptrdiff_t>(old_end);
            if (old_line < 0 || static_cast<std::size_t>(old_line) >= old_line_count) {
                return nullptr;
            }
            const auto index_old = static_cast<std::size_t>(old_line);
            if (old.line_states[index_old] != entry->line_states[line]) return nullptr;
            // in_macro：新行看新文本的上一行；旧行看旧文本的上一行
            const bool new_in_macro =
                line > 0 && RowEndsWithBackslash(
                                std::string_view(new_text).substr(
                                    new_lines.starts[line - 1],
                                    new_lines.starts[line] - new_lines.starts[line - 1]));
            const bool old_in_macro =
                index_old > 0 && RowEndsWithBackslash(
                                     std::string_view(old_text).substr(
                                         old.line_starts[index_old - 1],
                                         old.line_starts[index_old] -
                                             old.line_starts[index_old - 1]));
            if (new_in_macro != old_in_macro) return nullptr;
        }
    }
    for (std::size_t line = new_end; line < new_line_count; ++line) {
        const std::ptrdiff_t old_line =
            static_cast<std::ptrdiff_t>(line) - static_cast<std::ptrdiff_t>(new_end) +
            static_cast<std::ptrdiff_t>(old_end);
        if (old_line < 0 || static_cast<std::size_t>(old_line) >= old_line_count) {
            return nullptr;
        }
        const auto index_old = static_cast<std::size_t>(old_line);
        entry->verible_inside[line] = old.verible_inside[index_old];
        entry->line_tokens[line] = old.line_tokens[index_old];
        // ⚠ 行号平移：桶里每条 token 的 line 字段还是旧行号，必须整体改写成新行号
        //   （start/length/type 与行内容一起不变）。
        if (line != index_old) {
            for (SemToken& token : entry->line_tokens[line]) {
                token.line = static_cast<int>(line);
            }
        }
    }

    std::size_t total = 0;
    for (const std::vector<SemToken>& bucket : entry->line_tokens) total += bucket.size();
    auto flat = std::make_shared<std::vector<SemToken>>();
    flat->reserve(total);
    for (const std::vector<SemToken>& bucket : entry->line_tokens) {
        flat->insert(flat->end(), bucket.begin(), bucket.end());
    }

    // 6) 安全网：整篇重扫每行入口状态（O(n) 字节扫描，无掩码写入），与拼装结果
    //    不一致就说明行级推导有漏（例如跨行构造的边界判断），直接回退全量。
    //    这是"正确性优先"的最后一道闸，代价只有一次字节扫描。
    {
        int scanned = kMaskCode;
        for (std::size_t line = 0; line < new_line_count; ++line) {
            if (scanned != entry->line_states[line]) return nullptr;
            const std::size_t begin = new_lines.starts[line];
            const std::size_t end =
                line + 1 < new_line_count ? new_lines.starts[line + 1] : new_size;
            scanned = ScanMask(new_text, begin, end, scanned, nullptr);
        }
    }

    entry->all = flat;
    return entry;
}

}  // namespace

std::shared_ptr<const std::vector<SemToken>> SemanticTokensCached(
    const hdl::HdlIndex* index, std::string_view path,
    const std::shared_ptr<const std::string>& text, std::uint64_t index_generation,
    bool index_matches_text) {
    if (index == nullptr || !text || text->empty()) {
        return std::make_shared<const std::vector<SemToken>>();
    }
    const std::string key(path);
    // ⚠ 缓存键**不是** index 指针、也不是 generation：每次 didChange 后
    //   IndexService 都会重装一份索引快照（generation +1），拿 generation 当键等于
    //   永远不命中。这里改用"索引内容指纹"（IndexSignature）：只有索引的语义内容
    //   真的变了才失效；行号整体平移不算"变了"（见 RecordsMatchUnderShift）。
    const IndexSignatureData signature = IndexSignature(index, path);

    std::shared_ptr<const CacheEntry> previous;
    {
        CacheState& cache = Cache();
        std::scoped_lock lock(cache.mutex);
        const auto it = cache.entries.find(key);
        if (it != cache.entries.end()) previous = it->second;
    }

    const bool same_index = previous && previous->index_sig.flat == signature.flat;
    const bool same_text =
        previous && (previous->text == text || *previous->text == *text);
    if (same_index && same_text) {
        CacheState& cache = Cache();
        std::scoped_lock lock(cache.mutex);
        ++cache.stats.unchanged;
        return previous->all;
    }

    std::shared_ptr<CacheEntry> next;
    bool index_changed = false;
    if (previous) {
        // 索引内容不同时也可能只是"整行平移"：交给 TryIncremental 里的
        // RecordsMatchUnderShift 判定，不成立才回退全量。
        next = TryIncremental(*previous, index, path, text, index_generation, signature,
                              index_matches_text);
        index_changed = next == nullptr;
    }
    const bool incremental = next != nullptr;
    if (!next) {
        next = BuildFullEntry(index, path, text, index_generation, signature);
    }

    {
        CacheState& cache = Cache();
        std::scoped_lock lock(cache.mutex);
        next->stamp = ++cache.stamp;
        cache.entries[key] = next;
        if (cache.entries.size() > kMaxCacheEntries) {
            auto oldest = cache.entries.begin();
            for (auto it = cache.entries.begin(); it != cache.entries.end(); ++it) {
                if (it->second->stamp < oldest->second->stamp) oldest = it;
            }
            if (oldest->first != key) cache.entries.erase(oldest);
        }
        if (incremental) {
            ++cache.stats.incremental;
        } else {
            ++cache.stats.full;
            if (index_changed) ++cache.stats.full_index_changed;
        }
    }
    return next->all;
}

std::shared_ptr<const std::vector<SemToken>> SemanticTokensCached(
    const hdl::HdlIndex* index, std::string_view path, std::string_view text,
    std::uint64_t index_generation, bool index_matches_text) {
    return SemanticTokensCached(index, path,
                                std::make_shared<const std::string>(text), index_generation,
                                index_matches_text);
}

SemanticTokenCacheStats GetSemanticTokenCacheStats() {
    CacheState& cache = Cache();
    std::scoped_lock lock(cache.mutex);
    return cache.stats;
}

void ResetSemanticTokenCache() {
    CacheState& cache = Cache();
    std::scoped_lock lock(cache.mutex);
    cache.entries.clear();
    cache.stats = SemanticTokenCacheStats{};
}

std::vector<int> EncodeTokens(const std::vector<SemToken>& tokens) {
    std::vector<int> data;
    data.reserve(tokens.size() * 5);
    int previous_line = 0;
    int previous_start = 0;
    for (const SemToken& token : tokens) {
        const int delta_line = token.line - previous_line;
        const int delta_start = delta_line == 0 ? token.start - previous_start : token.start;
        data.push_back(delta_line);
        data.push_back(delta_start);
        data.push_back(token.length);
        data.push_back(token.type);
        data.push_back(0);
        previous_line = token.line;
        previous_start = token.start;
    }
    return data;
}

TokenStats Summarize(const std::vector<SemToken>& tokens) {
    TokenStats stats;
    stats.total = static_cast<int>(tokens.size());
    for (const SemToken& token : tokens) {
        ++stats.counts[std::string(TokenTypeName(token.type))];
    }
    return stats;
}

// -----------------------------------------------------------------------------
//  折叠范围（Python folding_ranges，逐行逐句照搬）
// -----------------------------------------------------------------------------

std::vector<FoldingRange> FoldingRanges(std::string_view text) {
    const LineTable lines = BuildLines(text);
    std::vector<FoldingRange> ranges;
    std::vector<int> stack;  // 只压行号（Python 还压了关键字，但只用于配对）

    int comment_start = -1;
    for (std::size_t line = 0; line < lines.rows.size(); ++line) {
        const std::string_view row = lines.rows[line];
        const int line_number = static_cast<int>(line);
        if (comment_start >= 0) {
            if (row.contains("*/")) {
                ranges.push_back(FoldingRange{.start_line=comment_start, .end_line=line_number, .comment=true});
                comment_start = -1;
            }
            continue;
        }
        const std::size_t open_block = row.find("/*");
        if (open_block != std::string_view::npos &&
            !row.substr(open_block + 2).contains("*/")) {
            comment_start = line_number;
            continue;
        }
        // code = row.split("//", 1)[0]
        std::string_view code = row;
        const std::size_t line_comment = row.find("//");
        if (line_comment != std::string_view::npos) code = row.substr(0, line_comment);

        // re.finditer(r"\b(open|close)\b", code) —— 按整词扫描
        std::size_t index = 0;
        while (index < code.size()) {
            if (!IsWordChar(static_cast<unsigned char>(code[index]))) {
                ++index;
                continue;
            }
            const std::size_t begin = index;
            while (index < code.size() && IsWordChar(static_cast<unsigned char>(code[index]))) {
                ++index;
            }
            const std::string_view word = code.substr(begin, index - begin);
            if (InSet(kOpenWords, word)) {
                stack.push_back(line_number);
            } else if (InSet(kCloseWords, word) && !stack.empty()) {
                const int start = stack.back();
                stack.pop_back();
                if (line_number > start) {
                    ranges.push_back(FoldingRange{.start_line=start, .end_line=line_number, .comment=false});
                }
            }
        }
    }
    if (comment_start >= 0 && comment_start < static_cast<int>(lines.rows.size()) - 1) {
        ranges.push_back(FoldingRange{.start_line=comment_start,
                                      .end_line=static_cast<int>(lines.rows.size()) - 1, .comment=true,});
    }

    // 连续 `//` 行块
    int block_start = -1;
    for (std::size_t line = 0; line < lines.rows.size(); ++line) {
        const std::string_view row = lines.rows[line];
        const int line_number = static_cast<int>(line);
        // Python: row.strip()（去首尾空白，含 \r）
        std::size_t begin = 0;
        std::size_t end = row.size();
        while (begin < end && IsSpaceChar(row[begin])) ++begin;
        while (end > begin && IsSpaceChar(row[end - 1])) --end;
        const std::string_view stripped = row.substr(begin, end - begin);
        if (stripped.starts_with("//")) {
            if (block_start < 0) block_start = line_number;
        } else {
            if (block_start >= 0 && line_number - 1 > block_start) {
                ranges.push_back(FoldingRange{.start_line=block_start, .end_line=line_number - 1, .comment=true});
            }
            block_start = -1;
        }
    }
    if (block_start >= 0 && static_cast<int>(lines.rows.size()) - 1 > block_start) {
        ranges.push_back(FoldingRange{.start_line=block_start,
                                      .end_line=static_cast<int>(lines.rows.size()) - 1, .comment=true,});
    }
    return ranges;
}

}  // namespace fpgalsp::semantics