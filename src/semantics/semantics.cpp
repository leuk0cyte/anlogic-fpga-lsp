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
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "platform/utf16.h"
#include "verible/common/text/token-info.h"
#include "verible/verilog/parser/verilog-lexer.h"
#include "verible/verilog/parser/verilog-parser.h"  // verilog_symbol_name()

namespace fpgalsp::semantics {
namespace {

// -----------------------------------------------------------------------------
//  Python 侧的三张词表（逐项照抄，顺序无关但内容不能改）
// -----------------------------------------------------------------------------

bool InSet(const std::string_view* table, std::size_t count, std::string_view word) {
    return std::find(table, table + count, word) != table + count;
}

constexpr std::string_view kTypeWords[] = {
    "wire", "reg", "logic", "integer", "signed", "unsigned",
    "tri", "genvar", "real", "time", "supply0", "supply1",
};
constexpr std::size_t kTypeWordCount = std::size(kTypeWords);

constexpr std::string_view kKeywords[] = {
    "module", "endmodule", "input", "output", "inout", "wire", "reg", "logic", "assign",
    "always", "always_ff", "always_comb", "always_latch", "begin", "end", "if", "else",
    "case", "casez", "casex", "endcase", "default", "for", "while", "repeat", "forever",
    "posedge", "negedge", "parameter", "localparam", "generate", "endgenerate",
    "function", "endfunction", "task", "endtask", "initial", "defparam",
    "specify", "endspecify", "and", "or", "nand", "nor", "xor", "xnor", "not", "buf",
};
constexpr std::size_t kKeywordCount = std::size(kKeywords);

constexpr std::string_view kOpenWords[] = {
    "module", "begin", "case", "casez", "casex", "generate", "function", "task", "fork",
};
constexpr std::size_t kOpenWordCount = std::size(kOpenWords);

constexpr std::string_view kCloseWords[] = {
    "endmodule", "end", "endcase", "endfunction", "endtask", "endgenerate", "join",
};
constexpr std::size_t kCloseWordCount = std::size(kCloseWords);

// declaration_re 的关键字顺序（Python 交替式的书写顺序）
constexpr std::string_view kDeclKeywords[] = {
    "input", "output", "inout", "wire", "reg", "logic", "tri", "integer",
    "genvar", "real", "time", "parameter", "localparam",
};
constexpr std::size_t kDeclKeywordCount = std::size(kDeclKeywords);

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

    void Add(int line, int start, int length, int type, int priority) {
        if (length <= 0 || start < 0) return;
        if (line < 0 || static_cast<std::size_t>(line) >= lines_.rows.size()) return;
        const int width = RowWidth(lines_.rows[static_cast<std::size_t>(line)]);
        if (start + length > width) return;
        const auto key = std::make_pair(line, start);
        const auto it = found_.find(key);
        if (it != found_.end() && it->second.priority >= priority) return;  // 同优先级先到先得
        found_[key] = Candidate{length, type, priority};
    }

