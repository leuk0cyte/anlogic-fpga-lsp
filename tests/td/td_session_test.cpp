// =============================================================================
//  tests/td/td_session_test.cpp —— 真实 TD 集成测试
// =============================================================================
//  这些测试会**真的启动 td_commands_prompt.exe**，因此用 LABEL "td" 隔离，
//  默认 `ctest` 不跑，`ctest -L td` 才跑。
//
//  按 lead 要求：
//   * 进程残留检查覆盖**所有 td 相关进程总数**，不只查一个名字
//   * 超时测试**先确认命令确实还在运行**再断言超时触发，避免假通过
// =============================================================================

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "project/al_project.h"
#include "td/td_install.h"
#include "td/td_output.h"
#include "td/td_session.h"

namespace {

using namespace fpgalsp::td;

// 所有与 TD 相关的可执行文件名（Job Object 清理要覆盖全部）
const char* kTdRelatedExes[] = {"td_commands_prompt.exe", "td_commands.exe", "td.exe",
                                "td_client.exe"};

int TotalTdProcesses() {
    int total = 0;
    for (const char* name : kTdRelatedExes) {
        const int n = CountProcessesByName(name);
        if (n > 0) total += n;
    }
    return total;
}

class TdSessionTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto choice = FindTd();
        if (!choice.has_value()) {
            GTEST_SKIP() << "本机没有可用的 TD 安装";
        }
        td_ = *choice;

        // 每个用例开始前确保没有上个用例遗留的 TD 进程。
        // 超时类用例会故意让 TD 处于"被 kill"的状态，若不做这一步，
        // 用 TotalTdProcesses() 做的残留断言会互相污染（实测踩过）。
        WaitForNoTdProcesses(5000);

