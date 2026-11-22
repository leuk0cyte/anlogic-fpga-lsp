// =============================================================================
//  tests/unit/flow_runner_test.cpp —— FlowRunner（用假 TD 会话，不需要真实 TD）
// =============================================================================
//  为什么能这样测：FlowRunner 的会话是 FlowSession 抽象，单测注入一个"会摆出
//  run 目录现场"的假实现，就能把 PRJ-8806 三种组合、busy 互斥、取消、增量布线
//  这些逻辑在毫秒级跑完。真实 TD 的端到端验证在 tests/td/flow_integration_test.cpp。
// =============================================================================

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "flow/flow_runner.h"
#include "platform/encoding.h"

namespace {

namespace fs = std::filesystem;
using fpgalsp::flow::FlowOptions;
using fpgalsp::flow::FlowRunner;
using fpgalsp::flow::FlowSession;
using fpgalsp::flow::FlowState;
using fpgalsp::jsonrpc::Json;

std::string Slashes(std::string text) {
    for (char& c : text) {
        if (c == '\\') c = '/';
    }
    return text;
}

class ScopedTempDir {
public:
    explicit ScopedTempDir(const std::string& tag) {
        path_ = Slashes(fs::temp_directory_path().string());
        if (!path_.empty() && path_.back() != '/') path_.push_back('/');
        path_ += "fpgalsp_flow_" + tag;
        std::error_code ec;
        fs::remove_all(fs::path(path_), ec);
        fs::create_directories(fs::path(path_), ec);
    }
    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(fs::path(path_), ec);
        fs::remove_all(fs::path(path_ + "_state"), ec);  // flow/cache 状态目录（在工程目录外）
    }
    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const std::string& path() const { return path_; }

private:
    std::string path_;
};

void Touch(const std::string& path, const std::string& content = "x") {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    (void)fpgalsp::platform::write_file_utf8(path, content);
}

// -----------------------------------------------------------------------------
// 假 TD 会话
// -----------------------------------------------------------------------------
struct FakeScript {
    // wait_run 的输出（决定 PRJ-8806 分支）
    std::string wait_run_output;
    // 各阶段的 run 目录里是否摆出完成标记
    bool complete_syn = true;
    bool complete_phy = true;
    std::vector<std::string> extra_syn_markers;  // 例如 ".opt_gate.error.f"
    std::vector<std::string> extra_phy_markers;
    // import_device 是否失败
    bool fail_import = false;
    // 完全没有 run 痕迹（模拟 PRJ-8001：子进程根本没起来）
    bool no_run_trace = false;
    // wait_run 阻塞直到被 kill（测试取消）
    bool block_in_wait_run = false;
    // 记录收到的命令
    std::vector<std::string> commands;
    std::atomic<bool> killed{false};
    std::atomic<bool> closed{false};
};

class FakeSession final : public FlowSession {
public:
    FakeSession(std::shared_ptr<FakeScript> script, std::string project_dir,
                std::string project_name, std::string al_path)
        : script_(std::move(script)),
          project_dir_(std::move(project_dir)),
          project_name_(std::move(project_name)),
          al_path_(std::move(al_path)) {}

