// =============================================================================
//  format_dump —— 阶段 1 formatter 门禁的「我方端点」
// =============================================================================
//  用途：用**内嵌 Verible 的 C++ API** 格式化一个文件，输出到 stdout，
//        供 tests/compare_format.py 与官方 verible-verilog-format.exe 逐字节对比。
//
//  ============================ 设计原则 ============================
//  **不重写任何输出逻辑，逐项复用官方 verilog-format.cc 的同一条路径：**
//
//    | 环节              | 官方 verilog-format.cc                | 本工具 |
//    | ----------------- | ------------------------------------- | ------ |
//    | stdout 二进制模式 | _setmode(_O_BINARY)                   | 同     |
//    | 参数解析          | verible::InitCommandLine              | 同     |
//    | 读文件            | verible::file::GetContentAsString     | 同     |
//    | style 构造        | InitializeFromFlags(&style)           | 同     |
//    | 诊断输出通道      | formatter_control.stream = &std::cout | 同     |
//    | 格式化            | FormatVerilog(...)                    | 同     |
//
//  因此「默认参数一致」与「输出字节一致」是**构造上保证**的，
//  而不是靠人工逐项对齐。一旦有差异，那一定是移植本身的问题。
//
//  ===================== 实测踩到的两个坑（务必保留） =====================
//
//  ★ 坑 1：stdout 必须设成二进制模式
//    Windows CRT 默认把 stdout 当文本流，**写 "\n" 会自动变成 "\r\n"**。
//    官方在 main() 第一行就 `_setmode(_fileno(stdout), _O_BINARY)` 关掉了这个转换。
//    我第一版没设，结果：官方输出 478 字节（裸 LF），我方 510 字节（CRLF），
//    首个差异在 offset 18（官方 0x0A，我方 0x0D）。
//    —— 这与本项目 LSP 传输层踩到的 `\r\r\n` 是**同一类**问题：
//       CRT 文本模式会静默改写字节，而且不报任何错。
//
//  ★ 坑 2：ExecutionControl::max_search_states 的**结构体默认值 ≠ CLI 默认值**
//    结构体里默认 10000，官方 CLI 的 --max_search_states 默认 **100000**。
//    官方在 formatter_control 里显式从 flag 赋值。我们必须照做，
//    否则遇到需要大量搜索的换行决策时会提前报 "search space exceeded"。
//    —— 这类"默认值不一致"造成的差异看起来像"格式化算法不同"，极难归因。
//
//  用法：
//    format_dump.exe [flags] <file>
//    flags 与官方 verible-verilog-format 同名同义（--helpfull 查看）。
//
//  退出码：
//    0 = 格式化成功
//    1 = 输入有语法错误或格式化失败（stdout 为原文，与官方 failsafe 行为一致）
//    2 = 用法错误 / 读文件失败
// =============================================================================

#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

#include "absl/flags/flag.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "verible/common/formatting/basic-format-style-init.h"
#include "verible/common/util/file-util.h"
#include "verible/common/strings/position.h"
#include "verible/common/util/init-command-line.h"
#include "verible/common/util/interval-set.h"
#include "verible/verilog/formatting/format-style-init.h"
#include "verible/verilog/formatting/formatter.h"

// 官方把这些 flag 定义在 verilog-format.cc（一个 main()，链接不进来），
// 所以在本工具里重新声明，**默认值必须与官方逐项一致**。
// 其余的 style flag（--column_limit / --indentation_spaces / ...）定义在
// basic-format-style-init.cc / format-style-init.cc 里，这些 .cc 我们直接链接，
// 所以默认值天然相同，不需要重复声明。
ABSL_FLAG(int, max_search_states, 100000,
          "Limits the number of search states explored during line wrap "
          "optimization.");
ABSL_FLAG(bool, verify_convergence, true,
          "If true, and not incrementally formatting with --lines, verify that "
          "re-formatting the formatted output yields no further changes, i.e. "
          "formatting is convergent.");
