// =============================================================================
//  tests/unit/td_device_test.cpp
// =============================================================================
//  基准值全部来自 Python td.py 在真实夹具上的实测输出（见 PYTHON-PARITY.md §6）。
//  夹具：tests/td/fixtures/qp_*.txt（TD 2026.2 真实输出）
// =============================================================================

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "td/td_device.h"
#include "td/td_output.h"

namespace {

using namespace fpgalsp::td;

std::string ReadFixture(const std::string& name) {
    const std::string path =
        std::string(FPGA_LSP_SOURCE_DIR) + "/tests/td/fixtures/" + name;
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// -----------------------------------------------------------------------------
// DB_PREFIXES
// -----------------------------------------------------------------------------

TEST(TdDbPrefixes, MatchesPythonTable) {
    EXPECT_EQ(DbPrefixesForFamily("EF2"), (std::vector<std::string>{"ef2", "ef3", "ef4"}));
    EXPECT_EQ(DbPrefixesForFamily("EF3"), (std::vector<std::string>{"ef3", "ef4", "ef2"}));
    EXPECT_EQ(DbPrefixesForFamily("EF4"), (std::vector<std::string>{"ef4", "ef3"}));
    EXPECT_EQ(DbPrefixesForFamily("EF5"), (std::vector<std::string>{"ef5", "ef4"}));
    EXPECT_EQ(DbPrefixesForFamily("EG4"), (std::vector<std::string>{"eagle", "eg4"}));
    EXPECT_EQ(DbPrefixesForFamily("PH1"), (std::vector<std::string>{"ph1"}));
    EXPECT_EQ(DbPrefixesForFamily("PH1P"), (std::vector<std::string>{"ph1p", "ph1"}));
    EXPECT_EQ(DbPrefixesForFamily("PH2"), (std::vector<std::string>{"ph2"}));
    EXPECT_EQ(DbPrefixesForFamily("DR1"), (std::vector<std::string>{"dr1"}));
    EXPECT_EQ(DbPrefixesForFamily("SF1"), (std::vector<std::string>{"sf1"}));
    EXPECT_EQ(DbPrefixesForFamily("SF2"), (std::vector<std::string>{"sf2"}));
}

TEST(TdDbPrefixes, CaseInsensitiveAndUnknown) {
    EXPECT_EQ(DbPrefixesForFamily("ef2"), (std::vector<std::string>{"ef2", "ef3", "ef4"}));
    EXPECT_TRUE(DbPrefixesForFamily("XXXX").empty());
    EXPECT_TRUE(DbPrefixesForFamily("").empty());
}

// -----------------------------------------------------------------------------
// _parse_pipe_table
// -----------------------------------------------------------------------------

TEST(TdPipeTable, StripsCodePrefixCell) {
    // 真实样本：RUN-1001 :   Idx  |   PE   |  Location
    const std::string text =
        "RUN-1001 :    1   |   p1   |    pb1b    |  fio   |     2     |     0\r\n";
    const auto rows = ParsePipeTableText(text);
    ASSERT_EQ(rows.size(), 1u);
    ASSERT_GE(rows[0].size(), 6u);
    EXPECT_EQ(rows[0][0], "1");     // 前缀被剥掉
    EXPECT_EQ(rows[0][1], "p1");
}

TEST(TdPipeTable, DropsCodeOnlyCell) {
    const std::string text = "RUN-1001|  a  |  b  \r\n";
    const auto rows = ParsePipeTableText(text);
    ASSERT_EQ(rows.size(), 1u);
    ASSERT_GE(rows[0].size(), 2u);
    EXPECT_EQ(rows[0][0], "a");
}

TEST(TdPipeTable, SkipsLinesWithoutTwoPipes) {
    // 分隔线只有 | 或没有 | -> 跳过
    const std::string text =
        "RUN-1001 : ----------------\r\n"
        "RUN-1001 : no pipes here\r\n"
        "RUN-1001 : a | b | c\r\n";
    const auto rows = ParsePipeTableText(text);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0][0], "a");
}

// -----------------------------------------------------------------------------
// parse_fio —— 真实夹具对照
// -----------------------------------------------------------------------------

TEST(TdParseFio, RealFixtureMatchesPythonCounts) {
    const std::string fio = ReadFixture("qp_fio.txt");
    if (fio.empty()) GTEST_SKIP() << "fixture missing";

    const auto pins = ParseFio(fio);
    // Python 实测：115 个引脚
    EXPECT_EQ(pins.size(), 115u);
}

