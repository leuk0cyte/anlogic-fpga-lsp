// =============================================================================
//  cst_dump —— Verible 解析行为导出工具（阶段 0 门禁用）
// =============================================================================
//  用途：把 vendored Verible（CMake 化之后）的解析结果 dump 成文本，与官方
//        发行的 verible-verilog-syntax.exe 做逐字节对比，证明「解析行为一致」。
//
//  设计原则（重要）：
//    **不是「参考官方格式自己重写一份」，而是尽量调用和官方工具完全相同的
//    代码路径和输出函数**，这样对比结果才反映真实的移植差异，而不是我这边
//    reimplement 带来的差异。
//
//    与 verible/verilog/tools/syntax/verilog-syntax.cc 的对应关系：
//      * 读文件      -> verible::file::GetContentAsMemBlock()      [完全相同]
//      * 解析        -> VerilogAnalyzer::AnalyzeAutomaticMode()    [完全相同]
//      * 预处理配置  -> VerilogPreprocess::Config{.filter_branches=true}
//                                                                  [完全相同]
//      * token 打印  -> TokenInfo::ToStream(stream, context)       [完全相同]
//        context 的翻译器用 verilog::verilog_symbol_name()         [完全相同]
//      * 错误信息    -> FileAnalyzer::LinterTokenErrorMessages()   [完全相同]
//      * 语法树打印  -> verilog::PrettyPrintVerilogTree()          [完全相同]
//    因此 --rawtokens 的输出应当与
//        verible-verilog-syntax --printrawtokens <file>
//    的 stdout **逐字节相同**（含 "All lexed tokens:" 标题和空行前缀）。
//    任何不一致都只可能来自：CMake 化编译产生的行为变化、第三方库版本差异、
//    或生成器（flex/bison）版本差异 —— 这正是阶段 0 门禁要抓的东西。
//
//  注意：--tokens 用的是 filtered token（GetTokenStreamView()），对应官方的
//  --printtokens，不是 --printrawtokens；两者语义不同，别混用。
//
//  命令行：
//    cst_dump [--rawtokens] [--tokens] [--tree] [--json] [--errors] <file>...
//    cst_dump --tokens <file>        # filtered token（对齐 --printtokens）
//    cst_dump --rawtokens <file>     # 所有原始 token（对齐 --printrawtokens）
//
//  退出码：0 = 全部文件词法+语法都 OK；1 = 至少一个文件存在词法/语法错误
//          （注意：**即使有语法错误也照常输出已识别的 token**，这样门禁可以
//           覆盖错误恢复路径的行为一致性）；2 = 用法/IO 错误。
// =============================================================================

#include <cstdio>
#include <format>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "verible/common/analysis/file-analyzer.h"
#include "verible/common/strings/mem-block.h"
#include "verible/common/text/concrete-syntax-tree.h"
#include "verible/common/text/syntax-tree-context.h"
#include "verible/common/text/token-info.h"
#include "verible/common/text/token-stream-view.h"
#include "verible/common/util/file-util.h"
#include "verible/verilog/CST/verilog-tree-print.h"
#include "verible/verilog/analysis/verilog-analyzer.h"
#include "verible/verilog/parser/verilog-parser.h"
#include "verible/verilog/parser/verilog-token.h"
#include "verible/verilog/preprocessor/verilog-preprocess.h"

