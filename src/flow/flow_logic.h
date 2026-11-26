// =============================================================================
//  src/flow/flow_logic.h —— Flow 引擎里**不碰进程、不碰线程**的判定逻辑
// =============================================================================
//  对应参考实现：%PYREF%\fpgalsp\flow.py
//      STAGE_NAMES / PHASE_TOKENS / _normalise_stages / _run_started / _run_completed /
//      _only_run_exit_errors / _error_lines / _summary / heartbeat 的 stall 判定
//
//  单独拆出来的理由：
//   * 这些判定是 flow 里最容易出错、也最需要回归保护的部分（尤其 PRJ-8806 容错）；
//   * 它们都是纯函数，可以在毫秒级单测里覆盖"三种组合"，不需要真实 TD 进程；
//   * 真正驱动进程的 FlowRunner 只是把它们串起来 —— 逻辑与 I/O 分离后，
//     "为什么这个 run 判成功/失败"可以逐条对着 Python 讲清楚。
// =============================================================================

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "jsonrpc/json_util.h"

namespace fpgalsp::flow {

// -----------------------------------------------------------------------------
// 阶段（stage）
// -----------------------------------------------------------------------------

// 对应 Python STAGE_NAMES
std::string StageLabel(std::string_view stage);

// 对应 Python _normalise_stages：把 UI 传来的各种别名收敛成 ["syn"] / ["phy"] / ["route"]。
//   * "all"                                  -> {"syn","phy"}
//   * "route"/"reroute"/"route_only"/"incremental" -> {"route"}（立即返回，忽略其余）
//   * "syn"/"synthesis"/"synth"              -> syn
//   * "phy"/"pnr"/"place_route"              -> phy
// 保持首次出现顺序并去重。空结果表示"没有可识别的阶段"。
std::vector<std::string> NormaliseStages(const std::vector<std::string>& stages);

// -----------------------------------------------------------------------------
// 阶段进度识别（heartbeat 用）
// -----------------------------------------------------------------------------

// 对应 Python PHASE_TOKENS，**顺序敏感**（同一行命中多个 token 时取表里靠前的）。
// 实测 TD 2026.2 会打 opt_rtl 与 optimize_rtl 两种写法，两种都要认。
struct PhaseToken {
    std::string_view token;
    std::string label;
};

const std::vector<PhaseToken>& PhaseTokens();

// 单行 -> 阶段标签；没有命中返回空串。
std::string MatchPhaseToken(std::string_view line);

struct PhaseDetection {
    std::string phase;    // "Optimize RTL" / "Route" / ...，可能为空
    std::string message;  // 最后一条非空、非 "===" 开头的行（截到 160 字符）

