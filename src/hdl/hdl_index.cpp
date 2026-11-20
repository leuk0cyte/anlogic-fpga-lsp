// =============================================================================
//  src/hdl/hdl_index.cpp —— HDL 索引实现
// =============================================================================
//  参考实现：Y:\fpga-lsp\fpgalsp\hdl.py
//    tokenizer            -> 换成 Verible（见 hdl_index.h 的取舍说明）
//    _match_pair/_split_top/_skip_* -> 逐行移植
//    _parse_range/_eval_tokens       -> 逐行移植（含 $clog2）
//    _parse_param_list/_decl/_direction_decl/_signal_decl/_port_list -> 逐行移植
//    _try_instance / _parse_module / _BodyScanner                   -> 逐行移植
//    HdlIndex.{document_symbols,symbol_at,definition_of}            -> 移植 + 改为结构化返回
//
//  ⚠ 移植时最容易出错的三处（已踩）：
//    1. `t in "([{"` 是**字符串成员**语义：只有单字符 token 才算括号
//    2. 端口名的"上一方向继承"（ANSI 里 `input a, b, output c`）—— last_dir 规则
//    3. 非 ANSI 的 header 名字只是 alias，方向/位宽来自 body 的 input/output 声明
// =============================================================================

#include "hdl/hdl_index.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <re2/re2.h>

#include "platform/path.h"
#include "platform/utf16.h"
#include "td/td_diagnostics.h"  // td::IpGeneratedHdl（.ipc -> 同名 HDL，P-1 的源头）

#include "verible/common/text/text-structure.h"
#include "verible/common/text/token-info.h"
#include "verible/common/text/token-stream-view.h"
#include "verible/verilog/analysis/verilog-analyzer.h"
#include "verible/verilog/preprocessor/verilog-preprocess.h"

namespace fpgalsp::hdl {
namespace {

// -----------------------------------------------------------------------------
// 词表（逐字抄 hdl.py 的 constants）
// -----------------------------------------------------------------------------

const std::set<std::string>& Directions() {
    static const std::set<std::string> v = {"input", "output", "inout", "ref"};
    return v;
}

const std::set<std::string>& NetTypes() {
    static const std::set<std::string> v = {
        "wire",  "reg",     "logic", "tri",     "tri0",    "tri1",   "triand",
        "trior", "wand",    "wor",   "supply0", "supply1", "uwire",  "var"};
    return v;
}

const std::set<std::string>& DataTypeWords() {
    static const std::set<std::string> v = {
        "integer", "int",    "real", "realtime", "time",   "bit",     "byte",
        "shortint", "longint", "string", "event", "genvar", "logic",  "reg",
        "wire"};
    return v;
}

const std::set<std::string>& TypeMods() {
    static const std::set<std::string> v = {"signed", "unsigned", "automatic", "static",
                                            "const"};
    return v;
}

const std::set<std::string>& DeclKeywords() {
    static const std::set<std::string> v = [] {
        std::set<std::string> out = NetTypes();
        for (const char* extra :
             {"integer", "int", "real", "realtime", "time", "bit", "byte", "shortint",
              "longint", "string", "event", "genvar"}) {
            out.insert(extra);
        }
        return out;
    }();
    return v;
}

const std::set<std::string>& Primitives() {
    static const std::set<std::string> v = {
        "and",    "nand",     "or",      "nor",     "xor",       "xnor",    "buf",
        "not",    "bufif0",   "bufif1",  "notif0",  "notif1",    "nmos",    "pmos",
        "cmos",   "rnmos",    "rpmos",   "rcmos",   "tran",      "tranif0", "tranif1",
        "rtran",  "rtranif0", "rtranif1", "pulldown", "pullup"};
    return v;
}

const std::set<std::string>& StmtPrev() {
    static const std::set<std::string> v = {
        ";",  "begin", "end",      "endgenerate", "generate",  "endcase",
        "endmodule",  "endfunction", "endtask", "else",     ":",         ")",
        "endcasex",   "endcasez"};
    return v;
}

const std::set<std::string>& NonModule() {
    static const std::set<std::string> v = {
        "module",       "endmodule",   "macromodule",  "interface",     "endinterface",
        "program",      "endprogram",  "package",      "endpackage",    "class",
        "endclass",     "function",    "endfunction",  "task",          "endtask",
        "begin",        "end",         "fork",         "join",          "join_any",
        "join_none",    "generate",    "endgenerate",  "always",        "always_comb",
        "always_ff",    "always_latch", "initial",     "final",         "assign",
        "deassign",     "force",       "release",      "defparam",      "specparam",
        "if",           "else",        "case",         "casex",         "casez",
        "endcase",      "default",     "for",          "while",         "repeat",
        "forever",      "do",          "return",       "break",         "continue",
        "wait",         "disable",     "event",        "posedge",       "negedge",
        "edge",         "input",       "output",       "inout",         "ref",
        "wire",         "reg",         "logic",        "tri",           "tri0",
        "tri1",         "triand",      "trior",        "wand",          "wor",
        "supply0",      "supply1",     "uwire",        "signed",        "unsigned",
        "var",          "integer",     "real",         "realtime",      "time",
        "genvar",       "parameter",   "localparam",   "bit",           "byte",
        "shortint",     "int",         "longint",      "string",        "chandle",
        "virtual",      "typedef",     "enum",         "struct",        "union",
        "packed",       "import",      "export",       "extern",        "static",
        "automatic",    "local",       "protected",    "pure",          "const",
        "let",          "assert",      "assume",       "cover",         "expect",
        "property",     "endproperty", "sequence",     "endsequence",   "covergroup",
        "endgroup",     "rand",        "randc",        "constraint",    "new",
        "super",        "this",        "null",         "void",          "type",
        "alias",        "bind",        "config",       "endconfig",     "cell",
        "endcell",      "design",      "instance",     "liblist",       "library",
        "use",          "table",       "endtable",     "primitive",     "endprimitive",
        "specify",      "endspecify",  "pulsestyle_onevent", "pulsestyle_ondetect",
        "showcancelled", "noshowcancelled", "scalared", "vectored",     "strong0",
        "strong1",      "pull0",       "pull1",        "weak0",         "weak1",
        "highz0",       "highz1",      "small",        "medium",        "large",
        "throughout",   "within",      "intersect",    "first_match",   "iff",
        "matches",      "dist",        "inside",       "unique",        "unique0",
        "priority",     "randcase",    "randsequence", "tagged",        "with",
        "coverpoint",   "bins",        "illegal_bins", "ignore_bins",   "modport",
        "clocking",     "endclocking", "global",       "timeunit",      "timeprecision",
        "nettype",      "interconnect", "wait_order",  "critical",      "suspend",
        "resume",       "assume_guarantee", "restrict", "endchecker",   "checker"};
    return v;
}

// -----------------------------------------------------------------------------
// 小工具
// -----------------------------------------------------------------------------

std::string ToLower(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// 对应 Python _key：escaped identifier 去掉开头的反斜杠并去掉尾部空白
std::string IdentKey(std::string_view name) {
    if (!name.empty() && name.front() == '\\') {
        std::string out(name.substr(1));
        while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) {
            out.pop_back();
        }
        return out;
    }
    return std::string(name);
}

bool IsSingleCharIn(std::string_view token, std::string_view chars) {
    return token.size() == 1 && chars.find(token[0]) != std::string_view::npos;
}

bool IsOpenBracket(std::string_view t) { return IsSingleCharIn(t, "([{"); }
bool IsCloseBracket(std::string_view t) { return IsSingleCharIn(t, ")]}"); }

// 行起始字节偏移表
class LineStarts {
public:
    explicit LineStarts(std::string_view text) {
        starts_.push_back(0);
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\n') starts_.push_back(i + 1);
        }
    }

    std::size_t size() const { return starts_.size(); }
    std::size_t at(std::size_t line) const { return starts_[line]; }

    int LineOf(std::size_t offset) const {
        const auto it = std::upper_bound(starts_.begin(), starts_.end(), offset);
        if (it == starts_.begin()) return 0;
        return static_cast<int>(it - starts_.begin()) - 1;
    }

    // 字节偏移 -> (line, UTF-16 column)
    void Resolve(std::string_view text, std::size_t offset, int* line, int* character) const {
        const int index = LineOf(offset);
        const std::size_t start = starts_[static_cast<std::size_t>(index)];
        *line = index;
        *character = static_cast<int>(
            platform::byte_index_to_utf16_column(text.substr(start, offset - start),
                                                 offset - start));
    }

    // 该行的文本（不含 '\n'）
    std::string_view LineText(std::string_view text, std::size_t line) const {
        if (line >= starts_.size()) return {};
        const std::size_t begin = starts_[line];
        const std::size_t end = (line + 1 < starts_.size()) ? starts_[line + 1] : text.size();
        std::string_view slice = text.substr(begin, end - begin);
        if (const std::size_t nl = slice.find('\n'); nl != std::string_view::npos) {
            slice = slice.substr(0, nl);
        }
        if (!slice.empty() && slice.back() == '\r') slice.remove_suffix(1);
        return slice;
    }

private:
    std::vector<std::size_t> starts_;
};

// 该行是否以反斜杠续行（预处理指令续行）
bool LineContinues(std::string_view text, const LineStarts& lines, std::size_t line) {
    std::string_view slice = lines.LineText(text, line);
    while (!slice.empty() && (slice.back() == ' ' || slice.back() == '\t')) {
        slice.remove_suffix(1);
    }
    return !slice.empty() && slice.back() == '\\';
}

// -----------------------------------------------------------------------------
// 常量表达式求值（对应 _num_value / _clog2 / _idiv / _eval_tokens）
// -----------------------------------------------------------------------------

std::optional<long long> NumValue(std::string_view text) {
    std::string s;
    s.reserve(text.size());
    for (const char c : text) {
        if (c != '_') s.push_back(c);
    }
    if (s.empty()) return std::nullopt;

    bool all_digits = true;
    for (const char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            all_digits = false;
            break;
        }
    }
    if (all_digits) {
        try {
            return std::stoll(s);
        } catch (...) {
            return std::nullopt;
        }
    }

