// =============================================================================
//  src/server/features_rename_util.h —— rename / code action 共用的纯文本小工具
// =============================================================================
//  OWNER: task B3 (parity-lsp)。头文件 only（inline），因此**不需要**改 CMake。
//
//  为什么单独抽出来：
//    1. B3 的两个 Feature（features_rename.cpp / features_code_actions.cpp）都要
//       做「按行取窗口 + UTF-16 列换算 + 注释/字符串排除」，各写一份必然漂移；
//    2. 这几个函数是**纯函数**（无索引、无传输、无磁盘），可以直接单测，
//       不必先搭一个 LspServer 夹具。
//
//  对应参考实现（逐条口径，改动前先对照）：
//    Y:\fpga-lsp\fpgalsp\refs.py::token_at / token_span      -> TextTokenAt
//    Y:\fpga-lsp\fpgalsp\nav.py::valid_identifier            -> IsValidVerilogIdentifier
//    Y:\fpga-lsp\fpgalsp\nav.py::block_scopes                -> BlockScopes / EnclosingScope
//    Y:\fpga-lsp\fpgalsp\verible.py::expand_comment_tabs      -> ExpandCommentTabs
//    Y:\fpga-lsp\fpgalsp\server.py::_read_text (universal newlines)
//                                                             -> NormalizeNewlines
//
//  ⚠ 列号口径：Python 的 str 索引是**码点**列，LSP/C++ 是 **UTF-16 码元**列。
//    对 BMP 字符两者相同；本文件按 UTF-16 口径（更正确），非 BMP 前缀时与
//    Python 有意不同（Python 参考实现这里是缺陷）。
// =============================================================================

#pragma once

#include <algorithm>
#include <cstddef>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "platform/utf16.h"

namespace fpgalsp::server::edit {

// -----------------------------------------------------------------------------
// 行尾
// -----------------------------------------------------------------------------

// 把 CRLF / 孤立 CR 统一成 LF。
// Python 侧所有文本读取都是 `open(..., encoding="utf-8")`（universal newlines），
// 所以 Python 眼里**永远是 LF 文本**；C++ 的 LoadText 给的是原始字节（可能是 CRLF）。
// 需要与 Python 逐字节比较的输出（codeAction 的 newText、整篇 range 的行列）都必须
// 先过这里。参见 doc/audit/2026-11-18/parity-lsp/PROBE.md §3.6（LSP-18 的归因）。
inline std::string NormalizeNewlines(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
            out.push_back('\n');
            continue;
        }
        out.push_back(text[i]);
    }
    return out;
}

// Verilog 标识符字符（ASCII）：alnum / '_' / '$'
inline bool IsIdentifierChar(char ch) {
    const unsigned char uc = static_cast<unsigned char>(ch);
    return std::isalnum(uc) != 0 || ch == '_' || ch == '$';
}

// 码点数（用于复刻 Python len(str) 的 tab 列计算；UTF-8 续字节不计）。
inline std::size_t CodepointCount(std::string_view text) {
    std::size_t count = 0;
    for (const char ch : text) {
        if ((static_cast<unsigned char>(ch) & 0xC0) != 0x80) ++count;
    }
    return count;
}

// 对应 Python refs._key：escaped identifier 去掉开头的反斜杠与尾部空白。
inline std::string IdentKey(std::string_view name) {
    if (!name.empty() && name.front() == '\\') {
        std::string out(name.substr(1));
        while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back())) != 0) {
            out.pop_back();
        }
        return out;
    }
    return std::string(name);
}

// -----------------------------------------------------------------------------
// 光标处的标识符（对应 refs.token_at / token_span）
// -----------------------------------------------------------------------------

struct TextToken {
    std::string text;
    int start = 0;  // UTF-16 列
    int end = 0;    // UTF-16 列（不含）
    bool found = false;
};

// 取 (line, character) 处的标识符。
//   * character 是 UTF-16 列，会先 clamp 到该行长度（Python: character > len(row) -> len(row)）
//   * 行号越界 / 光标不在标识符上 -> found=false
// 与 Python 的差别只有一处：列单位是 UTF-16 而不是码点（非 BMP 前缀时更正确）。
inline TextToken TextTokenAt(std::string_view text, int line, int character) {
    TextToken out;
    if (line < 0) return out;
    const std::string_view row = platform::line_at(text, static_cast<std::size_t>(line));
    if (row.empty()) return out;

    std::size_t column = static_cast<std::size_t>(character < 0 ? 0 : character);
    if (column > platform::utf16_len(row)) column = platform::utf16_len(row);
    const std::size_t cursor = platform::utf16_column_to_byte_index(row, column);

    std::size_t begin = cursor;
    while (begin > 0 && IsIdentifierChar(row[begin - 1])) --begin;
    std::size_t end = cursor;
    while (end < row.size() && IsIdentifierChar(row[end])) ++end;
    if (begin == end) return out;

    out.text = std::string(row.substr(begin, end - begin));
    out.start = static_cast<int>(platform::byte_index_to_utf16_column(row, begin));
    out.end = static_cast<int>(platform::byte_index_to_utf16_column(row, end));
    out.found = true;
    return out;
}

