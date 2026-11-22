// =============================================================================
//  lint_dump —— Verible lint 行为导出工具（阶段 1 门禁用）
// =============================================================================
//  用途：把 vendored Verible（CMake 化之后）的 lint 结果 dump 出来，与官方
//        发行的 verible-verilog-lint.exe 做逐字节对比。
//
//  设计原则（与阶段 0 的 cst_dump 完全一致）：
//    **不重写输出格式，而是直接调用官方同一套函数**，这样逐字节一致是
//    「构造上保证」的，而不是靠人工对齐。任何差异都只可能来自：
//      1. CMake 化编译后代码语义变化
//      2. 第三方依赖版本差异
//      3. **规则库漏链接**（阶段 1 头号风险，见下）
//      4. 转译时的 target 边界错误
//
//  对应关系（官方 verilog/verilog/tools/lint/verilog-lint.cc）：
//    --print_rules_file  ->  LinterConfigurationFromFlags("") + GetLintRuleFile()
//    普通 lint           ->  LintOneFile()（见 --lint）
//
//  ⚠️ 阶段 1 头号风险（务必理解）：
//    Verible 的每条 lint 规则靠 VERILOG_REGISTER_LINT_RULE 宏产生的
//    **static 全局对象在程序启动时自注册**。如果某条规则的 .cc 所在静态库
//    没有被链接进最终可执行文件，**那条规则会静默消失** —— 程序正常运行、
//    不报任何错，只是少了一条规则。
//
//    因此 --rules-file 的输出行数就是一张「链接完整性检查表」：
//      61 行 = 全部规则都在；62 行 = 少了 62-61 条，用 compare_lint.py
//      --diagnose-missing 精确定位是哪条、在哪个库。
//
//  ⚠️ 「源码层面不需要」≠「链接层面不需要」：
//    GetLintRuleFile / LinterConfigurationFromFlags 函数体本身不碰 parser，
//    但它们所在的 verilog-linter.cc **这个翻译单元** include 了 verilog-analyzer.h
//    并被链进 verilog-linter.lib。C++ 静态库的链接粒度是 TU 而非函数，
//    所以链接 verilog-linter.lib 就会要求 parser 符号（verilog-final.tab.cc）。
//    这一点曾让我误判「第 1 层不需要 parser」。
//
//  命令行：
//    lint_dump --rules-file              输出规则清单（对齐官方 --print_rules_file）
//    lint_dump --ruleset=<default|all|none>
//    lint_dump --rules=<spec>            形如 "+a,-b"（对齐官方 --rules）
//    lint_dump --lint <file>...          输出诊断（对齐官方普通 lint）
//    lint_dump --check-syntax            lint 前先检查语法错误
//
//  退出码：0 正常；1 有违规/语法错误（视 --lint-fatal/--parse-fatal）；2 用法错误
// =============================================================================

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/status/statusor.h"
#include "verible/common/util/init-command-line.h"
#include "verible/verilog/analysis/verilog-linter.h"
#include "verible/verilog/analysis/verilog-linter-configuration.h"

namespace {

using verilog::LinterConfiguration;
using verilog::LinterOptions;
using verilog::RuleSet;

// 把 --ruleset 字符串转成 RuleSet 枚举。
// 不能用 absl 的 FLAGS 机制直接绑，因为我们要显式控制默认值与透传。
bool ParseRuleSet(std::string_view text, RuleSet *out) {
  if (text == "default") {
    *out = RuleSet::kDefault;
    return true;
  }
  if (text == "all") {
    *out = RuleSet::kAll;
    return true;
  }
  if (text == "none") {
    *out = RuleSet::kNone;
    return true;
  }
  std::cerr << "lint_dump: invalid --ruleset value: " << text << '\n';
  return false;
}

void PrintUsage(const char *prog) {
  std::cerr
      << "usage: " << prog << " [--rules-file] [--ruleset=<default|all|none>]\n"
      << "                        [--rules=<spec>] [--lint] [--check-syntax]\n"
      << "                        [file...]\n"
      << "\n"
      << "  --rules-file    print the lint rule configuration file and exit\n"
      << "                  (same as verible-verilog-lint --print_rules_file)\n"
      << "  --ruleset=      base rule set: default|all|none\n"
      << "  --rules=        comma-separated rule spec, e.g. \"+a,-b\"\n"
      << "  --lint          lint the given files (same as verible-verilog-lint)\n"
      << "  --check-syntax  also report syntax errors while linting\n";
}

}  // namespace

