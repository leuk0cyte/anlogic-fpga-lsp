// =============================================================================
//  src/td/td_diagnostics.h —— TD 消息 → LSP Diagnostic 映射
// =============================================================================
//  对应参考实现：Y:\fpga-lsp\fpgalsp\td.py 的
//      _resolve_file / _diag / run_read_hdl / ip_generated_hdl / _file_list_tcl
//  以及 server.py 的 _sanitize / run_read_hdl 调用点。
//
//  ⚠ 硬约束（见 PYTHON-PARITY.md §5.2）
//      read_hdl 会让 TD 关闭 stdout 管道，**必须是会话的最后一条命令**。
//      任何在同一会话里 read_hdl 之后继续发命令的代码路径都是 bug。
//
//  ⚠ never-raise（CONTRACT.md 完整性规则）
//      路径解析、行号转换、文件查找任何一步失败都**必须产生一条诊断**，
//      而不是抛异常或静默返回空。本模块所有公开函数都不抛。
// =============================================================================

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lsp/diagnostic.h"
#include "project/al_project.h"
#include "td/td_install.h"
#include "td/td_output.h"

namespace fpgalsp::td {

// -----------------------------------------------------------------------------
// 路径解析（对应 Python _resolve_file）
// -----------------------------------------------------------------------------

// TD 报出的路径 -> 用于放置诊断的绝对路径。
//   * path 为空            -> project.al_path（整文件级）
//   * "X:/..." 绝对路径    -> 直接返回（反斜杠改成正斜杠）
//   * 相对路径             -> 依次尝试 project.dir、project.declared_path，
//                             **命中 isfile 的那个优先**；都不存在时回落到
//                            project.dir 拼接结果（复现 Python 的行为）
//   * project.dir 也为空   -> 返回清理后的相对路径本身
// never-raise。
std::string ResolveTdFile(const std::optional<std::string>& path,
                          const project::Project& project);

// -----------------------------------------------------------------------------
// 单条消息映射
// -----------------------------------------------------------------------------

// 把一条 TdMessage 映射成 Diagnostic（不含 LSP-PATH-SPACE，那一步单独做）。
//
// 映射规则（逐条复现 Python run_read_hdl）：
//   line      = (msg.line - 1) if msg.line else 0        # TD 1-based -> LSP 0-based
//   character = 0                                        # TD 无列号
//   end       = {line, 0} - {line, 1}                    # 1 列宽
//   severity  = msg.severity，但:
//                 - message 含 "black box" 且不含 "TD ignored this file"
//                   -> 升级为 2，并在消息尾部追加说明
//                 - 无 severity 的已由 ParseMessageLines 处理成 3(Info)
//   path      = ResolveTdFile(...)，但 code == "HDL-8007" 且无行号
//               -> project.al_path
//   source    = "fpga-lsp:td"
//   data      = {"backend": "read_hdl"}
// never-raise。
lsp::Diagnostic MapMessage(const TdMessage& message, const project::Project& project,
                           std::string_view backend = "read_hdl");

// 把一批 TD 消息映射成诊断列表。
lsp::DiagnosticList MapMessages(const std::vector<TdMessage>& messages,
                                const project::Project& project,
                                std::string_view backend = "read_hdl");

// -----------------------------------------------------------------------------
// 文件名含空格的特殊诊断（LSP-PATH-SPACE）
// -----------------------------------------------------------------------------

// 对应 Python run_read_hdl 末尾：
//   for f in skipped:  # skipped = 工程里路径含空格的 hdl 文件
//       diags.append(_diag(0,
//           "File name contains spaces and cannot be passed to TD's -file argument "
//           "(Tcl splits it): <rel>", "LSP-PATH-SPACE", 2, "fpga-lsp:td",
//           path=project.al_path))
//
// 这是**唯一一条 TD 不报错但我们主动提示**的诊断。
// 注意：错误码是 "LSP-PATH-SPACE"（不是 TD 的 XXX-1234 形态），
//       severity 固定 2，message 末尾带 f.rel（相对路径，不是绝对路径）。
lsp::DiagnosticList MakePathSpaceDiagnostics(const project::Project& project);

// 列出工程里因路径含空格而被跳过的 HDL 文件（对应 Python skipped）
std::vector<const project::AlFile*> SkippedSpaceHdlFiles(const project::Project& project);

// -----------------------------------------------------------------------------
// Tcl 命令行构造
// -----------------------------------------------------------------------------

// 对应 _file_list_tcl：跳过含空格的路径，反斜杠改成正斜杠，空格拼接
std::string FileListTcl(const std::vector<std::string>& paths);

// 对应 ip_generated_hdl：IP 以 .ipc 注册，TD 非工程流需要其生成的 HDL
// （找同 stem 的 .v/.sv/.vhd/.vhdl，且路径不含空格）
std::vector<std::string> IpGeneratedHdl(const project::Project& project);

// 构造 read_hdl 命令
std::string MakeReadHdlCommand(const std::vector<std::string>& files,
                               std::string_view top);

// -----------------------------------------------------------------------------
// 从 read_hdl 的输出文本直接产出诊断（不涉及进程，便于单测）
// -----------------------------------------------------------------------------

// 完整映射：ParseMessageLines + MapMessages + LSP-PATH-SPACE
// 这是 run_read_hdl 中"拿到输出之后"的全部逻辑，可离线测试。
lsp::DiagnosticList DiagnosticsFromReadHdlOutput(std::string_view output,
                                                 const project::Project& project);

// -----------------------------------------------------------------------------
// 入口：需要真实 TD 会话
// -----------------------------------------------------------------------------

class TdSession;

// ensure_device 的结果
struct EnsureDeviceResult {
    bool ok = false;
    std::string db;       // 成功时：实际使用的 db 文件名
    std::string error;    // 失败原因（不抛异常）
    int tried = 0;        // 尝试过的候选数
};

// 对应 Python ensure_device：
//   * 若 db 显式给出 -> 直接用它 import（不校验结果，复现 Python）
//   * 否则按 _db_candidates 顺序逐个 import，直到 !HasError
// 全部失败返回 ok=false（Python 抛 TdError，这里不抛）
EnsureDeviceResult EnsureDevice(const project::Project& project, TdSession& session,
                                std::string_view td_root,
                                std::string_view explicit_db = {});

// 执行一次完整的 read_hdl 并返回诊断。
//
// ⚠ 本函数执行后该 session 不可再用（read_hdl 关闭 stdout 管道）。
//
// never-raise：任何失败都以诊断形式返回，不抛异常。
lsp::DiagnosticList RunReadHdl(const project::Project& project, TdSession& session,
                               std::string_view explicit_db = {});

}  // namespace fpgalsp::td