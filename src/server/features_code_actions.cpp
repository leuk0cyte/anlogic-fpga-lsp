// =============================================================================
//  src/server/features_code_actions.cpp —— textDocument/codeAction
// =============================================================================
//  OWNER: task B3 (parity-lsp)。对应矩阵 LSP-03（S0）。
//
//  对应参考实现：Y:\fpga-lsp\fpgalsp\server.py:1517-1609
//      _adc_code_actions / _code_action_position / on_textDocument_codeAction
//
//  返回形状（Python 原样，**不新增字段**）：
//      [ {title, kind, isPreferred?, edit:{changes:{uri:[{range,newText}]}}} ]
//  顺序也与 Python 一致：实例化 refactor 在前，Verible autofix quickfix 在后。
//
//  三类动作（与 Python 的分支一一对应）：
//    1. 模块实例化 refactor（光标在模块名 / 实例名 / 裸标识符上）：
//       title = "Insert instantiation of <module>"，kind = "refactor"，isPreferred = true，
//       newText = ide::BuildInstantiationText(module, 行首缩进)（与 Python
//       _instantiation_text 逐字节一致，B1a 已有单测），插入点 = 当前行 (line,0)。
//    2. Verible lint autofix quickfix（整篇替换）：**内嵌 Verible**，
//       src/verible_bridge/lint_text.h::AutofixText（同一套规则、进程内跑；
//       产品约束：单一静态 exe，不依赖外部 verible-verilog-lint.exe）。
//       之后与 Python 一样跑注释内 tab 展开（verible.expand_comment_tabs）。
//    3. .adc 引脚分配行的 quickfix（Python _adc_code_actions，2 条）：
//       "Remove this pin assignment" / "Add IOSTANDARD = LVCMOS33"。
//       这部分本来算 LSP-11（.adc 符号能力，B10），但它属于 codeAction 的**协议出口**，
//       放在这里才不用让 B10 再改一次本文件。
//
//  ⚠ 与 Python 的已知差异（见 B3-REPORT.md）：
//    * autofix 用的是**内嵌**规则集（c6b29d62）而 Python 用外部 0.0-4296，
//      同一输入的修复文本可能不同（LSP-18 同族的版本基线差异）；
//      形状、动作条数、range 口径仍与 Python 逐字段一致。
//    * 未链接 verible 桥（FPGA_LSP_VERIBLE_ENABLED=OFF）时不产生该 quickfix
//      （Python 在外部工具缺失时同样不产生）。
//    * 文本先做 LF 归一化再参与计算：Python 的读取是 universal newlines（现成 LF 文本），
//      而 C++ 的 LoadText 给的是原始字节（可能是 CRLF）。两者归一后逐字节可比。
//      （同一根因见 PROBE.md §3.6 / LSP-18。）
// =============================================================================

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hdl/hdl_index.h"
#include "jsonrpc/json_util.h"
#include "platform/path.h"
#include "platform/utf16.h"
#include "server/features_rename_util.h"
#include "server/ide_features.h"
#include "server/lsp_server.h"

#if defined(FPGA_LSP_HAVE_VERIBLE_BRIDGE)
#include "verible_bridge/lint_text.h"
#endif

