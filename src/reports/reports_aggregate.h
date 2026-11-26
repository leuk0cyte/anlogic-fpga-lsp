// =============================================================================
//  src/reports/reports_aggregate.h —— fpgaLsp/reports 的汇总入口
// =============================================================================
//  对应参考实现：%PYREF%\fpgalsp\reports.py::collect()
//  协议：%PYREF%\PROTOCOL-V2.md §2 + v13 附录（分阶段 syn/phy）
//
//  输出骨架（结构见 BuildReports 的说明与 ToJson）：
//      {
//        ok, error?, generatedAt,
//        artifacts: {bit, syn_db, route_db, runs[]},
//        files:     {qor, timing, area, flow_status, bit},
//        qor?, timing?, area?, flowStatus?,
//        syn?: {ok, label, files, qor?, timing?, area?, flowStatus?},
//        phy?: {ok, label, files, qor?, timing?, area?, flowStatus?}
//      }
//
//  ⚠ flowStatus 有**两份**，语义不同（v13 的关键点）：
//    * 顶层 flowStatus.steps：syn 与 phy 两阶段的步骤**合并**，每步带 stage="syn"/"phy"；
//      另带 stages={syn:..., phy:...} 供前端分页展示。
//    * syn.flowStatus / phy.flowStatus：只有本阶段的步骤，且**不带** stage 标签。
//    这是 Python 版的实际行为（server 端的 webview 依赖它），不要"顺手统一"。
// =============================================================================

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "jsonrpc/json_util.h"
#include "project/al_project.h"
#include "reports/area.h"
#include "reports/artifacts.h"
#include "reports/flow_status.h"
#include "reports/qor.h"
#include "reports/timing.h"

namespace fpgalsp::reports {

// -----------------------------------------------------------------------------
// section 掩码（Phase 2 / A3）
// -----------------------------------------------------------------------------
//  fpgaLsp/reports 的 section 参数与 ReportStore 的懒构建都用这一套位。
//  语义（重要）：
//    * kQor / kTiming / kArea / kFlow 真正决定"要不要解析该段"（解析是热点）；
//    * kFiles / kArtifacts 是**廉价元数据**：`ok/error` 判定依赖 runs 目录，
//      且 ToJson 本来就会带 files/artifacts 两个键，所以这两位当前**不裁剪输出**
//      （保留是为了参数兼容与将来做"严格裁剪"）。
namespace sections {

inline constexpr std::uint32_t kNone = 0u;
inline constexpr std::uint32_t kFiles = 1u << 0;
inline constexpr std::uint32_t kArtifacts = 1u << 1;
inline constexpr std::uint32_t kQor = 1u << 2;
inline constexpr std::uint32_t kTiming = 1u << 3;
inline constexpr std::uint32_t kArea = 1u << 4;
inline constexpr std::uint32_t kFlow = 1u << 5;
inline constexpr std::uint32_t kAll =
    kFiles | kArtifacts | kQor | kTiming | kArea | kFlow;
// 解析热点段（用于"这次请求需不需要真的读/解析报告"的判断）
inline constexpr std::uint32_t kHeavy = kQor | kTiming | kArea | kFlow;

// "qor"/"timing"/"area"/"flow"/"files"/"artifacts"/"all" -> bit。
// 不认识的名字返回 kNone 且 *ok=false（调用方应回 invalid-argument）。
std::uint32_t Parse(std::string_view name, bool* ok);

// 单个 bit 的名字（多 bit 或 kNone 返回空串）。
std::string_view Name(std::uint32_t bit);

// 名字列表 -> 掩码（空列表 = kAll，与 Python/插件的默认语义一致）。
std::uint32_t MaskFromList(const std::vector<std::string>& names, bool* ok);

}  // namespace sections

// -----------------------------------------------------------------------------
// 每文件解析结果的记忆化接口（Phase 2 / A3）
// -----------------------------------------------------------------------------
//  BuildReports 在解析每个报告文件前先问它；返回非空就用缓存结果，不再读盘/解析。
//  ReportStore（src/server/report_store.*）是实现方；测试里可用计数实现来证明
//  "同一文件只解析一次"。
//
//  约定：
//    * 返回 `shared_ptr<const T>`：快照可以长期持有，调用方不需要拷贝。
//    * 实现方**必须**按 (path, mtime, size) 校验；文件变了要重新解析。
//    * 本接口不抛异常（实现内部 never-throw，失败返回 nullptr 让调用方走正常解析）。
class ReportParseCache {
  public:
    virtual ~ReportParseCache() = default;
    virtual std::shared_ptr<const QorReport> GetQor(const std::string& path,
                                                    std::string_view step) = 0;
    virtual std::shared_ptr<const TimingReport> GetTiming(const std::string& path,
                                                          std::string_view filename) = 0;
    virtual std::shared_ptr<const AreaReport> GetArea(const std::string& path) = 0;
    virtual std::shared_ptr<const FlowStatusReport> GetFlowStatus(const std::string& path) = 0;
    // TD 的 *_qor_suggestion.tcl（纯 set_param 处方）
    virtual std::shared_ptr<const std::vector<QorSuggestion>> GetSuggestions(
        const std::string& path) = 0;
};

// 构建参数：section 掩码 + 可选的每文件解析缓存。
//   cache 为空时行为与旧版 `BuildReports(project)` **完全一致**（测试/工具/CLI 都不受影响）。
struct ReportsBuildOptions {
    std::uint32_t sections = sections::kAll;
    ReportParseCache* cache = nullptr;
};

// 用到的报告文件路径（找不到的字段是空串）
struct ReportsFiles {
    std::string qor;
    std::string timing;
    std::string area;
    std::string flow_status;
    std::string bit;

