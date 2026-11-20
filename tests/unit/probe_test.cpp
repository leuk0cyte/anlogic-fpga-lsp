// =============================================================================
//  tests/unit/probe_test.cpp —— 片上探针（ChipWatcher）纯解析 / 判定
// =============================================================================
//  这里只测**不需要硬件**的部分：
//    * chip_debugger/user.mdn 的 mark_debug 解析与回写（保留其它行）
//    * phy_1/cw/compiled.cwc 的解析（探针是否真的进了 bit 的证据）
//    * write_hw_cw_data 生成的 CSV 解析（真机抓到的就是这个格式）
//  语料全部取自本机实测文件（%WORKSPACE%\CorpusDelta\corpus_delta）的片段。
// =============================================================================

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "flow/probe.h"
#include "platform/encoding.h"

namespace {

namespace fs = std::filesystem;
using fpgalsp::probe::CaptureData;
using fpgalsp::probe::CwcSnapshot;
using fpgalsp::probe::MdnConfig;

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
        path_ += "fpgalsp_probe_" + tag;
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

void Write(const std::string& path, const std::string& content) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    (void)fpgalsp::platform::write_file_utf8(path, content);
}

// 断言失败时把整个清单打出来，省去逐条 print
std::string joined(const std::vector<std::string>& items) {
    std::string out;
    for (const std::string& item : items) {
        if (!out.empty()) out += " | ";
        out += item;
    }
    return out;
}

// 真实 user.mdn 的形态（本机 chip_debugger/user.mdn 就是一行、64 个 net）
const char* kRealMdn =
    "mark_debug { WHG/pwm_reg0_i[0] WHG/pwm_reg0_i[10] WHG/pwm_reg0_i[1] WHG/pwm_reg0_i[2] "
    "WHG/pwm_reg1_i[0] WHG/pwm_reg1_i[1] WHG/pwm_reg6_i[0] WHG/pwm_reg7_i[0] }\n";

// 真实 compiled.cwc 的结构（节选自 phy_1/cw/compiled.cwc，13,705 B）
const char* kRealCwc = R"(<?xml version="1.0" encoding="utf-8"?>
<!-- HMAC is: 1595b6792e570b10b981366c5b455ae84b8dc8bd48243054a13f616b16a2e2dd -->
<chipwatcher version="3.070000" project_name="corpus_delta" chip_name="PH1A100SFG676">
	<watcher_nodes>
		<bus name="pwm_reg0_i[15:0]" clock="reg_init/clk" inst="auto_created_cwc1" type="MARK_DEBUG_PROBE" is_enabled="yes">
			<net name="pwm_reg0_i[15]" const="false" data="enable" trig="enable"/>
			<net name="pwm_reg0_i[14]" const="false" data="enable" trig="enable"/>
		</bus>
		<bus name="pwm_reg1_i[15:0]" clock="reg_init/clk" inst="auto_created_cwc1" type="MARK_DEBUG_PROBE" is_enabled="yes">
			<net name="pwm_reg1_i[15]" const="false" data="enable" trig="enable"/>
		</bus>
	</watcher_nodes>
	<instance name="auto_created_cwc1" id="0" ipgen="false" inst_enable="true">
		<clock clk_name="reg_init/clk" clk_peroid="10ns"/>
		<config bram_name="auto_created_cwc1/wrapper_cwc_top/U_cwc_ram" debughub_name="" input_pipe_num="0" output_pipe_num="0" sample_depth="1024"/>
		<trigger name="auto_created_cwc1_trigger" position="127" window_num="1" window_depth="1024">
			<condition level="1" enabled="yes" type="basic_and" capture_type="basic_and"/>
		</trigger>
	</instance>
</chipwatcher>
)";

