// =============================================================================
//  src/analysis/analysis.h —— 流程后分析（DRC / CDC）+ 工程版本同步
// =============================================================================
//  对应参考实现：
//    * Y:\fpga-lsp\fpgalsp\analysis.py
//        newest_routed_db / cache_dir / cached / run / _first_error
//        以及 reports.py 的 parse_drc / parse_cdc
//    * Y:\fpga-lsp\fpgalsp\server.py::on_workspace_executeCommand
//        fpgalsp.runAnalysis  -> run(project, td_info)
//        fpgalsp.updateProject-> check_project_version + open_project -update
//
//  ---- 契约（与 Python 逐字段一致；见 doc/audit/2026-11-18/parity-lsp/raw.json）----
//  runAnalysis:
//    { ok, generatedAt, device,
//      drc: {supported, violations, rules:[{rule,severity,count}], details:[str],
//            elapsed?, clean?, reason?, note?},
//      cdc: {supported, rows:[[cell]], counts:{safe,unsafe,unknown,no_async_reg},
//            elapsed?, clean?, reason?},
//      files: {drc?: path, cdc?: path},
//      db?: "<…_pr.db>", error }
//    * 没有布线结果时：ok=false + error="No placed/routed result yet (run Place & Route / Full Flow first)"
//    * 器件不支持 CDC 时：cdc={supported:false, reason:"Device (X) does not support check_cdc reports"}
//  updateProject:
//    { ok, al, output }   output = TD 输出最后 30 行（保留空行）
//    失败（TD 报错）时 ok=false 但**保留 output**；起会话失败时 {ok:false, error}
//
//  ---- 行为约束 ----
//    * read_hdl 必须是会话最后一条命令（td_session.h 硬约束）；本模块**不调用** read_hdl，
//      所以一个会话里顺序执行 import_db / report_drc / check_cdc 是安全的。
//    * never-throw：所有公开函数失败时返回 `{ok:false, error}`（唯一例外：TdSession 构造
//      异常，也在 Run/UpdateProject 内部兜底）。
//    * 缓存写 <state_dir>/cache/analysis.json（<state_dir> = %TEMP%\fpgalsp\<工程名>-<hash8>；
// =============================================================================

#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "jsonrpc/json_util.h"
#include "project/al_project.h"

namespace fpgalsp::analysis {

// 与 reports/ 一致的写法：输出结构直接是 nlohmann::json
using jsonrpc::Json;

// 日志回调（对应 Python 的 log=callable）。默认空实现。
using LogFn = std::function<void(std::string_view)>;

// -----------------------------------------------------------------------------
// 路径 / 缓存（对应 Python newest_routed_db / cache_dir / cached）
// -----------------------------------------------------------------------------

// <project>_Runs/phy_1 下最新的 *_pr.db / *_route.db（按 mtime 降序）。
// 找不到返回空串（never-throw）。
std::string NewestRoutedDb(const project::Project& project);

// <state_dir>/cache
std::string CacheDir(const project::Project& project);

// 读 <project>/.fpgalsp/cache/analysis.json；不存在/损坏返回 nullopt。
std::optional<Json> Cached(const project::Project& project);

// -----------------------------------------------------------------------------
// 纯解析层（不触碰进程，便于单测）
// -----------------------------------------------------------------------------

// 对应 reports.parse_drc：
//   {violations, rules:[{rule,severity,count}], details:[…≤200 条，每条≤300 字符], clean}
Json ParseDrc(std::string_view text);

// 对应 reports.parse_cdc：
//   {rows:[[cell]…≤200 行], counts:{safe…}, clean}
Json ParseCdc(std::string_view text);

// 对应 Python analysis._first_error：第一行含 "ERROR" / "CRITICAL-WARNING" 的文本（trim 后）。
std::string FirstError(std::string_view text);

// 对应 analysis.py:105-107 的 CDC 分支判定与 reason 文案（集中在这里，便于单测钉死契约）
bool CdcUnsupportedByDevice(std::string_view output);          // output 含 "does not support"
std::string CdcUnsupportedReason(std::string_view device);     // "Device (X) does not support check_cdc reports"

// 对应 Python "\n".join(text.splitlines()[-count:])（保留空行，丢掉结尾换行产生的空串）。
std::string TailLines(std::string_view text, std::size_t count);

// -----------------------------------------------------------------------------
// 入口
// -----------------------------------------------------------------------------

// 对应 Python analysis.run(project, td_info)。
//   td_explicit 对应 --td（空 = 自动检测）。
//   真实 TD 只应在 %TEMP% 副本上调用（P-4/P-7b：会写 .fpgalsp/）。
Json Run(const project::Project& project, std::string_view td_explicit = {},
         const LogFn& log = {});

// 对应 Python fpgalsp.updateProject（check_project_version + open_project -update）。
//   会写 .al（TD 自己回写）→ 只应在副本上调用。
Json UpdateProject(const project::Project& project, std::string_view td_explicit = {},
                   const LogFn& log = {});

}  // namespace fpgalsp::analysis