    std::string Run(std::string_view tcl, double /*timeout_seconds*/) override {
        const std::string command(tcl);
        script_->commands.push_back(command);

        if (command.find("import_device") == 0) {
            return script_->fail_import ? "USR-8001 ERROR: Invalid device_db name\n" : "ok\n";
        }
        if (command.find("open_project") == 0) return "project opened\n";
        if (command.find("reset_runs") == 0) return "reset\n";
        if (command.find("launch_runs") == 0) {
            const char* run = command.find("syn_1") != std::string::npos ? "syn_1" : "phy_1";
            if (!script_->no_run_trace) {
                Touch(RunPath(run, ".read_design.begin.f"));
            }
            return "launched\n";
        }
        if (command.find("wait_run syn_1") == 0) {
            if (script_->complete_syn) WriteSynMarkers();
            return script_->wait_run_output;
        }
        if (command.find("wait_run phy_1") == 0) {
            if (script_->block_in_wait_run) {
                while (!script_->killed.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                return {};
            }
            if (script_->complete_phy) WritePhyMarkers();
            return script_->wait_run_output;
        }
        if (command.find("save_best_bits") == 0) return "";
        return "";
    }

    void Kill() override { script_->killed.store(true); }
    void Close() override { script_->closed.store(true); }

private:
    std::string RunPath(const std::string& run, const std::string& name) const {
        return project_dir_ + "/" + project_name_ + "_Runs/" + run + "/" + name;
    }

    void WriteSynMarkers() {
        Touch(RunPath("syn_1", ".opt_gate.end.f"));
        Touch(RunPath("syn_1", project_name_ + "_gate.db"));
        Touch(RunPath("syn_1", project_name_ + "_gate.area"), "area");
        for (const std::string& extra : script_->extra_syn_markers) Touch(RunPath("syn_1", extra));
    }

    void WritePhyMarkers() {
        Touch(RunPath("phy_1", ".bitgen.end.f"));
        Touch(RunPath("phy_1", project_name_ + ".bit"), "bit");
        for (const std::string& extra : script_->extra_phy_markers) Touch(RunPath("phy_1", extra));
    }

    std::shared_ptr<FakeScript> script_;
    std::string project_dir_;
    std::string project_name_;
    std::string al_path_;
};

// -----------------------------------------------------------------------------
// 夹具：工程 + runner + 事件收集
// -----------------------------------------------------------------------------
struct Harness {
    ScopedTempDir temp;
    std::shared_ptr<FakeScript> script = std::make_shared<FakeScript>();
    std::vector<Json> events;
    std::mutex events_mutex;
    fpgalsp::project::Project project;

    explicit Harness(const std::string& tag,
                     const std::function<void(FakeScript&)>& configure = {})
        : temp("runner_" + tag) {
        if (configure) configure(*script);
        project.dir = temp.path();
        project.name = "demo_project";
        project.al_path = temp.path() + "/demo_project.al";
        project.device = "EF2L45LG144B";
        project.family = "EF2";
        Touch(project.al_path, "<Project/>");
    }

    FlowOptions Options() {
        FlowOptions options;
        options.heartbeat_seconds = 0.0;  // 关掉心跳线程，避免偶发时序
        options.stall_seconds = 600.0;
        options.start_timeout_seconds = 2.0;
        // 假会话：立刻杀，测的是"取消防火"本身（真实 TD 用宽限期，见 FlowOptions）
        options.cancel_kill_grace_seconds = 0.0;
        options.timeout_seconds = 5.0;
        // 真实状态下 flow/cache 在系统临时目录（不在工程目录里）；单测同样放工程目录外，
        // 名字带 "fpgalsp" 才会被写服务的 kTemp 规则接受。
        options.flow_root = temp.path() + "_state/flow";
        options.cache_root = temp.path() + "_state/cache";
        // 单测里没有真实 TD 安装，直接指定 db 名，跳过 DbCandidates 探测
        options.explicit_db = "ef2_4.db";
        const fpgalsp::project::Project local = project;
        auto script_local = script;
        options.session_factory = [script_local, local](const std::string&) {
            return std::make_unique<FakeSession>(script_local, local.dir, local.name,
                                                 local.al_path);
        };
        return options;
    }

    std::unique_ptr<FlowRunner> MakeRunner() {
        auto runner = std::make_unique<FlowRunner>(
            project, fpgalsp::td::TdChoice{}, [this](const Json& event) {
                std::lock_guard<std::mutex> lock(events_mutex);
                events.push_back(event);
            },
            [](int, std::string_view) {}, Options());
        return runner;
    }
};

bool HasEvent(const std::vector<Json>& events, const std::string& type) {
    for (const Json& event : events) {
        if (event.value("type", std::string()) == type) return true;
    }
    return false;
}

}  // namespace

// -----------------------------------------------------------------------------
// 正常路径
// -----------------------------------------------------------------------------