TEST(TdParseFio, RealFixtureFirstPins) {
    const std::string fio = ReadFixture("qp_fio.txt");
    if (fio.empty()) GTEST_SKIP() << "fixture missing";

    const auto pins = ParseFio(fio);

    // Python: p1 -> pe P1, location pb1b, type fio, int_bank 2, ext_bank 0
    auto p1 = pins.find("p1");
    ASSERT_NE(p1, pins.end());
    EXPECT_EQ(p1->second.pe, "P1");
    EXPECT_EQ(p1->second.pe_raw, "p1");
    EXPECT_EQ(p1->second.location, "pb1b");
    EXPECT_EQ(p1->second.type, "fio");
    ASSERT_TRUE(p1->second.int_bank.has_value());
    EXPECT_EQ(*p1->second.int_bank, 2);
    ASSERT_TRUE(p1->second.ext_bank.has_value());
    EXPECT_EQ(*p1->second.ext_bank, 0);
    EXPECT_FALSE(p1->second.clock_capable);
    EXPECT_FALSE(p1->second.lvds);

    // Python: p100 -> location pt8a, int_bank 7, ext_bank 2
    auto p100 = pins.find("p100");
    ASSERT_NE(p100, pins.end());
    EXPECT_EQ(p100->second.pe, "P100");
    EXPECT_EQ(p100->second.location, "pt8a");
    EXPECT_EQ(*p100->second.int_bank, 7);
    EXPECT_EQ(*p100->second.ext_bank, 2);
}

TEST(TdParseFio, KeysAreLowercaseValuesUppercase) {
    // 对应 Python：pins[pe.lower()] = {"pe": pe.upper(), "pe_raw": pe, ...}
    const std::string fio = ReadFixture("qp_fio.txt");
    if (fio.empty()) GTEST_SKIP() << "fixture missing";

    const auto pins = ParseFio(fio);
    for (const auto& [key, pin] : pins) {
        EXPECT_EQ(key, [&] {
            std::string lower = pin.pe_raw;
            for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return lower;
        }());
        EXPECT_EQ(pin.pe, [&] {
            std::string upper = pin.pe_raw;
            for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return upper;
        }());
    }
}

// -----------------------------------------------------------------------------
// parse_clock_io / parse_lvds —— 真实夹具对照
// -----------------------------------------------------------------------------

TEST(TdParseDevice, RealFixtureClockAndLvdsCounts) {
    std::string fio = ReadFixture("qp_fio.txt");
    const std::string clk = ReadFixture("qp_clk.txt");
    const std::string lvds = ReadFixture("qp_lvds.txt");
    if (fio.empty() || clk.empty() || lvds.empty()) GTEST_SKIP() << "fixture missing";

    auto pins = ParseFio(fio);
    ParseClockIo(clk, pins);
    ParseLvds(lvds, pins);

    // Python 实测：clock_capable 28，lvds 100
    int clock_count = 0;
    int lvds_count = 0;
    for (const auto& [k, p] : pins) {
        if (p.clock_capable) ++clock_count;
        if (p.lvds) ++lvds_count;
    }
    EXPECT_EQ(clock_count, 28);
    EXPECT_EQ(lvds_count, 100);
}

TEST(TdParseDevice, P121IsBothClockAndLvds) {
    // Python 实测样本：p121 -> clock_kind dpclkio_1, lvds_type LVDS
    std::string fio = ReadFixture("qp_fio.txt");
    const std::string clk = ReadFixture("qp_clk.txt");
    const std::string lvds = ReadFixture("qp_lvds.txt");
    if (fio.empty() || clk.empty() || lvds.empty()) GTEST_SKIP() << "fixture missing";

    auto pins = ParseFio(fio);
    ParseClockIo(clk, pins);
    ParseLvds(lvds, pins);

    auto it = pins.find("p121");
    ASSERT_NE(it, pins.end());
    EXPECT_EQ(it->second.pe, "P121");
    EXPECT_EQ(it->second.location, "pl18b");
    EXPECT_TRUE(it->second.clock_capable);
    ASSERT_TRUE(it->second.clock_kind.has_value());
    EXPECT_EQ(*it->second.clock_kind, "dpclkio_1");
    EXPECT_TRUE(it->second.lvds);
    ASSERT_TRUE(it->second.lvds_type.has_value());
    EXPECT_EQ(*it->second.lvds_type, "LVDS");
}

