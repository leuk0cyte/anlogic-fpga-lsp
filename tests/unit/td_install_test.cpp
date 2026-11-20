// =============================================================================
//  tests/unit/td_install_test.cpp
// =============================================================================
//  全部用注入的 TdEnv，不依赖真实文件系统与真实 TD 安装。
//  基准值来自 Y:\fpga-lsp\fpgalsp\td.py 实测输出。
// =============================================================================

#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "td/td_install.h"

namespace {

using namespace fpgalsp::td;

// 内存文件系统
struct FakeFs {
    // dir -> 子项名
    std::map<std::string, std::vector<std::string>> dirs;
    // 存在的文件集合
    std::vector<std::string> files;
    std::map<std::string, std::string> env;

    TdEnv Make() const {
        TdEnv e;
        e.list_dir = [this](std::string_view dir) {
            auto it = dirs.find(std::string(dir));
            if (it == dirs.end()) return std::vector<std::string>{};
            return it->second;
        };
        e.is_file = [this](std::string_view path) {
            for (const auto& f : files) {
                if (f == path) return true;
            }
            return false;
        };
        e.get_env = [this](std::string_view name) {
            auto it = env.find(std::string(name));
            return it == env.end() ? std::string() : it->second;
        };
        e.drives = "C";
        return e;
    }

    // 添加一个完整安装
    void AddInstall(const std::string& root) {
        dirs["C:/"].push_back("Anlogic");
        dirs["C:/Anlogic"].push_back(plat_filename(root));
        files.push_back(root + "/bin/td_commands_prompt.exe");
    }