// 一行里第一段非空白的缩进（Python: row[: len(row) - len(row.lstrip())]，空白行回落 4 空格）
inline std::string LeadingIndent(std::string_view row) {
    std::size_t first = 0;
    while (first < row.size() && std::isspace(static_cast<unsigned char>(row[first])) != 0) {
        ++first;
    }
    if (first >= row.size()) return "    ";  // 空行 / 纯空白行
    return std::string(row.substr(0, first));
}

// -----------------------------------------------------------------------------
// rename 合法性（对应 nav.valid_identifier）
// -----------------------------------------------------------------------------
// ⚠ Python 的关键字黑名单只有这 4 个（不是完整关键字表）—— 逐字复刻，别"顺手"扩大。
inline bool IsValidVerilogIdentifier(std::string_view name) {
    if (name.empty()) return false;
    const unsigned char first = static_cast<unsigned char>(name.front());
    if (std::isalpha(first) == 0 && name.front() != '_') return false;
    for (const char ch : name) {
        const unsigned char uc = static_cast<unsigned char>(ch);
        if (std::isalnum(uc) != 0 || ch == '_' || ch == '$') continue;
        return false;
    }
    return name != "module" && name != "endmodule" && name != "input" && name != "output";
}

// -----------------------------------------------------------------------------
// generate 作用域（对应 nav.block_scopes / enclosing_scope）
// -----------------------------------------------------------------------------
// 为什么需要：`generate for (genvar i = ...)` 里的 `i` 不在 HDL 符号索引里，
// 但同一个模块里另一个 generate 块可能也有自己的 `i`。Python 的做法是
// 「声明落在 generate / for-begin 块内时，只在该块（且只在声明文件）里重命名」。
struct BlockScope {
    int start = 0;       // 起始行（0-based）
    int end = 0;         // 结束行（0-based）
    bool for_begin = false;  // 来自 generate-for 的 begin（而不是裸 begin/generate）
};

namespace detail {

inline bool IsScopeWord(std::string_view word, std::string_view list) {
    for (std::size_t i = 0; i + word.size() <= list.size(); ++i) {
        if (i != 0 && list[i - 1] != ' ') continue;
        if (list.compare(i, word.size(), word) == 0 &&
            (i + word.size() == list.size() || list[i + word.size()] == ' ')) {
            return true;
        }
    }
    return false;
}

}  // namespace detail

// 逐行扫描 generate / begin / end / case / function / task / fork …，
// 返回 generate 与 generate-for 作用域（Python 只把这两类算作 rename 边界）。
inline std::vector<BlockScope> BlockScopes(std::string_view text) {
    std::vector<BlockScope> scopes;
    // 栈元素：(kind, 起始行)。kind 与 Python 的字符串一致。
    std::vector<std::pair<std::string, int>> stack;
    int generate_depth = 0;
    int pending_for = -1;

    const std::vector<std::string> rows = [&text] {
        std::vector<std::string> out;
        std::size_t position = 0;
        for (;;) {
            const std::size_t newline = text.find('\n', position);
            if (newline == std::string_view::npos) {
                out.emplace_back(text.substr(position));
                return out;
            }
            out.emplace_back(text.substr(position, newline - position));
            position = newline + 1;
        }
    }();

    for (std::size_t line = 0; line < rows.size(); ++line) {
        const int line_number = static_cast<int>(line);
        std::string_view code(rows[line]);
        if (const std::size_t comment = code.find("//"); comment != std::string_view::npos) {
            code = code.substr(0, comment);
        }
        // 按标识符词法扫描（等价 Python 的 \b(...)\b 交替）
        std::size_t i = 0;
        while (i < code.size()) {
            if (!IsIdentifierChar(code[i])) {
                ++i;
                continue;
            }
            std::size_t end = i;
            while (end < code.size() && IsIdentifierChar(code[end])) ++end;
            const std::string_view word = code.substr(i, end - i);
            i = end;

            if (word == "for") {
                pending_for = line_number;
                continue;
            }
            if (word == "begin") {
                if (pending_for >= 0) {
                    const bool is_for = generate_depth > 0 ||
                                        rows[static_cast<std::size_t>(pending_for)].find(
                                            "genvar") != std::string::npos;
                    stack.emplace_back(is_for ? "for-begin" : "begin",
                                       is_for ? pending_for : line_number);
                    pending_for = -1;
                } else {
                    stack.emplace_back("begin", line_number);
                }
                continue;
            }
            if (word == "generate") {
                ++generate_depth;
                stack.emplace_back("generate", line_number);
                continue;
            }
            if (word == "case" || word == "casez" || word == "casex" || word == "function" ||
                word == "task" || word == "fork") {
                stack.emplace_back(std::string(word), line_number);
                continue;
            }
            if (word == "end" || word == "endcase" || word == "endfunction" ||
                word == "endtask" || word == "join" || word == "endgenerate") {
                std::string want = "begin";
                if (word == "endcase") want = "case";
                else if (word == "endfunction") want = "function";
                else if (word == "endtask") want = "task";
                else if (word == "join") want = "fork";
                else if (word == "endgenerate") want = "generate";

                for (std::size_t index = stack.size(); index-- > 0;) {
                    const std::string& kind = stack[index].first;
                    if (kind == want || (want == "begin" && kind == "for-begin")) {
                        const std::string popped_kind = stack[index].first;
                        const int start = stack[index].second;
                        stack.erase(stack.begin() + static_cast<std::ptrdiff_t>(index));
                        if ((popped_kind == "generate" || popped_kind == "for-begin") &&
                            line_number > start) {
                            BlockScope scope;
                            scope.start = start;
                            scope.end = line_number;
                            scope.for_begin = popped_kind == "for-begin";
                            scopes.push_back(scope);
                        }
                        break;
                    }
                }
                if (word == "endgenerate" && generate_depth > 0) --generate_depth;
            }
        }
    }
    return scopes;
}

