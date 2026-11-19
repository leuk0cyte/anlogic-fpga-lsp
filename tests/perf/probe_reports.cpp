// =============================================================================
//  tests/perf/probe_reports.cpp —— 报告管线（fpgaLsp/reports）成本拆解
// =============================================================================
//  目标：把"full 报告 238 ms / 26 MB"这笔账拆开，回答三件事：
//    1. 238 ms 里各阶段（读文件 / 切行 / 解析 / 聚合 / JSON 树 / 序列化 / 拷贝）各占多少；
//    2. ReportStore 的 (path,mtime,size) 缓存到底管不管用，section 掩码是否真跳过解析；
//    3. 报告体积从 10 MB 涨到 100 MB 时的斜率（有没有 O(N²)）。
//
//  ⚠ 只读 src，不改 src：所有阶段都用**公开 API** 单独计时，再与 `BuildReports`
//     的总时长交叉核对（口径见 reports/README 的 M 级证据）。
//
//  场景（第一个位置参数）：
//     phases   阶段账本（无缓存全量 / 只 timing / 只 summary）
//     store    ReportStore 冷启动 + 连续 10 次命中曲线 + section 掩码实验 + 请求后处理
//     scale    体积序列（默认 3/6/10/20 MB）→ 斜率与 JSON/source 比
//     all      以上全部（默认）
//
//  用法：
//     .\build\bin\perf_reports.exe --al %TEMP%\fpgalsp-perf\reports\report010\report010.al ^
//         --iters 20 --cold-iters 5 --json-out %TEMP%\fpgalsp-perf\reports\probe.json
// =============================================================================

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "perf_common.h"

#include "analysis/analysis.h"
#include "flow/strategy.h"
#include "jsonrpc/json_util.h"
#include "project/al_project.h"
#include "reports/artifacts.h"
#include "reports/report_text.h"
#include "reports/reports_aggregate.h"
#include "reports/timing.h"
#include "server/report_store.h"
#include "td/td_install.h"

namespace {

using Json = fpgalsp::jsonrpc::Json;
using fpgalsp::project::Project;
namespace reports = fpgalsp::reports;
namespace sections = fpgalsp::reports::sections;

// -----------------------------------------------------------------------------
// 小工具
// -----------------------------------------------------------------------------

// ⚠ 不要写成 MsSince(Clock::now())：那是"当前时刻减当前时刻"，恒为 0。
//    正确用法：auto t0 = Clock::now(); ...; MsSince(t0)。
double ElapsedMs(perf::Clock::time_point t0) { return perf::MsSince(t0); }

void PrintBytes(const char* label, std::size_t bytes) {
    std::printf("%-38s %10zu bytes (%.2f MB)\n", label, bytes,
                static_cast<double>(bytes) / (1024.0 * 1024.0));
    std::printf("csv,bytes,%s,%zu,%.3f\n", label, bytes,
                static_cast<double>(bytes) / (1024.0 * 1024.0));
}

void PrintStat(const perf::Stats& st) {
    st.Print();
    st.PrintCsv();
}

// F3 起 `ReportsSnapshot::bytes` 恒为 0（去掉了一次 100–145 ms 的诊断 dump）。
// 探针要 payload 大小时自己序列化一次（只在打印/核对时做，不进计时循环）。
std::size_t SnapshotWireBytes(const fpgalsp::server::ReportsSnapshot& snapshot) {
    return fpgalsp::jsonrpc::Serialize(snapshot.data, /*compact=*/true).size();
}

// 取中位数（单次测量里最稳的"这一档要多久"）
double Median(const perf::Stats& st) { return st.P50(); }

// -----------------------------------------------------------------------------
// 计数用的 ReportParseCache：**不缓存**，只记录每个文件被问了几次。
// 用它证明「同一个报告文件在一次 BuildReports 里被解析几遍」。
// -----------------------------------------------------------------------------
class CountingCache final : public reports::ReportParseCache {
  public:
    std::shared_ptr<const reports::QorReport> GetQor(const std::string& path,
                                                     std::string_view) override {
        ++qor_calls;
        qor_paths.push_back(path);
        return nullptr;
    }
    std::shared_ptr<const reports::TimingReport> GetTiming(const std::string& path,
                                                           std::string_view) override {
        ++timing_calls;
        timing_paths.push_back(path);
        return nullptr;
    }
    std::shared_ptr<const reports::AreaReport> GetArea(const std::string& path) override {
        ++area_calls;
        area_paths.push_back(path);
        return nullptr;
    }
    std::shared_ptr<const reports::FlowStatusReport> GetFlowStatus(
        const std::string& path) override {
        ++flow_calls;
        flow_paths.push_back(path);
        return nullptr;
    }
    std::shared_ptr<const std::vector<reports::QorSuggestion>> GetSuggestions(
        const std::string& path) override {
        ++suggestion_calls;
        suggestion_paths.push_back(path);
        return nullptr;
    }

    void Reset() {
        qor_calls = timing_calls = area_calls = flow_calls = suggestion_calls = 0;
        qor_paths.clear();
        timing_paths.clear();
        area_paths.clear();
        flow_paths.clear();
        suggestion_paths.clear();
    }

