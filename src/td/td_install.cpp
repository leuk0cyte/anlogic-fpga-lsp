// =============================================================================
//  src/td/td_install.cpp
// =============================================================================

#include "td/td_install.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "platform/path.h"

namespace fpgalsp::td {
namespace {

namespace plat = fpgalsp::platform;

std::string ToLower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool EndsWithLower(std::string_view s, std::string_view suffix_lower) {
    const std::string lower = ToLower(s);
    if (lower.size() < suffix_lower.size()) return false;
    return lower.compare(lower.size() - suffix_lower.size(), suffix_lower.size(),
                         suffix_lower) == 0;
}

bool StartsWithUpper(std::string_view s, std::string_view prefix_upper) {
    if (s.size() < prefix_upper.size()) return false;
    for (std::size_t i = 0; i < prefix_upper.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(s[i])) != prefix_upper[i]) return false;
    }
    return true;
}

}  // namespace

// -----------------------------------------------------------------------------
// version key
// -----------------------------------------------------------------------------

std::vector<int> VersionKey(std::string_view release) {
    const std::string text(release);

    // 对应 Python: r"(20\d{2})[._](\d+)(?:[._]?SP(\d+))?[._](\d+)[._](\d+)" (忽略大小写)
    // 逐个位置尝试匹配 "20dd" 开头。
    for (std::size_t i = 0; i + 4 <= text.size(); ++i) {
        if (text.compare(i, 2, "20") != 0) continue;
        if (!std::isdigit(static_cast<unsigned char>(text[i + 2])) ||
            !std::isdigit(static_cast<unsigned char>(text[i + 3]))) {
            continue;
        }

        std::size_t p = i;
        auto read_digits = [&](std::size_t& pos) -> std::optional<int> {
            const std::size_t start = pos;
            long long value = 0;
            while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) {
                value = value * 10 + (text[pos] - '0');
                if (value > 1000000000LL) value = 1000000000LL;  // 防溢出
                ++pos;
            }
            if (pos == start) return std::nullopt;
            return static_cast<int>(value);
        };

        auto is_sep = [&](char c) { return c == '.' || c == '_'; };

        // year: 4 位
        const int year = std::stoi(text.substr(i, 4));
        p = i + 4;

        if (p >= text.size() || !is_sep(text[p])) continue;
        ++p;

        const auto version = read_digits(p);
        if (!version.has_value()) continue;

        // 可选: [._]?SP(\d+)
        int service_pack = 0;
        {
            const std::size_t save = p;
            std::size_t q = p;
            if (q < text.size() && is_sep(text[q])) ++q;
            if (q + 2 <= text.size() &&
                std::toupper(static_cast<unsigned char>(text[q])) == 'S' &&
                std::toupper(static_cast<unsigned char>(text[q + 1])) == 'P') {
                q += 2;
                const auto sp = read_digits(q);
                if (sp.has_value()) {
                    service_pack = *sp;
                    p = q;
                } else {
                    p = save;
                }
            } else {
                p = save;
            }
        }

        if (p >= text.size() || !is_sep(text[p])) continue;
        ++p;
        const auto build = read_digits(p);
        if (!build.has_value()) continue;

        if (p >= text.size() || !is_sep(text[p])) continue;
        ++p;
        const auto patch = read_digits(p);
        if (!patch.has_value()) continue;

        return {year, *version, service_pack, *build, *patch};
    }

    // 退化：提取所有数字
    std::vector<int> numbers;
    long long value = 0;
    bool in_number = false;
    for (char c : text) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            value = value * 10 + (c - '0');
            if (value > 1000000000LL) value = 1000000000LL;
            in_number = true;
        } else if (in_number) {
            numbers.push_back(static_cast<int>(value));
            value = 0;
            in_number = false;
        }
    }
    if (in_number) numbers.push_back(static_cast<int>(value));
    return numbers;
}

int CompareVersionKey(const std::vector<int>& a, const std::vector<int>& b) {
    // Python tuple 比较：按元素依次比；共同前缀相同则**更长的更大**
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    if (a.size() == b.size()) return 0;
    return a.size() < b.size() ? -1 : 1;
}

// -----------------------------------------------------------------------------
// 路径归一
// -----------------------------------------------------------------------------