TEST(FlowRunner, SynSucceedsAndIssuesExpectedCommands) {
    Harness harness("syn_ok");
    auto runner = harness.MakeRunner();
    const Json started = runner->Start({"syn"}, 4);
    ASSERT_TRUE(started.value("ok", false));
    EXPECT_FALSE(started.value("runId", std::string()).empty());

    ASSERT_TRUE(runner->WaitUntilIdle(5.0));
    const FlowState state = runner->Status();
    EXPECT_FALSE(state.running);
    EXPECT_EQ(state.stage, "done");
    ASSERT_TRUE(state.ok.has_value());
    EXPECT_TRUE(*state.ok);
    EXPECT_EQ(state.jobs, 4);
    EXPECT_FALSE(state.log.empty());
    EXPECT_TRUE(state.log.size() <= 200u);

    // 命令序列：import_device -> open_project -> reset_runs -> launch_runs -> wait_run
    const std::vector<std::string>& commands = harness.script->commands;
    ASSERT_GE(commands.size(), 5u);
    EXPECT_EQ(commands[0].rfind("import_device", 0), 0u);
    EXPECT_EQ(commands[1].rfind("open_project", 0), 0u);
    EXPECT_NE(commands[2].find("reset_runs"), std::string::npos);
    EXPECT_NE(commands[3].find("launch_runs"), std::string::npos);
    EXPECT_NE(commands[3].find("-jobs 4"), std::string::npos);
    EXPECT_NE(commands[4].find("wait_run syn_1 -quiet"), std::string::npos);

    EXPECT_TRUE(HasEvent(harness.events, "start"));
    EXPECT_TRUE(HasEvent(harness.events, "done"));
    EXPECT_FALSE(HasEvent(harness.events, "error"));
    EXPECT_TRUE(harness.script->killed.load() == false);
}

TEST(FlowRunner, AllRunsSynThenPhyThenSaveBestBits) {
    Harness harness("all_ok");
    auto runner = harness.MakeRunner();
    ASSERT_TRUE(runner->Start({"all"}, 2).value("ok", false));
    ASSERT_TRUE(runner->WaitUntilIdle(5.0));

    const FlowState state = runner->Status();
    EXPECT_TRUE(state.ok.has_value() && *state.ok);
    const std::vector<std::string>& commands = harness.script->commands;
    bool saw_save_best = false;
    for (const std::string& command : commands) {
        if (command == "save_best_bits") saw_save_best = true;
    }
    EXPECT_TRUE(saw_save_best);
}

// -----------------------------------------------------------------------------
// PRJ-8806 容错（三种组合，走完整 worker 路径）
// -----------------------------------------------------------------------------

TEST(FlowRunner, ToleratesPrj8806WhenArtifactsComplete) {
    Harness harness("prj_ok", [](FakeScript& script) {
        script.wait_run_output = "PRJ-8806 ERROR: bitgen exited with code 1\n";
    });
    auto runner = harness.MakeRunner();
    ASSERT_TRUE(runner->Start({"syn"}, 1).value("ok", false));
    ASSERT_TRUE(runner->WaitUntilIdle(5.0));

    const FlowState state = runner->Status();
    EXPECT_TRUE(state.ok.has_value() && *state.ok) << "产物齐全时应判成功";
    bool saw_note = false;
    for (const std::string& line : state.log) {
        if (line.find("completed despite TD PRJ-8806") != std::string::npos) saw_note = true;
    }
    EXPECT_TRUE(saw_note) << "要留下'容错'说明，便于事后追查";
}

TEST(FlowRunner, FailsPrj8806WhenNoCompletionMarker) {
    Harness harness("prj_nomarker", [](FakeScript& script) {
        script.wait_run_output = "PRJ-8806 ERROR: bitgen exited with code 1\n";
        script.complete_syn = false;
    });
    auto runner = harness.MakeRunner();
    ASSERT_TRUE(runner->Start({"syn"}, 1).value("ok", false));
    ASSERT_TRUE(runner->WaitUntilIdle(5.0));

    const FlowState state = runner->Status();
    EXPECT_TRUE(state.ok.has_value() && !*state.ok);
    EXPECT_EQ(state.stage, "error");
    EXPECT_NE(state.error.find("PRJ-8806"), std::string::npos);
}

