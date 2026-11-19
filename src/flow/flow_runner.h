// =============================================================================
//  src/flow/flow_runner.h —— Flow 引擎（一键综合 / 布局布线 / 增量布线）
// =============================================================================
//  对应参考实现：Y:\fpga-lsp\fpgalsp\flow.py::FlowRunner
//
//  ---- TD 命令序列（实测 TD 2026.2，逐条复现，不要改顺序）----
//      import_device <db> -package <pkg>       # 必须先于 open_project，否则 PRJ-8001
//      open_project {<abs>/<name>.al}
//      reset_runs { syn_1 };  launch_runs { syn_1 } -jobs N;  wait_run syn_1 -quiet
//      reset_runs { phy_1 };  launch_runs { phy_1 } -jobs N;  wait_run phy_1 -quiet
//      save_best_bits                          # -> best_result/*.bit
//  增量布线（route）：复用 <phy_1>/*_place.db，只跑 route + fix_hold + update_timing + bitgen
//
//  ---- 硬约束 ----
//  1. **同一时刻只允许一个 flow**：Start 在忙时返回 {"ok":false,"error":"busy"}。
//  2. **flow 跑在独立 TD 会话里**，不占用诊断会话（诊断可能正在 read_hdl）。
//  3. **取消/超时/失败后不能有残留 td_commands_prompt.exe**：
//     TdSession 的 Kill 走 Job Object，终止整棵进程树（Python 版会留孤儿，见 P-5）。
//  4. **PRJ-8806/PRJ-5303 容错的三个条件必须同时满足**（见 flow_logic.h）。
//  5. 公开方法全部线程安全（内部 mutex）。
//  6. 公开 API 不抛异常：所有失败都体现在返回的 JSON / FlowState.error 里。
// =============================================================================

#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "jsonrpc/json_util.h"
#include "project/al_project.h"
#include "td/td_install.h"

namespace fpgalsp::flow {

// -----------------------------------------------------------------------------
// 会话抽象：真实实现包 td::TdSession，单测注入假实现
// -----------------------------------------------------------------------------
//  为什么抽象：flow 的正确性几乎全在"判定"上（PRJ-8806 三条件、完成标记、
//  stall、失败详情），这些必须能在毫秒级单测里跑，不能要求真实 TD 进程。
class FlowSession {
public:
    virtual ~FlowSession() = default;
    // 执行一条 Tcl；超时/进程死亡时返回空串并让 LastError() 给出原因
    // （**不抛异常**：worker 跑在独立线程里，异常逃出去会 terminate 整个进程）。
    virtual std::string Run(std::string_view tcl, double timeout_seconds) = 0;
    virtual void Kill() = 0;
    virtual void Close() = 0;
    // 最近一次失败的原因（空 = 没有失败）
    virtual std::string LastError() const { return {}; }
};

// work_dir 是会话的 cwd（TD 自己的日志会写在这里）
using FlowSessionFactory = std::function<std::unique_ptr<FlowSession>(const std::string& work_dir)>;

struct FlowOptions {
    // 单条命令的默认上限（对应 Python timeout_s=3600）
    double timeout_seconds = 3600.0;
    // 日志长时间无新输出判为卡死（FPGALSP_FLOW_STALL_SECONDS，默认 600）
    double stall_seconds = 600.0;
    // 心跳线程采样周期；<=0 表示不起心跳线程（单测关掉，避免偶发时序）
    double heartbeat_seconds = 1.5;
    // 等 run 真正开工的上限（Python 45 s）
    double start_timeout_seconds = 45.0;
    // 取消时先写 .stop.f 让 TD 自己收尾，超过这个秒数才强杀进程树。
    // 0 = 立刻杀（单测里的假会话用这个）。
    // 为什么要留宽限：TD 的 Tcl 读取是阻塞的，强杀会让 td::TdSession 在
    // 无 /EHsc 展开语义下抛异常 -> std::terminate（见 PYTHON-BUGS P-16）。
    double cancel_kill_grace_seconds = 10.0;
    // 运行日志根目录，默认 <project>/.fpgalsp/flow/<runId>/
    // ⚠ 默认值**与 Python 一致**（会写进工程目录，见 PYTHON-BUGS P-4），
    //   但这里是可配置项：把它指到别处即可完全不污染工程。
    std::string flow_root;
    // 缓存目录，默认 <project>/.fpgalsp/cache
    std::string cache_root;
    // 是否落盘日志；false 时只走回调（不写工程目录）
    bool write_run_logs = true;
    // 显式指定的 db 文件名；空 = 按 DbCandidates 自动挑
    std::string explicit_db;
    // 空 = 用真实 td::TdSession
    FlowSessionFactory session_factory;
};

// 对应 Python FlowRunner._state 的对外视图
struct FlowState {
    bool running = false;
    std::string stage = "idle";  // idle/starting/syn/phy/route/done/error/cancelled
    std::vector<std::string> stages;
    std::string run_id;
    double started_at = 0.0;
    double elapsed = 0.0;
    std::string last_message;
    std::optional<bool> ok;
    std::string error;
    std::vector<std::string> log;  // 最后 200 行
    // P-C4：累计写入的日志行数（单调递增，不随裁剪回退）。
    // 客户端用 sinceSeq 只取新增行：log[i] 的序号 = log_seq - (log.size() - i)。
    std::uint64_t log_seq = 0;
    int jobs = 6;

