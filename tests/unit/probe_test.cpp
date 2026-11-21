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

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "flow/probe.h"
#include "platform/encoding.h"
#include "platform/path.h"

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

// 写一份"清单与 bit 完全一致"的探针配置（kRealCwc 里的两条 bus 各 16 位）。
// 供 status / verify 的 ready 场景共用，避免两边手抄同一份 fixture 抄歪。
std::vector<std::string> MatchingSignals() {
    std::vector<std::string> signals;
    for (const char* bus : {"pwm_reg0_i", "pwm_reg1_i"}) {
        for (int bit = 0; bit < 16; ++bit) {
            signals.push_back(std::string("WHG/") + bus + "[" + std::to_string(bit) + "]");
        }
    }
    return signals;
}

std::string MarkDebugLine(const std::vector<std::string>& signals) {
    std::string text = "mark_debug { ";
    for (const std::string& signal : signals) {
        if (signal.empty()) continue;
        text += signal;
        text += ' ';
    }
    text += "}\n";
    return text;
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
    // 清单必须与 kRealCwc 里那两条 bus 逐位一致，否则会被判成 stale
    std::vector<std::string> matching;
    for (const char* bus : {"pwm_reg0_i", "pwm_reg1_i"}) {
        for (int bit = 0; bit < 16; ++bit) {
            matching.push_back(std::string("WHG/") + bus + "[" + std::to_string(bit) + "]");
        }
    }
    Write(fpgalsp::probe::MdnPath(project),
          "mark_debug { " + [&matching] {
              std::string text;
              for (const std::string& signal : matching) {
                  if (!text.empty()) text += ' ';
                  text += signal;
              }
              return text;
          }() + " }\n");
    Write(fpgalsp::probe::CompiledCwc(project), kRealCwc);
    Write(fpgalsp::probe::CompiledBit(project), "bit");

    const fpgalsp::jsonrpc::Json status = fpgalsp::probe::Status(project);
    EXPECT_TRUE(status.value("ok", false)) << status.dump();
    EXPECT_TRUE(status.value("ready", false)) << status.dump();
    EXPECT_TRUE(status.value("matches_user_mdn", false));
    const auto watcher = status["watcher"];
    EXPECT_TRUE(watcher.value("present", false));
    EXPECT_FALSE(watcher.value("stale", true));
    EXPECT_EQ(watcher.value("clock", std::string()), "reg_init/clk");
    EXPECT_EQ(watcher.value("instance", std::string()), "auto_created_cwc1");
    EXPECT_EQ(watcher.value("sample_depth", 0), 1024);
    EXPECT_EQ(watcher.value("net_count", 0), 3);
    const auto mdn = status["chip_debugger"];
    EXPECT_EQ(mdn.value("count", 0), 32);
}

