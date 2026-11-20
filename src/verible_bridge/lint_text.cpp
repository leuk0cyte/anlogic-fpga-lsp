// =============================================================================
//  src/verible_bridge/lint_text.cpp —— 内嵌 Verible lint autofix
// =============================================================================
//  实现要点（踩坑记录，改之前先读）：
//
//  1. **不要调 verible::InitCommandLine**（format_dump/lint_dump 里调了，因为它们是
//     CLI 工具）。本文件在语言服务器进程里被调用，InitCommandLine 会解析**服务端
//     自己的 argv** 并可能直接 exit。因此配置显式构造 LinterOptions（与
//     lint_dump 的 --lint 分支同一取值），完全不碰 absl flag。
//
//  2. 解析用 `VerilogAnalyzer::AnalyzeAutomaticPreprocessFallback`（与官方
//     verilog-lint.cc 的 LintOneFile 完全一致）：先不预处理，语法错再回退到
//     "过滤 ifdef 分支"。auto-fix 的 fragment 是**指向原始文本缓冲**的 string_view，
//     所以必须用 analyzer 自己的 Data().Contents() 当 base 算偏移。
//
//  3. 规则库是**静态自注册**的：没链接到的规则会静默消失（见 lint_dump 的 CMake
//     注释）。本项目用 verible_lint_rules_all（逐库链接 + /WHOLEARCHIVE）保证规则
//     齐全；少了规则不会报错，只会"少几条 fix"。
//
//  4. 多规则的 fix 可能重叠（同一 token 被两条规则改）。官方 ViolationFixer 是交互
//     式逐个确认；这里按**位置排序、保留先到者、丢弃与其重叠的编辑**（等价于
//     官方 --autofix=inplace 的"全部接受"语义中不会互相打架的那部分）。
//
//  5. 只在最外层 try/catch 兜底：Verible 内部有 CHECK/DCHECK，非异常路径无法拦截，
//     但正常情况下（合法配置 + 任意文本）不会触发。
// =============================================================================

#include "verible_bridge/lint_text.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "verible/common/analysis/lint-rule-status.h"
#include "verible/verilog/analysis/verilog-analyzer.h"
#include "verible/verilog/analysis/verilog-linter.h"
#include "verible/verilog/analysis/verilog-linter-configuration.h"
#include "verible_bridge/flag_tokens.h"