    std::size_t j = 0;
    while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) ++j;
    if (j < s.size() && s[j] == '\'' && j + 1 < s.size()) {
        const char base_ch = static_cast<char>(std::tolower(static_cast<unsigned char>(s[j + 1])));
        const std::string digits = s.substr(j + 2);
        int base = 0;
        switch (base_ch) {
            case 'b': base = 2; break;
            case 'o': base = 8; break;
            case 'd': base = 10; break;
            case 'h': base = 16; break;
            default: return std::nullopt;
        }
        if (digits.empty()) return std::nullopt;
        try {
            std::size_t used = 0;
            const long long value = std::stoll(digits, &used, base);
            if (used == 0) return std::nullopt;
            return value;
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<long long> Clog2(std::optional<long long> v) {
    if (!v.has_value() || *v <= 0) return std::nullopt;
    if (*v == 1) return 0;
    long long value = *v - 1;
    long long bits = 0;
    while (value > 0) {
        value >>= 1;
        ++bits;
    }
    return bits;
}

std::optional<long long> IDiv(long long a, long long b) {
    if (b == 0) return std::nullopt;
    const long long q = std::llabs(a) / std::llabs(b);
    return ((a < 0) == (b < 0)) ? q : -q;
}

}  // namespace

// =============================================================================
// 词法：Verible filtered token 流 -> 我们的 Token（kind 分类 + UTF-16 列）
// =============================================================================

std::vector<Token> LexWithVerible(std::string_view path, std::string_view text,
                                  std::vector<ParseError>* errors) {
    std::vector<Token> out;
    if (text.empty()) return out;

    std::string filename(path);
    const verilog::VerilogPreprocess::Config preprocess_config{.filter_branches = true};

    std::unique_ptr<verilog::VerilogAnalyzer> analyzer;
    try {
        // ⚠ 这个重载接受 string_view（内部会拷贝成 MemBlock），不需要我们持有 MemBlock
        analyzer = verilog::VerilogAnalyzer::AnalyzeAutomaticMode(text, filename,
                                                                  preprocess_config);
    } catch (const std::exception& exc) {
        if (errors != nullptr) {
            errors->push_back({0, 0, std::string("Verible lexer threw: ") + exc.what()});
        }
        return out;
    } catch (...) {
        if (errors != nullptr) errors->push_back({0, 0, "Verible lexer threw: unknown"});
        return out;
    }
    if (analyzer == nullptr) {
        if (errors != nullptr) errors->push_back({0, 0, "Verible analyzer unavailable"});
        return out;
    }

    // Verible 的错误消息格式：<file>:<line>:<col>: <message>（collect 出来给 parse_errors）
    if (errors != nullptr && (!analyzer->LexStatus().ok() || !analyzer->ParseStatus().ok())) {
        static const re2::RE2 kMessageLine(R"(^.*?:(\d+):(\d+):\s*(.*)$)");
        for (const std::string& message : analyzer->LinterTokenErrorMessages(false)) {
            ParseError error;
            error.message = message;
            int line = 0;
            int column = 0;
            std::string body;
            if (kMessageLine.ok() &&
                re2::RE2::FullMatch(message, kMessageLine, &line, &column, &body)) {
                error.line = std::max(0, line - 1);
                error.character = std::max(0, column - 1);
            }
            errors->push_back(std::move(error));
        }
    }

    const std::string_view base = analyzer->Data().Contents();
    const LineStarts lines(base);

    bool have_previous = false;
    int previous_line = -1;
    int skip_through_line = -1;

    for (const auto iterator : analyzer->Data().GetTokenStreamView()) {
        const verible::TokenInfo& info = *iterator;
        const std::string_view piece = info.text();
        if (piece.empty()) continue;

        const int offset = info.left(base);
        if (offset < 0) continue;
        int line = 0;
        int character = 0;
        lines.Resolve(base, static_cast<std::size_t>(offset), &line, &character);

        const bool first_on_line = !have_previous || line != previous_line;
        have_previous = true;
        previous_line = line;

        // 预处理指令：按 Python 的规则"整行丢弃"（含 '\' 续行）
        if (first_on_line && piece.front() == '`') {
            int end_line = line;
            while (LineContinues(base, lines, static_cast<std::size_t>(end_line))) {
                ++end_line;
            }
            skip_through_line = std::max(skip_through_line, end_line);
            continue;
        }
        if (line <= skip_through_line) continue;

        Token token;
        token.text.assign(piece.data(), piece.size());
        token.offset = static_cast<std::size_t>(offset);

        const unsigned char first = static_cast<unsigned char>(token.text.front());
        if (token.text.front() == '"') {
            token.kind = 's';
        } else if (token.text.front() == '\\') {
            token.kind = 'i';  // escaped identifier
        } else if (std::isalpha(first) != 0 || token.text.front() == '_' ||
                   token.text.front() == '$') {
            token.kind = 'i';  // 标识符 / 关键字 / 宏名
        } else if (std::isdigit(first) != 0 ||
                   (token.text.front() == '\'' && token.text.size() > 1 &&
                    (std::isalpha(static_cast<unsigned char>(token.text[1])) != 0 ||
                     token.text[1] == '0' || token.text[1] == '1'))) {
            token.kind = 'n';
        } else {
            token.kind = 'o';
        }

        token.line = line;
        token.character = character;
        token.end_character =
            character + static_cast<int>(platform::utf16_len(token.text));
        out.push_back(std::move(token));
    }

    return out;
}

// =============================================================================
// 结构识别（移植 hdl.py）
// =============================================================================
namespace {

using TokenList = std::vector<Token>;
using Range = std::vector<std::pair<std::size_t, std::size_t>>;

// 对应 _match_pair：返回闭合括号之后的下标
std::optional<std::size_t> MatchPair(const TokenList& toks, std::size_t i, std::size_t hi,
                                     std::string_view open_ch, std::string_view close_ch) {
    int depth = 0;
    std::size_t j = i;
    while (j < hi) {
        const std::string_view t = toks[j].text;
        if (t == open_ch) {
            ++depth;
        } else if (t == close_ch) {
            --depth;
            if (depth == 0) return j + 1;
        }
        ++j;
    }
    return std::nullopt;
}

// 对应 _split_top
Range SplitTop(const TokenList& toks, std::size_t lo, std::size_t hi,
               std::string_view sep = ",") {
    Range items;
    int depth = 0;
    std::size_t start = lo;
    for (std::size_t j = lo; j < hi; ++j) {
        const std::string_view t = toks[j].text;
        if (IsOpenBracket(t)) {
            ++depth;
        } else if (IsCloseBracket(t)) {
            depth = std::max(0, depth - 1);
        } else if (t == sep && depth == 0) {
            items.emplace_back(start, j);
            start = j + 1;
        }
    }
    items.emplace_back(start, hi);
    return items;
}

// 对应 _skip_attrs：跳过 (* ... *) 属性实例
std::size_t SkipAttrs(const TokenList& toks, std::size_t i, std::size_t hi) {
    while (i + 1 < hi && toks[i].text == "(" && toks[i + 1].text == "*") {
        if (i + 2 < hi && toks[i + 2].text == ")") break;  // 这是 always @(*)
        std::size_t j = i + 2;
        std::optional<std::size_t> found;
        while (j + 1 < hi) {
            if (toks[j].text == "*" && toks[j + 1].text == ")") {
                found = j + 2;
                break;
            }
            if (toks[j].text == ";") break;
            ++j;
        }
        if (!found.has_value()) break;
        i = *found;
    }
    return i;
}

// 对应 _skip_to_semicolon
std::size_t SkipToSemicolon(const TokenList& toks, std::size_t i, std::size_t hi) {
    int depth = 0;
    while (i < hi) {
        const std::string_view t = toks[i].text;
        if (IsOpenBracket(t)) {
            ++depth;
        } else if (IsCloseBracket(t)) {
            depth = std::max(0, depth - 1);
        } else if (t == ";" && depth == 0) {
            return i + 1;
        }
        ++i;
    }
    return hi;
}

// 对应 _skip_value
std::size_t SkipValue(const TokenList& toks, std::size_t i, std::size_t hi) {
    int depth = 0;
    while (i < hi) {
        const std::string_view t = toks[i].text;
        if (IsOpenBracket(t)) {
            ++depth;
        } else if (IsCloseBracket(t)) {
            depth = std::max(0, depth - 1);
        } else if ((t == "," || t == ";") && depth == 0) {
            return i;
        }
        ++i;
    }
    return hi;
}

// 对应 _skip_block
std::size_t SkipBlock(const TokenList& toks, std::size_t i, std::size_t hi,
                      const std::set<std::string>& opens,
                      const std::set<std::string>& closes) {
    int depth = 0;
    while (i < hi) {
        const std::string& t = toks[i].text;
        if (opens.count(t) != 0) {
            ++depth;
        } else if (closes.count(t) != 0) {
            --depth;
        }
        ++i;
        if (depth <= 0) break;
    }
    return i;
}

// 对应 _eval_tokens
class ExprEvaluator {
public:
    ExprEvaluator(const TokenList& toks, std::size_t lo, std::size_t hi,
                  const std::map<std::string, long long>& env)
        : toks_(toks), lo_(lo), hi_(hi), pos_(lo), env_(env) {}

    std::optional<long long> Run() {
        if (lo_ >= hi_) return std::nullopt;
        try {
            return Expr();
        } catch (...) {
            return std::nullopt;
        }
    }

private:
    std::string_view Peek() const { return pos_ < hi_ ? toks_[pos_].text : std::string_view{}; }
    bool AtEnd() const { return pos_ >= hi_; }
    std::string_view Take() {
        if (pos_ >= hi_) {
            ++pos_;
            return {};
        }
        return toks_[pos_++].text;
    }

    std::optional<long long> Expr() {
        auto value = Term();
        while (!AtEnd()) {
            const std::string_view t = Peek();
            if (t == "+") {
                Take();
                const auto rhs = Term();
                value = (!value || !rhs) ? std::nullopt
                                         : std::optional<long long>(*value + *rhs);
            } else if (t == "-") {
                Take();
                const auto rhs = Term();
                value = (!value || !rhs) ? std::nullopt
                                         : std::optional<long long>(*value - *rhs);
            } else {
                return value;
            }
        }
        return value;
    }

    std::optional<long long> Term() {
        auto value = Unary();
        while (!AtEnd()) {
            const std::string_view t = Peek();
            if (t == "*") {
                // '**' 幂运算
                if (pos_ + 1 < hi_ && toks_[pos_ + 1].text == "*") {
                    Take();
                    Take();
                    const auto rhs = Unary();
                    if (!value || !rhs || *rhs < 0 || *rhs > 64) {
                        value = std::nullopt;
                    } else {
                        long long result = 1;
                        bool overflow = false;
                        for (long long k = 0; k < *rhs; ++k) {
                            if (result > std::numeric_limits<long long>::max() /
                                             std::max<long long>(1, *value)) {
                                overflow = true;
                                break;
                            }
                            result *= *value;
                        }
                        value = overflow ? std::nullopt
                                         : std::optional<long long>(result);
                    }
                } else {
                    Take();
                    const auto rhs = Unary();
                    value = (!value || !rhs) ? std::nullopt
                                             : std::optional<long long>(*value * *rhs);
                }
            } else if (t == "/") {
                Take();
                const auto rhs = Unary();
                value = (!value || !rhs) ? std::nullopt : IDiv(*value, *rhs);
            } else if (t == "%") {
                Take();
                const auto rhs = Unary();
                if (!value || !rhs || *rhs == 0) {
                    value = std::nullopt;
                } else {
                    value = *value % *rhs;
                }
            } else {
                return value;
            }
        }
        return value;
    }

    std::optional<long long> Unary() {
        const std::string_view t = Peek();
        if (t == "-") {
            Take();
            const auto v = Unary();
            return v ? std::optional<long long>(-*v) : std::nullopt;
        }
        if (t == "+") {
            Take();
            return Unary();
        }
        if (t == "~") {
            Take();
            const auto v = Unary();
            return v ? std::optional<long long>(~*v) : std::nullopt;
        }
        return Atom();
    }

    std::optional<long long> Atom() {
        if (AtEnd()) return std::nullopt;
        const std::string_view t = Peek();
        if (t == "(") {
            Take();
            const auto v = Expr();
            if (Peek() == ")") Take();
            return v;
        }
        if (!t.empty() && t.front() == '$') {
            Take();
            if (t == "$clog2" && Peek() == "(") {
                Take();
                const auto v = Expr();
                if (Peek() == ")") Take();
                return Clog2(v);
            }
            if (Peek() == "(") {
                int depth = 0;
                while (!AtEnd()) {
                    const std::string_view tt = Take();
                    if (tt == "(") {
                        ++depth;
                    } else if (tt == ")") {
                        --depth;
                        if (depth == 0) break;
                    }
                }
            }
            return std::nullopt;
        }
        if (!t.empty() && (std::isdigit(static_cast<unsigned char>(t.front())) != 0 ||
                           t.front() == '\'')) {
            Take();
            return NumValue(t);
        }
        if (!t.empty() && (std::isalpha(static_cast<unsigned char>(t.front())) != 0 ||
                           t.front() == '_' || t.front() == '\\')) {
            Take();
            const auto it = env_.find(IdentKey(t));
            return it == env_.end() ? std::nullopt : std::optional<long long>(it->second);
        }
        Take();
        return std::nullopt;
    }

    const TokenList& toks_;
    std::size_t lo_;
    std::size_t hi_;
    std::size_t pos_ = 0;
    const std::map<std::string, long long>& env_;
};

std::optional<long long> EvalTokens(const TokenList& toks, std::size_t lo, std::size_t hi,
                                    const std::map<std::string, long long>& env) {
    return ExprEvaluator(toks, lo, hi, env).Run();
}

struct BusRange {
    std::optional<int> msb;
    std::optional<int> lsb;
    int width = 1;
};

// 对应 _parse_range（注意它把 lo/hi 当成 [msb : lsb] 的内容，而不是带方括号）
BusRange ParseRange(const TokenList& toks, std::size_t lo, std::size_t hi,
                    const std::map<std::string, long long>& env) {
    BusRange out;
    const Range parts = SplitTop(toks, lo, hi, ":");
    if (parts.size() >= 2) {
        const auto msb = EvalTokens(toks, parts[0].first, parts[0].second, env);
        const auto lsb = EvalTokens(toks, parts[1].first, parts[1].second, env);
        if (msb.has_value() && lsb.has_value()) {
            out.msb = static_cast<int>(*msb);
            out.lsb = static_cast<int>(*lsb);
            out.width = static_cast<int>(std::llabs(*msb - *lsb)) + 1;
        }
        return out;
    }
    const auto single = EvalTokens(toks, lo, hi, env);
    if (single.has_value()) {
        out.msb = static_cast<int>(*single);
        out.lsb = static_cast<int>(*single);
        out.width = 1;
    }
    return out;
}

std::string JoinText(const TokenList& toks, std::size_t lo, std::size_t hi) {
    std::string out;
    for (std::size_t i = lo; i < hi && i < toks.size(); ++i) out += toks[i].text;
    return out;
}

// -----------------------------------------------------------------------------
// 解析器
// -----------------------------------------------------------------------------
class StructureParser {
public:
    StructureParser(const TokenList& toks, ParsedFile& file) : toks_(toks), file_(file) {}

    void Run() {
        std::size_t i = 0;
        while (i < toks_.size()) {
            const Token& t = toks_[i];
            if (t.kind == 'i' && (t.text == "module" || t.text == "macromodule")) {
                const std::size_t next = ParseModule(i);
                i = (next > i) ? next : i + 1;
            } else {
                ++i;
            }
        }
    }

private:
    void Error(int line, int character, std::string message) {
        file_.parse_errors.push_back({line, character, std::move(message)});
    }

    // ---- 参数表 -----------------------------------------------------------
    struct ParamItem {
        std::size_t name_index = 0;
        std::size_t value_begin = 0;
        std::size_t value_end = 0;
        bool local = false;
    };

    std::vector<ParamItem> ParseParamList(std::size_t lo, std::size_t hi) {
        std::vector<ParamItem> out;
        for (const auto& [raw_a, raw_b] : SplitTop(toks_, lo, hi)) {
            std::size_t a = SkipAttrs(toks_, raw_a, raw_b);
            const std::size_t b = raw_b;
            if (a >= b) continue;
            std::size_t j = a;
            bool local = false;
            if (toks_[j].text == "parameter") {
                ++j;
            } else if (toks_[j].text == "localparam") {
                ++j;
                local = true;
            }
            while (j < b && toks_[j].kind == 'i' &&
                   DataTypeWords().count(toks_[j].text) != 0) {
                ++j;
            }
            while (j < b && toks_[j].kind == 'i' &&
                   TypeMods().count(toks_[j].text) != 0) {
                ++j;
            }
            while (j < b && toks_[j].text == "[") {
                const auto e = MatchPair(toks_, j, b, "[", "]");
                if (!e.has_value()) break;
                j = *e;
            }
            if (j >= b || toks_[j].kind != 'i' || NonModule().count(toks_[j].text) != 0) {
                continue;
            }
            ParamItem item;
            item.name_index = j;
            ++j;
            if (j < b && toks_[j].text == "=") {
                item.value_begin = j + 1;
                item.value_end = b;
            } else {
                item.value_begin = j;
                item.value_end = j;
            }
            item.local = local;
            out.push_back(item);
        }
        return out;
    }

    // 对应 _parse_param_decl
    std::pair<std::vector<ParamItem>, std::size_t> ParseParamDecl(std::size_t i,
                                                                 std::size_t hi) {
        const bool local = toks_[i].text == "localparam";
        std::size_t j = i + 1;
        std::vector<ParamItem> out;
        while (j < hi) {
            j = SkipAttrs(toks_, j, hi);
            while (j < hi && toks_[j].kind == 'i' &&
                   DataTypeWords().count(toks_[j].text) != 0) {
                ++j;
            }
            while (j < hi && toks_[j].kind == 'i' && TypeMods().count(toks_[j].text) != 0) {
                ++j;
            }
            while (j < hi && toks_[j].text == "[") {
                const auto e = MatchPair(toks_, j, hi, "[", "]");
                if (!e.has_value()) break;
                j = *e;
            }
            if (j >= hi || toks_[j].kind != 'i' || NonModule().count(toks_[j].text) != 0) {
                break;
            }
            ParamItem item;
            item.name_index = j;
            item.local = local;
            ++j;
            if (j < hi && toks_[j].text == "=") {
                ++j;
                const std::size_t start = j;
                int depth = 0;
                while (j < hi) {
                    const std::string_view t = toks_[j].text;
                    if (IsOpenBracket(t)) {
                        ++depth;
                    } else if (IsCloseBracket(t)) {
                        depth = std::max(0, depth - 1);
                    } else if ((t == "," || t == ";") && depth == 0) {
                        break;
                    }
                    ++j;
                }
                item.value_begin = start;
                item.value_end = j;
            } else {
                item.value_begin = j;
                item.value_end = j;
            }
            out.push_back(item);
            if (j < hi && toks_[j].text == ",") {
                ++j;
                continue;
            }
            break;
        }
        return {out, SkipToSemicolon(toks_, j, hi)};
    }

    // ---- 声明 -------------------------------------------------------------
    struct DirectionDecl {
        std::string direction;
        std::string net;
        BusRange range;
        std::vector<std::size_t> names;
        std::size_t next = 0;
    };

    DirectionDecl ParseDirectionDecl(std::size_t i, std::size_t hi) {
        DirectionDecl out;
        out.direction = toks_[i].text;
        std::size_t j = SkipAttrs(toks_, i + 1, hi);
        while (j < hi && toks_[j].kind == 'i' &&
               (NetTypes().count(toks_[j].text) != 0 ||
                TypeMods().count(toks_[j].text) != 0 || toks_[j].text == "integer" ||
                toks_[j].text == "int" || toks_[j].text == "real" ||
                toks_[j].text == "time" || toks_[j].text == "bit")) {
            if (NetTypes().count(toks_[j].text) != 0) out.net = toks_[j].text;
            ++j;
        }
        if (j < hi && toks_[j].text == "[") {
            const auto e = MatchPair(toks_, j, hi, "[", "]");
            if (e.has_value()) {
                out.range = ParseRange(toks_, j + 1, *e - 1, env_);
                j = *e;
            }
        }
        while (j < hi) {
            if (toks_[j].kind != 'i' || NonModule().count(toks_[j].text) != 0) break;
            out.names.push_back(j);
            ++j;
            while (j < hi && toks_[j].text == "[") {
                const auto e = MatchPair(toks_, j, hi, "[", "]");
                if (!e.has_value()) break;
                j = *e;
            }
            if (j < hi && toks_[j].text == "=") j = SkipValue(toks_, j + 1, hi);
            if (j < hi && toks_[j].text == ",") {
                ++j;
                j = SkipAttrs(toks_, j, hi);
                continue;
            }
            break;
        }
        out.next = SkipToSemicolon(toks_, j, hi);
        return out;
    }

    struct SignalDecl {
        std::string keyword;
        BusRange range;
        std::vector<std::size_t> names;
        std::size_t next = 0;
    };

    SignalDecl ParseSignalDecl(std::size_t i, std::size_t hi) {
        SignalDecl out;
        out.keyword = toks_[i].text;
        std::size_t j = SkipAttrs(toks_, i + 1, hi);
        while (j < hi && toks_[j].kind == 'i' &&
               (NetTypes().count(toks_[j].text) != 0 ||
                TypeMods().count(toks_[j].text) != 0 || toks_[j].text == "integer" ||
                toks_[j].text == "int" || toks_[j].text == "real" ||
                toks_[j].text == "realtime" || toks_[j].text == "time" ||
                toks_[j].text == "bit")) {
            ++j;
        }
        if (j < hi && toks_[j].text == "[") {
            const auto e = MatchPair(toks_, j, hi, "[", "]");
            if (e.has_value()) {
                out.range = ParseRange(toks_, j + 1, *e - 1, env_);
                j = *e;
            }
        }
        while (j < hi) {
            if (toks_[j].kind != 'i' || NonModule().count(toks_[j].text) != 0) break;
            out.names.push_back(j);
            ++j;
            while (j < hi && toks_[j].text == "[") {
                const auto e = MatchPair(toks_, j, hi, "[", "]");
                if (!e.has_value()) break;
                j = *e;
            }
            if (j < hi && toks_[j].text == "=") j = SkipValue(toks_, j + 1, hi);
            if (j < hi && toks_[j].text == ",") {
                ++j;
                j = SkipAttrs(toks_, j, hi);
                continue;
            }
            break;
        }
        out.next = SkipToSemicolon(toks_, j, hi);
        return out;
    }

    // ---- 端口列表 ---------------------------------------------------------
    struct PortItem {
        std::size_t name_index = 0;
        std::string direction;
        std::string net;
        BusRange range;
    };

    std::pair<std::vector<PortItem>, bool> ParsePortList(std::size_t lo, std::size_t hi) {
        std::vector<PortItem> items;
        bool ansi = false;
        std::string last_direction;
        for (const auto& [raw_a, raw_b] : SplitTop(toks_, lo, hi)) {
            const std::size_t a = SkipAttrs(toks_, raw_a, raw_b);
            const std::size_t b = raw_b;
            if (a >= b) continue;
            std::size_t k = a;
            std::optional<std::string> direction;
            std::string net;
            BusRange range;
            if (toks_[k].kind == 'i' && Directions().count(toks_[k].text) != 0) {
                direction = toks_[k].text;
                ansi = true;
                ++k;
            }
            while (k < b && toks_[k].kind == 'i' &&
                   (NetTypes().count(toks_[k].text) != 0 ||
                    TypeMods().count(toks_[k].text) != 0 || toks_[k].text == "integer" ||
                    toks_[k].text == "int" || toks_[k].text == "real" ||
                    toks_[k].text == "time" || toks_[k].text == "bit")) {
                if (NetTypes().count(toks_[k].text) != 0) net = toks_[k].text;
                ++k;
            }
            if (k < b && toks_[k].text == "[") {
                const auto e = MatchPair(toks_, k, b, "[", "]");
                if (e.has_value()) {
                    range = ParseRange(toks_, k + 1, *e - 1, env_);
                    k = *e;
                }
            }
            std::optional<std::size_t> name_index;
            if (k + 1 < b && toks_[k].text == "." && toks_[k + 1].kind == 'i') {
                name_index = k + 1;
                k += 2;
                if (k < b && toks_[k].text == "(") {
                    const auto e = MatchPair(toks_, k, b, "(", ")");
                    if (e.has_value()) k = *e;
                }
            } else if (k < b && toks_[k].kind == 'i' &&
                       NonModule().count(toks_[k].text) == 0) {
                name_index = k;
                ++k;
            }
            if (!name_index.has_value()) continue;
            if (direction.has_value()) {
                last_direction = *direction;
            } else {
                direction = last_direction;
            }
            PortItem item;
            item.name_index = *name_index;
            item.direction = direction.value_or("");
            item.net = net;
            item.range = range;
            items.push_back(std::move(item));
        }
        return {items, ansi};
    }

    // ---- 例化 -------------------------------------------------------------
    struct InstanceGroup {
        std::string module;
        std::vector<std::size_t> name_indexes;  // 0 = 无名（primitive）
        std::vector<std::map<std::string, std::string>> connections;
        std::vector<std::map<std::string, std::string>> params;  // 每个实例继承同一份参数覆盖
        std::vector<std::vector<std::pair<std::pair<int, int>, std::string>>> conn_ports;
        std::size_t start_token = 0;
        std::size_t next = 0;
        int end_line = 0;
        int end_character = 0;
    };

    struct Connections {
        std::map<std::string, std::string> conns;
        std::vector<std::pair<std::pair<int, int>, std::string>> positions;
        std::size_t next = 0;
    };

    std::optional<Connections> ParseConnections(std::size_t i, std::size_t hi) {
        const auto e = MatchPair(toks_, i, hi, "(", ")");
        if (!e.has_value()) return std::nullopt;
        Connections out;
        std::size_t index = 0;
        for (const auto& [raw_a, raw_b] : SplitTop(toks_, i + 1, *e - 1)) {
            const std::size_t a = SkipAttrs(toks_, raw_a, raw_b);
            const std::size_t b = raw_b;
            if (a >= b) {
                ++index;
                continue;
            }
            if (toks_[a].text == "." && a + 1 < b && toks_[a + 1].kind == 'i') {
                const std::string port_name = toks_[a + 1].text;
                out.positions.push_back(
                    {{toks_[a + 1].line, toks_[a + 1].character}, port_name});
                if (a + 2 < b && toks_[a + 2].text == "(") {
                    const auto pe = MatchPair(toks_, a + 2, b, "(", ")");
                    const std::size_t expr_hi = pe.has_value() ? *pe - 1 : b;
                    out.conns[port_name] = JoinText(toks_, a + 3, expr_hi);
                } else {
                    out.conns[port_name] = "";
                }
            } else if (b - a == 1 && toks_[a].kind == 'i') {
                out.conns[std::to_string(index)] = toks_[a].text;
            } else {
                out.conns[std::to_string(index)] = JoinText(toks_, a, b);
            }
            ++index;
        }
        out.next = *e;
        return out;
    }

    std::map<std::string, std::string> ParseParamAssign(std::size_t lo, std::size_t hi) {
        std::map<std::string, std::string> out;
        std::size_t index = 0;
        for (const auto& [raw_a, raw_b] : SplitTop(toks_, lo, hi)) {
            const std::size_t a = SkipAttrs(toks_, raw_a, raw_b);
            const std::size_t b = raw_b;
            if (a >= b) {
                ++index;
                continue;
            }
            if (toks_[a].text == "." && a + 1 < b && toks_[a + 1].kind == 'i') {
                const std::string name = toks_[a + 1].text;
                if (a + 2 < b && toks_[a + 2].text == "(") {
                    const auto pe = MatchPair(toks_, a + 2, b, "(", ")");
                    const std::size_t expr_hi = pe.has_value() ? *pe - 1 : b;
                    out[name] = JoinText(toks_, a + 3, expr_hi);
                } else {
                    out[name] = "";
                }
            } else {
                out[std::to_string(index)] = JoinText(toks_, a, b);
            }
            ++index;
        }
        return out;
    }

    std::optional<InstanceGroup> TryInstance(std::size_t i, std::size_t hi) {
        if (toks_[i].kind != 'i') return std::nullopt;
        const std::string mod = toks_[i].text;
        if (NonModule().count(mod) != 0) return std::nullopt;
        if (!mod.empty() && mod.front() == '$') return std::nullopt;
        if (i > 0) {
            const std::string& prev = toks_[i - 1].text;
            if (StmtPrev().count(prev) == 0) {
                const bool label_ok =
                    toks_[i - 1].kind == 'i' && i >= 3 && toks_[i - 2].text == ":" &&
                    toks_[i - 3].text == "begin";
                if (!label_ok) return std::nullopt;
            }
        }

        std::size_t j = i + 1;
        std::map<std::string, std::string> params;
        if (j < hi && toks_[j].text == "#") {
            ++j;
            if (j >= hi || toks_[j].text != "(") return std::nullopt;
            const auto e = MatchPair(toks_, j, hi, "(", ")");
            if (!e.has_value()) return std::nullopt;
            params = ParseParamAssign(j + 1, *e - 1);
            j = *e;
        }

        InstanceGroup group;
        group.module = mod;
        group.start_token = i;

        std::optional<std::size_t> name_index;
        if (j < hi && toks_[j].kind == 'i' && NonModule().count(toks_[j].text) == 0) {
            if (j + 1 < hi && toks_[j + 1].text == "(") {
                name_index = j;
                ++j;
            } else {
                return std::nullopt;
            }
        }
        if (j >= hi || toks_[j].text != "(") return std::nullopt;
        const auto first = ParseConnections(j, hi);
        if (!first.has_value()) return std::nullopt;

        group.name_indexes.push_back(name_index.value_or(0));
        group.connections.push_back(first->conns);
        group.params.push_back(params);
        group.conn_ports.push_back(first->positions);
        j = first->next;

        while (j < hi && toks_[j].text == ",") {
            ++j;
            std::size_t next_name = 0;
            if (j < hi && toks_[j].kind == 'i' && NonModule().count(toks_[j].text) == 0 &&
                j + 1 < hi && toks_[j + 1].text == "(") {
                next_name = j;
                ++j;
            }
            if (j >= hi || toks_[j].text != "(") break;
            const auto more = ParseConnections(j, hi);
            if (!more.has_value()) break;
            group.name_indexes.push_back(next_name);
            group.connections.push_back(more->conns);
            group.params.push_back(params);
            group.conn_ports.push_back(more->positions);
            j = more->next;
        }

        if (j >= hi || toks_[j].text != ";") return std::nullopt;
        if (!name_index.has_value() && Primitives().count(mod) == 0) return std::nullopt;

        group.next = j + 1;
        group.end_line = toks_[j].line;
        group.end_character = toks_[j].end_character;
        return group;
    }

    // ---- 符号构造 ---------------------------------------------------------
    Param MakeParam(std::size_t name_index, std::size_t value_begin,
                    std::size_t value_end, bool local, const std::string& module_name) {
        Param p;
        p.name = toks_[name_index].text;
        p.value = JoinText(toks_, value_begin, value_end);
        p.local = local;
        p.line = toks_[name_index].line;
        p.character = toks_[name_index].character;
        p.file = file_.path;
        p.module = module_name;
        return p;
    }

    Signal MakeSignal(std::size_t name_index, const std::string& keyword, const BusRange& range,
                      const std::string& module_name) {
        Signal s;
        s.name = toks_[name_index].text;
        s.type = keyword;
        s.msb = range.msb;
        s.lsb = range.lsb;
        s.width = range.width;
        s.line = toks_[name_index].line;
        s.character = toks_[name_index].character;
        s.file = file_.path;
        s.module = module_name;
        return s;
    }

    // ---- 模块 -------------------------------------------------------------
    std::size_t ParseModule(std::size_t i) {
        const std::size_t hi = toks_.size();
        const std::size_t module_kw = i;
        std::size_t j = i + 1;
        while (j < hi && toks_[j].kind == 'i' &&
               (toks_[j].text == "automatic" || toks_[j].text == "static")) {
            ++j;
        }
        if (j >= hi || toks_[j].kind != 'i' || NonModule().count(toks_[j].text) != 0) {
            Error(toks_[module_kw].line, toks_[module_kw].character,
                  "expected module name after 'module'");
            return i + 1;
        }
        const std::size_t name_index = j;
        ++j;
        const std::string module_name = toks_[name_index].text;

        env_.clear();
        ModuleInfo info;
        info.module.name = module_name;
        info.module.file = file_.path;
        info.module.line = toks_[name_index].line;
        info.module.character = toks_[name_index].character;
        info.module.keyword_line = toks_[module_kw].line;
        info.module.keyword_character = toks_[module_kw].character;

        // 头部参数表 #( ... )
        if (j < hi && toks_[j].text == "#") {
            ++j;
            if (j >= hi || toks_[j].text != "(") {
                Error(toks_[name_index].line, toks_[name_index].character,
                      "expected '(' after '#' in module header");
            } else {
                const auto e = MatchPair(toks_, j, hi, "(", ")");
                if (!e.has_value()) {
                    Error(toks_[name_index].line, toks_[name_index].character,
                          "unbalanced parameter list in module '" + module_name + "'");
                    j = hi;
                } else {
                    for (const ParamItem& item : ParseParamList(j + 1, *e - 1)) {
                        info.params.push_back(MakeParam(item.name_index, item.value_begin,
                                                        item.value_end, item.local,
                                                        module_name));
                        const auto value = EvalTokens(toks_, item.value_begin,
                                                      item.value_end, env_);
                        if (value.has_value()) {
                            env_[IdentKey(toks_[item.name_index].text)] = *value;
                        }
                    }
                    j = *e;
                }
            }
        }

        // 头部端口表
        std::vector<PortItem> header_items;
        bool ansi = false;
        if (j < hi && toks_[j].text == "(") {
            const auto e = MatchPair(toks_, j, hi, "(", ")");
            if (!e.has_value()) {
                Error(toks_[name_index].line, toks_[name_index].character,
                      "unbalanced port list in module '" + module_name + "'");
                j = hi;
            } else {
                auto parsed = ParsePortList(j + 1, *e - 1);
                header_items = std::move(parsed.first);
                ansi = parsed.second;
                j = *e;
            }
        }
        if (j < hi && toks_[j].text == ";") ++j;
        if (header_items.empty()) ansi = true;

        // 找 endmodule
        std::size_t k = j;
        std::optional<std::size_t> end_token;
        bool unclosed = false;
        while (k < hi) {
            if (toks_[k].kind == 'i') {
                if (toks_[k].text == "endmodule") {
                    end_token = k;
                    break;
                }
                if (toks_[k].text == "module" || toks_[k].text == "macromodule") {
                    unclosed = true;
                    break;
                }
            }
            ++k;
        }
        if (!end_token.has_value()) unclosed = true;
        const std::size_t body_start = j;
        const std::size_t body_end = k;
        if (end_token.has_value()) {
            info.module.end_line = toks_[*end_token].line;
            info.module.end_character = toks_[*end_token].end_character;
        } else {
            info.module.end_line = (hi > 0) ? toks_[hi - 1].line : toks_[module_kw].line;
            info.module.end_character = (hi > 0) ? toks_[hi - 1].end_character : 0;
        }
        if (unclosed) {
            Error(toks_[name_index].line, toks_[name_index].character,
                  "unclosed module '" + module_name + "' (missing endmodule)");
        }

        info.module.ansi_ports = ansi;

        // 头部端口
        std::map<std::string, std::size_t> port_by_name;  // IdentKey -> index in ports
        for (const PortItem& item : header_items) {
            Port port;
            port.name = toks_[item.name_index].text;
            port.direction = item.direction;
            port.net = item.net;
            port.msb = item.range.msb;
            port.lsb = item.range.lsb;
            port.width = item.range.width;
            port.line = toks_[item.name_index].line;
            port.character = toks_[item.name_index].character;
            port.file = file_.path;
            info.module.ports.push_back(port);
            const std::size_t index = info.module.ports.size() - 1;
            port_by_name[IdentKey(port.name)] = index;
            if (!ansi) {
                info.positions.port_alias[{port.line, port.character}] = index;
            }
        }

        // body 扫描
        BodyScanner scanner(*this, info, port_by_name, module_name);
        scanner.Run(body_start, body_end);

        // 收尾
        for (std::size_t index = 0; index < info.module.ports.size(); ++index) {
            const Port& port = info.module.ports[index];
            info.positions.port_pos[{port.line, port.character}] = index;
        }
        for (std::size_t index = 0; index < info.params.size(); ++index) {
            const Param& param = info.params[index];
            info.positions.param_pos[{param.line, param.character}] = index;
        }
        for (std::size_t index = 0; index < info.signals.size(); ++index) {
            const Signal& signal = info.signals[index];
            info.positions.signal_pos[{signal.line, signal.character}] = index;
        }
        info.module.parameters.clear();
        for (const Param& param : info.params) info.module.parameters.push_back(param.name);

        // params 里"先头部后 body"的顺序与 Python 一致（params 向量就是这个顺序）
        file_.modules.push_back(std::move(info));
        return end_token.has_value() ? (*end_token + 1) : hi;
    }

    // ---- body 扫描 --------------------------------------------------------
    class BodyScanner {
    public:
        BodyScanner(StructureParser& parser, ModuleInfo& info,
                    std::map<std::string, std::size_t>& port_by_name,
                    const std::string& module_name)
            : parser_(parser),
              toks_(parser.toks_),
              file_(parser.file_),
              info_(info),
              port_by_name_(port_by_name),
              module_name_(module_name) {}

        void Run(std::size_t lo, std::size_t hi) {
            std::size_t i = lo;
            while (i < hi) {
                const Token& t = toks_[i];
                const std::string& tt = t.text;
                if (t.kind == 'i') {
                    if (tt == "function" || tt == "task") {
                        i = SkipBlock(toks_, i, hi, {"function", "task"},
                                      {"endfunction", "endtask"});
                        continue;
                    }
                    if (tt == "specify" || tt == "table") {
                        i = SkipBlock(toks_, i, hi, {tt}, {"end" + tt});
                        continue;
                    }
                    if (tt == "generate") {
                        ++i;
                        continue;
                    }
                    if (Directions().count(tt) != 0) {
                        auto decl = parser_.ParseDirectionDecl(i, hi);
                        for (const std::size_t name_index : decl.names) {
                            AddPort(name_index, decl.direction, decl.range);
                        }
                        i = decl.next;
                        continue;
                    }
                    if (DeclKeywords().count(tt) != 0) {
                        auto decl = parser_.ParseSignalDecl(i, hi);
                        for (const std::size_t name_index : decl.names) {
                            AddSignal(name_index, decl.keyword, decl.range);
                        }
                        i = decl.next;
                        continue;
                    }
                    if (tt == "parameter" || tt == "localparam") {
                        auto [items, next] = parser_.ParseParamDecl(i, hi);
                        for (const ParamItem& item : items) {
                            AddParam(item);
                        }
                        i = next;
                        continue;
                    }
                    if (tt == "assign") {
                        i = SkipToSemicolon(toks_, i, hi);
                        continue;
                    }
                    if (tt == "always" || tt == "always_comb" || tt == "always_ff" ||
                        tt == "always_latch" || tt == "initial" || tt == "final") {
                        ++i;
                        continue;
                    }
                }
                const auto instance = parser_.TryInstance(i, hi);
                if (instance.has_value()) {
                    AddInstance(*instance);
                    i = instance->next;
                    continue;
                }
                ++i;
            }
        }

    private:
        void AddParam(const ParamItem& item) {
            Param param = parser_.MakeParam(item.name_index, item.value_begin,
                                            item.value_end, item.local, module_name_);
            info_.positions.param_pos[{param.line, param.character}] = info_.params.size();
            const auto value =
                EvalTokens(toks_, item.value_begin, item.value_end, parser_.env_);
            if (value.has_value()) {
                parser_.env_[IdentKey(param.name)] = *value;
            }
            info_.params.push_back(std::move(param));
        }

        void AddSignal(std::size_t name_index, const std::string& keyword,
                       const BusRange& range) {
            if (port_by_name_.count(IdentKey(toks_[name_index].text)) != 0) return;
            Signal signal = parser_.MakeSignal(name_index, keyword, range, module_name_);
            info_.positions.signal_pos[{signal.line, signal.character}] = info_.signals.size();
            info_.signals.push_back(std::move(signal));
        }

        void AddPort(std::size_t name_index, const std::string& direction,
                     const BusRange& range) {
            const std::string key = IdentKey(toks_[name_index].text);
            const auto it = port_by_name_.find(key);
            if (it == port_by_name_.end()) {
                Port port;
                port.name = toks_[name_index].text;
                port.direction = direction;
                port.msb = range.msb;
                port.lsb = range.lsb;
                port.width = range.width;
                port.line = toks_[name_index].line;
                port.character = toks_[name_index].character;
                port.file = file_.path;
                info_.module.ports.push_back(port);
                const std::size_t index = info_.module.ports.size() - 1;
                port_by_name_[key] = index;
                info_.positions.port_pos[{port.line, port.character}] = index;
                return;
            }
            Port& port = info_.module.ports[it->second];
            if (!direction.empty()) port.direction = direction;
            if (range.msb.has_value()) {
                port.msb = range.msb;
                port.lsb = range.lsb;
                port.width = range.width;
            }
            // 位置更新到 body 里的声明处（Python 的行为）
            port.line = toks_[name_index].line;
            port.character = toks_[name_index].character;
            info_.positions.port_pos[{port.line, port.character}] = it->second;
        }

        void AddInstance(const InstanceGroup& group) {
            for (std::size_t index = 0; index < group.name_indexes.size(); ++index) {
                const std::size_t name_index = group.name_indexes[index];
                const bool named = name_index != 0;
                Instance instance;
                instance.module = group.module;
                instance.file = file_.path;
                instance.connections = group.connections[index];
                if (index < group.params.size()) instance.params = group.params[index];
                instance.connection_ports = group.conn_ports[index];
                instance.start_line = toks_[group.start_token].line;
                instance.start_character = toks_[group.start_token].character;
                instance.module_line = toks_[group.start_token].line;
                instance.module_character = toks_[group.start_token].character;
                instance.end_line = group.end_line;
                instance.end_character = group.end_character;
                if (named) {
                    instance.name = toks_[name_index].text;
                    instance.line = toks_[name_index].line;
                    instance.character = toks_[name_index].character;
                } else {
                    instance.line = toks_[group.start_token].line;
                    instance.character = toks_[group.start_token].character;
                }
                const std::size_t instance_index = file_.instances.size();
                file_.instances.push_back(std::move(instance));
                if (named) {
                    const Instance& added = file_.instances[instance_index];
                    info_.positions.inst_name_pos[{added.line, added.character}] =
                        instance_index;
                }
                info_.positions.modref_pos[{toks_[group.start_token].line,
                                            toks_[group.start_token].character}] =
                    group.module;
                for (const auto& [position, port_name] : group.conn_ports[index]) {
                    info_.positions.conn_port_pos[position] = {port_name, group.module};
                }
            }
        }

        StructureParser& parser_;
        const TokenList& toks_;
        ParsedFile& file_;
        ModuleInfo& info_;
        // ⚠ 这个 map 必须是**可写引用**：非 ANSI 端口在 body 里首次出现时要新增项。
        //    曾写成 const std::map& → C2678（operator[] 非 const）。
        //    不要改成 find/at 读法 —— 真正的语义是"要写"。
        std::map<std::string, std::size_t>& port_by_name_;
        std::string module_name_;
    };

    const TokenList& toks_;
    ParsedFile& file_;
    std::map<std::string, long long> env_;
};

}  // namespace

// =============================================================================
// 公共：单文件解析
// =============================================================================

ParsedFile ParseVerilog(std::string_view path, std::string_view text) {
    ParsedFile file;
    file.path = platform::normalize_path(path);
    file.tokens = LexWithVerible(file.path, text, &file.parse_errors);
    try {
        StructureParser parser(file.tokens, file);
        parser.Run();
    } catch (const std::exception& exc) {
        file.parse_errors.push_back(
            {0, 0, std::string("internal parser error: ") + exc.what()});
    } catch (...) {
        file.parse_errors.push_back({0, 0, "internal parser error: unknown"});
    }
    return file;
}

ParsedFile ParseVhdlEntities(std::string_view path, std::string_view text) {
    ParsedFile file;
    file.path = platform::normalize_path(path);

    // RE2 的大小写不敏感用内联 flag (?i)，不是构造参数
    static const re2::RE2 kEntity(R"((?im)^\s*entity\s+(?:work\.)?([A-Za-z_]\w*)\s+is\b)");
    // ⚠ re2 的变参只对应捕获组 1..n，没有"整段匹配"的位置。
    //    对 0 捕获组的正则传任何参数 → 恒返回 false（不报错、不崩溃）。
    //    所以这里必须给 kEndOfEntity 一个捕获组，只为了让参数个数对得上。
    //    同类陷阱见 src/constraints/text_util.h 的 LeadingIdentifier。
    //    回归保护：HdlVhdl.ExtractsEntity 断言 end_line == 3（恒 false 时会是 line+1）。
    static const re2::RE2 kEndOfEntity(R"((?im)^\s*(end\s+(?:entity\s+)?))");

    const LineStarts lines(text);
    re2::StringPiece rest(text.data(), text.size());
    re2::StringPiece match;
    std::set<std::string> seen;
    while (kEntity.ok() &&
           re2::RE2::FindAndConsume(&rest, kEntity, &match)) {
        const std::string name(match.data(), match.size());
        const std::string key = ToLower(name);
        if (!seen.insert(key).second) continue;

        const std::size_t name_offset =
            static_cast<std::size_t>(match.data() - text.data());
        int line = 0;
        int character = 0;
        lines.Resolve(text, name_offset, &line, &character);

        // end [entity] <name>
        int end_line = line + 1;
        const std::size_t tail_offset =
            static_cast<std::size_t>(match.data() - text.data()) + match.size();
        const std::string tail(text.substr(tail_offset));
        re2::StringPiece end_rest(tail.data(), tail.size());
        re2::StringPiece end_match;
        if (kEndOfEntity.ok() && re2::RE2::FindAndConsume(&end_rest, kEndOfEntity, &end_match)) {
            const std::size_t at = static_cast<std::size_t>(end_match.data() - tail.data()) +
                                   tail_offset;
            int ignored_line = 0;
            int ignored_character = 0;
            lines.Resolve(text, at, &ignored_line, &ignored_character);
            end_line = ignored_line;
        }

        ModuleInfo info;
        info.module.name = name;
        info.module.file = file.path;
        info.module.line = line;
        info.module.character = character;
        info.module.keyword_line = line;
        info.module.keyword_character = character;
        info.module.end_line = end_line;
        info.module.end_character = 0;
        info.module.vhdl = true;
        file.modules.push_back(std::move(info));
    }
    return file;
}

bool IsVerilogPath(std::string_view path) {
    const std::string ext = ToLower(platform::path_extension(path));
    return ext == ".v" || ext == ".sv" || ext == ".vh" || ext == ".svh";
}

bool IsVhdlPath(std::string_view path) {
    const std::string ext = ToLower(platform::path_extension(path));
    return ext == ".vhd" || ext == ".vhdl";
}

bool IsPrimitiveGate(std::string_view name) { return Primitives().count(std::string(name)) != 0; }

ParsedFile ParseFile(std::string_view path, std::string_view text) {
    if (IsVhdlPath(path)) return ParseVhdlEntities(path, text);
    return ParseVerilog(path, text);
}

// =============================================================================
// 缓存
// =============================================================================

std::uint64_t ContentHash(std::string_view text) {
    std::uint64_t hash = 1469598103934665603ULL;  // FNV-1a 64 offset basis
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;  // FNV prime
    }
    return hash;
}

