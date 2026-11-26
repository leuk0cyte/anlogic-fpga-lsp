// =============================================================================
//  src/constraints/sdc.cpp
// =============================================================================
//  逐条移植 %PYREF%\fpgalsp\sdc.py。
//
//  结构对应关系（改代码前先看对应的 Python 函数）：
//    LogicalLines()  <- _logical_lines()     反斜杠续行
//    StripComment()  <- _strip_comment()     尊重 {} [] "" 与转义的 # 注释
//    Tokenize()      <- _tokenize()          Tcl 词法（word/brace/bracket/quote/sep）
//    SplitCommands() <- _split_commands()
//    MakeCommand()   <- _make_command()
//    Parse()         <- parse()
//    Diagnostics()   <- diagnostics()
//    Context()       <- context() + _port_context() + _clock_name()
//
//  移植纪律同 adc.cpp：不抛异常；发现 Python 缺陷时"忠实复现 + 登记 + 钉死测试"。
// =============================================================================

#include "constraints/sdc.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <iterator>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "constraints/text_util.h"

#include <re2/re2.h>


namespace fpgalsp::constraints::sdc {
namespace {

using lsp::Diagnostic;
using lsp::kError;
using lsp::kInfo;
using lsp::kWarning;
using text::IsSpace;
using text::LStrip;
using text::LowerAscii;
using text::PyFloat;
using text::PyRepr;
using text::RStrip;
using text::Strip;
using text::StripQuotes;

constexpr const char* kSource = "fpga-lsp";

// -----------------------------------------------------------------------------
// 命令 / 选项表（Python 常量逐项复制）
// -----------------------------------------------------------------------------

// Python: CLOCK_COMMANDS
constexpr std::array<std::string_view, 3> kClockCommands = {
    "create_clock",
    "create_generated_clock",
    "derive_pll_clocks",
};

// Python: DELAY_COMMANDS
constexpr std::array<std::string_view, 2> kDelayCommands = {
    "set_input_delay",
    "set_output_delay",
};

// Python: GET_PORTS_OPTIONS
constexpr std::array<std::string_view, 4> kGetPortsOptions = {
    "-nocase",
    "-regexp",
    "-filter",
    "-of_objects",
};

// Python: COMMAND_OPTIONS
using OptionSet = std::set<std::string>;
const std::map<std::string, OptionSet>& CommandOptions() {
    static const std::map<std::string, OptionSet> options = {
        {"create_clock",
         {"-name", "-period", "-waveform", "-add", "-comment"},},
        {"create_generated_clock",
         {"-name", "-source", "-master_clock", "-divide_by", "-multiply_by", "-edges",
          "-edge_shift", "-duty_cycle", "-invert", "-add", "-comment",},},
        {"derive_pll_clocks", {"-create_base_clock", "-include_generated_clocks"}},
        {"set_input_delay",
         {"-clock", "-clock_fall", "-reference_pin", "-rise", "-fall", "-max", "-min",
          "-add_delay", "-source_latency_included", "-network_latency_included", "-comment",},},
        {"set_output_delay",
         {"-clock", "-clock_fall", "-reference_pin", "-rise", "-fall", "-max", "-min",
          "-add_delay", "-source_latency_included", "-network_latency_included", "-comment",},},
        {"set_false_path",
         {"-from", "-to", "-through", "-rise_from", "-fall_from", "-rise_to", "-fall_to",
          "-setup", "-hold", "-comment", "-reset_path",},},
        {"set_max_delay",
         {"-from", "-to", "-through", "-rise_from", "-fall_from", "-rise_to", "-fall_to",
          "-rise", "-fall", "-setup", "-hold", "-datapath_only", "-ignore_clock_latency",
          "-comment", "-reset_path",},},
        {"set_min_delay",
         {"-from", "-to", "-through", "-rise_from", "-fall_from", "-rise_to", "-fall_to",
          "-comment", "-reset_path",},},
        {"set_multicycle_path",
         {"-setup", "-hold", "-from", "-to", "-through", "-rise_from", "-fall_from",
          "-rise_to", "-fall_to", "-start", "-end", "-comment", "-reset_path",},},
        {"set_clock_groups",
         {"-asynchronous", "-synchronous", "-physically_exclusive", "-logically_exclusive",
          "-group", "-name", "-comment",},},
        {"set_clock_uncertainty",
         {"-setup", "-hold", "-from", "-to", "-rise_from", "-fall_from", "-rise_to",
          "-fall_to", "-rise", "-fall", "-comment",},},
        {"set_clock_latency",
         {"-source", "-rise", "-fall", "-min", "-max", "-early", "-late", "-clock",
          "-comment",},},
        {"set_clock_transition", {"-rise", "-fall", "-min", "-max", "-comment"}},
        {"set_propagated_clock", {"-comment"}},
        {"set_disable_timing", {"-from", "-to", "-comment"}},
    };
    return options;
}

// Python: _OPTION_RE = ^-[A-Za-z_][A-Za-z0-9_-]*$
const re2::RE2& OptionRe() {
    static const re2::RE2 re(R"(^-[A-Za-z_][A-Za-z0-9_-]*$)");
    return re;
}

// 端口模式的位选择：(-?\d+)\s*:\s*(-?\d+) / -?\d+（Python 用 re.fullmatch）
const re2::RE2& BitRangeRe() {
    static const re2::RE2 re(R"(^(-?[0-9]+)\s*:\s*(-?[0-9]+)$)");
    return re;
}
const re2::RE2& BitIndexRe() {
    static const re2::RE2 re(R"(^-?[0-9]+$)");
    return re;
}


// 行首标识符（补全前缀）
const re2::RE2& LeadingWordRe() {
    static const re2::RE2 re(R"(^\s*([A-Za-z_][A-Za-z0-9_]*))");
    return re;
}

// 已定义时钟名（Python server.py `_complete_sdc` 用的同一正则）
const re2::RE2& ClockNameRe() {
    static const re2::RE2 re(
        R"(create_(?:generated_)?clock\s+(?:[^\n]*?)-name\s+\{?([A-Za-z0-9_.\[\]/-]+))");
    return re;
}

// -----------------------------------------------------------------------------
// 小工具
// -----------------------------------------------------------------------------

lsp::Range RangeOf(int line, int start, int end) {
    lsp::Range range;
    range.start.line = line;
    range.start.character = start;
    range.end.line = line;
    range.end.character = end;
    return range;
}


Diagnostic MakeDiag(std::string code, int severity, const lsp::Range& range, std::string message) {
    Diagnostic diagnostic =
        Diagnostic::AtLine(range.start.line, range.start.character, range.end.character, severity,
                           std::move(code), kSource, std::move(message));
    diagnostic.WithData("origin", Json("sdc"));
    return diagnostic;
}

void SortDiagnostics(std::vector<Diagnostic>& diagnostics) {
    std::ranges::stable_sort(diagnostics,
                     [](const Diagnostic& a, const Diagnostic& b) {
                         if (a.range.start.line != b.range.start.line) {
                             return a.range.start.line < b.range.start.line;
                         }
                         if (a.range.start.character != b.range.start.character) {
                             return a.range.start.character < b.range.start.character;
                         }
                         return a.code < b.code;
                     });
}

// Python: _tok_text —— 花括号/引号取内部文本，其余取原文
std::string TokText(const Token& token) {
    if (token.kind == TokenKind::kBrace || token.kind == TokenKind::kQuote) {
        return token.inner_text;
    }
    return token.text;
}

// Python: _small_span —— 载荷（花括号/引号内）去两端空白后的 span
Span SmallSpan(const Token& token) {
    int start = token.start;
    std::string_view payload = token.text;
    if (token.kind == TokenKind::kBrace || token.kind == TokenKind::kQuote) {
        start = token.inner_start;
        payload = token.inner_text;
    }
    const size_t lead = payload.size() - LStrip(payload).size();
    const size_t tail = RStrip(payload).size();
    return Span{.line=0, .start=start + static_cast<int>(lead), .end=start + static_cast<int>(tail)};
}

// Python: _subword_span —— 载荷内被空白分隔的、光标所在的那个词
std::optional<Span> SubwordSpan(const Token& token, int character) {
    int start = token.start;
    std::string_view payload = token.text;
    if (token.kind == TokenKind::kBrace || token.kind == TokenKind::kQuote) {
        start = token.inner_start;
        payload = token.inner_text;
    }
    // re.finditer(r"\S+", text)
    size_t index = 0;
    while (index < payload.size()) {
        while (index < payload.size() && IsSpace(payload[index])) ++index;
        if (index >= payload.size()) break;
        const size_t begin = index;
        while (index < payload.size() && !IsSpace(payload[index])) ++index;
        const int span_start = start + static_cast<int>(begin);
        const int span_end = start + static_cast<int>(index);
        if (span_start <= character && character <= span_end) {
            return Span{.line=0, .start=span_start, .end=span_end};
        }
    }
    return std::nullopt;
}

int DigitsToInt(std::string_view digits) {
    bool negative = false;
    size_t index = 0;
    if (index < digits.size() && (digits[index] == '-' || digits[index] == '+')) {
        negative = digits[index] == '-';
        ++index;
    }
    long long value = 0;
    for (; index < digits.size(); ++index) {
        const char c = digits[index];
        if (c < '0' || c > '9') continue;
        value = value * 10 + (c - '0');
        if (value > 2147483647LL) return negative ? -2147483647 : 2147483647;
    }
    return static_cast<int>(negative ? -value : value);
}


}  // namespace

// =============================================================================
//  常量表访问
// =============================================================================

// A-5：三张命名空间级 `const std::set<std::string>` 改成 `constexpr std::array<
// std::string_view>` —— 零动态初始化（分配失败 -> 静态初始化期 throw -> terminate），
// 查询走 std::ranges::find（不再为每个元素构造 std::string 节点）。
std::span<const std::string_view> ClockCommands() { return kClockCommands; }
std::span<const std::string_view> DelayCommands() { return kDelayCommands; }
std::span<const std::string_view> GetPortsOptions() { return kGetPortsOptions; }

const std::set<std::string>& KnownCommands() {
    static const std::set<std::string> commands = [] {
        std::set<std::string> names;
        for (const auto& [name, options] : CommandOptions()) names.insert(name);
        return names;
    }();
    return commands;
}

const std::set<std::string>* OptionsFor(std::string_view command) {
    const auto it = CommandOptions().find(std::string(command));
    if (it == CommandOptions().end()) return nullptr;
    return &it->second;
}

// =============================================================================
//  词法
// =============================================================================

TokenizeResult Tokenize(std::string_view text, int offset) {
    TokenizeResult result;
    size_t index = 0;
    const size_t length = text.size();

    // Python 的切片语义：越界自动夹到 [0, size]，且 start > end 时得到空串
    const auto slice = [&text](size_t from, size_t to) {
        const size_t begin = std::min(from, text.size());
        const size_t end = std::min(to, text.size());
        if (end <= begin) return std::string();
        return std::string(text.substr(begin, end - begin));
    };

    while (index < length) {
        const char ch = text[index];
        if (IsSpace(ch)) {
            ++index;
            continue;
        }
        if (ch == ';') {
            Token token;
            token.kind = TokenKind::kSeparator;
            token.text = ";";
            token.inner_text = ";";
            token.start = offset + static_cast<int>(index);
            token.end = offset + static_cast<int>(index) + 1;
            token.inner_start = token.start;
            token.inner_end = token.end;
            result.tokens.push_back(std::move(token));
            ++index;
            continue;
        }
        if (ch == '}') {
            result.errors.push_back({.character=offset + static_cast<int>(index), .message="Unmatched '}'"});
            ++index;
            continue;
        }
        if (ch == ']') {
            result.errors.push_back({.character=offset + static_cast<int>(index), .message="Unmatched ']'"});
            ++index;
            continue;
        }
        if (ch == '{') {
            int depth = 1;
            size_t cursor = index + 1;
            bool closed = false;
            while (cursor < length) {
                const char current = text[cursor];
                if (current == '\\') {
                    cursor += 2;
                    continue;
                }
                if (current == '{') {
                    ++depth;
                } else if (current == '}') {
                    --depth;
                    if (depth == 0) {
                        closed = true;
                        break;
                    }
                }
                ++cursor;
            }
            Token token;
            token.kind = TokenKind::kBrace;
            if (closed) {
                token.text = slice(index, cursor + 1);
                token.inner_text = slice(index + 1, cursor);
                token.start = offset + static_cast<int>(index);
                token.end = offset + static_cast<int>(cursor) + 1;
                token.inner_start = offset + static_cast<int>(index) + 1;
                token.inner_end = offset + static_cast<int>(cursor);
                index = cursor + 1;
            } else {
                result.errors.push_back({.character=offset + static_cast<int>(index), .message="Unclosed '{'"});
                token.text = slice(index, length);
                token.inner_text = slice(index + 1, length);
                token.start = offset + static_cast<int>(index);
                token.end = offset + static_cast<int>(length);
                token.inner_start = offset + static_cast<int>(index) + 1;
                token.inner_end = offset + static_cast<int>(length);
                token.closed = false;
                index = length;
            }
            result.tokens.push_back(std::move(token));
            continue;
        }
        if (ch == '[') {
            int depth = 1;
            int braces = 0;
            bool quoted = false;
            size_t cursor = index + 1;
            bool closed = false;
            while (cursor < length) {
                const char current = text[cursor];
                if (current == '\\') {
                    cursor += 2;
                    continue;
                }
                if (quoted) {
                    if (current == '"') quoted = false;
                } else if (current == '"') {
                    quoted = true;
                } else if (braces) {
                    if (current == '{') {
                        ++braces;
                    } else if (current == '}') {
                        --braces;
                    }
                } else if (current == '{') {
                    braces = 1;
                } else if (current == '[') {
                    ++depth;
                } else if (current == ']') {
                    --depth;
                    if (depth == 0) {
                        closed = true;
                        break;
                    }
                }
                ++cursor;
            }
            Token token;
            token.kind = TokenKind::kBracket;
            if (closed) {
                token.text = slice(index, cursor + 1);
                token.inner_text = slice(index + 1, cursor);
                token.start = offset + static_cast<int>(index);
                token.end = offset + static_cast<int>(cursor) + 1;
                token.inner_start = offset + static_cast<int>(index) + 1;
                token.inner_end = offset + static_cast<int>(cursor);
                index = cursor + 1;
            } else {
                result.errors.push_back({.character=offset + static_cast<int>(index), .message="Unclosed '['"});
                token.text = slice(index, length);
                token.inner_text = slice(index + 1, length);
                token.start = offset + static_cast<int>(index);
                token.end = offset + static_cast<int>(length);
                token.inner_start = offset + static_cast<int>(index) + 1;
                token.inner_end = offset + static_cast<int>(length);
                token.closed = false;
                index = length;
            }
            result.tokens.push_back(std::move(token));
            continue;
        }
        if (ch == '"') {
            size_t cursor = index + 1;
            bool closed = false;
            while (cursor < length) {
                const char current = text[cursor];
                if (current == '\\') {
                    cursor += 2;
                    continue;
                }
                if (current == '"') {
                    closed = true;
                    break;
                }
                ++cursor;
            }
            Token token;
            token.kind = TokenKind::kQuote;
            // Python 对未闭合的引号**不报错**，只把 closed 置 False
            if (closed) {
                token.text = slice(index, cursor + 1);
                token.inner_text = slice(index + 1, cursor);
                token.start = offset + static_cast<int>(index);
                token.end = offset + static_cast<int>(cursor) + 1;
                token.inner_start = offset + static_cast<int>(index) + 1;
                token.inner_end = offset + static_cast<int>(cursor);
                index = cursor + 1;
            } else {
                token.text = slice(index, length);
                token.inner_text = slice(index + 1, length);
                token.start = offset + static_cast<int>(index);
                token.end = offset + static_cast<int>(length);
                token.inner_start = offset + static_cast<int>(index) + 1;
                token.inner_end = offset + static_cast<int>(length);
                token.closed = false;
                index = length;
            }
            result.tokens.push_back(std::move(token));
            continue;
        }

        // 裸词
        size_t cursor = index;
        while (cursor < length) {
            const char current = text[cursor];
            if (IsSpace(current) || current == ';' || current == ']' || current == '}') break;
            if (current == '\\') {
                cursor += 2;
                continue;
            }
            ++cursor;
        }
        const size_t clamped = std::min(cursor, length);
        Token token;
        token.kind = TokenKind::kWord;
        token.text = slice(index, clamped);
        token.inner_text = token.text;
        token.start = offset + static_cast<int>(index);
        token.end = offset + static_cast<int>(clamped);
        token.inner_start = token.start;
        token.inner_end = token.end;
        result.tokens.push_back(std::move(token));
        index = std::max(clamped, index + 1);
    }
    return result;
}

namespace {

// Python: _logical_lines —— 反斜杠续行拼成一条逻辑行
struct LogicalLine {
    int line = 0;  // 首物理行号
    std::string text;
};

std::vector<LogicalLine> LogicalLines(std::string_view text) {
    const std::vector<std::string_view> physical = text::SplitLines(text);
    std::vector<LogicalLine> out;
    size_t index = 0;
    while (index < physical.size()) {
        const size_t start = index;
        std::string buffer(physical[index]);
        while (true) {
            const std::string_view stripped = RStrip(buffer);
            size_t backslashes = 0;
            while (backslashes < stripped.size() &&
                   stripped[stripped.size() - 1 - backslashes] == '\\') {
                ++backslashes;
            }
            if (backslashes % 2 == 1 && index + 1 < physical.size()) {
                buffer = std::string(stripped.substr(0, stripped.size() - 1)) + " " +
                         std::string(physical[index + 1]);
                ++index;
            } else {
                break;
            }
        }
        out.push_back(LogicalLine{.line=static_cast<int>(start), .text=std::move(buffer)});
        ++index;
    }
    return out;
}

// Python: _strip_comment —— 去掉 Tcl 注释，但尊重 {} / [] / "" / 转义
std::string StripComment(std::string_view line) {
    int braces = 0;
    int brackets = 0;
    bool quoted = false;
    bool escaped = false;
    for (size_t index = 0; index < line.size(); ++index) {
        const char ch = line[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (quoted) {
            if (ch == '"') quoted = false;
            continue;
        }
        if (ch == '"') {
            quoted = true;
            continue;
        }
        if (braces) {
            if (ch == '{') {
                ++braces;
            } else if (ch == '}') {
                --braces;
            }
            continue;
        }
        if (ch == '{') {
            braces = 1;
            continue;
        }
        if (ch == '[') {
            ++brackets;
            continue;
        }
        if (ch == ']') {
            if (brackets) --brackets;
            continue;
        }
        if (ch == '#' && brackets == 0) {
            const char previous = (index > 0) ? line[index - 1] : '\0';
            if (index == 0 || previous == ' ' || previous == '\t' || previous == ';') {
                return std::string(line.substr(0, index));
            }
        }
    }
    return std::string(line);
}

std::vector<std::vector<Token>> SplitCommands(const std::vector<Token>& tokens) {
    std::vector<std::vector<Token>> groups;
    std::vector<Token> current;
    for (const Token& token : tokens) {
        if (token.kind == TokenKind::kSeparator) {
            if (!current.empty()) groups.push_back(current);
            current.clear();
        } else {
            current.push_back(token);
        }
    }
    if (!current.empty()) groups.push_back(current);
    return groups;
}

Command MakeCommand(const std::vector<Token>& tokens, int line, const std::string& raw) {
    Command command;
    command.line = line;
    command.raw = raw;
    if (!tokens.empty()) {
        const Token& first = tokens.front();
        command.command = TokText(first);
        command.command_span =
            Span{.line=line, .start=first.start, .end=first.end};
        command.arg_tokens.assign(tokens.begin() + 1, tokens.end());
        command.args.reserve(command.arg_tokens.size());
        for (const Token& token : command.arg_tokens) command.args.push_back(TokText(token));
    } else {
        command.command_span = Span{.line=line, .start=0, .end=0};
    }
    return command;
}

}  // namespace

std::vector<Command> Parse(std::string_view text) {
    std::vector<Command> commands;
    for (const LogicalLine& logical : LogicalLines(text)) {
        const std::string stripped = StripComment(logical.text);
        if (Strip(stripped).empty()) continue;

        const TokenizeResult tokenized = Tokenize(stripped);
        std::vector<size_t> added;  // 本条逻辑行新加的命令下标
        for (const std::vector<Token>& group : SplitCommands(tokenized.tokens)) {
            added.push_back(commands.size());
            commands.push_back(MakeCommand(group, logical.line, stripped));
        }
        for (const SyntaxError& error : tokenized.errors) {
            int target_index = -1;
            for (const size_t index : added) {
                if (!commands[index].command.empty()) {
                    target_index = static_cast<int>(index);
                    break;
                }
            }
            if (target_index < 0) {
                // 没有任何命令可挂：造一条空命令专门承载语法错误
                Command synthetic;
                synthetic.line = logical.line;
                synthetic.raw = stripped;
                synthetic.command_span = Span{.line=logical.line, .start=0, .end=0};
                commands.push_back(std::move(synthetic));
                target_index = static_cast<int>(commands.size()) - 1;
                added.push_back(static_cast<size_t>(target_index));
            }
            SyntaxError item;
            item.character = error.character;
            item.message = error.message.empty() ? "Tcl bracket mismatch" : error.message;
            commands[static_cast<size_t>(target_index)].syntax.push_back(std::move(item));
        }
    }
    return commands;
}

// =============================================================================
//  诊断
// =============================================================================

namespace {

// Python: _with_tokens —— 手工构造的命令（只有 args）也能进 diagnostics
Command WithTokens(Command command) {
    if (!command.arg_tokens.empty()) return command;
    if (command.args.empty()) return command;

    const int line = command.line;
    const std::string& raw = command.raw;
    size_t cursor = 0;
    std::vector<Token> tokens;
    for (const std::string& argument : command.args) {
        Token token;
        token.kind = TokenKind::kWord;
        token.text = argument;
        token.inner_text = argument;
        if (argument.size() >= 2 && argument.front() == '[' && argument.back() == ']') {
            token.kind = TokenKind::kBracket;
            token.inner_text = argument.substr(1, argument.size() - 2);
        }
        size_t position = raw.find(argument, cursor);
        if (position == std::string::npos) position = raw.find(argument);
        if (position == std::string::npos) position = cursor;
        const int start = static_cast<int>(position);
        const int end = start + static_cast<int>(argument.size());
        cursor = static_cast<size_t>(end);
        token.start = start;
        token.end = end;
        token.inner_start = (token.kind == TokenKind::kBracket) ? start + 1 : start;
        token.inner_end = (token.kind == TokenKind::kBracket) ? end - 1 : end;
        tokens.push_back(std::move(token));
    }
    command.arg_tokens = std::move(tokens);
    // Python: command_span 不是 dict 时才补。C++ 侧无法表示"缺失"，用退化 span
    // （end <= start）作为等价判据。
    if (command.command_span.end <= command.command_span.start) {
        const size_t found = raw.find(command.command);
        const int start = (found == std::string::npos) ? 0 : static_cast<int>(found);
        command.command_span =
            Span{.line=line, .start=start, .end=start + static_cast<int>(command.command.size())};
    }
    return command;
}

lsp::Range CommandRange(const Command& command) {
    return RangeOf(command.line, 0, static_cast<int>(command.raw.size()));
}

lsp::Range TokenRange(const Command& command, const Token* token) {
    if (token == nullptr) return CommandRange(command);
    const Span span = SmallSpan(*token);
    return RangeOf(command.line, span.start, span.end);
}

int OptionIndex(const Command& command, std::string_view name) {
    for (size_t index = 0; index < command.arg_tokens.size(); ++index) {
        if (TokText(command.arg_tokens[index]) == name) return static_cast<int>(index);
    }
    return -1;
}

struct PortPattern {
    std::string pattern;
    int start = 0;
    int end = 0;
};

// Python: _patterns_from —— 从 get_ports / get_clocks 之后取端口名
//
// 关键行为（都是刻意的保守设计）：
//   · `-regexp` 之后直接放弃（正则无法按端口名校验）
//   · `-filter` / `-of_objects` 跳过它的参数
//   · 其它未知 `-option` 直接停止
void PatternsFrom(const std::vector<Token>& tokens, size_t start_index,
                  std::vector<PortPattern>* out) {
    bool skip_next = false;
    for (size_t index = start_index; index < tokens.size(); ++index) {
        const Token& token = tokens[index];
        if (token.kind == TokenKind::kSeparator) break;
        const std::string text = TokText(token);
        if (skip_next) {
            skip_next = false;
            continue;
        }
        if (!text.empty() && text.front() == '-') {
            if (text == "-regexp") return;
            if (text == "-filter" || text == "-of_objects") {
                skip_next = true;
                continue;
            }
            if (std::ranges::find(kGetPortsOptions, std::string_view(text)) !=
                kGetPortsOptions.end())
                continue;
            return;
        }
        if (token.kind == TokenKind::kBrace || token.kind == TokenKind::kQuote) {
            // 组内按空白切成多个词，span 相对原文
            const std::string& inner = token.inner_text;
            const int base = token.inner_start;
            size_t cursor = 0;
            while (cursor < inner.size()) {
                while (cursor < inner.size() && IsSpace(inner[cursor])) ++cursor;
                if (cursor >= inner.size()) break;
                const size_t begin = cursor;
                while (cursor < inner.size() && !IsSpace(inner[cursor])) ++cursor;
                out->push_back(PortPattern{.pattern=inner.substr(begin, cursor - begin),
                                           .start=base + static_cast<int>(begin),
                                           .end=base + static_cast<int>(cursor),});
            }
        } else if (token.kind == TokenKind::kWord) {
            out->push_back(PortPattern{.pattern=text, .start=token.start, .end=token.end});
        }
    }
}

// Python: _iter_get_ports
void IterGetPorts(const Command& command, std::vector<PortPattern>* out) {
    if (command.command == "get_ports") {
        PatternsFrom(command.arg_tokens, 0, out);
        return;
    }
    for (const Token& token : command.arg_tokens) {
        if (token.kind != TokenKind::kBracket) continue;
        const TokenizeResult inner = Tokenize(token.inner_text, token.inner_start);
        for (size_t index = 0; index < inner.tokens.size(); ++index) {
            if (TokText(inner.tokens[index]) == "get_ports") {
                PatternsFrom(inner.tokens, index + 1, out);
                break;
            }
        }
    }
}

// Python: _iter_get_clocks（本工程新增，供 SDC-CLOCK 使用）
void IterGetClocks(const Command& command, std::vector<PortPattern>* out) {
    if (command.command == "get_clocks") {
        PatternsFrom(command.arg_tokens, 0, out);
        return;
    }
    for (const Token& token : command.arg_tokens) {
        if (token.kind != TokenKind::kBracket) continue;
        const TokenizeResult inner = Tokenize(token.inner_text, token.inner_start);
        for (size_t index = 0; index < inner.tokens.size(); ++index) {
            if (TokText(inner.tokens[index]) == "get_clocks") {
                PatternsFrom(inner.tokens, index + 1, out);
                break;
            }
        }
    }
}

using PortLookup = std::unordered_map<std::string, const Port*>;

PortLookup BuildLookup(const std::vector<Port>& ports) {
    PortLookup lookup;
    lookup.reserve(ports.size());
    for (const Port& port : ports) {
        if (port.name.empty()) continue;
        lookup.emplace(port.name, &port);  // 重名时第一个胜出
    }
    return lookup;
}

// Python: _valid_port_pattern —— true 合法 / false 一定非法 / nullopt 无法判断
std::optional<bool> ValidPortPattern(std::string_view pattern, const PortLookup& lookup) {
    const std::string text(StripQuotes(Strip(pattern)));
    if (text.empty()) return std::nullopt;

    if (text.contains('[')) {
        const size_t bracket = text.find('[');
        const std::string base(Strip(std::string_view(text).substr(0, bracket)));
        const std::string rest = text.substr(bracket + 1);
        const size_t close = rest.rfind(']');
        const std::string spec(Strip(close == std::string::npos ? std::string_view(rest)
                                                                : std::string_view(rest).substr(0, close)));
        const auto it = lookup.find(base);
        if (it == lookup.end()) return false;
        const Port* port = it->second;
        if (spec == "*" || spec.empty()) return true;

        const std::vector<int> bits = PortBits(*port);
        const auto contains = [&bits](int bit) {
            return std::ranges::find(bits, bit) != bits.end();
        };

        {
            absl::string_view input(spec.data(), spec.size());
            absl::string_view first;
            absl::string_view second;
            if (re2::RE2::FullMatch(input, BitRangeRe(), &first, &second)) {
                const int a = DigitsToInt({first.data(), first.size()});
                const int b = DigitsToInt({second.data(), second.size()});
                const int low = std::min(a, b);
                const int high = std::max(a, b);
                if (bits.empty()) return false;
                for (int bit = low; bit <= high; ++bit) {
                    if (!contains(bit)) return false;
                }
                return true;
            }
        }
        {
            absl::string_view input(spec.data(), spec.size());
            if (re2::RE2::FullMatch(input, BitIndexRe())) {
                return contains(DigitsToInt(spec));
            }
        }
        return std::nullopt;  // 位选择写法不认识 -> 不判断
    }

    if (text.contains('*') || text.contains('?')) {
        // Python: re.escape(text).replace("\\*", ".*").replace("\\?", ".")
        // 即：除通配符外全部字面转义
        std::string regex;
        for (const char c : text) {
            if (c == '*') {
                regex += ".*";
            } else if (c == '?') {
                regex += '.';
            } else {
                regex += re2::RE2::QuoteMeta(std::string(1, c));
            }
        }
        const re2::RE2 compiled(regex);
        if (!compiled.ok()) return std::nullopt;  // 理论上不会发生；不判断比误报好
        for (const auto& entry : lookup) {
            if (re2::RE2::FullMatch(entry.first, compiled)) return true;
        }
        return false;
    }

    if (text.contains('$') || text.contains('(')) {
        return std::nullopt;  // 变量替换 / 表达式：不判断
    }
    return lookup.contains(text);
}

bool IsOptionToken(const std::string& text) {
    const absl::string_view input(text.data(), text.size());
    return re2::RE2::FullMatch(input, OptionRe());
}

// 解析 create_clock 的 -period 值（Python: float(value)，失败 -> None）
std::optional<double> TokNumber(const Token* token) {
    if (token == nullptr) return std::nullopt;
    return PyFloat(TokText(*token));
}

}  // namespace

std::vector<Diagnostic> Diagnostics(const std::vector<Command>& commands,
                                    const std::vector<Port>& ports,
                                    const std::set<std::string>* known_commands) {
    std::vector<Diagnostic> result;
    const PortLookup lookup = BuildLookup(ports);
    bool clock_present = false;

    for (const Command& raw_command : commands) {
        const Command command = WithTokens(raw_command);
        const std::string name = command.command;
        const int line = command.line;

        // ---- 词法错误 ----
        for (const SyntaxError& error : command.syntax) {
            const int character = std::max(0, error.character);
            result.push_back(MakeDiag(
                "SDC-SYNTAX", kError, RangeOf(line, character, character + 1),
                error.message.empty() ? "Tcl bracket mismatch" : error.message));
        }

        if (name.empty()) continue;
        if (std::ranges::find(kClockCommands, std::string_view(name)) != kClockCommands.end())
            clock_present = true;

        // ---- 未知命令（只有调用方给了 known_commands 才判断）----
        if (known_commands != nullptr && !known_commands->contains(name)) {
            result.push_back(
                MakeDiag("SDC-CMD", kWarning, CommandRange(command), "Unknown SDC command " + name));
        }

        // ---- 不认识的选项 ----
        if (const std::set<std::string>* options = OptionsFor(name)) {
            for (const Token& token : command.arg_tokens) {
                const std::string text = TokText(token);
                if (IsOptionToken(text) && !options->contains(text)) {
                    result.push_back(MakeDiag(
                        "SDC-OPT", kInfo, TokenRange(command, &token),
                        std::format("Command {} does not support option {}", name, text)));
                }
            }
        }

        // ---- create_clock -period ----
        if (name == "create_clock") {
            const int index = OptionIndex(command, "-period");
            if (index < 0) {
                result.push_back(MakeDiag("SDC-PERIOD", kError, CommandRange(command),
                                          "create_clock is missing -period"));
            } else {
                const Token* value_token = (static_cast<size_t>(index) + 1 <
                                            command.arg_tokens.size())
                                               ? &command.arg_tokens[static_cast<size_t>(index) + 1]
                                               : nullptr;
                const std::string value = (value_token != nullptr) ? TokText(*value_token) : "";
                const std::optional<double> number = TokNumber(value_token);
                // ⚠ 忠实复现 Python：`-period nan` 时 nan <= 0 为 False，**不报错**。
                //   这是 Python 的缺陷，已登记 PYTHON-BUGS.md；修掉时此测试会变红。
                // 必须写成 <= 0（而不是 !(> 0)）：NaN 与任何数比较都为 false，
                // 所以 Python 的 number <= 0 会放行 -period nan，这才是 P-11 的真实行为。
                if (!number.has_value() || *number <= 0) {
                    result.push_back(MakeDiag(
                        "SDC-PERIOD", kError, TokenRange(command, value_token),
                        "Invalid create_clock -period value: " + PyRepr(value)));
                }
            }
        }

        // ---- set_input_delay / set_output_delay 必须有 -clock ----
        if (std::ranges::find(kDelayCommands, std::string_view(name)) != kDelayCommands.end() &&
            OptionIndex(command, "-clock") < 0) {
            result.push_back(MakeDiag("SDC-DELAY", kWarning, CommandRange(command),
                                      name + " is missing the -clock option"));
        }

        // ---- get_ports 里的端口是否存在 ----
        std::vector<PortPattern> patterns;
        IterGetPorts(command, &patterns);
        for (const PortPattern& pattern : patterns) {
            if (lookup.empty()) break;  // 没有顶层端口表 -> 不判断（单独预览 .sdc）
            const std::optional<bool> valid = ValidPortPattern(pattern.pattern, lookup);
            if (valid.has_value() && !*valid) {
                result.push_back(MakeDiag("SDC-PORT", kError,
                                          RangeOf(line, pattern.start, pattern.end),
                                          "Port " + pattern.pattern +
                                              " does not exist in the top module"));
            }
        }
    }

    // ---- 全文件没有时钟约束 ----
    if (!clock_present) {
        result.push_back(MakeDiag("SDC-NOCLOCK", kWarning, RangeOf(0, 0, 0),
                                  "No clock constraints in the file (create_clock / "
                                  "derive_pll_clocks)"));
    }

    SortDiagnostics(result);
    return result;
}

// =============================================================================
//  附加诊断：引用但未定义的时钟名（Python 版没有）
// =============================================================================

std::vector<Diagnostic> UndefinedClockDiagnostics(const std::vector<Command>& commands) {
    std::vector<Diagnostic> result;
    std::set<std::string> defined;
    for (const Command& command : commands) {
        if (command.command != "create_clock" && command.command != "create_generated_clock") {
            continue;
        }
        const int index = OptionIndex(command, "-name");
        if (index < 0) continue;
        if (static_cast<size_t>(index) + 1 >= command.arg_tokens.size()) continue;
        const std::string name(Strip(TokText(command.arg_tokens[static_cast<size_t>(index) + 1])));
        if (!name.empty()) defined.insert(name);
    }
    // 一条 create_clock / create_generated_clock 都没有 -> 不做判断。
    // 理由：时钟可能由 derive_pll_clocks 自动生成，或来自被 include 的其它 sdc；
    // 在这种情况下报"时钟未定义"是**误报**，比漏报有害得多。
    if (defined.empty()) return result;

    const auto describe = [](const std::string& name) {
        return "Clock " + name + " is referenced but never defined (create_clock -name " + name +
               ")";
    };
    const auto looks_like_pattern = [](std::string_view name) {
        return name.contains('*') ||
               name.contains('?');
    };

    for (const Command& command : commands) {
        // 1) -clock <name> / -master_clock <name> 的纯值形式
        for (const char* option : {"-clock", "-master_clock"}) {
            const int index = OptionIndex(command, option);
            if (index < 0) continue;
            if (static_cast<size_t>(index) + 1 >= command.arg_tokens.size()) continue;
            const Token& token = command.arg_tokens[static_cast<size_t>(index) + 1];
            if (token.kind == TokenKind::kBracket) continue;  // [get_clocks {...}] 走下面
            const std::string name(Strip(TokText(token)));
            if (name.empty() || looks_like_pattern(name)) continue;
            if (defined.contains(name)) continue;
            const Span span = SmallSpan(token);
            result.push_back(MakeDiag("SDC-CLOCK", kWarning,
                                      RangeOf(command.line, span.start, span.end),
                                      describe(name)));
        }
        // 2) [get_clocks {...}] 里的名字
        std::vector<PortPattern> patterns;
        IterGetClocks(command, &patterns);
        for (const PortPattern& pattern : patterns) {
            const std::string name(Strip(pattern.pattern));
            if (name.empty() || looks_like_pattern(name) ||
                name.contains('[')) {
                continue;
            }
            if (defined.contains(name)) continue;
            result.push_back(MakeDiag("SDC-CLOCK", kWarning,
                                      RangeOf(command.line, pattern.start, pattern.end),
                                      describe(name)));
        }
    }
    SortDiagnostics(result);
    return result;
}

// =============================================================================
//  光标上下文
// =============================================================================

namespace {

// 嵌套展开的深度上限。
// 真实 SDC 里 `[...]` 的嵌套深度远小于此（常见 2~3 层，`[get_ports {...}]` 就一层），
// 所以这个上限只会挡住恶意/损坏的输入，不会影响任何正常约束文件。
// 为什么必须有：`[` 的嵌套在 Tokenize 里是扁平的（一个 token 装下整段），
// 到这里才逐层递归展开 —— 一个含 20 万层 `[` 的文件会把递归栈直接打爆，
// 结果是整个 LSP 服务器进程崩溃（编辑器侧表现为语言服务反复重启）。
// 参照：xml_lite.cpp 深度上限 512、HDL 树 depth 24 + budget，同一类防护。
constexpr int kMaxFlattenDepth = 64;

// 就地展开：`*flat` 是累加结果，避免每层递归都新建 vector 再整段 copy（O(n·depth)）。
void FlattenInto(const std::vector<Token>& tokens, int depth, std::vector<Token>* flat) {
    for (const Token& token : tokens) {
        if (token.kind == TokenKind::kBracket) {
            if (depth >= kMaxFlattenDepth) {
                // 超过上限：**有意降级** —— 把这个 kBracket token 原样收进结果
                // （保留 token 身份与 span），不再往里递归展开。
                // 后果只限于"光标落在超深层嵌套里时拿不到上下文/补全"，
                // 这是可接受的，远好于栈溢出把进程带崩；也绝不静默丢弃 token。
                flat->push_back(token);
                continue;
            }
            const TokenizeResult inner = Tokenize(token.inner_text, token.inner_start);
            FlattenInto(inner.tokens, depth + 1, flat);
        } else if (token.kind == TokenKind::kSeparator) {
            continue;
        } else {
            flat->push_back(token);
        }
    }
}

std::vector<Token> Flatten(const std::vector<Token>& tokens) {
    std::vector<Token> flat;
    FlattenInto(tokens, 0, &flat);
    return flat;
}

CursorContext PortContext(int line, const Token& token, int character) {
    std::optional<Span> sub = SubwordSpan(token, character);
    if (!sub.has_value()) sub = SmallSpan(token);

    const std::string text = TokText(token);
    const std::string_view prefix = text::LeadingIdentifier(text);
    if (!prefix.empty() && text.contains('[')) {
        const int length = static_cast<int>(prefix.size());
        // 语义上的端口是基名（`pwmG`），不是位选择（`pwmG[3:0]`）
        if (sub->start + length <= sub->end) {
            sub = Span{.line=line, .start=sub->start, .end=sub->start + length};
        }
    }
    CursorContext context;
    context.kind = "port";
    context.span = Span{.line=line, .start=sub->start, .end=sub->end};
    return context;
}

CursorContext ClockNameContext(int line, const Token& token, int character) {
    CursorContext context;
    context.kind = "clock_name";
    std::optional<Span> sub = SubwordSpan(token, character);
    if (!sub.has_value()) sub = SmallSpan(token);
    context.span = Span{.line=line, .start=sub->start, .end=sub->end};
    return context;
}

}  // namespace

std::optional<CursorContext> Context(std::string_view text, int line, int character) {
    if (line < 0 || character < 0) return std::nullopt;
    const std::vector<std::string_view> lines = text::SplitLines(text);
    if (static_cast<size_t>(line) >= lines.size()) return std::nullopt;

    const std::string raw = StripComment(lines[static_cast<size_t>(line)]);
    const TokenizeResult tokenized = Tokenize(raw);
    if (tokenized.tokens.empty()) return std::nullopt;

    const Token& first = tokenized.tokens.front();
    if (first.start <= character && character <= first.end) {
        CursorContext context;
        context.kind = "command";
        context.span = Span{.line=line, .start=first.start, .end=first.end};
        return context;
    }

    const std::vector<Token> rest(tokenized.tokens.begin() + 1, tokenized.tokens.end());
    const std::vector<Token> flat = Flatten(rest);
    const Token* previous = nullptr;
    bool in_get_ports = false;

    for (const Token& token : flat) {
        const std::string text_value = TokText(token);
        if (in_get_ports && !text_value.empty() && text_value.front() == '-') in_get_ports = false;

        if (token.start <= character && character <= token.end) {
            const std::string previous_text = (previous != nullptr) ? TokText(*previous) : "";
            if (previous_text == "-name") return ClockNameContext(line, token, character);
            if (text_value == "get_ports") {
                CursorContext context;
                context.kind = "command";
                context.span = Span{.line=line, .start=token.start, .end=token.end};
                return context;
            }
            if (token.kind == TokenKind::kWord && !text_value.empty() &&
                text_value.front() == '-') {
                CursorContext context;
                context.kind = "option";
                context.span = Span{.line=line, .start=token.start, .end=token.end};
                context.command = TokText(first);
                return context;
            }
            if (previous_text == "get_clocks" || previous_text == "-clock") {
                return ClockNameContext(line, token, character);
            }
            if (in_get_ports) return PortContext(line, token, character);
            return std::nullopt;
        }

        if (text_value == "get_ports") {
            in_get_ports = true;
        } else if (token.kind == TokenKind::kWord && !text_value.empty() &&
                   text_value.front() == '-') {
            in_get_ports = false;
        }
        previous = &token;
    }
    return std::nullopt;
}

// =============================================================================
//  补全
// =============================================================================

std::vector<std::string> DefinedClockNames(std::string_view text) {
    std::vector<std::string> names;
    absl::string_view rest(text.data(), text.size());
    re2::StringPiece captured;
    // 注意：RE2 的变参只对应**捕获组**（1..n），这里正则只有 1 个捕获组，
    // 所以只能传 1 个参数；传 2 个会让 FindAndConsume 直接返回 false。
    while (!rest.empty()) {
        re2::RE2::Arg arg_name(&captured);
        const re2::RE2::Arg* args[1] = {&arg_name};
        const size_t before = rest.size();
        if (!re2::RE2::FindAndConsumeN(&rest, ClockNameRe(), args, 1)) break;
        if (rest.size() >= before) break;  // 防止零长匹配导致死循环
        const std::string name(captured.data(), captured.size());
        if (name.empty()) continue;
        if (std::ranges::find(names, name) == names.end()) names.push_back(name);
    }
    return names;
}

std::vector<CompletionItem> Completion(std::string_view text, int line, int character,
                                       const std::set<std::string>& known_commands,
                                       const std::vector<Port>& ports) {
    std::vector<CompletionItem> items;
    const std::optional<CursorContext> context = Context(text, line, character);
    if (!context.has_value()) return items;

    if (context->kind == "command") {
        // 前缀来自行首标识符（Python: re.match(r"^\s*([A-Za-z_]\w*)", row)）
        std::string prefix;
        const std::vector<std::string_view> lines = text::SplitLines(text);
        if (static_cast<size_t>(line) < lines.size()) {
            const std::string row(lines[static_cast<size_t>(line)]);
            absl::string_view input(row.data(), row.size());
            absl::string_view captured;
            if (re2::RE2::Consume(&input, LeadingWordRe(), &captured) && !captured.empty()) {
                prefix = LowerAscii({captured.data(), captured.size()});
            }
        }
        for (const std::string& name : known_commands) {
            const std::string lowered = LowerAscii(name);
            if (!prefix.empty() && !lowered.starts_with(prefix)) continue;
            items.push_back(CompletionItem{.label=name, .detail="SDC command", .kind="command"});
        }
        return items;
    }

    if (context->kind == "clock_name") {
        for (const std::string& name : DefinedClockNames(text)) {
            items.push_back(CompletionItem{.label=name, .detail="clock", .kind="clock"});
        }
        return items;
    }

    if (context->kind == "port") {
        for (const Port& port : ports) {
            if (port.name.empty()) continue;
            // 用 std::format 而不是定长缓冲 + snprintf：端口方向来自用户 Verilog，
            // 被 char[96] 截断时可能正好切在多字节 UTF-8 序列中间，
            // 非法 UTF-8 会直接进 LSP 的 CompletionItem.detail。
            const std::string detail =
                std::format("{} · {} bit", port.direction.empty() ? "?" : port.direction.c_str(),
                            port.width.value_or(1));
            items.push_back(CompletionItem{.label=port.name, .detail=detail, .kind="port"});
        }
        return items;
    }

    if (context->kind == "option") {
        if (const std::set<std::string>* options = OptionsFor(context->command)) {
            for (const std::string& option : *options) {
                items.push_back(CompletionItem{.label=option, .detail=context->command, .kind="option"});
            }
        }
        return items;
    }
    return items;
}

// =============================================================================
//  TD 命令表（vendored）
// =============================================================================

namespace {

#include "constraints/data/td_commands.inc"

}  // namespace

std::span<const CommandUsage> CommandUsages() {
    return {kSdcCommandUsages,
                                         std::size(kSdcCommandUsages)};
}

// =============================================================================
//  LSP 符号 / hover / 补全（对应 server.py 的 .sdc 分支）
// =============================================================================
//  纯函数：只吃「文本 + 光标位置（+ 可选顶层端口表）」，不读磁盘、不抛异常。
//  形状逐字段对齐 Python：
//    · on_textDocument_documentSymbol 的 `.sdc` 分支（top.sdc -> 2 条）
//    · _hover_sdc（命令 hover 用 TD help 的 usage 原文）
//    · _complete_sdc
// =============================================================================

namespace {

Json MakeRangeJson(int line, int start, int end) {
    Json range = Json::object();
    Json start_json = Json::object();
    start_json["line"] = line;
    start_json["character"] = start;
    Json end_json = Json::object();
    end_json["line"] = line;
    end_json["character"] = end;
    range["start"] = std::move(start_json);
    range["end"] = std::move(end_json);
    return range;
}

// Python: `max(1, len(row))`
int RowLength(std::string_view text, int line) {
    const int length = static_cast<int>(text::LineAt(text, line).size());
    return length > 1 ? length : 1;
}

// server.py `_ctx_word`：优先按解析器给的 span 切词
std::string ContextWord(std::string_view text, const std::optional<CursorContext>& context) {
    if (!context.has_value()) return {};
    const Span& span = context->span;
    const std::string_view row = text::LineAt(text, span.line);
    if (span.start >= 0 && span.start < span.end &&
        static_cast<std::size_t>(span.end) <= row.size()) {
        return std::string(row.substr(static_cast<std::size_t>(span.start),
                                      static_cast<std::size_t>(span.end - span.start)));
    }
    return {};
}

// Python: `commands.get(word.lower())` —— 注意键是**原样**（可能含大写），
// 所以 `JTAG_REG_ACCESS` 这类命令永远查不到（忠实复现）。
const CommandUsage* FindCommand(const std::string& lowered_word) {
    for (const CommandUsage& row : CommandUsages()) {
        if (row.name == lowered_word) return &row;
    }
    return nullptr;
}

// LSP CompletionItemKind
constexpr int kKindFunction = 3;
constexpr int kKindField = 5;
constexpr int kKindProperty = 10;
constexpr int kKindKeyword = 14;
constexpr int kKindConstant = 21;

constexpr std::size_t kMaxCompletionItems = 1500;
constexpr std::size_t kUsageDetailLength = 120;  // Python: `usage[:120]`

std::string TruncatedUsage(std::string_view usage) {
    if (usage.size() <= kUsageDetailLength) return std::string(usage);
    return std::string(usage.substr(0, kUsageDetailLength));
}

Json CompletionItemJson(const std::string& label, int kind, const std::string& detail) {
    Json item = Json::object();
    item["label"] = label;
    item["kind"] = kind;
    item["detail"] = detail;
    return item;
}

}  // namespace

Json DocumentSymbols(std::string_view text) {
    Json out = Json::array();
    for (const Command& command : Parse(text)) {
        const int line = command.line;
        std::string detail;
        for (std::size_t i = 0; i < command.args.size(); ++i) {
            if (i != 0) detail += ' ';
            detail += command.args[i];
        }
        if (detail.size() > 60) detail.resize(60);  // Python: `" ".join(args)[:60]`

        const std::string name = command.command.empty() ? "?" : command.command;
        Json symbol = Json::object();
        symbol["name"] = name;
        symbol["detail"] = std::move(detail);
        symbol["kind"] = kKindFunction;  // Python 侧写死 3
        symbol["range"] = MakeRangeJson(line, 0, RowLength(text, line));
        const int name_length = static_cast<int>(name.size());
        symbol["selectionRange"] =
            MakeRangeJson(line, 0, name_length > 1 ? name_length : 1);
        out.push_back(std::move(symbol));
    }
    return out;
}

Json Hover(std::string_view text, int line, int character, const std::vector<Port>& ports) {
    const std::optional<CursorContext> context = Context(text, line, character);
    if (!context.has_value()) return Json(nullptr);
    const Span span = context->span;
    const Json range = MakeRangeJson(span.line, span.start, span.end);

    if (context->kind == "command") {
        const std::string word = ContextWord(text, context);
        if (!word.empty()) {
            if (const CommandUsage* info = FindCommand(LowerAscii(word))) {
                std::string value = "**" + std::string(info->name) + "**\n\n```\n";
                value += info->usage.empty() ? std::string() : std::string(info->usage);
                value += "\n```";
                Json contents = Json::object();
                contents["kind"] = "markdown";
                contents["value"] = std::move(value);
                Json result = Json::object();
                result["contents"] = std::move(contents);
                result["range"] = range;
                return result;
            }
        }
    }

    if (context->kind == "port") {
        const std::string name = ContextWord(text, context);
        for (const Port& port : ports) {
            if (port.name != name) continue;
            std::string value = "**" + name + "** `" + port.direction + "` width=" +
                                std::to_string(port.width.value_or(1)) + "\n\ndef " + port.file +
                                ":" + std::to_string(port.line + 1);
            Json contents = Json::object();
            contents["kind"] = "markdown";
            contents["value"] = std::move(value);
            Json result = Json::object();
            result["contents"] = std::move(contents);
            result["range"] = range;
            return result;
        }
    }
    return Json(nullptr);
}

Json LspCompletion(std::string_view text, int line, int character,
                   const std::vector<Port>& ports) {
    const std::optional<CursorContext> context = Context(text, line, character);
    const std::string kind = context.has_value() ? context->kind : std::string();
    Json items = Json::array();

    if (kind == "command") {
        // Python：前缀 = 行首标识符（转小写），再对**原始命令名**做 startswith
        std::string prefix;
        const std::string_view row = text::LineAt(text, line);
        // Python: re.match(r"^\s*([A-Za-z_]\w*)", row)
        const std::string_view leading = text::LeadingIdentifier(text::LStrip(row));
        if (!leading.empty()) prefix = LowerAscii(leading);
        for (const CommandUsage& row_entry : CommandUsages()) {
            const std::string name(row_entry.name);
            if (!prefix.empty() && !name.starts_with(prefix)) continue;
            items.push_back(
                CompletionItemJson(name, kKindFunction, TruncatedUsage(row_entry.usage)));
        }
    } else if (kind == "port") {
        for (const Port& port : ports) {
            if (port.name.empty()) continue;
            // 同上：端口方向是用户可控文本，绝不能被定长缓冲从中间截断成非法 UTF-8
            const std::string detail =
                std::format("{} · {} bit", port.direction.empty() ? "?" : port.direction.c_str(),
                            port.width.value_or(1));
            items.push_back(CompletionItemJson(port.name, kKindField, detail));
        }
    } else if (kind == "clock_name") {
        for (const std::string& name : DefinedClockNames(text)) {
            items.push_back(CompletionItemJson(name, kKindConstant, "clock"));
        }
    } else if (kind == "option") {
        // Python 用 td_commands.json 的 options（原顺序）；这里同源
        if (const CommandUsage* info = FindCommand(LowerAscii(context->command))) {
            std::string_view rest = info->options;
            while (!rest.empty()) {
                const std::size_t space = rest.find(' ');
                const std::string_view token =
                    space == std::string_view::npos ? rest : rest.substr(0, space);
                if (!token.empty()) {
                    Json item = Json::object();
                    item["label"] = std::string(token);
                    item["kind"] = kKindProperty;
                    items.push_back(std::move(item));
                }
                if (space == std::string_view::npos) break;
                rest = rest.substr(space + 1);
            }
        }
    }

    if (items.size() > kMaxCompletionItems) {
        Json capped = Json::array();
        for (std::size_t i = 0; i < kMaxCompletionItems; ++i) capped.push_back(items[i]);
        items = std::move(capped);
    }

    Json result = Json::object();
    result["isIncomplete"] = false;
    result["items"] = std::move(items);
    return result;
}

// =============================================================================
//  JSON 序列化
// =============================================================================

namespace {

Json SpanToJson(const Span& span) {
    Json json = Json::object();
    json["line"] = span.line;
    json["start"] = span.start;
    json["end"] = span.end;
    return json;
}

const char* KindName(TokenKind kind) {
    switch (kind) {
        case TokenKind::kWord:
            return "word";
        case TokenKind::kBrace:
            return "brace";
        case TokenKind::kBracket:
            return "bracket";
        case TokenKind::kQuote:
            return "quote";
        case TokenKind::kSeparator:
            return "sep";
    }
    return "word";
}

}  // namespace

Json ToJson(const Token& token) {
    Json json = Json::object();
    json["kind"] = KindName(token.kind);
    json["text"] = token.text;
    json["inner_text"] = token.inner_text;
    json["start"] = token.start;
    json["end"] = token.end;
    json["inner_start"] = token.inner_start;
    json["inner_end"] = token.inner_end;
    json["closed"] = token.closed;
    return json;
}

Json ToJson(const Command& command) {
    Json json = Json::object();
    json["line"] = command.line;
    json["command"] = command.command;
    json["args"] = command.args;
    json["raw"] = command.raw;
    json["command_span"] = SpanToJson(command.command_span);

    Json tokens = Json::array();
    for (const Token& token : command.arg_tokens) tokens.push_back(ToJson(token));
    json["arg_tokens"] = std::move(tokens);

    Json syntax = Json::array();
    for (const SyntaxError& error : command.syntax) {
        Json item = Json::object();
        item["line"] = command.line;
        item["character"] = error.character;
        item["message"] = error.message;
        syntax.push_back(std::move(item));
    }
    json["syntax"] = std::move(syntax);
    return json;
}

Json ToJson(const std::vector<Command>& commands) {
    Json json = Json::array();
    for (const Command& command : commands) json.push_back(ToJson(command));
    return json;
}

} // namespace fpgalsp::constraints::sdc

