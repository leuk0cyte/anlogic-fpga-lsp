// =============================================================================
//  src/flow/strategy.h —— 多策略对比（同一设计跑多个 TD 策略，比 QoR）
// =============================================================================
//  对应参考实现：Y:\fpga-lsp\fpgalsp\strategy.py
//
//  在工程的**临时副本**上跑（`.fpgalsp/compare/<stamp>/prj`），用户的 .al 不会被塞进
//  一堆 `phy_cmp_N` run。策略名来自 TD 自己的
//      <td_root>/doc/strategies/official_strategy_{cpld,ph1,ph2}.json
//  所以 UI 里给出的名字一定是 TD 认识的；传错名字直接返回可读错误而不是让 TD 报错。
// =============================================================================

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "flow/flow_runner.h"
#include "jsonrpc/json_util.h"
#include "project/al_project.h"
#include "td/td_install.h"

namespace fpgalsp::flow {

// 对应 Python DEFAULT_STRATEGIES（按优先级）
const std::vector<std::string>& DefaultStrategies();

// 器件族 -> 策略文件名（PH1/PH1P -> ph1，PH2/PH2A -> ph2，其余 -> cpld）
std::string StrategyFileForFamily(std::string_view family);

// 读 <td_root>/doc/strategies/<file> 的 JSON，返回排序后的键。
// 文件缺失/不是 JSON -> 空表（不抛）。
std::vector<std::string> AvailableStrategies(std::string_view td_root, std::string_view family);

// 从 QoR 报告的 metrics 里摘出可比较的数字（对应 Python _qor_numbers）
struct QorNumbers {
    std::optional<int64_t> setup_wns_ps;
    std::optional<int64_t> setup_tns_ps;
    std::optional<int64_t> hold_wns_ps;
    std::optional<int64_t> luts;       // 优先 "#luts"，否则 "#lut"（EF2 vs PH1 的列名差异）
    std::optional<int64_t> registers;
    std::optional<int64_t> slices;
    std::string slice_pct;             // "61.52%"（注意 Python 返回的是带 % 的原串）

    jsonrpc::Json ToJson() const;
};

QorNumbers QorNumbersFromReport(const jsonrpc::Json& qor);

// 决定实际要跑哪些策略（对应 Python compare 开头那段）：
//   * 请求为空 -> 取 DEFAULT_STRATEGIES 里 available 的；都没有 -> available 前两个
//   * 请求里有 TD 不提供的名字 -> unknown 非空，调用方应直接报错
std::vector<std::string> ChooseStrategies(const std::vector<std::string>& requested,
                                          const std::vector<std::string>& available,
                                          std::vector<std::string>* unknown);

// .al 里的约束集名（对应 Python _constraint_set）
std::string ConstraintSetOf(std::string_view al_path);

struct CompareOptions {
    int jobs = 2;
    double timeout_seconds = 3600.0;
    double command_timeout_seconds = 300.0;
    std::string work_root;  // 空 = <project>/.fpgalsp/compare
    std::string explicit_db;
    FlowSessionFactory session_factory;  // 空 = 真实 TD
    int keep_work_dirs = 5;              // 只保留最近 N 次（对应 Python _prune(keep=5)）
};

// 跑一次策略对比。never-throw。
// 返回 {ok, strategies, available, runs[], best?, work, elapsed, generatedAt, error?}
jsonrpc::Json CompareStrategies(const project::Project& project, const td::TdChoice& td,
                                const std::vector<std::string>& requested,
                                const CompareOptions& options = {});

}  // namespace fpgalsp::flow