namespace {

std::string CacheKey(std::string_view path, std::string_view text) {
    return ToLower(platform::normalize_path(path)) + "#" + std::to_string(ContentHash(text));
}

}  // namespace

std::shared_ptr<const ParsedFile> ParseCache::Get(std::string_view path,
                                                 std::string_view text) {
    const std::string key = CacheKey(path, text);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
        ++misses_;
        return nullptr;
    }
    ++hits_;
    return it->second;
}

void ParseCache::Put(std::string_view path, std::string_view text,
                     std::shared_ptr<ParsedFile> file) {
    // 哈希与键构造在锁外做（CacheKey 只读 path/text，不碰共享状态）
    const std::string key = CacheKey(path, text);
    std::lock_guard<std::mutex> lock(mutex_);
    if (entries_.size() >= capacity) entries_.clear();
    entries_[key] = std::move(file);
}

void ParseCache::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
}

std::size_t ParseCache::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

std::size_t ParseCache::hits() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hits_;
}

std::size_t ParseCache::misses() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return misses_;
}

// =============================================================================
// 工程级构建
// =============================================================================

IpGeneratedHdl IpGeneratedHdlFor(const project::Project& project) {
    IpGeneratedHdl out;
    // 复用 td 层的 .ipc -> 同名 HDL 逻辑（P-1 的源头），但**按扩展名分流**
    for (const std::string& path : td::IpGeneratedHdl(project)) {
        if (IsVhdlPath(path)) {
            out.vhdl.push_back(path);
        } else if (IsVerilogPath(path)) {
            out.verilog.push_back(path);
        }
    }
    return out;
}

