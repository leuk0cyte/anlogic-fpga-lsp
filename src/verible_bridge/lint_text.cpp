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

namespace fpgalsp::verible_bridge {
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

// 与 `verible-verilog-lint`（不带参数）等价的规则配置：
//   ruleset = default、无额外规则、**不做**向上搜索 .rules.verible_lint、
//   无 waiver 文件。
bool BuildDefaultConfiguration(verilog::LinterConfiguration* config, std::string* error) {
    const verilog::RuleBundle bundle;  // 空：只用 ruleset 的默认集合
    const verilog::LinterOptions options = {
        .ruleset = verilog::RuleSet::kDefault,
        .rules = bundle,
        .config_file = "",
        .rules_config_search = false,
        .linting_start_file = "",
        .waiver_files = "",
    };
    const absl::Status status = config->ConfigureFromOptions(options);
    if (!status.ok()) {
        if (error != nullptr) *error = status.message();
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

LintAutofixResult AutofixText(std::string_view text, std::string_view filename) {
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
        if (!BuildDefaultConfiguration(&config, &result.error)) return result;

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

}  // namespace fpgalsp::verible_bridge