        // 每次用独立的临时工作目录，避免 TD 之间互相干扰
        work_dir_ = (std::filesystem::temp_directory_path() /
                     ("fpgalsp_td_test_" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
                        .string();
        std::filesystem::create_directories(work_dir_);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(work_dir_, ec);
        WaitForNoTdProcesses(10000);
    }

    // 等待所有 TD 相关进程退出；超时则强制清理并返回 false
    static bool WaitForNoTdProcesses(int timeout_ms) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (TotalTdProcesses() == 0) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        // 保底：强制清理，避免一个坏用例拖垮整个套件
        for (const char* name : kTdRelatedExes) {
            const int n = CountProcessesByName(name);
            if (n > 0) {
                std::string cmd = "taskkill /F /IM ";
                cmd += name;
                cmd += " >nul 2>&1";
                std::system(cmd.c_str());
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return TotalTdProcesses() == 0;
    }

    TdSessionOptions Options() const {
        TdSessionOptions o;
        o.work_dir = work_dir_;
        o.startup_timeout_seconds = 60.0;
        return o;
    }

    TdChoice td_;
    std::string work_dir_;
};

// -----------------------------------------------------------------------------
// 基本生命周期
// -----------------------------------------------------------------------------

TEST_F(TdSessionTest, StartsAndReportsBanner) {
    TdSession session(td_, Options());
    EXPECT_TRUE(session.Alive());
    EXPECT_FALSE(session.banner().empty());

    // 真实横幅里应含 TD 版本信息
    EXPECT_NE(session.banner().find("TD Release"), std::string::npos)
        << "banner=" << session.banner().substr(0, 200);
}

TEST_F(TdSessionTest, ExecutesVersionCommand) {
    TdSession session(td_, Options());
    const std::string out = session.Run("version", 60.0);
    // TD 对 version 会回显版本号
    EXPECT_FALSE(out.empty());
}

TEST_F(TdSessionTest, ExecutesTclExpression) {
    TdSession session(td_, Options());
    const std::string out = session.Run("puts [expr {6*7}]", 60.0);
    const auto lines = CleanLines(out);
    bool found = false;
    for (const auto& l : lines) {
        if (l == "42") found = true;
    }
    EXPECT_TRUE(found) << "lines: " << [&] {
        std::string s;
        for (const auto& l : lines) s += "[" + l + "]";
        return s;
    }();
}

TEST_F(TdSessionTest, ErrorInOneCommandDoesNotTruncateNext) {
    // 实测事实 3：交互模式下一条命令报错不会截断后续命令
    //
    // ⚠ 注意：TD 对**完全未知**的命令只吐出 Windows shell 的
    //   "'xxx' is not recognized as an internal or external command"
    //   这类文本，**没有 <CODE> ERROR 前缀**，所以 HasError() 返回 false
    //   （Python 版同样如此，这不是 bug）。
    //   要产生真正的 <CODE> ERROR，必须用一个 TD 认识但参数非法的命令。
    //   实测：import_device 一个不存在的设备库会给出
    //          USR-8001 ERROR: Invalid device_db name
    TdSession session(td_, Options());

    const std::string bad = session.Run("import_device no_such_db_xyz", 90.0);
    EXPECT_TRUE(HasError(bad)) << "bad=" << bad;

    // 后续命令仍然可用
    const std::string good = session.Run("puts [expr {1+1}]", 60.0);
    const auto lines = CleanLines(good);
    bool found = false;
    for (const auto& l : lines) {
        if (l == "2") found = true;
    }
    EXPECT_TRUE(found);
}

TEST_F(TdSessionTest, UnknownCommandStillAllowsNextCommand) {
    // 未知命令虽然不产生 ERROR 标记，但同样不应截断后续命令
    TdSession session(td_, Options());

    const std::string out = session.Run("this_command_does_not_exist_xyz", 60.0);
    EXPECT_FALSE(out.empty());  // 有 Windows shell 的错误文本
    EXPECT_FALSE(HasError(out)) << "未知命令不会产生 <CODE> ERROR（实测）";

    const std::string good = session.Run("puts [expr {3*3}]", 60.0);
    const auto lines = CleanLines(good);
    bool found = false;
    for (const auto& l : lines) {
        if (l == "9") found = true;
    }
    EXPECT_TRUE(found);
}

TEST_F(TdSessionTest, CloseLeavesNoProcess) {
    {
        TdSession session(td_, Options());
        session.Run("puts hi", 60.0);
    }  // 析构 -> Close -> Job Object 关闭
    // 轮询等待回收
    for (int i = 0; i < 100 && TotalTdProcesses() > 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    EXPECT_EQ(TotalTdProcesses(), 0) << "Close 后仍有 TD 进程残留";
}

TEST_F(TdSessionTest, KillLeavesNoProcess) {
    TdSession session(td_, Options());
    ASSERT_TRUE(session.Alive());
    session.Kill();

    for (int i = 0; i < 100 && TotalTdProcesses() > 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    EXPECT_EQ(TotalTdProcesses(), 0) << "Kill 后仍有 TD 进程残留（Job Object 应清理整棵进程树）";
}

// -----------------------------------------------------------------------------
// 超时（防假通过：先确认命令确实在跑）
// -----------------------------------------------------------------------------

TEST_F(TdSessionTest, TimeoutKillsSessionAndCleansUp) {
    TdSession session(td_, Options());
    ASSERT_TRUE(session.Alive());

    // 用一个会阻塞很久的 Tcl 命令（after 是 Tcl 内置，单位毫秒）
    // 先确认进程在跑 —— 避免命令被秒拒导致"假通过"
    ASSERT_TRUE(session.Alive());

    const auto t0 = std::chrono::steady_clock::now();
    bool threw = false;
    try {
        // 2 秒超时 + 30 秒命令
        session.Run("after 30000", 2.0);
    } catch (const TdError& exc) {
        threw = true;
        const std::string msg = exc.what();
        EXPECT_NE(msg.find("timed out"), std::string::npos) << "msg=" << msg;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();

    EXPECT_TRUE(threw) << "长命令应在 2s 超时后抛 TdError";
    // 允许一定抖动，但必须明显小于命令本身时长
    EXPECT_GE(elapsed, 1500) << "超时过早，可能是假通过";
    EXPECT_LT(elapsed, 15000) << "超时未生效（命令几乎跑完了）";

    // 超时后会话必须已关闭
    EXPECT_FALSE(session.Alive());

    for (int i = 0; i < 100 && TotalTdProcesses() > 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    EXPECT_EQ(TotalTdProcesses(), 0) << "超时后仍有 TD 进程残留";
}

// -----------------------------------------------------------------------------
// 真实工程的 read_hdl
// -----------------------------------------------------------------------------

TEST_F(TdSessionTest, ReadHdlOnRealProject) {
    const std::string al_dir = std::string(FPGA_LSP_SOURCE_DIR) +
                               "/tests/gate-corpus/corpus_alpha";
    const std::string al_path = al_dir + "/demo_project.al";
    if (!std::filesystem::exists(al_path)) {
        GTEST_SKIP() << "语料不存在: " << al_path;
    }

    const auto project = fpgalsp::project::ParseAl(al_path);
    ASSERT_FALSE(project.files.empty());

    std::vector<std::string> files;
    for (const auto& f : project.files) {
        if (f.kind == fpgalsp::project::FileKind::Verilog && f.exists) {
            files.push_back(f.path);
        }
    }
    ASSERT_FALSE(files.empty()) << "工程里没有可用的 verilog 文件";

    TdSession session(td_, Options());

    std::string file_list;
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (i) file_list += " ";
        file_list += files[i];
    }
    const std::string tcl =
        "read_hdl -file {" + file_list + "} -top " + (project.top.empty() ? "top" : project.top);

    // ⚠ read_hdl 是**本会话的最后一条命令**。
    //
    // 实测（Python 版同样如此，见 PYTHON-PARITY.md §5.3）：
    //   read_hdl 之后 TD 的 stdout 管道会失效——
    //   __E 哨兵收不到，后续写入报 "Invalid argument"，但进程本身还活着。
    // 因此 Python 版从不复用执行过 read_hdl 的会话：
    //   server.py 每次新建 TdSession；flow.py 用 `with` 每流程一个会话。
    // 这里遵守同样的约定：拿完输出就关会话。
    const std::string out = session.Run(tcl, 300.0);

    // 真实输出里应至少出现 "analyze verilog file"
    const auto lines = CleanLines(out);
    bool saw_analyze = false;
    for (const auto& l : lines) {
        if (l.find("analyze verilog file") != std::string::npos) saw_analyze = true;
    }
    EXPECT_TRUE(saw_analyze) << "read_hdl 输出里没有 analyze verilog file 行";

    // 解析出的消息里应有带文件行号的条目
    const auto msgs = ParseMessageLines(out);
    bool saw_with_location = false;
    int warning_count = 0;
    for (const auto& m : msgs) {
        if (m.severity == kSeverityWarning) ++warning_count;
        if (m.path.has_value() && m.line.has_value()) {
            saw_with_location = true;
            // 路径必须是正斜杠
            EXPECT_EQ(m.path->find('\\'), std::string::npos);
        }
    }
    EXPECT_TRUE(saw_with_location) << "没有解析出任何带文件+行号的诊断";

    // 实测：20 文件工程里 HDL-1007 全是 INFO，真正的 warning 很少
    // （本工程是 HDL-7144 instantiate unknown module pll_gen）
    EXPECT_GE(warning_count, 1) << "应至少有一条真实 warning";
    EXPECT_LE(warning_count, 5) << "warning 数量异常偏多，可能把 INFO 误判成了 warning";

    // 用完立即关闭（遵守"read_hdl 是会话最后一条命令"的约定）
    session.Kill();
}

// -----------------------------------------------------------------------------
// 全局残留兜底（最后一个测试）
// -----------------------------------------------------------------------------

TEST(TdSessionGlobal, NoResidualProcessesAfterAllTests) {
    for (int i = 0; i < 100 && TotalTdProcesses() > 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    EXPECT_EQ(TotalTdProcesses(), 0) << "测试结束后仍有 TD 进程残留";
}

}  // namespace