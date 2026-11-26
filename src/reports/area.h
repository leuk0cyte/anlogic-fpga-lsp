// =============================================================================
//  src/reports/area.h —— TD Area 报告解析（*_Runs/<run>/*.area）
// =============================================================================
//  对应参考实现：%PYREF%\fpgalsp\reports.py
//      parse_area() / _parse_area_hierarchy() / _norm_resource() / _area_total()
//
//  ⚠⚠ 本文件最重要的一条设计约束：**绝不假设固定资源名**
//
//  实测三份真实报告的差异（都放在 tests/fixtures/reports/）：
//
//    EF2（EF2L45LG144B）syn 阶段 demo_project_gate.area：
//        LUT Statistics
//        #Total_luts               678
//          #lut4                   529
//          #lut6                     0          <- 只有 EF2 的综合阶段才打 #lut6
//        Utilization Statistics
//        #lut                     1064   out of   4480   23.75%
//        |Instance |Module |lut |ripple |seq |bram |dsp |          <- 7 列
//
//    EF2 rtl 阶段 demo_project_rtl.area：
//        Gate Statistics
//        #Basic gates             1681
//        |Instance |Module |gates |seq |macros |                    <- 5 列，完全不同的一套资源
//
//    EG4/PH1（EG4X20BG256）phy 阶段 CorpusBeta_phy.area：
//        #lut                     7680   out of  19600   39.18%
//        #lut only                2534   out of  10122   25.03%
//        |Instance |Module |le |lut |ripple |seq |bram |dsp |       <- 8 列（多一个 le）
//
//  所以：
//    * rows/totals/counts 全部按"资源名归一化后的 key"存，不枚举；
//    * modules[].resources / utilization 保留**表头给出的每一列**；
//    * 百分比只是"能给则给"：_area_total 通过别名表把 PH1 的 logic lut/eram 映射到
//      webview 认识的 lut/bram，映射不上时 utilization 为 null —— 而不是 0。
// =============================================================================

#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "jsonrpc/json_util.h"

namespace fpgalsp::reports {

// 一行 Utilization 记录（对应 Python rows）
struct AreaRow {
    std::string resource;   // 原样带 # 前缀，如 "#lut6" / "#lut only"
    int used = 0;
    int total = 0;
    std::string utilization;  // 原样字符串，如 "39.18%"
};

// Report Hierarchy Area 表的一行（对应 Python modules）
struct AreaModule {
    std::string name;    // 实例名（cells[0]，已 strip）
    std::string module;  // 模块名（cells[1]）；CWC/黑盒常常是空串
    int level = 0;       // 由前导空格数 // 2 得到
    std::string path;    // 由 level 栈拼出的完整层次路径 "a/b/c"
    std::map<std::string, int> resources;      // 归一化资源名 -> 用量（保表头每一列）
    std::map<std::string, std::optional<double>> utilization;  // 归一化资源名 -> 百分比
};

struct AreaReport {
    std::map<std::string, int> io;      // input / output / inout / le
    std::vector<AreaRow> rows;
    std::map<std::string, int> counts;  // LUT Statistics 段里的 #xxx 计数
    std::map<std::string, int> totals;  // 归一化资源名 -> "out of" 后面的总数
    std::vector<AreaModule> modules;
    std::string raw;
    std::vector<std::string> warnings;

    [[nodiscard]] jsonrpc::Json ToJson() const;
};

// 解析 .area 文本。never-throw。
AreaReport ParseArea(std::string_view text);

// 单独解析 Report Hierarchy Area 表（测试用，等价 Python _parse_area_hierarchy）
std::vector<AreaModule> ParseAreaHierarchy(std::string_view text,
                                           const std::map<std::string, int>& totals);

// 资源名归一化（暴露给测试，等价 Python _norm_resource）
std::string NormAreaResource(std::string_view name);

// 等价 Python _area_total：按别名表在 totals 里找总数，找不到（或为 0）返回 nullopt。
std::optional<int> AreaTotal(const std::map<std::string, int>& totals, std::string_view resource);

}  // namespace fpgalsp::reports