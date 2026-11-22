// =============================================================================
//  src/td/td_session.h —— 持久 TD Tcl shell
// =============================================================================
//  对应参考实现：Y:\fpga-lsp\fpgalsp\td.py :: TdSession
//
//  实测事实（TD 2026.2，本机）：
//   * 交互式 Tcl 8.6 shell，启动 0.2-0.4 s，提示符 "td > "
//   * 提示符会粘在输出行里**且出现多次**，必须靠哨兵而不是提示符判断命令结束
//   * 一条命令报错不截断后续命令（交互模式与 batch 不同）
//   * stderr 合并进 stdout（Python 版 stderr=STDOUT）
//   * TD 从不通过退出码表达失败 → 只能扫文本
//
//  ⚠ 相对 Python 版的改进（任务要求）：kill 时用 Windows **Job Object** 终止整个
//    进程树，而不是只 terminate 单个进程。行为契约保持一致（kill 后不留孤儿）。
// =============================================================================

#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

#include "td/td_install.h"

namespace fpgalsp::td {

// TD 层错误（对应 Python 的 TdError）。C++ 侧只在显式 API 上抛，解析层不抛。
class TdError : public std::runtime_error {
public:
    explicit TdError(const std::string& what) : std::runtime_error(what) {}
};

struct TdSessionOptions {
    double startup_timeout_seconds = 40.0;  // 对应 Python startup_timeout=40.0
    // cwd（会被创建）。**留空是安全的默认值**（B7 / D-3）：不再让 TD 继承本进程
    // 的 cwd（那会把 TD 的日志/中间文件撒到用户工作目录），而是每个会话一个
    // %TEMP%/fpgalsp-td-<pid>-<n> 的独立 scratch 目录。
    // 需要固定 cwd 的调用方（bridge / flow / td_service）应显式传状态目录
    // （platform::state_dir(project.dir)/run）；**不要**再传 <project>/.fpgalsp/run，
    // 工程目录里的 .fpgalsp 已废弃（2026-11-22 审计 + docs_no_project_state 门禁）。
    std::string work_dir;
    // TD 把每个会话的日志写成 `<cwd>/td_<YYYYMMDD_HHMMSS>.log`（还有 `.logw`），崩溃时
    // 还会在 cwd 里丢一个 `minidump-*.dmp`。而会话 cwd 必须是 `.al` 所在目录（与 GUI
    // 一致，相对路径才解析得对），于是这些"短会话日志"就落到了**工程根目录**。
    // GUI 自己把它们放在 `<al>/log/` 下，所以这里传 `<al>/log` 即可：
    // 会话结束（Kill/Close/Destroy）时，**只把本次会话新产生的**这些文件挪过去
    //（挪不动就复制+删除；跨卷也成立）。空 = 不挪（scratch cwd 的会话不需要）。
    std::string log_dir;
};

class TdSession {
public:
    // 启动 TD。失败抛 TdError（对应 Python 在 __init__ 里抛）。
    TdSession(const TdChoice& td, const TdSessionOptions& options);
    ~TdSession();

    TdSession(const TdSession&) = delete;
    TdSession& operator=(const TdSession&) = delete;

    // 启动时的横幅文本（对应 Python self.banner）
    [[nodiscard]] const std::string& banner() const { return banner_; }

    // 执行一条 Tcl 命令，返回**未经 clean_lines 处理**的原始文本。
    // 语义与 Python run() 一致：截取 __B/__E 之间，再全局去掉 "td > "。
    // 超时 -> kill 并抛 TdError。
    //
    // ┌───────────────────────────────────────────────────────────────────────┐
    // │ ⚠ 硬约束：read_hdl 必须是本会话的最后一条命令                          │
    // ├───────────────────────────────────────────────────────────────────────┤
    // │ TD 的 read_hdl 执行后会关闭 stdout 管道（实测：收到 __B 但收不到 __E， │
    // │ 进程仍存活，后续写入 Errno 22 / Invalid argument）。                    │
    // │                                                                       │
    // │ Python 版从不在同一会话里执行 read_hdl + 后续命令：                     │
    // │   server.py:856  每次新建 TdSession                                    │
    // │   flow.py:199    用 `with TdSession(...)` 每流程一个会话               │
    // │ 那不是偷懒 —— 是被 TD 的行为逼出来的。                                 │
    // │                                                                       │
    // │ 结论：任何在同一会话里 read_hdl 之后继续发命令的代码路径都是 bug。      │
    // │ 需要多次 read_hdl 时必须每次新建会话。                                 │
    // └───────────────────────────────────────────────────────────────────────┘
    std::string Run(std::string_view tcl, double timeout_seconds = 180.0);

    [[nodiscard]] bool Alive() const;
    void Kill();
    void Close();

    [[nodiscard]] const TdChoice& td() const { return td_; }

    // 进程句柄（测试/诊断用）
    [[nodiscard]] unsigned long ProcessId() const;

    // 本会话实际使用的 cwd（D-3：未显式指定时是 %TEMP%/fpgalsp-td-<pid>-<n>）。
    [[nodiscard]] const std::string& WorkDir() const;

    // 默认超时（对应 Python run(..., timeout=180.0)）
    static constexpr double kDefaultTimeout = 180.0;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    TdChoice td_;
    std::string banner_;

    // 读到 marker（含）为止，返回从头到 marker 结尾的内容（对应 Python _read_until）
    std::string ReadUntil(std::string_view marker, double timeout_seconds);
};

// 进程树清理工具：等待指定 exe 名的进程数归零（供测试断言使用）。
// 返回当前同名进程数量。
int CountProcessesByName(std::string_view exe_name);

}  // namespace fpgalsp::td