namespace fpgalsp::server {
namespace {

// -----------------------------------------------------------------------------
//  JSON 小工具
// -----------------------------------------------------------------------------

Json Position(int line, int character) {
    Json position = Json::object();
    position["line"] = line;
    position["character"] = character;
    return position;
}

Json MakeRange(int start_line, int start_character, int end_line, int end_character) {
    Json range = Json::object();
    range["start"] = Position(start_line, start_character);
    range["end"] = Position(end_line, end_character);
    return range;
}

// Python server.py::_code_action_position：codeAction 带的是 **range**，只看 start
void CodeActionPosition(const Json& range, int* line, int* character) {
    *line = 0;
    *character = 0;
    if (!range.is_object()) return;
    const Json start = jsonrpc::GetObject(range, "start");
    if (!start.is_object()) return;
    *line = std::max(0, jsonrpc::GetInt(start, "line", 0));
    *character = std::max(0, jsonrpc::GetInt(start, "character", 0));
}

std::string LowerAscii(std::string_view text) {
    std::string out(text);
    for (char& ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

bool EndsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Python: doc.path.lower().endswith((".v", ".sv", ".vh", ".svh"))
bool IsCodeActionHdlPath(const std::string& lower_path) {
    return EndsWith(lower_path, ".v") || EndsWith(lower_path, ".sv") ||
           EndsWith(lower_path, ".vh") || EndsWith(lower_path, ".svh");
}

// 行切分（Python 的 text.split("\n")）。输入已是 LF 规范化的文本。
std::vector<std::string_view> SplitLines(std::string_view text) {
    std::vector<std::string_view> rows;
    std::size_t position = 0;
    for (;;) {
        const std::size_t newline = text.find('\n', position);
        if (newline == std::string_view::npos) {
            rows.push_back(text.substr(position));
            return rows;
        }
        rows.push_back(text.substr(position, newline - position));
        position = newline + 1;
    }
}

// 覆盖全文的 range：Python 用 text.split("\n") 的最后一行 + 该行长度。
Json WholeDocumentRange(std::string_view text) {
    const std::vector<std::string_view> rows = SplitLines(text);
    const int last_line = static_cast<int>(rows.size()) - 1;
    return MakeRange(0, 0, last_line,
                     static_cast<int>(platform::utf16_len(rows.back())));
}

// -----------------------------------------------------------------------------
//  动作构造
// -----------------------------------------------------------------------------

Json MakeEditAction(std::string title, std::string kind, const std::string& uri,
                    const Json& range, std::string new_text, bool preferred = false) {
    Json edit = Json::object();
    edit["range"] = range;
    edit["newText"] = std::move(new_text);

    Json edits = Json::array();
    edits.push_back(std::move(edit));

    Json changes = Json::object();
    changes[uri] = std::move(edits);

    Json action = Json::object();
    action["title"] = std::move(title);
    action["kind"] = std::move(kind);
    if (preferred) action["isPreferred"] = true;
    action["edit"] = Json::object();
    action["edit"]["changes"] = std::move(changes);
    return action;
}

// 实例化 refactor（Python on_textDocument_codeAction 的第一个分支）
std::optional<Json> InstantiationAction(const hdl::HdlIndex& index, const std::string& path,
                                        const std::string& uri, std::string_view text,
                                        int line, int character) {
    std::string module_name;
    if (const std::optional<hdl::SymbolAtResult> symbol = index.SymbolAt(path, line, character);
        symbol.has_value()) {
        if (symbol->kind == "module") {
            module_name = symbol->name;
        } else if (symbol->kind == "instance") {
            module_name = symbol->module;
        }
    }
    if (module_name.empty()) {
        // Python: 索引里没有符号时退回"光标处的裸标识符"（refs.token_at）
        module_name = edit::TextTokenAt(text, line, character).text;
    }
    if (module_name.empty()) return std::nullopt;

    const hdl::Module* module = index.ModuleByName(module_name);
    if (module == nullptr) return std::nullopt;

    // Python: row = line_at(doc.text, line); indent = 行首空白（空行 -> 4 空格）
    const std::string_view row = platform::line_at(text, static_cast<std::size_t>(line));
    const std::string indent = edit::LeadingIndent(row);
    const ide::InstantiationText built = ide::BuildInstantiationText(*module, indent);

    return MakeEditAction("Insert instantiation of " + module->name, "refactor", uri,
                          MakeRange(line, 0, line, 0), built.text, /*preferred=*/true);
}

// .adc 引脚分配 quickfix（Python _adc_code_actions，逐字移植）
Json AdcCodeActions(std::string_view text, const std::string& uri, int line) {
    Json actions = Json::array();
    const std::vector<std::string_view> rows = SplitLines(text);
    if (line < 0 || static_cast<std::size_t>(line) >= rows.size()) return actions;
    const std::string_view row = rows[static_cast<std::size_t>(line)];
    if (row.find("set_pin_assignment") == std::string_view::npos) return actions;

    const Json delete_range =
        (static_cast<std::size_t>(line) + 1 < rows.size())
            ? MakeRange(line, 0, line + 1, 0)
            : MakeRange(line, 0, line, static_cast<int>(platform::utf16_len(row)));
    actions.push_back(MakeEditAction("Remove this pin assignment", "quickfix", uri,
                                     delete_range, ""));

    // Python: "IOSTANDARD" not in row.upper() 等价于大小写不敏感查找
    const bool has_iostandard = LowerAscii(row).find("iostandard") != std::string::npos;
    if (!has_iostandard && row.find('}') != std::string_view::npos) {
        const std::size_t insert_at = row.rfind('}');
        const int column = static_cast<int>(platform::utf16_len(row.substr(0, insert_at)));
        actions.push_back(MakeEditAction("Add IOSTANDARD = LVCMOS33", "quickfix", uri,
                                         MakeRange(line, column, line, column),
                                         " IOSTANDARD = LVCMOS33;", /*preferred=*/true));
    }
    return actions;
}

#if defined(FPGA_LSP_HAVE_VERIBLE_BRIDGE)
// -----------------------------------------------------------------------------
//  lint 结果缓存（F1 / task-36）
// -----------------------------------------------------------------------------
//  为什么缓存的是**整份结果**而不是"动作"：codeAction 的 Verible 分支是
//  `AutofixText(text) -> expand_comment_tabs -> 整篇替换`，结果只取决于
//  (文件名, 规范化后的文本)；缓存这一层的产物，下面的分支代码可以**一字不改**，
//  因此响应字节必然与未加缓存时相同。
//
//  键 = (文件名, 文本内容哈希)：
//    * 文件名参与：Verible 有按文件名判定的规则（如 module-filename）；
//    * 规则集是编译期固定的（BuildDefaultConfiguration 每次都构造同一份 default
//      ruleset，`lint_text.cpp:61-77`），所以没有"可变配置哈希"。将来若规则集变成
//      可配置，把配置版本号并进键即可（见 F1 报告 §风险）。
//
//  上界：8 条 LRU；单条 >2 MiB 的文本不进缓存（避免 8 × 超大文本吃内存）。
//  线程安全：codeAction 现在跑在 AsyncDispatcher 的 worker 上（最多 2 个并发），
//  缓存自身有锁；Verible 的 `AutofixText` 内部会建 LinterConfiguration+Analyzer，
//  其全局状态的可重入性未验证，因此**执行**部分单独用一把锁串行化（命中不经过）。
// -----------------------------------------------------------------------------
namespace {

constexpr std::size_t kLintCacheCapacity = 8;
constexpr std::size_t kLintCacheMaxEntryBytes = 2u * 1024u * 1024u;

struct LintCacheEntry {
    std::string filename;
    std::uint64_t text_hash = 0;
    verible_bridge::LintAutofixResult result;
};

class LintCache {
public:
    // 命中：返回 true 并把结果**拷贝**给调用方（调用方会继续修改 fixed.text）。
    bool TryGet(const std::string& filename, std::uint64_t text_hash,
                verible_bridge::LintAutofixResult* out) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->text_hash != text_hash || it->filename != filename) continue;
            *out = it->result;
            entries_.splice(entries_.begin(), entries_, it);  // 提升为 MRU
            return true;
        }
        return false;
    }

    void Put(const std::string& filename, std::uint64_t text_hash,
             verible_bridge::LintAutofixResult result) {
        if (result.text.size() > kLintCacheMaxEntryBytes) return;
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->text_hash == text_hash && it->filename == filename) {
                it->result = std::move(result);
                entries_.splice(entries_.begin(), entries_, it);
                return;
            }
        }
        LintCacheEntry entry;
        entry.filename = filename;
        entry.text_hash = text_hash;
        entry.result = std::move(result);
        entries_.push_front(std::move(entry));
        while (entries_.size() > kLintCacheCapacity) entries_.pop_back();
    }

private:
    std::mutex mutex_;
    std::list<LintCacheEntry> entries_;  // front = 最近使用
};