    int qor_calls = 0;
    int timing_calls = 0;
    int area_calls = 0;
    int flow_calls = 0;
    int suggestion_calls = 0;
    std::vector<std::string> qor_paths;
    std::vector<std::string> timing_paths;
    std::vector<std::string> area_paths;
    std::vector<std::string> flow_paths;
    std::vector<std::string> suggestion_paths;
};

std::size_t CountUsages(const std::vector<std::string>& paths) {
    std::vector<std::string> unique = paths;
    std::sort(unique.begin(), unique.end());
    unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
    return unique.size();
}

// -----------------------------------------------------------------------------
// 场景 1：阶段账本
// -----------------------------------------------------------------------------
struct PhaseReport {
    std::size_t timing_file_bytes = 0;
    std::size_t area_file_bytes = 0;
    std::size_t json_bytes = 0;
    double total_ms = 0.0;
};

PhaseReport RunPhaseLedger(const Project& project, int cold_iters) {
    PhaseReport out;

    // 先把"报告文件路径"取出来（CollectArtifacts 一次就够）
    const reports::ArtifactsReport artifacts = reports::CollectArtifacts(project);
    if (!artifacts.runs_found) {
        std::printf("!! 找不到 *_Runs 目录（%s）——先跑 tools/perf/reports/make_synth.py\n",
                    artifacts.runs_dir.c_str());
        return out;
    }

    reports::ReportsBuildOptions all;
    all.sections = sections::kAll;
    // 探一次把路径确定下来（无缓存：这一次就是"冷"的）
    const reports::ReportsBundle warm = reports::BuildReports(project, all);
    const std::string timing_path = warm.files.timing;
    const std::string area_path = warm.files.area;
    out.timing_file_bytes = std::filesystem::file_size(timing_path);
    if (!area_path.empty()) out.area_file_bytes = std::filesystem::file_size(area_path);
    out.json_bytes = warm.ToJson().dump().size();

    std::printf("\n### 1. 阶段账本（full = 无 section 掩码、无缓存）\n");
    PrintBytes("timing 报告文件", out.timing_file_bytes);
    PrintBytes("area 报告文件", out.area_file_bytes);
    PrintBytes("full 响应 JSON（dump 后）", out.json_bytes);
    std::printf("json/source 比 = %.2fx\n",
                out.timing_file_bytes == 0
                    ? 0.0
                    : static_cast<double>(out.json_bytes) / out.timing_file_bytes);

    // 单阶段微基准：每个阶段单独跑 cold_iters 次（都基于同一份已读入内存的文本）
    const std::string timing_text = reports::ReadReportFile(timing_path);
    const std::string area_text = area_path.empty() ? std::string()
                                                    : reports::ReadReportFile(area_path);

    perf::PhaseLedger ledger;
    {
        const auto st = perf::Bench("CollectArtifacts()", cold_iters, [&] {
            (void)reports::CollectArtifacts(project);
        });
        ledger.Add("artifacts(stat/glob)", Median(st));
    }
    {
        const auto st = perf::Bench("ReadReportFile(timing 10MB)", cold_iters, [&] {
            const std::string text = reports::ReadReportFile(timing_path);
            if (text.empty()) std::printf("!! 读不到 %s\n", timing_path.c_str());
        });
        ledger.Add("read timing file", Median(st));
    }
    {
        const auto st = perf::Bench("SplitLines(10MB)", cold_iters, [&] {
            const auto lines = reports::SplitLines(timing_text);
            if (lines.empty()) std::printf("!! 空行表\n");
        });
        ledger.Add("split lines", Median(st));
    }
    {
        const auto st = perf::Bench("ParseTiming(10MB)", cold_iters, [&] {
            const reports::TimingReport report =
                reports::ParseTiming(timing_text, timing_path);
            if (report.raw.size() != timing_text.size()) std::printf("!! raw 长度不符\n");
        });
        ledger.Add("parse timing", Median(st));
    }
    if (!area_text.empty()) {
        const auto st = perf::Bench("ParseArea(2MB)", cold_iters, [&] {
            (void)reports::ParseArea(area_text);
        });
        ledger.Add("parse area", Median(st));
    }
    // 一次完整的 bundle + JSON 树 + 序列化
    {
        const auto st = perf::Bench("BuildReports(kAll) total", cold_iters, [&] {
            (void)reports::BuildReports(project, all);
        });
        out.total_ms = Median(st);
        ledger.Add("BuildReports(kAll) 总计", Median(st));
    }
    {
        const auto st = perf::Bench("bundle.ToJson()", cold_iters, [&] {
            (void)warm.ToJson();
        });
        ledger.Add("aggregate -> JSON tree", Median(st));
    }
    {
        const Json tree = warm.ToJson();
        const auto st = perf::Bench("json.dump(26MB)", cold_iters, [&] {
            const std::string text = tree.dump();
            if (text.empty()) std::printf("!! dump 为空\n");
        });
        ledger.Add("JSON serialize (dump)", Median(st));
    }

    ledger.Print("full 请求各阶段（p50，单次测量）");
    std::printf("注：阶段是**逐个单独测量**的，不是同一次调用的切片；"
                "BuildReports 总计与各段之和不必相等（解析器内部有共享的切行/拷贝）。\n");

    // 只 timing 的掩码（插件实际用的那一档）
    {
        reports::ReportsBuildOptions timing_only;
        timing_only.sections = sections::kTiming;
        const auto st = perf::Bench("BuildReports(kTiming)", cold_iters, [&] {
            (void)reports::BuildReports(project, timing_only);
        });
        PrintStat(st);
        const reports::ReportsBundle bundle = reports::BuildReports(project, timing_only);
        PrintBytes("kTiming 响应 JSON", bundle.ToJson().dump().size());
    }
    // 只有元数据（插件首帧 section=summary）
    {
        reports::ReportsBuildOptions summary;
        summary.sections = sections::kFiles | sections::kArtifacts;
        const auto st = perf::Bench("BuildReports(summary)", cold_iters, [&] {
            (void)reports::BuildReports(project, summary);
        });
        PrintStat(st);
        const reports::ReportsBundle bundle = reports::BuildReports(project, summary);
        PrintBytes("summary 响应 JSON", bundle.ToJson().dump().size());
    }

    // 同一文件被问几次：计数器版 cache（永不命中）
    {
        CountingCache counter;
        reports::ReportsBuildOptions options;
        options.sections = sections::kAll;
        options.cache = &counter;
        (void)reports::BuildReports(project, options);
        std::printf("\n### 2. 一次 full 构建里每个报告文件被请求了几次（无缓存）\n");
        std::printf("timing   : %d 次 / %zu 个不同路径\n", counter.timing_calls,
                    CountUsages(counter.timing_paths));
        std::printf("qor      : %d 次 / %zu 个不同路径\n", counter.qor_calls,
                    CountUsages(counter.qor_paths));
        std::printf("area     : %d 次 / %zu 个不同路径\n", counter.area_calls,
                    CountUsages(counter.area_paths));
        std::printf("flow     : %d 次 / %zu 个不同路径\n", counter.flow_calls,
                    CountUsages(counter.flow_paths));
        std::printf("suggest  : %d 次 / %zu 个不同路径\n", counter.suggestion_calls,
                    CountUsages(counter.suggestion_paths));
        for (const std::string& path : counter.timing_paths) {
            std::printf("  timing> %s\n", path.c_str());
        }
    }
    return out;
}

// -----------------------------------------------------------------------------
// 场景 2：ReportStore 缓存曲线 + section 掩码 + 请求后处理
// -----------------------------------------------------------------------------
void RunStoreCurve(const Project& project, int iters) {
    std::printf("\n### 3. ReportStore：冷启动 + 连续 10 次命中\n");
    std::vector<double> cold_ms;
    std::vector<double> hit_ms;
    std::vector<double> summary_hit_ms;
    std::uint64_t parses_before = 0;
    std::uint64_t parses_after_mask = 0;

    const int rounds = std::max(3, iters / 5);
    for (int round = 0; round < rounds; ++round) {
        fpgalsp::server::ReportStore store;
        const auto cold_start = perf::Clock::now();
        auto snapshot = store.Get(project, sections::kAll);
        cold_ms.push_back(ElapsedMs(cold_start));
        if (snapshot == nullptr || snapshot->data.is_null()) {
            std::printf("!! 快照为空\n");
            return;
        }
        // 连续 10 次同 mask 请求
        double first_hit = 0.0;
        for (int i = 0; i < 10; ++i) {
            const auto hit_start = perf::Clock::now();
            auto again = store.Get(project, sections::kAll);
            const double dt = ElapsedMs(hit_start);
            if (again != snapshot) std::printf("!! 第 %d 次命中拿到了新快照\n", i);
            if (i == 0) first_hit = dt;
            hit_ms.push_back(dt);
        }
        // summary 掩码（插件首帧）
        {
            const auto summary_start = perf::Clock::now();
            (void)store.Get(project, sections::kFiles | sections::kArtifacts);
            summary_hit_ms.push_back(ElapsedMs(summary_start));
        }
        // section 掩码实验：先记录解析数，再只要 QoR，看是否还会解析 timing
        const auto stats_before = store.stats();
        (void)store.Get(project, sections::kQor);
        const auto stats_after = store.stats();
        parses_before = stats_before.parses;
        parses_after_mask = stats_after.parses;
        if (round == 0) {
            std::printf("冷构建 %.3f ms（快照 JSON %.2f MB，revision=%llu）\n",
                        cold_ms.back(),
                        static_cast<double>(SnapshotWireBytes(*snapshot)) / (1024.0 * 1024.0),
                        static_cast<unsigned long long>(snapshot->revision));
            std::printf("首次命中 %.3f ms；随后 10 次：", first_hit);
            for (double v : hit_ms) std::printf("%.3f ", v);
            std::printf("ms\n");
            std::printf("summary 掩码首次 %.3f ms\n", summary_hit_ms.back());
            std::printf("只要 QoR 时的增量：parses %llu -> %llu，file_reads %llu -> %llu\n",
                        static_cast<unsigned long long>(stats_before.parses),
                        static_cast<unsigned long long>(stats_after.parses),
                        static_cast<unsigned long long>(stats_before.file_reads),
                        static_cast<unsigned long long>(stats_after.file_reads));
        }
    }

    const auto stat_of = [](const char* label, std::vector<double>& values) {
        std::sort(values.begin(), values.end());
        perf::Stats st;
        st.label = label;
        st.samples_ms = values;
        PrintStat(st);
    };
    stat_of("ReportStore 冷构建(kAll)", cold_ms);
    stat_of("ReportStore 快照命中(kAll)", hit_ms);
    stat_of("ReportStore 快照命中(summary)", summary_hit_ms);
    std::printf("（%d 轮；每轮一个新 ReportStore，所以冷构建每轮都真的重新解析）\n", rounds);

    // ---- 请求后处理成本：FeatureReports 在拿到快照之后还要做的事 ----
    std::printf("\n### 4. 请求后处理（拿到快照之后、返回之前的固定开销）\n");
    fpgalsp::server::ReportStore store;
    const auto snapshot = store.Get(project, sections::kAll);

    {
        const auto st = perf::Bench("Json copy = snapshot->data", iters, [&] {
            Json copy = snapshot->data;  // FeatureReports 就是这么拷的（26 MB 深拷贝）
            if (copy.is_null()) std::printf("!! 空\n");
        });
        PrintStat(st);
    }
    {
        Json copy = snapshot->data;
        const auto st = perf::Bench("copy.dump()", iters, [&] {
            const std::string text = copy.dump();
            if (text.empty()) std::printf("!! 空\n");
        });
        PrintStat(st);
    }
    {
        // 分节请求的 raw 裁剪（FeatureReports 的 drop_raw 递归）
        Json copy = snapshot->data;
        const auto st = perf::Bench("drop_raw 递归裁剪", iters, [&] {
            Json work = copy;
            const auto drop = [](auto&& self, Json& node) -> void {
                if (!node.is_object()) return;
                node.erase("raw");
                for (const char* section : {"timing", "area", "qor"}) {
                    if (node.contains(section) && node[section].is_object()) {
                        node[section].erase("raw");
                    }
                }
                for (const char* child : {"syn", "phy"}) {
                    if (node.contains(child) && node[child].is_object()) self(self, node[child]);
                }
            };
            drop(drop, work);
        });
        PrintStat(st);
    }
    {
        const auto st = perf::Bench("analysis::NewestRoutedDb()", iters, [&] {
            (void)fpgalsp::analysis::NewestRoutedDb(project);
        });
        PrintStat(st);
    }
    {
        std::optional<fpgalsp::td::TdChoice> choice;
        const auto st = perf::Bench("td::FindTd()", iters, [&] {
            choice = fpgalsp::td::FindTd({});
        });
        PrintStat(st);
        if (choice.has_value()) {
            const auto st2 = perf::Bench("flow::AvailableStrategies()", iters, [&] {
                (void)fpgalsp::flow::AvailableStrategies(choice->root, project.family);
            });
            PrintStat(st2);
        } else {
            std::printf("td::FindTd(): 未找到 TD（跳过 AvailableStrategies）\n");
        }
    }
    {
        // 真正"缓存命中后仍要付"的总和：拷贝 + 装饰 + 序列化
        const auto st = perf::Bench("命中后的整条返回路径(拷贝+dump)", iters, [&] {
            Json copy = snapshot->data;
            copy["project"] = project.name;
            copy["routed_db"] = std::string();
            const std::string text = copy.dump();
            if (text.empty()) std::printf("!! 空\n");
        });
        PrintStat(st);
    }

    // ---- 插件真实走的那一档：section=timing ----
    std::printf("\n### 4b. section=timing 这一档（插件打开时序面板时）\n");
    {
        fpgalsp::server::ReportStore store2;
        const auto cold_start = perf::Clock::now();
        const auto timing_snapshot = store2.Get(project, sections::kTiming);
        std::printf("冷构建 %.3f ms\n", ElapsedMs(cold_start));
        const auto hit_start = perf::Clock::now();
        (void)store2.Get(project, sections::kTiming);
        std::printf("快照命中 %.3f ms\n", ElapsedMs(hit_start));

        // FeatureReports：拷一份 -> 递归删 raw -> 序列化（include_raw 默认 false）
        const auto wire_start = perf::Clock::now();
        Json copy = timing_snapshot->data;
        const auto drop = [](auto&& self, Json& node) -> void {
            if (!node.is_object()) return;
            node.erase("raw");
            for (const char* section : {"timing", "area", "qor"}) {
                if (node.contains(section) && node[section].is_object()) node[section].erase("raw");
            }
            for (const char* child : {"syn", "phy"}) {
                if (node.contains(child) && node[child].is_object()) self(self, node[child]);
            }
        };
        drop(drop, copy);
        const std::string wire = copy.dump();
        std::printf("命中后的返回路径（拷贝 + 删 raw + dump）合计 %.3f ms，"
                    "实际 payload %.2f MB（快照原始 %.2f MB）\n",
                    ElapsedMs(wire_start), static_cast<double>(wire.size()) / (1024.0 * 1024.0),
                    static_cast<double>(SnapshotWireBytes(*timing_snapshot)) / (1024.0 * 1024.0));
        std::printf("（kTiming 快照里 raw 占 %.1f MB；paths 与 groups[].paths 各存一份）\n",
                    static_cast<double>(reports::ReadReportFile(project.dir + "/" +
                                                               project.name + "_Runs/phy_1/"
                                                               "final_timing.rpt").size()) /
                        (1024.0 * 1024.0));
    }
}

// -----------------------------------------------------------------------------
// 场景 3b：**命中之后**的成本（这是本任务的核心）
// -----------------------------------------------------------------------------
//  `FeatureReports`（src/server/commands.cpp:326）拿到快照后做四件事：
//      Json data = report_store_->Get(project, mask)->data;   // ① 深拷贝整份快照
//      data["section"] = ...; limit 裁剪 paths;               // ② 裁剪
//      data["project"]/["al"]/["flow"]/["routed_db"]/...      // ③ 装饰
//      jsonrpc::Serialize(data, compact=false)                // ④ dump + AddPythonStyleSpaces
//  ①④ 都是 O(payload)，与"解析"无关 —— 缓存命中救不了它们。
void RunHitPath(const Project& project, int iters) {
    std::printf("\n### 6. 命中之后的成本拆解（快照已就绪，解析为 0）\n");
    fpgalsp::server::ReportStore store;
    const auto snapshot = store.Get(project, sections::kAll);
    if (snapshot == nullptr) {
        std::printf("!! 快照为空\n");
        return;
    }
    const double payload_mb = static_cast<double>(SnapshotWireBytes(*snapshot)) / (1024.0 * 1024.0);
    std::printf("快照 payload: %.2f MB\n", payload_mb);

    // ---- payload 构成：每个顶层键占多少（决定"该裁哪里"）----
    {
        Json sized = snapshot->data;
        const auto drop_for_size = [](auto&& self, Json& node) -> void {
            if (!node.is_object()) return;
            node.erase("raw");
            for (const char* section : {"timing", "area", "qor"}) {
                if (node.contains(section) && node[section].is_object()) node[section].erase("raw");
            }
            for (const char* child : {"syn", "phy"}) {
                if (node.contains(child) && node[child].is_object()) self(self, node[child]);
            }
        };
        drop_for_size(drop_for_size, sized);
        std::printf("payload 构成（已去 raw）：\n");
        std::vector<std::pair<std::size_t, std::string>> rows;
        for (auto it = sized.begin(); it != sized.end(); ++it) {
            rows.emplace_back(fpgalsp::jsonrpc::Serialize(it.value(), true).size(), it.key());
        }
        std::sort(rows.begin(), rows.end(), std::greater<>());
        for (const auto& [bytes, key] : rows) {
            std::printf("  %-12s %8.2f MB\n", key.c_str(),
                        static_cast<double>(bytes) / (1024.0 * 1024.0));
        }
    }

    // ① 深拷贝（FeatureReports 的 `->data`）
    const std::size_t ws_before = perf::CurrentWorkingSetBytes();
    double ws_after_copy = 0.0;
    const auto copy_stats = perf::Bench("① Json copy = snapshot->data", iters, [&] {
        Json copy = snapshot->data;
        ws_after_copy = static_cast<double>(perf::CurrentWorkingSetBytes());
        if (copy.is_null()) std::printf("!! 空\n");
    });
    PrintStat(copy_stats);
    std::printf("   深拷贝后工作集 +%.2f MB（进程级，参考值）\n",
                (ws_after_copy - static_cast<double>(ws_before)) / (1024.0 * 1024.0));

    // ② 裁剪：drop_raw（分节请求）+ limit（paths 截断）
    const auto drop_raw = [](auto&& self, Json& node) -> void {
        if (!node.is_object()) return;
        node.erase("raw");
        for (const char* section : {"timing", "area", "qor"}) {
            if (node.contains(section) && node[section].is_object()) node[section].erase("raw");
        }
        for (const char* child : {"syn", "phy"}) {
            if (node.contains(child) && node[child].is_object()) self(self, node[child]);
        }
    };
    const auto prune_stats = perf::Bench("①+② copy + drop_raw", iters, [&] {
        Json copy = snapshot->data;
        drop_raw(drop_raw, copy);
    });
    PrintStat(prune_stats);
    std::printf("   ② drop_raw 净成本 ≈ %.2f ms（= 上一行 - ①）\n",
                prune_stats.P50() - copy_stats.P50());

    {
        const auto st = perf::Bench("①+②+③ copy+drop_raw+装饰", iters, [&] {
            Json copy = snapshot->data;
            drop_raw(drop_raw, copy);
            copy["project"] = project.name;
            copy["al"] = project.al_path;
            copy["section"] = "timing";
            copy["routed_db"] = std::string();
            copy["flow"] = Json::object();
            copy["strategies"] = Json{{"available", Json::array()}};
        });
        PrintStat(st);
        std::printf("   ③ 装饰净成本 ≈ %.2f ms\n", st.P50() - prune_stats.P50());
        (void)st;
    }

    // ④ 真实写线路径：jsonrpc::Serialize(compact=false) = dump(-1) + AddPythonStyleSpaces
    Json prepared = snapshot->data;
    drop_raw(drop_raw, prepared);
    double serialize_spaces_ms = 0.0;
    double serialize_compact_ms = 0.0;
    double wire_mb = 0.0;
    {
        const auto st = perf::Bench("④ Serialize(compact=false)", iters, [&] {
            const std::string out = fpgalsp::jsonrpc::Serialize(prepared, /*compact=*/false);
            if (out.empty()) std::printf("!! 空\n");
        });
        serialize_spaces_ms = st.P50();
        PrintStat(st);
        const std::string out = fpgalsp::jsonrpc::Serialize(prepared, false);
        wire_mb = static_cast<double>(out.size()) / (1024.0 * 1024.0);
        std::printf("   写线字节 %.2f MB\n", wire_mb);
        const auto compact = perf::Bench("④' Serialize(compact=true)", iters, [&] {
            const std::string text = fpgalsp::jsonrpc::Serialize(prepared, /*compact=*/true);
            if (text.empty()) std::printf("!! 空\n");
        });
        serialize_compact_ms = compact.P50();
        PrintStat(compact);
        std::printf("   AddPythonStyleSpaces 的额外成本 ≈ %.2f ms（compact 与非 compact 之差）\n",
                    serialize_spaces_ms - serialize_compact_ms);
    }

    // ⑤ 整条命中路径（与 FeatureReports 同序）：拷贝 -> 裁剪 -> 装饰 -> 序列化
    double hit_path_ms = 0.0;
    {
        const auto st = perf::Bench("整条命中路径(①+②+③+④)", iters, [&] {
            Json copy = snapshot->data;
            drop_raw(drop_raw, copy);
            copy["project"] = project.name;
            copy["al"] = project.al_path;
            copy["section"] = "timing";
            copy["routed_db"] = std::string();
            const std::string out = fpgalsp::jsonrpc::Serialize(copy, false);
            if (out.empty()) std::printf("!! 空\n");
        });
        hit_path_ms = st.P50();
        PrintStat(st);
    }

    // ---- 修法估算（不改代码，只量"如果不拷贝 / 如果提前裁剪"能省多少）----
    std::printf("\n### 7. 修法估算（不改产品代码，只量可省掉的那一段）\n");
    const double full_hit_no_copy = [&] {
        const auto st = perf::Bench("修法A：不拷贝直接序列化(引用快照)", iters, [&] {
            const std::string out = fpgalsp::jsonrpc::Serialize(snapshot->data, false);
            if (out.empty()) std::printf("!! 空\n");
        });
        PrintStat(st);
        return st.P50();
    }();

    // 修法 B 的三种"提前裁剪"强度（都不改产品代码，只构造裁剪后的 payload 量成本）：
    //   B1 = 现在的 `limit` 语义（只裁每个节点自己的 paths，groups[] 原样）——几乎不省
    //   B2 = 再把 groups 数组裁到 1 组（并裁组内 paths）——真正把 payload 压下来
    //   B3 = 只留摘要（paths / groups.paths 全清）——"几 KB"那一档
    const auto trim_paths = [](auto&& self, Json& node, std::size_t keep) -> void {
        if (node.is_array()) {
            for (Json& item : node) self(self, item, keep);
            return;
        }
        if (!node.is_object()) return;
        if (node.contains("paths") && node["paths"].is_array() &&
            node["paths"].size() > keep) {
            node["paths"].erase(node["paths"].begin() + static_cast<std::ptrdiff_t>(keep),
                                node["paths"].end());
        }
        for (auto it = node.begin(); it != node.end(); ++it) self(self, it.value(), keep);
    };

    const auto measure_pruned = [&](const char* label, Json pruned) {
        const double mb = static_cast<double>(fpgalsp::jsonrpc::Serialize(pruned, false).size()) /
                          (1024.0 * 1024.0);
        const auto st = perf::Bench(label, iters, [&] {
            Json copy = pruned;
            const std::string out = fpgalsp::jsonrpc::Serialize(copy, false);
            if (out.empty()) std::printf("!! 空\n");
        });
        PrintStat(st);
        std::printf("   payload %.3f MB -> 命中路径 %.1f ms（原 %.1f ms）\n", mb, st.P50(),
                    hit_path_ms);
        return mb;
    };

    {
        // 原样照抄 commands.cpp:331-344 的 limit 裁剪逻辑（只认 node 自己的 paths，
        // 且只下钻 syn/phy 两层）——用来验证它到底裁到了什么。
        Json exact = snapshot->data;
        drop_raw(drop_raw, exact);
        const auto exact_trim = [](auto&& self, Json& node) -> void {
            if (!node.is_object()) return;
            if (node.contains("paths") && node["paths"].is_array() &&
                node["paths"].size() > 200) {
                node["paths"].erase(node["paths"].begin() + 200, node["paths"].end());
                node["truncated"] = true;
            }
            for (const char* child : {"syn", "phy"}) {
                if (node.contains(child) && node[child].is_object()) self(self, node[child]);
            }
        };
        exact_trim(exact_trim, exact);
        const double mb = static_cast<double>(fpgalsp::jsonrpc::Serialize(exact, false).size()) /
                          (1024.0 * 1024.0);
        std::printf("按 commands.cpp:331-344 原样执行 limit=200 后 payload = %.3f MB"
                    "（与未裁剪的 32.72 MB 比对；相等 = limit 是 no-op）\n", mb);
    }
    {
        Json b1 = snapshot->data;
        drop_raw(drop_raw, b1);
        trim_paths(trim_paths, b1, 200);
        measure_pruned("修法B1：limit 语义(只裁 paths=200)", std::move(b1));
    }
    {
        Json b2 = snapshot->data;
        drop_raw(drop_raw, b2);
        const auto trim_groups = [&](auto&& self, Json& node) -> void {
            if (node.is_array()) {
                for (Json& item : node) self(self, item);
                return;
            }
            if (!node.is_object()) return;
            if (node.contains("groups") && node["groups"].is_array() &&
                node["groups"].size() > 1) {
                node["groups"].erase(node["groups"].begin() + 1, node["groups"].end());
            }
            for (auto it = node.begin(); it != node.end(); ++it) self(self, it.value());
        };
        trim_groups(trim_groups, b2);
        trim_paths(trim_paths, b2, 200);
        measure_pruned("修法B2：groups 只留 1 组 + paths=200", std::move(b2));
    }
    {
        Json b3 = snapshot->data;
        drop_raw(drop_raw, b3);
        trim_paths(trim_paths, b3, 0);  // 0 = 清空 paths
        measure_pruned("修法B3：只留摘要(paths 全清)", std::move(b3));
    }

    // 单位成本的回归（便于外推到别的 payload 体积）
    std::printf("\n单位成本（按本快照实测回归）：深拷贝 %.2f ms/MB（payload %.2f MB），"
                "Serialize(false) %.2f ms/MB（写线 %.2f MB），"
                "其中 AddPythonStyleSpaces %.2f ms/MB\n",
                payload_mb > 0 ? copy_stats.P50() / payload_mb : 0.0, payload_mb,
                wire_mb > 0 ? serialize_spaces_ms / wire_mb : 0.0, wire_mb,
                wire_mb > 0 ? (serialize_spaces_ms - serialize_compact_ms) / wire_mb : 0.0);
}

// -----------------------------------------------------------------------------
// 场景 3c：并发（顺序命中 vs 并发命中 vs 冷构建重叠）
// -----------------------------------------------------------------------------
void RunConcurrency(const Project& project, int iters) {
    std::printf("\n### 8. 并发：顺序命中 / 并发命中 / 冷构建重叠\n");

    // C1：顺序命中（对照）
    {
        fpgalsp::server::ReportStore store;
        (void)store.Get(project, sections::kAll);
        std::vector<double> calls;
        for (int i = 0; i < 10; ++i) {
            const auto t0 = perf::Clock::now();
            (void)store.Get(project, sections::kAll);
            calls.push_back(ElapsedMs(t0));
        }
        perf::Stats st;
        st.label = "C1 顺序命中(kAll) x10";
        st.samples_ms = calls;
        std::sort(st.samples_ms.begin(), st.samples_ms.end());
        PrintStat(st);
    }

    // C2：两个线程**同时**对一个空 store 请求同一 mask（一个建、一个等）
    {
        fpgalsp::server::ReportStore store;
        double first = 0.0;
        double second = 0.0;
        std::thread a([&] {
            const auto t0 = perf::Clock::now();
            (void)store.Get(project, sections::kAll);
            first = ElapsedMs(t0);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::thread b([&] {
            const auto t0 = perf::Clock::now();
            (void)store.Get(project, sections::kAll);
            second = ElapsedMs(t0);
        });
        a.join();
        b.join();
        const auto stats = store.stats();
        std::printf("C2 冷构建重叠：A(先到) %.1f ms，B(晚 50 ms 到) %.1f ms；"
                    "builds=%llu snapshot_hits=%llu\n",
                    first, second, static_cast<unsigned long long>(stats.builds),
                    static_cast<unsigned long long>(stats.snapshot_hits));
        std::printf("   -> B 也在等 A 的构建（Get 期间持有 state_mutex_ 写锁）\n");
    }

    // C3：两个线程**同时**对一个空 store 请求不同 mask（summary vs kAll）
    {
        fpgalsp::server::ReportStore store;
        double heavy = 0.0;
        double light = 0.0;
        std::thread a([&] {
            const auto t0 = perf::Clock::now();
            (void)store.Get(project, sections::kAll);
            heavy = ElapsedMs(t0);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::thread b([&] {
            const auto t0 = perf::Clock::now();
            (void)store.Get(project, sections::kFiles | sections::kArtifacts);
            light = ElapsedMs(t0);
        });
        a.join();
        b.join();
        std::printf("C3 重构建 + 轻请求：kAll %.1f ms，summary(light) %.1f ms"
                    "（轻请求被重构建阻塞）\n", heavy, light);
    }

    // C4：warm 后两个线程并发走"命中 + 拷贝 + 序列化"（真正的用户可见负载）
    {
        fpgalsp::server::ReportStore store;
        const auto snapshot = store.Get(project, sections::kAll);
        const auto wired = [&] {
            const auto t0 = perf::Clock::now();
            Json copy = snapshot->data;
            const std::string out = fpgalsp::jsonrpc::Serialize(copy, false);
            if (out.empty()) std::printf("!! 空\n");
            return ElapsedMs(t0);
        };
        std::vector<double> single;
        for (int i = 0; i < iters; ++i) single.push_back(wired());
        perf::Stats st_single;
        st_single.label = "C4 单线程 命中+拷贝+序列化";
        st_single.samples_ms = single;
        std::sort(st_single.samples_ms.begin(), st_single.samples_ms.end());
        PrintStat(st_single);

        std::vector<double> a_times;
        std::vector<double> b_times;
        const int per_thread = std::max(3, iters / 4);
        std::thread a([&] {
            for (int i = 0; i < per_thread; ++i) a_times.push_back(wired());
        });
        std::thread b([&] {
            for (int i = 0; i < per_thread; ++i) b_times.push_back(wired());
        });
        a.join();
        b.join();
        perf::Stats st_conc;
        st_conc.label = "C4 两线程并发 命中+拷贝+序列化";
        st_conc.samples_ms = a_times;
        st_conc.samples_ms.insert(st_conc.samples_ms.end(), b_times.begin(), b_times.end());
        std::sort(st_conc.samples_ms.begin(), st_conc.samples_ms.end());
        PrintStat(st_conc);
        std::printf("   并发/单线程 = %.2fx（>1.15x 说明有争用：内存带宽 / 分配器 / 锁）\n",
                    st_single.P50() > 0 ? st_conc.P50() / st_single.P50() : 0.0);
    }
}

// -----------------------------------------------------------------------------
// 场景 9：协议等价性核对（F3 的验收项 ⑤）
// -----------------------------------------------------------------------------
//  F3 只允许改变 `limit>0` 的输出，其余必须逐字节不变。
//  做法：把"改动前 FeatureReports 的语义"（BuildReports 结果直接 ToJson）与
//  "改动后快照里的 data" 逐字节比对；`generatedAt` 是 wall-clock，两次构建必然不同，
//  比对前从两侧一起摘掉（报告里注明）。
void RunVerify(const Project& project) {
    std::printf("\n### 9. 协议等价性（除 limit 外必须逐字节相同）\n");
    const std::vector<std::pair<std::string, std::uint32_t>> masks = {
        {"full(kAll)", sections::kAll},
        {"timing(kTiming)", sections::kTiming},
        {"summary(kFiles|kArtifacts)", sections::kFiles | sections::kArtifacts},
        {"qor(kQor)", sections::kQor},
        {"area(kArea)", sections::kArea},
        {"flow(kFlow)", sections::kFlow},
    };
    fpgalsp::server::ReportStore store;
    for (const auto& [label, mask] : masks) {
        reports::ReportsBuildOptions options;
        options.sections = mask;
        Json before = reports::BuildReports(project, options).ToJson();
        Json after = store.Get(project, mask, /*force=*/false, /*limit=*/0)->data;
        // generatedAt 是 wall clock：两侧都摘掉
        if (before.contains("generatedAt")) before.erase("generatedAt");
        if (after.contains("generatedAt")) after.erase("generatedAt");
        const std::string a = fpgalsp::jsonrpc::Serialize(before, /*compact=*/true);
        const std::string b = fpgalsp::jsonrpc::Serialize(after, /*compact=*/true);
        bool equal = a == b;
        std::size_t diff_at = 0;
        while (equal == false && diff_at < std::min(a.size(), b.size()) && a[diff_at] == b[diff_at]) {
            ++diff_at;
        }
        std::printf("%-28s %s  before=%zu B after=%zu B", label.c_str(),
                    equal ? "IDENTICAL" : "DIFF", a.size(), b.size());
        if (!equal) {
            std::printf("  首个差异 @%zu: before[..]=%s after[..]=%s", diff_at,
                        a.substr(diff_at, 24).c_str(), b.substr(diff_at, 24).c_str());
        }
        std::printf("\n");
    }

    // limit：**唯一**预期的输出变化（修复 no-op 缺陷）
    for (const std::uint32_t limit : {200u, 50u}) {
        Json before = reports::BuildReports(project, {}).ToJson();
        // 改动前的裁剪逻辑（commands.cpp 旧实现）：只找节点自己的 paths，不下钻 timing
        const auto old_trim = [limit](auto&& self, Json& node) -> void {
            if (!node.is_object()) return;
            if (node.contains("paths") && node["paths"].is_array() &&
                node["paths"].size() > static_cast<std::size_t>(limit)) {
                node["paths"].erase(node["paths"].begin() + static_cast<std::ptrdiff_t>(limit),
                                    node["paths"].end());
                node["truncated"] = true;
            }
            for (const char* child : {"syn", "phy"}) {
                if (node.contains(child) && node[child].is_object()) self(self, node[child]);
            }
        };
        old_trim(old_trim, before);
        if (before.contains("generatedAt")) before.erase("generatedAt");
        const std::size_t before_bytes = fpgalsp::jsonrpc::Serialize(before, true).size();

        Json after = store.Get(project, sections::kAll, /*force=*/true, limit)->data;
        if (after.contains("generatedAt")) after.erase("generatedAt");
        const std::size_t after_bytes = fpgalsp::jsonrpc::Serialize(after, true).size();
        std::printf("limit=%-4u before=%zu B (no-op)  after=%zu B（预期变小：真裁剪）\n", limit,
                    before_bytes, after_bytes);
    }
}

// -----------------------------------------------------------------------------
// 场景 10：三档"响应组装"成本（F3 验收 ②③）—— 与 FeatureReports 同序
// -----------------------------------------------------------------------------
//  组装 = 取快照 -> 深拷贝 -> (limit 已进快照) -> drop_raw -> 装饰 -> Serialize
void RunTiers(const Project& project, int iters) {
    std::printf("\n### 10. 三档响应组装（命中快照后的完整成本）\n");
    struct Tier {
        const char* label;
        std::uint32_t mask;
        std::uint32_t limit;
        bool drop_raw;
    };
    const Tier tiers[] = {
        {"full (kAll, limit=0)", sections::kAll, 0u, false},
        {"full (kAll, limit=200)", sections::kAll, 200u, false},
        {"timing (kTiming, limit=200)", sections::kTiming, 200u, true},
        {"summary (kFiles|kArtifacts)", sections::kFiles | sections::kArtifacts, 0u, true},
    };
    fpgalsp::server::ReportStore store;
    const auto drop_raw = [](auto&& self, Json& node) -> void {
        if (!node.is_object()) return;
        node.erase("raw");
        for (const char* section : {"timing", "area", "qor"}) {
            if (node.contains(section) && node[section].is_object()) node[section].erase("raw");
        }
        for (const char* child : {"syn", "phy"}) {
            if (node.contains(child) && node[child].is_object()) self(self, node[child]);
        }
    };
    for (const Tier& tier : tiers) {
        const auto cold_start = perf::Clock::now();
        auto snapshot = store.Get(project, tier.mask, /*force=*/true, tier.limit);
        const double cold_ms = ElapsedMs(cold_start);
        const double snapshot_mb =
            static_cast<double>(SnapshotWireBytes(*snapshot)) / (1024.0 * 1024.0);
        // 预热命中一次，让下面的统计只反映"命中路径"
        (void)store.Get(project, tier.mask, false, tier.limit);
        std::size_t wire = 0;
        const auto st = perf::Bench(tier.label, iters, [&] {
            Json data = store.Get(project, tier.mask, false, tier.limit)->data;  // 深拷贝
            if (tier.drop_raw) drop_raw(drop_raw, data);
            data["project"] = project.name;
            data["al"] = project.al_path;
            data["routed_db"] = std::string();
            const std::string out = fpgalsp::jsonrpc::Serialize(data, /*compact=*/false);
            wire = out.size();
        });
        PrintStat(st);
        std::printf("   冷构建 %.1f ms；快照 %.2f MB；写线 %.2f MB\n", cold_ms, snapshot_mb,
                    static_cast<double>(wire) / (1024.0 * 1024.0));
    }
}

// -----------------------------------------------------------------------------
// 场景 4：体积斜率（3/6/10/20 MB）
// -----------------------------------------------------------------------------
struct ScalePoint {
    double source_mb = 0.0;
    double parse_ms = 0.0;
    double json_ms = 0.0;
    double dump_ms = 0.0;
    double json_mb = 0.0;
    std::size_t paths = 0;
};

ScalePoint MeasureScale(const std::string& timing_path, int iters) {
    ScalePoint point;
    const std::string text = reports::ReadReportFile(timing_path);
    if (text.empty()) return point;
    point.source_mb = static_cast<double>(text.size()) / (1024.0 * 1024.0);

    const auto parse = perf::Bench("parse", iters, [&] {
        const reports::TimingReport report = reports::ParseTiming(text, timing_path);
        point.paths = report.paths.size();
    });
    point.parse_ms = parse.P50();

    const reports::TimingReport report = reports::ParseTiming(text, timing_path);
    point.paths = report.paths.size();
    const auto build = perf::Bench("json", iters, [&] {
        const Json tree = report.ToJson();
        if (tree.is_null()) std::printf("!! 空\n");
    });
    point.json_ms = build.P50();

    const Json tree = report.ToJson();
    point.json_mb = static_cast<double>(tree.dump().size()) / (1024.0 * 1024.0);
    const auto dump = perf::Bench("dump", iters, [&] {
        const std::string out = tree.dump();
        if (out.empty()) std::printf("!! 空\n");
    });
    point.dump_ms = dump.P50();
    return point;
}

void RunScale(const std::string& series_root, int iters) {
    std::printf("\n### 5. 体积斜率（报告体积 -> 解析 / JSON 树 / 序列化）\n");
    std::vector<ScalePoint> points;
    for (int mb : {3, 6, 10, 20}) {
        char name[32];
        std::snprintf(name, sizeof(name), "report%03d", mb * 10);
        const std::string dir = series_root + "/" + name;
        const std::string timing = dir + "/" + name + "_Runs/phy_1/final_timing.rpt";
        if (!std::filesystem::exists(timing)) {
            std::printf("（跳过 %s：不存在，先 make_synth.py --series --mb 3,6,10,20）\n",
                        timing.c_str());
            continue;
        }
        const ScalePoint point = MeasureScale(timing, iters);
        points.push_back(point);
        std::printf("%5.2f MB  paths=%-7zu  parse=%8.2f ms  json=%8.2f ms  dump=%8.2f ms  "
                    "json=%.2f MB\n",
                    point.source_mb, point.paths, point.parse_ms, point.json_ms,
                    point.dump_ms, point.json_mb);
        std::printf("csv,scale,%.2f,%zu,%.3f,%.3f,%.3f,%.3f\n", point.source_mb, point.paths,
                    point.parse_ms, point.json_ms, point.dump_ms, point.json_mb);
    }
    if (points.size() >= 2) {
        const ScalePoint& first = points.front();
        const ScalePoint& last = points.back();
        const auto slope = [&](double ScalePoint::*member) {
            const double d_size = last.source_mb - first.source_mb;
            return d_size <= 0 ? 0.0 : (last.*member - first.*member) / d_size;
        };
        std::printf("\n斜率（末点 - 首点）/ (MB)：parse %.2f ms/MB，json %.2f ms/MB，"
                    "dump %.2f ms/MB\n",
                    slope(&ScalePoint::parse_ms), slope(&ScalePoint::json_ms),
                    slope(&ScalePoint::dump_ms));
        // 线性外推（置信度：中；10->20 这一段如果明显 >2x 说明进入超线性）
        if (points.size() >= 3) {
            const ScalePoint& mid = points[points.size() - 2];
            const double ratio_parse = mid.parse_ms > 0 ? last.parse_ms / mid.parse_ms : 0.0;
            const double size_ratio =
                mid.source_mb > 0 ? last.source_mb / mid.source_mb : 0.0;
            std::printf("段内比：体积 %.2fx，parse %.2fx -> %s\n", size_ratio, ratio_parse,
                        ratio_parse > size_ratio * 1.25 ? "**超线性迹象**" : "线性");
        }
        std::printf("外推到 100 MB（线性，置信度中）：parse ≈ %.0f ms，json ≈ %.0f ms，"
                    "dump ≈ %.0f ms\n",
                    slope(&ScalePoint::parse_ms) * 100.0 + first.parse_ms,
                    slope(&ScalePoint::json_ms) * 100.0 + first.json_ms,
                    slope(&ScalePoint::dump_ms) * 100.0 + first.dump_ms);
        std::printf("外推 JSON 体积 ≈ %.1f MB（按 %.2fx 比）\n",
                    100.0 * (last.source_mb > 0 ? last.json_mb / last.source_mb : 0.0),
                    last.source_mb > 0 ? last.json_mb / last.source_mb : 0.0);
    }
    std::printf("峰值 Working Set: %.2f MB（进程级，含此前场景的驻留）\n",
                perf::Mb(perf::PeakWorkingSetBytes()));
}

}  // namespace

int main(int argc, char** argv) {
    perf::Banner("reports");

    std::string scene = "all";
    std::string al_path;
    std::string series_root;
    std::string json_out;
    int iters = 20;
    int cold_iters = 5;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--al" && i + 1 < argc) {
            al_path = argv[++i];
        } else if (arg == "--series-root" && i + 1 < argc) {
            series_root = argv[++i];
        } else if (arg == "--iters" && i + 1 < argc) {
            iters = std::atoi(argv[++i]);
        } else if (arg == "--cold-iters" && i + 1 < argc) {
            cold_iters = std::atoi(argv[++i]);
        } else if (arg == "--json-out" && i + 1 < argc) {
            json_out = argv[++i];
        } else if (!arg.empty() && arg.front() != '-') {
            scene = std::string(arg);
        }
    }
    if (series_root.empty() && !al_path.empty()) {
        series_root = std::filesystem::path(al_path).parent_path().parent_path().string();
    }
    if (al_path.empty()) {
        std::printf("用法: perf_reports.exe [phases|store|scale|all] --al <报告工程 .al> "
                    "[--iters 20] [--cold-iters 5] [--series-root DIR]\n");
        return 2;
    }

    const Project project = fpgalsp::project::ParseAl(al_path);
    std::printf("project: name=%s dir=%s top=%s\n", project.name.c_str(), project.dir.c_str(),
                project.top.c_str());
    std::printf("工作集(起始): %.2f MB\n", perf::Mb(perf::CurrentWorkingSetBytes()));

    PhaseReport phase;
    if (scene == "all" || scene == "phases") {
        phase = RunPhaseLedger(project, cold_iters);
    }
    if (scene == "all" || scene == "store") {
        RunStoreCurve(project, iters);
    }
    if (scene == "all" || scene == "hit") {
        RunHitPath(project, iters);
    }
    if (scene == "all" || scene == "conc") {
        RunConcurrency(project, iters);
    }
    if (scene == "all" || scene == "tiers") {
        RunTiers(project, iters);
    }
    if (scene == "verify") {
        RunVerify(project);
        return 0;
    }
    if (scene == "all" || scene == "scale") {
        RunScale(series_root, std::max(3, cold_iters));
    }

    std::printf("\n峰值 Working Set: %.2f MB\n", perf::Mb(perf::PeakWorkingSetBytes()));
    if (!json_out.empty()) {
        std::printf("（提示：csv, 行可直接 grep 汇总；--json-out 目前只做占位）\n");
    }
    return 0;
}