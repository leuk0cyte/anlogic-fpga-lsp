// =============================================================================
//  tests/unit/write_service_test.cpp —— 统一写服务（platform::write_service）
// =============================================================================
//  重点钉住"路径策略"和"原子/备份/dry-run"这几条最容易写错、也最容易回归的规则：
//    * 工程目录里的 `.fpgalsp` 段一律拒绝（工程根反复长出 .fpgalsp 的根因防护）；
//    * kProject 只允许登记过的用户文件（.al/.adc/.sdc、chip_debugger/*.mdn）；
//    * kState / kRun 可写；dry_run 不落盘；backup 进 <state>/backup；
//    * 覆盖已有文件成功（原子写）；AppendLine 只允许非工程文件。
//  测试用 FPGALSP_STATE_DIR 把状态目录指到临时目录，避免污染真实的 %TEMP%。
// =============================================================================

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "platform/encoding.h"
#include "platform/path.h"
#include "platform/write_service.h"

namespace {

namespace fs = std::filesystem;
using fpgalsp::platform::writes::Classify;
using fpgalsp::platform::writes::Options;
using fpgalsp::platform::writes::Root;
using fpgalsp::platform::writes::Write;

std::string Slashes(std::string text) {
    for (char& c : text) {
        if (c == '\\') c = '/';
    }
    return text;
}

class WriteServiceTest : public ::testing::Test {
  protected:
    void SetUp() override {
        base_ = Slashes((fs::temp_directory_path() / "ws_write_test").string());
        fs::remove_all(base_);
        fs::create_directories(base_ + "/project/src");
        state_ = base_ + "/state";
        fs::create_directories(state_);
#ifdef _WIN32
        const char* previous = std::getenv("FPGALSP_STATE_DIR");
        saved_state_env_ = previous == nullptr ? std::string() : std::string(previous);
        _putenv_s("FPGALSP_STATE_DIR", state_.c_str());
        _putenv_s("FPGALSP_WRITE_AUDIT", "0");  // 单测不写审计文件，避免干扰断言
#else
        setenv("FPGALSP_STATE_DIR", state_.c_str(), 1);
        setenv("FPGALSP_WRITE_AUDIT", "0", 1);
#endif
        project_ = base_ + "/project";
        fs::create_directories(project_ + "/chip_debugger");
        fs::create_directories(project_ + "/prj_Runs/syn_1");
    }
    void TearDown() override {
#ifdef _WIN32
        _putenv_s("FPGALSP_STATE_DIR", saved_state_env_.c_str());
        _putenv_s("FPGALSP_WRITE_AUDIT", "");
#else
        unsetenv("FPGALSP_STATE_DIR");
        unsetenv("FPGALSP_WRITE_AUDIT");
#endif
        std::error_code ec;
        fs::remove_all(base_, ec);
    }

    bool Exists(const std::string& p) { return fs::is_regular_file(fs::path(p)); }
    std::string Read(const std::string& p) {
        return fpgalsp::platform::try_read_file_utf8(p).value_or(std::string());
    }

    std::string base_;
    std::string project_;
    std::string state_;
    std::string saved_state_env_;
};

TEST_F(WriteServiceTest, RejectsProjectLocalFpgalspSegment) {
    std::string error;
    Root root = Root::kState;
    EXPECT_FALSE(Classify(project_, project_ + "/.fpgalsp/cache/x.json", &root, &error));
    EXPECT_NE(error.find(".fpgalsp"), std::string::npos) << error;
    // 即使 .fpgalsp 出现在中间段也拒绝
    EXPECT_FALSE(Classify(project_, project_ + "/src/.fpgalsp/x.txt", &root, &error));
}

TEST_F(WriteServiceTest, StateWriteSucceedsAndCreatesParents) {
    const std::string target = state_ + "/cache/deep/x.json";
    const auto written = Write(project_, target, "{\"a\":1}", {.caller = "test.state"});
    EXPECT_TRUE(written.ok) << written.error;
    EXPECT_EQ(written.root, Root::kState);
    EXPECT_EQ(Read(target), "{\"a\":1}");
}

TEST_F(WriteServiceTest, ProjectFileWhitelist) {
    std::string error;
    Root root = Root::kState;
    // 允许：.al / .adc / .sdc / chip_debugger/*.mdn
    EXPECT_TRUE(Classify(project_, project_ + "/top.al", &root, &error));
    EXPECT_EQ(root, Root::kProject);
    EXPECT_TRUE(Classify(project_, project_ + "/pin.adc", &root, &error));
    EXPECT_TRUE(Classify(project_, project_ + "/top.sdc", &root, &error));
    EXPECT_TRUE(Classify(project_, project_ + "/chip_debugger/user.mdn", &root, &error));
    // 拒绝：普通源文件 / JSON / 未知目录
    EXPECT_FALSE(Classify(project_, project_ + "/top.v", &root, &error));
    EXPECT_FALSE(Classify(project_, project_ + "/build.json", &root, &error));

    const auto written = Write(project_, project_ + "/top.al", "<Project/>",
                               {.caller = "test.project"});
    EXPECT_TRUE(written.ok) << written.error;
    EXPECT_EQ(Read(project_ + "/top.al"), "<Project/>");
}

TEST_F(WriteServiceTest, RunDirectoryIsWritable) {
    const std::string target = project_ + "/prj_Runs/syn_1/flow.status";
    fpgalsp::platform::write_file_utf8(target, "old");
    const auto written = Write(project_, target, "new", {.caller = "test.run"});
    EXPECT_TRUE(written.ok) << written.error;
    EXPECT_EQ(written.root, Root::kRun);
    EXPECT_EQ(Read(target), "new");
}

TEST_F(WriteServiceTest, DryRunDoesNotTouchDisk) {
    const std::string target = state_ + "/cache/dry.json";
    const auto written = Write(project_, target, "x", {.dry_run = true, .caller = "test.dry"});
    EXPECT_TRUE(written.ok) << written.error;
    EXPECT_FALSE(Exists(target));
}

TEST_F(WriteServiceTest, BackupGoesToStateDirectory) {
    const std::string target = project_ + "/top.al";
    fpgalsp::platform::write_file_utf8(target, "old-content");
    const auto written =
        Write(project_, target, "new-content", {.backup = true, .caller = "test.backup"});
    EXPECT_TRUE(written.ok) << written.error;
    EXPECT_EQ(Read(target), "new-content");

    bool found_backup = false;
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(state_ + "/backup", ec)) {
        if (!entry.is_regular_file()) continue;
        if (Read(Slashes(entry.path().string())) == "old-content") found_backup = true;
    }
    EXPECT_TRUE(found_backup) << "备份必须落在 <state>/backup，而不是工程目录";
}

TEST_F(WriteServiceTest, RejectsUnknownRootAndTraversal) {
    std::string error;
    Root root = Root::kState;
    EXPECT_FALSE(Classify(project_, project_ + "/../outside.txt", &root, &error));
    EXPECT_FALSE(Classify(project_, "C:/Windows/win.ini", &root, &error));
}

TEST_F(WriteServiceTest, AppendLineOnlyForNonProjectFiles) {
    const auto ok =
        fpgalsp::platform::writes::AppendLine(project_, state_ + "/logs/t.log", "line-1", {});
    EXPECT_TRUE(ok.ok) << ok.error;
    EXPECT_NE(Read(state_ + "/logs/t.log").find("line-1"), std::string::npos);

    const auto rejected = fpgalsp::platform::writes::AppendLine(
        project_, project_ + "/top.al", "x", {});
    EXPECT_FALSE(rejected.ok);
}

}  // namespace
