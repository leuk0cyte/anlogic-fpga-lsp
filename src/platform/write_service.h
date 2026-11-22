// =============================================================================
//  src/platform/write_service.h —— 统一写入服务（backend 唯一写文件入口）
// =============================================================================
//  为什么需要（2026-11-22 用户反馈）：
//    写缓存 / 写日志 / 改工程文件的代码散落在 flow / probe / analysis / server / mcp / td，
//    每处自己拼路径、自己决定是否原子写。以后要改"写哪、怎么写、写坏了怎么办"就会漏改
//    （工程根反复长出 `.fpgalsp` 就是这么来的：一处 `project.dir + "/.fpgalsp/run"`）。
//
//  设计（详见 doc/design/write-service.md）：
//    * **一个入口**：所有文件写入走 `writes::Write` / `writes::AppendLine`；
//    * **四类 root**（按目标路径自动分类）：
//        kState   —— `platform::state_dir(project.dir)`（%TEMP%\fpgalsp\<名>-<hash8>）
//        kProject —— 工程目录，仅允许 `.al/.adc/.sdc` 与 `chip_debugger/*.mdn`
//        kRun     —— `<prj>_Runs/<run>/`（TD 产物；父目录必须已存在）
//        kTemp    —— 系统临时目录下含 fpgalsp 的 scratch
//      其它路径（含任何 `.fpgalsp` 段）一律拒绝，并给出可诊断的 error；
//    * **原子写**：同目录 `.tmp-<pid>-<seq>` → flush → rename（重试 3 次）；
//    * **审计**：`<state>/logs/writes.jsonl` 追加（`FPGALSP_WRITE_AUDIT=0` 可关）；
//    * **dry-run**：只校验路径/内容，不落盘（给 agent 的"预演"用）；
//    * **never-throw**：全部返回 `Result`，异常在内部转成 `ok=false`。
//
//  门禁：`tools/audit/check_raw_file_writes.py`（CTest `docs_no_raw_writes`）会扫描
//  `src/`，只允许本文件 / `platform/encoding.cpp` / `platform/log.cpp` 直接调
//  `write_file_utf8`、`std::ofstream`；新增写入必须经过这里。
// =============================================================================

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace fpgalsp::platform::writes {

// 写入目标分类（决定允许的路径范围）。
enum class Root {
    kState,    // 状态目录：后端可自由读写
    kProject,  // 工程目录：只允许登记过的用户文件
    kRun,      // TD 的 run 目录：只允许已存在的父目录
    kTemp,     // 系统临时目录（fpgalsp scratch）
};

// 写入选项。默认：不 dry-run、不备份、无标签。
struct Options {
    bool dry_run = false;      // true = 只校验，不落盘
    bool backup = false;       // true = 覆盖前把旧内容备份到 <state>/backup/
    bool force = false;        // Remove/Rename：允许递归/覆盖（默认严格）
    const char* caller = nullptr;  // 审计标签，例如 "probe.config"
};

struct Result {
    bool ok = false;
    Root root = Root::kState;
    std::string path;   // 归一化后的绝对路径（dry_run 时为"将会写"的路径）
    std::string error;  // 失败原因（策略拒绝 / IO 失败）
};

// 把 `path` 分类到四类 root。失败时 *out 不变、*error 写入原因。
// 规则（大小写不敏感比较、统一正斜杠）：
//   * 含 `.fpgalsp` 路径段 -> 拒绝（工程目录里的 .fpgalsp 已废弃）；
//   * 在 state_dir(project_dir) 之下 -> kState；
//   * 在 <prj>_Runs/ 之下 -> kRun（允许创建 run 目录，但只限这个前缀之下）；
//   * 在工程目录之下且扩展名 ∈ {.al,.adc,.sdc} 或 chip_debugger/*.mdn|*.cfg -> kProject；
//   * 在系统临时目录之下且路径含 "fpgalsp" -> kTemp；
//   * 其它 -> 拒绝。
bool Classify(std::string_view project_dir, std::string_view path, Root* out,
              std::string* error);

// **唯一写入入口**。相对路径会按工程目录解析（绝对路径直接用）。
// 成功返回 ok=true 与最终绝对路径；失败返回 ok=false + error。
Result Write(std::string_view project_dir, std::string_view path, std::string_view bytes,
             const Options& options = {});

// 追加一行（自动补 '\n'；只允许 kState/kTemp/kRun 下的日志/状态文件）。
Result AppendLine(std::string_view project_dir, std::string_view path, std::string_view line,
                  const Options& options = {});

// 便捷：<state_dir(project_dir)>/<rel>（不校验 rel，只做拼接；写入仍要过 Write）。
std::string StatePath(std::string_view project_dir, std::string_view rel);

// 便捷：把绝对路径转成"相对工程目录"的审计显示名（越界时返回原路径）。
std::string DisplayPath(std::string_view project_dir, std::string_view path);

// -----------------------------------------------------------------------------
// 目录 / 删除 / 改名（与 Write 同一套 root 策略；0.25.0 起业务代码只能用这些）
// -----------------------------------------------------------------------------
// EnsureDir：kState/kTemp/kRun 可建；kProject 只允许「已存在」。
Result EnsureDir(std::string_view project_dir, std::string_view path,
                 const Options& options = {});

// Remove：只允许 kState/kTemp/kRun；kProject 一律拒绝（不替用户删工程文件）。
// recursive=true 等价 remove_all。
Result Remove(std::string_view project_dir, std::string_view path, bool recursive = false,
              const Options& options = {});

// Rename：from/to 必须在同一 root，且都不能是 kProject；目标已存在时需 force=true。
Result Rename(std::string_view project_dir, std::string_view from, std::string_view to,
              const Options& options = {});

// -----------------------------------------------------------------------------
// 写指标（fpgalsp.metrics 用；进程内累计，永不抛）
// -----------------------------------------------------------------------------
struct Metrics {
    std::uint64_t writes_ok = 0;
    std::uint64_t writes_failed = 0;
    std::uint64_t denied = 0;       // 被 root 策略拒绝的次数
    std::uint64_t bytes_written = 0;
    std::uint64_t dirs_created = 0;
    std::uint64_t removals = 0;
    std::uint64_t renames = 0;
    std::uint64_t backups = 0;
    std::uint64_t by_root[4] = {0, 0, 0, 0};  // state/project/run/temp
};
Metrics SnapshotMetrics();
void ResetMetricsForTest();

// -----------------------------------------------------------------------------
// 测试用后端（内存 FS）：SetBackendForTest 之后所有 Write/EnsureDir/Remove/Rename
// 都走内存，不落盘；ResetBackendForTest 恢复真实磁盘。用于单测（避免污染临时目录）。
// -----------------------------------------------------------------------------
class Backend {
  public:
    virtual ~Backend() = default;
    virtual bool WriteFile(const std::string& path, std::string_view bytes) = 0;
    virtual bool AppendFile(const std::string& path, std::string_view bytes) = 0;
    virtual bool EnsureDir(const std::string& path) = 0;
    virtual bool Remove(const std::string& path, bool recursive) = 0;
    virtual bool Rename(const std::string& from, const std::string& to) = 0;
    virtual bool Exists(const std::string& path) = 0;
};
std::unique_ptr<Backend> MakeMemoryBackend();
void SetBackendForTest(std::unique_ptr<Backend> backend);
void ResetBackendForTest();

}  // namespace fpgalsp::platform::writes
