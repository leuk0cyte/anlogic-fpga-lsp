// =============================================================================
//  src/server/features_optional.cpp —— LSP 3.17/3.18 可选合规项（A4–A12）
// =============================================================================
//  纯逻辑在 `features_optional` 命名空间（输入 = 索引/文本/位置，输出 = 协议 JSON，
//  可被 tests/unit/features_optional_test.cpp 直接调用）；
//  LspServer::Feature*/Handle* 只做"取工程/索引/缓冲区 + 解包 params"。
//
//  ⚠ 红线：
//    * 每一项都必须有真实语义，禁止返回固定空值充数（验收见单测）；
//    * 位置一律 0-based 行 + UTF-16 列；
//    * never-throw：畸形参数 -> 合法空值（null / [] / {signatures:[]}），绝不 -32601。
// =============================================================================

#include "server/features_optional.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "hdl/hdl_index.h"
#include "jsonrpc/json_util.h"
#include "platform/log.h"
#include "platform/path.h"
#include "platform/utf16.h"
#include "project/al_project.h"
#include "semantics/semantics.h"
#include "server/lsp_server.h"

#include "platform/text.h"
namespace fpgalsp::server {

namespace {

// -----------------------------------------------------------------------------
// 小工具（本文件自用；不导出）
// -----------------------------------------------------------------------------

Json MakePosition(int line, int character) {
    Json position = Json::object();
    position["line"] = line;
    position["character"] = character;
    return position;
}

Json MakeRange(int start_line, int start_character, int end_line, int end_character) {
    Json range = Json::object();
    range["start"] = MakePosition(start_line, start_character);
    range["end"] = MakePosition(end_line, end_character);
    return range;
}

using platform::LowerAscii;

using platform::EndsWith;

// `.v/.sv/.vh/.svh`：只有这些扩展名才走 HDL 索引路径。
bool IsHdlFilePath(std::string_view path) {
    const std::string lower = LowerAscii(path);
    return EndsWith(lower, ".v") || EndsWith(lower, ".sv") || EndsWith(lower, ".vh") ||
           EndsWith(lower, ".svh");
}

// A5：只有 `.al` 才解析 `<File Path>`。
bool IsAlFilePath(std::string_view path) {
    return EndsWith(LowerAscii(path), ".al");
}

bool IsIdentByte(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '$';
}

std::size_t SkipSpaceBackwards(std::string_view text, std::size_t pos) {
    while (pos > 0 && std::isspace(static_cast<unsigned char>(text[pos - 1])) != 0) --pos;
    return pos;
}

// 从 end 向前读一个标识符；返回长度，*begin 为起点。
std::size_t ReadIdentBackwards(std::string_view text, std::size_t end, std::size_t* begin) {
    std::size_t pos = end;
    while (pos > 0 && IsIdentByte(text[pos - 1])) --pos;
    *begin = pos;
    return end - pos;
}

// 给 close_pos 处的 ')' 找配对的 '('（文本级，容忍畸形括号）。
bool MatchParenBackwards(std::string_view text, std::size_t close_pos, std::size_t* open_pos) {
    if (close_pos >= text.size() || text[close_pos] != ')') return false;
    int depth = 0;
    for (std::size_t i = close_pos + 1; i-- > 0;) {
        const char c = text[i];
        if (c == ')') {
            ++depth;
        } else if (c == '(') {
            --depth;
            if (depth == 0) {
                *open_pos = i;
                return true;
            }
        }
    }
    return false;
}

Json EmptySignatureHelp() {
    Json result = Json::object();
    result["signatures"] = Json::array();
    result["activeSignature"] = 0;
    result["activeParameter"] = 0;
    return result;
}

std::string PortDocumentation(const hdl::Port& port) {
    std::string doc = port.direction.empty() ? "port" : port.direction;
    if (port.msb.has_value()) {
        doc += " [";
        doc += std::to_string(*port.msb);
        if (port.lsb.has_value()) {
            doc += ':';
            doc += std::to_string(*port.lsb);
        }
        doc += ']';
    } else if (port.width > 1) {
        doc += " [" + std::to_string(port.width) + " bits]";
    }
    return doc;
}

// -----------------------------------------------------------------------------
// A8：块（begin..end / { } / module..endmodule ...）收集
// -----------------------------------------------------------------------------
struct BlockSpan {
    int start_line = 0;
    int start_character = 0;
    int end_line = 0;
    int end_character = 0;
};

bool SpanContains(const BlockSpan& span, int line) {
    return line >= span.start_line && line <= span.end_line;
}

std::vector<BlockSpan> CollectBlocks(const hdl::HdlIndex& index, std::string_view path,
                                     int last_line, int last_line_length) {
    std::vector<BlockSpan> blocks;
    const hdl::ParsedFile* file = index.File(path);
    if (file == nullptr) return blocks;

    struct Frame {
        char kind = 0;  // m/b/f/t/g/c/{/
        int line = 0;
        int character = 0;
    };
    std::vector<Frame> stack;
    const auto close_blocks = [&](char kind, int line, int end_character) {
        for (std::size_t k = stack.size(); k-- > 0;) {
            if (stack[k].kind != kind) continue;
            BlockSpan span;
            span.start_line = stack[k].line;
            span.start_character = stack[k].character;
            span.end_line = line;
            span.end_character = end_character;
            blocks.push_back(span);
            stack.resize(k);  // 容错：顺带弹出中间未闭合的帧
            return;
        }
    };

    for (const hdl::Token& token : file->tokens) {
        const std::string_view text = token.text;
        if (text == "module" || text == "macromodule") {
            stack.push_back({.kind='m', .line=token.line, .character=token.character});
        } else if (text == "begin") {
            stack.push_back({.kind='b', .line=token.line, .character=token.character});
        } else if (text == "function") {
            stack.push_back({.kind='f', .line=token.line, .character=token.character});
        } else if (text == "task") {
            stack.push_back({.kind='t', .line=token.line, .character=token.character});
        } else if (text == "generate") {
            stack.push_back({.kind='g', .line=token.line, .character=token.character});
        } else if (text == "case" || text == "casex" || text == "casez") {
            stack.push_back({.kind='c', .line=token.line, .character=token.character});
        } else if (text == "{") {
            stack.push_back({.kind='{', .line=token.line, .character=token.character});
        } else if (text == "endmodule") {
            close_blocks('m', token.line, token.end_character);
        } else if (text == "end") {
            close_blocks('b', token.line, token.end_character);
        } else if (text == "endfunction") {
            close_blocks('f', token.line, token.end_character);
        } else if (text == "endtask") {
            close_blocks('t', token.line, token.end_character);
        } else if (text == "endgenerate") {
            close_blocks('g', token.line, token.end_character);
        } else if (text == "endcase") {
            close_blocks('c', token.line, token.end_character);
        } else if (text == "}") {
            close_blocks('{', token.line, token.end_character);
        }
    }
    // 未闭合的块（正在编辑中的文件很常见）：一直延伸到文件末尾。
    for (const Frame& frame : stack) {
        BlockSpan span;
        span.start_line = frame.line;
        span.start_character = frame.character;
        span.end_line = last_line;
        span.end_character = last_line_length;
        blocks.push_back(span);
    }
    return blocks;
}

// 词范围：从光标所在字节向两侧扩展标识符字符（光标在词中/词尾都能覆盖整个词）。
std::pair<int, int> WordSpan(std::string_view line_text, int character) {
    const std::size_t column = character > 0 ? static_cast<std::size_t>(character) : 0;
    std::size_t byte = platform::utf16_column_to_byte_index(line_text, column);
    if (byte > line_text.size()) byte = line_text.size();
    std::size_t begin = byte;
    while (begin > 0 && IsIdentByte(line_text[begin - 1])) --begin;
    std::size_t end = byte;
    while (end < line_text.size() && IsIdentByte(line_text[end])) ++end;
    if (begin == end) {
        // 不在标识符上：给一个 1 列宽的点范围（clamp 到行尾）
        const std::size_t start_byte = std::min(byte, line_text.size());
        const std::size_t end_byte = std::min(start_byte + 1, line_text.size());
        return {static_cast<int>(platform::byte_index_to_utf16_column(line_text, start_byte)),
                static_cast<int>(platform::byte_index_to_utf16_column(line_text, end_byte))};
    }
    return {static_cast<int>(platform::byte_index_to_utf16_column(line_text, begin)),
            static_cast<int>(platform::byte_index_to_utf16_column(line_text, end))};
}

struct SimpleRange {
    int start_line = 0;
    int start_character = 0;
    int end_line = 0;
    int end_character = 0;
};

bool SameRange(const SimpleRange& a, const SimpleRange& b) {
    return a.start_line == b.start_line && a.start_character == b.start_character &&
           a.end_line == b.end_line && a.end_character == b.end_character;
}

Json RangeJson(const SimpleRange& range) {
    return MakeRange(range.start_line, range.start_character, range.end_line,
                     range.end_character);
}

// 链：由内向外 [word, line, block..., doc] -> SelectionRange 对象（parent 逐层向外）。
Json SelectionChainJson(const std::vector<SimpleRange>& chain) {
    if (chain.empty()) return Json(nullptr);
    // 最外层：只有 range（parent 缺省 = null），客户端据此结束扩展。
    Json node = Json::object();
    node["range"] = RangeJson(chain.back());
    for (std::size_t i = chain.size() - 1; i-- > 0;) {
        Json wrapped = Json::object();
        wrapped["range"] = RangeJson(chain[i]);
        wrapped["parent"] = std::move(node);
        node = std::move(wrapped);
    }
    return node;
}

}  // namespace

// =============================================================================
namespace features_optional {

// -----------------------------------------------------------------------------
// A4 textDocument/signatureHelp
// -----------------------------------------------------------------------------
Json SignatureHelp(const hdl::HdlIndex& index, std::string_view path, std::string_view text,
                   int line, int character) {
    Json empty = EmptySignatureHelp();
    if (!IsHdlFilePath(path) || text.empty()) return empty;

    const int safe_line = std::max(line, 0);
    const int safe_character = std::max(character, 0);
    const std::size_t offset = platform::position_to_byte_index(
        text, static_cast<std::size_t>(safe_line), static_cast<std::size_t>(safe_character));

    // 1) 从光标向前找最近的、仍处于"未闭合"状态的 '('。
    //    遇到同层的 ';' 说明光标不在调用/例化括号里（普通语句）。
    //    若最近的括号是**命名端口连接** `.port(x)` 的内层括号（前面是 '.'），
    //    它解析不出模块，需要继续向外一层找例化语句的括号 —— 所以这里循环。
    std::size_t open = 0;
    std::string module_name;
    std::size_t search_end = offset;
    for (int level = 0; level < 8 && module_name.empty(); ++level) {
        bool found = false;
        int depth = 0;
        std::size_t candidate = 0;
        for (std::size_t i = search_end; i-- > 0;) {
            const char c = text[i];
            if (c == ')' || c == ']' || c == '}') {
                ++depth;
                continue;
            }
            if (c == '(') {
                if (depth == 0) {
                    candidate = i;
                    found = true;
                    break;
                }
                --depth;
                continue;
            }
            if (c == '[' || c == '{') {
                if (depth > 0) --depth;
                continue;
            }
            if (depth == 0 && c == ';') break;
        }
        if (!found) return empty;
        open = candidate;

        // 2) 解析 `(` 前面的标识符；跳过可选的 `#(...)` 参数覆盖列表。
        std::size_t name_end = SkipSpaceBackwards(text, open);
        bool parameter_list = false;
        if (name_end > 0 && text[name_end - 1] == ')') {
            std::size_t param_open = 0;
            if (!MatchParenBackwards(text, name_end - 1, &param_open)) return empty;
            name_end = SkipSpaceBackwards(text, param_open);
            if (name_end > 0 && text[name_end - 1] == '#') {
                name_end = SkipSpaceBackwards(text, name_end - 1);
            }
            parameter_list = true;
        }
        std::size_t name_begin = 0;
        bool is_ident = ReadIdentBackwards(text, name_end, &name_begin) > 0;
        if (!is_ident) {
            // `(` 前面不是标识符（运算符/逗号等）：向外一层继续找。
            search_end = open;
            if (open == 0) break;
            continue;
        }
        const std::string ident(text.substr(name_begin, name_end - name_begin));

        // 3) 名字 -> 模块：优先当"实例名"查（`child u0 (`），否则当模块名查，
        //    再否则看前一个标识符（`child u0 (` 里 u0 没被索引进实例表时的兜底）。
        //    `#(...)` 前面的标识符一定是实例名/模块名，不做"前一个标识符"兜底。
        const hdl::Instance* instance = nullptr;
        for (const hdl::Instance* found_instance : index.InstancesOfFile(path)) {
            if (found_instance == nullptr || found_instance->name != ident) continue;
            if (instance == nullptr || (found_instance->start_line <= safe_line &&
                                        safe_line <= found_instance->end_line)) {
                instance = found_instance;
            }
        }
        if (instance != nullptr) {
            module_name = instance->module;
        } else if (index.ModuleByName(ident) != nullptr) {
            module_name = ident;
        } else if (!parameter_list) {
            const std::size_t prev_end = SkipSpaceBackwards(text, name_begin);
            std::size_t prev_begin = 0;
            if (ReadIdentBackwards(text, prev_end, &prev_begin) > 0) {
                const std::string previous(text.substr(prev_begin, prev_end - prev_begin));
                if (index.ModuleByName(previous) != nullptr) module_name = previous;
            }
        }
        // 没解析出模块：search_end = open，继续向外层括号找。
        search_end = open;
        if (open == 0) break;
    }
    if (module_name.empty()) return empty;

    const hdl::Module* module = index.ModuleByName(module_name);
    if (module == nullptr) return empty;

    // 4) activeParameter = 括号内、光标之前的**顶层**逗号数。
    int commas = 0;
    int nested = 0;
    for (std::size_t i = open + 1; i < offset && i < text.size(); ++i) {
        const char c = text[i];
        if (c == '(' || c == '[' || c == '{') {
            ++nested;
        } else if (c == ')' || c == ']' || c == '}') {
            if (nested > 0) --nested;
        } else if (c == ',' && nested == 0) {
            ++commas;
        }
    }

    Json parameters = Json::array();
    std::string label = "module " + module->name + "(";
    for (std::size_t i = 0; i < module->ports.size(); ++i) {
        const hdl::Port& port = module->ports[i];
        if (i > 0) label += ", ";
        label += port.name;
        Json parameter = Json::object();
        parameter["label"] = port.name;
        parameter["documentation"] = PortDocumentation(port);
        parameters.push_back(std::move(parameter));
    }
    label += ')';

    Json signature = Json::object();
    signature["label"] = std::move(label);
    signature["parameters"] = std::move(parameters);
    // 定义位置放进 documentation（SignatureInformation 没有 location 字段）。
    signature["documentation"] = "Defined at " + platform::path_to_uri(module->file) + ":" +
                                 std::to_string(module->line + 1);

    int active_parameter = commas;
    if (!module->ports.empty()) {
        active_parameter = std::min(active_parameter,
                                    static_cast<int>(module->ports.size()) - 1);
    } else {
        active_parameter = 0;
    }

    Json result = Json::object();
    result["signatures"] = Json::array({std::move(signature)});
    result["activeSignature"] = 0;
    result["activeParameter"] = active_parameter;
    return result;
}

// -----------------------------------------------------------------------------
// A5 textDocument/documentLink
// -----------------------------------------------------------------------------
// `.al` 里 `Path=` 只应链接 `<File>` 元素。`<Project ... Path="Y:/proj">` 是**工程根**
// （目录），把它当文件链接会得到 24 条里夹一条 "file not found: C:/work/CorpusAlpha"（实测）。
// 判定：属性前面最近的 `<File` 必须在同一个 start tag 内（中间不出现 '>'），且元素名
// 后紧跟空白/'>'（排除 `<FileInfo`）。
bool InFileElementTag(std::string_view row, std::size_t attr_pos) {
    std::size_t found = std::string_view::npos;
    std::size_t from = 0;
    while (true) {
        const std::size_t p = row.find("<File", from);
        if (p == std::string_view::npos) break;
        const std::size_t name_end = p + 5;
        const bool boundary =
            name_end >= row.size() ||
            std::isspace(static_cast<unsigned char>(row[name_end])) != 0 ||
            row[name_end] == '>';
        if (boundary && p < attr_pos) found = p;
        from = p + 5;
    }
    if (found == std::string_view::npos) return false;
    return !row.substr(found, attr_pos - found).contains('>');
}

Json DocumentLinks(std::string_view path, std::string_view text) {
    Json links = Json::array();
    if (!IsAlFilePath(path) || text.empty()) return links;

    const std::string al_path(path);
    const std::string project_dir = platform::path_dirname(al_path);
    const std::size_t line_total = platform::line_count(text);

    for (std::size_t line = 0; line < line_total; ++line) {
        const std::string_view row = platform::line_at(text, line);
        std::size_t cursor = row.find("Path");
        while (cursor != std::string_view::npos) {
            // 属性名边界：`<File Path=` 命中；`XPath=` 不命中。
            if (cursor > 0 && (IsIdentByte(row[cursor - 1]) || row[cursor - 1] == ':')) {
                cursor = row.find("Path", cursor + 4);
                continue;
            }
            if (!InFileElementTag(row, cursor)) {
                cursor = row.find("Path", cursor + 4);
                continue;
            }
            std::size_t p = cursor + 4;
            while (p < row.size() && std::isspace(static_cast<unsigned char>(row[p])) != 0) ++p;
            if (p >= row.size() || row[p] != '=') {
                cursor = row.find("Path", cursor + 4);
                continue;
            }
            ++p;
            while (p < row.size() && std::isspace(static_cast<unsigned char>(row[p])) != 0) ++p;
            if (p >= row.size() || row[p] != '"') {
                cursor = row.find("Path", cursor + 4);
                continue;
            }
            const std::size_t value_begin = p + 1;
            const std::size_t value_end = row.find('"', value_begin);
            if (value_end == std::string_view::npos) break;

            const std::string rel(row.substr(value_begin, value_end - value_begin));
            const project::ResolveResult resolved = project::ResolveRelative(project_dir, rel);
            const bool exists =
                !resolved.path.empty() && project::DefaultFileExists(resolved.path);

            Json link = Json::object();
            link["range"] = MakeRange(
                static_cast<int>(line),
                static_cast<int>(platform::byte_index_to_utf16_column(row, value_begin)),
                static_cast<int>(line),
                static_cast<int>(platform::byte_index_to_utf16_column(row, value_end)));
            if (exists) {
                link["target"] = platform::path_to_uri(resolved.path);
                link["tooltip"] = "Open " + rel;
            } else {
                // 无 target -> 客户端把链接置灰；tooltip 说明原因（不报错）。
                link["tooltip"] = "file not found: " + rel;
            }
            links.push_back(std::move(link));
            cursor = row.find("Path", value_end + 1);
        }
    }
    return links;
}

// -----------------------------------------------------------------------------
// A6 textDocument/codeLens
// -----------------------------------------------------------------------------
Json CodeLenses(const hdl::HdlIndex& index, std::string_view path) {
    Json lenses = Json::array();
    if (!IsHdlFilePath(path)) return lenses;

    std::map<std::string, int> instantiation_count;
    for (const hdl::Instance* instance : index.AllInstances()) {
        if (instance == nullptr) continue;
        ++instantiation_count[instance->module];
    }

    for (const hdl::ModuleInfo* info : index.ModulesOfFile(path)) {
        if (info == nullptr) continue;
        const hdl::Module& module = info->module;
        const auto found = instantiation_count.find(module.name);
        const int count = found == instantiation_count.end() ? 0 : found->second;

        Json lens = Json::object();
        lens["range"] =
            MakeRange(module.line, module.character, module.line,
                      module.character + static_cast<int>(module.name.size()));
        Json command = Json::object();
        command["title"] = std::to_string(count) + " instantiations";
        command["command"] = "fpgalsp.searchHierarchy";
        Json arguments = Json::array();
        Json argument = Json::object();
        argument["query"] = module.name;
        arguments.push_back(std::move(argument));
        command["arguments"] = std::move(arguments);
        lens["command"] = std::move(command);
        lenses.push_back(std::move(lens));
    }
    return lenses;
}

// -----------------------------------------------------------------------------
// A7 textDocument/documentHighlight
// -----------------------------------------------------------------------------
Json DocumentHighlights(const hdl::HdlIndex& index, std::string_view path, int line,
                        int character) {
    Json highlights = Json::array();
    if (!IsHdlFilePath(path)) return highlights;

    const std::string key = platform::path_key(platform::normalize_path(path));
    std::set<std::tuple<int, int, int, int>> seen;
    for (const hdl::Location& location : index.References(path, line, character)) {
        // 只看同一文档（索引/调用方可能用不同的路径写法，统一 normalize 后再比）。
        if (platform::path_key(platform::normalize_path(location.path)) != key) continue;
        const auto identity = std::make_tuple(location.line, location.character,
                                              location.end_line, location.end_character);
        if (!seen.insert(identity).second) continue;
        Json highlight = Json::object();
        highlight["range"] =
            MakeRange(location.line, location.character, location.end_line,
                      location.end_character);
        // token 流判不出读写（A7 设计结论）：统一 Text，不说谎。
        highlight["kind"] = 1;
        highlights.push_back(std::move(highlight));
    }
    return highlights;
}

// -----------------------------------------------------------------------------
// A8 textDocument/selectionRange
// -----------------------------------------------------------------------------
Json SelectionRanges(const hdl::HdlIndex& index, std::string_view path, std::string_view text,
                     const std::vector<std::pair<int, int>>& positions) {
    Json results = Json::array();
    if (text.empty() && positions.empty()) return results;

    const int last_line = static_cast<int>(platform::line_count(text)) - 1;
    const std::string_view last_row = platform::line_at(text, static_cast<std::size_t>(
                                                                  std::max(last_line, 0)));
    const SimpleRange document_range{.start_line=0, .start_character=0, .end_line=std::max(last_line, 0),
                                     .end_character=static_cast<int>(platform::utf16_len(last_row)),};
    const std::vector<BlockSpan> blocks =
        CollectBlocks(index, path, std::max(last_line, 0),
                      static_cast<int>(platform::utf16_len(last_row)));

    for (const auto& [raw_line, raw_character] : positions) {
        const int line = std::max(raw_line, 0);
        const int character = std::max(raw_character, 0);
        const std::string_view row = platform::line_at(text, static_cast<std::size_t>(line));
        const int row_length = static_cast<int>(platform::utf16_len(row));

        std::vector<SimpleRange> chain;
        const auto [word_begin, word_end] = WordSpan(row, character);
        chain.push_back(SimpleRange{.start_line=line, .start_character=word_begin, .end_line=line, .end_character=word_end});

        const SimpleRange line_range{.start_line=line, .start_character=0, .end_line=line, .end_character=row_length};
        if (!SameRange(line_range, chain.back())) chain.push_back(line_range);

        // 包含光标行的块，按"由内向外"排序（区间小的先）。
        std::vector<BlockSpan> containing;
        for (const BlockSpan& block : blocks) {
            if (!SpanContains(block, line)) continue;
            containing.push_back(block);
        }
        std::ranges::sort(containing,
                  [](const BlockSpan& a, const BlockSpan& b) {
                      const int span_a = a.end_line - a.start_line;
                      const int span_b = b.end_line - b.start_line;
                      if (span_a != span_b) return span_a < span_b;
                      return a.start_line > b.start_line;
                  });
        for (const BlockSpan& block : containing) {
            // 用"整行"边界，保证 parent 一定包含 child（LSP 规范硬要求）。
            const SimpleRange block_range{
                .start_line=block.start_line, .start_character=0, .end_line=block.end_line,
                .end_character=static_cast<int>(platform::utf16_len(platform::line_at(
                    text, static_cast<std::size_t>(std::max(block.end_line, 0))))),};
            if (SameRange(block_range, chain.back())) continue;
            chain.push_back(block_range);
        }

        if (!SameRange(document_range, chain.back())) chain.push_back(document_range);
        results.push_back(SelectionChainJson(chain));
    }
    return results;
}

// -----------------------------------------------------------------------------
// A12 semanticTokens/range
// -----------------------------------------------------------------------------
std::vector<semantics::SemToken> FilterSemanticTokens(
    const std::vector<semantics::SemToken>& tokens, int start_line, int start_character,
    int end_line, int end_character) {
    std::vector<semantics::SemToken> filtered;
    filtered.reserve(tokens.size());
    for (const semantics::SemToken& token : tokens) {
        const bool after_start =
            token.line > start_line ||
            (token.line == start_line && token.start >= start_character);
        const bool before_end =
            token.line < end_line || (token.line == end_line && token.start < end_character);
        if (after_start && before_end) filtered.push_back(token);
    }
    return filtered;
}

Json SemanticTokensRange(const hdl::HdlIndex* index, std::string_view path,
                         std::string_view text, int start_line, int start_character,
                         int end_line, int end_character) {
    Json result = Json::object();
    result["data"] = Json::array();
    if (text.empty()) return result;

    // 复用全量计算（SemanticTokens 是 LSP 与 MCP 共用的同一条流水线），
    // 过滤后必须**重新编码**：5 元组是相对编码，直接截数组会让 delta 全部错位。
    const std::vector<semantics::SemToken> tokens =
        semantics::SemanticTokens(index, path, text);
    const std::vector<semantics::SemToken> filtered =
        FilterSemanticTokens(tokens, start_line, start_character, end_line, end_character);
    const std::vector<int> encoded = semantics::EncodeTokens(filtered);

    Json data = Json::array();
    data.get_ref<nlohmann::json::array_t&>().reserve(encoded.size());
    for (const int value : encoded) data.push_back(value);
    result["data"] = std::move(data);
    return result;
}

Json ResolveCompletionItem(const hdl::HdlIndex* index, const Json& item) {
    Json result = item.is_object() ? item : Json::object();
    if (index == nullptr) return result;
    const std::string label = jsonrpc::GetString(result, "label");
    if (label.empty()) return result;
    // 只给"模块"候选补文档（器件原语/关键字没有端口表可查）。
    const hdl::Module* module = index->ModuleByName(label);
    if (module == nullptr) return result;

    const std::string uri = platform::path_to_uri(module->file);
    result["detail"] = "module (" + std::to_string(module->ports.size()) + " ports)";

    std::string documentation = "module " + module->name + " — " +
                                std::to_string(module->ports.size()) + " ports";
    for (const hdl::Port& port : module->ports) {
        documentation += "\n- " + port.name + ": " + PortDocumentation(port);
    }
    documentation += "\n\nDefined at " + uri + ":" + std::to_string(module->line + 1);
    Json markup = Json::object();
    markup["kind"] = "markdown";
    markup["value"] = std::move(documentation);
    result["documentation"] = std::move(markup);

    // data：客户端"跳到定义"或再次 resolve 时可用的稳定标识（不影响其它字段）。
    Json data = (result.contains("data") && result["data"].is_object()) ? result["data"]
                                                                        : Json::object();
    data["module"] = module->name;
    data["uri"] = uri;
    data["line"] = module->line;
    result["data"] = std::move(data);
    return result;
}

// -----------------------------------------------------------------------------
// A9 pull diagnostics 形状
// -----------------------------------------------------------------------------
std::string DiagnosticsResultId(std::string_view uri, const Json& items) {
    const std::string payload = items.is_array() ? items.dump() : std::string("[]");
    std::uint64_t hash = 0xcbf29ce484222325ULL;  // FNV-1a 64
    const auto mix = [&hash](std::string_view bytes) {
        for (const char raw : bytes) {
            hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(raw));
            hash *= 0x100000001b3ULL;
        }
    };
    mix(uri);
    mix("\x1f");
    mix(payload);
    // 与 snprintf("%016llx") 逐字节一致
    return std::format("{:016x}", static_cast<unsigned long long>(hash));
}

Json BuildDocumentDiagnosticReport(const Json& items, std::string_view result_id) {
    Json report = Json::object();
    report["kind"] = "full";
    report["items"] = items.is_array() ? items : Json::array();
    report["resultId"] = std::string(result_id);
    return report;
}

Json BuildWorkspaceDiagnosticReport(const std::map<std::string, Json>& latest) {
    Json report = Json::object();
    Json items = Json::array();
    for (const auto& [uri, diagnostics] : latest) {
        Json entry = Json::object();
        entry["uri"] = uri;
        entry["kind"] = "full";
        entry["items"] = diagnostics.is_array() ? diagnostics : Json::array();
        entry["resultId"] = DiagnosticsResultId(uri, entry["items"]);
        items.push_back(std::move(entry));
    }
    report["items"] = std::move(items);
    return report;
}

}  // namespace features_optional