ABSL_FLAG(int, show_largest_token_partitions, 0,
          "If > 0, print token partitioning and then exit without formatting "
          "output.");
ABSL_FLAG(bool, show_token_partition_tree, false,
          "If true, print diagnostics after token partitioning and then exit "
          "without formatting output.");
ABSL_FLAG(bool, show_inter_token_info, false,
          "If true, along with show_token_partition_tree, include inter-token "
          "information such as spacing and break penalties.");
ABSL_FLAG(bool, show_equally_optimal_wrappings, false,
          "If true, print when multiple optimal solutions are found (stderr), "
          "but continue to operate normally.");
// ★ 坑 3：官方 --failsafe_success 默认 **true** —— 即使输入有语法错误也返回 0。
//    我第一版在失败时返回 1，结果与官方 exit code 不一致（stdout 相同、退出码不同）。
//    官方 verilog-format.cc 的失败路径是 `return absl::GetFlag(FLAGS_failsafe_success);`
ABSL_FLAG(bool, failsafe_success, true,
          "If true, always exit with 0 status, even if there were input errors "
          "or internal errors. In all error conditions, the original text is "
          "always preserved.");

// ---- 局部（选区）格式化 ------------------------------------------------------
// 对应官方 --lines N-M。两个都 >0 且 range_start <= range_end 时走
// FormatVerilogRange（只格式化该行区间，**不做收敛性检验**）。
// 留 0 则走全文档路径（已通过 102/102 门禁的基线）。
ABSL_FLAG(int, range_start, 0,
          "Selection formatting: first line to format (1-based, inclusive). "
          "0 = format the whole file.");
ABSL_FLAG(int, range_end, 0,
          "Selection formatting: last line to format (1-based, inclusive). "
          "0 = format the whole file.");