// -----------------------------------------------------------------------------
// parse_cfg_io —— 真实夹具对照
// -----------------------------------------------------------------------------

TEST(TdParseCfgIo, RealFixtureDedicatedPins) {
    const std::string cfg = ReadFixture("qp_cfg.txt");
    if (cfg.empty()) GTEST_SKIP() << "fixture missing";

    const auto dedicated = ParseCfgIo(cfg);
    // Python 实测：5 条
    ASSERT_EQ(dedicated.size(), 5u);

    EXPECT_EQ(dedicated[0].role, "programn");
    EXPECT_EQ(dedicated[0].pe, "P119");

    // "tdi/tms/tck/tdo | P136/P130/P131/P137" 会拆成 4 条
    EXPECT_EQ(dedicated[1].role, "tdi/tms/tck/tdo");
    EXPECT_EQ(dedicated[1].pe, "P136");
    EXPECT_EQ(dedicated[2].pe, "P130");
    EXPECT_EQ(dedicated[3].pe, "P131");
}

TEST(TdParseCfgIo, OnlyDedicateSettingCounts) {
    // "gpio" 行不进黑名单
    const std::string text =
        "ARC-1001 :        done        |         P109          |    gpio\r\n"
        "ARC-1001 :      programn      |         P119          |  dedicate\r\n";
    const auto dedicated = ParseCfgIo(text);
    ASSERT_EQ(dedicated.size(), 1u);
    EXPECT_EQ(dedicated[0].pe, "P119");
}

TEST(TdParseCfgIo, CaseInsensitiveDedicate) {
    const std::string text = "ARC-1001 : x | P1 | DEDICATE\r\n";
    EXPECT_EQ(ParseCfgIo(text).size(), 1u);
}

// -----------------------------------------------------------------------------
// BuildDeviceInfo 组合
// -----------------------------------------------------------------------------

TEST(TdBuildDeviceInfo, AppliesAllFourTables) {
    const std::string fio = ReadFixture("qp_fio.txt");
    const std::string clk = ReadFixture("qp_clk.txt");
    const std::string lvds = ReadFixture("qp_lvds.txt");
    const std::string cfg = ReadFixture("qp_cfg.txt");
    if (fio.empty()) GTEST_SKIP() << "fixture missing";

    const auto info = BuildDeviceInfo(fio, clk, lvds, cfg);
    EXPECT_EQ(info.pins.size(), 115u);
    EXPECT_EQ(info.dedicated.size(), 5u);
}

// -----------------------------------------------------------------------------
// speed 参数（去正负号）
// -----------------------------------------------------------------------------

TEST(TdSpeedArg, StripsSign) {
    EXPECT_EQ(SpeedArgSuffix(std::string("-2")), " -speed 2");
    EXPECT_EQ(SpeedArgSuffix(std::string("+3")), " -speed 3");
    EXPECT_EQ(SpeedArgSuffix(std::string("2")), " -speed 2");
}

TEST(TdSpeedArg, EmptyOrMissingGivesNothing) {
    EXPECT_EQ(SpeedArgSuffix(std::nullopt), "");
    EXPECT_EQ(SpeedArgSuffix(std::string("")), "");
}

// -----------------------------------------------------------------------------
// import_device 命令行（实测形态：裸 db 文件名）
// -----------------------------------------------------------------------------

TEST(TdImportCommand, UsesBareDbFileName) {
    // 实测正确形态：import_device ef2_4.db -package EF2L45LG144B
    EXPECT_EQ(MakeImportDeviceCommand("ef2_4.db", "EF2L45LG144B", std::nullopt),
              "import_device ef2_4.db -package EF2L45LG144B");
}

TEST(TdImportCommand, AppendsSpeedWithoutSign) {
    // 实测：带 "-speed -2" 会报 USR-8316，必须去掉符号
    EXPECT_EQ(MakeImportDeviceCommand("ef2_4.db", "EF2L45LG144B", std::string("-2")),
              "import_device ef2_4.db -package EF2L45LG144B -speed 2");
}

// -----------------------------------------------------------------------------
// 缓存键
// -----------------------------------------------------------------------------