// =============================================================================
//  LspServer：A4–A12 的 Feature*（取工程/索引/缓冲区）+ Handle*（解包 params）
// =============================================================================

namespace {

// features_optional.cpp 与 hdl_handlers.cpp 各有一份；这里只做"params 解包"，
// 不重复语义（协议层薄包装是既有分层约定）。
bool ReadTextDocumentUri(const Json& params, std::string* uri) {
    const auto doc = params.find("textDocument");
    if (doc == params.end() || !doc->is_object()) return false;
    const auto value = doc->find("uri");
    if (value == doc->end() || !value->is_string()) return false;
    *uri = value->get<std::string>();
    return !uri->empty();
}

bool ReadCursorPosition(const Json& params, int* line, int* character) {
    const auto position = params.find("position");
    if (position == params.end() || !position->is_object()) return false;
    *line = jsonrpc::GetInt(*position, "line", -1);
    *character = jsonrpc::GetInt(*position, "character", -1);
    return *line >= 0 && *character >= 0;
}

// 空索引：没有工程时 SelectionRanges / semantic range 仍要给出合法结果。
const hdl::HdlIndex& EmptyIndex() {
    static const hdl::HdlIndex kEmpty;
    return kEmpty;
}

}  // namespace

Json LspServer::FeatureSignatureHelp(std::string_view path, int line, int character) {
    try {
        if (!IsHdlFilePath(path)) return EmptySignatureHelp();
        const std::optional<std::string> text = LoadText(std::string(path));
        if (!text.has_value()) return EmptySignatureHelp();
        // C1：只读工程 -> shared 访问器（缓存命中时 0 份 Project 拷贝）。
        const std::shared_ptr<const project::Project> project = ProjectForPathShared(path);
        if (project == nullptr) return EmptySignatureHelp();
        const std::shared_ptr<const hdl::HdlIndex> index = IndexForProject(*project);
        if (!index) return EmptySignatureHelp();
        return features_optional::SignatureHelp(*index, path, *text, line, character);
    } catch (...) {
        return EmptySignatureHelp();
    }
}