int main(int argc, char **argv) {
  // ⚠️ 顺序很关键：
//   verible::InitCommandLine() 内部用 absl 解析 argv，**遇到未知 flag 会直接报错退出**
//   （"ERROR: Unknown command line flag 'rules-file'"）。
//   本工具的自定义参数（--rules-file / --lint / --rules= / --ruleset=）absl 不认识，
//   所以必须**先自己解析并剥离**，再把剩下的 argv 交给 InitCommandLine。
//
//   这一点实测踩过：先调 InitCommandLine 再解析，--rules-file 直接被 absl 拒掉。
  bool print_rules_file = false;
  bool do_lint = false;
  bool check_syntax = true;   // 与官方 ABSL_FLAG(check_syntax, true) 默认一致
  std::string ruleset_text = "default";
  std::string rules_text;
  std::vector<std::string> files;
  std::vector<char *> passthrough;
  passthrough.push_back(argv[0]);

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--rules-file") {
      print_rules_file = true;
    } else if (arg == "--lint") {
      do_lint = true;
    } else if (arg == "--check-syntax") {
      check_syntax = true;
    } else if (arg.starts_with("--ruleset=")) {
      ruleset_text = std::string(arg.substr(10));
    } else if (arg.starts_with("--rules=")) {
      rules_text = std::string(arg.substr(8));
    } else if (arg == "--help" || arg == "-h") {
      PrintUsage(argv[0]);
      return 0;
    } else if (!arg.empty() && arg.front() == '-') {
      std::cerr << argv[0] << ": unknown option: " << arg << '\n';
      PrintUsage(argv[0]);
      return 2;
    } else {
      files.emplace_back(arg);
      passthrough.push_back(argv[i]);
    }
  }

  // 现在把剥离后的 argv 交给 absl（只剩程序名 + 文件）
  int pass_argc = static_cast<int>(passthrough.size());
  char **pass_argv = passthrough.data();
  const auto usage = std::string("usage: ") + argv[0] +
                     " [options] [file...]";
  verible::InitCommandLine(usage, &pass_argc, &pass_argv);

  RuleSet ruleset = RuleSet::kDefault;
  if (!ParseRuleSet(ruleset_text, &ruleset)) return 2;

  // --rules 字符串 -> RuleBundle。
  // 官方用 ABSL_FLAG(verilog::RuleBundle, rules, ...) 并依赖 absl 的 flag 解析器，
  // 其内部调用的正是 RuleBundle::ParseConfiguration(text, ',', &error)。
  // 这里直接调用同一个函数，保证解析行为一致。
  verilog::RuleBundle rule_bundle;
  if (!rules_text.empty()) {
    std::string parse_error;
    if (!rule_bundle.ParseConfiguration(rules_text, ',', &parse_error)) {
      std::cerr << "lint_dump: invalid --rules: " << parse_error << '\n';
      return 2;
    }
  }

  // ---------------------------------------------------------------------------
  // --rules-file：与官方 --print_rules_file 走完全相同的两个函数
  // ---------------------------------------------------------------------------
  if (print_rules_file) {
    // 官方等价：LinterConfigurationFromFlags("")
    // 这里直接构造 LinterOptions 而不读 FLAGS，避免命令行 flag 相互干扰；
    // 但字段取值与官方 LinterConfigurationFromFlags 完全一致。
    const LinterOptions options = {
        .ruleset = ruleset,
        .rules = rule_bundle,
        .config_file = "",
        .rules_config_search = false,  // 关键：禁止向上搜索 .rules.verible_lint
        .linting_start_file = "",
        .waiver_files = "",
    };
    LinterConfiguration config;
    const absl::Status status = config.ConfigureFromOptions(options);
    if (!status.ok()) {
      std::cerr << status.message() << std::endl;
      return 1;
    }
    // 官方等价：verilog::GetLintRuleFile(&std::cout, config)
    verilog::GetLintRuleFile(&std::cout, config);
    return 0;
  }

  // ---------------------------------------------------------------------------
  // --lint：走官方 LintOneFile
  //
  // ⚠️ violation_handler 不能传 nullptr：
  //    verilog-linter.cc:168 是**无条件解引用**
  //        violation_handler->HandleViolations(violations, text_base, filename);
  //    传空指针会直接 SIGSEGV（表现为栈探测 _chkstk，极易误判成栈溢出/递归）。
  //    官方 verilog-lint.cc 用的是 `new verible::ViolationPrinter(&std::cerr)`。
  //
  //    本工具的输出约定是「工具输出走 stdout」，所以这里用 &std::cout
  //    （对比脚本会合并 stdout+stderr，两边通道差异不影响比较）。
  // ---------------------------------------------------------------------------
  if (do_lint) {
    if (files.empty()) {
      PrintUsage(argv[0]);
      return 2;
    }
    verible::ViolationPrinter violation_printer(&std::cout);
    int exit_status = 0;
    for (const auto &filename : files) {
      const LinterOptions options = {
          .ruleset = ruleset,
          .rules = rule_bundle,
          .config_file = "",
          .rules_config_search = false,
          .linting_start_file = filename,
          .waiver_files = "",
      };
      LinterConfiguration config;
      const absl::Status status = config.ConfigureFromOptions(options);
      if (!status.ok()) {
        std::cerr << status.message() << std::endl;
        exit_status = 1;
        continue;
      }
      // 与官方一致：lint_fatal / parse_fatal 均为 true（官方默认值）
      const int lint_status =
          verilog::LintOneFile(&std::cout, filename, config,
                               &violation_printer, check_syntax,
                               /*parse_fatal=*/true, /*lint_fatal=*/true,
                               /*show_diagnostic_context=*/false);
      if (lint_status > exit_status) exit_status = lint_status;
    }
    return exit_status;
  }

  PrintUsage(argv[0]);
  return 2;
}