TEST(TdCacheKey, MatchesPythonFormat) {
    // Python: device_%s.json with re.sub(r"[^A-Za-z0-9_.-]+", "_", ...)
    EXPECT_EQ(DeviceCacheKey("EF2L45LG144B", "EF2", std::nullopt),
              "device_EF2L45LG144B_EF2_na.json");
    EXPECT_EQ(DeviceCacheKey("EF2L45LG144B", "EF2", std::string("-2")),
              "device_EF2L45LG144B_EF2_-2.json");
}

TEST(TdCacheKey, ReplacesIllegalChars) {
    EXPECT_EQ(DeviceCacheKey("EF2 L45/144B", "EF2", std::nullopt),
              "device_EF2_L45_144B_EF2_na.json");
}

// -----------------------------------------------------------------------------
// _db_candidates 实战：本机真实 TD 目录
// -----------------------------------------------------------------------------

TEST(TdDbCandidates, RealTdInstallEf2) {
    const std::string arch =
        "C:/Anlogic/TD_Release_2026.2.210.850";
    if (!std::filesystem::exists(arch + "/arch")) {
        GTEST_SKIP() << "本机没有 TD 安装";
    }

    const auto candidates = DbCandidates(arch, "EF2");
    ASSERT_FALSE(candidates.empty());

    // Python 实测前 4 个：ef2_1.db, ef2_2.db, ef2_4.db, ef2_s4.db
    ASSERT_GE(candidates.size(), 4u);
    EXPECT_EQ(candidates[0], "ef2_1.db");
    EXPECT_EQ(candidates[1], "ef2_2.db");
    EXPECT_EQ(candidates[2], "ef2_4.db");
    EXPECT_EQ(candidates[3], "ef2_s4.db");

    // 降级前缀 ef3 应在全部 ef2 候选之后出现
    bool saw_ef3 = false;
    std::size_t ef3_pos = 0;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (candidates[i].rfind("ef3_", 0) == 0) {
            saw_ef3 = true;
            ef3_pos = i;
            break;
        }
    }
    EXPECT_TRUE(saw_ef3);
    EXPECT_GT(ef3_pos, 3u) << "ef3 候选应排在全部 ef2 候选之后";

    // ⚠ 候选总数 = arch/*.db 的总数（全部 db 都会被补进列表）。
    //   不要把这个数字写死：TD 升级会增删 db 文件，写死会导致
    //   "环境一变就红" 的脆弱测试。这里按目录实际文件数对比。
    std::size_t db_file_count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(arch + "/arch")) {
        const std::string name = entry.path().filename().string();
        if (name.size() > 3 && name.compare(name.size() - 3, 3, ".db") == 0) ++db_file_count;
    }
    EXPECT_EQ(candidates.size(), db_file_count);

    // 无重复
    auto unique = candidates;
    std::sort(unique.begin(), unique.end());
    unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
    EXPECT_EQ(unique.size(), candidates.size()) << "候选列表里不应有重复项";
}

TEST(TdDbCandidates, UnknownFamilyStillReturnsAllSorted) {
    const std::string arch = "C:/Anlogic/TD_Release_2026.2.210.850";
    if (!std::filesystem::exists(arch + "/arch")) {
        GTEST_SKIP() << "本机没有 TD 安装";
    }
    // family 无前缀映射时，退化为全部 db 按字典序
    const auto candidates = DbCandidates(arch, "UNKNOWN");
    ASSERT_FALSE(candidates.empty());
    auto sorted = candidates;
    std::sort(sorted.begin(), sorted.end());
    EXPECT_EQ(candidates, sorted) << "未知 family 时应完全按字典序";
}

TEST(TdDbCandidates, Ef3PrefersEf3OverEf2) {
    const std::string arch = "C:/Anlogic/TD_Release_2026.2.210.850";
    if (!std::filesystem::exists(arch + "/arch")) {
        GTEST_SKIP() << "本机没有 TD 安装";
    }
    // EF3 的前缀顺序是 (ef3, ef4, ef2)：第一个候选必须是 ef3_*
    const auto candidates = DbCandidates(arch, "EF3");
    ASSERT_FALSE(candidates.empty());
    EXPECT_EQ(candidates.front().rfind("ef3_", 0), 0u) << candidates.front();
}

TEST(TdDbCandidates, MissingArchDirReturnsEmpty) {
    EXPECT_TRUE(DbCandidates("C:/definitely/not/here", "EF2").empty());
}

}  // namespace