Json LspServer::FeatureDocumentLinks(std::string_view path) {
    try {
        if (!IsAlFilePath(path)) return Json::array();
        const std::optional<std::string> text = LoadText(std::string(path));
        if (!text.has_value()) return Json::array();
        return features_optional::DocumentLinks(path, *text);
    } catch (...) {
        return Json::array();
    }
}

Json LspServer::FeatureCodeLenses(std::string_view path) {
    try {
        if (!IsHdlFilePath(path)) return Json::array();
        const std::shared_ptr<const project::Project> project = ProjectForPathShared(path);
        if (project == nullptr) return Json::array();
        const std::shared_ptr<const hdl::HdlIndex> index = IndexForProject(*project);
        if (!index) return Json::array();
        return features_optional::CodeLenses(*index, path);
    } catch (...) {
        return Json::array();
    }
}

Json LspServer::FeatureDocumentHighlights(std::string_view path, int line, int character) {
    try {
        if (!IsHdlFilePath(path)) return Json::array();
        const std::shared_ptr<const project::Project> project = ProjectForPathShared(path);
        if (project == nullptr) return Json::array();
        const std::shared_ptr<const hdl::HdlIndex> index = IndexForProject(*project);
        if (!index) return Json::array();
        return features_optional::DocumentHighlights(*index, path, line, character);
    } catch (...) {
        return Json::array();
    }
}

