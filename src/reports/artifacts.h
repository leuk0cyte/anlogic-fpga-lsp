// =============================================================================
//  src/reports/artifacts.h —— 流程产物探测（bit / db / runs 列表）
// =============================================================================
//  对应参考实现：%PYREF%\fpgalsp\reports.py
//      _runs_dir() / _first_existing() / _glob() / _stat() 以及 collect() 里选文件的那段
//
//  产物布局（TD 2026.2）：
//      <project>/<name>_Runs/
//          syn_1/     gate.qor  *_gate.area  *_gate.timing  flow.status  *_gate.db
//          phy_1/     route.qor place.qor  *_phy.area  final_timing.rpt
//                     *_pr.timing  *_pr.db  *.bit  route_flow.status  place_flow.status
//          best_result/  *.bit
//
//  ⚠ 全部探测都必须容忍"目录不存在 / 权限不足 / 路径里有中文"：
//     全部走 error_code 重载，任何一步失败都只是"没找到"，不抛异常。
// =============================================================================

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "jsonrpc/json_util.h"
#include "project/al_project.h"

namespace fpgalsp::reports {

// 一个产物文件的状态（对应 Python _stat）
struct ArtifactFile {
    std::string path;
    bool exists = false;
    // size / mtime 只在 exists 时给出：Python 的 `{"path": "", "exists": False}`
    // 里没有这两个键，JSON 必须一致。
    std::optional<int64_t> size;
    std::optional<double> mtime;  // epoch 秒（浮点，与 os.stat().st_mtime 同值）

    [[nodiscard]] jsonrpc::Json ToJson() const;
};

// <name>_Runs 下的一个 run 目录
struct RunEntry {
    std::string name;
    std::string dir;
    bool exists = true;

    [[nodiscard]] jsonrpc::Json ToJson() const;
};

struct ArtifactsReport {
    std::vector<RunEntry> runs;
    ArtifactFile bit;
    ArtifactFile syn_db;    // 综合阶段 db（*_gate.db）
    ArtifactFile route_db;  // 布局布线 db（*_pr.db）
    // _Runs 目录是否存在；false 时上层给 ok=false + error
    bool runs_found = false;
    std::string runs_dir;

    [[nodiscard]] jsonrpc::Json ToJson() const;
};

// -----------------------------------------------------------------------------
// 文件系统工具（全部 never-throw，可单测）
// -----------------------------------------------------------------------------

// 等价 os.path.join：按 os.sep（Windows 上是反斜杠）拼接，不做规范化。
// 返回的路径可能是 "Y:/a/b\\c\\d" 这样的混合形态 —— 这是 Python 版的实际输出，
// 见 PYTHON-BUGS P-13。不要"顺手修成正斜杠"，否则 JSON 字段名对不上。
std::string JoinPath(std::string_view dir, std::string_view name);

// 仅供测试/断言：把反斜杠统一成正斜杠
std::string ToForwardSlashes(std::string text);

// 等价 Python _runs_dir：先找 <dir>/<name>_Runs，没有再找 <dir> 下第一个 "*_Runs" 目录。
// 都不存在时返回拼出来的候选路径（调用方用 IsDirectory 判定）。
std::string RunsDirectory(std::string_view project_dir, std::string_view project_name);

// 等价 Python _glob：目录里**文件名**能被 pattern 整串匹配（re2）的文件的完整路径，已排序。
std::vector<std::string> GlobReportFiles(std::string_view dir, std::string_view pattern);

// 目录下所有子目录名（已排序）
std::vector<std::string> ListSubdirectories(std::string_view dir);

// 等价 Python _first_existing：返回第一个 isfile 的路径；都不在返回空串。
std::string FirstExisting(const std::vector<std::string>& candidates);

bool IsFile(std::string_view path);
bool IsDirectory(std::string_view path);

// 等价 Python _stat
ArtifactFile StatFile(std::string_view path);

// 探测某个工程的全部产物（对应 collect() 里 artifacts 那段）
ArtifactsReport CollectArtifacts(const project::Project& project);

}  // namespace fpgalsp::reports