// 真实 write_hw_cw_data 输出（表头 + 数据列是二进制位串）
const char* kRealCsv =
    "Sample,Trigger,pwm_reg0_i[15:0],pwm_reg1_i[15:0],pwm_reg6_i[15:0],pwm_reg7_i[15:0]\n"
    "0,0,0000000000000000,0000000000000000,0000000000000000,0000000000000000\n"
    "1,0,0000000000000010,0000000000001111,0000000000000000,0000000000000110\n"
    "2,1,0000000000000100,0000000000001111,0000000000000001,0000000000001100\n"
    "3,0,0000000000000110,0000000000001111,0000000000000010,0000000000010010\n";

}  // namespace

// -----------------------------------------------------------------------------
// user.mdn
// -----------------------------------------------------------------------------

TEST(ProbeMdn, ParseRealMarkDebug) {
    const std::vector<std::string> signals = fpgalsp::probe::ParseMarkDebug(kRealMdn);
    ASSERT_EQ(signals.size(), 8u);
    EXPECT_EQ(signals[0], "WHG/pwm_reg0_i[0]");
    EXPECT_EQ(signals[1], "WHG/pwm_reg0_i[10]");
}

TEST(ProbeMdn, ParseEmptyOrMissing) {
    EXPECT_TRUE(fpgalsp::probe::ParseMarkDebug("").empty());
    EXPECT_TRUE(fpgalsp::probe::ParseMarkDebug("// nothing here\n").empty());
    // 大括号没闭合也要给出可用结果（真实工程里见过手改坏的文件）
    const std::vector<std::string> broken =
        fpgalsp::probe::ParseMarkDebug("mark_debug { WHG/a[0] WHG/a[1]\n");
    ASSERT_EQ(broken.size(), 2u);
}

TEST(ProbeMdn, NormaliseSortsByBitIndexNotLexicographically) {
    // 字典序会把 [10] 排到 [2] 前面；探针清单必须按位号，否则 diff/回写不稳定
    const std::vector<std::string> normalised = fpgalsp::probe::NormaliseSignals(
        {"WHG/b[10]", "WHG/b[2]", "WHG/b[1]", "WHG/b[2]", "  WHG/a[3]  ", ""});
    ASSERT_EQ(normalised.size(), 4u);
    EXPECT_EQ(normalised[0], "WHG/a[3]");
    EXPECT_EQ(normalised[1], "WHG/b[1]");
    EXPECT_EQ(normalised[2], "WHG/b[2]");
    EXPECT_EQ(normalised[3], "WHG/b[10]");
}

TEST(ProbeMdn, NormaliseKeepsLongHierarchicalNames) {
    // 回归：曾经用 char[32] 拼名字，38 字符的层次名被 snprintf 截断，
    // 16 个 counter 位被并成一个 `WHG/ANPC_init0/ANPC_SVPWM_init/`，写进 user.mdn 就废了。
    const std::vector<std::string> normalised = fpgalsp::probe::NormaliseSignals(
        {"WHG/ANPC_init0/ANPC_SVPWM_init/cnt[15]", "WHG/ANPC_init0/ANPC_SVPWM_init/cnt[0]",
         "WHG/pwm_reg0_i[1]"});
    ASSERT_EQ(normalised.size(), 3u) << joined(normalised);
    EXPECT_EQ(normalised[0], "WHG/ANPC_init0/ANPC_SVPWM_init/cnt[0]");
    EXPECT_EQ(normalised[1], "WHG/ANPC_init0/ANPC_SVPWM_init/cnt[15]");
    EXPECT_EQ(normalised[2], "WHG/pwm_reg0_i[1]");
}

