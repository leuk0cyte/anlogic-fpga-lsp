// =============================================================================
//  tests/unit/flow_programmer_test.cpp —— BitWizard 烧录（三条安全规则）
// =============================================================================
//  烧录是唯一会碰真实硬件的路径，所以这里的重点是**拒绝**而不是成功：
//    R1 list_cable 为空 -> 一条烧录命令都不下发
//    R2 confirm 不为 true -> 什么都不做
//    R3 器件不匹配 -> 即使 allowMismatch=true 也拒绝
//  三条各有用例，并且都断言"没有发出 program/program_spi"。
// =============================================================================

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "flow/programmer.h"
#include "platform/encoding.h"

namespace {

namespace fs = std::filesystem;
// Json 在 fpgalsp::jsonrpc 里；这些用例大量用到，起个别名
namespace jsonrpc = fpgalsp::jsonrpc;
using Json = fpgalsp::jsonrpc::Json;
using namespace fpgalsp::flow;  // NOLINT(google-build-using-namespace)

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
        path_ += "fpgalsp_prog_" + tag;
        std::error_code ec;
        fs::remove_all(fs::path(path_), ec);
        fs::create_directories(fs::path(path_), ec);
    }
    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(fs::path(path_), ec);
    }
    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const std::string& path() const { return path_; }

private:
    std::string path_;
};

void Touch(const std::string& path, const std::string& content = "bit") {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    (void)fpgalsp::platform::write_file_utf8(path, content);
}

// -----------------------------------------------------------------------------
// 假 BitWizard
// -----------------------------------------------------------------------------
struct BwScript {
    std::string list_cable_output = "Connect device 0: PH1A100\n";
    std::string device_id_output = "PH1A100\n";
    std::string program_output = "RUN-100 starting\nprogram done\n";
    std::vector<std::string> commands;
};

class FakeBwSession final : public BwSessionLike {
public:
    explicit FakeBwSession(std::shared_ptr<BwScript> script) : script_(std::move(script)) {}

    std::string Run(std::string_view tcl, double /*timeout*/) override {
        const std::string command(tcl);
        script_->commands.push_back(command);
        if (command == "list_cable") return script_->list_cable_output;
        if (command == "read_device_id") return script_->device_id_output;
        if (command.rfind("program", 0) == 0) return script_->program_output;
        if (command.rfind("svfgen", 0) == 0) {
            // svfgen 会真的产出文件；这里从命令里解析出 -svf 目标
            const std::size_t marker = command.find("-svf {");
            if (marker != std::string::npos) {
                const std::size_t begin = marker + 6;
                const std::size_t end = command.find('}', begin);
                if (end != std::string::npos) {
                    Touch(command.substr(begin, end - begin), "svf");
                }
            }
            return "svf generated\n";
        }
        return "";
    }

    void Kill() override {}
    void Close() override {}

private:
    std::shared_ptr<BwScript> script_;
};

struct Harness {
    ScopedTempDir temp;
    std::shared_ptr<BwScript> script = std::make_shared<BwScript>();
    fpgalsp::project::Project project;
    std::string bit_path;

    explicit Harness(const std::string& tag, const std::function<void(BwScript&)>& configure = {})
        : temp("harness_" + tag) {
        if (configure) configure(*script);
        project.dir = temp.path();
        project.name = "demo_project";
        project.al_path = temp.path() + "/demo_project.al";
        project.device = "EF2L45LG144B";
        project.family = "EF2";
        bit_path = temp.path() + "/demo_project_Runs/phy_1/demo_project.bit";
        Touch(bit_path, "0123456789");
    }

    ProgramOptions Options(bool confirm, bool allow_mismatch = false) {
        ProgramOptions options;
        options.cwd = temp.path();
        options.bit_path = bit_path;
        options.confirm = confirm;
        options.allow_mismatch = allow_mismatch;
        auto script_local = script;
        options.session_factory = [script_local](const std::string&) {
            return std::make_unique<FakeBwSession>(script_local);
        };
        return options;
    }

    // 有没有下发过烧录命令
    bool IssuedProgramCommand() const {
        for (const std::string& command : script->commands) {
            if (command.rfind("program", 0) == 0) return true;
        }
        return false;
    }
};

}  // namespace