Json LspServer::FeatureSelectionRanges(std::string_view path, const Json& positions) {
    try {
        const std::optional<std::string> text = LoadText(std::string(path));
        if (!text.has_value()) return Json::array();

        std::vector<std::pair<int, int>> parsed;
        if (positions.is_array()) {
            parsed.reserve(positions.size());
            for (const Json& position : positions) {
                if (!position.is_object()) continue;
                parsed.emplace_back(jsonrpc::GetInt(position, "line", 0),
                                    jsonrpc::GetInt(position, "character", 0));
            }
        }
        if (parsed.empty()) return Json::array();

        std::shared_ptr<const project::Project> project = ProjectForPathShared(path);
        std::shared_ptr<const hdl::HdlIndex> index;
        if (project != nullptr) index = IndexForProject(*project);
        return features_optional::SelectionRanges(index ? *index : EmptyIndex(), path, *text,
                                                  parsed);
    } catch (...) {
        return Json::array();
    }
}

Json LspServer::FeatureSemanticTokensRange(std::string_view path, const Json& range) {
    Json result = Json::object();
    result["data"] = Json::array();
    try {
        // 与全量版共用同一个开关：`fpgaLsp.verible.highlight=false` 时不出 token。
        if (!settings_.verible_highlight) return result;
        const std::optional<std::string> text = LoadText(std::string(path));
        if (!text.has_value()) return result;

        std::shared_ptr<const project::Project> project = ProjectForPathShared(path);
        std::shared_ptr<const hdl::HdlIndex> index;
        if (project != nullptr) index = IndexForProject(*project);

        const Json start = jsonrpc::GetObject(range, "start");
        const Json end = jsonrpc::GetObject(range, "end");
        // 缺省区间 = 整篇（客户端没给 range 时不要返回空）。
        const int start_line = jsonrpc::GetInt(start, "line", 0);
        const int start_character = jsonrpc::GetInt(start, "character", 0);
        const int end_line = jsonrpc::GetInt(end, "line", 1 << 30);
        const int end_character = jsonrpc::GetInt(end, "character", 1 << 30);
        return features_optional::SemanticTokensRange(
            index.get(), path, *text, std::max(start_line, 0), std::max(start_character, 0),
            std::max(end_line, 0), std::max(end_character, 0));
    } catch (...) {
        return result;
    }
}