TEST(ProbeMdn, RenderReplacesCommandAndKeepsOtherLines) {
    const std::string original =
        "# hand written note\n"
        "mark_debug { WHG/old[0] WHG/old[1] }\n"
        "mark_debug { WHG/second[0] }\n";
    const std::string rendered = fpgalsp::probe::RenderMdn(original, {"WHG/new[0]"});
    EXPECT_NE(rendered.find("# hand written note"), std::string::npos) << "其它行必须原样保留";
    EXPECT_NE(rendered.find("mark_debug { WHG/new[0] }"), std::string::npos);
    // 只替换第一条 mark_debug，第二条不动
    EXPECT_NE(rendered.find("mark_debug { WHG/second[0] }"), std::string::npos);
    EXPECT_EQ(rendered.find("WHG/old[0]"), std::string::npos);
}

TEST(ProbeMdn, RenderAppendsWhenMissing) {
    const std::string rendered = fpgalsp::probe::RenderMdn("# only a comment\n", {"WHG/a[0]"});
    EXPECT_NE(rendered.find("# only a comment"), std::string::npos);
    EXPECT_NE(rendered.find("mark_debug { WHG/a[0] }"), std::string::npos);
}

TEST(ProbeMdn, RenderEmptySignalsDropsTheCommand) {
    const std::string rendered = fpgalsp::probe::RenderMdn(kRealMdn, {});
    EXPECT_EQ(rendered.find("mark_debug"), std::string::npos);
}

TEST(ProbeMdn, ReadAndWriteRoundTrip) {
    ScopedTempDir temp("mdn");
    const std::string path = temp.path() + "/chip_debugger/user.mdn";
    Write(path, kRealMdn);

    const MdnConfig config = fpgalsp::probe::ReadMdn(path);
    ASSERT_TRUE(config.ok) << config.error;
    EXPECT_TRUE(config.exists);
    EXPECT_EQ(config.signals.size(), 8u);

    std::string error;
    ASSERT_TRUE(fpgalsp::probe::WriteMdn(path, {"WHG/pwm_reg0_i[0]"}, &error)) << error;
    const MdnConfig after = fpgalsp::probe::ReadMdn(path);
    ASSERT_TRUE(after.ok);
    ASSERT_EQ(after.signals.size(), 1u);
    EXPECT_EQ(after.signals[0], "WHG/pwm_reg0_i[0]");
}

// -----------------------------------------------------------------------------
// compiled.cwc
// -----------------------------------------------------------------------------

TEST(ProbeCwc, ParseRealSnippet) {
    const CwcSnapshot snapshot = fpgalsp::probe::ParseCwc(kRealCwc);
    ASSERT_TRUE(snapshot.ok) << snapshot.error;
    EXPECT_EQ(snapshot.version, "3.070000");
    EXPECT_EQ(snapshot.project, "corpus_delta");
    EXPECT_EQ(snapshot.chip, "PH1A100SFG676");
    EXPECT_EQ(snapshot.clock, "reg_init/clk");
    EXPECT_EQ(snapshot.instance, "auto_created_cwc1");
    EXPECT_EQ(snapshot.sample_depth, 1024);
    EXPECT_EQ(snapshot.trigger_position, 127);
    EXPECT_EQ(snapshot.window_num, 1);
    EXPECT_EQ(snapshot.window_depth, 1024);
    EXPECT_EQ(snapshot.trigger_type, "basic_and");
    EXPECT_TRUE(snapshot.trigger_enabled);
    ASSERT_EQ(snapshot.buses.size(), 2u);
    EXPECT_EQ(snapshot.buses[0].name, "pwm_reg0_i[15:0]");
    EXPECT_EQ(snapshot.buses[0].type, "MARK_DEBUG_PROBE");
    EXPECT_EQ(snapshot.buses[0].instance, "auto_created_cwc1");
    EXPECT_TRUE(snapshot.buses[0].enabled);
    EXPECT_EQ(snapshot.net_count, 3);
}

TEST(ProbeCwc, RejectsNonCwcText) {
    EXPECT_FALSE(fpgalsp::probe::ParseCwc("").ok);
    EXPECT_FALSE(fpgalsp::probe::ParseCwc("hello\n").ok);
    // 有根节点但没有 bus = 没探针
    const CwcSnapshot empty = fpgalsp::probe::ParseCwc("<chipwatcher version=\"3.0\"></chipwatcher>");
    EXPECT_FALSE(empty.ok);
    EXPECT_FALSE(empty.error.empty());
}