namespace {

using verible::MemBlock;
using verible::TokenInfo;
using verible::TokenSequence;
using verible::TokenStreamView;
using verilog::VerilogAnalyzer;
using verilog::VerilogPreprocess;

// -----------------------------------------------------------------------------
// 与官方 verilog-syntax.cc 完全一致的 token 枚举 -> 名字 翻译器
// -----------------------------------------------------------------------------
void PrintTokenEnumName(std::ostream &stream, int e) {
  stream << verilog::verilog_symbol_name(e);
}

// -----------------------------------------------------------------------------
// JSON 字符串转义（最小实现，够门禁对比用）
// -----------------------------------------------------------------------------
void JsonEscape(std::ostream &out, std::string_view text) {
  out << '"';
  for (const char ch : text) {
    const auto uc = static_cast<unsigned char>(ch);
    switch (ch) {
      case '"':  out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      default:
        if (uc < 0x20) {
          // 其它控制字符用 \u00XX（与原来 snprintf("\\u%04x") 逐字节一致）
          out << std::format("\\u{:04x}", uc);
        } else {
          out << ch;
        }
    }
  }
  out << '"';
}

// -----------------------------------------------------------------------------
// 打印一条 token（与官方 --printrawtokens / --printtokens 逐字节一致的格式）
// -----------------------------------------------------------------------------
void PrintTokenLine(std::ostream &out, const TokenInfo &token,
                    const TokenInfo::Context &context) {
  token.ToStream(out, context) << '\n';
}

// -----------------------------------------------------------------------------
// 单文件处理参数
// -----------------------------------------------------------------------------
struct DumpOptions {
  bool raw_tokens = false;
  bool filtered_tokens = false;
  bool tree = false;
  bool json = false;
  bool errors = false;
};

// 打印错误信息（与官方一致：走 LinterTokenErrorMessages）。
// 官方在 --export_json 时只把错误塞进 JSON，不打印到 stdout；这里保持同样语义。
void EmitMessages(std::ostream &out, const VerilogAnalyzer &analyzer,
                  const DumpOptions &opts) {
  const auto lex_status = analyzer.LexStatus();
  const auto parse_status = analyzer.ParseStatus();
  if (lex_status.ok() && parse_status.ok()) return;

  const std::vector<std::string> messages =
      analyzer.LinterTokenErrorMessages(false /* diagnostic_context */);
  if (opts.json) {
    // 官方把错误放到 JSON 的 "errors" 字段，这里在 JSON 模式下交给调用方，
    // 不直接写 stdout（避免破坏 JSON 结构）。见 DumpJson()。
    return;
  }
  for (const auto &message : messages) {
    out << message << '\n';
  }
}

// -----------------------------------------------------------------------------
// JSON 输出
// -----------------------------------------------------------------------------
// JSON 里的 token 类型名与官方 --export_json 对齐：用 verilog::TokenTypeToString()，
// **不是** verilog_symbol_name()。两者对空白/注释类 token 结果不同：
//   verilog_symbol_name(TK_SPACE)  -> "<<space>>"    （文本模式下用这个）
//   TokenTypeToString(TK_SPACE)    -> "TK_SPACE"     （JSON 模式下用这个，与官方一致）
// TokenTypeToString 已经做了去引号处理，直接返回裸名。
std::string TokenKindToString(int token_enum) {
  return std::string(verilog::TokenTypeToString(
      static_cast<size_t>(token_enum)));
}

void EmitJsonArray(std::ostream &out, const TokenSequence &tokens,
                   const TokenInfo::Context &context, std::string_view key,
                   bool first_field) {
  if (!first_field) out << ",\n";
  out << "  \"";
  out << key;
  out << "\": [\n";
  std::string_view base = context.base;
  bool first = true;
  for (const auto &token : tokens) {
    if (!first) out << ",\n";
    first = false;
    // kind 必须做 JSON 转义：token 名可能含双引号（如 "\"module\""）或反斜杠
    // （如转义标识符），裸输出会产生非法 JSON。
    out << "    {\"kind\": ";
    JsonEscape(out, TokenKindToString(token.token_enum()));
    out << ", \"offset\": " << token.left(base) << ", \"length\": "
        << (token.right(base) - token.left(base)) << ", \"text\": ";
    JsonEscape(out, token.text());
    out << '}';
  }
  out << "\n  ]";
}

void EmitJsonErrors(std::ostream &out, const VerilogAnalyzer &analyzer,
                    std::string_view filename, bool first_field) {
  if (!first_field) out << ",\n";
  (void)filename;
  out << "  \"errors\": [";
  const auto lex_status = analyzer.LexStatus();
  const auto parse_status = analyzer.ParseStatus();
  if (lex_status.ok() && parse_status.ok()) {
    out << "]\n";
    return;
  }
  out << '\n';
  // 复用官方错误消息文本，从中提取 offset 不可靠，故同时给出原文。
  const auto messages = analyzer.LinterTokenErrorMessages(false);
  bool first = true;
  for (const auto &message : messages) {
    if (!first) out << ",\n";
    first = false;
    out << "    {\"message\": ";
    JsonEscape(out, message);
    out << '}';
  }
  out << "\n  ]\n";
}

// -----------------------------------------------------------------------------
// 处理一个文件；返回 true 表示词法+语法都 OK
// -----------------------------------------------------------------------------
bool DumpOneFile(const std::shared_ptr<MemBlock> &content,
                 std::string_view filename, const DumpOptions &opts) {
  // 与官方 verilog-syntax.cc 完全一致的预处理配置
  const VerilogPreprocess::Config preprocess_config{
      .filter_branches = true,
  };

  // 与官方一致：AnalyzeAutomaticMode 会先看 "// verilog_syntax:" 指令，
  // 然后走 Analyze()。这条路径是官方 --printrawtokens 的同一条。
  const std::unique_ptr<VerilogAnalyzer> analyzer =
      VerilogAnalyzer::AnalyzeAutomaticMode(content, filename,
                                            preprocess_config);
  if (analyzer == nullptr) {
    std::cerr << filename << ": AnalyzeAutomaticMode returned null"
              << std::endl;
    return false;
  }

  const bool ok = analyzer->LexStatus().ok() && analyzer->ParseStatus().ok();

  // TokenInfo::Context 需要 contents 的 base 才能算字节偏移。
  // 翻译器与官方 verilog-syntax.cc 中完全一致：verilog::verilog_symbol_name()。
  const verible::TokenInfo::Context context(
      analyzer->Data().Contents(), [](std::ostream &stream, int e) {
        stream << verilog::verilog_symbol_name(e);
      });

  if (opts.json) {
    std::cout << "{\n";
    bool need_comma = false;
    if (opts.raw_tokens) {
      EmitJsonArray(std::cout, analyzer->Data().TokenStream(), context,
                    "rawtokens", !need_comma);
      need_comma = true;
    }
    if (opts.filtered_tokens) {
      // GetTokenStreamView() 是 vector<TokenSequence::const_iterator>，
      // 解引用得到 TokenInfo，拷贝成 TokenSequence 以便复用 EmitJsonArray。
      TokenSequence filtered;
      for (const auto it : analyzer->Data().GetTokenStreamView()) {
        filtered.push_back(*it);
      }
      EmitJsonArray(std::cout, filtered, context, "tokens", !need_comma);
      need_comma = true;
    }
    EmitJsonErrors(std::cout, *analyzer, filename, !need_comma);
    std::cout << "}\n";
    return ok;
  }

  // ---- 非 JSON：先打错误（官方也是先错误后 token） ----
  // 与官方一致：只要 lex/parse 失败就无条件打印错误消息，即使 --printrawtokens
  // 也一样。这与门禁的"token 对比"不冲突，因为对比脚本只取 token 行。
  EmitMessages(std::cout, *analyzer, opts);

  if (opts.raw_tokens) {
    // 与官方 verilog-syntax.cc 逐字节一致：先一个空行，再标题
    std::cout << "\nAll lexed tokens:\n";
    for (const auto &token : analyzer->Data().TokenStream()) {
      PrintTokenLine(std::cout, token, context);
    }
  }

  if (opts.filtered_tokens) {
    std::cout << "\nLexed and filtered tokens:\n";
    for (const auto it : analyzer->Data().GetTokenStreamView()) {
      PrintTokenLine(std::cout, *it, context);
    }
  }

  if (opts.tree) {
    const auto &syntax_tree = analyzer->Data().SyntaxTree();
    if (syntax_tree != nullptr) {
      const bool parse_ok = analyzer->ParseStatus().ok();
      std::cout << "\nParse Tree"
                << (parse_ok ? ":" : " (incomplete due to syntax errors):")
                << '\n';
      verilog::PrettyPrintVerilogTree(*syntax_tree, analyzer->Data().Contents(),
                                      &std::cout);
    }
  }

  return ok;
}

void PrintUsage(const char *prog) {
  std::cerr
      << "usage: " << prog
      << " [--rawtokens] [--tokens] [--tree] [--json] [--errors] <file>...\n"
      << "\n"
      << "  --rawtokens  print every lexed token (same as verible-verilog-syntax"
         " --printrawtokens)\n"
      << "  --tokens     print lexed+filtered tokens (same as --printtokens)\n"
      << "  --tree       print the concrete syntax tree (same as --printtree)\n"
      << "  --json       emit JSON instead of text\n"
      << "  --errors     (no-op, errors are always printed on text output)\n"
      << "\n"
      << "  exit status: 0 all files lex+parse OK; 1 at least one file has\n"
      << "  lexer/parser errors; 2 usage or I/O error.\n";
}

}  // namespace

