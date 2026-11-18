// =============================================================================
//  tests/td/flow_integration_test.cpp —— Flow 引擎的真实 TD 集成测试
// =============================================================================
//  用 tests/gate-corpus/corpus_alpha 的副本跑一次真实综合，验证：
//    * TD 命令序列能跑通（import_device -> open_project -> reset/launch/wait_run）
//    * flowStatus / reports 的字段完整
//    * 跑完 **0 个残留 td_commands_prompt.exe**
//    * 取消后同样 0 残留（Job Object 终止整棵进程树；Python 版会留孤儿，见 P-5）
//
//  ⚠ 会真的启动 TD（约 10-40 s），所以标记 LABEL "td"，默认 ctest 不跑。
//  ⚠ 工程复制到临时目录再跑：**不要改动 tests/gate-corpus 里的任何文件**
//     （`.al` 的 Path 属性只是元数据，见 PYTHON-BUGS P-7，TD 会按副本目录解析相对路径）。
// =============================================================================

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "flow/flow_logic.h"
#include "flow/flow_runner.h"
#include "platform/encoding.h"
#include "project/al_project.h"
#include "reports/report_text.h"
#include "reports/reports_aggregate.h"
#include "td/td_install.h"
#include "td/td_session.h"

namespace {

namespace fs = std::filesystem;
using fpgalsp::jsonrpc::Json;

std::string Slashes(std::string text) {
    for (char& c : text) {
        if (c == '\\') c = '/';
    }
    return text;
}

std::string CorpusDir() {
    return std::string(FPGA_LSP_SOURCE_DIR) + "/tests/gate-corpus/corpus_alpha";
}

class ScopedTempDir {
public:
    explicit ScopedTempDir(const std::string& tag) {
        path_ = Slashes(fs::temp_directory_path().string());
        if (!path_.empty() && path_.back() != '/') path_.push_back('/');
        path_ += "fpgalsp_flow_td_" + tag;
        std::error_code ec;
        fs::remove_all(fs::path(path_), ec);
        fs::create_directories(fs::path(path_), ec);
    }
    ~ScopedTempDir() {
        // 失败时保留现场，便于排查（成功时会被 ctest 的临时目录清理带走）
        if (!std::getenv("FPGALSP_FLOW_KEEP_WORK")) {
            std::error_code ec;
            fs::remove_all(fs::path(path_), ec);
        }
    }
    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const std::string& path() const { return path_; }

private:
    std::string path_;
};

// 复制语料（跳过 TD 产物与缓存，避免把上一个工程的 runs 带进来）
void CopyCorpus(const std::string& to, std::error_code& ec) {
    fs::create_directories(fs::path(to), ec);
    if (ec) return;
    const fs::path from(CorpusDir());
    for (fs::recursive_directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path relative = fs::relative(it->path(), from, ec);
        if (ec) return;
        const std::string name = it->path().filename().string();
        // 排除上一轮跑出来的产物与缓存：带着 *_Runs 复制过去会让 reset_runs 行为不确定
        if (name == ".fpgalsp") continue;
        if (name.size() >= 5 && name.compare(name.size() - 5, 5, "_Runs") == 0) continue;
        const fs::path target = fs::path(to) / relative;
        if (it->is_directory()) {
            fs::create_directories(target, ec);
            if (ec) return;
        } else if (it->is_regular_file()) {
            fs::create_directories(target.parent_path(), ec);
            std::error_code copy_ec;
            fs::copy_file(it->path(), target, fs::copy_options::overwrite_existing, copy_ec);
        }
    }
}

// P-7b：把 .al 根节点 Path 改写到临时目录（公用工具在 src/flow/flow_logic.cpp）。
// 不改写的话 TD 会把 *_Runs 写到 Path 指向的**原工程**里去。
int TdProcessCount() {
    return fpgalsp::td::CountProcessesByName("td_commands_prompt.exe");
}

struct Prepared {
    ScopedTempDir temp;
    fpgalsp::project::Project project;
    fpgalsp::td::TdChoice td;
    bool ready = false;
    std::string skip_reason;