    // 排序 + 去同行重叠（Python 的 ordered / last_line,last_end 逻辑）
    std::vector<SemToken> Finish() const {
        std::vector<SemToken> ordered;
        ordered.reserve(found_.size());
        for (const auto& [position, candidate] : found_) {
            ordered.push_back(SemToken{position.first, position.second,
                                       candidate.length, candidate.type});
        }
        std::sort(ordered.begin(), ordered.end(), [](const SemToken& a, const SemToken& b) {
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
        if (InSet(kTypeWords, kTypeWordCount, word)) {
            AddSpan(sink, line, row, begin, index, kType, 6);
        } else if (InSet(kKeywords, kKeywordCount, word)) {
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
    for (std::size_t i = 0; i < kDeclKeywordCount; ++i) {
        const std::string_view keyword = kDeclKeywords[i];
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
                                             std::string_view("unsigned")}) {
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
    return !(IsWordChar(static_cast<unsigned char>(c)) || c == '\'' || c == ']');
}
bool NextAllowsNumber(char c) {
    return !(IsWordChar(static_cast<unsigned char>(c)) || c == '\'');
}

// -----------------------------------------------------------------------------
//  Verible 原始词法流
// -----------------------------------------------------------------------------

struct RawToken {
    int type = 0;
    std::size_t begin = 0;
    std::size_t end = 0;
};

std::vector<RawToken> LexVerible(std::string_view text) {
    std::vector<RawToken> tokens;
    if (text.empty()) return tokens;
    verilog::VerilogLexer lexer(text);
    for (;;) {
        const verible::TokenInfo& token = lexer.DoNextToken();
        const int code = token.token_enum();
        if (code == 0) break;  // EOF
        const int left = token.left(text);
        const int right = token.right(text);
        if (left < 0 || right <= left) break;  // 位置不前进（词法错误）就退出，绝不空转
        const std::string_view name =
            verilog::verilog_symbol_name(static_cast<std::size_t>(code));
        const int type = TokenTypeFromSymbolName(name);
        if (type < 0) continue;
        tokens.push_back(RawToken{type, static_cast<std::size_t>(left),
                                  static_cast<std::size_t>(right)});
    }
    return tokens;
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
    std::sort(ordered.begin(), ordered.end(), [](const SemToken& a, const SemToken& b) {
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
        return kind.find(needle) != std::string_view::npos;
    };
    if (contains("<<") || contains("end of file") || contains("space") ||
        contains("newline") || contains("define-tokens")) {
        return -1;
    }
    if (kind.size() >= 2 && kind.front() == '"') {
        const std::string_view inner = kind.substr(1, kind.size() - 2);
        if (inner.rfind("//", 0) == 0 || inner.rfind("/&lowast;", 0) == 0 ||
            inner.find("comment") != std::string_view::npos) {
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
    enum class State { kCode, kLine, kBlock, kString };
    State state = State::kCode;
    const std::size_t size = text.size();
    std::size_t index = 0;
    const auto at = [&](std::size_t position) -> char {
        return position < size ? text[position] : '\0';
    };
    while (index < size) {
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
                mask[index] = 1;
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
    return mask;
}

// -----------------------------------------------------------------------------
//  primary：hdl 索引 + 行内正则
// -----------------------------------------------------------------------------

std::vector<SemToken> HdlSideTokens(const hdl::HdlIndex* index, std::string_view path,
                                    std::string_view text) {
    if (index == nullptr || text.empty()) return {};

    const LineTable lines = BuildLines(text);
    CandidateTable sink(lines);

    // a) 索引里的 module / port / instance
    if (const hdl::ParsedFile* parsed = index->File(path); parsed != nullptr) {
        for (const hdl::ModuleInfo& info : parsed->modules) {
            sink.Add(info.module.line, info.module.character,
                     static_cast<int>(info.module.name.size()), kClass, 9);
            for (const hdl::Port& port : info.module.ports) {
                sink.Add(port.line, port.character, static_cast<int>(port.name.size()),
                         kProperty, 8);
            }
        }
    }
    for (const hdl::Instance* instance : index->InstancesOfFile(path)) {
        sink.Add(instance->line, instance->character,
                 static_cast<int>(instance->name.size()), kFunction, 7);
    }

    // b) 行内正则（注释/字符串已掩码成空格）
    const std::vector<char> mask = CodeMask(text);
    std::string code;
    for (std::size_t line = 0; line < lines.rows.size(); ++line) {
        const std::string_view row = lines.rows[line];
        const std::size_t base = lines.starts[line];
        code.assign(row);
        for (std::size_t i = 0; i < row.size(); ++i) {
            if (mask[base + i] == 0) code[i] = ' ';
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

// -----------------------------------------------------------------------------
//  extra：Verible 原始词法流（Python verible_tokens_to_semantic）
// -----------------------------------------------------------------------------

std::vector<SemToken> VeribleSideTokens(const hdl::HdlIndex* index, std::string_view path,
                                        std::string_view text) {
    std::vector<SemToken> decoded;
    if (text.empty()) return decoded;

    const LineTable lines = BuildLines(text);
    const std::vector<RawToken> raw = LexVerible(text);
    decoded.reserve(raw.size());

    for (const RawToken& token : raw) {
        // first = 最后一个 start <= token.begin 的行
        std::size_t first = static_cast<std::size_t>(
            std::upper_bound(lines.starts.begin(), lines.starts.end(), token.begin) -
            lines.starts.begin());
        if (first == 0) continue;
        --first;
        std::size_t last = static_cast<std::size_t>(
            std::upper_bound(lines.starts.begin(), lines.starts.end(), token.end) -
            lines.starts.begin());
        if (last == 0) continue;
        --last;
        if (last < first) last = first;
        if (first >= lines.rows.size()) continue;

        const std::string_view first_row = lines.rows[first];
        const int start_column =
            ByteToColumn(first_row, token.begin - lines.starts[first]);
        if (first == last) {
            const int end_column = ByteToColumn(first_row, token.end - lines.starts[first]);
            if (end_column > start_column) {
                decoded.push_back(SemToken{static_cast<int>(first), start_column,
                                           end_column - start_column, token.type});
            }
            continue;
        }
        // 跨行 token（块注释）：每行一段
        const int first_length = RowWidth(first_row) - start_column;
        if (first_length > 0) {
            decoded.push_back(SemToken{static_cast<int>(first), start_column, first_length,
                                       token.type});
        }
        for (std::size_t line = first + 1; line < last && line < lines.rows.size(); ++line) {
            const int width = RowWidth(lines.rows[line]);
            if (width > 0) {
                decoded.push_back(SemToken{static_cast<int>(line), 0, width, token.type});
            }
        }
        if (last < lines.rows.size()) {
            const int end_column = ByteToColumn(lines.rows[last], token.end - lines.starts[last]);
            if (end_column > 0) {
                decoded.push_back(SemToken{static_cast<int>(last), 0, end_column, token.type});
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
            if (row.find("*/") != std::string_view::npos) {
                ranges.push_back(FoldingRange{comment_start, line_number, true});
                comment_start = -1;
            }
            continue;
        }
        const std::size_t open_block = row.find("/*");
        if (open_block != std::string_view::npos &&
            row.substr(open_block + 2).find("*/") == std::string_view::npos) {
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
            if (InSet(kOpenWords, kOpenWordCount, word)) {
                stack.push_back(line_number);
            } else if (InSet(kCloseWords, kCloseWordCount, word) && !stack.empty()) {
                const int start = stack.back();
                stack.pop_back();
                if (line_number > start) {
                    ranges.push_back(FoldingRange{start, line_number, false});
                }
            }
        }
    }
    if (comment_start >= 0 && comment_start < static_cast<int>(lines.rows.size()) - 1) {
        ranges.push_back(FoldingRange{comment_start,
                                      static_cast<int>(lines.rows.size()) - 1, true});
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
        if (stripped.rfind("//", 0) == 0) {
            if (block_start < 0) block_start = line_number;
        } else {
            if (block_start >= 0 && line_number - 1 > block_start) {
                ranges.push_back(FoldingRange{block_start, line_number - 1, true});
            }
            block_start = -1;
        }
    }
    if (block_start >= 0 && static_cast<int>(lines.rows.size()) - 1 > block_start) {
        ranges.push_back(FoldingRange{block_start,
                                      static_cast<int>(lines.rows.size()) - 1, true});
    }
    return ranges;
}

}  // namespace fpgalsp::semantics