// -----------------------------------------------------------------------------
// 纯解析
// -----------------------------------------------------------------------------

TEST(FlowProgrammerParse, ParseCables) {
    const std::vector<CableInfo> cables =
        ParseCables("INFO: scanning\nConnect device 0: PH1A100\n");
    ASSERT_EQ(cables.size(), 1u);
    EXPECT_EQ(cables[0].index, 0);
    EXPECT_EQ(cables[0].device, "PH1A100");

    EXPECT_TRUE(ParseCables("no devices attached\n").empty());
    EXPECT_TRUE(ParseCables("").empty());
}

TEST(FlowProgrammerParse, ParseCablesMultipleIndexes) {
    const std::vector<CableInfo> cables =
        ParseCables("Connect device 1: EF2L45LG144B\nConnect device 2: PH1A100\n");
    ASSERT_EQ(cables.size(), 2u);
    EXPECT_EQ(cables[0].index, 1);
    EXPECT_EQ(cables[1].device, "PH1A100");
}

TEST(FlowProgrammerParse, ParseDeviceId) {
    EXPECT_EQ(ParseDeviceId("PH1A100\n"), "PH1A100");
    EXPECT_EQ(ParseDeviceId("banner\nPH1A100\n"), "PH1A100");
    // 错误码行不是器件 ID
    EXPECT_EQ(ParseDeviceId("PRJ-8806 ERROR: foo\n"), "");
    // 带冒号/空格的都不是
    EXPECT_EQ(ParseDeviceId("Device : PH1A100\n"), "");
    EXPECT_EQ(ParseDeviceId(""), "");
}

TEST(FlowProgrammerParse, SeriesAndLetters) {
    EXPECT_EQ(SeriesOf("EF2L45LG144B"), "EF2L");
    EXPECT_EQ(SeriesOf("PH1A100"), "PH1A");
    EXPECT_EQ(SeriesOf("EF2M45LG100"), "EF2M");
    // letters_of 只取**开头连续的字母**（Python: re.match(r"^([A-Za-z]+)"))，
    // 所以 "EF2L45LG144B" -> "EF"；这也是 CompareDevices 判定"同一 family"的依据
    // （EF2L vs EF2M 都是 "EF" -> 同族不同型号；EF2 vs EG4 -> "EF" vs "EG" -> 不同族）。
    EXPECT_EQ(LettersOf("EF2L45LG144B"), "EF");
    EXPECT_EQ(LettersOf("PH1A100"), "PH");
    // 小写输入也要归一成大写
    EXPECT_EQ(SeriesOf("ef2l45lg144b"), "EF2L");
}

TEST(FlowProgrammerParse, CompareSameSeriesMatches) {
    const DeviceComparison comparison = CompareDevices("EF2L45LG144B", "EF2L45LG144B");
    EXPECT_TRUE(comparison.match);
    EXPECT_FALSE(comparison.warn);
    EXPECT_TRUE(comparison.reason.empty());
}

TEST(FlowProgrammerParse, CompareSameFamilyDifferentDeviceWarns) {
    const DeviceComparison comparison = CompareDevices("EF2L45LG144B", "EF2M45LG100");
    EXPECT_FALSE(comparison.match);
    EXPECT_TRUE(comparison.warn);
    EXPECT_NE(comparison.reason.find("Same family"), std::string::npos);
}

TEST(FlowProgrammerParse, CompareDifferentFamilyNeverMatches) {
    const DeviceComparison comparison = CompareDevices("EF2L45LG144B", "PH1A100");
    EXPECT_FALSE(comparison.match);
    EXPECT_FALSE(comparison.warn);
    EXPECT_NE(comparison.reason.find("Different family"), std::string::npos);
}

TEST(FlowProgrammerParse, CompareWithUnknownOnlineDevice) {
    const DeviceComparison comparison = CompareDevices("EF2L45LG144B", "");
    EXPECT_FALSE(comparison.match);
    EXPECT_NE(comparison.reason.find("Cannot read the online device ID"), std::string::npos);
}