Json LspServer::FeatureCompletionResolve(const Json& item) {
    try {
        // 只给"模块"候选补文档；没有工程时原样返回（不丢客户端字段）。
        const std::shared_ptr<const project::Project> project = CurrentProjectShared();
        if (project == nullptr) return features_optional::ResolveCompletionItem(nullptr, item);
        const std::shared_ptr<const hdl::HdlIndex> index = IndexForProject(*project);
        return features_optional::ResolveCompletionItem(index.get(), item);
    } catch (...) {
        return item.is_object() ? item : Json::object();
    }
}

Json LspServer::FeatureDocumentDiagnostic(std::string_view uri,
                                          std::string_view previous_result_id) {
    try {
        const std::string uri_string(uri);
        Json items = Json::array();
        {
            // A9：数据源 = 服务端最近一次 push publishDiagnostics 的缓存（同一把
            // state_mutex_ 保护，发布线程与 pull 线程不会读到半份数组）。
            std::scoped_lock lock(state_mutex_);
            const auto found = latest_diagnostics_.find(uri_string);
            if (found != latest_diagnostics_.end()) items = found->second;
        }
        const std::string result_id =
            features_optional::DiagnosticsResultId(uri_string, items);
        if (!previous_result_id.empty() && previous_result_id == result_id) {
            Json unchanged = Json::object();
            unchanged["kind"] = "unchanged";
            unchanged["resultId"] = result_id;
            return unchanged;
        }
        return features_optional::BuildDocumentDiagnosticReport(items, result_id);
    } catch (...) {
        return features_optional::BuildDocumentDiagnosticReport(Json::array(), "");
    }
}