    static std::string plat_filename(const std::string& path) {
        const std::size_t p = path.find_last_of('/');
        return p == std::string::npos ? path : path.substr(p + 1);
    }
};

// -----------------------------------------------------------------------------
// VersionKey（Python 基准已核对）
// -----------------------------------------------------------------------------

TEST(TdVersionKey, StandardRelease) {
    // Python: version_key('TD_Release_2026.2.210.850') == (2026, 2, 0, 210, 850)
    EXPECT_EQ(VersionKey("TD_Release_2026.2.210.850"),
              (std::vector<int>{2026, 2, 0, 210, 850}));
}

TEST(TdVersionKey, ServicePackForm) {
    // Python: version_key('2026.1_SP2.200.067') == (2026, 1, 2, 200, 67)
    EXPECT_EQ(VersionKey("2026.1_SP2.200.067"), (std::vector<int>{2026, 1, 2, 200, 67}));
}

// 本轮真实升级留下的样本：2026.2(210850) -> 2026.2_SP1(218141)。
// 把"SP1 必须排在不带 SP 的旧版之前"固化下来 —— 这条断的是
// 「自动发现会选中新装的 SP1，而不是回落到旧目录」。
TEST(TdVersionKey, Sp1IsNewerThanBaseRelease) {
    const std::string sp1 = "TD_Release_2026.2_SP1.218.141";
    EXPECT_EQ(VersionKey("2026.2_SP1.218.141"), (std::vector<int>{2026, 2, 1, 218, 141}));
    EXPECT_EQ(VersionKey("2026.2.210.850"), (std::vector<int>{2026, 2, 0, 210, 850}));
    EXPECT_GT(CompareVersionKey(VersionKey("2026.2_SP1.218.141"),
                                VersionKey("2026.2.210.850")),
              0);
    EXPECT_EQ(ReleaseOf("C:/Anlogic/" + sp1), "2026.2_SP1.218.141");
    // 目录名若把补充版本点写丢（...SP1.218141），也仍应比旧版新（走数字兜底）
    EXPECT_LT(CompareVersionKey(VersionKey("2026.2.210.850"),
                                VersionKey("2026.2_SP1.218141")),
              0);
}

TEST(TdVersionKey, AnotherYear) {
    // Python: version_key('TD_Release_2025.1.100.001') == (2025, 1, 0, 100, 1)
    EXPECT_EQ(VersionKey("TD_Release_2025.1.100.001"),
              (std::vector<int>{2025, 1, 0, 100, 1}));
}

TEST(TdVersionKey, FallsBackToAllNumbers) {
    const auto key = VersionKey("foo-1-2-3");
    EXPECT_EQ(key, (std::vector<int>{1, 2, 3}));
}

TEST(TdVersionKey, EmptyGivesEmptyKey) {
    EXPECT_TRUE(VersionKey("").empty());
}

TEST(TdVersionKey, NonReleaseStringHasNoYearPattern) {
    // 不满足 20dd 的年份模式 -> 退化提取数字
    EXPECT_EQ(VersionKey("v12.3"), (std::vector<int>{12, 3}));
}

// -----------------------------------------------------------------------------
// 版本比较
// -----------------------------------------------------------------------------

TEST(TdVersionCompare, LargerYearWins) {
    EXPECT_GT(CompareVersionKey(VersionKey("2026.2.210.850"), VersionKey("2025.1.100.001")), 0);
}

TEST(TdVersionCompare, SamePrefixShorterIsSmaller) {
    // Python tuple 比较：[2026,2] < [2026,2,0]
    EXPECT_LT(CompareVersionKey({2026, 2}, {2026, 2, 0}), 0);
}

TEST(TdVersionCompare, EqualKeys) {
    EXPECT_EQ(CompareVersionKey({2026, 2, 0}, {2026, 2, 0}), 0);
}

TEST(TdVersionCompare, ServicePackBreaksTie) {
    EXPECT_GT(CompareVersionKey(VersionKey("2026.1_SP2.200.067"),
                               VersionKey("2026.1_SP1.200.067")),
              0);
}

// -----------------------------------------------------------------------------
// NormaliseRoot / ReleaseOf
// -----------------------------------------------------------------------------

TEST(TdNormalise, FromRoot) {
    EXPECT_EQ(NormaliseRoot("C:/Anlogic/TD_Release_2026.2.210.850"),
              "C:/Anlogic/TD_Release_2026.2.210.850");
}

TEST(TdNormalise, FromBinDirectory) {
    EXPECT_EQ(NormaliseRoot("C:/Anlogic/TD_Release_2026.2.210.850/bin"),
              "C:/Anlogic/TD_Release_2026.2.210.850");
}

TEST(TdNormalise, FromExePath) {
    EXPECT_EQ(NormaliseRoot("C:/Anlogic/TD_Release_2026.2.210.850/bin/td_commands_prompt.exe"),
              "C:/Anlogic/TD_Release_2026.2.210.850");
}

TEST(TdNormalise, BackslashesConverted) {
    EXPECT_EQ(NormaliseRoot("C:\\Anlogic\\TD_Release_2026.2.210.850\\bin"),
              "C:/Anlogic/TD_Release_2026.2.210.850");
}

TEST(TdReleaseOf, ExtractsVersion) {
    EXPECT_EQ(ReleaseOf("C:/Anlogic/TD_Release_2026.2.210.850"), "2026.2.210.850");
}

TEST(TdReleaseOf, ContainsServicePack) {
    EXPECT_EQ(ReleaseOf("C:/x/TD_Release_2026.1_SP2.200.067"), "2026.1_SP2.200.067");
}

TEST(TdReleaseOf, FallsBackToBasename) {
    EXPECT_EQ(ReleaseOf("C:/weird/MyTdDir"), "MyTdDir");
}

// -----------------------------------------------------------------------------
// 安装发现（注入文件系统）
// -----------------------------------------------------------------------------

TEST(TdInstallDiscovery, FindsAnlogicDir) {
    FakeFs fs;
    fs.AddInstall("C:/Anlogic/TD_Release_2026.2.210.850");
    const auto installs = InstallsWith(fs.Make());

    ASSERT_EQ(installs.size(), 1u);
    EXPECT_EQ(installs[0].root, "C:/Anlogic/TD_Release_2026.2.210.850");
    EXPECT_EQ(installs[0].exe,
              "C:/Anlogic/TD_Release_2026.2.210.850/bin/td_commands_prompt.exe");
    EXPECT_EQ(installs[0].version, "2026.2.210.850");
    EXPECT_EQ(installs[0].key, (std::vector<int>{2026, 2, 0, 210, 850}));
}

TEST(TdInstallDiscovery, SortedNewestFirst) {
    FakeFs fs;
    fs.dirs["C:/Anlogic"] = {"TD_Release_2025.1.100.001", "TD_Release_2026.2.210.850"};
    fs.files = {"C:/Anlogic/TD_Release_2025.1.100.001/bin/td_commands_prompt.exe",
                "C:/Anlogic/TD_Release_2026.2.210.850/bin/td_commands_prompt.exe"};

    const auto installs = InstallsWith(fs.Make());
    ASSERT_EQ(installs.size(), 2u);
    EXPECT_EQ(installs[0].version, "2026.2.210.850");  // 最新的在前
    EXPECT_EQ(installs[1].version, "2025.1.100.001");
}

TEST(TdInstallDiscovery, SkipsDirectoryWithoutExe) {
    FakeFs fs;
    fs.dirs["C:/Anlogic"] = {"TD_Release_2026.2.210.850"};
    // 不创建 exe

    EXPECT_TRUE(InstallsWith(fs.Make()).empty());
}

TEST(TdInstallDiscovery, ScansProgramFiles) {
    FakeFs fs;
    fs.dirs["C:/Program Files/Anlogic"] = {"TD_Release_2026.2.210.850"};
    fs.files = {"C:/Program Files/Anlogic/TD_Release_2026.2.210.850/bin/td_commands_prompt.exe"};

    const auto installs = InstallsWith(fs.Make());
    ASSERT_EQ(installs.size(), 1u);
    EXPECT_EQ(installs[0].root, "C:/Program Files/Anlogic/TD_Release_2026.2.210.850");
}

TEST(TdInstallDiscovery, ScansProgramFilesX86) {
    FakeFs fs;
    fs.dirs["C:/Program Files (x86)/Anlogic"] = {"TD_Release_2025.1.100.001"};
    fs.files = {
        "C:/Program Files (x86)/Anlogic/TD_Release_2025.1.100.001/bin/td_commands_prompt.exe"};

    EXPECT_EQ(InstallsWith(fs.Make()).size(), 1u);
}

TEST(TdInstallDiscovery, ScansOneLevelDeeper) {
    // <drive>/<vendor>/Anlogic/TD_Release_*
    FakeFs fs;
    fs.dirs["C:/"] = {"Tools"};
    fs.dirs["C:/Tools/Anlogic"] = {"TD_Release_2026.2.210.850"};
    fs.files = {"C:/Tools/Anlogic/TD_Release_2026.2.210.850/bin/td_commands_prompt.exe"};

    const auto installs = InstallsWith(fs.Make());
    ASSERT_EQ(installs.size(), 1u);
    EXPECT_EQ(installs[0].root, "C:/Tools/Anlogic/TD_Release_2026.2.210.850");
}

TEST(TdInstallDiscovery, DeduplicatesSameRoot) {
    // 同时能通过浅层与深层路径发现同一个安装 -> 只应出现一次
    FakeFs fs;
    fs.dirs["C:/"] = {"Anlogic"};
    fs.dirs["C:/Anlogic"] = {"TD_Release_2026.2.210.850"};
    fs.files = {"C:/Anlogic/TD_Release_2026.2.210.850/bin/td_commands_prompt.exe"};

    const auto roots = InstallRootsWith(fs.Make());
    EXPECT_EQ(roots.size(), 1u);
}

TEST(TdInstallDiscovery, IgnoresNonReleaseDirs) {
    FakeFs fs;
    fs.dirs["C:/Anlogic"] = {"docs", "TD_Release_2026.2.210.850"};
    fs.files = {"C:/Anlogic/TD_Release_2026.2.210.850/bin/td_commands_prompt.exe"};

    EXPECT_EQ(InstallsWith(fs.Make()).size(), 1u);
}

TEST(TdInstallDiscovery, EmptyFsFindsNothing) {
    FakeFs fs;
    EXPECT_TRUE(InstallsWith(fs.Make()).empty());
    EXPECT_FALSE(FindTdWith(fs.Make()).has_value());
}

// -----------------------------------------------------------------------------
// FindTd 优先级
// -----------------------------------------------------------------------------

TEST(TdFind, ExplicitWins) {
    FakeFs fs;
    fs.dirs["C:/Anlogic"] = {"TD_Release_2025.1.100.001", "TD_Release_2026.2.210.850"};
    fs.files = {"C:/Anlogic/TD_Release_2025.1.100.001/bin/td_commands_prompt.exe",
                "C:/Anlogic/TD_Release_2026.2.210.850/bin/td_commands_prompt.exe"};

    // 显式指定较旧的版本，应压过更新版本
    const auto choice = FindTdWith(fs.Make(), "C:/Anlogic/TD_Release_2025.1.100.001");
    ASSERT_TRUE(choice.has_value());
    EXPECT_EQ(choice->version, "2025.1.100.001");
    EXPECT_EQ(choice->source, "explicit");
    ASSERT_EQ(choice->alternatives.size(), 1u);
    EXPECT_EQ(choice->alternatives[0].version, "2026.2.210.850");
}

TEST(TdFind, EnvVarUsed) {
    FakeFs fs;
    fs.dirs["C:/Anlogic"] = {"TD_Release_2025.1.100.001", "TD_Release_2026.2.210.850"};
    fs.files = {"C:/Anlogic/TD_Release_2025.1.100.001/bin/td_commands_prompt.exe",
                "C:/Anlogic/TD_Release_2026.2.210.850/bin/td_commands_prompt.exe"};
    fs.env["TD_ROOT"] = "C:/Anlogic/TD_Release_2025.1.100.001";

    const auto choice = FindTdWith(fs.Make());
    ASSERT_TRUE(choice.has_value());
    EXPECT_EQ(choice->source, "explicit");
    EXPECT_EQ(choice->version, "2025.1.100.001");
}

TEST(TdFind, EnvVarIgnoredWhenExeMissing) {
    FakeFs fs;
    fs.dirs["C:/Anlogic"] = {"TD_Release_2026.2.210.850"};
    fs.files = {"C:/Anlogic/TD_Release_2026.2.210.850/bin/td_commands_prompt.exe"};
    fs.env["TD_ROOT"] = "C:/nonexistent/TD_Release_9999.9.9.9";

    // 无效的环境变量应被跳过，回落到自动检测
    const auto choice = FindTdWith(fs.Make());
    ASSERT_TRUE(choice.has_value());
    EXPECT_EQ(choice->source, "auto");
    EXPECT_EQ(choice->version, "2026.2.210.850");
}

TEST(TdFind, AutoPicksNewest) {
    FakeFs fs;
    fs.dirs["C:/Anlogic"] = {"TD_Release_2025.1.100.001", "TD_Release_2026.2.210.850"};
    fs.files = {"C:/Anlogic/TD_Release_2025.1.100.001/bin/td_commands_prompt.exe",
                "C:/Anlogic/TD_Release_2026.2.210.850/bin/td_commands_prompt.exe"};

    const auto choice = FindTdWith(fs.Make());
    ASSERT_TRUE(choice.has_value());
    EXPECT_EQ(choice->source, "auto");
    EXPECT_EQ(choice->version, "2026.2.210.850");
    ASSERT_EQ(choice->alternatives.size(), 1u);
}

TEST(TdFind, PathFallback) {
    FakeFs fs;
    // 没有 Anlogic 安装，但 PATH 里有 bin
    fs.files = {"C:/custom/TD_Release_2026.2.210.850/bin/td_commands_prompt.exe"};
    fs.env["PATH"] = "C:/other;C:/custom/TD_Release_2026.2.210.850/bin";

    const auto choice = FindTdWith(fs.Make());
    ASSERT_TRUE(choice.has_value());
    EXPECT_EQ(choice->source, "path");
    // ⚠ 逐字复现 Python:
    //     root = os.path.dirname(os.path.dirname(os.path.abspath(entry)))
    //   entry 是 PATH 里那一项（这里是 ".../bin"），取上两级后得到 ".../TD_Release_.../bin"
    //   的**上两级** = "C:/custom"，而不是安装根。
    //   这看起来像 Python 版的 bug，但按"等价优先、不修正"的原则保持一致。
    EXPECT_EQ(choice->root, "C:/custom");
    // exe 仍然指向正确位置
    EXPECT_EQ(choice->exe, "C:/custom/TD_Release_2026.2.210.850/bin/td_commands_prompt.exe");
}

TEST(TdFind, ReturnsNulloptWhenNothingFound) {
    FakeFs fs;
    fs.env["PATH"] = "C:/nothing";
    EXPECT_FALSE(FindTdWith(fs.Make()).has_value());
}

// -----------------------------------------------------------------------------
// 本机真实环境（不做断言强依赖，仅记录）
// -----------------------------------------------------------------------------

TEST(TdInstallReal, FindsThisMachineInstall) {
    const auto choice = FindTd();
    ASSERT_TRUE(choice.has_value()) << "本机应能找到 TD 安装";
    EXPECT_NE(choice->exe.find("td_commands_prompt.exe"), std::string::npos);
    // ⚠ 不要在这里硬编码 TD 版本/build 号 —— TD 升级一次（如 2026.2.210.850 →
    //   2026.2_SP1.218.141）就会把这条断言变成假红。只断言「版本串确实是从安装
    //   目录名派生的、且能被解析成版本号」，具体数字交给上面对合成目录的用例。
    EXPECT_EQ(choice->version, ReleaseOf(choice->root));
    EXPECT_FALSE(choice->version.empty());
    // 至少三段（year.major[.SPn].build.patch）：解析失败会退化成"提取全部数字"，
    // 这里要求结构化解析出来的键足够长，能挡住 "TD_Release_" 后面接垃圾名的情况。
    EXPECT_GE(VersionKey(choice->version).size(), 4u) << "版本串: " << choice->version;
    EXPECT_TRUE(choice->source == "auto" || choice->source == "explicit");
}

// 安装发现缓存（%TEMP%\fpgalsp\td-install-cache.txt）：
//   第一次调用必须真的扫盘（from_cache=false），第二次必须命中缓存。
//   这里刻意先删缓存文件，避免与同机其它测试进程互相影响。
TEST(TdInstallReal, SecondLookupIsServedFromCache) {
    std::error_code ec;
    const std::filesystem::path temp = std::filesystem::temp_directory_path(ec);
    if (ec) GTEST_SKIP() << "无法定位系统临时目录";
    const std::filesystem::path cache = temp / "fpgalsp" / "td-install-cache.txt";
    std::filesystem::remove(cache, ec);

    _putenv_s("FPGALSP_NO_TD_CACHE", "");      // 确保缓存开启
    _putenv_s("FPGALSP_TD_CACHE_TTL", "3600"); // 1 h，避免默认 24 h 受机器时间影响

    const auto first = FindTd();
    ASSERT_TRUE(first.has_value()) << "本机应能找到 TD 安装";
    EXPECT_FALSE(first->from_cache) << "缓存文件刚删过，第一次必须是真扫描";
    EXPECT_TRUE(std::filesystem::exists(cache)) << "扫描成功后应写出缓存: " << cache.string();

    const auto second = FindTd();
    ASSERT_TRUE(second.has_value());
    EXPECT_TRUE(second->from_cache) << "第二次应命中缓存";
    EXPECT_EQ(second->root, first->root);
    EXPECT_EQ(second->exe, first->exe);
    // 缓存命中时 source 仍是 "auto"：三值契约不变，缓存只通过 from_cache 体现
    EXPECT_EQ(second->source, "auto");

    _putenv_s("FPGALSP_TD_CACHE_TTL", "");
}

}  // namespace