// -----------------------------------------------------------------------------
// 采集 CSV
// -----------------------------------------------------------------------------

TEST(ProbeCaptureCsv, ParseRealHeaderAndValues) {
    const CaptureData data = fpgalsp::probe::ParseCaptureCsv(kRealCsv);
    ASSERT_TRUE(data.ok) << data.error;
    ASSERT_EQ(data.columns.size(), 4u);
    EXPECT_EQ(data.columns[0].bus, "pwm_reg0_i");
    EXPECT_EQ(data.columns[0].width, 16);
    EXPECT_EQ(data.columns[0].msb, 15);
    EXPECT_EQ(data.columns[0].lsb, 0);
    ASSERT_EQ(data.rows.size(), 4u);

    EXPECT_EQ(data.rows[0].index, 0);
    EXPECT_FALSE(data.rows[0].trigger);
    EXPECT_EQ(data.rows[0].values[0], 0u);

    EXPECT_EQ(data.rows[1].values[0], 2u);
    EXPECT_EQ(data.rows[1].values[1], 15u);
    EXPECT_EQ(data.rows[1].values[3], 6u);

    EXPECT_EQ(data.trigger_index, 2) << "Trigger 列唯一为 1 的那个采样点";
    EXPECT_TRUE(data.rows[2].trigger);
}

TEST(ProbeCaptureCsv, ToleratesBomAndTrailingNewline) {
    const std::string text = std::string("\xEF\xBB\xBF") +
                             "Sample,Trigger,WHG/a[7:0]\n"
                             "0,0,00000001\n"
                             "1,0,11111111\n";
    const CaptureData data = fpgalsp::probe::ParseCaptureCsv(text);
    ASSERT_TRUE(data.ok) << data.error;
    ASSERT_EQ(data.columns.size(), 1u);
    EXPECT_EQ(data.columns[0].bus, "WHG/a");
    EXPECT_EQ(data.columns[0].width, 8);
    ASSERT_EQ(data.rows.size(), 2u);
    EXPECT_EQ(data.rows[0].values[0], 1u);
    EXPECT_EQ(data.rows[1].values[0], 255u);
}

TEST(ProbeCaptureCsv, RejectsEmptyInput) {
    EXPECT_FALSE(fpgalsp::probe::ParseCaptureCsv("").ok);
    EXPECT_FALSE(fpgalsp::probe::ParseCaptureCsv("Sample,Trigger\n").ok)
        << "只有表头没有数据行 -> 不算成功";
}

// -----------------------------------------------------------------------------
// Status / Configure（不碰硬件）
// -----------------------------------------------------------------------------

namespace {

fpgalsp::project::Project MakeProject(const std::string& dir) {
    fpgalsp::project::Project project;
    project.dir = dir;
    project.name = "corpus_delta";
    project.al_path = dir + "/corpus_delta.al";
    project.family = "PH1";
    project.device = "PH1A100SFG676";
    project.top = "WHG";
    return project;
}

}  // namespace

TEST(ProbeStatus, ReportsNotReadyWithoutWatcher) {
    ScopedTempDir temp("status_empty");
    const fpgalsp::project::Project project = MakeProject(temp.path());
    const fpgalsp::jsonrpc::Json status = fpgalsp::probe::Status(project);
    EXPECT_FALSE(status.value("ok", false));
    EXPECT_FALSE(status.value("ready", false));
    const auto mdn = status["chip_debugger"];
    ASSERT_TRUE(mdn.is_object());
    EXPECT_FALSE(mdn.value("exists", true));
    const auto watcher = status["watcher"];
    ASSERT_TRUE(watcher.is_object());
    EXPECT_FALSE(watcher.value("present", true));
}

