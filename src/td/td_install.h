// =============================================================================
//  src/td/td_install.h —— TD（TangDynasty）安装发现
// =============================================================================
//  对应参考实现：Y:\fpga-lsp\fpgalsp\td.py
//      version_key / _install_roots / installs / find_td / _normalise_root / _release_of
//
//  本机实测可用安装：C:\Anlogic\TD_Release_2026.2.210.850
// =============================================================================

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fpgalsp::td {

inline constexpr const char* kTdExeName = "td_commands_prompt.exe";
inline constexpr const char* kReleaseDir = "Anlogic";

// 一个可用安装
struct TdInstall {
    std::string root;     // 安装根目录（正斜杠绝对路径）
    std::string exe;      // <root>/bin/td_commands_prompt.exe
    std::string version;  // 从目录名提取的 release 串，如 "2026.2.210.850"
    std::vector<int> key; // 排序键（对应 Python tuple；长度可变，按字典序比较）
};

// 选中的 TD（对应 Python find_td 的返回 dict）
struct TdChoice {
    std::string root;
    std::string exe;
    std::string version;
    std::string source;  // "explicit" | "auto" | "path"
    std::vector<TdInstall> alternatives;
};

// 对应 Python version_key：把 release 目录名转成可比较的数字序列。
//   "TD_Release_2026.2.210.850" -> [2026, 2, 0, 210, 850]
//   "2026.1_SP2.200.067"        -> [2026, 1, 2, 200, 67]
// 匹配不到时退化为「提取全部数字」。
std::vector<int> VersionKey(std::string_view release);

// 比较两个 version key：返回 <0 / 0 / >0。
// Python 的 tuple 比较是「先比共同前缀，再比长度」，这里保持一致。
int CompareVersionKey(const std::vector<int>& a, const std::vector<int>& b);

// 所有可用的 TD 安装，按 key 降序（最新在前）。
std::vector<TdInstall> Installs();

// 所有可能的安装根目录（去重、不排序）。对应 Python _install_roots()。
// 扫描 C-J 盘的 <drive>:/Anlogic、<drive>:/Program Files/Anlogic、
// <drive>:/Program Files (x86)/Anlogic，以及更深一层 <drive>/<任意目录>/Anlogic。
std::vector<std::string> InstallRoots();

// 选择 TD 安装。priority: explicit > 环境变量 > 自动检测（最新）> PATH。
// 找不到时返回 nullopt（对应 Python 的 TdError，但这里不抛，符合 never-raise）。
std::optional<TdChoice> FindTd(std::string_view explicit_root = {});

// 对应 Python _normalise_root：把可能指向 exe 或 bin 的路径归一成安装根。
// 接受 "C:/Anlogic/TD_Release_x/bin/td_commands_prompt.exe" / ".../bin" / "...".
std::string NormaliseRoot(std::string_view root);

// 对应 Python _release_of：从根目录路径里提取 "TD_Release_<ver>" 的 <ver> 部分；
// 匹配不到时返回目录名。
std::string ReleaseOf(std::string_view root);

// ---- 便于测试的注入点 ----
// 目录列举 / 文件存在 / 环境变量读取，默认走真实文件系统；测试可替换。
struct TdEnv {
    // 列目录，失败返回空
    std::function<std::vector<std::string>(std::string_view dir)> list_dir;
    // 是否为普通文件
    std::function<bool(std::string_view path)> is_file;
    // 读环境变量，不存在返回空
    std::function<std::string(std::string_view name)> get_env;
    // 枚举的盘符（默认 "CDEFGHIJ"，与 Python 一致）
    std::string drives = "CDEFGHIJ";

    static TdEnv Default();
};

std::vector<std::string> InstallRootsWith(const TdEnv& env);
std::vector<TdInstall> InstallsWith(const TdEnv& env);
std::optional<TdChoice> FindTdWith(const TdEnv& env, std::string_view explicit_root = {});

}  // namespace fpgalsp::td