int main(int argc, char **argv) {
  DumpOptions opts;
  std::vector<std::string> files;
  bool saw_mode_flag = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--rawtokens" || arg == "--printrawtokens") {
      opts.raw_tokens = true;
      saw_mode_flag = true;
    } else if (arg == "--tokens" || arg == "--printtokens") {
      opts.filtered_tokens = true;
      saw_mode_flag = true;
    } else if (arg == "--tree" || arg == "--printtree") {
      opts.tree = true;
      saw_mode_flag = true;
    } else if (arg == "--json") {
      opts.json = true;
    } else if (arg == "--errors") {
      opts.errors = true;
    } else if (arg == "--help" || arg == "-h") {
      PrintUsage(argv[0]);
      return 0;
    } else if (!arg.empty() && arg.front() == '-') {
      std::cerr << argv[0] << ": unknown option: " << arg << '\n';
      PrintUsage(argv[0]);
      return 2;
    } else {
      files.emplace_back(arg);
    }
  }

  if (files.empty()) {
    PrintUsage(argv[0]);
    return 2;
  }

  // 没有任何模式 flag 时，默认打印 raw tokens —— 这是门禁最常用的输出，
  // 对应官方 --printrawtokens。
  if (!saw_mode_flag && !opts.json) {
    opts.raw_tokens = true;
  }

  int exit_status = 0;
  for (const std::string &filename : files) {
    // 与官方一致：GetContentAsMemBlock（Windows 上是 fopen("rb") 全量读入，
    // 不做换行转换；这样 CRLF/CR 的 token 边界和官方完全相同）。
    auto content_status = verible::file::GetContentAsMemBlock(filename);
    if (!content_status.status().ok()) {
      std::cerr << filename << ": " << content_status.status().message()
                << '\n';
      exit_status = 2;
      continue;
    }
    std::shared_ptr<MemBlock> content = std::move(*content_status);

    const bool ok = DumpOneFile(content, filename, opts);
    if (!ok) exit_status = 1;
  }

  return exit_status;
}