std::string NormaliseRoot(std::string_view root) {
    if (root.empty()) return {};
    // 对应 Python: os.path.abspath(root).replace("\\", "/")
    std::string path = plat::normalize_path(root);
    for (char& c : path) {
        if (c == '\\') c = '/';
    }

    // 以 td_commands_prompt.exe 结尾 -> 取上上级
    if (EndsWithLower(path, "/" + ToLower(kTdExeName)) || EndsWithLower(path, ToLower(kTdExeName))) {
        path = plat::path_dirname(path);            // .../bin
        path = plat::path_dirname(path);            // 安装根
        return path;
    }
    if (EndsWithLower(path, "/bin")) {
        return plat::path_dirname(path);
    }
    return path;
}

std::string ReleaseOf(std::string_view root) {
    std::string path(root);
    for (char& c : path) {
        if (c == '\\') c = '/';
    }
    // 对应 Python: r"TD_Release_([0-9A-Za-z_.]+)"
    constexpr std::string_view kNeedle = "TD_Release_";
    const std::size_t p = path.find(kNeedle);
    if (p != std::string::npos) {
        std::size_t i = p + kNeedle.size();
        const std::size_t start = i;
        while (i < path.size()) {
            const char c = path[i];
            const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                            (c >= 'a' && c <= 'z') || c == '_' || c == '.';
            if (!ok) break;
            ++i;
        }
        if (i > start) return path.substr(start, i - start);
    }
    return plat::path_filename(path);
}

// -----------------------------------------------------------------------------
// 环境注入
// -----------------------------------------------------------------------------

TdEnv TdEnv::Default() {
    TdEnv env;
    env.list_dir = [](std::string_view dir) {
        std::vector<std::string> names;
        try {
            // 注意：C++23 下 std::filesystem::path(std::string) 存在歧义（构造函数被删），
            // 必须显式走 u8path/字符串转换。
            const std::filesystem::path p{std::string(dir)};
            if (!std::filesystem::is_directory(p)) return names;
            for (const auto& entry : std::filesystem::directory_iterator(p)) {
                names.push_back(entry.path().filename().string());
            }
        } catch (...) {
            names.clear();
        }
        return names;
    };
    env.is_file = [](std::string_view path) {
        try {
            const std::filesystem::path p{std::string(path)};
            return std::filesystem::is_regular_file(p);
        } catch (...) {
            return false;
        }
    };
    env.get_env = [](std::string_view name) {
        const std::string key(name);
#if defined(_MSC_VER)
        // C4996: MSVC 认为 std::getenv 不安全（返回指向内部静态缓冲的指针）。
        // _dupenv_s 分配一份副本，语义与 Python 的 os.environ.get 一致：调用方拥有返回值。
        char* buffer = nullptr;
        size_t size = 0;
        if (_dupenv_s(&buffer, &size, key.c_str()) != 0 || buffer == nullptr) {
            return std::string();
        }
        std::string value(buffer);
        std::free(buffer);
        return value;
#else
        const char* value = std::getenv(key.c_str());
        return value != nullptr ? std::string(value) : std::string();
#endif
    };
    env.drives = "CDEFGHIJ";
    return env;
}

// -----------------------------------------------------------------------------
// 扫描
// -----------------------------------------------------------------------------

std::vector<std::string> InstallRootsWith(const TdEnv& env) {
    std::vector<std::string> roots;
    std::vector<std::string> seen;

    auto push = [&](std::string_view raw) {
        if (raw.empty()) return;
        std::string root = plat::normalize_path(raw);
        for (char& c : root) {
            if (c == '\\') c = '/';
        }
        // 指向 exe -> 上两级
        if (EndsWithLower(root, ToLower(kTdExeName))) {
            root = plat::path_dirname(plat::path_dirname(root));
        }
        if (EndsWithLower(root, "/bin")) {
            root = plat::path_dirname(root);
        }
        if (root.empty()) return;

        const std::string key = ToLower(root);
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) return;
        seen.push_back(key);
        roots.push_back(root);
    };

    for (char drive : env.drives) {
        const std::string d(1, drive);

        // 三个浅层基目录
        const std::string bases[3] = {
            d + ":/" + kReleaseDir,
            d + ":/Program Files/" + kReleaseDir,
            d + ":/Program Files (x86)/" + kReleaseDir,
        };
        for (const auto& base : bases) {
            for (const auto& name : env.list_dir(base)) {
                if (StartsWithUpper(name, "TD_RELEASE")) {
                    push(base + "/" + name);
                }
            }
        }

        // 深一层：<drive>/<任意目录>/Anlogic/TD_Release_*
        for (const auto& top : env.list_dir(d + ":/")) {
            const std::string candidate = d + ":/" + top + "/" + kReleaseDir;
            for (const auto& name : env.list_dir(candidate)) {
                if (StartsWithUpper(name, "TD_RELEASE")) {
                    push(candidate + "/" + name);
                }
            }
        }
    }

    return roots;
}