TEST(FlowProgrammerParse, CheckBitRules) {
    ScopedTempDir temp("checkbit");
    const std::string good = temp.path() + "/a.bit";
    Touch(good, "0123456789");
    const BitCheck ok = CheckBit(good);
    EXPECT_TRUE(ok.ok);
    EXPECT_EQ(ok.size, 10);

    EXPECT_FALSE(CheckBit("").ok);
    EXPECT_FALSE(CheckBit(temp.path() + "/missing.bit").ok);
    const std::string wrong = temp.path() + "/a.svf";
    Touch(wrong);
    const BitCheck bad = CheckBit(wrong);
    EXPECT_FALSE(bad.ok);
    EXPECT_NE(bad.reason.find("Not a .bit file"), std::string::npos);
}

TEST(FlowProgrammerParse, DefaultBitPrefersBestResult) {
    ScopedTempDir temp("defaultbit");
    const std::string runs = temp.path() + "/demo_project_Runs";
    Touch(runs + "/phy_1/demo_project.bit");
    Touch(runs + "/best_result/demo_project.bit");

    fpgalsp::project::Project project;
    project.dir = temp.path();
    project.name = "demo_project";
    const std::string bit = DefaultBit(project);
    EXPECT_NE(bit.find("best_result"), std::string::npos);
}

// -----------------------------------------------------------------------------
// 安全规则 2：confirm
// -----------------------------------------------------------------------------

TEST(FlowProgrammerSafety, RefusesWithoutConfirm) {
    Harness harness("r2");
    const Json result = Program(fpgalsp::td::TdChoice{}, harness.project,
                                harness.Options(/*confirm=*/false));
    EXPECT_FALSE(result.value("ok", false));
    EXPECT_TRUE(result.value("refused", false));
    EXPECT_EQ(result.value("stage", std::string()), "confirm");
    EXPECT_NE(result.value("reason", std::string()).find("confirm:true"), std::string::npos);

    // 关键：连 BitWizard 会话都不该开，更不该下发命令
    EXPECT_TRUE(harness.script->commands.empty());
    EXPECT_FALSE(harness.IssuedProgramCommand());
}

// confirm=default（不传）同样是拒绝
TEST(FlowProgrammerSafety, DefaultOptionsRefuse) {
    Harness harness("r2b");
    ProgramOptions options = harness.Options(/*confirm=*/true);
    options.confirm = false;
    const Json result = Program(fpgalsp::td::TdChoice{}, harness.project, options);
    EXPECT_EQ(result.value("stage", std::string()), "confirm");
    EXPECT_FALSE(harness.IssuedProgramCommand());
}

// -----------------------------------------------------------------------------
// 安全规则 1：list_cable 为空
// -----------------------------------------------------------------------------

TEST(FlowProgrammerSafety, RefusesWhenNoCable) {
    Harness harness("r1", [](BwScript& script) { script.list_cable_output = "no devices\n"; });
    const Json result = Program(fpgalsp::td::TdChoice{}, harness.project,
                                harness.Options(/*confirm=*/true));
    EXPECT_FALSE(result.value("ok", false));
    EXPECT_TRUE(result.value("refused", false));
    EXPECT_EQ(result.value("stage", std::string()), "cable");
    EXPECT_FALSE(harness.IssuedProgramCommand());
    // list_cable 本身是只读的，跑了是对的
    ASSERT_FALSE(harness.script->commands.empty());
    EXPECT_EQ(harness.script->commands[0], "list_cable");
}

// -----------------------------------------------------------------------------
// 安全规则 3：器件不匹配
// -----------------------------------------------------------------------------

TEST(FlowProgrammerSafety, RefusesDeviceMismatch) {
    Harness harness("r3", [](BwScript& script) {
        script.list_cable_output = "Connect device 0: PH1A100\n";
        script.device_id_output = "PH1A100\n";
    });
    const Json result = Program(fpgalsp::td::TdChoice{}, harness.project,
                                harness.Options(/*confirm=*/true));
    EXPECT_FALSE(result.value("ok", false));
    EXPECT_TRUE(result.value("refused", false));
    EXPECT_EQ(result.value("stage", std::string()), "device");
    EXPECT_NE(result.value("reason", std::string()).find("Different family"), std::string::npos);
    EXPECT_FALSE(harness.IssuedProgramCommand());
}

