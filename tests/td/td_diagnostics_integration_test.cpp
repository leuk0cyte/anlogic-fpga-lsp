// =============================================================================
//  tests/td/td_diagnostics_integration_test.cpp
// =============================================================================
//  真实 TD 全链路：ensure_device -> read_hdl -> Diagnostic 列表
//  用 LABEL "td" 隔离（需真实 TD，慢）。
// =============================================================================

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "lsp/diagnostic.h"
#include "project/al_project.h"
#include "td/td_diagnostics.h"
#include "td/td_install.h"
#include "td/td_service.h"
#include "td/td_session.h"

namespace {

using namespace fpgalsp;
using namespace fpgalsp::td;

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

bool WaitForNoTdProcesses(int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (TotalTdProcesses() == 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    for (const char* name : kTdRelatedExes) {
        if (CountProcessesByName(name) > 0) {
            std::string cmd = "taskkill /F /IM ";
            cmd += name;
            cmd += " >nul 2>&1";
            std::system(cmd.c_str());
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return TotalTdProcesses() == 0;
}

class TdDiagnosticsIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto choice = FindTd();
        if (!choice.has_value()) {
            GTEST_SKIP() << "本机没有可用的 TD 安装";
        }
        td_ = *choice;

        const std::string al = std::string(FPGA_LSP_SOURCE_DIR) +
                               "/tests/gate-corpus/corpus_alpha/demo_project.al";
        if (!std::filesystem::exists(al)) {
            GTEST_SKIP() << "语料不存在: " << al;
        }
        project_ = project::ParseAl(al);

        WaitForNoTdProcesses(5000);

        work_dir_ = (std::filesystem::temp_directory_path() /
                     ("fpgalsp_tddiag_" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
                        .string();
        std::filesystem::create_directories(work_dir_);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(work_dir_, ec);
        WaitForNoTdProcesses(10000);
    }

    TdSessionOptions Options() const {
        TdSessionOptions o;
        o.work_dir = work_dir_;
        o.startup_timeout_seconds = 60.0;
        return o;
    }

    TdChoice td_;
    project::Project project_;
    std::string work_dir_;
};

// -----------------------------------------------------------------------------
// ensure_device
// -----------------------------------------------------------------------------

TEST_F(TdDiagnosticsIntegrationTest, EnsureDeviceFindsEf2) {
    TdSession session(td_, Options());

    const std::string td_root = [&] {
        const std::string exe = td_.exe;
        const std::size_t bin = exe.rfind("/bin/");
        return bin == std::string::npos ? std::string() : exe.substr(0, bin);
    }();

    const auto result = EnsureDevice(project_, session, td_root);
    EXPECT_TRUE(result.ok) << "error=" << result.error;
    // 实测：EF2L45LG144B 需要 ef2_4.db（ef2_1/ef2_2 不支持该封装）
    EXPECT_EQ(result.db, "ef2_4.db");
    EXPECT_GE(result.tried, 1);
}

TEST_F(TdDiagnosticsIntegrationTest, EnsureDeviceReportsFailureWithoutThrowing) {
    auto broken = project_;
    broken.device = "NOSUCHDEVICE999";
    broken.family = "EF2";

    TdSession session(td_, Options());
    const std::string td_root = [&] {
        const std::string exe = td_.exe;
        const std::size_t bin = exe.rfind("/bin/");
        return bin == std::string::npos ? std::string() : exe.substr(0, bin);
    }();

    EnsureDeviceResult result;
    EXPECT_NO_THROW(result = EnsureDevice(broken, session, td_root));
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.error.empty());
}

// -----------------------------------------------------------------------------
// 全链路：RunReadHdl
// -----------------------------------------------------------------------------

TEST_F(TdDiagnosticsIntegrationTest, RunReadHdlProducesDiagnostics) {
    TdSession session(td_, Options());

    lsp::DiagnosticList diags;
    // never-raise 契约
    EXPECT_NO_THROW(diags = RunReadHdl(project_, session));

    ASSERT_FALSE(diags.empty()) << "read_hdl 应产出诊断";

    // ⚠ 实测基线（CorpusAlpha，20 个 HDL 文件 + 2 个 IP 生成 HDL）：
    //   40 条，**全部 HDL-1007 Info，零 warning**。
    //
    // 为什么不是"20 条 elaborate + warning"？—— 因为 Python 版有一个已知 bug：
    //   `ip_generated_hdl()` 把 PL 的 `.v` 和 `.vhd` **两个都**塞进 `read_hdl -file`，
    //   而该列表被当作 Verilog 编译。`.vhd` 混进去后 TD 报
    //       HDL-8007 ERROR: 'ef2_components' is not compiled in library 'ef2_macro'
    //       HDL-8007 ERROR: ignore unit pll_gen due to previous errors
    //   elaborate 阶段整体失败，于是：
    //       * 20 条 `elaborate module ...` 全部消失
    //       * pll_gen 不再是未知模块，`HDL-7144` 与 `black box` 警告也消失
    //       * 剩下 21 条 `analyze verilog file`（无行号，被 ParseMessageLines 丢弃）
    //         + 15 条 `undeclared symbol pll_gen_openN`（带行号 → Info）
    //         + 若干 HDL-8007/1008/1009
    //   已实测确认 **Python 版在同一工程上同样产出 40 条全 Info**（零 warning），
    //   C++ 与 Python 逐字段一致。详见 PYTHON-PARITY.md §7.2。
    EXPECT_EQ(diags.size(), 40u);

    // 所有诊断的公共属性
    for (const auto& d : diags) {
        EXPECT_EQ(d.source, "fpga-lsp:td");
        EXPECT_EQ(d.range.start.character, 0) << "TD 无列号";
        EXPECT_EQ(d.range.end.character, 1);
        EXPECT_EQ(d.range.start.line, d.range.end.line) << "应为单行 range";
        EXPECT_GE(d.range.start.line, 0) << "不能出现负行号";
        EXPECT_TRUE(lsp::IsValidSeverity(d.severity));
    }

    // 该工程实测没有 warning/error（含上面所述的 elaborate 失败现象）
    int warnings = 0;
    int errors = 0;
    for (const auto& d : diags) {
        if (d.severity == lsp::kWarning) ++warnings;
        if (d.severity == lsp::kError) ++errors;
        EXPECT_EQ(d.code, "HDL-1007") << "该工程实测全部是 HDL-1007";
    }
    EXPECT_EQ(warnings, 0);
    EXPECT_EQ(errors, 0);

    // 带位置的诊断必须都能落到绝对路径
    int with_location = 0;
    for (const auto& d : diags) {
        if (d.data.is_object() && d.data.contains("path")) {
            const std::string path = d.data["path"].get<std::string>();
            EXPECT_EQ(path.find('\\'), std::string::npos) << path;
            if (d.range.start.line > 0 || path.find(".v") != std::string::npos) {
                ++with_location;
            }
        }
    }
    EXPECT_GT(with_location, 0) << "应解析出带文件位置的诊断";

    // 路径都是绝对正斜杠
    for (const auto& d : diags) {
        if (d.data.is_object() && d.data.contains("path")) {
            const std::string path = d.data["path"].get<std::string>();
            EXPECT_EQ(path.find('\\'), std::string::npos) << path;
            EXPECT_TRUE(path.find(':') != std::string::npos) << "应为绝对路径: " << path;
        }
    }
}

TEST_F(TdDiagnosticsIntegrationTest, RunReadHdlNoResidualProcesses) {
    {
        TdSession session(td_, Options());
        RunReadHdl(project_, session);
        // read_hdl 之后会话不可再用，析构会走 Job Object 清理
    }
    EXPECT_TRUE(WaitForNoTdProcesses(15000)) << "read_hdl 后不应有 TD 进程残留";
}

// -----------------------------------------------------------------------------
// 无 HDL 文件的工程：不启动 read_hdl
// -----------------------------------------------------------------------------

TEST_F(TdDiagnosticsIntegrationTest, EmptyProjectReturnsNoDiagnostics) {
    project::Project empty;
    empty.dir = work_dir_;
    empty.al_path = work_dir_ + "/empty.al";
    empty.device = "EF2L45LG144B";
    empty.family = "EF2";

    TdSession session(td_, Options());
    lsp::DiagnosticList diags;
    EXPECT_NO_THROW(diags = RunReadHdl(empty, session));
    // 对应 Python: if not files: return []
    EXPECT_TRUE(diags.empty());
}

// -----------------------------------------------------------------------------
// A5 / task-25：RunReadHdlCheck 的分段计时 + 按文件切分 + TdSessionRunner
// -----------------------------------------------------------------------------

TEST_F(TdDiagnosticsIntegrationTest, CheckResultCarriesTimingsAndPerFileSplit) {
    TdSession session(td_, Options());
    const TdCheckResult result = RunReadHdlCheck(project_, session, {});
    session.Kill();

    ASSERT_FALSE(result.cancelled);
    ASSERT_FALSE(result.failed) << result.error;
    EXPECT_FALSE(result.all.empty());
    EXPECT_EQ(result.all.size(), result.total());

    // 分段必须都真的量到（>0），且 total 覆盖各段之和
    // 注意：直接调用 RunReadHdlCheck 时 startup_ms 恒为 0 —— 会话是调用方建的，
    // 只有 TdSessionRunner 才知道 startup（见下面两个用例）。
    EXPECT_DOUBLE_EQ(result.timings.startup_ms, 0.0);
    EXPECT_GT(result.timings.import_ms, 0.0);
    EXPECT_GT(result.timings.read_hdl_ms, 0.0);
    EXPECT_GE(result.timings.parse_ms, 0.0);
    EXPECT_GT(result.timings.total_ms, result.timings.read_hdl_ms);
    EXPECT_LE(result.timings.accounted_ms(), result.timings.total_ms + 1.0)
        << "分段之和不应超过 total（允许 1 ms 的打点误差）";

    // 按文件切分：所有 key 都是绝对路径，且分组后的条数等于全量条数
    std::size_t grouped = 0;
    for (const auto& [path, list] : result.by_file) {
        EXPECT_FALSE(path.empty());
        EXPECT_FALSE(list.empty());
        grouped += list.size();
    }
    EXPECT_EQ(grouped, result.total());
}

TEST_F(TdDiagnosticsIntegrationTest, SessionRunnerUsesProjectWorkDirAndFillsStartup) {
    // use_project_work_dir=true（默认）= 与 lsp_server::CreateSession 的 RunDir(project) 一致
    TdSessionRunner runner(td_, Options(), /*use_project_work_dir=*/true);
    const CancelToken no_cancel;
    const TdCheckResult result = runner.Run(project_, no_cancel);

    ASSERT_FALSE(result.failed) << result.error;
    EXPECT_GT(result.timings.startup_ms, 0.0) << "startup 必须由 runner 记下来";
    EXPECT_GT(result.timings.total_ms, result.timings.startup_ms);
    EXPECT_NE(runner.last_work_dir().find("/.fpgalsp/run"), std::string::npos)
        << runner.last_work_dir();
}

TEST_F(TdDiagnosticsIntegrationTest, SessionRunnerCanUseScratchWorkDir) {
    // use_project_work_dir=false -> 落到 %TEMP% 的 scratch（B7/D-3），完全不碰工程
    TdSessionOptions options = Options();
    options.work_dir.clear();
    TdSessionRunner runner(td_, options, /*use_project_work_dir=*/false);
    const CancelToken no_cancel;
    const TdCheckResult result = runner.Run(project_, no_cancel);

    ASSERT_FALSE(result.failed) << result.error;
    EXPECT_NE(runner.last_work_dir().find("fpgalsp-td-"), std::string::npos)
        << runner.last_work_dir();
}

// -----------------------------------------------------------------------------
// 全局残留兜底
// -----------------------------------------------------------------------------

TEST(TdDiagnosticsGlobal, NoResidualProcesses) {
    EXPECT_TRUE(WaitForNoTdProcesses(15000));
}

}  // namespace