// ⚠ 最容易骗过用户的场景：改了 user.mdn 但只跑了增量布线 -> bit 里还是旧探针。
//   必须报 stale 并列出差异 base，否则用户会以为探针加上了。
TEST(ProbeStatus, ReportsStaleWhenMdnNoLongerMatchesTheBit) {
    ScopedTempDir temp("status_stale");
    const fpgalsp::project::Project project = MakeProject(temp.path());
    Write(fpgalsp::probe::MdnPath(project), kRealMdn);
    Write(fpgalsp::probe::CompiledCwc(project), kRealCwc);
    Write(fpgalsp::probe::CompiledBit(project), "bit");

    const fpgalsp::jsonrpc::Json status = fpgalsp::probe::Status(project);
    EXPECT_FALSE(status.value("ok", false));
    EXPECT_FALSE(status.value("ready", false));
    EXPECT_FALSE(status.value("matches_user_mdn", true));
    const auto watcher = status["watcher"];
    EXPECT_TRUE(watcher.value("stale", false));
    // kRealMdn 里有 reg6/reg7，而 kRealCwc 只有 reg0/reg1
    const auto missing = watcher["missing_in_bit"];
    ASSERT_TRUE(missing.is_array());
    EXPECT_GE(missing.size(), 2u) << missing.dump();
    EXPECT_NE(status.value("reason", std::string()).find("incremental route"), std::string::npos);
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

// -----------------------------------------------------------------------------
// sampling：采样时钟/深度是编译期属性（只读，editable 恒 false）
// -----------------------------------------------------------------------------

TEST(ProbeStatus, SamplingIsReportedAsImmutable) {
    ScopedTempDir temp("status_sampling");
    const fpgalsp::project::Project project = MakeProject(temp.path());
    Write(fpgalsp::probe::MdnPath(project), MarkDebugLine(MatchingSignals()));
    Write(fpgalsp::probe::CompiledCwc(project), kRealCwc);
    Write(fpgalsp::probe::CompiledBit(project), "bit");

    const fpgalsp::jsonrpc::Json status = fpgalsp::probe::Status(project);
    ASSERT_TRUE(status.contains("sampling")) << status.dump();
    const auto& sampling = status["sampling"];
    ASSERT_TRUE(sampling.is_object());
    EXPECT_EQ(sampling.value("clock", std::string()), "reg_init/clk");
    EXPECT_EQ(sampling.value("depth", 0), 1024);
    EXPECT_FALSE(sampling.value("editable", true)) << "采样时钟/深度不可在采集期改";
    // reason 是给 agent 看的英文原句：必须点明"编译期 + CLI 没有 setter"
    const std::string reason = sampling.value("reason", std::string());
    EXPECT_NE(reason.find("compile-time"), std::string::npos) << reason;
    EXPECT_NE(reason.find("never rewrites compiled.cwc"), std::string::npos) << reason;
    // 平铺字段必须保留（测试与插件依赖）
    EXPECT_EQ(status["watcher"].value("clock", std::string()), "reg_init/clk");
    EXPECT_EQ(status["watcher"].value("sample_depth", 0), 1024);
}

// -----------------------------------------------------------------------------
// Verify：逐位比对 mdn 与 compiled.cwc
// -----------------------------------------------------------------------------

TEST(ProbeVerify, ReportsNoBitstreamWhenWatcherIsMissing) {
    ScopedTempDir temp("verify_nocwc");
    const fpgalsp::project::Project project = MakeProject(temp.path());
    Write(fpgalsp::probe::MdnPath(project), "mark_debug { WHG/cnt[0] WHG/cnt[1] }\n");

    const fpgalsp::jsonrpc::Json result = fpgalsp::probe::Verify(project, false);
    EXPECT_TRUE(result.value("ok", false)) << result.dump();
    EXPECT_FALSE(result.value("stale", true));
    const auto& signals = result["signals"];
    EXPECT_EQ(signals.value("requested", 0), 2);
    EXPECT_EQ(signals.value("embedded", 0), 0);
    EXPECT_EQ(signals.value("missing", 0), 2);
    ASSERT_EQ(result["buses"].size(), 1u);
    EXPECT_EQ(result["buses"][0].value("name", std::string()), "cnt");
    EXPECT_EQ(result["buses"][0].value("width", 0), 2);
    EXPECT_EQ(result["buses"][0].value("status", std::string()), "dropped");
    EXPECT_EQ(result["buses"][0].value("reason", std::string()), "no-bitstream");
    EXPECT_EQ(result["missing_nets"].size(), 2u);
    EXPECT_FALSE(result["sampling"].value("editable", true));
    const std::string hints = result["hints"].dump();
    EXPECT_NE(hints.find("run phy"), std::string::npos) << hints;
}

TEST(ProbeVerify, SplitsPartialAndDroppedBusesAndFlagsStale) {
    ScopedTempDir temp("verify_partial");
    const fpgalsp::project::Project project = MakeProject(temp.path());
    // kRealMdn 请求 reg0(0,1,2,10) / reg1(0,1) / reg6[0] / reg7[0]，
    // kRealCwc 只嵌了 reg0 全 16 位与 reg1 全 16 位 -> reg6/reg7 被丢。
    Write(fpgalsp::probe::MdnPath(project), kRealMdn);
    Write(fpgalsp::probe::CompiledCwc(project), kRealCwc);

    const fpgalsp::jsonrpc::Json result = fpgalsp::probe::Verify(project, true);
    EXPECT_TRUE(result.value("ok", false)) << result.dump();
    EXPECT_TRUE(result.value("stale", false)) << "mdn 与 bit 不一致必须报 stale";
    const auto& signals = result["signals"];
    EXPECT_EQ(signals.value("requested", 0), 8);
    EXPECT_EQ(signals.value("embedded", 0), 6);
    EXPECT_EQ(signals.value("missing", 0), 2);

    ASSERT_EQ(result["buses"].size(), 4u);
    bool reg0_embedded = false;
    bool reg6_dropped = false;
    for (const auto& bus : result["buses"]) {
        const std::string name = bus.value("name", std::string());
        if (name == "pwm_reg0_i") {
            reg0_embedded = bus.value("status", std::string()) == "embedded";
            EXPECT_EQ(bus.value("embedded_bits", -1), 4);
            EXPECT_EQ(bus.value("missing_bits", -1), 0);
        }
        if (name == "pwm_reg6_i") {
            reg6_dropped = bus.value("status", std::string()) == "dropped";
            EXPECT_EQ(bus.value("embedded_bits", -1), 0);
            EXPECT_EQ(bus.value("missing_bits", -1), 1);
            // 没传 known_name -> 无法判定名字是否存在，按 dropped-by-td 报
            EXPECT_EQ(bus.value("reason", std::string()), "dropped-by-td");
        }
    }
    EXPECT_TRUE(reg0_embedded) << result["buses"].dump();
    EXPECT_TRUE(reg6_dropped) << result["buses"].dump();

    const auto& missing = result["missing_nets"];
    ASSERT_EQ(missing.size(), 2u) << result.dump();
    EXPECT_EQ(missing[0].get<std::string>(), "WHG/pwm_reg6_i[0]");
    EXPECT_EQ(missing[1].get<std::string>(), "WHG/pwm_reg7_i[0]");
    EXPECT_FALSE(result.value("truncated", true));
    EXPECT_NE(result["hints"].dump().find("cross-clock-domain"), std::string::npos);
}

TEST(ProbeVerify, KnownNameCallbackDistinguishesNotFoundFromDropped) {
    ScopedTempDir temp("verify_known");
    const fpgalsp::project::Project project = MakeProject(temp.path());
    Write(fpgalsp::probe::MdnPath(project), "mark_debug { WHG/typo_sig[0] }\n");
    Write(fpgalsp::probe::CompiledCwc(project), kRealCwc);

    const auto result = fpgalsp::probe::Verify(
        project, false, [](std::string_view name) { return name != "typo_sig"; });
    ASSERT_EQ(result["buses"].size(), 1u);
    EXPECT_EQ(result["buses"][0].value("reason", std::string()), "not-in-build");
    EXPECT_NE(result["hints"].dump().find("fpga_symbol_path"), std::string::npos);

    const auto dropped = fpgalsp::probe::Verify(
        project, false, [](std::string_view) { return true; });
    EXPECT_EQ(dropped["buses"][0].value("reason", std::string()), "dropped-by-td");
}

TEST(ProbeVerify, TruncatesMissingNetsUnlessIncludeBits) {
    ScopedTempDir temp("verify_truncate");
    const fpgalsp::project::Project project = MakeProject(temp.path());
    std::vector<std::string> signals;
    for (int bit = 0; bit < 40; ++bit) {
        signals.push_back("WHG/gone[" + std::to_string(bit) + "]");
    }
    Write(fpgalsp::probe::MdnPath(project), MarkDebugLine(signals));
    Write(fpgalsp::probe::CompiledCwc(project), kRealCwc);

    const auto capped = fpgalsp::probe::Verify(project, false);
    EXPECT_EQ(capped["missing_nets"].size(), 32u) << "默认只给前 32 条";
    EXPECT_TRUE(capped.value("truncated", false));

    const auto full = fpgalsp::probe::Verify(project, true);
    EXPECT_EQ(full["missing_nets"].size(), 40u);
    EXPECT_FALSE(full.value("truncated", true));
}

// -----------------------------------------------------------------------------
// disabled：被 disable 的条目不占 debug 资源，但仍要写进 XML（GUI/回读一致）
// -----------------------------------------------------------------------------

TEST(ProbeDisabled, ResolvesBusBaseSingleBitAndRangeEntries) {
    // 整条总线（WHG/e2e_hstat 形态）、单根、范围三种写法都要能落到具体位上
    const fpgalsp::probe::DisabledPlan plan = fpgalsp::probe::ResolveDisabled(
        MatchingSignals(), {"WHG/pwm_reg0_i", "WHG/pwm_reg1_i[3]", "WHG/pwm_reg1_i[6:4]"});
    EXPECT_TRUE(plan.unresolved.empty()) << joined(plan.unresolved);
    EXPECT_EQ(plan.bits.size(), 16u + 1u + 3u);
    EXPECT_TRUE(plan.IsBitDisabled("WHG/pwm_reg0_i[0]"));
    EXPECT_TRUE(plan.IsBitDisabled("WHG/pwm_reg0_i[15]"));
    EXPECT_TRUE(plan.IsBitDisabled("WHG/pwm_reg1_i[3]"));
    EXPECT_TRUE(plan.IsBitDisabled("WHG/pwm_reg1_i[4]"));
    EXPECT_TRUE(plan.IsBitDisabled("WHG/pwm_reg1_i[6]"));
    EXPECT_FALSE(plan.IsBitDisabled("WHG/pwm_reg1_i[7]"));
    // 整条总线保持基名，单根/范围展开成逐位
    EXPECT_NE(std::find(plan.entries.begin(), plan.entries.end(), "WHG/pwm_reg0_i"),
              plan.entries.end());
    EXPECT_NE(std::find(plan.entries.begin(), plan.entries.end(), "WHG/pwm_reg1_i[3]"),
              plan.entries.end());
}

TEST(ProbeDisabled, ReportsUnresolvedEntriesWithoutFailing) {
    const fpgalsp::probe::DisabledPlan plan =
        fpgalsp::probe::ResolveDisabled(MatchingSignals(), {"WHG/typo_sig", "WHG/pwm_reg0_i"});
    EXPECT_TRUE(plan.IsBitDisabled("WHG/pwm_reg0_i[0]"));
    ASSERT_EQ(plan.unresolved.size(), 1u);
    EXPECT_EQ(plan.unresolved[0], "WHG/typo_sig");
}

TEST(ProbeConfigure, ReportsDisabledAndEnabledCounts) {
    ScopedTempDir temp("config_disabled");
    const fpgalsp::project::Project project = MakeProject(temp.path());
    Write(fpgalsp::probe::MdnPath(project), MarkDebugLine(MatchingSignals()));

    const auto result = fpgalsp::probe::Configure(project, {}, false, false, std::string(), 0,
                                                  {"WHG/pwm_reg0_i"}, true);
    EXPECT_TRUE(result.value("ok", false)) << result.dump();
    ASSERT_EQ(result["disabled"].size(), 1u) << result.dump();
    EXPECT_EQ(result["disabled"][0].get<std::string>(), "WHG/pwm_reg0_i");
    EXPECT_EQ(result.value("disabled_count", 0), 16);
    // bit_count/bus_count 保持"清单全量"的旧语义，enabled_* 才是真正占资源的量
    EXPECT_EQ(result.value("bit_count", 0), 32);
    EXPECT_EQ(result.value("bus_count", 0), 2);
    EXPECT_EQ(result.value("enabled_bit_count", 0), 16);
    EXPECT_EQ(result.value("enabled_bus_count", 0), 1);
    EXPECT_TRUE(result.value("requires_apply_build_config", false))
        << "disable 只能靠自写的 cwc 表达，必须提示 apply_build_config";
    // dry run：config.txt 不落盘
    EXPECT_FALSE(std::filesystem::exists(fpgalsp::probe::ProbeConfigPath(project)));
}

TEST(ProbeConfigure, WarnsButDoesNotFailOnUnresolvedDisabled) {
    ScopedTempDir temp("config_disabled_unresolved");
    const fpgalsp::project::Project project = MakeProject(temp.path());
    Write(fpgalsp::probe::MdnPath(project), MarkDebugLine(MatchingSignals()));

    const auto result = fpgalsp::probe::Configure(project, {}, false, false, std::string(), 0,
                                                  {"WHG/not_here"}, true);
    EXPECT_TRUE(result.value("ok", false)) << result.dump();
    EXPECT_EQ(result.value("disabled_count", 0), 0);
    ASSERT_TRUE(result.contains("unresolved_disabled")) << result.dump();
    EXPECT_EQ(result["unresolved_disabled"][0].get<std::string>(), "WHG/not_here");
    EXPECT_NE(result.value("warning", std::string()).find("not part of the probe list"),
              std::string::npos);
}

TEST(ProbeConfigure, PersistsDisabledIntoConfigTxtAndReadsItBack) {
    ScopedTempDir temp("config_disabled_persist");
    const fpgalsp::project::Project project = MakeProject(temp.path());
    Write(fpgalsp::probe::MdnPath(project), MarkDebugLine(MatchingSignals()));

    const auto written = fpgalsp::probe::Configure(project, MatchingSignals(), true, false,
                                                   std::string(), 0, {"WHG/pwm_reg1_i[7]"}, true);
    ASSERT_TRUE(written.value("written", false)) << written.dump();
    const fpgalsp::probe::ProbeBuildConfig config =
        fpgalsp::probe::ReadProbeBuildConfig(project);
    ASSERT_EQ(config.disabled.size(), 1u) << fpgalsp::probe::ProbeConfigPath(project);
    EXPECT_EQ(config.disabled[0], "WHG/pwm_reg1_i[7]");

    // 不给 disabled 参数 = 沿用落盘的那份（按当前清单重新解析）
    const auto read = fpgalsp::probe::Configure(project, {}, false);
    EXPECT_EQ(read.value("disabled_count", 0), 1);
    ASSERT_EQ(read["disabled"].size(), 1u) << read.dump();
    EXPECT_EQ(read["disabled"][0].get<std::string>(), "WHG/pwm_reg1_i[7]");
}

TEST(ProbeCwc, RendersDisabledBusesAndNets) {
    ScopedTempDir temp("cwc_disabled");
    const fpgalsp::project::Project project = MakeProject(temp.path());
    Write(fpgalsp::probe::MdnPath(project), MarkDebugLine(MatchingSignals()));

    fpgalsp::probe::ProbeBuildConfig config;
    config.has_clock = true;
    config.clock = "reg_init/clk";
    config.has_depth = true;
    config.depth = 1024;
    // 整条 disabled + 单根 disabled 两种形态一起覆盖
    config.disabled = {"WHG/pwm_reg0_i", "WHG/pwm_reg1_i[3]"};
    std::string error;
    fpgalsp::jsonrpc::Json info = fpgalsp::jsonrpc::Json::object();
    const std::string text = fpgalsp::probe::BuildCwcFromConfig(
        project, fpgalsp::probe::ReadMdn(fpgalsp::probe::MdnPath(project)), config, &error, &info,
        /*include_eram=*/false);
    ASSERT_FALSE(text.empty()) << error;

    // bus 节点：整条 disable 的 is_enabled="no"，只 disable 一根的仍是 "yes"
    EXPECT_NE(text.find("<bus name=\"pwm_reg0_i[15:0]\" clock=\"reg_init/clk\""),
              std::string::npos) << "disabled 的 bus 仍必须出现在 XML 里";
    EXPECT_NE(text.find("type=\"MARK_DEBUG_PROBE\" is_enabled=\"no\""), std::string::npos);
    EXPECT_NE(text.find("type=\"MARK_DEBUG_PROBE\" is_enabled=\"yes\""), std::string::npos);
    // net 节点：data/trig 与 signal_vec 的 cond/capture_cond
    EXPECT_NE(text.find("<net name=\"pwm_reg1_i[3]\" const=\"false\" data=\"disable\" "
                        "trig=\"disable\"/>"),
              std::string::npos);
    EXPECT_NE(text.find("<net name=\"pwm_reg1_i[2]\" const=\"false\" data=\"enable\" "
                        "trig=\"enable\"/>"),
              std::string::npos);
    EXPECT_NE(text.find("<net name=\"pwm_reg1_i[3]\" cond=\"dont_care\" "
                        "capture_cond=\"dont_care\"/>"),
              std::string::npos);
    EXPECT_NE(text.find("<net name=\"pwm_reg1_i[2]\" cond=\"low\" capture_cond=\"low\"/>"),
              std::string::npos);

    // 回读：cwc 里的 disable 数量（GUI/回读一致性）
    const fpgalsp::probe::CwcSnapshot snapshot = fpgalsp::probe::ParseCwc(text);
    ASSERT_TRUE(snapshot.ok) << snapshot.error;
    ASSERT_EQ(snapshot.buses.size(), 2u);
    EXPECT_FALSE(snapshot.buses[0].enabled) << "整条 disabled 的 bus";
    EXPECT_TRUE(snapshot.buses[1].enabled) << "只有 1 根被 disable 的 bus 仍 enabled";
    EXPECT_EQ(snapshot.disabled_bus_count, 1);
    EXPECT_EQ(snapshot.disabled_net_count, 16 + 1);

    // disabled 的 bit 不参与位宽/位序：32 - (16 + 1) = 15
    EXPECT_EQ(info.value("bits", 0), 32);
    EXPECT_EQ(info.value("enabled_bits", 0), 15);
    EXPECT_EQ(info.value("disabled_bits", 0), 17);
}

// BuildCwcFromWatcher 是 phy 之后写 sidecar / 抓取前补 cwc 的那条路径（不是 insert 输入）。
// 它必须用同一套 disabled 语义渲染，同时用"只剩 enable 位"的投影校验 TD 的
// CWC_BUS_*（被 disable 的条目不占 debug 资源，TD 可能把它们从 watcher 里摘掉）。
TEST(ProbeCwc, WatcherPathRendersDisabledAndChecksOnlyEnabledWidths) {
    ScopedTempDir temp("cwc_watcher_disabled");
    const fpgalsp::project::Project project = MakeProject(temp.path());
    Write(fpgalsp::probe::MdnPath(project), MarkDebugLine(MatchingSignals()));

    // 采样时钟只能从历史 cwc 取（ResolveSampleClock 读 `clock="..."`）
    // 状态目录已迁到系统 temp（platform::state_dir）；测试跟着写同一处
    Write(fpgalsp::platform::state_dir(project.dir) + "/probe/history.cwc",
          "<chipwatcher><watcher_nodes>"
          "<bus name=\"pwm_reg0_i[15:0]\" clock=\"reg_init/clk\" inst=\"auto_created_cwc1\" "
          "type=\"MARK_DEBUG_PROBE\" is_enabled=\"yes\"/></watcher_nodes></chipwatcher>\n");

    // TD 的 watcher 产物：整条 disabled 的 pwm_reg0_i 已被摘掉，只剩 pwm_reg1_i。
    // 部分 disabled 的 bus 保留全宽（16），DIN 计数落在 [15,16] —— 容差检查要接住。
    const std::string watcher_dir =
        project.dir + "/" + project.name + "_Runs/syn_1/cw/auto_created_cwc1";
    Write(watcher_dir + "/auto_created_cwc1_watcherInst.sv",
          "module auto_created_cwc1 ();\n"
          "    localparam CWC_BUS_NUM = 1;\n"
          "    localparam CWC_BUS_DIN_NUM = 16;\n"
          "    localparam RAM_LEN = 16;\n"
          "    localparam INPUT_PIPE_NUM = 0;\n"
          "    localparam OUTPUT_PIPE_NUM = 0;\n"
          "    localparam RAM_DATA_DEPTH = 1024;\n"
          "    localparam integer CWC_BUS_WIDTH[0:0] = {16};\n"
          "    localparam integer CWC_BUS_DIN_POS[0:0] = {0};\n"
          "endmodule\n");
    Write(project.dir + "/" + project.name + "_Runs/phy_1/" + project.name + "_inst.bid",
          "<INST_ERAM>\n"
          "<name>auto_created_cwc1/wrapper_cwc_top/U_cwc_ram/mem_syn_11</name>\n"
          "<rid>0X00000241</rid>\n<wid>0X00000241</wid>\n<model_type>2</model_type>\n"
          "<width_a>20</width_a>\n<width_b>20</width_b>\n"
          "<is_debuggable>enable</is_debuggable>\n<is_initialize>enable</is_initialize>\n"
          "<sub_bid_info>\n<width>20</width>\n<depth>1024</depth>\n"
          "<address_offset>0</address_offset>\n<data_offset>0</data_offset>\n"
          "<num_section>1</num_section>\n<section_size>65</section_size>\n"
          "<width_per_section>20</width_per_section>\n"
          "<bytes_in_per_section>1</bytes_in_per_section>\n"
          "<address_step>1</address_step>\n<num_byte>2</num_byte>\n<ecc>0</ecc>\n"
          "</sub_bid_info>\n</INST_ERAM>\n");

    fpgalsp::probe::ProbeBuildConfig config;
    config.disabled = {"WHG/pwm_reg0_i", "WHG/pwm_reg1_i[3]"};
    ASSERT_TRUE(fpgalsp::probe::WriteProbeBuildConfig(project, config));

    std::string error;
    fpgalsp::jsonrpc::Json info = fpgalsp::jsonrpc::Json::object();
    const std::string text = fpgalsp::probe::BuildCwcFromWatcher(
        project, fpgalsp::probe::ReadMdn(fpgalsp::probe::MdnPath(project)), {}, &error, &info);
    ASSERT_FALSE(text.empty()) << error;

    EXPECT_EQ(info.value("enabled_bits", 0), 15);
    EXPECT_EQ(info.value("bits", 0), 32);
    const fpgalsp::probe::CwcSnapshot snapshot = fpgalsp::probe::ParseCwc(text);
    ASSERT_TRUE(snapshot.ok) << snapshot.error;
    ASSERT_EQ(snapshot.buses.size(), 2u) << "disabled 的 bus 仍要写进 XML";
    EXPECT_FALSE(snapshot.buses[0].enabled);
    EXPECT_TRUE(snapshot.buses[1].enabled);
    EXPECT_EQ(snapshot.disabled_bus_count, 1);
    EXPECT_EQ(snapshot.disabled_net_count, 17);
    // ram width 用 TD 里的 RAM_LEN（16），而不是 32
    EXPECT_NE(text.find("width=\"16\" depth=\"1024\""), std::string::npos);
}

TEST(ProbeStatus, ReportsDisabledRequestAndRenderedCounts) {
    ScopedTempDir temp("status_disabled");
    const fpgalsp::project::Project project = MakeProject(temp.path());
    Write(fpgalsp::probe::MdnPath(project), MarkDebugLine(MatchingSignals()));
    Write(fpgalsp::probe::CompiledBit(project), "bit");

    // ① 只有请求、compiled.cwc 还是旧的（没有 disable 标记）
    fpgalsp::probe::ProbeBuildConfig config;
    config.has_clock = true;
    config.clock = "reg_init/clk";
    config.disabled = {"WHG/pwm_reg0_i"};
    ASSERT_TRUE(fpgalsp::probe::WriteProbeBuildConfig(project, config));
    Write(fpgalsp::probe::CompiledCwc(project), kRealCwc);

    const auto requested = fpgalsp::probe::Status(project);
    EXPECT_TRUE(requested.value("ready", false)) << requested.dump();
    EXPECT_EQ(requested["disabled"].size(), 1u) << requested.dump();
    EXPECT_EQ(requested.value("disabled_count", 0), 16);
    const auto watcher_before = requested["watcher"];
    EXPECT_EQ(watcher_before.value("disabled_count", 0), 16);
    EXPECT_EQ(watcher_before.value("disabled_bus_count", 0), 0)
        << "旧 cwc 里还没有 is_enabled=\"no\"";
    EXPECT_EQ(watcher_before.value("disabled_net_count", 0), 0);

    // ② 生成的 cwc 里确实带 disable 标记
    std::string error;
    fpgalsp::jsonrpc::Json info = fpgalsp::jsonrpc::Json::object();
    const std::string text = fpgalsp::probe::BuildCwcFromConfig(
        project, fpgalsp::probe::ReadMdn(fpgalsp::probe::MdnPath(project)), config, &error, &info,
        /*include_eram=*/false);
    ASSERT_FALSE(text.empty()) << error;
    Write(fpgalsp::probe::CompiledCwc(project), text);

    const auto rendered = fpgalsp::probe::Status(project);
    EXPECT_TRUE(rendered.value("ready", false)) << rendered.dump();
    const auto watcher_after = rendered["watcher"];
    EXPECT_EQ(watcher_after.value("disabled_bus_count", 0), 1);
    EXPECT_EQ(watcher_after.value("disabled_net_count", 0), 16);
    EXPECT_EQ(watcher_after.value("enabled_net_count", 0), 32 - 16);
}

// disabled 位流的 sidecar：必须"保留 gate cwc 的总线/触发/时钟，只换 <logic_eram> 的几何"。
// 这是 2026-11-21 实测出来的唯一可行组合（gate cwc + 新 bid 的 ERAM 行 -> 触发完成、1024 点）：
//   * 整份重生成 -> Chipwatcher code 与位流不一致，wait_on_hw_cw 挂死；
//   * 不换 ERAM   -> KIT-8445 "sample depth is shorter than the set value (54/48)"。
TEST(ProbeCwc, GateSidecarKeepsBusListAndSplicesFreshEramRows) {
    ScopedTempDir temp("cwc_gate_sidecar");
    const fpgalsp::project::Project project = MakeProject(temp.path());

    // 新位流的 bid（三条 ERAM，宽度 20/20/9，data_offset 0/20/40，section 49）
    const std::string bid_dir = temp.path() + "/corpus_delta_Runs/phy_1";
    fs::create_directories(fs::path(bid_dir));
    std::string bid = "<All_Bram_Infos version=\"2.0\">\n";
    const int widths[] = {20, 20, 9};
    const int offsets[] = {0, 20, 40};
    const char* names[] = {"mem_syn_11", "mem_syn_32", "mem_syn_53"};
    const char* rids[] = {"0X0000044E", "0X0000044D", "0X0000044F"};
    for (int i = 0; i < 3; ++i) {
        bid += "\t<INST_" + std::to_string(i + 1) + ">\n";
        bid += "\t\t<rid>" + std::string(rids[i]) + "</rid>\n";
        bid += "\t\t<wid>" + std::string(rids[i]) + "</wid>\n";
        bid += "\t\t<is_debuggable>y</is_debuggable>\n\t\t<is_initialize>y</is_initialize>\n";
        bid += "\t\t<model_type>2</model_type>\n";
        bid += "\t\t<name>auto_created_cwc1/wrapper_cwc_top/U_cwc_ram/" + std::string(names[i]) +
               "</name>\n";
        bid += "\t\t<width_a>20</width_a>\n\t\t<width_b>20</width_b>\n";
        bid += "\t\t<logic_width>49</logic_width>\n\t\t<logic_depth>1024</logic_depth>\n";
        bid += "\t\t<sub_bid_info>\n\t\t\t<address_offset>0</address_offset>\n";
        bid += "\t\t\t<data_offset>" + std::to_string(offsets[i]) + "</data_offset>\n";
        bid += "\t\t\t<depth>1024</depth>\n\t\t\t<width>" + std::to_string(widths[i]) +
               "</width>\n";
        bid += "\t\t\t<num_section>1</num_section>\n\t\t\t<section_size>49</section_size>\n";
        bid += "\t\t\t<width_per_section>" + std::to_string(widths[i]) +
               "</width_per_section>\n";
        bid += "\t\t\t<bytes_in_per_section>1</bytes_in_per_section>\n";
        bid += "\t\t\t<working_mode>\n\t\t\t\t<address_step>1</address_step>\n";
        bid += "\t\t\t\t<depth>1024</depth>\n\t\t\t\t<mode_type>2</mode_type>\n";
        bid += "\t\t\t\t<width>20</width>\n\t\t\t\t<num_byte>2</num_byte>\n";
        bid += "\t\t\t\t<ecc>0</ecc>\n\t\t\t</working_mode>\n\t\t</sub_bid_info>\n\t</INST_" +
               std::to_string(i + 1) + ">\n";
    }
    bid += "</All_Bram_Infos>\n";
    Write(bid_dir + "/corpus_delta_inst.bid", bid);

    // NewestWatcherDir 需要一个 watcher 目录（inst 名从目录名来）
    fs::create_directories(fs::path(temp.path() +
                                    "/corpus_delta_Runs/syn_1/cw/auto_created_cwc1"));

    // gate cwc：两条总线、一条 disabled、旧 ERAM（section 54）—— 正是"抓不了"的那份
    std::string gate;
    gate += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    gate += "<chipwatcher version=\"3.070000\" project_name=\"corpus_delta\" "
            "chip_name=\"PH1A100SFG676\">\n\t<watcher_nodes>\n";
    gate += "\t\t<bus name=\"pwm_reg0_i[15:0]\" clock=\"reg_init/clk\" "
            "inst=\"auto_created_cwc1\" type=\"MARK_DEBUG_PROBE\" is_enabled=\"yes\">\n";
    gate += "\t\t\t<net name=\"pwm_reg0_i[15]\" const=\"false\" data=\"enable\" "
            "trig=\"enable\"/>\n\t\t</bus>\n";
    gate += "\t\t<bus name=\"pwm_reg1_i[15:0]\" clock=\"reg_init/clk\" "
            "inst=\"auto_created_cwc1\" type=\"MARK_DEBUG_PROBE\" is_enabled=\"no\">\n";
    gate += "\t\t\t<net name=\"pwm_reg1_i[15]\" const=\"false\" data=\"disable\" "
            "trig=\"disable\"/>\n\t\t</bus>\n";
    gate += "\t</watcher_nodes>\n\t<instance name=\"auto_created_cwc1\" id=\"0\" "
            "ipgen=\"false\" inst_enable=\"true\">\n";
    gate += "\t\t<clock clk_name=\"reg_init/clk\" clk_peroid=\"10ns\"/>\n";
    gate += "\t\t<trigger name=\"auto_created_cwc1_trigger\" position=\"127\" window_num=\"1\" "
            "window_depth=\"1024\">\n";
    gate += "\t\t\t<condition level=\"1\" enabled=\"yes\" type=\"basic_and\" "
            "capture_type=\"basic_and\"/>\n\t\t</trigger>\n";
    gate += "\t\t<logic_eram name=\"auto_created_cwc1/wrapper_cwc_top/U_cwc_ram\" width=\"53\" "
            "depth=\"1024\">\n";
    gate += "\t\t\t<phy_eram name=\"auto_created_cwc1/wrapper_cwc_top/U_cwc_ram/mem_syn_53\" "
            "width=\"14\" depth=\"1024\" address_offset=\"0\" data_offset=\"40\" "
            "section_size=\"54\"/>\n";
    gate += "\t\t</logic_eram>\n\t</instance>\n</chipwatcher>\n";
    const std::string gate_path = temp.path() + "/gate_probe_config.cwc";
    Write(gate_path, gate);

    const fpgalsp::jsonrpc::Json out =
        fpgalsp::probe::WriteCompiledCwcFromGate(project, gate_path);
    ASSERT_TRUE(out.value("ok", false)) << out.value("reason", std::string("?"));
    EXPECT_EQ(out.value("eram_rows", 0), 3);

    const std::string written =
        fpgalsp::platform::try_read_file_utf8(fpgalsp::probe::CompiledCwc(project)).value_or("");
    ASSERT_FALSE(written.empty());
    // 总线/触发/时钟原样保留（Chipwatcher code 才跟位流一致）
    EXPECT_NE(written.find("<bus name=\"pwm_reg1_i[15:0]\" clock=\"reg_init/clk\""),
              std::string::npos);
    EXPECT_NE(written.find("is_enabled=\"no\""), std::string::npos);
    EXPECT_NE(written.find("clk_peroid=\"10ns\""), std::string::npos);
    EXPECT_NE(written.find("position=\"127\""), std::string::npos);
    // ERAM 换成新 bid 的几何
    EXPECT_EQ(written.find("section_size=\"54\""), std::string::npos)
        << "旧 bid 的 section_size 必须被替换掉";
    EXPECT_NE(written.find("data_offset=\"40\""), std::string::npos);
    EXPECT_NE(written.find("section_size=\"49\""), std::string::npos);
    EXPECT_NE(written.find("mem_syn_53"), std::string::npos);
    EXPECT_EQ(written.find("</logic_eram>\t</instance>"), std::string::npos)
        << "拼接后必须保留换行结构";
}

// user.cfg 是 TD 真正读的探针配置（优先级高于 user.mdn）。写入规则：
//   1) 模板（compiled.cwc / user.cfg）总线集合与 mdn 一致 -> 只按 disabled 打补丁；
//   2) 集合不一致 -> 从零生成（否则会留下 mdn 里没有的总线，判 stale 抓不了）。
TEST(ProbeUserCfg, PatchesTemplateAndFallsBackWhenBusSetDiffers) {
    ScopedTempDir temp("user_cfg");
    const fpgalsp::project::Project project = MakeProject(temp.path());
    Write(fpgalsp::probe::MdnPath(project), MarkDebugLine(MatchingSignals()));

    // 模板：两条总线全 enable（与 mdn 一致）
    std::string tmpl;
    tmpl += "<chipwatcher version=\"3.070000\" project_name=\"corpus_delta\" "
            "chip_name=\"PH1A100SFG676\">\n";
    tmpl += "\t<watcher_nodes>\n";
    for (const char* bus : {"pwm_reg0_i", "pwm_reg1_i"}) {
        tmpl += std::string("\t\t<bus name=\"") + bus + "[15:0]\" clock=\"reg_init/clk\" "
                "inst=\"auto_created_cwc1\" type=\"MARK_DEBUG_PROBE\" is_enabled=\"yes\">\n";
        for (int bit = 15; bit >= 0; --bit) {
            tmpl += std::string("\t\t\t<net name=\"") + bus + "[" + std::to_string(bit) +
                    "]\" const=\"false\" data=\"enable\" trig=\"enable\"/>\n";
        }
        tmpl += "\t\t</bus>\n";
    }
    tmpl += "\t</watcher_nodes>\n\t<instance name=\"auto_created_cwc1\" id=\"0\" ipgen=\"false\" "
            "inst_enable=\"true\">\n\t\t<clock clk_name=\"reg_init/clk\" clk_peroid=\"10ns\"/>\n";
    tmpl += "\t\t<logic_eram name=\"auto_created_cwc1/wrapper_cwc_top/U_cwc_ram\" width=\"32\" "
            "depth=\"1024\">\n\t\t</logic_eram>\n\t</instance>\n</chipwatcher>\n";
    fs::create_directories(fs::path(temp.path() + "/chip_debugger"));
    Write(temp.path() + "/phy_1_cw_compiled.cwc", tmpl);
    Write(fpgalsp::probe::CompiledCwc(project), tmpl);

    fpgalsp::probe::ProbeBuildConfig cfg;
    cfg.disabled = {"WHG/pwm_reg1_i"};
    ASSERT_TRUE(fpgalsp::probe::WriteProbeBuildConfig(project, cfg));
    std::string error;
    ASSERT_TRUE(fpgalsp::probe::WriteUserCfgFromMdn(project, &error)) << error;

    const std::string written = fpgalsp::platform::try_read_file_utf8(
                                    temp.path() + "/chip_debugger/user.cfg")
                                    .value_or("");
    ASSERT_FALSE(written.empty());
    EXPECT_NE(written.find("is_enabled=\"no\""), std::string::npos) << "整条 disable 的总线";
    EXPECT_NE(written.find("data=\"disable\""), std::string::npos);
    EXPECT_NE(written.find("pwm_reg0_i[15:0]"), std::string::npos);
}