// ⚠ 核心：即使 allowMismatch=true 也必须拒绝（与 Python 的唯一一处有意收紧）
TEST(FlowProgrammerSafety, RefusesDeviceMismatchEvenWithAllowMismatch) {
    Harness harness("r3b", [](BwScript& script) {
        script.list_cable_output = "Connect device 0: PH1A100\n";
        script.device_id_output = "PH1A100\n";
    });
    const Json result = Program(fpgalsp::td::TdChoice{}, harness.project,
                                harness.Options(/*confirm=*/true, /*allow_mismatch=*/true));
    EXPECT_FALSE(result.value("ok", false));
    EXPECT_EQ(result.value("stage", std::string()), "device");
    EXPECT_TRUE(result.value("allowMismatchIgnored", false));
    EXPECT_FALSE(harness.IssuedProgramCommand())
        << "器件不匹配时无论如何都不能下发烧录命令";
}

// 同系列不同型号：也拒绝（收紧行为）
TEST(FlowProgrammerSafety, RefusesSameFamilyDifferentPart) {
    Harness harness("r3c", [](BwScript& script) {
        script.list_cable_output = "Connect device 0: EF2M45LG100\n";
        script.device_id_output = "EF2M45LG100\n";
    });
    const Json result = Program(fpgalsp::td::TdChoice{}, harness.project,
                                harness.Options(/*confirm=*/true, /*allow_mismatch=*/true));
    EXPECT_FALSE(result.value("ok", false));
    EXPECT_EQ(result.value("stage", std::string()), "device");
    EXPECT_FALSE(harness.IssuedProgramCommand());
}

// -----------------------------------------------------------------------------
// 正常路径（假硬件）
// -----------------------------------------------------------------------------

TEST(FlowProgrammerSafety, ProgramsWhenEverythingMatches) {
    Harness harness("ok", [](BwScript& script) {
        script.list_cable_output = "Connect device 0: EF2L45LG144B\n";
        script.device_id_output = "EF2L45LG144B\n";
    });
    const Json result = Program(fpgalsp::td::TdChoice{}, harness.project,
                                harness.Options(/*confirm=*/true));
    EXPECT_TRUE(result.value("ok", false)) << result.dump();
    EXPECT_EQ(result.value("mode", std::string()), "sram");
    EXPECT_EQ(result.value("mode_label", std::string()), "SRAM (JTAG)");
    EXPECT_NE(result.value("command", std::string()).find("program {"), std::string::npos);

    bool issued = false;
    for (const std::string& command : harness.script->commands) {
        if (command.rfind("program {", 0) == 0) issued = true;
    }
    EXPECT_TRUE(issued);
}

TEST(FlowProgrammerSafety, FlashUsesProgramSpi) {
    Harness harness("flash", [](BwScript& script) {
        script.list_cable_output = "Connect device 0: EF2L45LG144B\n";
        script.device_id_output = "EF2L45LG144B\n";
    });
    ProgramOptions options = harness.Options(/*confirm=*/true);
    options.mode = "flash";
    const Json result = Program(fpgalsp::td::TdChoice{}, harness.project, options);
    EXPECT_TRUE(result.value("ok", false));
    EXPECT_NE(result.value("command", std::string()).find("program_spi {"), std::string::npos);
    EXPECT_EQ(result.value("mode_label", std::string()), "Flash (SPI)");
}

TEST(FlowProgrammerSafety, UnknownModeRejected) {
    Harness harness("mode");
    ProgramOptions options = harness.Options(/*confirm=*/true);
    options.mode = "jtag";
    const Json result = Program(fpgalsp::td::TdChoice{}, harness.project, options);
    EXPECT_FALSE(result.value("ok", false));
    EXPECT_NE(result.value("error", std::string()).find("Unknown mode"), std::string::npos);
    EXPECT_FALSE(harness.IssuedProgramCommand());
}