int main(int argc, char** argv) {
#if defined(_WIN32)
  // ★ 坑 1：必须关掉 CRT 的换行转换，否则输出的每个 "\n" 都会变成 "\r\n"。
  // 官方 verilog-format.cc 的 main() 第一件事就是这一句。
  _setmode(_fileno(stdout), _O_BINARY);
#endif

  const auto usage = absl::StrCat("usage: ", argv[0], " [options] <file>\n");

  // 与官方一样走 InitCommandLine（做 absl flag 解析 + 日志初始化）。
  // 返回的 vector **包含程序名**（file_args[0] == argv[0]），
  // 与官方 verilog-format.cc 的 `if (file_args.size() == 1)` → 打印 usage 一致。
  // 本工具定义/链接了全部官方 flag，因此不会遇到 lint_dump 那种
  // "未知 flag 被 absl 拒绝" 的问题。
  const std::vector<std::string_view> file_args =
      verible::InitCommandLine(usage, &argc, &argv);

  if (file_args.size() < 2) {
    std::cerr << usage;
    return 2;
  }
  if (file_args.size() > 2) {
    std::cerr << "format_dump: 只支持一个输入文件（收到 "
              << (file_args.size() - 1) << " 个）\n";
    return 2;
  }

  const std::string_view filename = file_args[1];

  // 与官方同一个读取函数（保证换行/编码处理一致）。
  const absl::StatusOr<std::string> content_or =
      verible::file::GetContentAsString(filename);
  if (!content_or.ok()) {
    std::cerr << content_or.status().message() << std::endl;
    return 2;
  }

  // ---- style：官方同一个入口，默认值逐字段相同 --------------------------------
  verilog::formatter::FormatStyle format_style;
  verilog::formatter::InitializeFromFlags(&format_style);

  // ---- ExecutionControl：与官方逐项对齐 ---------------------------------------
  verilog::formatter::ExecutionControl control;
  // 诊断走 **stdout**（与官方一致，不是 stderr）。
  // 门禁比的是 stdout 字节，通道必须一致，否则出错场景下两边不可比。
  control.stream = &std::cout;
  control.show_largest_token_partitions =
      absl::GetFlag(FLAGS_show_largest_token_partitions);
  control.show_token_partition_tree =
      absl::GetFlag(FLAGS_show_token_partition_tree);
  control.show_inter_token_info = absl::GetFlag(FLAGS_show_inter_token_info);
  control.show_equally_optimal_wrappings =
      absl::GetFlag(FLAGS_show_equally_optimal_wrappings);
  control.max_search_states = absl::GetFlag(FLAGS_max_search_states);
  control.verify_convergence = absl::GetFlag(FLAGS_verify_convergence);

  // ---- 行区间：默认空集合 = 全部行启用格式化 --------------------------------
  //
  // 对应官方 --lines N-M 的语义（**注意不是 FormatVerilogRange**）：
  //   官方 verilog-format.cc 把 --lines 用
  //     verible::ParseInclusiveRanges(&lines_to_format, ...)
  //   解析成 LineNumberSet，再传给
  //     FormatVerilog(text, name, style, stream, lines_to_format, control)
  //   输出仍然是**整个文件**，只是"仅这些行参与格式化决策"。
  //
  // ★ 这里必须调用**同一个** ParseInclusiveRanges，不能自己手工 Add(Interval)。
  //   我第一版就是手工 `Add(Interval(range_start - 1, range_end))`，结果
  //   单行区间（如 `--lines 3-3`）下我们的输出**完全不格式化了**，而官方正常：
  //     输入   `    a <= b; // [Style: ...]`
  //     官方   `    a <= b;  // [Style: ...]`   ← 规范化了空格
  //     我方   `    a <= b; // [Style: ...]`    ← 原样不动
  //   全语料 194 个区间用例里 58 个不一致（一致率只有 70.10%）。
  //   原因就是 ParseInclusiveRanges 里的 `ParseInclusiveRange` 对闭区间
  //   [N, M] → 半开区间的换算与"1 基 → 0 基"的约定，和我手写的不一致。
  //   **凡是上游提供了"解析这个参数"的函数，就一定要用它**——自己重新推导
  //   边界语义是这类 off-by-one 陷阱的温床。
  verible::LineNumberSet lines_to_format{};
  {
    const int range_start = absl::GetFlag(FLAGS_range_start);
    const int range_end = absl::GetFlag(FLAGS_range_end);
    if (range_start > 0 && range_end > 0) {
      if (range_start > range_end) {
        std::cerr << "format_dump: --range_start=" << range_start
                  << " 大于 --range_end=" << range_end << "\n";
        return 2;
      }
      // 与官方 --lines 完全同形："N-M"
      std::vector<std::string> line_ranges;
      line_ranges.push_back(std::to_string(range_start) + "-" +
                            std::to_string(range_end));
      if (!verible::ParseInclusiveRanges(&lines_to_format,
                                         line_ranges.begin(),
                                         line_ranges.end(), &std::cerr, '-')) {
        std::cerr << "format_dump: --range 解析失败\n";
        return 2;
      }
    } else if (range_start != 0 || range_end != 0) {
      std::cerr << "format_dump: --range_start 与 --range_end 必须同时给出"
                   "（0 表示格式化整个文件）\n";
      return 2;
    }
  }

  std::ostringstream stream;
  const absl::Status format_status = verilog::formatter::FormatVerilog(
      *content_or, filename, format_style, stream, lines_to_format, control);

  const std::string& formatted_output = stream.str();

  if (!format_status.ok()) {
    // 与官方一致的三件事：
    //   1. 失败时把**原文**写到 stdout（failsafe：原文永不丢失）
    //   2. 诊断已随 control.stream 进了 stdout
    //   3. 退出码 = --failsafe_success（默认 true → 0）
    std::cout << *content_or;
    std::cout.flush();
    return absl::GetFlag(FLAGS_failsafe_success) ? 0 : 1;
  }

  std::cout << formatted_output;
  std::cout.flush();
  return 0;
}