namespace fpgalsp::verible_bridge {

// 旧插件 `fpgaLsp.verible.lintArgs` 的默认值（Python 版是**字符串**，服务端只认
// list，于是被静默丢弃 —— 见 lint_text.h 的说明）。这里保留它的"本意"：
// 行宽阈值 160，与 formatter 的 column_limit 一致。
std::vector<std::string> DefaultLintFlags() {
    return {"--rules=+line-length=length:160"};
}

namespace {

using verible::AutoFix;
using verible::LintRuleStatus;
using verible::ReplacementEdit;

// 一处待应用的替换（偏移相对于 base 文本的起点）
struct AppliedEdit {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::string replacement;
};

// 由 flag 列表构造 LinterConfiguration。
//
// 默认（flags 为空）等价于 `verible-verilog-lint --rules=+line-length=length:160`
// 的规则集，**其余保持官方默认语义**：
//   ruleset = default、不做向上搜索 .rules.verible_lint、无 waiver 文件。
//
// 支持（与旧插件 `fpgaLsp.verible.lintArgs` 同形）：
//   --rules=<+rule,-rule,rule=config>   （可多次出现，累积到同一个 bundle）
//   --ruleset=default|all|none
//   --rules_config=<path>
//   --rules_config_search[=true|false]
//   --waiver_files=<path>
// 其它 flag 归入 unknown（调用方记日志），**不**让一次拼错把 lint 整个关掉。
bool BuildConfiguration(const std::vector<std::string>& flags,
                        verilog::LinterConfiguration* config, std::string* error,
                        std::vector<std::string>* unknown) {
    const std::vector<std::string> effective =
        flags.empty() ? DefaultLintFlags() : flags;

    verilog::RuleBundle bundle;
    verilog::RuleSet ruleset = verilog::RuleSet::kDefault;
    std::string config_file;
    bool config_search = false;
    std::string waiver_files;

    for (const ParsedFlag& flag : ParseFlagList(effective)) {
        const std::string& name = flag.name;

        if (name == "rules") {
            if (!flag.has_value) {
                *error = "--rules 需要一个取值（形如 --rules=+line-length=length:160）";
                return false;
            }
            std::string parse_error;
            if (!verilog::AbslParseFlag(flag.value, &bundle, &parse_error)) {
                *error = "--rules=" + flag.value + " 非法：" + parse_error;
                return false;
            }
            continue;
        }

        if (name == "ruleset") {
            if (!flag.has_value) {
                *error = "--ruleset 需要一个取值（default / all / none）";
                return false;
            }
            std::string parse_error;
            if (!verilog::AbslParseFlag(flag.value, &ruleset, &parse_error)) {
                *error = "--ruleset=" + flag.value + " 非法：" + parse_error;
                return false;
            }
            continue;
        }

        if (name == "rules_config") {
            if (!flag.has_value) {
                *error = "--rules_config 需要一个文件路径";
                return false;
            }
            config_file = flag.value;
            continue;
        }

        if (name == "rules_config_search") {
            if (!flag.has_value) {  // 只写开关 = 打开
                config_search = true;
                continue;
            }
            bool parsed = false;
            if (!ParseBoolValue(flag.value, &parsed)) {
                *error = "--rules_config_search=" + flag.value + " 不是合法布尔值";
                return false;
            }
            config_search = parsed;
            continue;
        }

        if (name == "waiver_files") {
            if (!flag.has_value) {
                *error = "--waiver_files 需要一个文件路径（多为逗号分隔）";
                return false;
            }
            waiver_files = flag.value;
            continue;
        }

        unknown->push_back("--" + name);
    }

    const verilog::LinterOptions options = {
        .ruleset = ruleset,
        .rules = bundle,
        .config_file = config_file,
        .rules_config_search = config_search,
        .linting_start_file = "",
        .waiver_files = waiver_files,
    };
    const absl::Status status = config->ConfigureFromOptions(options);
    if (!status.ok()) {
        *error = status.message();
        return false;
    }
    return true;
}

// 把 string_view fragment 换成 base 里的字节偏移
bool OffsetOf(std::string_view base, std::string_view fragment, std::size_t* begin,
              std::size_t* end) {
    if (fragment.data() < base.data()) return false;
    const std::size_t start =
        static_cast<std::size_t>(fragment.data() - base.data());
    if (start > base.size() || fragment.size() > base.size() - start) return false;
    *begin = start;
    *end = start + fragment.size();
    return true;
}

}  // namespace

LintAutofixResult AutofixText(std::string_view text, std::string_view filename,
                              const std::vector<std::string>& lint_flags) {
    LintAutofixResult result;
    if (text.empty()) {
        result.ok = true;
        result.changed = false;
        result.text = std::string(text);
        return result;
    }

    try {
        const std::string name(filename);

        verilog::LinterConfiguration config;
        if (!BuildConfiguration(lint_flags, &config, &result.error,
                                &result.unknown_flags)) {
            return result;
        }

        // 与官方 LintOneFile 同一个解析入口（失败会返回可用的部分 AST）
        const std::unique_ptr<verilog::VerilogAnalyzer> analyzer =
            verilog::VerilogAnalyzer::AnalyzeAutomaticPreprocessFallback(text, name);
        if (!analyzer) {
            result.error = "verible analyzer unavailable";
            return result;
        }

        const verible::TextStructureView& structure = analyzer->Data();
        const std::string_view base = structure.Contents();

        const absl::StatusOr<std::vector<LintRuleStatus>> statuses =
            verilog::VerilogLintTextStructure(name, config, structure);
        if (!statuses.ok()) {
            result.error = statuses.status().message();
            return result;
        }

        // 收集所有可应用的 auto-fix 编辑
        std::vector<AppliedEdit> edits;
        for (const LintRuleStatus& status : *statuses) {
            for (const verible::LintViolation& violation : status.violations) {
                for (const AutoFix& fix : violation.autofixes) {
                    for (const ReplacementEdit& edit : fix.Edits()) {
                        AppliedEdit applied;
                        if (!OffsetOf(base, edit.fragment, &applied.begin, &applied.end)) {
                            continue;  // fragment 不在 base 里：跳过而不是猜
                        }
                        applied.replacement = edit.replacement;
                        edits.push_back(std::move(applied));
                    }
                }
            }
        }

        // 按位置排序；**丢弃与已保留编辑重叠**的编辑（见文件头第 4 点）
        std::sort(edits.begin(), edits.end(),
                  [](const AppliedEdit& left, const AppliedEdit& right) {
                      if (left.begin != right.begin) return left.begin < right.begin;
                      return left.end < right.end;
                  });

        std::vector<const AppliedEdit*> kept;
        kept.reserve(edits.size());
        std::size_t cursor = 0;  // 已保留编辑覆盖到的最大偏移
        for (const AppliedEdit& edit : edits) {
            if (!kept.empty() && edit.begin < cursor) continue;  // 重叠 -> 丢弃
            kept.push_back(&edit);
            cursor = std::max(cursor, edit.end);
        }

        if (kept.empty()) {
            result.ok = true;
            result.changed = false;
            result.text = std::string(text);
            return result;
        }

        // 拼出新文本
        std::string fixed;
        fixed.reserve(base.size() + kept.size() * 8);
        std::size_t position = 0;
        for (const AppliedEdit* edit : kept) {
            fixed.append(base.substr(position, edit->begin - position));
            fixed.append(edit->replacement);
            position = edit->end;
        }
        fixed.append(base.substr(position));

        result.ok = true;
        result.applied_edits = static_cast<int>(kept.size());
        result.text = std::move(fixed);
        result.changed = result.text != base;
        return result;
    } catch (const std::exception& error) {
        result.ok = false;
        result.error = std::string("verible lint threw: ") + error.what();
        return result;
    } catch (...) {
        result.ok = false;
        result.error = "verible lint threw: unknown";
        return result;
    }
}

// -----------------------------------------------------------------------------
//  T1 / task-50：只收集 violation（服务端 lint）
// -----------------------------------------------------------------------------
LintResult LintText(std::string_view text, std::string_view filename,
                    const std::vector<std::string>& lint_flags) {
    LintResult result;
    if (text.empty()) {
        result.ok = true;
        return result;
    }

    try {
        const std::string name(filename);

        verilog::LinterConfiguration config;
        if (!BuildConfiguration(lint_flags, &config, &result.error,
                                &result.unknown_flags)) {
            return result;
        }

        const std::unique_ptr<verilog::VerilogAnalyzer> analyzer =
            verilog::VerilogAnalyzer::AnalyzeAutomaticPreprocessFallback(text, name);
        if (!analyzer) {
            result.error = "verible analyzer unavailable";
            return result;
        }

        const verible::TextStructureView& structure = analyzer->Data();
        const absl::StatusOr<std::vector<LintRuleStatus>> statuses =
            verilog::VerilogLintTextStructure(name, config, structure);
        if (!statuses.ok()) {
            result.error = statuses.status().message();
            return result;
        }

        for (const LintRuleStatus& status : *statuses) {
            const std::string rule(status.lint_rule_name);
            for (const verible::LintViolation& violation : status.violations) {
                // token -> (line, column) 由 TextStructure 的 LineColumnMap 给出；
                // 它是 0-based、**码点**列（多字节字符算 1 列），server 侧再转 UTF-16。
                const verible::LineColumnRange range =
                    structure.GetRangeForToken(violation.token);
                LintDiagnostic diagnostic;
                diagnostic.line = range.start.line;
                diagnostic.column = range.start.column;
                diagnostic.end_line = range.end.line;
                diagnostic.end_column = range.end.column;
                diagnostic.severity = 2;  // lint 一律 warning（与 verible CLI 的措辞一致）
                diagnostic.rule = rule;
                diagnostic.message = violation.reason;
                result.diagnostics.push_back(std::move(diagnostic));
            }
        }

        // 稳定顺序（同一位置按规则名），便于上层做"内容相同就不重发"的比较。
        std::stable_sort(result.diagnostics.begin(), result.diagnostics.end(),
                         [](const LintDiagnostic& a, const LintDiagnostic& b) {
                             if (a.line != b.line) return a.line < b.line;
                             if (a.column != b.column) return a.column < b.column;
                             if (a.rule != b.rule) return a.rule < b.rule;
                             return a.message < b.message;
                         });
        result.ok = true;
        return result;
    } catch (const std::exception& error) {
        result.ok = false;
        result.error = std::string("verible lint threw: ") + error.what();
        return result;
    } catch (...) {
        result.ok = false;
        result.error = "verible lint threw: unknown";
        return result;
    }
}

// -----------------------------------------------------------------------------
//  T1 / task-50：丢掉"注释里的 no-tabs"
// -----------------------------------------------------------------------------
namespace {

// 每一行里"哪些码点列属于注释"的掩码（1 = 注释）。
// 只做 Verilog 需要的三种词法状态：行注释、块注释、双引号字符串（含转义）。
std::vector<std::vector<char>> BuildCommentMask(std::string_view text) {
    std::vector<std::vector<char>> mask(1);
    std::size_t line = 0;
    std::size_t column = 0;
    bool in_block = false;
    bool in_string = false;
    bool escaped = false;

    auto mark = [&](std::size_t ln, std::size_t col) {
        if (ln >= mask.size()) return;
        if (mask[ln].size() <= col) mask[ln].resize(col + 1, 0);
        mask[ln][col] = 1;
    };
    // UTF-8 码点长度（非法字节按 1 处理，never-raise）
    auto utf8_len = [](unsigned char c) -> std::size_t {
        if ((c & 0xE0) == 0xC0) return 2;
        if ((c & 0xF0) == 0xE0) return 3;
        if ((c & 0xF8) == 0xF0) return 4;
        return 1;
    };

    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        const char next = (i + 1 < text.size()) ? text[i + 1] : '\0';
        if (c == '\n') {
            ++line;
            column = 0;
            mask.emplace_back();
            escaped = false;
            ++i;
            continue;
        }
        const std::size_t len = utf8_len(c);

        if (in_block) {
            if (c == '*' && next == '/') {
                mark(line, column);
                mark(line, column + 1);
                in_block = false;
                i += 2;
                column += 2;
                continue;
            }
            mark(line, column);
            i += len;
            ++column;
            continue;
        }
        if (in_string) {
            if (escaped) {
                escaped = false;
                i += len;
                ++column;
                continue;
            }
            if (c == '\\') {
                escaped = true;
                i += len;
                ++column;
                continue;
            }
            if (c == '"') in_string = false;
            i += len;
            ++column;
            continue;
        }
        if (c == '/' && next == '/') {
            std::size_t j = i;
            std::size_t col = column;
            while (j < text.size() && text[j] != '\n') {
                mark(line, col);
                j += utf8_len(static_cast<unsigned char>(text[j]));
                ++col;
            }
            i = j;
            column = col;
            continue;
        }
        if (c == '/' && next == '*') {
            in_block = true;
            mark(line, column);
            mark(line, column + 1);
            i += 2;
            column += 2;
            continue;
        }
        if (c == '"') {
            in_string = true;
            i += 1;
            ++column;
            continue;
        }
        i += len;
        ++column;
    }
    return mask;
}

bool ColumnInComment(const std::vector<std::vector<char>>& mask, int line, int column) {
    if (line < 0 || static_cast<std::size_t>(line) >= mask.size()) return false;
    if (column < 0) return false;
    const std::vector<char>& row = mask[static_cast<std::size_t>(line)];
    return static_cast<std::size_t>(column) < row.size() && row[static_cast<std::size_t>(column)] != 0;
}

}  // namespace

std::vector<LintDiagnostic> DropCommentNoTabs(std::string_view text,
                                             std::vector<LintDiagnostic> diagnostics) {
    if (diagnostics.empty()) return diagnostics;
    const std::vector<std::vector<char>> mask = BuildCommentMask(text);
    std::vector<LintDiagnostic> kept;
    kept.reserve(diagnostics.size());
    for (LintDiagnostic& diagnostic : diagnostics) {
        // 只对 no-tabs 做这个过滤（其它规则在注释里仍然有意义，例如行宽）。
        if (diagnostic.rule == "no-tabs" &&
            ColumnInComment(mask, diagnostic.line, diagnostic.column)) {
            continue;
        }
        kept.push_back(std::move(diagnostic));
    }
    return kept;
}

}  // namespace fpgalsp::verible_bridge