LintCache& SharedLintCache() {
    static LintCache cache;  // 函数内 static：C++11 起初始化线程安全
    return cache;
}

std::mutex& LintExecutionMutex() {
    static std::mutex mutex;
    return mutex;
}

}  // namespace
#endif  // FPGA_LSP_HAVE_VERIBLE_BRIDGE

}  // namespace

// =============================================================================
//  textDocument/codeAction
// =============================================================================

Json LspServer::FeatureCodeActions(std::string_view path, const Json& range,
                                   const Json& /*context*/) {
    Json actions = Json::array();
    const std::string path_string(path);
    if (path_string.empty()) return actions;

    const std::string lower_path = LowerAscii(path_string);
    const std::string uri = platform::path_to_uri(path_string);
    int line = 0;
    int character = 0;
    CodeActionPosition(range, &line, &character);

    // Python: 文档文本一律按 universal newlines 读（即 LF 文本）。
    // C++ 的 LoadText 可能是 CRLF 原文（PROBE §3.6 / LSP-18 的同一根因），
    // 这里统一规范化，保证 newText 与整篇 range 与 Python 逐字节可比。
    std::string text;
    if (const std::optional<std::string> loaded = LoadText(path_string); loaded.has_value()) {
        text = edit::NormalizeNewlines(*loaded);
    }

    // .adc：只做引脚分配 quickfix（Python 在这里就 return，不走 Verilog 分支）
    if (EndsWith(lower_path, ".adc")) {
        return AdcCodeActions(text, uri, line);
    }
    if (!IsCodeActionHdlPath(lower_path)) return actions;

    // C1：只读工程 -> shared 访问器。
    const std::shared_ptr<const project::Project> project = ProjectForPathShared(path_string);
    const std::shared_ptr<const hdl::HdlIndex> index =
        project != nullptr ? IndexForProject(*project) : nullptr;

    // 1) 实例化 refactor
    if (index) {
        if (std::optional<Json> action =
                InstantiationAction(*index, path_string, uri, text, line, character);
            action.has_value()) {
            actions.push_back(std::move(*action));
        }
    }

    // 2) Verible lint autofix quickfix（整篇替换，内嵌规则集）
#if defined(FPGA_LSP_HAVE_VERIBLE_BRIDGE)
    // F1：先查缓存（键 = 文件名 + 规范化文本的内容哈希）。命中时**不**跑 Verible；
    // 未命中才在 LintExecutionMutex 保护下跑一次并回填。下面的分支逻辑与缓存前
    // 完全一致 —— 缓存只影响"算多久"，不影响"算什么"。
    const std::uint64_t text_hash = hdl::ContentHash(text);
    verible_bridge::LintAutofixResult fixed;
    const bool cache_hit = SharedLintCache().TryGet(path_string, text_hash, &fixed);
    if (!cache_hit) {
        {
            std::lock_guard<std::mutex> lock(LintExecutionMutex());
            fixed = verible_bridge::AutofixText(text, path_string, settings_.verible_lint_args);
        }
        SharedLintCache().Put(path_string, text_hash, fixed);
    }
    if (fixed.ok) {
        // Python: fixed = expand_comment_tabs(fixed); changed = fixed != doc.text
        std::string new_text = edit::ExpandCommentTabs(edit::NormalizeNewlines(fixed.text));
        if (new_text != text && !new_text.empty()) {
            actions.push_back(MakeEditAction("Verible: apply lint autofixes", "quickfix", uri,
                                             WholeDocumentRange(text), std::move(new_text)));
        }
    }
#endif

    return actions;
}

}  // namespace fpgalsp::server