std::vector<TdInstall> InstallsWith(const TdEnv& env) {
    std::vector<TdInstall> found;

    for (const auto& root : InstallRootsWith(env)) {
        const std::string exe = plat::path_join(root, std::string("bin/") + kTdExeName);
        std::string exe_slash = exe;
        for (char& c : exe_slash) {
            if (c == '\\') c = '/';
        }
        if (!env.is_file(exe_slash)) continue;

        TdInstall install;
        install.root = root;
        install.exe = exe_slash;
        install.version = ReleaseOf(root);
        install.key = VersionKey(install.version);
        found.push_back(std::move(install));
    }

    // 对应 Python: sort(key=(key, root), reverse=True) —— 最新在前
    std::sort(found.begin(), found.end(), [](const TdInstall& a, const TdInstall& b) {
        const int c = CompareVersionKey(a.key, b.key);
        if (c != 0) return c > 0;
        return a.root > b.root;
    });

    return found;
}

// -----------------------------------------------------------------------------
// 安装发现缓存（系统临时目录）
// -----------------------------------------------------------------------------
// 目的：把"扫盘找 TD"的一次性成本（本机约 20 s）从**每个进程**降到**每次安装变化**。
// 目录选 %TEMP%\fpgalsp\ 而不是工程目录：这是整机级事实（哪台机器装了哪个 TD），
// 不该跟着某个工程走，也不该往用户工程里写文件。
namespace {

std::string CacheDir() {
    std::error_code ec;
    const std::filesystem::path temp = std::filesystem::temp_directory_path(ec);
    if (ec || temp.empty()) return {};
    return plat::normalize_path((temp / "fpgalsp").string());
}

std::string CachePath() {
    const std::string dir = CacheDir();
    if (dir.empty()) return {};
    return plat::path_join(dir, "td-install-cache.txt");
}

bool CacheDisabled(const TdEnv& env) {
    const std::string flag = env.get_env("FPGALSP_NO_TD_CACHE");
    return !flag.empty() && flag != "0";
}

long long CacheTtlSeconds(const TdEnv& env) {
    const std::string raw = env.get_env("FPGALSP_TD_CACHE_TTL");
    if (!raw.empty()) {
        long long value = 0;
        bool digits = true;
        for (const char c : raw) {
            if (c < '0' || c > '9') {
                digits = false;
                break;
            }
            value = value * 10 + (c - '0');
            if (value > 100000000) break;
        }
        if (digits) return value;  // 0 = 永不过期
    }
    return 24 * 60 * 60;  // 默认 1 天
}

long long NowSeconds() {
    return static_cast<long long>(std::time(nullptr));
}

std::optional<TdChoice> ReadCache(const TdEnv& env) {
    const std::string path = CachePath();
    if (path.empty() || !env.is_file(path)) return std::nullopt;
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;

    std::string root;
    std::string exe;
    std::string version;
    long long saved_at = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (key == "root") {
            root = value;
        } else if (key == "exe") {
            exe = value;
        } else if (key == "version") {
            version = value;
        } else if (key == "saved_at") {
            saved_at = std::atoll(value.c_str());
        }
    }
    if (root.empty() || exe.empty()) return std::nullopt;
    // 卸载/换版本后缓存必须失效：exe 不在了就重新扫。
    if (!env.is_file(exe)) return std::nullopt;
    const long long ttl = CacheTtlSeconds(env);
    if (ttl > 0 && saved_at > 0 && NowSeconds() - saved_at > ttl) return std::nullopt;

    TdChoice choice;
    choice.root = root;
    choice.exe = exe;
    choice.version = ReleaseOf(root).empty() ? version : ReleaseOf(root);
    choice.source = "auto";   // 保持三值契约（explicit/auto/path），额外用 from_cache 标记
    choice.from_cache = true;
    return choice;
}

void WriteCache(const TdEnv& env, const TdChoice& choice) {
    (void)env;
    const std::string dir = CacheDir();
    const std::string path = CachePath();
    if (dir.empty() || path.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);  // 已存在时 ec 为 0，可直接忽略
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return;
    out << "# fpgalsp TD install discovery cache (auto-generated; delete to force a rescan)\n";
    out << "root=" << choice.root << "\n";
    out << "exe=" << choice.exe << "\n";
    out << "version=" << choice.version << "\n";
    out << "source=" << choice.source << "\n";
    out << "saved_at=" << NowSeconds() << "\n";
}

void ClearCache() {
    const std::string path = CachePath();
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

}  // namespace