Json LspServer::FeatureWorkspaceDiagnostic() {
    try {
        std::map<std::string, Json> latest;
        {
            std::scoped_lock lock(state_mutex_);
            latest = latest_diagnostics_;
        }
        return features_optional::BuildWorkspaceDiagnosticReport(latest);
    } catch (...) {
        Json report = Json::object();
        report["items"] = Json::array();
        return report;
    }
}

// =============================================================================
//  handler（本文件：A4–A12）
// =============================================================================

HandlerResult LspServer::HandleSignatureHelp(const Json& params) {
    std::string uri;
    int line = 0;
    int character = 0;
    if (!ReadTextDocumentUri(params, &uri) || !ReadCursorPosition(params, &line, &character)) {
        return HandlerResult::Ok(EmptySignatureHelp());
    }
    return HandlerResult::Ok(
        FeatureSignatureHelp(platform::uri_to_path(uri), line, character));
}

HandlerResult LspServer::HandleDocumentLink(const Json& params) {
    std::string uri;
    if (!ReadTextDocumentUri(params, &uri)) return HandlerResult::Ok(Json::array());
    return HandlerResult::Ok(FeatureDocumentLinks(platform::uri_to_path(uri)));
}

HandlerResult LspServer::HandleCodeLens(const Json& params) {
    std::string uri;
    if (!ReadTextDocumentUri(params, &uri)) return HandlerResult::Ok(Json::array());
    return HandlerResult::Ok(FeatureCodeLenses(platform::uri_to_path(uri)));
}