TEST(ProbeStatus, ReportsReadyWithMdnAndCompiledWatcher) {
    ScopedTempDir temp("status_ready");
    const fpgalsp::project::Project project = MakeProject(temp.path());
    Write(fpgalsp::probe::MdnPath(project), kRealMdn);
    Write(fpgalsp::probe::CompiledCwc(project), kRealCwc);
    Write(fpgalsp::probe::CompiledBit(project), "bit");

    const fpgalsp::jsonrpc::Json status = fpgalsp::probe::Status(project);
    EXPECT_TRUE(status.value("ok", false)) << status.dump();
    EXPECT_TRUE(status.value("ready", false));
    const auto watcher = status["watcher"];
    EXPECT_TRUE(watcher.value("present", false));
    EXPECT_EQ(watcher.value("clock", std::string()), "reg_init/clk");
    EXPECT_EQ(watcher.value("instance", std::string()), "auto_created_cwc1");
    EXPECT_EQ(watcher.value("sample_depth", 0), 1024);
    EXPECT_EQ(watcher.value("net_count", 0), 3);
    const auto mdn = status["chip_debugger"];
    EXPECT_EQ(mdn.value("count", 0), 8);
}

TEST(ProbeConfigure, DryRunDoesNotTouchTheFile) {
    ScopedTempDir temp("config_dry");
    const fpgalsp::project::Project project = MakeProject(temp.path());
    Write(fpgalsp::probe::MdnPath(project), kRealMdn);
    const std::string before =
        fpgalsp::platform::try_read_file_utf8(fpgalsp::probe::MdnPath(project)).value_or("");

    const fpgalsp::jsonrpc::Json result =
        fpgalsp::probe::Configure(project, {"WHG/ANPC_init0/ANPC_SVPWM_init/cnt[0]"}, false);
    EXPECT_TRUE(result.value("ok", false)) << result.dump();
    EXPECT_FALSE(result.value("written", true));
    EXPECT_EQ(result.value("count", 0), 1);
    const std::string after =
        fpgalsp::platform::try_read_file_utf8(fpgalsp::probe::MdnPath(project)).value_or("");
    EXPECT_EQ(before, after) << "write:false 时绝对不能落盘";
}

TEST(ProbeConfigure, WritesAndReportsAddedAndRemoved) {
    ScopedTempDir temp("config_write");
    const fpgalsp::project::Project project = MakeProject(temp.path());
    Write(fpgalsp::probe::MdnPath(project), kRealMdn);

    const fpgalsp::jsonrpc::Json result =
        fpgalsp::probe::Configure(project, {"WHG/keep[0]", "WHG/keep[1]"}, true);
    EXPECT_TRUE(result.value("ok", false)) << result.dump();
    EXPECT_TRUE(result.value("written", false));
    EXPECT_EQ(result.value("count", 0), 2);
    ASSERT_FALSE(result["added"].empty());
    ASSERT_FALSE(result["removed"].empty());

    const MdnConfig after = fpgalsp::probe::ReadMdn(fpgalsp::probe::MdnPath(project));
    ASSERT_TRUE(after.ok);
    ASSERT_EQ(after.signals.size(), 2u);
    EXPECT_EQ(after.signals[0], "WHG/keep[0]");
}

TEST(ProbeConfigure, RefusesToWriteAnEmptyList) {
    ScopedTempDir temp("config_empty");
    const fpgalsp::project::Project project = MakeProject(temp.path());
    Write(fpgalsp::probe::MdnPath(project), kRealMdn);
    // signals 为空 = 读取当前清单（dry run），不会清空
    const fpgalsp::jsonrpc::Json read = fpgalsp::probe::Configure(project, {}, false);
    EXPECT_TRUE(read.value("ok", false)) << read.dump();
    EXPECT_EQ(read.value("count", 0), 8);
}