std::vector<std::string> VerilogFilesFor(const project::Project& project) {
    std::vector<std::string> files;
    std::set<std::string> seen;
    for (const auto* file : project.HdlFiles()) {
        if (!IsVerilogPath(file->path)) continue;
        if (seen.insert(ToLower(file->path)).second) files.push_back(file->path);
    }
    for (const std::string& path : IpGeneratedHdlFor(project).verilog) {
        if (seen.insert(ToLower(path)).second) files.push_back(path);
    }
    return files;
}

std::vector<std::string> VhdlFilesFor(const project::Project& project) {
    std::vector<std::string> files;
    std::set<std::string> seen;
    for (const auto* file : project.HdlFiles()) {
        if (!IsVhdlPath(file->path)) continue;
        if (seen.insert(ToLower(file->path)).second) files.push_back(file->path);
    }
    for (const std::string& path : IpGeneratedHdlFor(project).vhdl) {
        if (seen.insert(ToLower(path)).second) files.push_back(path);
    }
    return files;
}

// =============================================================================
// 冷构建并行（Phase 2 / A1）
// =============================================================================
//  只并行**每文件独立**的那一段（读文本 + 解析 + 缓存），合并仍在调用线程按
//  files 顺序做 —— 所以输出（文件顺序 / 模块首次出现顺序 / 截断点）与顺序版
//  逐字段一致。预算（max_files / max_total_tokens）只在合并阶段扣减。
namespace {

// 并行取"文本 + 解析结果"；下标与 files 一一对应，失败项为 nullptr。
std::vector<std::shared_ptr<const ParsedFile>> ParseFileListParallel(
    const std::vector<std::string>& files, std::size_t count, const TextLoader& load,
    ParseCache* cache, std::size_t threads) {
    std::vector<std::shared_ptr<const ParsedFile>> parsed(count);
    if (count == 0) return parsed;
    if (load == nullptr) return parsed;

    std::atomic<std::size_t> next{0};
    const std::size_t workers =
        std::max<std::size_t>(1, std::min<std::size_t>(threads == 0 ? 1 : threads, count));

    auto worker = [&]() {
        for (;;) {
            const std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= count) return;
            const std::string& path = files[i];
            std::optional<std::string> text;
            try {
                text = load(path);
            } catch (...) {
                text = std::nullopt;
            }
            if (!text.has_value()) continue;
            std::shared_ptr<const ParsedFile> one;
            if (cache != nullptr) one = cache->Get(path, *text);
            if (!one) {
                try {
                    auto fresh = std::make_shared<ParsedFile>(ParseFile(path, *text));
                    if (cache != nullptr) cache->Put(path, *text, fresh);
                    one = std::move(fresh);
                } catch (...) {
                    one = nullptr;  // never-throw：一个文件失败不拖垮整次构建
                }
            }
            parsed[i] = std::move(one);
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(workers - 1);
    for (std::size_t w = 1; w < workers; ++w) pool.emplace_back(worker);
    worker();  // 调用线程也参与，不白等
    for (std::thread& thread : pool) thread.join();
    return parsed;
}

}  // namespace

std::size_t RecommendedParseThreads() {
    const unsigned hw = std::thread::hardware_concurrency();
    if (hw <= 1) return 1;
    return std::min<std::size_t>(8, static_cast<std::size_t>(hw) - 1);
}

BuildResult BuildIndex(const project::Project& project, const TextLoader& load,
                       const BuildOptions& options) {
    BuildResult result;

    std::vector<std::string> files = VerilogFilesFor(project);
    for (const std::string& path : VhdlFilesFor(project)) files.push_back(path);

    std::size_t token_budget = options.max_total_tokens;

    auto consume = [&](const std::shared_ptr<const ParsedFile>& parsed) {
        token_budget = parsed->tokens.size() >= token_budget
                           ? 0
                           : token_budget - parsed->tokens.size();
        ++result.files_indexed;
        result.parse_error_count += parsed->parse_errors.size();
        result.index.AddFile(parsed);  // 共享所有权：不再深拷贝整份 ParsedFile
    };

    std::size_t remaining_begin = 0;

    // ---- 阶段 1（可选）：并行解析前 min(files, max_files) 个文件 ----
    if (options.parse_threads > 1 && load != nullptr && files.size() > 1) {
        const std::size_t parallel_count = std::min(files.size(), options.max_files);
        const std::vector<std::shared_ptr<const ParsedFile>> parsed = ParseFileListParallel(
            files, parallel_count, load, options.cache, options.parse_threads);
        for (std::size_t i = 0; i < parallel_count; ++i) {
            if (result.files_indexed >= options.max_files || token_budget == 0) {
                result.truncated = true;
                break;
            }
            if (!parsed[i]) continue;
            consume(parsed[i]);
        }
        if (result.truncated) return result;
        remaining_begin = parallel_count;
    }

    // ---- 阶段 2：顺序合并/补尾（与 B8 之前的循环逐字一致）----
    for (std::size_t i = remaining_begin; i < files.size(); ++i) {
        const std::string& path = files[i];
        if (result.files_indexed >= options.max_files || token_budget == 0) {
            result.truncated = true;
            break;
        }
        if (load == nullptr) break;
        std::optional<std::string> text;
        try {
            text = load(path);
        } catch (...) {
            text = std::nullopt;
        }
        if (!text.has_value()) continue;

        std::shared_ptr<const ParsedFile> parsed;
        if (options.cache != nullptr) parsed = options.cache->Get(path, *text);
        if (!parsed) {
            auto fresh = std::make_shared<ParsedFile>(ParseFile(path, *text));
            if (options.cache != nullptr) options.cache->Put(path, *text, fresh);
            parsed = fresh;
        }
        consume(parsed);
    }
    return result;
}

// =============================================================================
// HdlIndex
// =============================================================================

namespace {

// 索引里"文件"的键：规范化 + 小写（Windows 语义）。AddFile / File / ReplaceFile /
// RemoveFile 必须用同一个函数，否则增量更新会找不到条目。
std::string IndexPathKey(std::string_view path) {
    return ToLower(platform::normalize_path(path));
}

}  // namespace

void HdlIndex::AddFile(std::shared_ptr<const ParsedFile> file) {
    if (!file) return;
    const std::size_t file_index = files_.size();
    by_lower_path_[IndexPathKey(file->path)] = file_index;

    for (std::size_t module_index = 0; module_index < file->modules.size(); ++module_index) {
        const Module& module = file->modules[module_index].module;
        const std::string key = ToLower(IdentKey(module.name));
        if (modules_by_key_.find(key) == modules_by_key_.end()) {
            modules_by_key_[key] = {file_index, module_index};
            module_order_.push_back(module.name);
        }
    }
    files_.push_back(std::move(file));
}

// -----------------------------------------------------------------------------
// 增量更新（Phase 2 / A1）
// -----------------------------------------------------------------------------

HdlIndex HdlIndex::Clone() const {
    HdlIndex copy;
    copy.files_ = files_;  // 只拷 shared_ptr（不深拷贝 ParsedFile / token 表）
    copy.by_lower_path_ = by_lower_path_;
    copy.modules_by_key_ = modules_by_key_;
    copy.module_order_ = module_order_;
    return copy;
}

void HdlIndex::ReplaceFile(std::shared_ptr<const ParsedFile> file) {
    if (!file) return;
    const auto it = by_lower_path_.find(IndexPathKey(file->path));
    if (it == by_lower_path_.end()) {
        // 新文件：追加（AddFile 会增量维护模块索引，首次出现顺序 = 追加顺序）
        AddFile(std::move(file));
        return;
    }
    files_[it->second] = std::move(file);
    // 该文件的模块集合可能整体替换/改名/增减：模块表整体重建（O(模块数)）
    RebuildModuleIndex();
}

bool HdlIndex::RemoveFile(std::string_view path) {
    const auto it = by_lower_path_.find(IndexPathKey(path));
    if (it == by_lower_path_.end()) return false;
    files_.erase(files_.begin() + static_cast<std::ptrdiff_t>(it->second));
    RebuildPathIndex();     // 后面的下标整体左移
    RebuildModuleIndex();
    return true;
}

void HdlIndex::RebuildPathIndex() {
    by_lower_path_.clear();
    for (std::size_t i = 0; i < files_.size(); ++i) {
        if (!files_[i]) continue;
        by_lower_path_[IndexPathKey(files_[i]->path)] = i;
    }
}

void HdlIndex::RebuildModuleIndex() {
    modules_by_key_.clear();
    module_order_.clear();
    for (std::size_t file_index = 0; file_index < files_.size(); ++file_index) {
        const std::shared_ptr<const ParsedFile>& file = files_[file_index];
        if (!file) continue;
        for (std::size_t module_index = 0; module_index < file->modules.size(); ++module_index) {
            const std::string& name = file->modules[module_index].module.name;
            const std::string key = ToLower(IdentKey(name));
            if (modules_by_key_.find(key) == modules_by_key_.end()) {
                modules_by_key_.emplace(key, std::make_pair(file_index, module_index));
                module_order_.push_back(name);
            }
        }
    }
}

const ParsedFile* HdlIndex::File(std::string_view path) const {
    const std::string key = IndexPathKey(path);
    const auto it = by_lower_path_.find(key);
    if (it == by_lower_path_.end()) return nullptr;
    return files_[it->second].get();
}

const Module* HdlIndex::ModuleByName(std::string_view name) const {
    if (name.empty()) return nullptr;
    const auto it = modules_by_key_.find(ToLower(IdentKey(name)));
    if (it == modules_by_key_.end()) return nullptr;
    const auto& [file_index, module_index] = it->second;
    return &files_[file_index]->modules[module_index].module;
}

std::vector<std::string> HdlIndex::ModuleNames() const { return module_order_; }

std::vector<const ModuleInfo*> HdlIndex::ModulesOfFile(std::string_view path) const {
    std::vector<const ModuleInfo*> out;
    const ParsedFile* file = File(path);
    if (file == nullptr) return out;
    for (const ModuleInfo& info : file->modules) out.push_back(&info);
    return out;
}

std::vector<const Instance*> HdlIndex::InstancesOfFile(std::string_view path) const {
    std::vector<const Instance*> out;
    const ParsedFile* file = File(path);
    if (file == nullptr) return out;
    for (const Instance& instance : file->instances) out.push_back(&instance);
    return out;
}

std::vector<const Instance*> HdlIndex::AllInstances() const {
    std::vector<const Instance*> out;
    for (const auto& file_ptr : files_) {
        if (!file_ptr) continue;
        for (const Instance& instance : file_ptr->instances) out.push_back(&instance);
    }
    return out;
}

std::vector<std::string> HdlIndex::Paths() const {
    std::vector<std::string> out;
    out.reserve(files_.size());
    for (const auto& file_ptr : files_) {
        if (file_ptr) out.push_back(file_ptr->path);
    }
    return out;
}

const Token* HdlIndex::TokenAt(const ParsedFile& file, int line, int character) {
    if (file.tokens.empty()) return nullptr;
    // token 按 (line, character) 有序 -> 二分
    std::size_t lo = 0;
    std::size_t hi = file.tokens.size();
    while (lo < hi) {
        const std::size_t mid = (lo + hi) / 2;
        const Token& token = file.tokens[mid];
        if (token.line < line || (token.line == line && token.character <= character)) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    const long long index = static_cast<long long>(lo) - 1;

    const Token* fallback = nullptr;
    for (const long long candidate : {index, index + 1}) {
        if (candidate < 0 || candidate >= static_cast<long long>(file.tokens.size())) {
            continue;
        }
        const Token& token = file.tokens[static_cast<std::size_t>(candidate)];
        if (token.line != line) continue;
        if (!(token.character <= character && character <= token.end_character)) continue;
        if (character < token.end_character || character == token.character) return &token;
        if (fallback == nullptr) fallback = &token;
    }
    return fallback;
}

const ModuleInfo* HdlIndex::ModuleAt(const ParsedFile& file, int line) {
    for (const ModuleInfo& info : file.modules) {
        if (info.module.line <= line && line <= info.module.end_line) return &info;
    }
    return nullptr;
}

namespace {

std::string RangeText(const std::optional<int>& msb, const std::optional<int>& lsb) {
    if (!msb.has_value() || !lsb.has_value()) return {};
    return "[" + std::to_string(*msb) + ":" + std::to_string(*lsb) + "]";
}

std::string PortDetail(const Port& port) {
    std::string detail = port.direction.empty() ? "port" : port.direction;
    const std::string range = RangeText(port.msb, port.lsb);
    if (!range.empty()) detail += " " + range;
    return detail;
}

std::string SignalDetail(const Signal& signal) {
    std::string detail = signal.type.empty() ? "signal" : signal.type;
    const std::string range = RangeText(signal.msb, signal.lsb);
    if (!range.empty()) detail += " " + range;
    return detail;
}

Span SpanOf(int line, int character, std::string_view name) {
    Span span;
    span.line = line;
    span.start_character = character;
    span.end_character = character + static_cast<int>(platform::utf16_len(name));
    return span;
}

SymbolAtResult PortSymbol(const Port& port, const Module& module) {
    SymbolAtResult out;
    out.kind = "port";
    out.name = port.name;
    out.detail = PortDetail(port);
    out.file = port.file;
    out.span = SpanOf(port.line, port.character, port.name);
    out.msb = port.msb;
    out.lsb = port.lsb;
    out.width = port.width;
    out.module = module.name;
    return out;
}

SymbolAtResult SignalSymbol(const Signal& signal) {
    SymbolAtResult out;
    out.kind = "signal";
    out.name = signal.name;
    out.detail = SignalDetail(signal);
    out.file = signal.file;
    out.span = SpanOf(signal.line, signal.character, signal.name);
    out.msb = signal.msb;
    out.lsb = signal.lsb;
    out.width = signal.width;
    out.module = signal.module;
    return out;
}

SymbolAtResult ParamSymbol(const Param& param) {
    SymbolAtResult out;
    out.kind = "parameter";
    out.name = param.name;
    out.detail = param.local ? "localparam" : "parameter";
    if (!param.value.empty()) out.detail += " = " + param.value;
    out.file = param.file;
    out.span = SpanOf(param.line, param.character, param.name);
    out.module = param.module;
    return out;
}

SymbolAtResult ModuleSymbol(const Module& module) {
    SymbolAtResult out;
    out.kind = "module";
    out.name = module.name;
    out.detail = "module (" + std::to_string(module.ports.size()) + " ports)";
    out.file = module.file;
    out.span = SpanOf(module.line, module.character, module.name);
    out.module = module.name;
    return out;
}

SymbolAtResult InstanceSymbol(const Instance& instance) {
    SymbolAtResult out;
    out.kind = "instance";
    out.name = instance.name;
    out.detail = "instance of " + instance.module;
    out.file = instance.file;
    out.span = SpanOf(instance.line, instance.character, instance.name);
    out.module = instance.module;
    return out;
}

Location LocationOf(const std::string& path, int line, int character,
                    std::string_view name) {
    Location location;
    location.path = path;
    location.line = line;
    location.character = character;
    location.end_line = line;
    location.end_character = character + static_cast<int>(platform::utf16_len(name));
    return location;
}

}  // namespace

std::optional<SymbolAtResult> HdlIndex::SymbolAt(std::string_view path, int line,
                                                 int character) const {
    const ParsedFile* file = File(path);
    if (file == nullptr) return std::nullopt;
    const Token* token = TokenAt(*file, line, character);
    if (token == nullptr || token->kind != 'i') return std::nullopt;

    const std::pair<int, int> pos{token->line, token->character};

    // 模块定义名本身
    for (const ModuleInfo& info : file->modules) {
        const Module& module = info.module;
        if (module.line == pos.first && module.character == pos.second) {
            return ModuleSymbol(module);
        }
    }

    const ModuleInfo* info = ModuleAt(*file, line);
    if (info != nullptr) {
        if (const auto it = info->positions.port_pos.find(pos);
            it != info->positions.port_pos.end()) {
            return PortSymbol(info->module.ports[it->second], info->module);
        }
        if (const auto it = info->positions.port_alias.find(pos);
            it != info->positions.port_alias.end()) {
            return PortSymbol(info->module.ports[it->second], info->module);
        }
        if (const auto it = info->positions.signal_pos.find(pos);
            it != info->positions.signal_pos.end()) {
            return SignalSymbol(info->signals[it->second]);
        }
        if (const auto it = info->positions.param_pos.find(pos);
            it != info->positions.param_pos.end()) {
            return ParamSymbol(info->params[it->second]);
        }
        if (const auto it = info->positions.inst_name_pos.find(pos);
            it != info->positions.inst_name_pos.end()) {
            return InstanceSymbol(file->instances[it->second]);
        }
        if (const auto it = info->positions.modref_pos.find(pos);
            it != info->positions.modref_pos.end()) {
            if (const Module* module = ModuleByName(it->second); module != nullptr) {
                return ModuleSymbol(*module);
            }
            return std::nullopt;
        }
        if (const auto it = info->positions.conn_port_pos.find(pos);
            it != info->positions.conn_port_pos.end()) {
            const Module* module = ModuleByName(it->second.second);
            if (module != nullptr) {
                for (const Port& port : module->ports) {
                    if (IdentKey(port.name) == IdentKey(it->second.first)) {
                        return PortSymbol(port, *module);
                    }
                }
            }
            return std::nullopt;
        }
        // 回落到"同名符号"
        for (const Port& port : info->module.ports) {
            if (IdentKey(port.name) == IdentKey(token->text)) {
                return PortSymbol(port, info->module);
            }
        }
        for (const Signal& signal : info->signals) {
            if (IdentKey(signal.name) == IdentKey(token->text)) return SignalSymbol(signal);
        }
        for (const Param& param : info->params) {
            if (IdentKey(param.name) == IdentKey(token->text)) return ParamSymbol(param);
        }
        for (const Instance& instance : file->instances) {
            if (!instance.name.empty() &&
                IdentKey(instance.name) == IdentKey(token->text)) {
                return InstanceSymbol(instance);
            }
        }
    }
    if (const Module* module = ModuleByName(token->text); module != nullptr) {
        return ModuleSymbol(*module);
    }
    return std::nullopt;
}

std::string HdlIndex::KindAt(std::string_view path, int line, int character) const {
    const auto symbol = SymbolAt(path, line, character);
    return symbol.has_value() ? symbol->kind : std::string();
}

std::optional<Location> HdlIndex::DefinitionOf(std::string_view path, int line,
                                               int character) const {
    const ParsedFile* file = File(path);
    if (file == nullptr) return std::nullopt;
    const Token* token = TokenAt(*file, line, character);
    if (token == nullptr || token->kind != 'i') return std::nullopt;

    const std::pair<int, int> pos{token->line, token->character};
    const ModuleInfo* info = ModuleAt(*file, line);
    if (info != nullptr) {
        if (const auto it = info->positions.inst_name_pos.find(pos);
            it != info->positions.inst_name_pos.end()) {
            const Instance& instance = file->instances[it->second];
            if (const Module* module = ModuleByName(instance.module); module != nullptr) {
                return LocationOf(module->file, module->line, module->character, module->name);
            }
            return std::nullopt;
        }
        if (const auto it = info->positions.modref_pos.find(pos);
            it != info->positions.modref_pos.end()) {
            if (const Module* module = ModuleByName(it->second); module != nullptr) {
                return LocationOf(module->file, module->line, module->character, module->name);
            }
            return std::nullopt;
        }
        if (const auto it = info->positions.conn_port_pos.find(pos);
            it != info->positions.conn_port_pos.end()) {
            const Module* module = ModuleByName(it->second.second);
            if (module != nullptr) {
                for (const Port& port : module->ports) {
                    if (IdentKey(port.name) == IdentKey(it->second.first)) {
                        return LocationOf(port.file, port.line, port.character, port.name);
                    }
                }
            }
            return std::nullopt;
        }
        for (const Port& port : info->module.ports) {
            if (IdentKey(port.name) == IdentKey(token->text)) {
                return LocationOf(port.file, port.line, port.character, port.name);
            }
        }
        for (const Signal& signal : info->signals) {
            if (IdentKey(signal.name) == IdentKey(token->text)) {
                return LocationOf(signal.file, signal.line, signal.character, signal.name);
            }
        }
        for (const Param& param : info->params) {
            if (IdentKey(param.name) == IdentKey(token->text)) {
                return LocationOf(param.file, param.line, param.character, param.name);
            }
        }
    }
    if (const Module* module = ModuleByName(token->text); module != nullptr) {
        return LocationOf(module->file, module->line, module->character, module->name);
    }
    return std::nullopt;
}

std::vector<DocSymbol> HdlIndex::DocumentSymbols(std::string_view path) const {
    std::vector<DocSymbol> out;
    const ParsedFile* file = File(path);
    if (file == nullptr) return out;

    for (const ModuleInfo& info : file->modules) {
        const Module& module = info.module;
        DocSymbol symbol;
        symbol.name = module.name;
        symbol.detail = "module";
        symbol.kind = 2;
        symbol.range_start_line = module.keyword_line;
        symbol.range_start_character = module.keyword_character;
        symbol.range_end_line = module.end_line;
        symbol.range_end_character = module.end_character;
        symbol.selection = SpanOf(module.line, module.character, module.name);

        for (const Port& port : module.ports) {
            DocSymbol child;
            child.name = port.name;
            child.detail = PortDetail(port);
            child.kind = 8;  // Field
            child.selection = SpanOf(port.line, port.character, port.name);
            child.range_start_line = child.selection.line;
            child.range_start_character = child.selection.start_character;
            child.range_end_line = child.selection.line;
            child.range_end_character = child.selection.end_character;
            symbol.children.push_back(std::move(child));
        }
        for (const Param& param : info.params) {
            DocSymbol child;
            child.name = param.name;
            child.detail = param.local ? "localparam" : "parameter";
            child.kind = 14;  // Constant
            child.selection = SpanOf(param.line, param.character, param.name);
            child.range_start_line = child.selection.line;
            child.range_start_character = child.selection.start_character;
            child.range_end_line = child.selection.line;
            child.range_end_character = child.selection.end_character;
            symbol.children.push_back(std::move(child));
        }
        for (const Signal& signal : info.signals) {
            DocSymbol child;
            child.name = signal.name;
            child.detail = signal.type.empty() ? "signal" : signal.type;
            child.kind = 13;  // Variable
            child.selection = SpanOf(signal.line, signal.character, signal.name);
            child.range_start_line = child.selection.line;
            child.range_start_character = child.selection.start_character;
            child.range_end_line = child.selection.line;
            child.range_end_character = child.selection.end_character;
            symbol.children.push_back(std::move(child));
        }
        out.push_back(std::move(symbol));
    }
    return out;
}

std::vector<WorkspaceSymbol> HdlIndex::SearchSymbols(std::string_view query) const {
    std::vector<WorkspaceSymbol> out;
    const std::string needle = ToLower(query);

    auto matches = [&needle](std::string_view name) {
        if (needle.empty()) return true;
        return ToLower(name).find(needle) != std::string::npos;
    };

    for (const auto& file_ptr : files_) {
        if (!file_ptr) continue;
        const ParsedFile& file = *file_ptr;
        for (const ModuleInfo& info : file.modules) {
            const Module& module = info.module;
            if (matches(module.name)) {
                WorkspaceSymbol symbol;
                symbol.name = module.name;
                symbol.container.clear();
                symbol.detail = "module";
                symbol.kind = 2;
                symbol.path = module.file;
                symbol.span = SpanOf(module.line, module.character, module.name);
                out.push_back(std::move(symbol));
            }
            for (const Port& port : module.ports) {
                if (!matches(port.name)) continue;
                WorkspaceSymbol symbol;
                symbol.name = port.name;
                symbol.container = module.name;
                symbol.detail = PortDetail(port);
                symbol.kind = 8;
                symbol.path = port.file;
                symbol.span = SpanOf(port.line, port.character, port.name);
                out.push_back(std::move(symbol));
            }
            for (const Signal& signal : info.signals) {
                if (!matches(signal.name)) continue;
                WorkspaceSymbol symbol;
                symbol.name = signal.name;
                symbol.container = module.name;
                symbol.detail = SignalDetail(signal);
                symbol.kind = 13;
                symbol.path = signal.file;
                symbol.span = SpanOf(signal.line, signal.character, signal.name);
                out.push_back(std::move(symbol));
            }
            for (const Param& param : info.params) {
                if (!matches(param.name)) continue;
                WorkspaceSymbol symbol;
                symbol.name = param.name;
                symbol.container = module.name;
                symbol.detail = param.local ? "localparam" : "parameter";
                symbol.kind = 14;
                symbol.path = param.file;
                symbol.span = SpanOf(param.line, param.character, param.name);
                out.push_back(std::move(symbol));
            }
        }
    }
    return out;
}

std::vector<Location> HdlIndex::References(std::string_view path, int line,
                                           int character) const {
    return References(path, line, character, /*include_declaration=*/true);
}

std::vector<Location> HdlIndex::References(std::string_view path, int line, int character,
                                           bool include_declaration) const {
    std::vector<Location> out;
    const ParsedFile* file = File(path);
    if (file == nullptr) return out;
    const Token* token = TokenAt(*file, line, character);
    if (token == nullptr || token->kind != 'i') return out;

    // 名字匹配用 IdentKey（escaped identifier 去掉反斜杠）后大小写敏感比较 ——
    // Verilog 是大小写敏感语言（hdl.py 的 _key 只处理反斜杠，不折大小写）。
    const std::string wanted = IdentKey(token->text);
    if (wanted.empty()) return out;

    // 声明位置：只在 include_declaration=false 时需要。符号解析失败（例如光标在
    // 一个只有隐式声明、索引里没有记录的网名上）就退化为"不过滤"，不吞掉真引用。
    std::optional<SymbolAtResult> symbol;
    if (!include_declaration) symbol = SymbolAt(path, line, character);

    for (const auto& candidate_ptr : files_) {
        if (!candidate_ptr) continue;
        const ParsedFile& candidate = *candidate_ptr;
        for (const Token& other : candidate.tokens) {
            if (other.kind != 'i') continue;
            if (IdentKey(other.text) != wanted) continue;
            if (symbol.has_value() && candidate.path == symbol->file &&
                other.line == symbol->span.line &&
                other.character == symbol->span.start_character) {
                continue;  // 声明本身
            }
            out.push_back(LocationOf(candidate.path, other.line, other.character,
                                     other.text));
        }
    }
    return out;
}

}  // namespace fpgalsp::hdl