HandlerResult LspServer::HandleDocumentHighlight(const Json& params) {
    std::string uri;
    int line = 0;
    int character = 0;
    if (!ReadTextDocumentUri(params, &uri) || !ReadCursorPosition(params, &line, &character)) {
        return HandlerResult::Ok(Json::array());
    }
    return HandlerResult::Ok(
        FeatureDocumentHighlights(platform::uri_to_path(uri), line, character));
}

HandlerResult LspServer::HandleSelectionRange(const Json& params) {
    std::string uri;
    if (!ReadTextDocumentUri(params, &uri)) return HandlerResult::Ok(Json::array());
    const Json positions = jsonrpc::GetArray(params, "positions");
    return HandlerResult::Ok(
        FeatureSelectionRanges(platform::uri_to_path(uri), positions));
}

HandlerResult LspServer::HandleSemanticTokensRange(const Json& params) {
    std::string uri;
    if (!ReadTextDocumentUri(params, &uri)) {
        Json empty = Json::object();
        empty["data"] = Json::array();
        return HandlerResult::Ok(std::move(empty));
    }
    const Json range = jsonrpc::GetObject(params, "range");
    return HandlerResult::Ok(
        FeatureSemanticTokensRange(platform::uri_to_path(uri), range));
}

HandlerResult LspServer::HandleCompletionResolve(const Json& params) {
    return HandlerResult::Ok(FeatureCompletionResolve(params));
}