TEST(FlowProgrammerSafety, RefusesWhenBitMissing) {
    Harness harness("nobit", [](BwScript& script) {
        script.list_cable_output = "Connect device 0: EF2L45LG144B\n";
        script.device_id_output = "EF2L45LG144B\n";
    });
    ProgramOptions options = harness.Options(/*confirm=*/true);
    options.bit_path = harness.temp.path() + "/nope.bit";
    const Json result = Program(fpgalsp::td::TdChoice{}, harness.project, options);
    EXPECT_FALSE(result.value("ok", false));
    EXPECT_EQ(result.value("stage", std::string()), "bit");
    EXPECT_FALSE(harness.IssuedProgramCommand());
}

TEST(FlowProgrammerSafety, ProgramErrorIsReported) {
    Harness harness("progerr", [](BwScript& script) {
        script.list_cable_output = "Connect device 0: EF2L45LG144B\n";
        script.device_id_output = "EF2L45LG144B\n";
        script.program_output = "PRJ-8806 ERROR: device not responding\n";
    });
    const Json result = Program(fpgalsp::td::TdChoice{}, harness.project,
                                harness.Options(/*confirm=*/true));
    EXPECT_FALSE(result.value("ok", false));
    EXPECT_NE(result.value("error", std::string()).find("BitWizard error"), std::string::npos);
}

// -----------------------------------------------------------------------------
// 预检 / SVF
// -----------------------------------------------------------------------------

TEST(FlowProgrammerSafety, InspectWithoutBit) {
    ScopedTempDir temp("inspect_nobit");
    fpgalsp::project::Project project;
    project.dir = temp.path();
    project.name = "nothing";
    project.device = "EF2L45LG144B";

    const Json result = Inspect(fpgalsp::td::TdChoice{}, project, ProgramOptions{});
    EXPECT_FALSE(result.value("ok", false));
    EXPECT_NE(result.value("reason", std::string()).find("No .bit yet"), std::string::npos);
}

TEST(FlowProgrammerSafety, InspectReportsCablesAndMatch) {
    Harness harness("inspect", [](BwScript& script) {
        script.list_cable_output = "Connect device 0: EF2L45LG144B\n";
        script.device_id_output = "EF2L45LG144B\n";
    });
    const Json result = Inspect(fpgalsp::td::TdChoice{}, harness.project,
                                harness.Options(/*confirm=*/true));
    EXPECT_TRUE(result.value("ok", false)) << result.dump();
    EXPECT_TRUE(result.value("match", false));
    EXPECT_EQ(result.value("device_id", std::string()), "EF2L45LG144B");
    ASSERT_TRUE(result["cables"].is_array());
    EXPECT_EQ(result["cables"].size(), 1u);
    // 预检绝不能下发烧录命令
    EXPECT_FALSE(harness.IssuedProgramCommand());
}

TEST(FlowProgrammerSafety, GeneratesSvfWithoutHardware) {
    Harness harness("svf");
    const Json result =
        GenerateSvf(fpgalsp::td::TdChoice{}, harness.project, harness.Options(/*confirm=*/false));
    EXPECT_TRUE(result.value("ok", false)) << result.dump();
    EXPECT_EQ(result.value("scan_mode", std::string()), "prog_sram");
    EXPECT_NE(result.value("svf", std::string()).find(".svf"), std::string::npos);
    EXPECT_TRUE(fs::is_regular_file(fs::path(result.value("svf", std::string()))));
    // svfgen 不是烧录，不需要 confirm
    bool issued_svf = false;
    for (const std::string& command : harness.script->commands) {
        if (command.rfind("svfgen ", 0) == 0) issued_svf = true;
    }
    EXPECT_TRUE(issued_svf);
}

TEST(FlowProgrammerSafety, GenerateSvfNeedsABit) {
    Harness harness("svf_nobit");
    ProgramOptions options = harness.Options(/*confirm=*/false);
    options.bit_path = harness.temp.path() + "/missing.bit";
    const Json result = GenerateSvf(fpgalsp::td::TdChoice{}, harness.project, options);
    EXPECT_FALSE(result.value("ok", false));
    EXPECT_TRUE(harness.script->commands.empty());
}