    [[nodiscard]] bool Empty() const { return phase.empty() && message.empty(); }
};

// 对应 Python _heartbeat 里从 log 尾部推导进度的那两段循环：
//   1. 从后往前找第一行能命中 PHASE_TOKENS 的，得到 phase
//   2. 从后往前找第一行非空且不以 "===" 开头的，得到 message
PhaseDetection DetectProgress(const std::vector<std::string>& tail_lines);

// 没有 run.log 时，用 TD 写下的步骤标记推导已完成步骤（Python 的 else 分支）。
// 返回 {"read_design","opt_rtl",...}（按 PHASE_TOKENS 顺序）。
std::vector<std::string> CompletedStepsFromMarkers(const std::vector<std::string>& run_dir_names);

// -----------------------------------------------------------------------------
// run 目录状态
// -----------------------------------------------------------------------------

// 对应 Python _run_started：出现任一 .begin.f/.end.f，或存在 run.log
bool RunStarted(const std::vector<std::string>& run_dir_names);

// 对应 Python _run_completed：终态成功标记在、且**没有**任何 .error.f
//   syn_*  : .opt_gate.end.f 存在 且 有 *_gate.db
//   phy_*  : .bitgen.end.f  存在 且 有 <name>.bit 或任一 *.bit
//   其它   : 任一 *.end.f
bool RunCompleted(std::string_view run_name, const std::vector<std::string>& run_dir_names,
                  bool project_bit_exists, std::string_view project_name);

// 该目录里的步骤错误标记（对应 Python 的 .error.f 扫描）
std::vector<std::string> ErrorMarkers(const std::vector<std::string>& run_dir_names);

// -----------------------------------------------------------------------------
// PRJ-8806 / PRJ-5303 容错（本模块最关键的一条）
// -----------------------------------------------------------------------------

// 文本里所有含 "ERROR" 的行是否**只**含 PRJ-8806 / PRJ-5303。
bool OnlyToleratedPrjErrors(std::string_view text);

// 这个错误码是否属于"TD 在产物完整时仍会报"的那一类
bool IsToleratedPrjCode(std::string_view text);

// 含 ERROR / CRITICAL-WARNING 的报错行（对应 Python _error_lines），用 " / " 连接
std::string ErrorLines(std::string_view text, int limit = 3);

// 第一条报错行（对应 Python strategy._first_error）
std::string FirstErrorLine(std::string_view text);

// 一次 run 的成败结论。
struct RunOutcome {
    // true = 接受为成功。failure_detail 为空。
    bool accepted = false;
    // 是否需要记一条"产物完整、只是 TD 报了 PRJ-xxxx"的说明日志
    bool tolerated_prj_error = false;
    // 产物完整、但 TD 只打了 CRITICAL-WARNING（没有任何 ERROR）时，这里带上前几条
    // 警告原文：接受为成功，但必须留痕，避免"修好判定却漏报告警"。
    // 见 flow_logic.cpp 的 DecideRunOutcome 条件 1b（TD 2026.2_SP1 起的行为差异）。
    std::string warnings_only;
    // 失败原因（accepted=false 时由调用方抛/记录）
    std::string failure_detail;
};

// 判定一次 run 需要知道的全部现场信息
struct RunSnapshot {
    std::string run_name;                 // "syn_1" / "phy_1"
    std::vector<std::string> run_dir_names;  // run 目录下的文件名（不含路径）
    bool project_bit_exists = false;       // <run_dir>/<project>.bit 是否存在
    std::string project_name;
    std::string run_dir;                   // 仅用于失败详情兜底
    std::string run_log_tail;              // 已读到的 run.log 报错行（可空）
    std::string missing_run_log;           // TD 报告过但文件不存在的 run.log（可空）
};

// ⚠ 核心判定，逐条复现 Python 0.10.29 的三条件（**不要放宽**）：
//   1) out 里确实有错误（否则直接成功）；
//   2) 所有 ERROR 行都是 PRJ-8806 / PRJ-5303；
//   3) run 目录里有完成标记或 .bit，且**没有** .error.f（由 RunCompleted 一并判定）。
// 三条全满足才判成功；否则失败，并给出 failure_detail。
RunOutcome DecideRunOutcome(const RunSnapshot& snapshot, std::string_view out);

// 把 DecideRunOutcome 的 failure_detail 组装得和 Python 一致：
//   "<error lines> | failed run step: a, b | run.log: ..."
std::string BuildFailureDetail(std::string_view out, const std::vector<std::string>& error_markers,
                               std::string_view run_log_tail, std::string_view run_dir);

// -----------------------------------------------------------------------------
// stall 看门狗
// -----------------------------------------------------------------------------

// 对应 Python heartbeat 里的两段判定。now/activity 都是 epoch 秒（0 表示"什么都还没有"）。
// 返回空串表示"没卡"。
//   * 完全没有活动痕迹且已经等了超过 120 s -> 子进程可能根本没起来（典型 PRJ-8001）
//   * 有活动但超过 stall_seconds 没有新文件    -> 卡死/子进程死了
std::string StallReason(double activity_mtime, double now, double stage_started,
                        double stall_seconds);

// -----------------------------------------------------------------------------
// 日志与摘要
// -----------------------------------------------------------------------------

// flowStatus 只保留最后 limit 行（对应 Python state["log"][-200:]）
std::vector<std::string> TrimLog(std::vector<std::string> log, std::size_t limit = 200);

// 日志行格式："  12.3s [syn] message"（对应 Python "%6.1fs [%s] %s"）
std::string FormatLogLine(double elapsed, std::string_view stage, std::string_view message);

// 对应 Python _summary：从 reports.collect 的结果里摘一行人类可读摘要。
//   "flow complete: Setup WNS 171ps, Hold WNS 23ps, SWNS 0.171ns, LUT 6029/19600, bit 614 KB"
std::string SummaryFromReports(const jsonrpc::Json& reports);

// 对应 Python _reported_run_log：从 launch_runs 的输出里抓 TD 报告的 run.log 路径
std::string ReportedRunLogPath(std::string_view text);

// 把 run 目录名列表里的步骤标记清理成 "opt_gate" 这样的步骤名
std::vector<std::string> ErrorMarkerSteps(const std::vector<std::string>& run_dir_names);

// -----------------------------------------------------------------------------
// .al 根节点 Path 改写（PYTHON-BUGS P-7b）
// -----------------------------------------------------------------------------
//  ⚠ 任何"复制工程再跑 TD"的路径都**必须**先调用这个：
//    TD open_project 之后拿 .al 根节点的 Path 当**工程根**，会把 *_Runs / .fpgalsp
//    写到那个目录。若 Path 指向另一个真实工程，就等于写坏用户数据。
//    （实测见 tests/gate-corpus/corpus_alpha/demo_project.al：Path="C:/work/CorpusAlpha"）
//
//  只改根节点那一个 Path="..."，其余内容原样保留。成功返回 true。
bool RewriteProjectPath(const std::string& al_path, const std::string& new_root);


}  // namespace fpgalsp::flow