HandlerResult LspServer::HandleDocumentDiagnostic(const Json& params) {
    std::string uri;
    if (!ReadTextDocumentUri(params, &uri)) {
        return HandlerResult::Ok(
            features_optional::BuildDocumentDiagnosticReport(Json::array(), ""));
    }
    const std::string previous = jsonrpc::GetString(params, "previousResultId");
    return HandlerResult::Ok(FeatureDocumentDiagnostic(uri, previous));
}

HandlerResult LspServer::HandleWorkspaceDiagnostic(const Json& params) {
    (void)params;  // identifier/previousResultIds 目前不参与（缓存即真相）
    return HandlerResult::Ok(FeatureWorkspaceDiagnostic());
}

// A10：willSave 是通知（无返回值）；willSaveWaitUntil 是请求，必须返回 TextEdit[]
// 并且**立刻**返回（阻塞保存是本项最大的风险，设计上不在这里跑 TD/lint）。
HandlerResult LspServer::HandleWillSave(const Json& params) {
    (void)params;
    return HandlerResult::Ok(Json());
}

HandlerResult LspServer::HandleWillSaveWaitUntil(const Json& params) {
    (void)params;
    return HandlerResult::Ok(Json::array());
}

// A11：`$/setTrace` 记录级别（off/messages/verbose），不报错、不丢弃；
// `$/logTrace` 是对端（或本机诊断）送来的跟踪消息，落 debug 日志后丢弃，
// 但**不会**被 dispatcher 的 `$/` 静默分支吃掉（见 jsonrpc/dispatcher.cpp）。
HandlerResult LspServer::HandleSetTrace(const Json& params) {
    const std::string value = jsonrpc::GetString(params, "value", "off");
    const std::string normalized = LowerAscii(value);
    if (normalized == "off" || normalized == "messages" || normalized == "verbose") {
        std::scoped_lock lock(state_mutex_);
        trace_level_ = normalized;
    }
    return HandlerResult::Ok(Json());
}

HandlerResult LspServer::HandleLogTrace(const Json& params) {
    const std::string message = jsonrpc::GetString(params, "message");
    platform::default_logger().debug("$/logTrace: " + message);
    return HandlerResult::Ok(Json());
}

}  // namespace fpgalsp::server