    [[nodiscard]] jsonrpc::Json ToJson(bool include_bit) const;
};

// 一个阶段（Synthesis / Place & Route）的报告
struct StageSection {
    bool ok = false;
    std::string label;  // "Synthesis" / "Place & Route"
    ReportsFiles files;
    std::optional<QorReport> qor;
    std::optional<TimingReport> timing;
    std::optional<AreaReport> area;
    std::optional<FlowStatusReport> flow_status;

    [[nodiscard]] jsonrpc::Json ToJson() const;
};

// -----------------------------------------------------------------------------
//  B-B6：两次 run 的差异（QoR 指标 + 时序 summary + 面积总量）
// -----------------------------------------------------------------------------
//  `_Runs/<name>/` 里每次 flow 都会留下 route.qor / final_timing.rpt / *.area。
//  "为什么这次比上次差" 是用户最常问的问题之一，而数据一直都在，只是没有对比入口。
struct RunMetrics {
    std::string name;              // "phy_1"
    std::string dir;
    std::map<std::string, double> values;  // 指标名 -> 数值
    std::vector<std::string> files;        // 参与解析的文件（便于追溯）
};

// 列出工程下的 run（按名字排序）。没有 *_Runs 目录时返回空。
std::vector<RunMetrics> ListRuns(const project::Project& project);

// 对比两次 run。from/to 为空时取最近两个（名字序）。返回可直接给 UI/MCP 的 JSON：
//   {ok, from, to, metrics:[{name,from,to,delta,direction,improved}], summary:{...}}
jsonrpc::Json CompareRuns(const project::Project& project, std::string_view from,
                          std::string_view to);

struct ReportsBundle {
    bool ok = false;
    std::string error;
    ArtifactsReport artifacts;
    ReportsFiles files;
    std::optional<QorReport> qor;
    std::optional<TimingReport> timing;
    std::optional<AreaReport> area;
    std::optional<FlowStatusReport> flow_status;
    std::optional<StageSection> syn;
    std::optional<StageSection> phy;
    double generated_at = 0.0;

    // v13：顶层 flowStatus.stages = {"syn": <syn.flowStatus 的 JSON>, "phy": ...}
    // 只包含**实际存在**的阶段（与 Python 的 `if export.get(key)` 一致），保序。
    std::vector<std::pair<std::string, jsonrpc::Json>> flow_stages;

    // 解析过程中所有子解析器的 warnings（不进 ToJson，供日志/测试）
    std::vector<std::string> warnings;

    [[nodiscard]] jsonrpc::Json ToJson() const;
};

// 汇总一个工程的最新一次 flow 的所有报告。
// never-throw：没有任何 runs 目录时返回 ok=false + error，不抛异常。
//
// options.sections 控制"解析哪几段"（懒构建）；options.cache 提供每文件解析记忆化。
// 不传 options 时等价于"全量 + 无缓存"，与 Phase 1 的行为逐字节一致。
ReportsBundle BuildReports(const project::Project& project,
                          const ReportsBuildOptions& options = ReportsBuildOptions{});

}  // namespace fpgalsp::reports