TEST(FlowRunner, FailsPrj8806WhenErrorMarkerPresent) {
    Harness harness("prj_errf", [](FakeScript& script) {
        script.wait_run_output = "PRJ-8806 ERROR: bitgen exited with code 1\n";
        script.extra_syn_markers.push_back(".opt_gate.error.f");
    });
    auto runner = harness.MakeRunner();
    ASSERT_TRUE(runner->Start({"syn"}, 1).value("ok", false));
    ASSERT_TRUE(runner->WaitUntilIdle(5.0));

    const FlowState state = runner->Status();
    EXPECT_TRUE(state.ok.has_value() && !*state.ok);
    EXPECT_NE(state.error.find("failed run step"), std::string::npos);
}

TEST(FlowRunner, FailsOnOtherErrorCodes) {
    Harness harness("other_error", [](FakeScript& script) {
        script.wait_run_output = "USR-8316 ERROR: invalid speed grade\n";
    });
    auto runner = harness.MakeRunner();
    ASSERT_TRUE(runner->Start({"syn"}, 1).value("ok", false));
    ASSERT_TRUE(runner->WaitUntilIdle(5.0));
    const FlowState state = runner->Status();
    EXPECT_TRUE(state.ok.has_value() && !*state.ok);
    EXPECT_NE(state.error.find("USR-8316"), std::string::npos);
}

// -----------------------------------------------------------------------------
// 互斥 / 参数校验
// -----------------------------------------------------------------------------

TEST(FlowRunner, BusyRejectsSecondStart) {
    Harness harness("busy", [](FakeScript& script) { script.block_in_wait_run = true; });
    auto runner = harness.MakeRunner();
    const Json first = runner->Start({"phy"}, 1);
    ASSERT_TRUE(first.value("ok", false));

    const Json second = runner->Start({"syn"}, 1);
    EXPECT_FALSE(second.value("ok", false));
    EXPECT_EQ(second.value("error", std::string()), "busy");
    EXPECT_EQ(second.value("runId", std::string()), first.value("runId", std::string()));

    runner->Cancel();
    ASSERT_TRUE(runner->WaitUntilIdle(5.0));
}

TEST(FlowRunner, UnknownStageRejected) {
    Harness harness("unknown");
    auto runner = harness.MakeRunner();
    const Json result = runner->Start({"bitgen"}, 1);
    EXPECT_FALSE(result.value("ok", false));
    EXPECT_NE(result.value("error", std::string()).find("unknown stage"), std::string::npos);
    EXPECT_TRUE(harness.script->commands.empty()) << "参数不合法时一条命令都不该发";
}

// -----------------------------------------------------------------------------
// 取消
// -----------------------------------------------------------------------------