    explicit Prepared(const std::string& tag) : temp("it_" + tag) {
        std::error_code ec;
        CopyCorpus(temp.path(), ec);
        if (ec) {
            skip_reason = "cannot copy the corpus: " + ec.message();
            return;
        }
        const std::string al = temp.path() + "/demo_project.al";
        if (!fs::is_regular_file(fs::path(al))) {
            skip_reason = "corpus is missing demo_project.al";
            return;
        }
        // P-7：把根节点 Path 改写到临时目录，否则 TD 会把 *_Runs 写回原工程
        fpgalsp::flow::RewriteProjectPath(al, temp.path());
        project = fpgalsp::project::ParseAl(al);
        const std::optional<fpgalsp::td::TdChoice> choice = fpgalsp::td::FindTd();
        if (!choice.has_value()) {
            skip_reason = "no TangDynasty installation found";
            return;
        }
        td = *choice;
        ready = true;
    }
};

// 等 TD 进程全部退出（kill 之后可能有极短的收尾窗口）
bool WaitForNoTdProcesses(double timeout_seconds) {
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(static_cast<long long>(timeout_seconds * 1000.0));
    while (std::chrono::steady_clock::now() < deadline) {
        if (TdProcessCount() == 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return TdProcessCount() == 0;
}

}  // namespace

// -----------------------------------------------------------------------------
// 真实综合
// -----------------------------------------------------------------------------

TEST(FlowTdIntegration, RunsASynthesisToCompletion) {
    Prepared prepared("syn");
    if (!prepared.ready) GTEST_SKIP() << prepared.skip_reason;

    // 开工前先把上一轮可能残留的 TD 等干净（进程退出有收尾窗口），再断言干净
    ASSERT_TRUE(WaitForNoTdProcesses(30.0)) << "测试开始前就有残留的 td_commands_prompt.exe";

    std::vector<std::string> log;
    fpgalsp::flow::FlowOptions options;
    options.timeout_seconds = 900.0;
    options.start_timeout_seconds = 120.0;
    options.heartbeat_seconds = 0.5;
    options.stall_seconds = 600.0;

    fpgalsp::flow::FlowRunner runner(prepared.project, prepared.td,
                                    [](const Json&) {},  // 不需要事件
                                    [&log](int, std::string_view text) { log.emplace_back(text); },
                                    options);

    const Json started = runner.Start({"syn"}, 4);
    ASSERT_TRUE(started.value("ok", false)) << started.dump();
    ASSERT_TRUE(runner.WaitUntilIdle(880.0)) << "综合没有在 880 s 内结束";

    const fpgalsp::flow::FlowState state = runner.Status();
    EXPECT_FALSE(state.running);
    EXPECT_EQ(state.stage, "done") << "error=" << state.error;
    ASSERT_TRUE(state.ok.has_value());
    EXPECT_TRUE(*state.ok) << "error=" << state.error;
    EXPECT_TRUE(state.error.empty());

    // 产物：syn_1 下必须有 .opt_gate.end.f 与 gate 报告
    const std::string runs = prepared.project.dir + "/" + prepared.project.name + "_Runs/syn_1";
    EXPECT_TRUE(fs::is_directory(fs::path(runs)));
    bool saw_end_marker = false;
    for (const auto& entry : fs::directory_iterator(fs::path(runs))) {
        const std::string name = entry.path().filename().string();
        if (name == ".opt_gate.end.f") saw_end_marker = true;
    }
    EXPECT_TRUE(saw_end_marker) << "缺 .opt_gate.end.f：综合没真正跑完";

    // reports 能读到综合阶段的报告（area/qor 至少在 syn 阶段产生）
    const fpgalsp::reports::ReportsBundle bundle =
        fpgalsp::reports::BuildReports(prepared.project);
    EXPECT_TRUE(bundle.ok);
    EXPECT_FALSE(bundle.files.area.empty());
    ASSERT_TRUE(bundle.syn.has_value());
    EXPECT_EQ(bundle.syn->label, "Synthesis");

    // 日志与状态字段完整
    EXPECT_FALSE(state.log.empty());
    EXPECT_TRUE(state.log.size() <= 200u);
    const Json status_json = state.ToJson();
    for (const char* key : {"running", "stage", "elapsed", "lastMessage", "log", "jobs"}) {
        EXPECT_TRUE(status_json.contains(key)) << key;
    }

    // ---- 硬验收：不留孤儿 TD 进程 ----
    EXPECT_TRUE(WaitForNoTdProcesses(30.0))
        << "综合结束后仍有 " << TdProcessCount() << " 个 td_commands_prompt.exe";
    // RUN_SERIAL 保证本用例不会和别的 TD 测试并行，所以这里要求一个都不剩
    EXPECT_EQ(TdProcessCount(), 0);
}

// -----------------------------------------------------------------------------
// 取消：Job Object 必须把整棵进程树带走，且不得让进程 abort
// -----------------------------------------------------------------------------
//  ⚠ 这个用例曾经因为 **异常逃出 flow worker 线程** -> std::terminate ->
//    __fastfail -> 0xC0000409 而让整个 gtest 进程崩掉（PYTHON-BUGS P-16）。
//    修法是 worker/心跳线程入口的 catch-all（flow_runner.cpp 有注释），
//    不是编译选项 —— 本工程所有 TU 都开着 /EHsc。
//    这个用例是"取消不留孤儿"的唯一真实证据，**不要因为偶发失败就删掉**。

TEST(FlowTdIntegration, CancelLeavesNoOrphanProcesses) {
    Prepared prepared("cancel");
    if (!prepared.ready) GTEST_SKIP() << prepared.skip_reason;

    fpgalsp::flow::FlowOptions options;
    options.timeout_seconds = 900.0;
    options.start_timeout_seconds = 120.0;
    options.heartbeat_seconds = 0.3;
    // 保留宽限期（默认 10 s）：先写 .stop.f 让 TD 自己收尾，超时才强杀。
    // 这条路径才是用户真正会走的取消路径。
    options.cancel_kill_grace_seconds = 10.0;

    fpgalsp::flow::FlowRunner runner(prepared.project, prepared.td, [](const Json&) {},
                                    [](int, std::string_view) {}, options);
    ASSERT_TRUE(runner.Start({"syn"}, 4).value("ok", false));

    // 等 TD 真的起来再取消（太早取消测不到进程树清理）
    bool saw_td = false;
    for (int i = 0; i < 600; ++i) {
        if (TdProcessCount() > 0) {
            saw_td = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_TRUE(saw_td) << "TD 进程一直没起来，取消用例没测到东西";

    const Json cancelled = runner.Cancel();
    EXPECT_TRUE(cancelled.value("ok", false));
    ASSERT_TRUE(runner.WaitUntilIdle(300.0)) << "取消后 flow 没有退出";

    const fpgalsp::flow::FlowState state = runner.Status();
    EXPECT_FALSE(state.running);
    EXPECT_TRUE(state.stage == "cancelled" || state.stage == "error") << state.stage;
    ASSERT_TRUE(state.ok.has_value());
    EXPECT_FALSE(*state.ok);

    // ---- 硬验收：取消后不留任何 TD 进程 ----
    EXPECT_TRUE(WaitForNoTdProcesses(60.0))
        << "取消后仍有 " << TdProcessCount() << " 个 td_commands_prompt.exe";
}

TEST(FlowTdIntegration, FailedRunLeavesNoOrphanProcesses) {
    Prepared prepared("fail");
    if (!prepared.ready) GTEST_SKIP() << prepared.skip_reason;

    // 让工程指向一个不存在的器件档案：import_device 会失败，flow 提前退出
    fpgalsp::flow::FlowOptions options;
    options.timeout_seconds = 120.0;
    options.start_timeout_seconds = 30.0;
    options.heartbeat_seconds = 0.0;
    options.explicit_db = "no_such_device_family_9.db";

    fpgalsp::flow::FlowRunner runner(prepared.project, prepared.td, [](const Json&) {},
                                    [](int, std::string_view) {}, options);
    ASSERT_TRUE(runner.Start({"syn"}, 2).value("ok", false));
    ASSERT_TRUE(runner.WaitUntilIdle(180.0));

    const fpgalsp::flow::FlowState state = runner.Status();
    EXPECT_FALSE(state.running);
    EXPECT_EQ(state.stage, "error") << "error=" << state.error;
    EXPECT_TRUE(state.ok.has_value());
    EXPECT_FALSE(*state.ok);

    EXPECT_TRUE(WaitForNoTdProcesses(30.0))
        << "失败后仍有 " << TdProcessCount() << " 个 td_commands_prompt.exe";
}
// -----------------------------------------------------------------------------
// 共享语料必须保持干净
// -----------------------------------------------------------------------------
//  这个用例排在最后：它检查前面那些 flow 用例**没有**把产物写进
//  tests/gate-corpus/corpus_alpha（那里是只读共享语料，被污染会让其他人的用例假失败）。
//  历史上发生过：e2e 的 runFlow->cancelFlow 在语料里留下 .stop.f 与半个 syn_1，
//  导致另一个人的 td.flow 稳定失败。
TEST(FlowTdIntegration, SharedCorpusStaysClean) {
    std::error_code ec;
    const fs::path corpus(CorpusDir());
    ASSERT_TRUE(fs::is_directory(corpus, ec)) << "语料目录不见了";

    std::vector<std::string> debris;
    for (fs::directory_iterator it(corpus, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        const bool is_dir = it->is_directory();
        const bool runs_dir = name.size() >= 5 && name.compare(name.size() - 5, 5, "_Runs") == 0;
        if (is_dir && (runs_dir || name == ".fpgalsp")) debris.push_back(name);
    }
    for (const std::string& name : debris) {
        ADD_FAILURE() << "共享语料被污染了：" << CorpusDir() << "/" << name
                      << "（跑 flow 的测试必须用临时副本，并且改写 .al 的 Path 属性）";
    }
    EXPECT_TRUE(debris.empty());
}