std::vector<TdInstall> Installs() { return InstallsWith(TdEnv::Default()); }

std::vector<std::string> InstallRoots() { return InstallRootsWith(TdEnv::Default()); }

std::optional<TdChoice> FindTdWith(const TdEnv& env, std::string_view explicit_root) {
    // 1) 显式 + 环境变量
    std::vector<std::string> explicit_roots;
    if (!explicit_root.empty()) explicit_roots.emplace_back(explicit_root);
    for (const char* name : {"TD_ROOT", "ANLOGIC_TD_ROOT", "TD_HOME", "TD_INSTALL_DIR"}) {
        const std::string value = env.get_env(name);
        if (!value.empty()) explicit_roots.push_back(value);
    }

    for (const auto& raw : explicit_roots) {
        const std::string resolved = NormaliseRoot(raw);
        if (resolved.empty()) continue;
        std::string exe = plat::path_join(resolved, std::string("bin/") + kTdExeName);
        for (char& c : exe) {
            if (c == '\\') c = '/';
        }
        if (!env.is_file(exe)) continue;

        TdChoice choice;
        choice.root = resolved;
        choice.exe = exe;
        choice.version = ReleaseOf(resolved);
        choice.source = "explicit";
        for (const auto& item : InstallsWith(env)) {
            if (ToLower(item.root) != ToLower(resolved)) {
                choice.alternatives.push_back(item);
            }
        }
        return choice;
    }

    // 2) 自动检测（最新）
    const auto found = InstallsWith(env);
    if (!found.empty()) {
        TdChoice choice;
        choice.root = found[0].root;
        choice.exe = found[0].exe;
        choice.version = found[0].version;
        choice.source = "auto";
        choice.alternatives.assign(found.begin() + 1, found.end());
        return choice;
    }

    // 3) PATH 回退
    const std::string path_env = env.get_env("PATH");
    std::size_t start = 0;
    while (start <= path_env.size()) {
        const std::size_t semi = path_env.find(';', start);
        const std::string entry = path_env.substr(
            start, (semi == std::string::npos ? path_env.size() : semi) - start);
        start = (semi == std::string::npos) ? path_env.size() + 1 : semi + 1;
        if (entry.empty()) continue;

        std::string exe = plat::path_join(entry, kTdExeName);
        for (char& c : exe) {
            if (c == '\\') c = '/';
        }
        if (!env.is_file(exe)) continue;

        // 对应 Python: dirname(dirname(abspath(entry)))
        const std::string abs_entry = plat::normalize_path(entry);
        TdChoice choice;
        choice.root = plat::path_dirname(plat::path_dirname(abs_entry));
        choice.exe = exe;
        choice.version = ReleaseOf(choice.root);
        choice.source = "path";
        return choice;
    }

    return std::nullopt;
}

std::optional<TdChoice> FindTd(std::string_view explicit_root) {
    // ⚠ 为什么要有缓存：自动发现要扫 C..J 盘的 Anlogic 目录 + PATH，本机实测一次约
    //   **20 s**（MCP 每次新建进程都要付一次）。而安装位置一年也不会变一次，所以把
    //   发现结果按"路径 + 版本"记在系统临时目录里，命中就直接用。
    //
    //   失效条件（任一即重新扫描）：
    //     * FPGALSP_NO_TD_CACHE=1
    //     * 缓存里记录的 exe 已不存在（卸载/换版本）
    //     * 超过 TTL（默认 24 h；FPGALSP_TD_CACHE_TTL=<秒> 可改，0 = 永不过期）
    //   显式指定（参数 / TD_ROOT 等环境变量）时**不读缓存**（用户明确要求用哪一份），
    //   但仍然会刷新缓存。
    const TdEnv env = TdEnv::Default();
    const auto explicit_roots = [&env]() {
        std::vector<std::string> roots;
        for (const char* name : {"TD_ROOT", "ANLOGIC_TD_ROOT", "TD_HOME", "TD_INSTALL_DIR"}) {
            const std::string value = env.get_env(name);
            if (!value.empty()) roots.push_back(value);
        }
        return roots;
    };

    const bool pinned = !explicit_root.empty() || !explicit_roots().empty();
    if (!pinned && !CacheDisabled(env)) {
        if (std::optional<TdChoice> cached = ReadCache(env); cached.has_value()) {
            return cached;
        }
    }

    std::optional<TdChoice> choice = FindTdWith(env, explicit_root);
    if (choice.has_value()) {
        WriteCache(env, *choice);
    } else {
        ClearCache();
    }
    return choice;
}

}  // namespace fpgalsp::td