TEST(FlowRunner, CancelMarksCancelledAndKillsSession) {
    Harness harness("cancel", [](FakeScript& script) { script.block_in_wait_run = true; });
    auto runner = harness.MakeRunner();
    ASSERT_TRUE(runner->Start({"phy"}, 1).value("ok", false));

    // 等 worker 真的进到 wait_run 再取消
    for (int i = 0; i < 200; ++i) {
        if (harness.script->commands.size() >= 5) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const Json cancelled = runner->Cancel();
    EXPECT_TRUE(cancelled.value("ok", false));
    ASSERT_TRUE(runner->WaitUntilIdle(5.0));

    const FlowState state = runner->Status();
    EXPECT_EQ(state.stage, "cancelled");
    EXPECT_TRUE(state.ok.has_value() && !*state.ok);
    EXPECT_EQ(state.error, "cancelled");
    EXPECT_TRUE(harness.script->killed.load()) << "取消必须终止 TD 进程树";
    EXPECT_TRUE(harness.script->closed.load()) << "会话要关掉，不能留孤儿进程";
    EXPECT_TRUE(HasEvent(harness.events, "error"));
}

TEST(FlowRunner, CancelWhenIdleIsHarmless) {
    Harness harness("cancel_idle");
    auto runner = harness.MakeRunner();
    const Json result = runner->Cancel();
    EXPECT_TRUE(result.value("ok", false));
    EXPECT_EQ(result.value("note", std::string()), "not running");
}

// -----------------------------------------------------------------------------
// 失败路径
// -----------------------------------------------------------------------------

TEST(FlowRunner, ImportDeviceFailureSurfaces) {
    Harness harness("import_fail", [](FakeScript& script) { script.fail_import = true; });
    auto runner = harness.MakeRunner();
    ASSERT_TRUE(runner->Start({"syn"}, 1).value("ok", false));
    ASSERT_TRUE(runner->WaitUntilIdle(5.0));
    const FlowState state = runner->Status();
    EXPECT_TRUE(state.ok.has_value() && !*state.ok);
    EXPECT_NE(state.error.find("import_device failed"), std::string::npos);
}

TEST(FlowRunner, RunThatNeverStartsFailsFast) {
    Harness harness("nostart", [](FakeScript& script) { script.no_run_trace = true; });
    auto runner = harness.MakeRunner();
    ASSERT_TRUE(runner->Start({"syn"}, 1).value("ok", false));
    ASSERT_TRUE(runner->WaitUntilIdle(10.0));
    const FlowState state = runner->Status();
    EXPECT_TRUE(state.ok.has_value() && !*state.ok);
    EXPECT_NE(state.error.find("did not start"), std::string::npos);
}

// -----------------------------------------------------------------------------
// 增量布线
// -----------------------------------------------------------------------------

TEST(FlowRunner, RouteOnlyRequiresAPlacementDb) {
    Harness harness("route_missing");
    auto runner = harness.MakeRunner();
    ASSERT_TRUE(runner->Start({"route"}, 1).value("ok", false));
    ASSERT_TRUE(runner->WaitUntilIdle(5.0));
    const FlowState state = runner->Status();
    EXPECT_TRUE(state.ok.has_value() && !*state.ok);
    EXPECT_NE(state.error.find("No reusable placement database"), std::string::npos);
}

TEST(FlowRunner, RouteOnlyReusesNewestPlacementDb) {
    Harness harness("route_ok");
    const std::string phy = harness.project.dir + "/demo_project_Runs/phy_1";
    Touch(phy + "/demo_project_place.db", "old");
    Touch(phy + "/demo_project_route.db", "new");
    // 让 route.db 的 mtime 更新
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Touch(phy + "/demo_project_route.db", "newer");

    auto runner = harness.MakeRunner();
    ASSERT_TRUE(runner->Start({"route"}, 1).value("ok", false));
    ASSERT_TRUE(runner->WaitUntilIdle(5.0));

    const FlowState state = runner->Status();
    EXPECT_TRUE(state.ok.has_value() && *state.ok) << state.error;
    bool imported_newest = false;
    for (const std::string& command : harness.script->commands) {
        if (command.find("import_db") == 0 &&
            command.find("demo_project_route.db") != std::string::npos) {
            imported_newest = true;
        }
    }
    EXPECT_TRUE(imported_newest);
    // 增量布线不该走 open_project
    for (const std::string& command : harness.script->commands) {
        EXPECT_EQ(command.rfind("open_project", 0), std::string::npos);
    }
    // 缓存文件要写出来
    EXPECT_TRUE(fs::is_regular_file(
        fs::path(harness.temp.path() + "_state/cache/last_flow.json")));
}

// -----------------------------------------------------------------------------
// 状态视图
// -----------------------------------------------------------------------------

TEST(FlowRunner, StatusJsonShape) {
    Harness harness("status_shape");
    auto runner = harness.MakeRunner();
    const Json status = runner->Status().ToJson();
    EXPECT_TRUE(status.contains("running"));
    EXPECT_TRUE(status.contains("stage"));
    EXPECT_TRUE(status.contains("log"));
    EXPECT_TRUE(status.contains("jobs"));
    EXPECT_TRUE(status.contains("elapsed"));
    EXPECT_EQ(status["stage"], "idle");
    EXPECT_TRUE(status["ok"].is_null());
}
// -----------------------------------------------------------------------------
// 回归保护：Worker 提前 return 时必须 join 心跳线程
// -----------------------------------------------------------------------------
//  曾经的 bug：heartbeat 线程是 Worker 的局部 std::thread，而 Worker 有 7 处提前
//  return（import_device 失败 / 取消 / run 失败 …）。提前 return 会跳过 join，
//  析构一个 joinable() 的 std::thread 直接 std::terminate() ->
//  Windows __fastfail -> 整个进程以 0xC0000409 崩掉（PYTHON-BUGS P-16）。
//
//  这些用例**打开心跳线程**并走提前 return 路径：修好前它们会让测试进程 abort，
//  修好后正常返回。这是唯一能在单测层面钉住该 bug 的方式。

TEST(FlowRunner, EarlyReturnWithHeartbeatDoesNotAbort) {
    Harness harness("heartbeat_import_fail", [](FakeScript& script) {
        script.fail_import = true;  // ImportDevice 失败 -> Worker 提前 return
    });
    FlowOptions options = harness.Options();
    options.heartbeat_seconds = 0.05;  // 一定要起心跳线程

    auto runner = std::make_unique<FlowRunner>(
        harness.project, fpgalsp::td::TdChoice{},
        [](const Json&) {}, [](int, std::string_view) {}, options);
    ASSERT_TRUE(runner->Start({"syn"}, 1).value("ok", false));
    ASSERT_TRUE(runner->WaitUntilIdle(5.0));

    const FlowState state = runner->Status();
    EXPECT_TRUE(state.ok.has_value() && !*state.ok);
    EXPECT_NE(state.error.find("import_device failed"), std::string::npos);
}

TEST(FlowRunner, CancelWithHeartbeatDoesNotAbort) {
    Harness harness("heartbeat_cancel", [](FakeScript& script) {
        script.block_in_wait_run = true;  // 取消 -> FinishError -> 提前 return
    });
    FlowOptions options = harness.Options();
    options.heartbeat_seconds = 0.05;
    options.cancel_kill_grace_seconds = 0.0;  // 假会话，立刻杀

    auto runner = std::make_unique<FlowRunner>(
        harness.project, fpgalsp::td::TdChoice{},
        [](const Json&) {}, [](int, std::string_view) {}, options);
    ASSERT_TRUE(runner->Start({"phy"}, 1).value("ok", false));
    for (int i = 0; i < 200 && harness.script->commands.size() < 5; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(runner->Cancel().value("ok", false));
    ASSERT_TRUE(runner->WaitUntilIdle(5.0));
    EXPECT_EQ(runner->Status().stage, "cancelled");
}

// -----------------------------------------------------------------------------
//  曾经的 bug（2026-11-22，真机 MCP 复现）：同一个 FlowRunner 上第二次 Start
//  永久挂起 —— StartInternal 在 join 上一个 worker **之前**就把 state.running 置回
//  true、stage 置成 "starting"，旧 run 的心跳线程因此被"复活"（它只看 running），
//  永远不会退出；于是 join 等心跳、心跳等状态，第二次 Start 的调用方永久阻塞
//  （MCP 侧表现为 tools/call 无响应，且后续 flow_status 也排在单 worker 队列里）。
//
//  修法：心跳带 generation 代号，Start 递增代号让旧心跳立即作废。
//  这个用例必须有心跳线程才会复现，所以 heartbeat_seconds 设成 0.05。
// -----------------------------------------------------------------------------
TEST(FlowRunner, SecondStartAfterCompletionDoesNotHang) {
    Harness harness("restart_after_done");
    FlowOptions options = harness.Options();
    options.heartbeat_seconds = 0.05;  // 必须起心跳线程，否则测不出该 bug

    auto runner = std::make_unique<FlowRunner>(
        harness.project, fpgalsp::td::TdChoice{},
        [](const Json&) {}, [](int, std::string_view) {}, options);
    ASSERT_TRUE(runner->Start({"syn"}, 1).value("ok", false));
    // WaitUntilIdle 只等 running=false；此时上一个 worker 可能还卡在 heartbeat join 上，
    // 正好是复现窗口。
    ASSERT_TRUE(runner->WaitUntilIdle(5.0));

    std::atomic<bool> returned{false};
    std::thread starter([&runner, &returned]() {
        (void)runner->Start({"syn"}, 1);
        returned.store(true);
    });
    for (int i = 0; i < 300 && !returned.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!returned.load()) {
        // 不能 ASSERT_TRUE 后直接返回：starter 仍是 joinable，析构会 terminate。
        std::fprintf(stderr, "second Start blocked (stale heartbeat deadlock)\n");
        std::abort();
    }
    starter.join();
    ASSERT_TRUE(runner->WaitUntilIdle(5.0));
    EXPECT_EQ(runner->Status().stage, "done");
    EXPECT_TRUE(runner->Status().ok.value_or(false));
}