// 包含该行的最内层（start 最大）作用域；没有则返回 nullptr。
inline const BlockScope* EnclosingScope(const std::vector<BlockScope>& scopes, int line) {
    const BlockScope* best = nullptr;
    for (const BlockScope& scope : scopes) {
        if (scope.start <= line && line <= scope.end) {
            if (best == nullptr || scope.start > best->start) best = &scope;
        }
    }
    return best;
}

// -----------------------------------------------------------------------------
// 注释里的 tab 展开（对应 verible.expand_comment_tabs）
// -----------------------------------------------------------------------------
// 为什么存在这条后处理：Verible 的 no-tabs 规则不可 autofix，而
// verible-verilog-format **不改注释**，所以 lint autofix 之后注释里的 tab 会留下来。
// Python 侧在 autofix 之后无条件跑这一步，C++ 必须同样跑，否则 newText 有差异。
inline std::string ExpandCommentTabs(std::string_view text, int tab_size = 4) {
    if (tab_size <= 0) tab_size = 4;

    const std::size_t newlines = static_cast<std::size_t>(
        std::count(text.begin(), text.end(), '\n'));
    std::string out;
    out.reserve(text.size() + newlines * 2);

    std::size_t position = 0;
    // ⚠ 与 Python 一致：in_block 跨行保持（块注释可能跨行），其余三个状态每行重置。
    bool in_block = false;
    while (position <= text.size()) {
        const std::size_t newline = text.find('\n', position);
        const std::size_t line_end = newline == std::string_view::npos ? text.size() : newline;
        const std::string_view line = text.substr(position, line_end - position);

        // 该行已输出的码点数（Python: len("".join(result))）
        std::size_t column = 0;
        bool in_string = false;
        bool escaped = false;
        bool in_line_comment = false;
        std::size_t i = 0;
        while (i < line.size()) {
            const char ch = line[i];
            const char next = (i + 1 < line.size()) ? line[i + 1] : '\0';

            if (in_line_comment || in_block) {
                if (!in_line_comment && ch == '*' && next == '/') {
                    out.append("*/");
                    column += 2;
                    i += 2;
                    in_block = false;
                    continue;
                }
                if (ch == '\t') {
                    const std::size_t spaces =
                        static_cast<std::size_t>(tab_size) - column % static_cast<std::size_t>(tab_size);
                    out.append(spaces, ' ');
                    column += spaces;
                    ++i;
                    continue;
                }
                const std::size_t width =
                    (static_cast<unsigned char>(ch) & 0xC0) == 0x80 ? 0 : 1;
                out.push_back(ch);
                column += width;
                ++i;
                continue;
            }
            if (in_string) {
                out.push_back(ch);
                column += (static_cast<unsigned char>(ch) & 0xC0) == 0x80 ? 0 : 1;
                if (escaped) {
                    escaped = false;
                } else if (ch == '\\') {
                    escaped = true;
                } else if (ch == '"') {
                    in_string = false;
                }
                ++i;
                continue;
            }
            if (ch == '"') {
                in_string = true;
                out.push_back(ch);
                ++column;
                ++i;
                continue;
            }
            if (ch == '/' && next == '*') {
                out.append("/*");
                column += 2;
                in_block = true;
                i += 2;
                continue;
            }
            if (ch == '/' && next == '/') {
                out.append("//");
                column += 2;
                in_line_comment = true;
                i += 2;
                continue;
            }
            out.push_back(ch);
            column += (static_cast<unsigned char>(ch) & 0xC0) == 0x80 ? 0 : 1;
            ++i;
        }

        if (newline == std::string_view::npos) break;
        out.push_back('\n');
        position = newline + 1;
    }
    return out;
}

}  // namespace fpgalsp::server::edit