    jsonrpc::Json ToJson() const;
};

class FlowRunner {
public:
    // 推 fpgaLsp/flowEvent（对应 LspServer::SendFlowEvent）
    using NotifyFn = std::function<void(const jsonrpc::Json&)>;
    // 日志回调：(LSP MessageType, 文本)。MessageType 3 = Info。
    using LogFn = std::function<void(int, std::string_view)>;

    FlowRunner(project::Project project, td::TdChoice td, NotifyFn notify, LogFn log,
               FlowOptions options = {});
    ~FlowRunner();

    FlowRunner(const FlowRunner&) = delete;
    FlowRunner& operator=(const FlowRunner&) = delete;

    // 启动一次 flow。
    //   成功 -> {"ok":true,"runId":"flow_xxxxxxxx","stages":["syn"]}
    //   忙   -> {"ok":false,"error":"busy","runId":"<当前>"}
    //   阶段全不认识 -> {"ok":false,"error":"unknown stage; use syn / phy / all"}
    // 立即返回，实际工作在线程里。
    jsonrpc::Json Start(const std::vector<std::string>& stages, int jobs = 6);

    // 请求取消：写 .stop.f + 终止 TD 进程树。没在跑时返回 {"ok":true,"note":"not running"}。
    jsonrpc::Json Cancel();

    // 线程安全地取当前状态（elapsed 会按当前时刻重算）
    FlowState Status() const;

    const project::Project& project() const { return project_; }
    const FlowOptions& options() const { return options_; }

    // 等 flow 结束（测试 / 关停用）。返回 true 表示已空闲。
    bool WaitUntilIdle(double timeout_seconds);

    // 给测试用的可注入时钟（默认 steady/system 真实时间）。返回 epoch 秒。
    void SetClockForTest(std::function<double()> clock);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    project::Project project_;
    td::TdChoice td_;
    FlowOptions options_;
};

// 便捷构造：从环境变量取 stall 秒数（对应 Python os.environ["FPGALSP_FLOW_STALL_SECONDS"]）
double StallSecondsFromEnv(double fallback = 600.0);

// 工作目录：<project>/.fpgalsp/flow（可被 FlowOptions::flow_root 覆盖）
std::string DefaultFlowRoot(const project::Project& project);
std::string DefaultCacheRoot(const project::Project& project);

}  // namespace fpgalsp::flow