// =============================================================================
//  src/mcp/bridge.cpp —— MCP 工具实现
// =============================================================================
//  逐条移植 Y:\fpga-lsp\fpgalsp\mcp_bridge.py 的对应方法（注释里标了 Python 方法名）。
//  未移植的模块一律显式占位（见 bridge.h 的分阶段说明）。
// =============================================================================

#include "mcp/bridge.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <re2/re2.h>

#include "analysis/analysis.h"
#include "constraints/adc.h"
#include "flow/flow_runner.h"
#include "flow/programmer.h"
#include "flow/probe.h"
#include "flow/strategy.h"
#include "hdl/hdl_index.h"
#include "jsonrpc/dispatcher.h"
#include "mcp/guide.h"
#include "server/features.h"
#include "constraints/sdc.h"
#include "platform/encoding.h"
#include "platform/log.h"
#include "platform/path.h"
#include "project/al_project.h"
#include "reports/reports_aggregate.h"
#include "server/features_rename_util.h"
#include "server/lsp_server.h"
#include "server/report_store.h"
#include "td/td_device.h"
#include "td/td_diagnostics.h"
#include "td/td_session.h"

// Phase B / B6：MCP format 与 LSP formatting 用同一个 Verible 桥。
// 宏由 CMake 定义（FPGA_LSP_VERIBLE_ENABLED=ON 时）；关闭构建时 format 返回
// "No formatter available（Verible missing and fallback disabled）"，与 Python 的
// ToolError 文案一致。
#if defined(FPGA_LSP_HAVE_VERIBLE_BRIDGE)
#include "verible_bridge/format_text.h"
#endif

namespace fpgalsp::mcp {
namespace {

namespace fs = std::filesystem;
using platform::normalize_path;
using platform::path_dirname;
using platform::path_filename;
using platform::path_join;

// Python mcp_bridge.SEVERITY
const char* SeverityName(int severity) {
    switch (severity) {
        case 1: return "error";
        case 2: return "warning";
        case 3: return "information";
        case 4: return "hint";
        default: return "warning";
    }
}

// std::filesystem 的 u8string() 在 C++20 返回 std::u8string；项目内部统一用 UTF-8 字节串
std::string ToUtf8(const fs::path& path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

bool IsFile(const std::string& path) {
    std::error_code ec;
    return fs::is_regular_file(fs::u8path(path), ec);
}

bool IsDirectory(const std::string& path) {
    std::error_code ec;
    return fs::is_directory(fs::u8path(path), ec);
}

bool PathExists(const std::string& path) {
    std::error_code ec;
    return fs::exists(fs::u8path(path), ec);
}

std::string ReadTextFile(const std::string& path) {
    const platform::ReadFileResult result = platform::read_file_utf8(path);
    return result.content;
}

// Python project.rel(path)：相对工程目录、正斜杠
std::string RelativeTo(const std::string& path, const std::string& base) {
    std::error_code ec;
    const fs::path relative = fs::relative(fs::u8path(path), fs::u8path(base), ec);
    if (ec || relative.empty()) return std::string();
    std::string text = relative.generic_string();
    if (text == ".") return std::string();
    // Python os.path.relpath 会给出 ../ 形式；fs::relative 同语义
    return text;
}

std::string BaseName(const std::string& path) { return path_filename(path); }

// Python os.path.isdir(join(dir, "<name>_Runs"))
std::vector<std::string> ListSubdirectories(const std::string& dir) {
    std::vector<std::string> names;
    std::error_code ec;
    fs::directory_iterator it(fs::u8path(dir), ec);
    if (ec) return names;
    for (const fs::directory_entry& entry : it) {
        if (!entry.is_directory(ec) || ec) continue;
        names.push_back(ToUtf8(entry.path().filename()));
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::string> ListFiles(const std::string& dir) {
    std::vector<std::string> names;
    std::error_code ec;
    fs::directory_iterator it(fs::u8path(dir), ec);
    if (ec) return names;
    for (const fs::directory_entry& entry : it) {
        if (!entry.is_regular_file(ec) || ec) continue;
        names.push_back(ToUtf8(entry.path().filename()));
    }
    std::sort(names.begin(), names.end());
    return names;
}

int CountOccurrences(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return 0;
    int count = 0;
    size_t position = 0;
    while ((position = haystack.find(needle, position)) != std::string_view::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

std::string LowerAscii(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::string Trim(std::string_view text) {
    size_t begin = 0;
    size_t end = text.size();
    const auto is_space = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
    };
    while (begin < end && is_space(text[begin])) ++begin;
    while (end > begin && is_space(text[end - 1])) --end;
    return std::string(text.substr(begin, end - begin));
}

std::vector<std::string> SplitLines(std::string_view text) {
    std::vector<std::string> lines;
    std::string current;
    for (const char c : text) {
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
        } else if (c != '\r') {
            current.push_back(c);
        }
    }
    lines.push_back(current);
    return lines;
}

// -----------------------------------------------------------------------------
// F4 / R7：MCP 侧的两个进程内缓存（只影响延迟，不改变任何响应字段）
// -----------------------------------------------------------------------------
//  为什么放在 bridge.cpp 内部（匿名命名空间）而不是 Bridge 成员：
//    * 本任务的写域只有 src/mcp/bridge.cpp（PLAN §2.1），不动 bridge.h；
//    * Bridge 每个 MCP 连接/进程一个实例，缓存做成文件级单例后，同一进程内的
//      多个连接（loopback TCP 每个连接新建 Bridge）也能共享，收益更大。
//  上界（内存）：
//    * 文本缓存：32 MiB 文本 + 4096 条，LRU 淘汰（200 文件工程实测占用 0.08 MB）；
//    * 工程列表缓存：最多 8 个 root，每个只存 1 个 Json 数组（200 工程 ≈ 10 KB）。
//  失效条件（明确，不依赖调用方）：
//    * 文本：键 (规范化 path)，命中前 stat 比对 (size, mtime) —— 任何写文件立刻失效；
//    * 工程列表：base 目录 mtime + 每个已发现 .al 的 (size, mtime) 比对；
//      另有 5 s TTL 兜底"在已有子目录里新增 .al"（目录枚举本身 8 ms，不能每请求做）。

// 取一个路径的 (mtime, size)；失败返回 false。
// 注：用 char8_t 构造 path（C++20 的 u8path 已弃用），避免引入新的 C4996 告警。
fs::path AsPath(const std::string& utf8) {
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

bool StatStamp(const std::string& path, std::int64_t* mtime_ns, std::uintmax_t* size) {
    std::error_code ec;
    const fs::path native = AsPath(path);
    const std::uintmax_t bytes = fs::file_size(native, ec);
    if (ec) return false;
    const fs::file_time_type write_time = fs::last_write_time(native, ec);
    if (ec) return false;
    if (mtime_ns != nullptr) {
        *mtime_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        write_time.time_since_epoch())
                        .count();
    }
    if (size != nullptr) *size = bytes;
    return true;
}

// 目录 mtime（用 last_write_time，目录也支持）。
bool StatDirMtime(const std::string& path, std::int64_t* mtime_ns) {
    std::error_code ec;
    const fs::file_time_type write_time = fs::last_write_time(AsPath(path), ec);
    if (ec) return false;
    if (mtime_ns != nullptr) {
        *mtime_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        write_time.time_since_epoch())
                        .count();
    }
    return true;
}

// 按 '\n' 切行、返回视图（不复制）。与 SplitLines 的差异只有一个：
// SplitLines 会丢掉所有 '\r'，视图做不到"中途删字符"——所以含 '\r' 的文本
// 调用方必须回落到 SplitLines（见 SearchTextTool），保证匹配语义逐字节一致。
std::vector<std::string_view> SplitLineViews(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = text.find('\n', start);
        if (pos == std::string_view::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, pos - start));
        start = pos + 1;
    }
    return lines;
}

// 快速相对路径：proj.dir 与 file_path 都已规范化（正斜杠、绝对），
// 前缀相减即可，不必走 fs::relative（它每次 canonicalize 两个路径，本机 ≈25–70 µs）。
// 前缀不匹配（跨盘 / 大小写异常）时回落到原来的 RelativeTo，语义不变。
std::string FastRelativeTo(const std::string& path, const std::string& base) {
    if (!base.empty() && path.size() > base.size()) {
        if (path.compare(0, base.size(), base) == 0 && (path[base.size()] == '/' || path[base.size()] == '\\')) {
            return path.substr(base.size() + 1);
        }
    }
    return RelativeTo(path, base);
}

// 目录级快照：一次 FindFirstFile 枚举拿到该目录下所有条目的 (size, mtime)。
// 为什么不用"每文件 stat"：本机每次 stat 要 2 次 open（本机 Defender 使每次 open ~40–90 µs），
// 200 文件 = 14 ms+，比读文件本身还贵；目录枚举 201 条目只要 0.6 ms（实测）。
struct FileInfoLite {
    std::uintmax_t size = 0;
    std::int64_t mtime_ns = 0;
};

std::map<std::string, FileInfoLite> SnapshotDirectories(
    const std::vector<std::string>& directories) {
    std::map<std::string, FileInfoLite> info;
    for (const std::string& directory : directories) {
        std::error_code ec;
        fs::directory_iterator iterator(AsPath(directory), ec);
        if (ec) continue;
        for (const fs::directory_entry& entry : iterator) {
            const std::string path = normalize_path(ToUtf8(entry.path()));
            std::error_code entry_ec;
            const std::uintmax_t size = entry.file_size(entry_ec);
            if (entry_ec) continue;
            const fs::file_time_type write_time = entry.last_write_time(entry_ec);
            if (entry_ec) continue;
            FileInfoLite lite;
            lite.size = size;
            lite.mtime_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(write_time.time_since_epoch())
                    .count();
            info[path] = lite;
        }
    }
    return info;
}

// 文本缓存（LRU，有界）。命中条件 = (size, mtime) 与缓存时一致。
class TextCache {
  public:
    static TextCache& Instance() {
        static TextCache instance;
        return instance;
    }

    // 用**调用方给出的 stamp**（来自目录快照）判断命中，命中时不再碰文件系统。
    std::shared_ptr<const std::string> Get(const std::string& path, std::uintmax_t size,
                                           std::int64_t mtime_ns) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = entries_.find(path);
            if (it != entries_.end() && it->second.size == size &&
                it->second.mtime_ns == mtime_ns) {
                Touch(it->second);
                return it->second.text;
            }
        }
        const platform::ReadFileResult read = platform::read_file_utf8(path);
        if (!read.ok) return nullptr;
        auto text = std::make_shared<const std::string>(read.content);
        std::lock_guard<std::mutex> lock(mutex_);
        auto& entry = entries_[path];
        if (entry.text) {
            bytes_ -= entry.text->size();
        } else {
            order_.push_front(path);
            entry.order_pos = order_.begin();
        }
        entry.size = size;
        entry.mtime_ns = mtime_ns;
        entry.text = text;
        bytes_ += text->size();
        Evict();
        return text;
    }

    // 仅测试/诊断用（不进协议）。
    std::size_t bytes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return bytes_;
    }
    std::size_t count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.size();
    }

  private:
    struct Entry {
        std::uintmax_t size = 0;
        std::int64_t mtime_ns = 0;
        std::shared_ptr<const std::string> text;
        std::list<std::string>::iterator order_pos;
    };

    static constexpr std::size_t kMaxBytes = 32u * 1024u * 1024u;
    static constexpr std::size_t kMaxEntries = 4096;

    void Touch(Entry& entry) { order_.splice(order_.begin(), order_, entry.order_pos); }

    void Evict() {
        while ((bytes_ > kMaxBytes || entries_.size() > kMaxEntries) && !order_.empty()) {
            const std::string victim = order_.back();
            order_.pop_back();
            const auto it = entries_.find(victim);
            if (it != entries_.end()) {
                bytes_ -= it->second.text ? it->second.text->size() : 0;
                entries_.erase(it);
            }
        }
    }

    mutable std::mutex mutex_;
    std::map<std::string, Entry> entries_;
    std::list<std::string> order_;  // 前端 = 最近使用
    std::size_t bytes_ = 0;
};

// 工程列表缓存：键 = 规范化后的搜索根（小写）。
// 值 = 已构建好的 `projects` Json 数组 + 校验用的 stamp。
struct ProjectListEntry {
    std::int64_t base_mtime = 0;
    std::vector<std::pair<std::string, std::pair<std::uintmax_t, std::int64_t>>> als;
    Json projects = Json::array();
    // W4-A：Bridge 构造函数默认工程发现直接复用这里的结果（原来每个连接都要
    // 重新 DiscoverProjects + 重新建工程对象，TCP attach 冷启动 = 8–16 ms/连接）。
    std::string chosen_al;
    std::chrono::steady_clock::time_point refresh_after{};
    std::chrono::steady_clock::time_point last_used{};
};

std::mutex g_project_list_mutex;
std::map<std::string, ProjectListEntry> g_project_list_cache;

constexpr std::size_t kProjectListMaxRoots = 8;
constexpr std::chrono::seconds kProjectListTtl{5};

// 对应 Python mcp_bridge.project_list 的循环体，带缓存：
//   * 命中：base 目录 mtime + 每个已发现 .al 的 (size, mtime) 全部一致，且未过 TTL；
//   * 未命中：DiscoverProjects + ParseAl 重建，并把结果替换进缓存。
// 返回的 Json 数组是缓存值的**拷贝**（调用方要把它放进响应对象里）。
Json CachedProjectListEntries(const std::string& base) {
    const std::string key = LowerAscii(normalize_path(base));
    const auto now = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(g_project_list_mutex);
        const auto it = g_project_list_cache.find(key);
        if (it != g_project_list_cache.end()) {
            bool valid = now < it->second.refresh_after;
            if (valid) {
                std::int64_t base_mtime = 0;
                valid = StatDirMtime(base, &base_mtime) && base_mtime == it->second.base_mtime;
            }
            if (valid) {
                for (const auto& item : it->second.als) {
                    std::int64_t mtime_ns = 0;
                    std::uintmax_t size = 0;
                    if (!StatStamp(item.first, &mtime_ns, &size) || mtime_ns != item.second.second ||
                        size != item.second.first) {
                        valid = false;
                        break;
                    }
                }
            }
            if (valid) {
                it->second.last_used = now;
                return it->second.projects;
            }
        }
    }

    // 重建（不持锁，避免把 8–25 ms 的扫描串行化到其它请求上）
    const std::vector<std::string> found = project::DiscoverProjects(base, 6);
    Json projects = Json::array();
    std::vector<std::pair<std::string, std::pair<std::uintmax_t, std::int64_t>>> als;
    int index = 0;
    std::string chosen_al;
    const std::string base_lower = LowerAscii(normalize_path(base));
    for (const std::string& al : found) {
        if (index++ >= 100) break;
        const project::Project parsed = project::ParseAl(al);
        Json item = Json::object();
        item["name"] = parsed.name;
        item["al"] = parsed.al_path;
        item["dir"] = parsed.dir;
        item["family"] = parsed.family;
        item["device"] = parsed.device;
        item["top"] = parsed.top;
        item["hdl"] = static_cast<int>(parsed.HdlFiles().size());
        item["warnings"] = static_cast<int>(parsed.warnings.size());
        projects.push_back(std::move(item));

        // 与 DiscoverDefaultAl 相同的优先级：工程根下的 .al 优先，否则第一个
        if (chosen_al.empty()) chosen_al = al;
        if (LowerAscii(path_dirname(al)) == base_lower) chosen_al = al;

        std::int64_t mtime_ns = 0;
        std::uintmax_t size = 0;
        (void)StatStamp(al, &mtime_ns, &size);
        als.emplace_back(normalize_path(al), std::make_pair(size, mtime_ns));
    }
    std::int64_t base_mtime = 0;
    (void)StatDirMtime(base, &base_mtime);

    {
        std::lock_guard<std::mutex> lock(g_project_list_mutex);
        auto& slot = g_project_list_cache[key];
        slot.base_mtime = base_mtime;
        slot.als = std::move(als);
        slot.projects = projects;
        slot.chosen_al = chosen_al;
        slot.last_used = now;
        slot.refresh_after = now + kProjectListTtl;
        while (g_project_list_cache.size() > kProjectListMaxRoots) {
            auto victim = g_project_list_cache.begin();
            for (auto it = g_project_list_cache.begin(); it != g_project_list_cache.end(); ++it) {
                if (it->second.last_used < victim->second.last_used) victim = it;
            }
            if (victim == g_project_list_cache.end()) break;
            g_project_list_cache.erase(victim);
        }
    }
    return projects;
}

// -----------------------------------------------------------------------------
// W4-A：进程级"默认工程发现 + .al 解析结果"缓存（连接级复用）
// -----------------------------------------------------------------------------
//  背景：loopback TCP 主机对**每个连接**新建一个 Bridge，构造时要做
//  `DiscoverProjects`(200 文件工程实测 7.7–8.1 ms) + 首次工具调用时 `ParseAl`。
//  多个连接其实看的是同一个工作区，完全没必要重复做。
//
//  为什么不是"多个连接共享同一个 Bridge"：Bridge 的可变状态（projects_/devices_/
//  session_/td_choice_/flow_runner_/feature_server_/active_al_）全部没有锁，
//  而宿主 mcp_tcp.cpp 的 accept/Serve 实现不在本任务写域（src/mcp/**）内；
//  直接共享会出现"连接 A 的 active project 泄漏到连接 B"以及关闭连接时把正在
//  跑的请求所用的会话析构掉。这里改为共享**不可变结果**（发现结果 + 解析后的
//  Project），每个连接仍有自己的 Bridge/active_al_/TD 会话，语义零变化。

// 取默认工程（替代 Bridge::DiscoverDefaultAl 的每次重扫）。
// 语义与 DiscoverDefaultAl 完全一致：优先工程根下的 .al，否则发现顺序里的第一个；
// 找不到抛 ToolError（构造函数的 catch 分支照旧）。
std::string CachedDefaultAl(const std::string& root) {
    (void)CachedProjectListEntries(root);  // 确保工程列表缓存已建立/已校验
    const std::string key = LowerAscii(normalize_path(root));
    {
        std::lock_guard<std::mutex> lock(g_project_list_mutex);
        const auto it = g_project_list_cache.find(key);
        if (it != g_project_list_cache.end() && !it->second.chosen_al.empty()) {
            return it->second.chosen_al;
        }
    }
    throw ToolError("No Anlogic .al project found under " + root);
}

// 进程级 .al 解析缓存：键 = 规范化 .al 路径，命中条件 = (size, mtime) 一致。
// 这样第二个连接不需要重新 ParseAl；同时 `.al` 被改/被换掉时会自动重新解析
// （与 D5 的索引 stamp 校验同一套思路，只是粒度更粗）。
struct GlobalProjectEntry {
    std::uintmax_t size = 0;
    std::int64_t mtime_ns = 0;
    std::shared_ptr<const project::Project> project;
    std::chrono::steady_clock::time_point last_used{};
};

std::mutex g_project_cache_mutex;
std::map<std::string, GlobalProjectEntry> g_project_cache;
constexpr std::size_t kProjectCacheMax = 8;

std::shared_ptr<const project::Project> CachedParsedProject(const std::string& al_path) {
    const std::string key = normalize_path(al_path);
    std::int64_t mtime_ns = 0;
    std::uintmax_t size = 0;
    const bool stat_ok = StatStamp(key, &mtime_ns, &size);
    const auto now = std::chrono::steady_clock::now();
    if (stat_ok) {
        std::lock_guard<std::mutex> lock(g_project_cache_mutex);
        const auto it = g_project_cache.find(key);
        if (it != g_project_cache.end() && it->second.size == size &&
            it->second.mtime_ns == mtime_ns && it->second.project) {
            it->second.last_used = now;
            return it->second.project;
        }
    }
    auto parsed = std::make_shared<const project::Project>(project::ParseAl(key));
    {
        std::lock_guard<std::mutex> lock(g_project_cache_mutex);
        auto& slot = g_project_cache[key];
        slot.size = size;
        slot.mtime_ns = mtime_ns;
        slot.project = parsed;
        slot.last_used = now;
        while (g_project_cache.size() > kProjectCacheMax) {
            auto victim = g_project_cache.begin();
            for (auto it = g_project_cache.begin(); it != g_project_cache.end(); ++it) {
                if (it->second.last_used < victim->second.last_used) victim = it;
            }
            if (victim == g_project_cache.end()) break;
            g_project_cache.erase(victim);
        }
    }
    return parsed;
}

// .al 被写工具改过后必须丢掉进程级缓存，否则其它连接会继续看到旧名字/旧文件表。
void DropCachedProject(const std::string& al_path) {
    std::lock_guard<std::mutex> lock(g_project_cache_mutex);
    g_project_cache.erase(normalize_path(al_path));
}

// Python fnmatch.fnmatch 的子集（* 与 ?）。MCP 的 glob 参数只用到这个程度。
bool GlobMatch(std::string_view pattern, std::string_view text) {
    size_t p = 0;
    size_t t = 0;
    size_t star = std::string_view::npos;
    size_t mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

// 文本扫描端口时的词法：标识符字符集（与 Python re 的 \w 基本一致，ASCII 版）
bool IsIdentStart(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
bool IsIdentChar(char c) {
    return IsIdentStart(c) || (c >= '0' && c <= '9') || c == '$';
}

// 1-based 位置（对应 Python _one_based_range 的 start）
Json DiagnosticJson(const lsp::Diagnostic& diagnostic, const std::string& path,
                    const project::Project& proj) {
    Json item = Json::object();
    item["file"] = path;
    item["line"] = diagnostic.range.start.line + 1;
    item["column"] = diagnostic.range.start.character + 1;
    item["endLine"] = diagnostic.range.end.line + 1;
    item["endColumn"] = diagnostic.range.end.character + 1;
    item["severity"] = SeverityName(diagnostic.severity);
    item["code"] = diagnostic.code;
    item["source"] = diagnostic.source;
    item["message"] = diagnostic.message;
    if (!path.empty()) item["relative"] = RelativeTo(path, proj.dir);
    return item;
}

// 把 JSON 数组截到前 limit 项（Python 的 list[:limit]）
Json TrimListTo(const Json& value, int limit) {
    Json out = Json::array();
    if (!value.is_array()) return out;
    int count = 0;
    for (const Json& item : value) {
        if (count++ >= limit) break;
        out.push_back(item);
    }
    return out;
}

// 前置声明：定义在文件后半段
std::vector<ScannedPort> ScanPortHeader(std::string_view header, size_t header_offset);

// 把 // 与 /* */ 注释替换成空格（保留换行与字节偏移，便于 span 仍指向原文）。
// 为什么需要：top.v 的端口列表里有 `input SUPSTS, // driver enable output signal`
// 这样的行尾注释；不先清空注释，扫描会把注释里的 "output signal" 当成一个端口。
std::string BlankComments(std::string_view text) {
    std::string out(text);
    size_t index = 0;
    while (index < out.size()) {
        if (out[index] == '/' && index + 1 < out.size() && out[index + 1] == '/') {
            while (index < out.size() && out[index] != '\n') {
                out[index] = ' ';
                ++index;
            }
            continue;
        }
        if (out[index] == '/' && index + 1 < out.size() && out[index + 1] == '*') {
            out[index] = ' ';
            out[index + 1] = ' ';
            index += 2;
            while (index + 1 < out.size() && !(out[index] == '*' && out[index + 1] == '/')) {
                if (out[index] != '\n') out[index] = ' ';
                ++index;
            }
            if (index + 1 < out.size()) {
                out[index] = ' ';
                out[index + 1] = ' ';
                index += 2;
            }
            continue;
        }
        ++index;
    }
    return out;
}
Json SummarizeReports(const Json& data, const std::string& project_name, std::string stage,
                      std::string section, int limit, bool include_raw);

// B6c：code_actions / rename_edits 需要一个"没有 transport 的 LspServer"上下文。
// Feature* 只把 transport 用于发布诊断/日志（codeAction/rename 路径上不会发），
// 所以一个 no-op transport 就够；ReadMessage 永远不会被调用（我们不跑 serve 循环）。
class NullTransport final : public jsonrpc::TransportBaseCommon {
public:
    ReadResult ReadMessage() override { return ReadResult{ReadStatus::EndOfStream, Json()}; }
    bool WriteMessage(const Json& /*message*/) override { return true; }
    bool closed() const override { return false; }
    void Close() override {}
    bool compact_json() const override { return true; }
    int parse_errors() const override { return 0; }
    int write_errors() const override { return 0; }
};

jsonrpc::TransportBaseCommon& NullTransportRef() {
    static NullTransport transport;
    return transport;
}

}  // namespace

// =============================================================================
//  构造 / 工程解析
// =============================================================================

Bridge::Bridge(BridgeOptions options) : options_(std::move(options)) {
    root_ = normalize_path(options_.root.empty() ? std::string(".") : options_.root);
    if (!IsDirectory(root_)) {
        throw ToolError("Root directory does not exist: " + root_);
    }
    if (!options_.project.empty()) {
        active_al_ = ResolveAl(options_.project);
    } else {
        try {
            // W4-A：不再每个连接重新 `DiscoverProjects`（200 文件工程 7.7–8.1 ms），
            // 改为复用进程级发现缓存（F4 引入、这里补充 chosen_al）。
            active_al_ = CachedDefaultAl(root_);
        } catch (const ToolError&) {
            active_al_.clear();  // 没有工程也能起服务：工程类工具按需报错
        }
    }
}

Bridge::~Bridge() = default;

// P-D1：报告缓存。附着模式直接借用宿主（LSP 进程）的 ReportStore，
// 这样 MCP 与编辑器命中的是同一份解析结果与快照；独立 stdio 模式才自建。
server::ReportStore& Bridge::Reports() {
    if (options_.shared_server != nullptr) {
        if (server::ReportStore* shared = options_.shared_server->report_store();
            shared != nullptr) {
            return *shared;
        }
    }
    if (!owned_report_store_) owned_report_store_ = std::make_unique<server::ReportStore>();
    return *owned_report_store_;
}

void Bridge::Shutdown() {
    if (session_) {
        session_->Close();
        session_.reset();
    }
    td_choice_.reset();
    devices_.clear();
    // B6c：feature 上下文随 Bridge 一起释放（它只是 Feature* 的宿主，没有外部资源）
    feature_server_.reset();
}

std::string Bridge::DiscoverDefaultAl() const {
    const std::vector<std::string> candidates = project::DiscoverProjects(root_, 6);
    if (candidates.empty()) throw ToolError("No Anlogic .al project found under " + root_);
    const std::string root_lower = LowerAscii(root_);
    for (const std::string& candidate : candidates) {
        if (LowerAscii(path_dirname(candidate)) == root_lower) return candidate;
    }
    return candidates.front();
}

std::string Bridge::ResolveAl(const std::string& value) const {
    const std::string trimmed = Trim(value);
    if (trimmed.empty()) return std::string();

    const std::string candidate =
        normalize_path(platform::is_absolute_path(trimmed) ? trimmed : path_join(root_, trimmed));
    // B7 / S-03：`--root` 是安全边界，不是"自动发现工程的起点"。绝对路径同样要落在
    // root_ 之内（用物理路径判定，防 junction/symlink 把边界搬走）。
    const bool inside_root = platform::path_is_within_canonical(candidate, root_);
    if (IsFile(candidate) && platform::path_extension(candidate) == ".al") {
        if (!inside_root) {
            throw ToolError("Project is outside the MCP root (" + root_ + "): " + candidate);
        }
        return candidate;
    }

    if (IsDirectory(candidate)) {
        if (!inside_root) {
            throw ToolError("Project directory is outside the MCP root (" + root_ + "): " +
                            candidate);
        }
        const std::string dot_al = path_join(candidate, ".al");
        if (IsFile(dot_al)) return normalize_path(dot_al);
        const std::vector<std::string> found = project::DiscoverProjects(candidate, 4);
        if (!found.empty()) return found.front();
        throw ToolError("No .al project found in " + candidate);
    }

    const std::string lowered = LowerAscii(trimmed);
    for (const std::string& al : project::DiscoverProjects(root_, 6)) {
        const project::Project parsed = project::ParseAl(al);
        if (LowerAscii(parsed.name) == lowered ||
            LowerAscii(BaseName(al)) == lowered) {
            return al;
        }
    }
    throw ToolError("Project not found: " + trimmed);
}

const project::Project& Bridge::ActiveProject() {
    if (!active_al_.empty()) {
        // W4-A：每次调用都问一次进程级缓存（内部按 (size,mtime) 校验，命中只花 1 次
        // stat + 一次 map 查找），因此 `.al` 改动后下一次工具调用就能看到新内容，
        // 而连接之间又共享同一份解析结果。
        projects_[active_al_] = CachedParsedProject(active_al_);
        return *projects_[active_al_];
    }
    active_al_ = CachedDefaultAl(root_);
    return ActiveProject();
}

const project::Project& Bridge::ProjectFor(const Json& arguments) {
    const auto it = arguments.find("project");
    if (it != arguments.end() && it->is_string() && !it->get<std::string>().empty()) {
        const std::string al = ResolveAl(it->get<std::string>());
        projects_[al] = CachedParsedProject(al);
        active_al_ = al;
        return *projects_[al];
    }
    return ActiveProject();
}

std::string Bridge::ResolveFile(const std::string& value, const project::Project& proj) const {
    if (value.empty()) throw ToolError("file is required");
    std::string candidate = value;
    if (!platform::is_absolute_path(candidate)) {
        const std::string in_project = normalize_path(path_join(proj.dir, candidate));
        if (IsFile(in_project)) {
            candidate = in_project;
        } else {
            const std::string in_root = normalize_path(path_join(root_, candidate));
            // 两个基目录都没有这个文件时仍以 in_project 为准往下走边界判定，这样
            // "相对名写错"报 File not found，而"用 .. 逃出边界"报越界（错误才可诊断）；
            // 顺带去掉旧实现里"相对路径最终按进程 cwd 解析"这个隐含的第三个基目录。
            candidate = IsFile(in_root) ? in_root : in_project;
        }
    }
    candidate = normalize_path(candidate);
    // SEC-02（doc/audit/2026-11-19/static/SECURITY.md）：11 个只读工具共用这里的返回值，
    // 而 --root 是沙箱根（Bridge 构造时 root 不存在就直接抛错）。读文件的权利必须在这一层
    // 统一约束：逐个调用点补校验必然漏（同一权利的重复实现正是本轮漏洞的成因）。
    // 边界取「工程目录或 root_」而不是只取 proj.dir：root_ 下可能有与工程不同目录的
    // 合法文件（--root 为信任根），只认 proj.dir 会破坏既有用例。
    if (!IsWithin(candidate, proj.dir) && !IsWithin(candidate, root_)) {
        throw ToolError("File is outside the project directory and the MCP root (" + root_ +
                        "): " + candidate);
    }
    // B7 / S-04：词法包含挡不住符号链接/junction（工程内放一个 junction 就能读出外部
    // 文件），所以和 SafeProjectFile 一样再补一次 canonical 物理复核。
    if (!platform::path_is_within_canonical(candidate, proj.dir) &&
        !platform::path_is_within_canonical(candidate, root_)) {
        throw ToolError("File resolves outside the project directory and the MCP root (link): " +
                        candidate);
    }
    if (!IsFile(candidate)) throw ToolError("File not found: " + value);
    return candidate;
}

bool Bridge::IsWithin(const std::string& path, const std::string& root) {
    const std::string target = LowerAscii(normalize_path(path));
    const std::string base = LowerAscii(normalize_path(root));
    if (base.empty()) return false;
    if (target == base) return true;
    if (target.size() <= base.size()) return false;
    if (target.compare(0, base.size(), base) != 0) return false;
    const char separator = target[base.size()];
    return separator == '/' || separator == '\\';
}

std::string Bridge::SafeProjectFile(const std::string& value,
                                    const project::Project& proj) const {
    const std::string path = ResolveFile(value, proj);
    if (!IsWithin(path, proj.dir)) throw ToolError("File is outside the project directory: " + path);
    // B7 / S-04：词法包含挡不住符号链接/junction（工程内一个 junction 就能读出外部文件）。
    // 物理复核：canonical 之后仍必须落在工程目录内。
    if (!platform::path_is_within_canonical(path, proj.dir)) {
        throw ToolError("File resolves outside the project directory (link): " + path);
    }
    return path;
}

std::string Bridge::SafeProjectPath(const std::string& value,
                                    const project::Project& proj) const {
    const std::string trimmed = Trim(value);
    if (trimmed.empty()) throw ToolError("path is required");
    std::string candidate = trimmed;
    if (!platform::is_absolute_path(candidate)) {
        const std::string in_project = normalize_path(path_join(proj.dir, candidate));
        const std::string in_root = normalize_path(path_join(root_, candidate));
        if (PathExists(in_project)) {
            candidate = in_project;
        } else if (PathExists(in_root)) {
            candidate = in_root;
        } else {
            candidate = in_project;
        }
    }
    candidate = normalize_path(candidate);
    if (!IsWithin(candidate, proj.dir)) {
        throw ToolError("Path is outside the project directory: " + candidate);
    }
    // B7 / S-04：同上，补物理复核（junction/symlink 不能把边界搬出工程）。
    if (!platform::path_is_within_canonical(candidate, proj.dir)) {
        throw ToolError("Path resolves outside the project directory (link): " + candidate);
    }
    if (!PathExists(candidate)) throw ToolError("Path not found: " + candidate);
    return candidate;
}

// =============================================================================
//  TD 会话 / 器件
// =============================================================================

Json Bridge::TdSummary() const {
    const std::optional<td::TdChoice> choice = td::FindTd(options_.td_path);
    Json out = Json::object();
    if (!choice.has_value()) {
        out["ok"] = false;
        out["error"] = "No TangDynasty installation found";
        return out;
    }
    out["ok"] = true;
    out["root"] = choice->root;
    out["version"] = choice->version;
    out["source"] = choice->source;
    // 本次是否命中"安装发现缓存"（%TEMP%/fpgalsp/td-install-cache.txt）。
    // 自动发现要扫 C..J 盘，本机约 20 s；缓存命中时这个字段为 true，便于解释
    // "为什么这次 initialize 快了"。source 仍是 auto，契约不变。
    out["cached"] = choice->from_cache;
    Json alternatives = Json::array();
    for (const td::TdInstall& install : choice->alternatives) {
        Json item = Json::object();
        item["root"] = install.root;
        item["version"] = install.version;
        alternatives.push_back(std::move(item));
    }
    out["alternatives"] = std::move(alternatives);
    return out;
}

td::TdSession& Bridge::EnsureSession(const project::Project& project) {
    if (session_) return *session_;
    std::optional<td::TdChoice> choice = td::FindTd(options_.td_path);
    if (!choice.has_value()) throw ToolError("No TangDynasty installation found");
    td_choice_ = *choice;
    td::TdSessionOptions session_options;
    // D-3：显式给 scratch cwd。B7 已把 TdSession 的默认 cwd 改成 %TEMP% 下的独立目录，
    // "不设"也不会污染调用者目录；这里仍显式指向工程，是为了让 TD 会话日志留在
    // `<project>/.fpgalsp/run/`（与 Python 的 `_run_dir()` 同一位置，便于两侧对照排障）。
    session_options.work_dir = path_join(project.dir, ".fpgalsp/run");
    session_ = std::make_unique<td::TdSession>(*td_choice_, session_options);
    return *session_;
}

const td::DeviceInfo& Bridge::EnsureDeviceInfo(const project::Project& proj, bool refresh) {
    const std::string key =
        proj.device + "|" + proj.family + "|" + proj.speed.value_or("") + "|" + root_;
    if (!refresh) {
        const auto cached = devices_.find(key);
        if (cached != devices_.end()) return *cached->second;
    }
    if (proj.device.empty()) throw ToolError("Project has no device in the .al");

    td::TdSession& session = EnsureSession(proj);
    const td::EnsureDeviceResult ensured = td::EnsureDevice(proj, session, td_choice_->root);
    if (!ensured.ok) {
        throw ToolError("No device library supports " + proj.device +
                        (ensured.error.empty() ? std::string() : (": " + ensured.error)));
    }
    // 对应 Python load_device_info 的四次 query_current_package
    const std::string fio = session.Run("query_current_package -fio");
    const std::string clock = session.Run("query_current_package -clock_io");
    const std::string lvds = session.Run("query_current_package -lvds");
    const std::string cfg = session.Run("query_current_package -cfg_io");

    auto info = std::make_unique<td::DeviceInfo>(td::BuildDeviceInfo(fio, clock, lvds, cfg));
    info->device = proj.device;
    info->family = proj.family;
    if (info->db.empty()) info->db = ensured.db;
    if (info->package.empty()) info->package = LowerAscii(proj.device);
    if (info->pins.empty()) {
        throw ToolError("query_current_package -fio returned no pins (device " + proj.device + ")");
    }
    auto& slot = devices_[key];
    slot = std::move(info);
    return *slot;
}

// =============================================================================
//  顶层端口：文本扫描（HDL 索引就绪前的降级）
// =============================================================================

namespace {

// 在文本里找 `module <top>`，返回模块头（到第一个 ';' 或 ')' 结束）的起始偏移与文本
bool FindModuleHeader(const std::string& text, const std::string& top, size_t* header_begin,
                      size_t* header_end) {
    // 两个捕获组 + 两个参数：RE2 的变参只对应捕获组 1..n（见 text_util.h 的说明），
    // 传的参数多于捕获组数量时 FindAndConsumeN 直接返回 false。
    const re2::RE2& pattern = []() -> const re2::RE2& {
        static const re2::RE2 re(R"(\b(module\s+([A-Za-z_][A-Za-z0-9_$]*)))");
        return re;
    }();
    absl::string_view rest(text.data(), text.size());
    re2::StringPiece statement;  // 组 1：`module <name>`（含关键字）
    re2::StringPiece name;       // 组 2：模块名
    while (true) {
        re2::RE2::Arg arg_statement(&statement);
        re2::RE2::Arg arg_name(&name);
        const re2::RE2::Arg* args[2] = {&arg_statement, &arg_name};
        if (!re2::RE2::FindAndConsumeN(&rest, pattern, args, 2)) return false;
        if (statement.size() == 0) return false;
        const std::string found(name.data(), name.size());
        if (found != top) continue;
        const size_t match_end = text.size() - rest.size();
        const size_t match_begin = match_end - statement.size();
        size_t cursor = match_end;
        while (cursor < text.size() && text[cursor] != ';' && text[cursor] != ')') ++cursor;
        *header_begin = match_begin;
        *header_end = std::min(text.size(), cursor + 1);
        return true;
    }
}

}  // namespace

std::vector<ScannedPort> Bridge::ScanTopPorts(const project::Project& proj) const {
    std::vector<ScannedPort> ports;
    if (proj.top.empty()) return ports;

    std::string text;
    size_t header_begin = 0;
    size_t header_end = 0;
    for (const project::AlFile* file : proj.HdlFiles()) {
        const std::string content = ReadTextFile(file->path);
        if (content.empty()) continue;
        if (FindModuleHeader(content, proj.top, &header_begin, &header_end)) {
            text = content;
            break;
        }
    }
    if (text.empty()) return ports;
    return ScanPortHeader(text.substr(header_begin, header_end - header_begin), header_begin);
}
// =============================================================================
//  符号层（阶段二 A 批，11 个工具）
// =============================================================================
//  数据来源分两类，刻意不混用：
//
//  1) **复用 provider::LspServer 的公开 handler**（document_symbols / workspace_symbols /
//     definition / references / hover / project_structure）。
//     这些 handler 已经与 Python 版逐字段对齐过；在 MCP 里重写一遍只会引入第二个偏差源。
//     features 是**无状态纯函数**（index -> LSP JSON），不持有 transport，
//     所以不存在"LSP 通知写进 MCP stdout"的风险 —— 这是选它而不是 LspServer 的原因。
//
//  2) **直接用 hdl::HdlIndex**（module_info / duplicate_modules / search_hierarchy /
//     symbol_path / call_hierarchy）。
//     这 5 个在 C++ 里**没有**现成的 feature API（Python 的 ide.py / nav.py 尚未移植），
//     而 LspServer 也没有对应 handler。这里按 Python 的算法逐行移植；
//     将来它们应该上移到 feature 层（hdl 或新的 ide 模块），MCP 只保留 JSON 转换。
//
//  两种来源的**位置约定不同**，转换必须逐处确认：
//     LspServer / hdl  都输出 0-based (line, character)
//     MCP 输出          1-based (line, column)  —— 与 Python bridge 一致
//   所以凡是把位置交给 agent 的字段，都过 one_based_range()；而 Python 原样透传的字段
//   （duplicate_modules / search_hierarchy 的 line）必须保持 0-based，不能"顺手改成 1-based"。
// =============================================================================

namespace {

// LSP DocumentSymbolKind -> Python mcp_bridge.SYMBOL_KIND 的字符串
const char* SymbolKindName(int kind) {
    switch (kind) {
        case 1: return "file";
        case 2: return "module";
        case 3: return "namespace";
        case 4: return "package";
        case 5: return "class";
        case 6: return "method";
        case 7: return "property";
        case 8: return "field";
        case 9: return "constructor";
        case 10: return "enum";
        case 11: return "interface";
        case 12: return "function";
        case 13: return "variable";
        case 14: return "constant";
        case 15: return "string";
        case 16: return "number";
        case 17: return "boolean";
        case 18: return "array";
        case 19: return "object";
        case 20: return "key";
        case 21: return "null";
        case 22: return "enumMember";
        case 23: return "struct";
        case 24: return "event";
        case 25: return "operator";
        case 26: return "typeParameter";
        default: return "unknown";
    }
}

// 对应 Python _one_based_range：LSP 的 0-based range -> MCP 的 1-based {start,end}
Json OneBasedRange(const Json& range) {
    Json out = Json::object();
    const Json start = range.is_object() && range.contains("start") ? range["start"] : Json::object();
    const Json end = range.is_object() && range.contains("end") ? range["end"] : Json::object();
    Json start_out = Json::object();
    start_out["line"] = (start.is_object() ? start.value("line", 0) : 0) + 1;
    start_out["column"] = (start.is_object() ? start.value("character", 0) : 0) + 1;
    Json end_out = Json::object();
    end_out["line"] = (end.is_object() ? end.value("line", 0) : 0) + 1;
    end_out["column"] = (end.is_object() ? end.value("character", 0) : 0) + 1;
    out["start"] = std::move(start_out);
    out["end"] = std::move(end_out);
    return out;
}

// 对应 Python _file_of：{file, relative}
Json FileOf(const std::string& path, const project::Project& proj) {
    Json out = Json::object();
    out["file"] = path;
    if (!path.empty()) out["relative"] = RelativeTo(path, proj.dir);
    return out;
}

// 对应 Python _location：{file, relative, range(1-based), line, column}
Json LocationJson(const Json& location, const project::Project& proj) {
    if (!location.is_object()) return Json(nullptr);
    const std::string uri = location.value("uri", std::string());
    Json out = FileOf(platform::uri_to_path(uri), proj);
    const Json range = OneBasedRange(location.value("range", Json::object()));
    out["range"] = range;
    out["line"] = range["start"]["line"];
    out["column"] = range["start"]["column"];
    return out;
}

// 对应 Python _symbol：LSP DocumentSymbol -> MCP 形状
Json SymbolJson(const Json& symbol, const std::string& path, const project::Project& proj,
                int depth) {
    if (!symbol.is_object()) return Json(nullptr);
    Json out = Json::object();
    out["name"] = symbol.value("name", std::string());
    const int kind = symbol.value("kind", 0);
    out["kind"] = SymbolKindName(kind);
    out["kindId"] = symbol.contains("kind") ? symbol["kind"] : Json(nullptr);
    out["detail"] = symbol.value("detail", std::string());
    out["range"] = OneBasedRange(symbol.value("range", Json::object()));
    if (symbol.contains("selectionRange") && symbol["selectionRange"].is_object()) {
        out["selectionRange"] = OneBasedRange(symbol["selectionRange"]);
    }
    if (depth < 8 && symbol.contains("children") && symbol["children"].is_array()) {
        Json children = Json::array();
        int count = 0;
        for (const Json& child : symbol["children"]) {
            if (count++ >= 1000) break;
            children.push_back(SymbolJson(child, path, proj, depth + 1));
        }
        out["children"] = std::move(children);
    }
    const Json file = FileOf(path, proj);
    out["file"] = file["file"];
    if (file.contains("relative")) out["relative"] = file["relative"];
    return out;
}

// 对应 Python _hover_text：把 LSP hover 的 contents 归一成一个 markdown 字符串
std::string HoverText(const Json& hover) {
    if (hover.is_null() || !hover.is_object()) return std::string();
    const auto it = hover.find("contents");
    if (it == hover.end()) return std::string();
    if (it->is_string()) return it->get<std::string>();
    if (it->is_object()) return it->value("value", std::string());
    if (it->is_array()) {
        std::string joined;
        for (const Json& item : *it) {
            std::string value;
            if (item.is_string()) {
                value = item.get<std::string>();
            } else if (item.is_object()) {
                value = item.value("value", std::string());
            }
            if (value.empty()) continue;
            if (!joined.empty()) joined += "\n\n";
            joined += value;
        }
        return joined;
    }
    return std::string();
}

// Python ide.module_parents：模块名 -> [(父模块, 实例名)]
std::map<std::string, std::vector<std::pair<const hdl::Module*, std::string>>> ModuleParents(
    const hdl::HdlIndex& index) {
    std::map<std::string, std::vector<std::pair<const hdl::Module*, std::string>>> parents;
    for (const hdl::Instance* instance : index.AllInstances()) {
        const hdl::ParsedFile* file = index.File(instance->file);
        if (file == nullptr) continue;
        const hdl::ModuleInfo* info = hdl::HdlIndex::ModuleAt(*file, instance->line);
        if (info == nullptr) continue;
        parents[instance->module].emplace_back(&info->module, instance->name);
    }
    return parents;
}

// Python ide.modules_in_file 的排序语义：按 line 升序
std::vector<const hdl::Module*> ModulesSortedByName(const hdl::HdlIndex& index) {
    std::vector<const hdl::Module*> modules;
    for (const std::string& path : index.Paths()) {
        const hdl::ParsedFile* file = index.File(path);
        if (file == nullptr) continue;
        for (const hdl::ModuleInfo& info : file->modules) modules.push_back(&info.module);
    }
    std::sort(modules.begin(), modules.end(),
              [](const hdl::Module* a, const hdl::Module* b) { return a->name < b->name; });
    return modules;
}

// call_hierarchy 的"被例化项"输出（Python 的 instances() 内嵌函数）
Json InstanceEntries(const std::vector<const hdl::Instance*>& instances) {
    Json out = Json::array();
    int count = 0;
    for (const hdl::Instance* instance : instances) {
        if (count++ >= 500) break;
        Json item = Json::object();
        item["module"] = instance->module;
        item["instance"] = instance->name;
        item["file"] = instance->file;
        item["line"] = instance->line + 1;
        item["column"] = instance->character + 1;
        out.push_back(std::move(item));
    }
    return out;
}

// Python nav.py：模块的 outgoing（同文件、定义行范围内的例化）/ incoming（module==name）
struct CallHierarchyData {
    const hdl::Module* definition = nullptr;
    std::vector<const hdl::Instance*> outgoing;
    std::vector<const hdl::Instance*> incoming;
};

std::optional<CallHierarchyData> CallHierarchyForModule(const hdl::HdlIndex& index,
                                                        const std::string& module_name) {
    if (module_name.empty()) return std::nullopt;
    const hdl::Module* module = index.ModuleByName(module_name);
    if (module == nullptr) return std::nullopt;
    CallHierarchyData data;
    data.definition = module;
    const long long kUnbounded = 1LL << 30;
    const long long end_line = module->end_line > 0 ? module->end_line : kUnbounded;
    for (const hdl::Instance* instance : index.AllInstances()) {
        if (instance->file == module->file && instance->line >= module->line &&
            static_cast<long long>(instance->line) <= end_line) {
            data.outgoing.push_back(instance);
        }
        if (instance->module == module_name) data.incoming.push_back(instance);
    }
    return data;
}

}  // namespace

// -----------------------------------------------------------------------------
//  LspServer / HDL 索引的惰性装配
// -----------------------------------------------------------------------------

// 说明：符号类的 JSON 形状由 `server::features`（LspServer 与 MCP 共用的无状态层）
// 产出，这里**不需要**构造 LspServer —— 也就不需要担心它的通知写进 MCP 的 stdout。
// 这是比"复用 LspServer handler"更干净的分层：features 不持有状态、不碰传输。
const hdl::HdlIndex& Bridge::EnsureHdlIndex(const project::Project& proj) {
    // D5：交给 server::IndexService（与 LSP 侧同一套增量语义）。
    // 它按 (path, mtime, size) 校验，默认 250 ms 窗口；因此长连接不会再读陈旧索引，
    // 同时也不会退化成"每请求全量重建"。
    if (!index_service_) index_service_ = std::make_unique<server::IndexService>();
    index_service_->SetProject(proj);
    if (auto snapshot = index_service_->Get(); snapshot != nullptr) {
        hdl_snapshot_ = std::move(snapshot);
    } else if (!hdl_snapshot_) {
        // 工程为空 / 一个 HDL 文件都没有：返回一个稳定的空索引。
        hdl_snapshot_ = std::make_shared<hdl::HdlIndex>();
    }
    return *hdl_snapshot_;
}

std::vector<constraints::Port> Bridge::TopPorts(const project::Project& proj,
                                                std::string* source) {
    std::vector<constraints::Port> ports;
    if (source != nullptr) *source = "text-scan";

    // 优先 HDL 索引：它是 server/符号工具同一个数据源，不会与 hover 的端口信息打架
    if (!proj.top.empty()) {
        const hdl::HdlIndex& index = EnsureHdlIndex(proj);
        const hdl::Module* module = index.ModuleByName(proj.top);
        if (module != nullptr && !module->ports.empty()) {
            for (const hdl::Port& scanned : module->ports) {
                constraints::Port port;
                port.name = scanned.name;
                port.direction = scanned.direction;
                port.width = scanned.width;
                port.msb = scanned.msb;
                port.lsb = scanned.lsb;
                port.line = scanned.line;
                port.character = scanned.character;
                port.file = scanned.file;
                ports.push_back(std::move(port));
            }
            if (source != nullptr) *source = "hdl-index";
            return ports;
        }
    }

    // 回落：文本扫描（索引里没有 top 模块 / 端口为空时）
    for (const ScannedPort& scanned : ScanTopPorts(proj)) {
        constraints::Port port;
        port.name = scanned.name;
        port.direction = scanned.direction;
        port.width = scanned.width;
        if (scanned.width > 1) {
            port.msb = scanned.msb;
            port.lsb = scanned.lsb;
        }
        port.line = scanned.line;
        port.character = scanned.character;
        port.file = proj.top;
        ports.push_back(std::move(port));
    }
    return ports;
}

// -----------------------------------------------------------------------------
//  1) project_structure —— server::features::ModuleTree（含环路保护 / 节点预算）
// -----------------------------------------------------------------------------
Json Bridge::ProjectStructure(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);

    bool include_ports = false;
    bool include_primitives = false;
    int max_depth = 24;
    if (const auto it = arguments.find("include_ports");
        it != arguments.end() && it->is_boolean()) {
        include_ports = it->get<bool>();
    }
    if (const auto it = arguments.find("include_primitives");
        it != arguments.end() && it->is_boolean()) {
        include_primitives = it->get<bool>();
    }
    if (const auto it = arguments.find("max_depth");
        it != arguments.end() && it->is_number_integer()) {
        max_depth = std::clamp(it->get<int>(), 1, 64);
    }
    if (arguments.contains("max_nodes")) {
        // server 侧有自己的 TreeBudget；这里只校验取值范围（与 schema 的 1..50000 一致）
        const int max_nodes = arguments.value("max_nodes", 5000);
        if (max_nodes < 1 || max_nodes > 50000) {
            throw ToolError("max_nodes must be between 1 and 50000");
        }
    }

    const hdl::HdlIndex& index = EnsureHdlIndex(proj);
    const Json tree = server::features::ModuleTree(index, proj, include_primitives);
    if (!tree.is_object() || !tree.value("ok", false)) {
        throw ToolError(tree.is_object() ? tree.value("error", std::string("no project hierarchy"))
                                         : std::string("no project hierarchy"));
    }

    // 对应 Python 的 convert()：MCP 用 1-based 位置，字段名保持 moduleTree 的 camelCase
    //
    // B8/PERF-02：这一层原先每个节点要复制 3–4 份中间 Json（range / usedAt 整对象），
    // 且每节点都调一次 RelativeTo —— 后者内部是 fs::relative()，会对两个路径各做一次
    // canonicalize（本机约 70 µs/次），是全工具最贵的一行。这里：
    //   ① range 只取指针，不再 node.value("range") 复制整对象；
    //   ② RelativeTo 按路径**记忆**（同文件的多个节点只算一次）；
    //   ③ usedAt 就地生成，复用记忆结果。
    // 输出的字段与顺序、1-based 换算、空串回落规则全部与旧实现一致。
    std::map<std::string, std::string> relative_cache;
    const auto relative_of = [&](const std::string& path) -> std::string {
        if (path.empty()) return std::string();
        const auto it = relative_cache.find(path);
        if (it != relative_cache.end()) return it->second;
        std::string value = RelativeTo(path, proj.dir);
        relative_cache.emplace(path, value);
        return value;
    };
    const auto location_of = [&](const Json& used) -> Json {
        if (!used.is_object()) return Json(nullptr);
        const std::string path = platform::uri_to_path(used.value("uri", std::string()));
        Json out = Json::object();
        out["file"] = path;
        if (!path.empty()) out["relative"] = relative_of(path);
        const Json* used_range = nullptr;
        if (const auto it = used.find("range"); it != used.end() && it->is_object()) {
            used_range = &(*it);
        }
        Json one_based =
            OneBasedRange(used_range != nullptr ? *used_range : Json::object());
        out["range"] = one_based;
        out["line"] = one_based["start"]["line"];
        out["column"] = one_based["start"]["column"];
        return out;
    };

    std::function<Json(const Json&, int)> convert = [&](const Json& node, int depth) -> Json {
        if (!node.is_object()) return Json(nullptr);
        const Json* node_range = nullptr;
        if (const auto it = node.find("range"); it != node.end() && it->is_object()) {
            node_range = &(*it);
        }
        const Json range = OneBasedRange(node_range != nullptr ? *node_range : Json::object());
        Json out = Json::object();
        out["module"] = node.value("module", std::string());
        out["instance"] = node.value("instance", std::string());
        out["file"] = node.value("file", std::string());
        out["line"] = range["start"]["line"];
        out["column"] = range["start"]["column"];
        out["defined"] = node.value("defined", false);
        out["blackbox"] = node.value("blackbox", false);
        out["recursive"] = node.value("recursive", false);
        out["portCount"] = node.value("portCount", 0);
        if (const auto used = node.find("usedAt"); used != node.end() && used->is_object()) {
            out["usedAt"] = location_of(*used);
        }
        if (include_ports) {
            const hdl::Module* definition = index.ModuleByName(node.value("module", std::string()));
            if (definition != nullptr) {
                Json ports = Json::array();
                int count = 0;
                for (const hdl::Port& port : definition->ports) {
                    if (count++ >= 500) break;
                    Json item = Json::object();
                    item["name"] = port.name;
                    item["direction"] = port.direction;
                    item["width"] = port.width;
                    ports.push_back(std::move(item));
                }
                out["ports"] = std::move(ports);
                Json parameters = Json::array();
                for (const std::string& name : definition->parameters) parameters.push_back(name);
                out["parameters"] = std::move(parameters);
            }
        }
        if (depth < max_depth && node.contains("children") && node["children"].is_array()) {
            Json children = Json::array();
            for (const Json& child : node["children"]) children.push_back(convert(child, depth + 1));
            out["children"] = std::move(children);
        }
        return out;
    };

    Json out = Json::object();
    out["ok"] = true;
    out["project"] = proj.name;
    // 注意：旧写法 tree.value("root", Json(nullptr)) 会**深拷贝整棵树**再转换一遍。
    const Json* root = nullptr;
    if (const auto it = tree.find("root"); it != tree.end()) root = &(*it);
    out["root"] = root != nullptr ? convert(*root, 0) : Json(nullptr);
    out["modulesSeen"] = tree.value("modulesSeen", 0);
    out["truncated"] = tree.value("truncated", false);
    return out;
}

// -----------------------------------------------------------------------------
//  2) document_symbols / workspace_symbols / definition / references / hover
//     —— 全部走 server::features（LspServer 与 MCP 共用的无状态转换层）
//     features 只吃 (index, path, line, character) 并返回 LSP 形状的 JSON，
//     不持有状态、不碰传输，所以这里**不需要**构造 LspServer。
// -----------------------------------------------------------------------------
Json Bridge::DocumentSymbolsTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);
    const std::string path = ResolveFile(arguments.value("file", std::string()), proj);

    const hdl::HdlIndex& index = EnsureHdlIndex(proj);
    const Json result = server::features::DocumentSymbols(index, path);

    Json symbols = Json::array();
    int count = 0;
    for (const Json& symbol : result) {
        if (count++ >= 500) break;
        symbols.push_back(SymbolJson(symbol, path, proj, 0));
    }
    Json out = Json::object();
    out["ok"] = true;
    out["file"] = path;
    out["symbols"] = std::move(symbols);
    return out;
}

Json Bridge::WorkspaceSymbolsTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);
    const std::string query = arguments.value("query", std::string());
    int limit = 200;
    if (const auto it = arguments.find("limit"); it != arguments.end() && it->is_number_integer()) {
        limit = std::clamp(it->get<int>(), 1, 1000);
    }
    // P-B2：分页偏移。裁剪后的 nextOffset 会回填到 arguments.offset 里继续取。
    int offset = 0;
    if (const auto it = arguments.find("offset"); it != arguments.end() && it->is_number_integer()) {
        offset = std::clamp(it->get<int>(), 0, 1000000);
    }

    const hdl::HdlIndex& index = EnsureHdlIndex(proj);
    const Json result = server::features::WorkspaceSymbols(index, query);

    Json symbols = Json::array();
    int skipped = 0;
    int count = 0;
    for (const Json& symbol : result) {
        if (skipped < offset) {
            ++skipped;
            continue;
        }
        // ⚠ 不能写 `if (count++ >= limit) break;`：那会在 break 时也自增，
        // nextOffset 会多 1（分页错位一位）。先判断再自增。
        if (count >= limit) break;
        Json item = Json::object();
        item["name"] = symbol.value("name", std::string());
        item["kind"] = SymbolKindName(symbol.value("kind", 0));
        item["containerName"] = symbol.value("containerName", std::string());
        item["location"] =
            symbol.contains("location") ? LocationJson(symbol["location"], proj) : Json(nullptr);
        symbols.push_back(std::move(item));
        ++count;
    }
    Json out = Json::object();
    out["ok"] = true;
    out["symbols"] = std::move(symbols);
    if (offset > 0 || static_cast<int>(result.size()) > offset + count) {
        out["total"] = result.size();
        out["nextOffset"] = offset + count;
    }
    return out;
}

Json Bridge::DefinitionTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);
    const std::string path = ResolveFile(arguments.value("file", std::string()), proj);
    const int line = std::max(0, arguments.value("line", 1) - 1);
    const int column = std::max(0, arguments.value("column", 1) - 1);

    const hdl::HdlIndex& index = EnsureHdlIndex(proj);
    const Json result = server::features::Definition(index, path, line, column);

    Json locations = Json::array();
    if (result.is_array()) {
        for (const Json& location : result) {
            if (locations.size() >= 200) break;
            locations.push_back(LocationJson(location, proj));
        }
    } else if (result.is_object()) {
        locations.push_back(LocationJson(result, proj));
    }
    Json out = Json::object();
    out["ok"] = true;
    out["locations"] = std::move(locations);
    return out;
}

Json Bridge::ReferencesTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);
    const std::string path = ResolveFile(arguments.value("file", std::string()), proj);
    const int line = std::max(0, arguments.value("line", 1) - 1);
    const int column = std::max(0, arguments.value("column", 1) - 1);
    bool include_declaration = true;
    if (const auto it = arguments.find("include_declaration");
        it != arguments.end() && it->is_boolean()) {
        include_declaration = it->get<bool>();
    }

    const hdl::HdlIndex& index = EnsureHdlIndex(proj);
    const Json result = server::features::References(index, path, line, column, include_declaration);

    Json references = Json::array();
    int count = 0;
    for (const Json& location : result) {
        if (count++ >= 500) break;
        references.push_back(LocationJson(location, proj));
    }
    Json out = Json::object();
    out["ok"] = true;
    out["references"] = std::move(references);
    return out;
}

Json Bridge::HoverTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);
    const std::string path = ResolveFile(arguments.value("file", std::string()), proj);
    const int line = std::max(0, arguments.value("line", 1) - 1);
    const int column = std::max(0, arguments.value("column", 1) - 1);

    const hdl::HdlIndex& index = EnsureHdlIndex(proj);
    const Json result = server::features::Hover(index, path, line, column);

    Json out = Json::object();
    out["ok"] = true;
    out["hover"] = HoverText(result);
    return out;
}
// -----------------------------------------------------------------------------
//  3) module_info / duplicate_modules / search_hierarchy / symbol_path / call_hierarchy
//     —— 直接的 HDL 索引查询（Python ide.py / nav.py 的移植）
// -----------------------------------------------------------------------------
Json Bridge::ModuleInfoTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);
    const std::string module_name = arguments.value("module", std::string());
    if (module_name.empty()) throw ToolError("Missing required argument: module");

    const hdl::HdlIndex& index = EnsureHdlIndex(proj);
    const hdl::Module* module = index.ModuleByName(module_name);
    if (module == nullptr) throw ToolError("Module not found: " + module_name);

    Json ports = Json::array();
    for (const hdl::Port& port : module->ports) {
        Json item = Json::object();
        item["name"] = port.name;
        item["direction"] = port.direction;
        item["net"] = port.net;
        item["msb"] = port.msb.has_value() ? Json(*port.msb) : Json(nullptr);
        item["lsb"] = port.lsb.has_value() ? Json(*port.lsb) : Json(nullptr);
        item["width"] = port.width;
        item["line"] = port.line;
        item["character"] = port.character;
        item["file"] = port.file;
        ports.push_back(std::move(item));
    }
    Json parameters = Json::array();
    for (const std::string& name : module->parameters) parameters.push_back(name);

    Json instances = Json::array();
    for (const hdl::Instance* instance : index.AllInstances()) {
        if (instance->module != module_name) continue;
        if (instances.size() >= 200) break;
        Json item = Json::object();
        item["instance"] = instance->name;
        item["file"] = instance->file;
        item["line"] = instance->line + 1;
        instances.push_back(std::move(item));
    }

    Json out = Json::object();
    out["ok"] = true;
    out["module"] = module->name;
    out["file"] = module->file;
    out["line"] = module->line + 1;
    out["endLine"] = module->end_line + 1;
    out["ports"] = std::move(ports);
    out["parameters"] = std::move(parameters);
    out["instances"] = std::move(instances);
    return out;
}

Json Bridge::DuplicateModulesTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);
    const hdl::HdlIndex& index = EnsureHdlIndex(proj);

    // Python ide.duplicate_modules：按模块名分组，>=2 个定义就算重复。
    // ⚠ 注意 line / end_line 是 **0-based**（Python 原样透传，没有 +1）；不要"顺手"改成 1-based。
    std::map<std::string, std::vector<const hdl::Module*>> groups;
    for (const hdl::Module* module : ModulesSortedByName(index)) {
        groups[module->name].push_back(module);
    }

    Json duplicates = Json::array();
    for (const auto& [name, modules] : groups) {
        if (modules.size() < 2) continue;
        Json definitions = Json::array();
        for (const hdl::Module* module : modules) {
            Json item = Json::object();
            item["file"] = module->file;
            item["line"] = module->line;
            item["end_line"] = module->end_line;
            definitions.push_back(std::move(item));
        }
        Json entry = Json::object();
        entry["name"] = name;
        entry["definitions"] = std::move(definitions);
        duplicates.push_back(std::move(entry));
    }

    Json out = Json::object();
    out["ok"] = true;
    out["duplicates"] = std::move(duplicates);
    return out;
}

Json Bridge::SearchHierarchyTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);
    const std::string query = arguments.value("query", std::string());
    std::string kind = "both";
    if (const auto it = arguments.find("kind"); it != arguments.end() && it->is_string()) {
        kind = LowerAscii(it->get<std::string>());
    }
    int limit = 200;
    if (const auto it = arguments.find("limit"); it != arguments.end() && it->is_number_integer()) {
        limit = std::clamp(it->get<int>(), 1, 2000);
    }
    const std::string needle = LowerAscii(query);
    if (needle.empty()) {
        Json out = Json::object();
        out["ok"] = true;
        out["matches"] = Json::array();
        return out;
    }

    const hdl::HdlIndex& index = EnsureHdlIndex(proj);

    Json matches = Json::array();
    std::set<std::string> seen;
    const auto add = [&](Json entry) {
        const std::string key = entry.dump();
        if (!seen.insert(key).second) return;
        matches.push_back(std::move(entry));
    };

    // 模块名命中
    for (const hdl::Module* module : ModulesSortedByName(index)) {
        if (LowerAscii(module->name).find(needle) == std::string::npos) continue;
        Json item = Json::object();
        item["kind"] = "module";
        item["name"] = module->name;
        item["file"] = module->file;
        item["line"] = module->line;  // 0-based，同 Python
        item["module"] = module->name;
        item["instance"] = "";
        add(std::move(item));
    }
    // 实例名 / 被例化模块名命中
    for (const hdl::Instance* instance : index.AllInstances()) {
        if (LowerAscii(instance->name).find(needle) == std::string::npos &&
            LowerAscii(instance->module).find(needle) == std::string::npos) {
            continue;
        }
        std::string parent;
        if (const hdl::ParsedFile* file = index.File(instance->file)) {
            if (const hdl::ModuleInfo* info = hdl::HdlIndex::ModuleAt(*file, instance->line)) {
                parent = info->module.name;
            }
        }
        Json item = Json::object();
        item["kind"] = "instance";
        item["name"] = instance->name;
        item["file"] = instance->file;
        item["line"] = instance->line;  // 0-based，同 Python
        item["module"] = instance->module;
        item["instance"] = instance->name;
        item["parent"] = parent;
        add(std::move(item));
        if (static_cast<int>(matches.size()) >= limit) break;
    }

    Json trimmed = Json::array();
    int count = 0;
    for (const Json& item : matches) {
        if (count++ >= limit) break;
        if (kind == "module" || kind == "instance") {
            if (item.value("kind", std::string()) != kind) continue;
        }
        trimmed.push_back(item);
    }
    Json out = Json::object();
    out["ok"] = true;
    out["matches"] = std::move(trimmed);
    return out;
}

Json Bridge::SymbolPathTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);
    const std::string path = ResolveFile(arguments.value("file", std::string()), proj);
    const int line = std::max(0, arguments.value("line", 1) - 1);
    const int character = std::max(0, arguments.value("column", 1) - 1);

    const hdl::HdlIndex& index = EnsureHdlIndex(proj);

    std::string name;
    if (const std::optional<hdl::SymbolAtResult> symbol = index.SymbolAt(path, line, character)) {
        name = symbol->name;
    }
    if (name.empty()) {
        // 对应 Python refs.token_span 的兜底：光标处 token 的文本
        if (const hdl::ParsedFile* file = index.File(path)) {
            if (const hdl::Token* token = hdl::HdlIndex::TokenAt(*file, line, character)) {
                name = token->text;
            }
        }
    }
    if (name.empty()) {
        Json out = Json::object();
        out["ok"] = false;
        out["error"] = "No copyable symbol under the cursor";
        return out;
    }

    const hdl::ParsedFile* file = index.File(path);
    const hdl::ModuleInfo* module =
        file != nullptr ? hdl::HdlIndex::ModuleAt(*file, line) : nullptr;
    if (module == nullptr) {
        Json out = Json::object();
        out["ok"] = true;
        out["name"] = name;
        out["path"] = name;
        out["instancePath"] = "";
        out["module"] = "";
        return out;
    }

    const auto parents = ModuleParents(index);
    std::vector<std::string> chain;
    const hdl::ModuleInfo* current = module;
    std::set<std::string> visited;
    while (current != nullptr && !proj.top.empty() && current->module.name != proj.top) {
        const std::string current_name = current->module.name;
        if (!visited.insert(current_name).second) break;
        const auto it = parents.find(current_name);
        if (it == parents.end()) break;
        const std::pair<const hdl::Module*, std::string>* chosen = nullptr;
        for (const auto& candidate : it->second) {
            if (candidate.first->name != current_name && !candidate.first->name.empty()) {
                chosen = &candidate;
                break;
            }
        }
        if (chosen == nullptr) break;
        chain.insert(chain.begin(), chosen->second);
        // 继续向上：父模块的 ModuleInfo 由索引按 (文件, 行) 反查
        const hdl::Module* parent_module = chosen->first;
        const hdl::ParsedFile* parent_file = index.File(parent_module->file);
        if (parent_file == nullptr) {
            current = nullptr;
            break;
        }
        const hdl::ModuleInfo* parent_info =
            hdl::HdlIndex::ModuleAt(*parent_file, parent_module->line);
        if (parent_info == nullptr || parent_info->module.name == current_name) {
            current = nullptr;
            break;
        }
        current = parent_info;
    }

    std::string instance_path;
    for (const std::string& segment : chain) {
        if (!instance_path.empty()) instance_path += "/";
        instance_path += segment;
    }
    std::string full_path = instance_path.empty() ? name : instance_path + "/" + name;

    // 与 server::ide::SymbolPath 保持一致：mark_debug / ChipWatcher 要的是**综合后网表**
    // 的层次名，所以必须带顶层模块名前缀，并把位宽写成 `[msb:lsb]`（清单侧会自动展开）。
    std::optional<hdl::SymbolAtResult> symbol_at = index.SymbolAt(path, line, character);
    std::string chip_watcher_path;
    if (!proj.top.empty()) chip_watcher_path = proj.top + "/";
    chip_watcher_path += full_path;
    if (symbol_at.has_value() && symbol_at->width.has_value() && *symbol_at->width > 1 &&
        symbol_at->msb.has_value() && symbol_at->lsb.has_value()) {
        chip_watcher_path +=
            "[" + std::to_string(*symbol_at->msb) + ":" + std::to_string(*symbol_at->lsb) + "]";
    }

    Json out = Json::object();
    out["ok"] = true;
    out["name"] = name;
    out["path"] = full_path;
    out["chipWatcherPath"] = chip_watcher_path;
    out["instancePath"] = instance_path;
    out["module"] = module->module.name;
    return out;
}

Json Bridge::CallHierarchyTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);
    std::string direction = "both";
    if (const auto it = arguments.find("direction"); it != arguments.end() && it->is_string()) {
        direction = LowerAscii(it->get<std::string>());
    }

    const hdl::HdlIndex& index = EnsureHdlIndex(proj);

    std::string module_name = arguments.value("module", std::string());
    if (module_name.empty()) {
        // 没有 module 参数 -> 用光标位置解析（Python nav.call_hierarchy）
        const std::string file_value = arguments.value("file", std::string());
        if (file_value.empty()) throw ToolError("file/line/column or module is required");
        const std::string path = ResolveFile(file_value, proj);
        const int line = std::max(0, arguments.value("line", 1) - 1);
        const int character = std::max(0, arguments.value("column", 1) - 1);
        if (const std::optional<hdl::SymbolAtResult> symbol = index.SymbolAt(path, line, character)) {
            if (symbol->kind == "module") {
                module_name = symbol->name;
            } else if (symbol->kind == "instance") {
                module_name = symbol->module;
            }
        }
    }

    const std::optional<CallHierarchyData> found =
        module_name.empty() ? std::nullopt : CallHierarchyForModule(index, module_name);
    if (!found.has_value()) {
        Json out = Json::object();
        out["ok"] = true;
        out["module"] = "";
        out["incoming"] = Json::array();
        out["outgoing"] = Json::array();
        return out;
    }

    const hdl::Module* definition = found->definition;
    Json definition_json = Json(nullptr);
    if (definition != nullptr) {
        Json location = FileOf(definition->file, proj);
        Json range = Json::object();
        Json start = Json::object();
        start["line"] = definition->line + 1;
        start["column"] = definition->character + 1;
        range["start"] = start;
        range["end"] = start;
        location["range"] = std::move(range);
        location["line"] = definition->line + 1;
        location["column"] = definition->character + 1;
        definition_json = std::move(location);
    }

    Json out = Json::object();
    out["ok"] = true;
    out["module"] = module_name;
    out["definition"] = std::move(definition_json);
    out["incoming"] = (direction == "both" || direction == "incoming")
                          ? InstanceEntries(found->incoming)
                          : Json::array();
    out["outgoing"] = (direction == "both" || direction == "outgoing")
                          ? InstanceEntries(found->outgoing)
                          : Json::array();
    return out;
}

// =============================================================================
//  fpga_guide
// =============================================================================
//  唯一一个不碰工程、不碰 TD、不碰硬件的"工具"：按 topic 返回一段固定 Markdown。
//  正文在 guide.cpp（每个 topic ≤3 KB），这里只做参数归一化与响应整形。
//  刻意**不**走 ProjectFor()：没有工程也应该能读帮助（新手上来的第一步）。
namespace {

Json GuideTool(const Json& arguments) {
    std::string requested = "overview";
    if (const auto it = arguments.find("topic"); it != arguments.end() && it->is_string()) {
        requested = it->get<std::string>();
    }
    // 未知/空 topic 回落到 overview；回填的是**归一化之后**的值，避免 agent 以为
    // 自己拿到的是某个不存在的 topic 的正文。
    const std::string_view topic = ResolveGuideTopic(requested);
    Json out = Json::object();
    out["ok"] = true;
    out["topic"] = topic;
    out["text"] = GuideText(topic);
    return out;
}

}  // namespace

// =============================================================================
//  工具分派
// =============================================================================

Json Bridge::Call(std::string_view handler, const Json& arguments) {
    const std::string name(handler);
    // 帮助排在最前：agent 拿不准顺序时，这一条应该是它第一个看到的实现分支。
    if (name == "guide") return GuideTool(arguments);
    if (name == "project_list") return ProjectList(arguments);
    if (name == "project_info") return ProjectInfo(arguments);
    if (name == "project_files") return ProjectFiles(arguments);
    if (name == "project_reports") return ProjectReports(arguments);
    if (name == "diagnostics") return DiagnosticsTool(arguments);
    if (name == "device_info") return DeviceInfoTool(arguments);
    if (name == "constraints_status") return ConstraintsStatus(arguments);
    if (name == "constraint_audit") return ConstraintAudit(arguments);
    if (name == "td_info") return TdInfoTool(arguments);
    if (name == "read_file") return ReadFileTool(arguments);
    if (name == "search_text") return SearchTextTool(arguments);
    if (name == "flow_status") return FlowStatus(arguments);
    if (name == "flow_log") return FlowLog(arguments);
    if (name == "flow_issues") return FlowIssues(arguments);
    if (name == "flow_wait") return FlowWait(arguments);
    if (name == "flow_run") return FlowRunTool(arguments);
    if (name == "flow_cancel") return FlowCancelTool(arguments);
    // P-B5：job 是 runId 的稳定别名（flow_run 启动异步线程后立刻返回 runId）
    if (name == "testbench") {
        // B-B10：与 LSP 的 fpgalsp.testbench 同一实现
        if (options_.shared_server != nullptr) {
            return options_.shared_server->FeatureTestbench(arguments);
        }
        const project::Project& project = ProjectFor(arguments);
        const hdl::HdlIndex& index = EnsureHdlIndex(project);
        return server::features::TestbenchSkeleton(index, project,
                                                   arguments.value("module", std::string()),
                                                   arguments.value("clock", std::string()),
                                                   arguments.value("reset", std::string()));
    }
    if (name == "run_diff") {
        // B-B6：两次 run 的 QoR/时序/面积差异（数据来自 <project>_Runs/*）
        const project::Project& project = ProjectFor(arguments);
        return reports::CompareRuns(project, arguments.value("from", std::string()),
                                    arguments.value("to", std::string()));
    }
    if (name == "job_status") return JobStatusTool(arguments);
    if (name == "job_cancel") return JobCancelTool(arguments);
    if (name == "strategy_compare") return StrategyCompareTool(arguments);
    if (name == "cables") return CablesTool(arguments);
    if (name == "probe_status") return ProbeStatusTool(arguments);
    if (name == "probe_config") return ProbeConfigTool(arguments);
    if (name == "probe_verify") return ProbeVerifyTool(arguments);
    if (name == "probe_capture") return ProbeCaptureTool(arguments);
    if (name == "completion") return CompletionTool(arguments);
    if (name == "instantiation") return InstantiationTool(arguments);
    if (name == "project_rename") return ProjectRenameTool(arguments);
    if (name == "project_add_source") return ProjectAddSourceTool(arguments);
    // ---- 阶段二 A 批：符号类 ----
    if (name == "project_structure") return ProjectStructure(arguments);
    if (name == "document_symbols") return DocumentSymbolsTool(arguments);
    if (name == "workspace_symbols") return WorkspaceSymbolsTool(arguments);
    if (name == "search_hierarchy") return SearchHierarchyTool(arguments);
    if (name == "definition") return DefinitionTool(arguments);
    if (name == "references") return ReferencesTool(arguments);
    if (name == "hover") return HoverTool(arguments);
    if (name == "symbol_path") return SymbolPathTool(arguments);
    if (name == "call_hierarchy") return CallHierarchyTool(arguments);
    if (name == "module_info") return ModuleInfoTool(arguments);
    if (name == "duplicate_modules") return DuplicateModulesTool(arguments);

    // ---- B6c：代码动作 / 重命名编辑（与 LSP 同一条 Feature 实现）----
    if (name == "code_actions") return CodeActionsTool(arguments);
    if (name == "rename_edits") return RenameEditsTool(arguments);
    if (name == "format") return FormatTool(arguments);
    if (name == "analysis_run") return AnalysisRunTool(arguments);
    if (name == "project_update") return ProjectUpdateTool(arguments);

    throw ToolError("Tool implementation missing: " + name);
}

// =============================================================================
//  工程类工具
// =============================================================================

// P-B6：resource 目录。全部是只读聚合视图，读起来走 ReadResource。
Json Bridge::ListResources() {
    Json out = Json::array();
    const auto add = [&out](const char* uri, const char* name, const char* description) {
        Json item = Json::object();
        item["uri"] = uri;
        item["name"] = name;
        item["description"] = description;
        item["mimeType"] = "application/json";
        out.push_back(std::move(item));
    };
    add("fpgalsp://project/info", "Project info", "Selected project: device, top, file counts, TD summary.");
    add("fpgalsp://project/files", "Project files", "HDL/ADC/SDC/IP file list with existence flags.");
    add("fpgalsp://project/reports/summary", "Report summary", "QoR/timing/area/flow summary (no raw text).");
    add("fpgalsp://project/flow", "Flow status", "Current flow state, last message and run id.");
    return out;
}

std::optional<std::string> Bridge::ReadResource(std::string_view uri) {
    Json payload;
    if (uri == "fpgalsp://project/info") {
        payload = ProjectInfo(Json::object());
    } else if (uri == "fpgalsp://project/files") {
        payload = ProjectFiles(Json::object());
    } else if (uri == "fpgalsp://project/reports/summary") {
        Json args = Json::object();
        args["section"] = "summary";
        args["limit"] = 200;
        payload = ProjectReports(args);
    } else if (uri == "fpgalsp://project/flow") {
        payload = FlowStatus(Json::object());
    } else {
        return std::nullopt;
    }
    return payload.dump(1, ' ');
}

Json Bridge::ProjectList(const Json& arguments) {
    std::string base = root_;
    if (const auto it = arguments.find("root"); it != arguments.end() && it->is_string() &&
                                                     !it->get<std::string>().empty()) {
        // B7 / S-03：扫描根同样只允许 root_ 之内（含 root_ 本身）
        const std::string requested = normalize_path(it->get<std::string>());
        if (!platform::path_is_within_canonical(requested, root_)) {
            throw ToolError("root is outside the MCP root (" + root_ + "): " + requested);
        }
        base = requested;
    }
    // F4 / R7：走缓存（键 = base + 目录/.al stamp；失效条件见 CachedProjectListEntries）。
    // 返回内容与改动前逐字节一致：缓存里存的就是原来那个 projects 数组。
    Json projects = CachedProjectListEntries(base);
    Json out = Json::object();
    out["ok"] = true;
    out["root"] = base;
    out["projects"] = std::move(projects);
    out["selected"] = active_al_;
    return out;
}

Json Bridge::ProjectInfo(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);

    Json runs = Json::array();
    const std::string runs_dir = path_join(proj.dir, proj.name + "_Runs");
    if (IsDirectory(runs_dir)) {
        for (const std::string& name : ListSubdirectories(runs_dir)) {
            if (name == "best_result") continue;
            runs.push_back(name);
        }
    }

    Json files = Json::object();
    files["hdl"] = static_cast<int>(proj.HdlFiles().size());
    files["verilog"] = static_cast<int>(proj.VerilogFiles().size());
    files["vhdl"] = static_cast<int>(proj.VhdlFiles().size());
    files["adc"] = static_cast<int>(proj.AdcFiles().size());
    files["sdc"] = static_cast<int>(proj.SdcFiles().size());
    files["ip"] = static_cast<int>(proj.IpFiles().size());
    files["excluded"] = static_cast<int>(proj.NotInBuild().size());

    Json warnings = Json::array();
    for (const std::string& warning : proj.warnings) warnings.push_back(warning);

    bool device_cached = false;
    for (const auto& [key, value] : devices_) {
        if (value != nullptr && value->device == proj.device) device_cached = true;
    }

    Json out = Json::object();
    out["ok"] = true;
    out["project"] = proj.name;
    out["al"] = proj.al_path;
    out["dir"] = proj.dir;
    out["family"] = proj.family;
    out["device"] = proj.device;
    // Python: project.speed 为 None 时输出 JSON null（<Speed></Speed> -> null）
    out["speed"] = proj.speed.has_value() ? Json(*proj.speed) : Json(nullptr);
    out["top"] = proj.top;
    out["td_version"] = proj.td_version;
    out["files"] = std::move(files);
    out["warnings"] = std::move(warnings);
    out["runs"] = std::move(runs);
    out["deviceCached"] = device_cached;
    out["td"] = TdSummary();
    return out;
}

Json Bridge::ProjectFiles(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);
    std::string kind = "all";
    if (const auto it = arguments.find("kind"); it != arguments.end() && it->is_string()) {
        kind = LowerAscii(it->get<std::string>());
    }
    bool include_excluded = false;
    if (const auto it = arguments.find("include_excluded");
        it != arguments.end() && it->is_boolean()) {
        include_excluded = it->get<bool>();
    }
    // 被排除清单单独给（默认只给前 50 条）：agent 通常只需要"有几条、什么原因"，
    // 全量塞进同一个 files 数组反而看不出哪条进了构建。
    int excluded_limit = 50;
    if (const auto it = arguments.find("limit"); it != arguments.end() && it->is_number_integer()) {
        excluded_limit = std::clamp(it->get<int>(), 1, 2000);
    }

    Json files = Json::array();
    Json excluded = Json::array();
    bool truncated = false;
    bool excluded_truncated = false;
    int in_build = 0;
    int excluded_count = 0;
    int missing_count = 0;
    for (const project::AlFile& file : proj.files) {
        const std::string file_kind(project::FileKindName(file.kind));
        if (kind == "hdl") {
            if (file_kind != "verilog" && file_kind != "vhdl") continue;
        } else if (kind != "all" && file_kind != kind) {
            continue;
        }
        // reason 的优先级 = InBuild() 的短路顺序：先不存在，再没启用，最后才是 .al 里的排除标记。
        // 一条文件同时满足多个条件时只报最"硬"的那个，保证 in_build/excluded/missing 三类互斥、
        // 三个计数相加等于文件总数（agent 可以拿它做一致性检查）。
        std::string reason;
        if (!file.exists) {
            reason = "missing";
            ++missing_count;
        } else if (!file.enabled) {
            reason = "disabled";
            ++excluded_count;
        } else if (file.excluded) {
            reason = "auto-excluded";
            ++excluded_count;
        } else {
            ++in_build;
        }
        if (!file.InBuild()) {
            if (static_cast<int>(excluded.size()) < excluded_limit) {
                Json item = Json::object();
                item["path"] = file.path;
                item["kind"] = file_kind;
                item["reason"] = reason;
                excluded.push_back(std::move(item));
            } else {
                excluded_truncated = true;
            }
        }
        if (!include_excluded && !file.InBuild()) continue;
        if (files.size() >= 2000) {
            truncated = true;
            continue;
        }
        Json item = Json::object();
        item["file"] = file.path;
        item["relative"] = file.rel;
        item["kind"] = file_kind;
        item["compileOrder"] = file.compile_order;
        item["enabled"] = file.enabled;
        item["excluded"] = file.excluded;
        item["exists"] = file.exists;
        item["inBuild"] = file.InBuild();
        item["resolvedVia"] = std::string(project::ResolvedViaName(file.resolved_via));
        files.push_back(std::move(item));
    }

    // counts 覆盖**kind 过滤后**的全部文件（与 excluded 同口径），不受 include_excluded 影响。
    Json counts = Json::object();
    counts["in_build"] = in_build;
    counts["excluded"] = excluded_count;
    counts["missing"] = missing_count;

    Json out = Json::object();
    out["ok"] = true;
    out["project"] = proj.name;
    out["files"] = std::move(files);
    out["truncated"] = truncated;
    out["counts"] = std::move(counts);
    out["excluded"] = std::move(excluded);
    out["excluded_truncated"] = excluded_truncated;
    return out;
}

Json Bridge::ProjectReports(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);

    std::string stage = "all";
    std::string section = "summary";
    int limit = 100;
    bool include_raw = false;
    if (const auto it = arguments.find("stage"); it != arguments.end() && it->is_string()) {
        stage = LowerAscii(it->get<std::string>());
    }
    if (const auto it = arguments.find("section"); it != arguments.end() && it->is_string()) {
        section = LowerAscii(it->get<std::string>());
    }
    if (const auto it = arguments.find("limit"); it != arguments.end() && it->is_number_integer()) {
        limit = it->get<int>();
    }
    if (const auto it = arguments.find("include_raw");
        it != arguments.end() && it->is_boolean()) {
        include_raw = it->get<bool>();
    }
    limit = std::clamp(limit, 1, 1000);

    // P-D1：走 ReportStore（附着模式 = 与 LSP 同一份缓存）。
    // 以前每次调用都 `reports::BuildReports`：冷构建审计实测 768–784 ms，
    // 而第二次调用本该是亚毫秒的命中。
    // 这里仍然用 kAll 掩码（SummarizeReports 需要哪段由 section 决定，掩码收窄留给 P-C3），
    // 变化只在于"构建一次、之后命中"。
    const auto snapshot = Reports().Get(proj, reports::sections::kAll);
    if (snapshot == nullptr) {
        const Json empty = Json::object();
        return SummarizeReports(empty, proj.name, stage, section, limit, include_raw);
    }
    return SummarizeReports(snapshot->data, proj.name, stage, section, limit, include_raw);
}

// =============================================================================
//  诊断
// =============================================================================

namespace {

// 工程里的 ADC/SDC 诊断（对应 Python 的 offline 表里 adc/sdc 两部分）
void CollectConstraintDiagnostics(const project::Project& proj,
                                  const std::vector<constraints::Port>& ports,
                                  std::vector<std::pair<std::string, lsp::Diagnostic>>* out) {
    for (const project::AlFile* file : proj.AdcFiles()) {
        const std::string text = ReadTextFile(file->path);
        const std::vector<constraints::adc::Entry> entries = constraints::adc::Parse(text);
        for (const lsp::Diagnostic& diagnostic :
             constraints::adc::Diagnostics(entries, ports, nullptr, text)) {
            if (diagnostic.code == "BIT-8123") continue;  // 落在顶层模块端口声明行，由 hdl 层发布
            out->emplace_back(file->path, diagnostic);
        }
    }
    for (const project::AlFile* file : proj.SdcFiles()) {
        const std::string text = ReadTextFile(file->path);
        const std::vector<constraints::sdc::Command> commands = constraints::sdc::Parse(text);
        for (const lsp::Diagnostic& diagnostic :
             constraints::sdc::Diagnostics(commands, ports, nullptr)) {
            out->emplace_back(file->path, diagnostic);
        }
    }
}

}  // namespace

Json Bridge::DiagnosticsTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);

    std::vector<std::string> sources;
    bool have_sources = false;
    if (const auto it = arguments.find("sources"); it != arguments.end() && it->is_array()) {
        have_sources = true;
        for (const Json& item : *it) {
            if (item.is_string()) sources.push_back(LowerAscii(item.get<std::string>()));
        }
    }
    std::string file_value;
    if (const auto it = arguments.find("file"); it != arguments.end() && it->is_string()) {
        file_value = it->get<std::string>();
    }
    if (!have_sources) {
        sources = {"hdl", "adc", "sdc"};
        if (!file_value.empty()) sources.push_back("verible");
    }
    int limit = 500;
    if (const auto it = arguments.find("limit"); it != arguments.end() && it->is_number_integer()) {
        limit = it->get<int>();
    }
    limit = std::clamp(limit, 1, 2000);

    const auto want = [&sources](const char* name) {
        return std::find(sources.begin(), sources.end(), name) != sources.end();
    };

    std::string file_path;
    if (!file_value.empty()) file_path = ResolveFile(file_value, proj);

    // 端口表：优先 HDL 索引（portsSource 会如实标注来源）
    std::string ports_source;
    const std::vector<constraints::Port> ports = TopPorts(proj, &ports_source);

    std::vector<std::pair<std::string, lsp::Diagnostic>> raw;
    if (want("adc") || want("sdc")) CollectConstraintDiagnostics(proj, ports, &raw);

    Json diagnostics = Json::array();
    std::set<std::string> seen;
    for (const auto& [path, diagnostic] : raw) {
        if (!file_path.empty() && path != file_path) continue;
        const std::string source = diagnostic.source;
        const bool is_adc = source.find("adc") != std::string::npos;
        const bool is_sdc = source.find("sdc") != std::string::npos;
        if (is_adc && !want("adc")) continue;
        if (is_sdc && !want("sdc")) continue;
        const Json item = DiagnosticJson(diagnostic, path, proj);
        const std::string key = item.dump();
        if (!seen.insert(key).second) continue;
        if (diagnostics.size() < static_cast<size_t>(limit)) diagnostics.push_back(item);
    }

    Json unavailable = Json::object();
    bool td_ok = false;
    // TD 层（可选，需要真实 TD；Python 同）
    if (want("td")) {
        try {
            td::TdSession& session = EnsureSession(proj);
            const lsp::DiagnosticList td_diags = td::RunReadHdl(proj, session);
            // read_hdl 会关闭 stdout 管道（PYTHON-BUGS.md P-6）：会话必须丢弃
            session.Close();
            session_.reset();
            td_ok = true;
            for (const lsp::Diagnostic& diagnostic : td_diags) {
                const std::string path = proj.al_path;
                if (!file_path.empty() && path != file_path) continue;
                if (diagnostics.size() >= static_cast<size_t>(limit)) break;
                diagnostics.push_back(DiagnosticJson(diagnostic, path, proj));
            }
        } catch (const std::exception& error) {
            unavailable["td"] = error.what();
        }
    }
    if (want("hdl")) unavailable["hdl"] = "HDL diagnostics come from the LSP server tier; this MCP build only surfaces adc/sdc/td";
    if (want("verible")) unavailable["verible"] = "Verible lint bridge not ported yet";
    (void)td_ok;

    Json out = Json::object();
    out["ok"] = true;
    out["project"] = proj.name;
    out["file"] = file_path;
    out["diagnostics"] = std::move(diagnostics);
    out["truncated"] = false;
    // D-2：Python 的 diagnostics 只有 {ok, project, file, diagnostics, truncated}。
    // `portsSource` / `unavailable` 是 C++ 早期追加的"数据可信度"字段，Python 没有，
    // 为字段级等价已移除；TD 层的错误改为写日志（不再静默丢失）。
    if (!unavailable.empty()) {
        platform::default_logger().warn(
            std::string("diagnostics: unavailable layers: ") + unavailable.dump());
    }
    return out;
}

// =============================================================================
//  器件 / 约束
// =============================================================================

Json Bridge::DeviceInfoTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);

    bool include_pins = false;
    bool refresh = false;
    std::string pin_filter;
    int limit = 200;
    if (const auto it = arguments.find("include_pins");
        it != arguments.end() && it->is_boolean()) {
        include_pins = it->get<bool>();
    }
    if (const auto it = arguments.find("refresh"); it != arguments.end() && it->is_boolean()) {
        refresh = it->get<bool>();
    }
    if (const auto it = arguments.find("pin_filter"); it != arguments.end() && it->is_string()) {
        pin_filter = LowerAscii(it->get<std::string>());
    }
    if (const auto it = arguments.find("limit"); it != arguments.end() && it->is_number_integer()) {
        limit = it->get<int>();
    }
    limit = std::clamp(limit, 1, 5000);

    const td::DeviceInfo device = EnsureDeviceInfo(proj, refresh);

    Json out = Json::object();
    out["ok"] = true;
    out["device"] = proj.device;
    out["family"] = proj.family;
    out["package"] = device.package;
    out["db"] = device.db;
    out["pinCount"] = static_cast<int>(device.pins.size());

    if (include_pins) {
        Json pins = Json::array();
        // D-1：pin/location 输出**与 Python 一致的大小写**——Python 的 `pin.get("pe")` 是
        // TD 器件表里的规范名 "P81"（td_device 的 `pe`），不是原始小写 `pe_raw`。
        // 过滤仍然大小写不敏感（Python: `needle = pin_filter.lower()` 后比 `name.lower()`）。
        const std::string needle = LowerAscii(pin_filter);
        for (const auto& [key, pin] : device.pins) {
            const std::string name = pin.pe.empty() ? pin.pe_raw : pin.pe;
            const std::string location = pin.location;
            if (!needle.empty() && LowerAscii(name).find(needle) == std::string::npos &&
                LowerAscii(location).find(needle) == std::string::npos) {
                continue;
            }
            Json item = Json::object();
            item["pin"] = name;
            item["location"] = location;
            item["intBank"] = pin.int_bank.has_value() ? Json(*pin.int_bank) : Json(nullptr);
            item["extBank"] = pin.ext_bank.has_value() ? Json(*pin.ext_bank) : Json(nullptr);
            item["clockCapable"] = pin.clock_capable;
            item["lvds"] = pin.lvds;
            pins.push_back(std::move(item));
            if (static_cast<int>(pins.size()) >= limit) break;
        }
        out["pins"] = std::move(pins);
    }
    return out;
}

namespace {

bool Contains(const std::vector<int>& bits, int bit) {
    return std::find(bits.begin(), bits.end(), bit) != bits.end();
}

}  // namespace

Json Bridge::ConstraintsStatus(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);

    std::string ports_source;
    const std::vector<constraints::Port> ports = TopPorts(proj, &ports_source);

    std::vector<constraints::adc::Entry> entries;
    Json adc_files = Json::array();
    for (const project::AlFile* file : proj.AdcFiles()) {
        adc_files.push_back(file->path);
        const std::string text = ReadTextFile(file->path);
        const auto parsed = constraints::adc::Parse(text);
        entries.insert(entries.end(), parsed.begin(), parsed.end());
    }

    const constraints::adc::CoverageReport coverage =
        constraints::adc::Coverage(entries, ports, proj.top);

    Json missing = Json::array();
    for (const constraints::Port& port : ports) {
        const auto covered = coverage.covered.find(port.name);
        const bool has_any = covered != coverage.covered.end();
        if (port.width.value_or(1) <= 1) {
            if (!has_any) missing.push_back(port.name);
            continue;
        }
        const std::vector<int> bits = constraints::PortBits(port);
        for (const int bit : bits) {
            if (!has_any || !Contains(covered->second, bit)) {
                missing.push_back(port.name + "[" + std::to_string(bit) + "]");
            }
        }
    }

    Json sdc = Json::array();
    for (const project::AlFile* file : proj.SdcFiles()) {
        const std::string text = ReadTextFile(file->path);
        Json item = Json::object();
        item["file"] = file->path;
        item["relative"] = file->rel;
        item["createClock"] = CountOccurrences(text, "create_clock");
        item["generatedClock"] = CountOccurrences(text, "create_generated_clock");
        item["derivePll"] = CountOccurrences(text, "derive_pll_clocks");
        sdc.push_back(std::move(item));
    }

    Json warnings = Json::array();
    for (const std::string& warning : proj.warnings) warnings.push_back(warning);

    Json out = Json::object();
    out["ok"] = true;
    out["project"] = proj.name;
    out["top"] = proj.top;
    out["adcFiles"] = std::move(adc_files);
    out["ports"] = static_cast<int>(ports.size());
    out["constrainedPorts"] = static_cast<int>(coverage.covered.size());
    out["missing"] = TrimListTo(missing, 200);
    out["missingCount"] = static_cast<int>(missing.size());
    out["sdc"] = std::move(sdc);
    out["warnings"] = std::move(warnings);
    return out;
}

Json Bridge::ConstraintAudit(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);

    bool include_sdc = true;
    bool include_device = true;
    if (const auto it = arguments.find("include_sdc"); it != arguments.end() && it->is_boolean()) {
        include_sdc = it->get<bool>();
    }
    if (const auto it = arguments.find("include_device");
        it != arguments.end() && it->is_boolean()) {
        include_device = it->get<bool>();
    }

    std::string ports_source;
    const std::vector<constraints::Port> ports = TopPorts(proj, &ports_source);

    std::vector<constraints::adc::Entry> entries;
    Json adc_files = Json::array();
    for (const project::AlFile* file : proj.AdcFiles()) {
        adc_files.push_back(file->path);
        const std::string text = ReadTextFile(file->path);
        const auto parsed = constraints::adc::Parse(text);
        entries.insert(entries.end(), parsed.begin(), parsed.end());
    }
    const constraints::adc::CoverageReport coverage =
        constraints::adc::Coverage(entries, ports, proj.top);

    // 器件：失败不致命（Python 用 device_error 字段报告）
    Json device_json = Json(nullptr);
    std::string device_error;
    bool device_loaded = false;
    if (include_device) {
        try {
            const td::DeviceInfo& device = EnsureDeviceInfo(proj, false);
            device_loaded = true;
            device_json = Json::object();
            device_json["package"] = device.package;
            device_json["db"] = device.db;
            device_json["pinCount"] = static_cast<int>(device.pins.size());
        } catch (const std::exception& error) {
            device_error = error.what();
        }
    }

    // adc::Diagnostics 需要 Json 形状的 device；把 DeviceInfo 转成 Python 的 device dict
    Json device_for_diag = Json(nullptr);
    if (device_loaded) {
        device_for_diag = Json::object();
        Json pins = Json::object();
        for (const auto& [key, pin] : EnsureDeviceInfo(proj, false).pins) {
            Json record = Json::object();
            record["pe"] = LowerAscii(pin.pe_raw.empty() ? pin.pe : pin.pe_raw);
            record["location"] = LowerAscii(pin.location);
            record["type"] = LowerAscii(pin.type);
            if (pin.int_bank.has_value()) record["int_bank"] = *pin.int_bank;
            if (pin.ext_bank.has_value()) record["ext_bank"] = *pin.ext_bank;
            record["clock_capable"] = pin.clock_capable;
            record["lvds"] = pin.lvds;
            pins[key] = std::move(record);
        }
        device_for_diag["pins"] = std::move(pins);
        Json dedicated = Json::array();
        for (const td::DedicatedPin& pin : EnsureDeviceInfo(proj, false).dedicated) {
            Json item = Json::object();
            item["pe"] = LowerAscii(pin.pe);
            item["role"] = pin.role;
            dedicated.push_back(std::move(item));
        }
        device_for_diag["dedicated"] = std::move(dedicated);
    }

    Json issues = Json::array();
    for (const project::AlFile* file : proj.AdcFiles()) {
        const std::string text = ReadTextFile(file->path);
        const auto entries_for_file = constraints::adc::Parse(text);
        const Json* device_ptr = device_for_diag.is_object() ? &device_for_diag : nullptr;
        for (const lsp::Diagnostic& diagnostic :
             constraints::adc::Diagnostics(entries_for_file, ports, device_ptr, text)) {
            Json item = Json::object();
            item["severity"] = SeverityName(diagnostic.severity);
            item["code"] = diagnostic.code.empty() ? "ADC" : diagnostic.code;
            item["message"] = diagnostic.message;
            item["file"] = file->path;
            item["line"] = diagnostic.range.start.line + 1;
            issues.push_back(std::move(item));
        }
    }

    Json sdc_summary = Json::array();
    if (include_sdc) {
        for (const project::AlFile* file : proj.SdcFiles()) {
            const std::string text = ReadTextFile(file->path);
            Json item = Json::object();
            item["file"] = file->path;
            item["createClock"] = CountOccurrences(text, "create_clock");
            item["generatedClock"] = CountOccurrences(text, "create_generated_clock");
            item["derivePll"] = CountOccurrences(text, "derive_pll_clocks");
            sdc_summary.push_back(std::move(item));
        }
    }

    const int total = coverage.total_bits;
    const int constrained = coverage.constrained_bits;
    const double percent = total > 0 ? std::round(constrained * 100.0 / total * 100.0) / 100.0 : 0.0;

    Json missing = Json::object();
    for (const auto& [name, bits] : coverage.missing) missing[name] = bits;
    Json unconstrained = Json::array();
    for (const std::string& name : coverage.unconstrained_ports) unconstrained.push_back(name);
    Json extra = Json::array();
    for (const constraints::adc::Entry& entry : coverage.extra_ports) {
        Json item = Json::object();
        item["port"] = entry.port;
        item["line"] = entry.line + 1;
        extra.push_back(std::move(item));
    }
    Json duplicate_ports = Json::array();
    for (const constraints::adc::DuplicatePort& duplicate : coverage.duplicate_ports) {
        Json item = Json::object();
        item["port"] = duplicate.port;
        item["lines"] = duplicate.lines;
        duplicate_ports.push_back(std::move(item));
    }
    Json duplicate_pins = Json::array();
    for (const constraints::adc::DuplicatePin& duplicate : coverage.duplicate_pins) {
        Json item = Json::object();
        item["pin"] = duplicate.pin;
        item["ports"] = duplicate.ports;
        item["lines"] = duplicate.lines;
        duplicate_pins.push_back(std::move(item));
    }

    Json warnings = Json::array();
    for (const std::string& warning : proj.warnings) warnings.push_back(warning);

    Json out = Json::object();
    out["ok"] = true;
    out["project"] = proj.name;
    out["top"] = proj.top;
    out["adcFiles"] = std::move(adc_files);
    out["totalBits"] = total;
    out["constrainedBits"] = constrained;
    out["coveragePercent"] = percent;
    out["missing"] = std::move(missing);
    out["unconstrainedPorts"] = std::move(unconstrained);
    out["extraPorts"] = std::move(extra);
    out["duplicatePorts"] = std::move(duplicate_ports);
    out["duplicatePins"] = std::move(duplicate_pins);
    out["issues"] = TrimListTo(issues, 500);
    out["issueCount"] = static_cast<int>(issues.size());
    out["deviceLoaded"] = device_loaded;
    out["deviceError"] = device_error;
    out["sdc"] = std::move(sdc_summary);
    out["warnings"] = std::move(warnings);
    return out;
}

// =============================================================================
//  TD / 文件 / 搜索
// =============================================================================

Json Bridge::TdInfoTool(const Json&) {
    Json out = Json::object();
    out["ok"] = true;
    out["td"] = TdSummary();
    return out;
}

Json Bridge::ReadFileTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);

    std::string file_value;
    if (const auto it = arguments.find("file"); it != arguments.end() && it->is_string()) {
        file_value = it->get<std::string>();
    }
    const std::string path = SafeProjectFile(file_value, proj);

    // B7 / S-07：先看大小，别把整个文件读进内存再截断。
    // 16 MiB 远大于任何 HDL/约束文件，也远小于会伤内存的规模。
    constexpr std::uintmax_t kMaxReadBytes = 16u * 1024u * 1024u;
    {
        std::error_code size_ec;
        const auto size = fs::file_size(AsPath(path), size_ec);
        if (!size_ec && size > kMaxReadBytes) {
            Json too_large = Json::object();
            too_large["ok"] = false;
            too_large["file"] = path;
            too_large["error"] = "File is too large to read (" + std::to_string(size) +
                                 " bytes > 16777216)";
            return too_large;
        }
    }

    int offset = 1;
    int limit = 400;
    int max_chars = 200000;
    if (const auto it = arguments.find("offset"); it != arguments.end() && it->is_number_integer()) {
        offset = it->get<int>();
    }
    if (const auto it = arguments.find("limit"); it != arguments.end() && it->is_number_integer()) {
        limit = it->get<int>();
    }
    if (const auto it = arguments.find("max_chars");
        it != arguments.end() && it->is_number_integer()) {
        max_chars = it->get<int>();
    }
    offset = std::max(1, offset);
    limit = std::clamp(limit == 0 ? 400 : limit, 1, 5000);
    max_chars = std::clamp(max_chars == 0 ? 200000 : max_chars, 1000, 1000000);

    const std::string text = ReadTextFile(path);
    const std::vector<std::string> lines = SplitLines(text);
    const int total = static_cast<int>(lines.size());
    const int start = std::min(offset - 1, total);
    const int end = std::min(total, start + limit);
    std::string selected;
    for (int index = start; index < end; ++index) {
        if (index != start) selected.push_back('\n');
        selected += lines[static_cast<size_t>(index)];
    }
    bool truncated = end < total;
    if (static_cast<int>(selected.size()) > max_chars) {
        selected = selected.substr(0, static_cast<size_t>(max_chars));
        truncated = true;
    }

    Json out = Json::object();
    out["ok"] = true;
    out["file"] = path;
    out["relative"] = RelativeTo(path, proj.dir);
    out["lineStart"] = start + 1;
    out["lineEnd"] = end;
    out["totalLines"] = total;
    out["text"] = selected;
    out["truncated"] = truncated;
    return out;
}

Json Bridge::SearchTextTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);

    std::string query;
    if (const auto it = arguments.find("query"); it != arguments.end() && it->is_string()) {
        query = it->get<std::string>();
    }
    if (query.empty()) throw ToolError("query must not be empty");

    std::string path_filter;
    std::string glob;
    bool regex_mode = false;
    bool case_sensitive = false;
    int limit = 100;
    int context = 0;
    if (const auto it = arguments.find("path"); it != arguments.end() && it->is_string()) {
        path_filter = it->get<std::string>();
    }
    if (const auto it = arguments.find("glob"); it != arguments.end() && it->is_string()) {
        glob = it->get<std::string>();
    }
    if (const auto it = arguments.find("regex"); it != arguments.end() && it->is_boolean()) {
        regex_mode = it->get<bool>();
    }
    if (const auto it = arguments.find("case_sensitive");
        it != arguments.end() && it->is_boolean()) {
        case_sensitive = it->get<bool>();
    }
    if (const auto it = arguments.find("limit"); it != arguments.end() && it->is_number_integer()) {
        limit = it->get<int>();
    }
    if (const auto it = arguments.find("context"); it != arguments.end() && it->is_number_integer()) {
        context = it->get<int>();
    }
    limit = std::clamp(limit == 0 ? 100 : limit, 1, 1000);
    context = std::clamp(context, 0, 5);

    // 包一层捕获组：RE2 的变参只对应捕获组（见 text_util.h 的说明），
    // 没有捕获组时 FindAndConsumeN 拿不到匹配文本。
    std::string pattern = std::string("(?") + (case_sensitive ? "" : "i") + ")(" +
                          (regex_mode ? query : re2::RE2::QuoteMeta(query)) + ")";
    const re2::RE2 compiled(pattern);
    if (!compiled.ok()) throw ToolError("Invalid regex: " + query);

    std::vector<std::string> files;
    if (!path_filter.empty()) {
        const std::string target = SafeProjectPath(path_filter, proj);
        if (IsDirectory(target)) {
            std::error_code ec;
            for (fs::recursive_directory_iterator it(fs::u8path(target), ec), end;
                 it != end && !ec; it.increment(ec)) {
                const std::string name = ToUtf8(it->path().filename());
                if (it->path().filename() == ".fpgalsp" || name == "__pycache__") {
                    if (it->is_directory(ec)) it.disable_recursion_pending();
                    continue;
                }
                if (it->is_regular_file(ec)) files.push_back(ToUtf8(it->path()));
            }
        } else {
            files.push_back(target);
        }
    } else {
        for (const project::AlFile* file : proj.HdlFiles()) files.push_back(file->path);
        for (const project::AlFile* file : proj.AdcFiles()) files.push_back(file->path);
        for (const project::AlFile* file : proj.SdcFiles()) files.push_back(file->path);
        files.push_back(proj.al_path);
    }

    const std::vector<std::string> suffixes = {".v",  ".sv",  ".vh",  ".svh", ".vhd",
                                               ".vhdl", ".adc", ".sdc", ".al",  ".tcl"};
    // F4 / R7：先按目录做一次快照（一次 FindFirstFile 拿 size+mtime），
    // 避免每文件 2 次 open 的 stat（本机 200 文件 ≈14 ms，比读文件还贵）。
    std::set<std::string> directories;
    for (const std::string& file_path : files) {
        if (file_path.empty()) continue;
        directories.insert(normalize_path(path_dirname(file_path)));
    }
    std::vector<std::string> directory_list(directories.begin(), directories.end());
    const std::map<std::string, FileInfoLite> file_info = SnapshotDirectories(directory_list);
    Json matches = Json::array();
    int searched = 0;
    bool truncated = false;
    for (const std::string& file_path : files) {
        // P-B4：协作式取消（客户端发 notifications/cancelled 后，长扫描要能收手）
        if (Cancelled()) throw ToolError("cancelled");
        if (!glob.empty() && !GlobMatch(glob, BaseName(file_path))) continue;
        const std::string lowered = LowerAscii(file_path);
        bool suffix_ok = false;
        for (const std::string& suffix : suffixes) {
            if (lowered.size() >= suffix.size() &&
                lowered.compare(lowered.size() - suffix.size(), suffix.size(), suffix) == 0) {
                suffix_ok = true;
                break;
            }
        }
        if (!suffix_ok) continue;
        // F4 / R7：用目录快照里的 (size, mtime) 做缓存校验；快照里没有（枚举失败 /
        // 大小写或 8.3 短名差异 / 刚新建）就回落到**单文件 stat**，保证"跳过或读取"
        // 的判断与改动前完全一致——不为性能牺牲语义。
        const std::string& normalized_path = file_path;  // proj 解析时已规范化（见 al_project）
        FileInfoLite info;
        const auto info_it = file_info.find(normalized_path);
        if (info_it != file_info.end()) {
            info = info_it->second;
        } else if (!StatStamp(normalized_path, &info.mtime_ns, &info.size)) {
            continue;
        }
        if (info.size > 8u * 1024u * 1024u) continue;
        ++searched;
        std::shared_ptr<const std::string> cached_text =
            TextCache::Instance().Get(normalized_path, info.size, info.mtime_ns);
        if (!cached_text) {
            // 读失败：与改动前一致——用空文本继续（该文件记一次 searched，不产生匹配）
            cached_text = std::make_shared<const std::string>(ReadTextFile(file_path));
        }
        const std::string& text = *cached_text;
        // F4 / R7：相对路径**每文件算一次**（原实现是每个匹配算一次，而 RelativeTo 内部
        // 会做两次 canonicalize，本机 ≈70 µs/次；200 个匹配就是 14 ms，占 search_text 大头）。
        const std::string relative_path = FastRelativeTo(file_path, proj.dir);
        const bool has_cr = text.find('\r') != std::string::npos;
        std::vector<std::string> owned_rows;
        std::vector<std::string_view> rows;
        if (has_cr) {
            owned_rows = SplitLines(text);
            rows.reserve(owned_rows.size());
            for (const std::string& row : owned_rows) rows.emplace_back(row);
        } else {
            rows = SplitLineViews(text);
        }
        for (size_t index = 0; index < rows.size(); ++index) {
            std::string_view rest(rows[index]);
            re2::StringPiece found;
            while (true) {
                re2::RE2::Arg arg_found(&found);
                const re2::RE2::Arg* args[1] = {&arg_found};
                if (!re2::RE2::FindAndConsumeN(&rest, compiled, args, 1)) break;
                if (found.size() == 0 && rest.empty()) {
                    // 零长匹配：避免死循环（Python 的 finditer 会自动前进一格）
                }
                const size_t match_end = rows[index].size() - rest.size();
                const size_t match_start = match_end - found.size();
                Json item = Json::object();
                item["file"] = file_path;
                item["relative"] = relative_path;
                item["line"] = static_cast<int>(index) + 1;
                item["column"] = static_cast<int>(match_start) + 1;
                item["text"] = Trim(rows[index].substr(0, 500));
                if (context > 0) {
                    Json snippet = Json::array();
                    const int before = std::max<int>(0, static_cast<int>(index) - context);
                    const int after =
                        std::min<int>(static_cast<int>(rows.size()), static_cast<int>(index) + context + 1);
                    for (int row = before; row < after; ++row) {
                        snippet.push_back(std::string(rows[static_cast<size_t>(row)]));
                    }
                    item["context"] = std::move(snippet);
                }
                matches.push_back(std::move(item));
                if (static_cast<int>(matches.size()) >= limit) {
                    truncated = true;
                    break;
                }
                if (found.size() == 0) break;
            }
            if (static_cast<int>(matches.size()) >= limit) break;
        }
        if (static_cast<int>(matches.size()) >= limit) break;
    }

    Json out = Json::object();
    out["ok"] = true;
    out["query"] = query;
    out["files"] = searched;
    out["matches"] = std::move(matches);
    out["count"] = static_cast<int>(out["matches"].size());
    out["truncated"] = truncated;
    return out;
}

// =============================================================================
//  Flow（C++ 还没有 flow 模块：状态恒为 idle，日志/问题按磁盘文件解析）
// =============================================================================

namespace {

Json IdleFlowState() {
    Json flow = Json::object();
    flow["running"] = false;
    flow["stage"] = "idle";
    flow["elapsed"] = 0.0;
    flow["lastMessage"] = "";
    flow["ok"] = Json(nullptr);
    flow["log"] = Json::array();
    return flow;
}

std::string FlowBase(const project::Project& proj) {
    return path_join(proj.dir, ".fpgalsp/flow");
}

std::vector<std::string> FlowDirs(const project::Project& proj) {
    std::vector<std::string> dirs;
    const std::string base = FlowBase(proj);
    std::error_code ec;
    fs::directory_iterator it(fs::u8path(base), ec);
    if (ec) return dirs;
    std::vector<std::pair<std::string, long long>> found;
    for (const fs::directory_entry& entry : it) {
        const std::string name = ToUtf8(entry.path().filename());
        if (name.rfind("flow_", 0) != 0) continue;
        if (!entry.is_directory(ec)) continue;
        const auto mtime = fs::last_write_time(entry.path(), ec);
        found.emplace_back(ToUtf8(entry.path()),
                           ec ? 0 : static_cast<long long>(mtime.time_since_epoch().count()));
    }
    std::sort(found.begin(), found.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    for (auto& [path, stamp] : found) dirs.push_back(path);
    return dirs;
}

std::string SelectFlowDir(const project::Project& proj, const std::string& run_id) {
    if (!run_id.empty()) {
        const std::string name = Trim(run_id);
        if (name.rfind("flow_", 0) != 0 || BaseName(name) != name) {
            throw ToolError("Invalid run_id: " + run_id);
        }
        const std::string path = path_join(FlowBase(proj), name);
        if (!IsDirectory(path)) throw ToolError("Flow run not found: " + name);
        return path;
    }
    const std::vector<std::string> dirs = FlowDirs(proj);
    return dirs.empty() ? std::string() : dirs.front();
}

std::vector<std::string> RunLogPaths(const project::Project& proj) {
    std::vector<std::string> paths;
    const std::string runs = path_join(proj.dir, proj.name + "_Runs");
    std::error_code ec;
    fs::directory_iterator it(fs::u8path(runs), ec);
    if (!ec) {
        for (const fs::directory_entry& entry : it) {
            if (!entry.is_directory(ec)) continue;
            const std::string log = ToUtf8(entry.path()) + "/run.log";
            if (IsFile(log)) paths.push_back(log);
        }
    }
    return paths;
}

Json TailText(const std::string& path, int max_chars) {
    Json out = Json::object();
    out["path"] = path;
    std::error_code ec;
    const auto size = fs::file_size(fs::u8path(path), ec);
    if (ec) {
        out["error"] = "cannot stat file";
        out["text"] = "";
        out["truncated"] = false;
        out["size"] = 0;
        return out;
    }
    std::string text = ReadTextFile(path);
    const bool truncated = static_cast<int>(text.size()) > max_chars;
    if (truncated) text = text.substr(text.size() - static_cast<size_t>(max_chars));
    out["text"] = text;
    out["truncated"] = truncated;
    out["size"] = static_cast<long long>(size);
    out["lines"] = CountOccurrences(text, "\n") + (text.empty() ? 0 : 1);
    return out;
}

Json TailLines(const std::string& text, int lines) {
    const std::vector<std::string> rows = SplitLines(text);
    const int begin = std::max<int>(0, static_cast<int>(rows.size()) - lines);
    std::string out;
    for (int index = begin; index < static_cast<int>(rows.size()); ++index) {
        if (!out.empty()) out.push_back('\n');
        out += rows[static_cast<size_t>(index)];
    }
    return out;
}

// ---- 顶层端口文本扫描（HDL 索引就绪前的降级实现）----
//
// 支持的头写法：
//    module top( input clk, input [2:0] w, output reg [3:0] q );
//    module top(a, b);            // 非 ANSI 头：只给出方向未知的端口名
// 解析规则（保守，宁可少报 diganostics 也不虚构端口）：
//    · 只在模块头（module ... ; / )）范围内扫描
//    · 方向关键字 input/output/inout 之后可选 wire/reg/logic/signed/unsigned
//    · 可选 [msb:lsb] 位宽
//    · 之后是逗号分隔的一个或多个名字（`input a, b` 与 `input a` 都支持）
//    · 端口名不带位选择；非 ANSI 头里单独出现的名字视为 direction="inout" width=1
std::vector<ScannedPort> ScanPortHeader(std::string_view header, size_t header_offset);


std::vector<ScannedPort> ScanPortHeader(std::string_view raw_header, size_t header_offset) {
    std::vector<ScannedPort> ports;
    const std::string blanked = BlankComments(raw_header);
    const std::string_view header(blanked);

    // 逐字符扫描，遇到方向关键字就解析一小段声明
    const auto is_space = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
    };
    size_t position = 0;
    std::string current_direction;

    while (position < header.size()) {
        // 找下一个标识符
        while (position < header.size() && !IsIdentStart(header[position])) ++position;
        if (position >= header.size()) break;
        const size_t word_begin = position;
        while (position < header.size() && IsIdentChar(header[position])) ++position;
        const std::string word(header.substr(word_begin, position - word_begin));

        if (word == "input" || word == "output" || word == "inout") {
            current_direction = word;
            // 跳过类型修饰与位宽
            int width = 1;
            int msb = 0;
            int lsb = 0;
            size_t cursor = position;
            bool saw_bracket = false;
            while (cursor < header.size()) {
                while (cursor < header.size() && is_space(header[cursor])) ++cursor;
                if (cursor >= header.size()) break;
                if (header[cursor] == '[') {
                    const size_t close = header.find(']', cursor);
                    if (close == std::string_view::npos) break;
                    const std::string spec(header.substr(cursor + 1, close - cursor - 1));
                    const size_t colon = spec.find(':');
                    if (colon != std::string::npos) {
                        try {
                            msb = std::stoi(Trim(spec.substr(0, colon)));
                            lsb = std::stoi(Trim(spec.substr(colon + 1)));
                            width = std::abs(msb - lsb) + 1;
                            saw_bracket = true;
                        } catch (...) {
                            width = 1;
                        }
                    }
                    cursor = close + 1;
                    continue;
                }
                if (IsIdentStart(header[cursor])) {
                    const size_t begin = cursor;
                    while (cursor < header.size() && IsIdentChar(header[cursor])) ++cursor;
                    const std::string qualifier(header.substr(begin, cursor - begin));
                    if (qualifier == "wire" || qualifier == "reg" || qualifier == "logic" ||
                        qualifier == "signed" || qualifier == "unsigned" || qualifier == "var") {
                        continue;
                    }
                    // 到了端口名：先记录，再看后面是否还有逗号分隔的兄弟
                    ScannedPort port;
                    port.name = qualifier;
                    port.direction = current_direction;
                    port.width = saw_bracket ? width : 1;
                    port.msb = saw_bracket ? msb : 0;
                    port.lsb = saw_bracket ? lsb : 0;
                    // 行号：header 内的换行累计
                    const size_t absolute = header_offset + begin;
                    const int line = static_cast<int>(
                        std::count(header.begin(), header.begin() + static_cast<long>(begin), '\n'));
                    const size_t last_newline =
                        header.rfind('\n', begin == 0 ? 0 : begin - 1);
                    port.line = line;
                    port.character =
                        (last_newline == std::string_view::npos)
                            ? static_cast<int>(begin)
                            : static_cast<int>(begin - last_newline - 1);
                    (void)absolute;
                    ports.push_back(std::move(port));

                    // 兄弟名字：`, name` / `, [msb:lsb] name`
                    while (true) {
                        size_t look = cursor;
                        while (look < header.size() && is_space(header[look])) ++look;
                        if (look >= header.size() || header[look] != ',') break;
                        ++look;
                        while (look < header.size() && is_space(header[look])) ++look;
                        if (look < header.size() && header[look] == '[') {
                            const size_t close = header.find(']', look);
                            if (close == std::string_view::npos) break;
                            look = close + 1;
                            while (look < header.size() && is_space(header[look])) ++look;
                        }
                        if (look >= header.size() || !IsIdentStart(header[look])) break;
                        const size_t name_begin = look;
                        while (look < header.size() && IsIdentChar(header[look])) ++look;
                        const std::string name(header.substr(name_begin, look - name_begin));
                        // 逗号后面若是下一个方向的声明，说明这个逗号属于参数列表
                        if (name == "input" || name == "output" || name == "inout") break;
                        ScannedPort sibling;
                        sibling.name = name;
                        sibling.direction = current_direction;
                        sibling.width = saw_bracket ? width : 1;
                        sibling.msb = saw_bracket ? msb : 0;
                        sibling.lsb = saw_bracket ? lsb : 0;
                        sibling.line = static_cast<int>(
                            std::count(header.begin(), header.begin() + static_cast<long>(name_begin), '\n'));
                        const size_t newline = header.rfind('\n', name_begin == 0 ? 0 : name_begin - 1);
                        sibling.character = (newline == std::string_view::npos)
                                                ? static_cast<int>(name_begin)
                                                : static_cast<int>(name_begin - newline - 1);
                        ports.push_back(std::move(sibling));
                        cursor = look;
                    }
                    position = cursor;
                    break;
                }
                // 别的 token（比如 '='）-> 放弃这条声明
                position = cursor;
                current_direction.clear();
                break;
            }
            continue;
        }
    }
    return ports;
}

// 对应 Python _flow_issue_from_line：只认带 ERROR/WARNING/CRITICAL-WARNING 的行
std::optional<Json> FlowIssueFromLine(const std::string& line) {
    const std::string text = Trim(line);
    if (text.empty() || text.front() == '=') return std::nullopt;

    static const re2::RE2 severity_re(R"((?i)\b(CRITICAL-WARNING|WARNING|ERROR)\b)");
    static const re2::RE2 code_re(R"(\b([A-Z][A-Z0-9]{1,5}-[0-9]{3,5})\b)");
    static const re2::RE2 file_re(
        R"(([A-Za-z]:[\\/][^()\n]+?|[A-Za-z_][A-Za-z0-9_./\\-]*\.(?:v|sv|vh|svh|vhd|vhdl|adc|sdc|al))\(([0-9]+)\))");

    std::string severity;
    if (!re2::RE2::PartialMatch(text, severity_re, &severity)) return std::nullopt;
    for (char& c : severity) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    std::string code;
    if (!re2::RE2::PartialMatch(text, code_re, &code)) code = "FLOW";
    std::string file;
    std::string line_text;
    int line_number = 0;
    if (re2::RE2::PartialMatch(text, file_re, &file, &line_text)) {
        try {
            line_number = std::stoi(line_text);
        } catch (...) {
            line_number = 0;
        }
    }

    Json out = Json::object();
    out["severity"] = (severity == "ERROR") ? "error" : "warning";
    out["code"] = code;
    out["message"] = text.substr(0, 1200);
    out["file"] = file;
    out["line"] = line_number;
    return out;
}

// 对应 Python mcp_bridge._report_summary
Json TrimList(const Json& value, int limit) {
    Json out = Json::array();
    if (!value.is_array()) return out;
    int count = 0;
    for (const Json& item : value) {
        if (count++ >= limit) break;
        out.push_back(item);
    }
    return out;
}

const Json& Member(const Json& object, const char* key) {
    static const Json kNull = Json(nullptr);
    if (object.is_object()) {
        const auto it = object.find(key);
        if (it != object.end()) return *it;
    }
    return kNull;
}

Json SummarizeReports(const Json& data, const std::string& project_name, std::string stage,
                      std::string section, int limit, bool include_raw) {
    const Json* source = &data;
    if ((stage == "syn" || stage == "phy") && data.contains(stage) && data[stage].is_object()) {
        source = &data[stage];
    }

    Json result = Json::object();
    result["ok"] = data.value("ok", false);
    result["project"] = project_name;
    result["stage"] = stage;
    result["runs"] = Member(Member(data, "artifacts"), "runs");
    // Python: `source.get("files") or data.get("files") or {}` -> 缺失时是空对象，不是 null
    const Json& files_source = Member(*source, "files").is_object() ? Member(*source, "files")
                                                                    : Member(data, "files");
    result["files"] = files_source.is_object() ? files_source : Json::object();
    if (!data.value("ok", false)) {
        const Json& error = Member(data, "error");
        result["error"] = error.is_string() ? error : Json("No reports available");
        return result;
    }

    const Json& qor = Member(*source, "qor").is_object() ? Member(*source, "qor") : Member(data, "qor");
    const Json& timing = Member(*source, "timing").is_object() ? Member(*source, "timing")
                                                              : Member(data, "timing");
    const Json& area = Member(*source, "area").is_object() ? Member(*source, "area")
                                                           : Member(data, "area");
    const Json& flow = Member(*source, "flowStatus").is_object() ? Member(*source, "flowStatus")
                                                                 : Member(data, "flowStatus");
    const Json& analysis = Member(data, "analysis");

    if (section == "summary" || section == "all") {
        Json summary = Json::object();
        summary["qorStep"] = Member(qor, "step").is_string() ? Member(qor, "step") : Json("");
        summary["metrics"] = TrimList(Member(qor, "metrics"), limit);
        summary["suggestions"] = TrimList(Member(qor, "suggestions"), limit);
        summary["timing"] = Member(timing, "summary").is_object() ? Member(timing, "summary")
                                                                  : Json::object();
        summary["coverage"] = Member(timing, "coverage");
        summary["areaRows"] = TrimList(Member(area, "rows"), limit);
        summary["flowSteps"] = TrimList(Member(flow, "steps"), limit);
        result["summary"] = std::move(summary);

        const Json& drc = Member(analysis, "drc");
        const Json& cdc = Member(analysis, "cdc");
        Json drc_cdc = Json::object();
        drc_cdc["drcViolations"] = Member(drc, "violations");
        drc_cdc["drcClean"] = Member(drc, "clean");
        drc_cdc["drcRules"] = TrimList(Member(drc, "rules"), limit);
        drc_cdc["cdcCounts"] = Member(cdc, "counts").is_object() ? Member(cdc, "counts")
                                                                 : Json::object();
        result["drcCdc"] = std::move(drc_cdc);
    }
    if (section == "qor" || section == "all") {
        Json qor_out = Json::object();
        qor_out["step"] = Member(qor, "step").is_string() ? Member(qor, "step") : Json("");
        qor_out["metrics"] = TrimList(Member(qor, "metrics"), limit);
        qor_out["suggestions"] = TrimList(Member(qor, "suggestions"), limit);
        result["qor"] = std::move(qor_out);
    }
    if (section == "timing" || section == "all") {
        Json timing_out = Json::object();
        if (timing.is_object()) {
            for (auto it = timing.begin(); it != timing.end(); ++it) {
                if (it.key() == "raw" || it.key() == "paths" || it.key() == "groups" ||
                    it.key() == "clock_detail") {
                    continue;
                }
                timing_out[it.key()] = it.value();
            }
        }
        timing_out["paths"] = TrimList(Member(timing, "paths"), limit);
        timing_out["groups"] = TrimList(Member(timing, "groups"), std::max(1, limit / 10));
        result["timing"] = std::move(timing_out);
    }
    if (section == "area" || section == "all") {
        Json area_out = Json::object();
        area_out["rows"] = TrimList(Member(area, "rows"), limit);
        area_out["counts"] = Member(area, "counts").is_object() ? Member(area, "counts")
                                                                : Json::object();
        area_out["modules"] = TrimList(Member(area, "modules"), limit);
        result["area"] = std::move(area_out);
    }
    if (section == "flow" || section == "all") result["flow"] = flow;
    if (include_raw) {
        result["raw"] = Member(*source, "raw");
        result["rawFiles"] = Member(*source, "files");
    }
    return result;
}
}  // namespace

Json Bridge::FlowLog(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);

    std::string run_id;
    std::string kind = "both";
    int tail = 200;
    int max_chars = 200000;
    if (const auto it = arguments.find("run_id"); it != arguments.end() && it->is_string()) {
        run_id = it->get<std::string>();
    }
    if (const auto it = arguments.find("kind"); it != arguments.end() && it->is_string()) {
        kind = LowerAscii(it->get<std::string>());
    }
    if (const auto it = arguments.find("tail"); it != arguments.end() && it->is_number_integer()) {
        tail = it->get<int>();
    }
    if (const auto it = arguments.find("max_chars");
        it != arguments.end() && it->is_number_integer()) {
        max_chars = it->get<int>();
    }
    if (kind != "td" && kind != "run" && kind != "both") {
        throw ToolError("kind must be one of td/run/both");
    }
    tail = std::clamp(tail == 0 ? 200 : tail, 1, 5000);
    max_chars = std::clamp(max_chars == 0 ? 200000 : max_chars, 1000, 1000000);

    const std::string run_dir = SelectFlowDir(proj, run_id);
    if (run_dir.empty()) {
        Json out = Json::object();
        out["ok"] = false;
        out["error"] = "No flow run directory found";
        out["files"] = Json::array();
        return out;
    }

    Json files = Json::array();
    if (kind == "td" || kind == "both") {
        for (const std::string& name : ListFiles(run_dir)) {
            if (name.rfind("td_", 0) != 0) continue;
            if (!(name.size() >= 4 &&
                  (name.compare(name.size() - 4, 4, ".log") == 0 ||
                   name.compare(name.size() - 5, 5, ".logw") == 0))) {
                continue;
            }
            Json data = TailText(path_join(run_dir, name), max_chars);
            data["text"] = TailLines(data["text"].get<std::string>(), tail);
            data["kind"] = "td";
            files.push_back(std::move(data));
        }
    }
    if (kind == "run" || kind == "both") {
        for (const std::string& path : RunLogPaths(proj)) {
            Json data = TailText(path, max_chars);
            data["text"] = TailLines(data["text"].get<std::string>(), tail);
            data["kind"] = "run";
            files.push_back(std::move(data));
        }
    }

    Json out = Json::object();
    out["ok"] = true;
    out["runId"] = BaseName(run_dir);
    out["runDir"] = run_dir;
    out["files"] = std::move(files);
    out["count"] = static_cast<int>(out["files"].size());
    return out;
}

Json Bridge::FlowIssues(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);

    std::string run_id;
    int tail = 2000;
    int limit = 500;
    if (const auto it = arguments.find("run_id"); it != arguments.end() && it->is_string()) {
        run_id = it->get<std::string>();
    }
    if (const auto it = arguments.find("tail"); it != arguments.end() && it->is_number_integer()) {
        tail = it->get<int>();
    }
    if (const auto it = arguments.find("limit"); it != arguments.end() && it->is_number_integer()) {
        limit = it->get<int>();
    }
    tail = std::clamp(tail == 0 ? 2000 : tail, 1, 10000);
    limit = std::clamp(limit == 0 ? 500 : limit, 1, 2000);

    const std::string run_dir = SelectFlowDir(proj, run_id);

    Json issues = Json::array();
    std::set<std::string> seen;
    const auto add_issue = [&](const Json& issue) {
        if (!issue.is_object()) return;
        const std::string key = issue.dump();
        if (!seen.insert(key).second) return;
        if (static_cast<int>(issues.size()) < limit) issues.push_back(issue);
    };

    std::vector<std::string> logs;
    Json markers = Json::array();
    if (!run_dir.empty()) {
        for (const std::string& name : ListFiles(run_dir)) {
            if (name.size() >= 8 && name.compare(name.size() - 8, 8, ".error.f") == 0) {
                markers.push_back(name);
            }
        }
        for (const std::string& name : ListFiles(run_dir)) {
            if (name.rfind("td_", 0) != 0) continue;
            const bool is_log = name.size() >= 4 &&
                                (name.compare(name.size() - 4, 4, ".log") == 0 ||
                                 name.compare(name.size() - 5, 5, ".logw") == 0);
            if (!is_log) continue;
            const Json data = TailText(path_join(run_dir, name), 500000);
            logs.push_back(path_join(run_dir, name));
            for (const std::string& row : SplitLines(TailLines(data["text"].get<std::string>(), tail))) {
                if (auto issue = FlowIssueFromLine(row)) add_issue(*issue);
            }
        }
    }
    for (const std::string& path : RunLogPaths(proj)) {
        const Json data = TailText(path, 500000);
        logs.push_back(path);
        for (const std::string& row : SplitLines(TailLines(data["text"].get<std::string>(), tail))) {
            if (auto issue = FlowIssueFromLine(row)) add_issue(*issue);
        }
    }
    for (const Json& marker : markers) {
        const std::string name = marker.get<std::string>();
        Json issue = Json::object();
        issue["severity"] = "error";
        issue["code"] = "FLOW-STEP";
        issue["message"] = "run step failed: " + name;
        issue["file"] = "";
        issue["line"] = 0;
        add_issue(issue);
    }

    int errors = 0;
    int warnings = 0;
    for (const Json& issue : issues) {
        const std::string severity = issue.value("severity", "");
        if (severity == "error") ++errors;
        if (severity == "warning") ++warnings;
    }
    Json counts = Json::object();
    counts["error"] = errors;
    counts["warning"] = warnings;

    Json out = Json::object();
    out["ok"] = true;
    out["runId"] = run_dir.empty() ? std::string() : BaseName(run_dir);
    out["runDir"] = run_dir;
    out["running"] = false;
    out["stage"] = "idle";
    out["error"] = "";
    out["issues"] = std::move(issues);
    out["counts"] = std::move(counts);
    out["markers"] = std::move(markers);
    out["logs"] = std::move(logs);
    return out;
}

// -----------------------------------------------------------------------------
//  B 批：flow 引擎接入（flow_run / flow_cancel / flow_status / flow_wait /
//        strategy_compare / cables）
// -----------------------------------------------------------------------------
//  实现直接复用 `flow::FlowRunner` / `flow::CompareStrategies` / `flow::Inspect` ——
//  与 LSP 的 `fpgalsp.runFlow` / `compareStrategies` / `cables` 是**同一份实现**
//  （见 src/server/commands.cpp 的对应分支），MCP 这里只做参数抽取。
//
//  ⚠ 三条安全约定：
//  1. **不写 stdout**：FlowRunner 的 notify 回调在 MCP 里丢弃（flowEvent 是 LSP 通知形状，
//     混进 MCP 的 newline 流就是协议污染）；日志走 platform logger。
//  2. **工程切换必须重建 runner**：FlowRunner 绑定 Project 副本，跑在旧工程上比没跑更糟。
//  3. **flow 会往 <project>/.fpgalsp/flow 写日志**（P-4）：调用方要在临时副本上跑，
//     测试里由 e2e 负责 copytree；这里不替调用方改写工程路径。
flow::FlowRunner* Bridge::EnsureFlowRunner(const project::Project& proj) {
    if (flow_runner_ != nullptr && flow_project_ == proj.al_path) return flow_runner_.get();
    if (flow_runner_ != nullptr) {
        // 上一个工程的 runner：先取消再销毁（析构会 join 线程）
        flow_runner_->Cancel();
        flow_runner_.reset();
        flow_project_.clear();
    }
    const std::optional<td::TdChoice> choice = td::FindTd(options_.td_path);
    if (!choice.has_value()) throw ToolError("No TangDynasty installation found");
    td_choice_ = *choice;

    flow::FlowOptions flow_options;
    // notify：MCP 没有 fpgaLsp/flowEvent 通道，明确丢弃（不要接到 stdout）
    const flow::FlowRunner::NotifyFn notify = [](const jsonrpc::Json&) {};
    const flow::FlowRunner::LogFn log = [](int level, std::string_view message) {
        if (level >= 2) {
            fpgalsp::platform::default_logger().warn(std::string("[flow] ") + std::string(message));
        } else {
            fpgalsp::platform::default_logger().info(std::string("[flow] ") + std::string(message));
        }
    };
    flow_runner_ = std::make_unique<flow::FlowRunner>(proj, *choice, notify, log, flow_options);
    flow_project_ = proj.al_path;
    return flow_runner_.get();
}

Json Bridge::FlowRunTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);

    std::vector<std::string> stages;
    if (const auto it = arguments.find("stages"); it != arguments.end() && it->is_array()) {
        for (const Json& item : *it) {
            if (item.is_string()) stages.push_back(item.get<std::string>());
        }
    }
    if (stages.empty()) stages.push_back("all");  // 等价 GUI 的 Generate Bitstream
    int jobs = 6;
    if (const auto it = arguments.find("jobs"); it != arguments.end() && it->is_number_integer()) {
        jobs = std::clamp(it->get<int>(), 1, 64);
    }

    flow::FlowRunner* runner = EnsureFlowRunner(proj);
    Json started = runner->Start(stages, jobs);
    // P-B5：`job` 是 `runId` 的稳定别名，供 fpga_job_status / fpga_job_cancel 使用。
    if (started.is_object() && started.contains("runId")) {
        started["job"] = started["runId"];
        started["async"] = true;  // 已经在后台线程跑，本调用立刻返回
    }
    return started;
}

Json Bridge::FlowCancelTool(const Json& arguments) {
    (void)ProjectFor(arguments);  // 校验工程存在（与 Python 的 self.project(project) 一致）
    // runner 为 nullptr 或不在跑 -> {ok:true, note:"not running"}（客户端契约）
    if (flow_runner_ == nullptr) {
        Json out = Json::object();
        out["ok"] = true;
        out["note"] = "not running";
        return out;
    }
    return flow_runner_->Cancel();
}

// P-B5：按 job id 查询。job 不在跑（或不是这个 job）时明确返回 found:false，
// 而不是让 agent 把"没在跑"误读成"跑完了"。
Json Bridge::JobStatusTool(const Json& arguments) {
    const std::string job = arguments.value("job", std::string());
    const project::Project& proj = ProjectFor(arguments);
    Json out = Json::object();
    out["ok"] = true;
    out["job"] = job;
    if (flow_runner_ == nullptr || flow_project_ != proj.al_path) {
        out["found"] = false;
        out["running"] = false;
        out["note"] = "no active flow for this project";
        out["flow"] = IdleFlowState();
        return out;
    }
    flow::FlowState state = flow_runner_->Status();
    const bool matches = job.empty() || job == state.run_id;
    out["found"] = matches;
    out["running"] = matches && state.running;
    out["flow"] = state.ToJson();
    if (!matches) out["note"] = "job id does not match the active run";
    return out;
}

Json Bridge::JobCancelTool(const Json& arguments) {
    const std::string job = arguments.value("job", std::string());
    (void)ProjectFor(arguments);
    Json out = Json::object();
    out["ok"] = true;
    out["job"] = job;
    if (flow_runner_ == nullptr) {
        out["cancelled"] = false;
        out["note"] = "not running";
        return out;
    }
    const flow::FlowState state = flow_runner_->Status();
    if (!job.empty() && job != state.run_id) {
        out["cancelled"] = false;
        out["note"] = "job id does not match the active run";
        return out;
    }
    Json cancelled = flow_runner_->Cancel();
    out["cancelled"] = cancelled.value("ok", false);
    out["runId"] = state.run_id;
    return out;
}

Json Bridge::FlowStatus(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);
    const reports::ReportsBundle bundle = reports::BuildReports(proj);
    const Json data = bundle.ToJson();

    Json out = Json::object();
    out["ok"] = true;
    if (flow_runner_ != nullptr && flow_project_ == proj.al_path) {
        out["flow"] = flow_runner_->Status().ToJson();
    } else {
        out["flow"] = IdleFlowState();
    }
    out["artifacts"] = data.contains("artifacts") ? data["artifacts"] : Json::object();
    return out;
}

Json Bridge::FlowWait(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);

    int timeout = 3600;
    int interval = 1;
    if (const auto it = arguments.find("timeout"); it != arguments.end() && it->is_number_integer()) {
        timeout = std::clamp(it->get<int>(), 1, 7200);
    }
    if (const auto it = arguments.find("interval");
        it != arguments.end() && it->is_number_integer()) {
        interval = std::clamp(it->get<int>(), 1, 30);
    }

    if (flow_runner_ == nullptr || flow_project_ != proj.al_path) {
        // 对应 Python：runner 为空 / 别的工程 -> 立即返回，不发进度
        Json out = Json::object();
        out["ok"] = true;
        out["running"] = false;
        out["note"] = "no active flow for this project";
        out["flow"] = IdleFlowState();
        return out;
    }

    const auto started = std::chrono::steady_clock::now();
    const auto elapsed_seconds = [&started]() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    };
    while (true) {
        // P-B4：flow_wait 是最长的阻塞点（默认 3600 s），必须能被取消。
        if (Cancelled()) throw ToolError("cancelled");
        flow::FlowState state = flow_runner_->Status();
        const double elapsed = elapsed_seconds();
        const std::string message = !state.last_message.empty() ? state.last_message
                                  : !state.stage.empty()     ? state.stage
                                                             : "flow running";
        if (progress_) progress_(elapsed, message);
        if (!state.running) break;
        if (elapsed >= static_cast<double>(timeout)) {
            Json out = Json::object();
            out["ok"] = false;
            out["timedOut"] = true;
            out["running"] = true;
            out["elapsed"] = elapsed;
            out["flow"] = state.ToJson();
            out["issues"] = FlowIssues(arguments);
            return out;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(interval * 100));
    }

    flow::FlowState state = flow_runner_->Status();
    if (progress_) progress_(elapsed_seconds(), "flow finished");
    Json out = Json::object();
    out["ok"] = state.ok.value_or(true);
    out["timedOut"] = false;
    out["running"] = false;
    out["elapsed"] = elapsed_seconds();
    out["flow"] = state.ToJson();
    out["issues"] = FlowIssues(arguments);
    return out;
}

Json Bridge::StrategyCompareTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);

    std::optional<td::TdChoice> choice = td::FindTd(options_.td_path);
    if (!choice.has_value()) throw ToolError("No TangDynasty installation found");
    td_choice_ = *choice;

    std::vector<std::string> wanted;
    if (const auto it = arguments.find("strategies"); it != arguments.end() && it->is_array()) {
        for (const Json& item : *it) {
            if (item.is_string()) wanted.push_back(item.get<std::string>());
        }
    }
    flow::CompareOptions compare_options;
    if (const auto it = arguments.find("jobs"); it != arguments.end() && it->is_number_integer()) {
        compare_options.jobs = std::clamp(it->get<int>(), 1, 64);
    }
    return flow::CompareStrategies(proj, *choice, wanted, compare_options);
}

Json Bridge::CablesTool(const Json&) {
    // 对应 fpgalsp.cables：只读探测，**不烧录**
    const project::Project& proj = ActiveProject();
    std::optional<td::TdChoice> choice = td::FindTd(options_.td_path);
    if (!choice.has_value()) throw ToolError("No TangDynasty installation found");
    td_choice_ = *choice;
    return flow::Inspect(*choice, proj, flow::ProgramOptions{});
}

// -----------------------------------------------------------------------------
//  0.16.0：片上探针（ChipWatcher）
// -----------------------------------------------------------------------------
//  status / config 只读工程文件（毫秒级，不碰硬件）；
//  capture 会起一个独立 TD 会话驱动 JTAG，因此：
//    * 与 fpgalsp.program 共用进程级 HardwareLease（并发会抢同一根下载线）
//    * download:true 需要 confirm:true（等同烧录 SRAM 的确认强度）
// -----------------------------------------------------------------------------
Json Bridge::ProbeStatusTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);
    return probe::Status(proj);
}

Json Bridge::ProbeConfigTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);
    std::vector<std::string> signals;
    if (const auto it = arguments.find("signals");
        it != arguments.end() && it->is_array()) {
        for (const auto& item : *it) {
            if (item.is_string()) signals.push_back(item.get<std::string>());
        }
    }
    bool write = false;
    if (const auto it = arguments.find("write"); it != arguments.end() && it->is_boolean()) {
        write = it->get<bool>();
    }
    // 采样时钟/深度：TD 只在 "Optimize Gate 之后" 的 insert_debugger 里采纳，且必须通过
    // `-cwc <配置>` 传入（实测 depth 2048 生效）。这里只收参数，flow 负责生成配置。
    bool has_clock = false;
    std::string clock;
    if (const auto it = arguments.find("clock"); it != arguments.end() && it->is_string()) {
        clock = it->get<std::string>();
        has_clock = !clock.empty();
    }
    int depth = 0;
    if (const auto it = arguments.find("depth"); it != arguments.end() && it->is_number_integer()) {
        depth = it->get<int>();
    }
    bool apply_build_config = false;
    if (const auto it = arguments.find("apply_build_config");
        it != arguments.end() && it->is_boolean()) {
        apply_build_config = it->get<bool>();
    }
    Json out = probe::Configure(proj, signals, write, has_clock, clock, depth);
    if (apply_build_config) {
        probe::ProbeBuildConfig config = probe::ReadProbeBuildConfig(proj);
        config.apply_build_config = true;
        probe::WriteProbeBuildConfig(proj, config);
        out["apply_build_config"] = true;
    }
    // ---- 名字预检（非阻塞）----
    // 写进 user.mdn 的名字若在 RTL 里根本不存在，TD 只会在跑完一轮 syn+phy 之后
    // 静默丢掉（KIT-2501 / empty watcher）。这里用 HDL 索引先看一眼：找不到的放进
    // `unresolved` 并给一句 warning，但**不**判失败 —— 索引只认声明，层级名/生成块
    // 里的网可能查不到，硬拦会误伤。
    const Json& after = out.contains("after") ? out["after"] : Json::array();
    if (after.is_array() && !after.empty()) {
        Json unresolved = Json::array();
        try {
            const hdl::HdlIndex& index = EnsureHdlIndex(proj);
            std::map<std::string, bool> cache;
            std::set<std::string> seen;
            for (const Json& item : after) {
                if (!item.is_string()) continue;
                std::string name = item.get<std::string>();
                const std::size_t bracket = name.rfind('[');
                if (bracket != std::string::npos) name.erase(bracket);
                const std::size_t slash = name.rfind('/');
                const std::string leaf = slash == std::string::npos ? name : name.substr(slash + 1);
                if (!seen.insert(name).second) continue;
                bool known = false;
                if (const auto hit = cache.find(name); hit != cache.end()) {
                    known = hit->second;
                }
                if (!known) {
                    for (const auto& symbol : index.SearchSymbols(leaf)) {
                        if (symbol.name == leaf) {
                            known = true;
                            break;
                        }
                    }
                    cache[name] = known;
                }
                if (!known) unresolved.push_back(name);
            }
        } catch (...) {
            // 索引不可用就当没做预检，绝不因此挡住写 mdn
        }
        out["unresolved"] = std::move(unresolved);
        if (out["unresolved"].size() > 0) {
            out["warning"] =
                "some probe names were not found by the HDL index - TD silently drops nets it "
                "cannot resolve after a full syn+phy run. Check them with fpga_symbol_path "
                "(it returns the exact <top>/<inst>/<net>[msb:lsb] form).";
        }
    }
    return out;
}

Json Bridge::ProbeVerifyTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);
    bool include_bits = false;
    if (const auto it = arguments.find("include_bits");
        it != arguments.end() && it->is_boolean()) {
        include_bits = it->get<bool>();
    }

    // "名字在设计里到底有没有" 只有 HDL 索引能回答。这层已经在（EnsureHdlIndex），
    // 所以不把 hdl 依赖塞进 flow/probe：判定逻辑留在 MCP 侧，probe::Verify 只收一个谓词。
    // ⚠ 索引是懒构建的（大工程冷构建几百 ms），所以**第一次真正需要判定时**才建；
    //   probe::Verify 只在确实缺 net 时才调这个谓词，"全嵌进去了"的常规路径零成本。
    const hdl::HdlIndex* index = nullptr;
    std::map<std::string, bool> cache;
    const std::string top_prefix = proj.top.empty() ? std::string() : proj.top + "/";
    const auto known = [this, &proj, &index, &cache,
                        &top_prefix](std::string_view name) -> bool {
        std::string base(name);
        if (!top_prefix.empty() && base.rfind(top_prefix, 0) == 0) {
            base.erase(0, top_prefix.size());
        }
        const std::size_t open = base.find_last_of('[');
        if (open != std::string::npos) base.erase(open);
        const std::size_t slash = base.find_last_of('/');
        if (slash != std::string::npos) base.erase(0, slash + 1);
        if (base.empty()) return false;
        const auto hit = cache.find(base);
        if (hit != cache.end()) return hit->second;
        if (index == nullptr) index = &EnsureHdlIndex(proj);

        bool found = index->ModuleByName(base) != nullptr;
        if (!found) {
            // SearchSymbols 是大小写不敏感子串匹配，必须再按原样精确比一次
            //（Verilog 大小写敏感，`Foo` 不能因为存在 `foo` 就被判成"名字没写错"）。
            for (const hdl::WorkspaceSymbol& symbol : index->SearchSymbols(base)) {
                if (symbol.name == base) {
                    found = true;
                    break;
                }
            }
        }
        cache.emplace(base, found);
        return found;
    };
    return probe::Verify(proj, include_bits, known);
}

Json Bridge::ProbeCaptureTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);
    std::optional<td::TdChoice> choice = td::FindTd(options_.td_path);
    if (!choice.has_value()) throw ToolError("No TangDynasty installation found");
    td_choice_ = *choice;

    probe::CaptureOptions capture_options;
    if (const auto it = arguments.find("download"); it != arguments.end()) {
        if (it->is_boolean()) {
            capture_options.download = it->get<bool>() ? "true" : "false";
        } else if (it->is_string()) {
            capture_options.download = it->get<std::string>();
        }
    }
    if (const auto it = arguments.find("speed"); it != arguments.end() && it->is_number_integer()) {
        const int value = it->get<int>();
        if (value >= 2 && value <= 9) capture_options.speed = value;
    }
    if (const auto it = arguments.find("trigger_now"); it != arguments.end() && it->is_boolean()) {
        capture_options.trigger_now = it->get<bool>();
    }
    if (const auto it = arguments.find("instance");
        it != arguments.end() && it->is_string()) {
        capture_options.instance = it->get<std::string>();
    }
    if (const auto it = arguments.find("cable"); it != arguments.end() && it->is_string()) {
        capture_options.cable = it->get<std::string>();
    }
    if (const auto it = arguments.find("csv"); it != arguments.end() && it->is_string()) {
        capture_options.csv_path = it->get<std::string>();
    }
    if (const auto it = arguments.find("allow_stale");
        it != arguments.end() && it->is_boolean()) {
        capture_options.allow_stale = it->get<bool>();
    }
    // 触发设置（运行期可改）：扁平参数，和 LSP 的嵌套 trigger 对象等价
    if (const auto it = arguments.find("trigger_position");
        it != arguments.end() && it->is_number_integer()) {
        capture_options.trigger.position = it->get<int>();
    }
    if (const auto it = arguments.find("window_num");
        it != arguments.end() && it->is_number_integer()) {
        capture_options.trigger.window_num = it->get<int>();
    }
    if (const auto it = arguments.find("window_depth");
        it != arguments.end() && it->is_number_integer()) {
        capture_options.trigger.window_depth = it->get<int>();
    }
    if (const auto it = arguments.find("trigger_type");
        it != arguments.end() && it->is_string()) {
        capture_options.trigger.type = it->get<std::string>();
    }
    if (const auto it = arguments.find("trigger_enabled");
        it != arguments.end() && it->is_boolean()) {
        capture_options.trigger.enabled = it->get<bool>();
    }
    if (const auto it = arguments.find("bus_op"); it != arguments.end() && it->is_string()) {
        capture_options.trigger.bus_op = it->get<std::string>();
    }
    if (const auto it = arguments.find("cwc_bus_order");
        it != arguments.end() && it->is_array()) {
        for (const Json& item : *it) {
            if (item.is_string()) capture_options.cwc_bus_order.push_back(item.get<std::string>());
        }
    }
    if (const auto it = arguments.find("generate_cwc");
        it != arguments.end() && it->is_boolean()) {
        capture_options.generate_cwc = it->get<bool>();
    }
    // per-bus 触发值：`[{name, op, value}]`，value 是**二进制位串**（与采集 CSV 同格式）。
    // 校验放在这里而不是后面：TD 对未知 op / 缺名字的元素不会报错，只会静默按默认处理，
    // 那种失败在 agent 侧看起来就是"工具不支持这个条件"。
    if (const auto it = arguments.find("buses"); it != arguments.end() && it->is_array()) {
        for (const Json& item : *it) {
            if (!item.is_object()) {
                throw ToolError(
                    "buses entries must be objects: {\"name\":\"pwm_reg0_i[15:0]\",\"op\":\"equal\","
                    "\"value\":\"0000000000000000\"}");
            }
            const std::string name = item.value("name", std::string());
            if (name.empty()) {
                throw ToolError("buses entry is missing \"name\" (the watched bus, e.g. pwm_reg0_i[15:0])");
            }
            probe::CaptureOptions::Trigger::BusTrigger bus;
            bus.name = name;
            if (const auto op = item.find("op"); op != item.end()) {
                if (!op->is_string()) {
                    throw ToolError("buses entry \"" + name + "\": op must be a string");
                }
                bus.op = op->get<std::string>();
                if (!probe::IsTriggerBusOp(bus.op)) {
                    // 白名单与 schema 的 enum 逐字一致；故意不含 dont_care
                    //（TD 里全局 bus op=dont_care 会让 per-net 条件失效）。
                    throw ToolError(
                        "buses entry \"" + name + "\": unsupported op \"" + bus.op +
                        "\" (expected equal, not_equal, less, less_equal, greater, "
                        "greater_equal or bit_trigger)");
                }
            }
            if (const auto value = item.find("value"); value != item.end()) {
                if (!value->is_string()) {
                    throw ToolError("buses entry \"" + name +
                                    "\": value must be a binary bit string, e.g. \"0000000000000000\"");
                }
                const std::optional<std::uint64_t> bits =
                    probe::ParseBitString(value->get<std::string>());
                if (!bits.has_value()) {
                    throw ToolError("buses entry \"" + name +
                                    "\": value must be a binary bit string, e.g. "
                                    "\"0000000000000000\" (only 0/1, at most 64 significant bits)");
                }
                bus.value = *bits;
                bus.has_value = true;
            }
            capture_options.trigger.buses.push_back(std::move(bus));
        }
    }
    if (const auto it = arguments.find("timeout");
        it != arguments.end() && it->is_number_integer()) {
        const int timeout = it->get<int>();
        if (timeout > 0) capture_options.wait_timeout = static_cast<double>(timeout);
    }
    bool confirm = false;
    if (const auto it = arguments.find("confirm"); it != arguments.end() && it->is_boolean()) {
        confirm = it->get<bool>();
    }
    if (capture_options.download == "true" || capture_options.download == "auto" ||
        capture_options.download == "always") {
        if (!confirm) {
            return flow::Refusal(
                "confirm",
                "capture may reprogram the FPGA SRAM (download:auto keeps the current bitstream "
                "when it already answers, download:true always reloads); pass confirm:true, or "
                "download:false to only read the already-loaded design");
        }
    }
    if (capture_options.download == "false") {
        // 显式只读：不需要确认
    }
    return probe::Capture(*choice, proj, capture_options);
}
// -----------------------------------------------------------------------------
//  C 批：补全 / 实例化模板
// -----------------------------------------------------------------------------
//  completion  —— features::Completion（与 LSP 同一实现）+ Python bridge 的过滤/裁剪
//  instantiation —— 按 Python `_instantiation_text` 的纯文本格式生成（无 snippet 占位符）
//  两者都只读：不写文件、不启动 TD。
// -----------------------------------------------------------------------------
Json Bridge::CompletionTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);
    const std::string path = ResolveFile(arguments.value("file", std::string()), proj);
    const int line = std::max(0, arguments.value("line", 1) - 1);
    const int column = std::max(0, arguments.value("column", 1) - 1);

    std::string prefix = LowerAscii(arguments.value("prefix", std::string()));
    int limit = 50;
    if (const auto it = arguments.find("limit"); it != arguments.end() && it->is_number_integer()) {
        limit = std::clamp(it->get<int>(), 1, 500);
    }
    bool include_snippets = true;
    if (const auto it = arguments.find("include_snippets");
        it != arguments.end() && it->is_boolean()) {
        include_snippets = it->get<bool>();
    }

    const hdl::HdlIndex& index = EnsureHdlIndex(proj);
    // completion 需要"当前行内容"判断光标是否在行首/缩进；MCP 没有未保存缓冲区 -> 读磁盘
    const std::string text = ReadTextFile(path);
    const Json result = server::features::Completion(index, path, text, line, column);

    Json items = Json::array();
    bool truncated = false;
    const Json& raw_items = result.contains("items") ? result["items"] : Json::array();
    if (raw_items.is_array()) {
        for (const Json& item : raw_items) {
            if (!item.is_object()) continue;
            // Python：label 可能是 {label: ...} 形状的 TextEdit 关联对象
            std::string label;
            if (item.contains("label") && item["label"].is_string()) {
                label = item["label"].get<std::string>();
            } else if (item.contains("label") && item["label"].is_object()) {
                label = item["label"].value("label", std::string());
            }
            if (!prefix.empty() && LowerAscii(label).rfind(prefix, 0) != 0) continue;
            if (!include_snippets && item.value("insertTextFormat", 1) == 2) continue;

            Json out_item = Json::object();
            out_item["label"] = label;
            out_item["kind"] = SymbolKindName(item.value("kind", 0));
            out_item["detail"] = item.value("detail", std::string());
            out_item["insertText"] = item.value("insertText", std::string());
            out_item["insertTextFormat"] =
                item.contains("insertTextFormat") ? item["insertTextFormat"] : Json(nullptr);
            out_item["documentation"] = HoverText(item.contains("documentation")
                                                      ? Json{{"contents", item["documentation"]}}
                                                      : Json(nullptr));
            items.push_back(std::move(out_item));
            if (static_cast<int>(items.size()) >= limit) {
                truncated = true;
                break;
            }
        }
    }

    Json out = Json::object();
    out["ok"] = true;
    out["items"] = std::move(items);
    out["truncated"] = truncated;
    return out;
}

Json Bridge::InstantiationTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);
    const std::string module_name = arguments.value("module", std::string());
    if (module_name.empty()) throw ToolError("Missing required argument: module");

    const hdl::HdlIndex& index = EnsureHdlIndex(proj);
    const hdl::Module* definition = index.ModuleByName(module_name);
    if (definition == nullptr) throw ToolError("Module not found: " + module_name);

    std::string indent = arguments.value("indent", std::string());
    if (indent.empty()) indent = "    ";
    std::string instance = arguments.value("instance_name", std::string());
    if (instance.empty()) instance = "u_" + definition->name;

    // 逐行对应 Python server.py 的 _instantiation_text（纯文本，无 $1 占位符）
    const std::string default_instance = "u_" + definition->name;
    std::string text;
    if (!definition->parameters.empty()) {
        text += definition->name + " #(\n";
        for (size_t i = 0; i < definition->parameters.size(); ++i) {
            const std::string& parameter = definition->parameters[i];
            const bool last = (i + 1 == definition->parameters.size());
            text += indent + "    ." + parameter + " (" + parameter + ")" + (last ? "" : ",") + "\n";
        }
        text += indent + ") " + default_instance + " (\n";
    } else {
        text += definition->name + " " + default_instance + " (\n";
    }
    for (size_t i = 0; i < definition->ports.size(); ++i) {
        const std::string& port_name = definition->ports[i].name;
        const bool last = (i + 1 == definition->ports.size());
        text += indent + "    ." + port_name + " (" + port_name + ")" + (last ? "" : ",") + "\n";
    }
    text += indent + ");\n";

    // Python: `return indent + "\n".join(lines) + "\n"` —— 整段文本还有一个**行首缩进**，
    // 所以首行是 "    trans #(" 而不是 "trans #("（实测 diff 就出在这一处）。
    text = indent + text;

    // 自定义实例名：与 Python 一样做一次文本替换（默认实例名 -> 指定名）
    if (instance != default_instance) {
        size_t position = 0;
        while ((position = text.find(default_instance, position)) != std::string::npos) {
            text.replace(position, default_instance.size(), instance);
            position += instance.size();
        }
    }

    Json ports = Json::array();
    for (const hdl::Port& port : definition->ports) {
        Json item = Json::object();
        item["name"] = port.name;
        item["direction"] = port.direction;
        item["width"] = port.width;
        ports.push_back(std::move(item));
    }
    Json parameters = Json::array();
    for (const std::string& name : definition->parameters) parameters.push_back(name);

    Json out = Json::object();
    out["ok"] = true;
    out["module"] = definition->name;
    out["instance"] = instance;
    out["text"] = text;
    out["ports"] = std::move(ports);
    out["parameters"] = std::move(parameters);
    return out;
}
// -----------------------------------------------------------------------------
//  C 批：写工程（project_rename / project_add_source）
// -----------------------------------------------------------------------------
//  对应 Python：`mcp_bridge.project_rename/add_source` -> `ide.set_project_name/add_source_file`。
//
//  ⚠ 这是**唯一会修改用户文件**的工具组（MCP schema 里 requires="write"，
//    默认不暴露，必须 --allow-write）。所以流程刻意做重：
//      1. 先把新内容算好（纯字符串操作，不碰文件）
//      2. **原子写**：写同目录临时文件 -> rename 覆盖（失败不会留下半截文件）
//      3. 写完**重新解析 .al 验证**：只有预期字段变化才算成功
//      4. 验证失败 -> 用原文原子写回（回滚），并返回 {ok:false, error}
//  这套"算-写-验-回滚"是为了满足"失败时不要留下半改状态"。
// -----------------------------------------------------------------------------
namespace {

// 原子写：同目录临时文件 + rename 覆盖。失败返回 false（不抛）。
bool WriteFileAtomic(const std::string& path, const std::string& content) {
    const std::string temp = path + ".fpgalsp-tmp";
    if (!platform::write_file_utf8(temp, content)) return false;
    std::error_code ec;
    fs::rename(fs::u8path(temp), fs::u8path(path), ec);
    if (ec) {
        fs::remove(fs::u8path(temp), ec);
        return false;
    }
    return true;
}

std::string NormaliseSlashes(std::string text) {
    for (char& c : text) {
        if (c == '\\') c = '/';
    }
    return text;
}

std::string LowerText(std::string_view text) { return LowerAscii(text); }

// Python ide._relative：相对工程目录、正斜杠
std::string ProjectRelative(const std::string& path, const std::string& project_dir) {
    return NormaliseSlashes(RelativeTo(path, project_dir));
}

}  // namespace

Json Bridge::ProjectRenameTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);
    std::string name = Trim(arguments.value("name", std::string()));
    if (name.empty()) throw ToolError("Project name cannot be empty");
    if (name.find_first_of("<>\"&") != std::string::npos) {
        throw ToolError("Project name cannot contain < > \" &");
    }

    const std::string original = ReadTextFile(proj.al_path);
    if (original.empty()) throw ToolError("Cannot read " + proj.al_path);

    static const re2::RE2 name_tag(R"(<Name>[^<]*</Name>)");
    static const re2::RE2 project_tag(R"(<Project\b[^>]*>)");
    std::string updated;
    {
        const std::string replacement = "<Name>" + name + "</Name>";
        std::string::size_type position = original.find("<Name>");
        bool replaced = false;
        if (position != std::string::npos) {
            const std::string::size_type close = original.find("</Name>", position);
            if (close != std::string::npos) {
                updated = original.substr(0, position) + replacement +
                          original.substr(close + 7);
                replaced = true;
            }
        }
        if (!replaced) {
            std::string match;
            if (!re2::RE2::PartialMatch(original, project_tag, &match)) {
                throw ToolError("No <Project> tag in the .al");
            }
            const std::string::size_type insert_at = original.find(match) + match.size();
            updated = original.substr(0, insert_at) + "\n    <Name>" + name + "</Name>" +
                      original.substr(insert_at);
        }
    }
    (void)name_tag;

    if (updated == original) {
        Json out = Json::object();
        out["ok"] = true;
        out["changed"] = false;
        out["name"] = name;
        return out;
    }

    if (!WriteFileAtomic(proj.al_path, updated)) {
        throw ToolError("Cannot write " + proj.al_path);
    }

    // 验证：重新解析，只允许项目名变化
    const project::Project after = project::ParseAl(proj.al_path);
    if (after.name != name) {
        (void)WriteFileAtomic(proj.al_path, original);  // 回滚
        throw ToolError("Verification failed: project name is still '" + after.name + "' (rolled back)");
    }
    if (after.files.size() != proj.files.size()) {
        (void)WriteFileAtomic(proj.al_path, original);  // 回滚
        throw ToolError("Verification failed: file list changed unexpectedly (rolled back)");
    }

    Json out = Json::object();
    out["ok"] = true;
    out["changed"] = true;
    out["name"] = name;
    // D-2：Python 的 setProjectName 只回 {ok, changed, name}（无 al），为字段级等价已移除。
    return out;
}

Json Bridge::ProjectAddSourceTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);

    const std::string file_value = arguments.value("file", std::string());
    const std::string source = SafeProjectFile(file_value, proj);  // 必须在工程目录内且存在
    const std::string rel = ProjectRelative(source, proj.dir);

    const std::string original = ReadTextFile(proj.al_path);
    if (original.empty()) throw ToolError("Cannot read " + proj.al_path);

    // 定位 <Source_Files> ... </Source_Files>
    const std::string open_tag = "<Source_Files>";
    const std::string close_tag = "</Source_Files>";
    const std::string::size_type open_at = original.find(open_tag);
    const std::string::size_type close_at =
        open_at == std::string::npos ? std::string::npos
                                     : original.find(close_tag, open_at + open_tag.size());
    if (open_at == std::string::npos || close_at == std::string::npos) {
        throw ToolError("No <Source_Files> section in the .al");
    }
    const std::string body =
        original.substr(open_at + open_tag.size(), close_at - open_at - open_tag.size());

    // 已存在（大小写不敏感、斜杠归一）-> added:false
    const std::string rel_lower = LowerText(rel);
    {
        static const re2::RE2 file_path(R"re(<File\s+Path="([^"]+)")re");
        absl::string_view rest(body);
        re2::StringPiece captured;
        while (!rest.empty()) {
            re2::RE2::Arg arg(&captured);
            const re2::RE2::Arg* args[1] = {&arg};
            const size_t before = rest.size();
            if (!re2::RE2::FindAndConsumeN(&rest, file_path, args, 1)) break;
            if (rest.size() >= before) break;
            const std::string existing(captured.data(), captured.size());
            if (LowerText(NormaliseSlashes(existing)) == rel_lower) {
                Json out = Json::object();
                out["ok"] = true;
                out["added"] = false;
                out["rel"] = rel;
                out["compileOrder"] = Json(nullptr);
                return out;
            }
        }
    }

    // 下一个 CompileOrder = max + 1
    int compile_order = 1;
    {
        static const re2::RE2 order_re(R"re(Name="CompileOrder"\s+Val="([0-9]+)")re");
        absl::string_view rest(body);
        re2::StringPiece captured;
        while (!rest.empty()) {
            re2::RE2::Arg arg(&captured);
            const re2::RE2::Arg* args[1] = {&arg};
            const size_t before = rest.size();
            if (!re2::RE2::FindAndConsumeN(&rest, order_re, args, 1)) break;
            if (rest.size() >= before) break;
            try {
                compile_order = std::max(compile_order,
                                         std::stoi(std::string(captured.data(), captured.size())) + 1);
            } catch (...) {
                continue;
            }
        }
    }

    const bool vhdl = LowerText(rel).size() >= 4 &&
                      (LowerText(rel).compare(LowerText(rel).size() - 4, 4, ".vhd") == 0 ||
                       (LowerText(rel).size() >= 5 &&
                        LowerText(rel).compare(LowerText(rel).size() - 5, 5, ".vhdl") == 0));
    const std::string tag = vhdl ? "VHDL" : "Verilog";

    // 缩进：沿用同段里第一个 <Verilog>/<VHDL>/<ADC_FILE>/<SDC_FILE>/<IP> 的缩进，缺省 8 空格
    std::string indent = "        ";
    {
        static const re2::RE2 indent_re(R"(\n([ \t]+)<(?:Verilog|VHDL|ADC_FILE|SDC_FILE|IP)>)");
        std::string captured;
        if (re2::RE2::PartialMatch(body, indent_re, &captured)) indent = captured;
    }
    const std::string inner = indent + "    ";

    std::string block;
    block += indent + "<" + tag + ">\n";
    block += inner + "<File Path=\"" + rel + "\">\n";
    block += inner + "    <FileInfo>\n";
    block += inner + "        <Attr Name=\"UsedInSyn\" Val=\"true\"/>\n";
    block += inner + "        <Attr Name=\"UsedInP&R\" Val=\"true\"/>\n";
    block += inner + "        <Attr Name=\"BelongTo\" Val=\"design_1\"/>\n";
    block += inner + "        <Attr Name=\"CompileOrder\" Val=\"" + std::to_string(compile_order) +
             "\"/>\n";
    block += inner + "    </FileInfo>\n";
    block += inner + "</File>\n";
    block += indent + "</" + tag + ">\n";

    // 插入点：</Source_Files> 之前；若该行只有空白，则插到行首（保持闭合标签缩进）
    std::string::size_type insert_at = close_at;
    const std::string::size_type line_start =
        original.rfind('\n', insert_at == 0 ? 0 : insert_at - 1);
    const std::string::size_type begin = (line_start == std::string::npos) ? 0 : line_start + 1;
    if (Trim(original.substr(begin, insert_at - begin)).empty()) insert_at = begin;
    const std::string updated =
        original.substr(0, insert_at) + block + original.substr(insert_at);

    if (!WriteFileAtomic(proj.al_path, updated)) {
        throw ToolError("Cannot write " + proj.al_path);
    }

    // 验证：新 .al 必须能解析、文件数 +1、且包含这个 rel
    const project::Project after = project::ParseAl(proj.al_path);
    bool found = false;
    for (const project::AlFile& item : after.files) {
        if (LowerText(NormaliseSlashes(item.rel)) == rel_lower) found = true;
    }
    if (!found || after.files.size() != proj.files.size() + 1) {
        (void)WriteFileAtomic(proj.al_path, original);  // 回滚
        throw ToolError("Verification failed: the new source is not in the .al (rolled back)");
    }

    Json out = Json::object();
    out["ok"] = true;
    out["added"] = true;
    out["rel"] = rel;
    out["compileOrder"] = compile_order;
    // D-2：Python 的 ide.add_source_file 只回 {ok, added, rel, compileOrder}；
    // C++ 早期多给的 fileCount 已移除（字段级等价）。
    if (arguments.value("update", false) && out.value("ok", false)) {
        // 对应 Python project_add_source：update=True 时把 project_update 的结果挂在 "update" 下。
        out["update"] = ProjectUpdateTool(arguments);
    }
    return out;
}

// =============================================================================
//  Phase B / B6：format / analysis_run / project_update 的真实实现
// =============================================================================
//  1) format：与 LSP `textDocument/formatting` 共用 Verible 桥（`verible_bridge::FormatText`），
//     所以同一份文本在两个通道得到逐字节相同的结果。
//  2) analysis_run / project_update：与 LSP 的 `fpgalsp.runAnalysis` / `fpgalsp.updateProject`
//     共用同一实现（`analysis::Run` / `analysis::UpdateProject`；B5 的
//     `FeatureRunAnalysis/FeatureUpdateProject` 就是这两个函数的一行包装）。
//     这里**不**去构造 LspServer：那需要 transport、会注册一堆 handler，
//     而这两个命令只依赖 project + td，直接调 analysis:: 更小、副作用更少。
// =============================================================================

Json Bridge::FormatTool(const Json& arguments) {
    std::string file_value;
    if (const auto it = arguments.find("file"); it != arguments.end() && it->is_string()) {
        file_value = it->get<std::string>();
    }
    std::string text_value;
    if (const auto it = arguments.find("text"); it != arguments.end() && it->is_string()) {
        text_value = it->get<std::string>();
    }

    // Python：file 优先，否则用 text；两者都空 -> ToolError("file or text is required")
    std::string path;
    std::string source;
    if (!file_value.empty()) {
        // SEC-02：这里过去有两条绕过 ResolveFile 的读路径 —— 绝对路径 normalize 一下
        // 就直接 ReadTextFile，以及"工程解析失败"时回退成 path_join(root_, file) 再读；
        // 后者连词法边界都没有，于是 format 成了磁盘任意文件（含密钥）的全文读取原语。
        // 现在两条分支都收敛到 ResolveFile（唯一的边界判定点），边界退化为 --root 本身。
        project::Project root_only;  // 没有可用工程时，信任根只有 root_
        root_only.dir = root_;
        std::string resolved;
        if (platform::is_absolute_path(file_value)) {
            resolved = ResolveFile(file_value, root_only);
        } else {
            try {
                resolved = ResolveFile(file_value, ProjectFor(arguments));
            } catch (const ToolError&) {
                resolved = ResolveFile(file_value, root_only);
            }
        }
        path = resolved;
        source = ReadTextFile(path);
        // Python 用文本模式读文件（universal newlines），doc.text 里 CRLF 已变成 LF；
        // 这里必须先做同样的归一化，否则 Verible 的 line_terminator=auto 会保留 CRLF，
        // 与 Python 的输出逐字节不同（实测 10883 B vs 11224 B）。
        if (source.find('\r') != std::string::npos) {
            std::string normalized_text;
            normalized_text.reserve(source.size());
            for (size_t index = 0; index < source.size(); ++index) {
                if (source[index] == '\r') {
                    normalized_text.push_back('\n');
                    if (index + 1 < source.size() && source[index + 1] == '\n') ++index;
                } else {
                    normalized_text.push_back(source[index]);
                }
            }
            source = std::move(normalized_text);
        }
    } else if (!text_value.empty()) {
        path = path_join(root_, "mcp_format.v");
        source = text_value;
    } else {
        throw ToolError("file or text is required");
    }

    // Python 的 tab_size/insert_spaces 只影响**fallback 重排器**，Verible 路径不读它们；
    // 这里保留参数解析只为与 Python 的入参契约一致（值不参与格式化）。
    int tab_size = 4;
    bool insert_spaces = true;
    if (const auto it = arguments.find("tab_size"); it != arguments.end() && it->is_number_integer()) {
        tab_size = it->get<int>();
    }
    if (const auto it = arguments.find("insert_spaces");
        it != arguments.end() && it->is_boolean()) {
        insert_spaces = it->get<bool>();
    }
    (void)tab_size;
    (void)insert_spaces;

#if defined(FPGA_LSP_HAVE_VERIBLE_BRIDGE)
    const verible_bridge::FormatResult formatted = verible_bridge::FormatText(source, path);
    if (!formatted.ok) {
        // Python 在 Verible 失败时会退到"保守重排器"（format_fallback.py，C++ 未移植）。
        // 这里选择**显式失败**，而不是返回一段未经格式化却看似成功的文本。
        throw ToolError("Formatter failed: " + formatted.error);
    }
    const std::string formatted_text = formatted.text;
#else
    // 与 Python 的 ToolError 文案一致（Verible 缺失且 fallback 关闭）
    throw ToolError("No formatter available (Verible missing and fallback disabled)");
#endif

    // range 变体（对应 Python server.on_textDocument_rangeFormatting）
    std::optional<int> start_line;
    std::optional<int> end_line;
    if (const auto it = arguments.find("start_line");
        it != arguments.end() && it->is_number_integer()) {
        start_line = it->get<int>();
    }
    if (const auto it = arguments.find("end_line"); it != arguments.end() && it->is_number_integer()) {
        end_line = it->get<int>();
    }
    if (start_line.has_value() && end_line.has_value()) {
        // MCP 的行号是 1-based；LSP range 是 0-based（Python 的 _mcp_range(start,1,end,1)）
        const int start = std::max(0, *start_line - 1);
        const int end = std::max(0, *end_line - 1);
        const std::vector<std::string> old_rows = SplitLines(source);
        const std::vector<std::string> new_rows = SplitLines(formatted_text);

        Json edits = Json::array();
        if (old_rows.size() != new_rows.size()) {
            // 行结构被改写：只有"整文档替换"能表达
            if (!formatted_text.empty()) {
                Json edit = Json::object();
                Json range = Json::object();
                Json rstart = Json::object();
                rstart["line"] = 0;
                rstart["character"] = 0;
                Json rend = Json::object();
                rend["line"] = static_cast<int>(old_rows.empty() ? 0 : old_rows.size() - 1);
                rend["character"] = static_cast<int>(old_rows.empty() ? 0 : old_rows.back().size());
                range["start"] = std::move(rstart);
                range["end"] = std::move(rend);
                edit["range"] = std::move(range);
                edit["newText"] = formatted_text;
                edits.push_back(std::move(edit));
            }
        } else {
            const int last = static_cast<int>(old_rows.size()) - 1;
            const int stop = std::min(end, last);
            for (int line = start; line <= stop; ++line) {
                if (old_rows[static_cast<size_t>(line)] == new_rows[static_cast<size_t>(line)]) {
                    continue;
                }
                if (Trim(new_rows[static_cast<size_t>(line)]).empty()) continue;
                Json edit = Json::object();
                Json range = Json::object();
                Json rstart = Json::object();
                rstart["line"] = line;
                rstart["character"] = 0;
                Json rend = Json::object();
                rend["line"] = line;
                rend["character"] = static_cast<int>(old_rows[static_cast<size_t>(line)].size());
                range["start"] = std::move(rstart);
                range["end"] = std::move(rend);
                edit["range"] = std::move(range);
                edit["newText"] = new_rows[static_cast<size_t>(line)];
                edits.push_back(std::move(edit));
            }
        }
        Json out = Json::object();
        out["ok"] = true;
        out["edits"] = std::move(edits);
        out["changed"] = !out["edits"].empty();
        return out;
    }

    // 整文档变体（对应 Python mcp_bridge.format 的非 range 分支）
    if (formatted_text.empty() && !source.empty()) {
        throw ToolError("No formatter available (Verible missing and fallback disabled)");
    }
    Json out = Json::object();
    out["ok"] = true;
    out["text"] = formatted_text;
    out["changed"] = formatted_text != source;
    return out;
}

Json Bridge::AnalysisRunTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);
    return analysis::Run(proj, options_.td_path, [](std::string_view message) {
        platform::default_logger().info(std::string(message));
    });
}

Json Bridge::ProjectUpdateTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);
    const std::string al = proj.al_path;
    Json result = analysis::UpdateProject(proj, options_.td_path, [](std::string_view message) {
        platform::default_logger().info(std::string(message));
    });
    // 对应 Python 的 invalidate_project_caches()：.al 可能被 TD 改写，缓存必须失效。
    projects_.erase(al);
    DropCachedProject(al);  // W4-A：同时丢掉进程级解析缓存（其它连接也要看到新 .al）
    return result;
}

// =============================================================================
//  B6c：code_actions / rename_edits
// =============================================================================
//  与 LSP 的 `textDocument/codeAction` / `textDocument/rename` **同一条实现**：
//  B3 把逻辑放在 LspServer::FeatureCodeActions / FeatureRename 里，它们依赖服务层
//  状态（ProjectForPath / IndexForProject / LoadText）。Bridge 这边不复制这套逻辑，
//  而是持有一个"无 transport 的 LspServer"（见文件上方 NullTransport 的说明），
//  懒构造 + 用 --root 初始化，之后转调 Feature*。
//
//  为什么不在 Bridge 里重写"索引 -> JSON"：B6 的教训是两份实现必然漂移
//  （bridge.h 的文件头也写了同一句话）。代价只是多一个 LspServer 对象（不跑 serve
//  循环、不注册 handler、不 publish 诊断），比重写一遍安全得多。
// =============================================================================

server::LspServer& Bridge::FeatureServer() {
    // Phase 2 / B1：单 Engine 模式下直接复用宿主进程的 LspServer。
    if (options_.shared_server != nullptr) return *options_.shared_server;
    if (feature_server_) return *feature_server_;
    feature_server_ = std::make_unique<server::LspServer>(NullTransportRef(), options_.td_path);
    Json params = Json::object();
    params["rootUri"] = platform::path_to_uri(root_);
    // 只取"能力声明 + 设置根路径"这一半语义；不打开任何文档、不发通知。
    feature_server_->FeatureInitialize(params);
    return *feature_server_;
}

Json Bridge::CodeActionsTool(const Json& arguments) {
    const project::Project& proj = ProjectFor(arguments);
    const std::string path = ResolveFile(arguments.value("file", std::string()), proj);

    int start_line = 1;
    int start_column = 1;
    int end_line = 0;
    int end_column = 0;
    if (const auto it = arguments.find("start_line");
        it != arguments.end() && it->is_number_integer()) {
        start_line = it->get<int>();
    }
    if (const auto it = arguments.find("start_column");
        it != arguments.end() && it->is_number_integer()) {
        start_column = it->get<int>();
    }
    end_line = start_line;
    end_column = start_column;
    if (const auto it = arguments.find("end_line");
        it != arguments.end() && it->is_number_integer()) {
        end_line = it->get<int>();
    }
    if (const auto it = arguments.find("end_column");
        it != arguments.end() && it->is_number_integer()) {
        end_column = it->get<int>();
    }
    bool include_edits = false;
    if (const auto it = arguments.find("include_edits");
        it != arguments.end() && it->is_boolean()) {
        include_edits = it->get<bool>();
    }

    // 对应 Python _mcp_range(start_line, start_column, end_line, end_column)：1-based -> 0-based
    Json range = Json::object();
    Json range_start = Json::object();
    range_start["line"] = std::max(0, start_line - 1);
    range_start["character"] = std::max(0, start_column - 1);
    Json range_end = Json::object();
    range_end["line"] = std::max(0, end_line - 1);
    range_end["character"] = std::max(0, end_column - 1);
    range["start"] = std::move(range_start);
    range["end"] = std::move(range_end);

    Json context = Json::object();
    context["diagnostics"] = Json::array();  // Python: {"diagnostics": []}

    const Json actions = FeatureServer().FeatureCodeActions(path, range, context);

    Json out_actions = Json::array();
    int count = 0;
    for (const Json& action : actions) {
        if (count++ >= 200) break;  // Python: actions[:200]
        Json item = Json::object();
        item["title"] = action.value("title", std::string());
        // Python: kind 可能是字符串或 {"value": ...}
        std::string kind;
        if (const auto it = action.find("kind"); it != action.end()) {
            if (it->is_string()) {
                kind = it->get<std::string>();
            } else if (it->is_object() && it->contains("value") && (*it)["value"].is_string()) {
                kind = (*it)["value"].get<std::string>();
            }
        }
        item["kind"] = kind;
        item["isPreferred"] = action.value("isPreferred", false);
        if (include_edits) {
            if (const auto edit = action.find("edit"); edit != action.end() && edit->is_object()) {
                Json edits = Json::object();
                if (const auto changes = edit->find("changes");
                    changes != edit->end() && changes->is_object()) {
                    for (auto it = changes->begin(); it != changes->end(); ++it) {
                        // Python: uri -> 本地路径作为 key，changes 原样（LSP 0-based range）
                        edits[platform::uri_to_path(it.key())] = it.value();
                    }
                }
                item["edits"] = std::move(edits);
            }
        }
        out_actions.push_back(std::move(item));
    }

    Json out = Json::object();
    out["ok"] = true;
    out["actions"] = std::move(out_actions);
    return out;
}

Json Bridge::RenameEditsTool(const Json& arguments) {
    const std::string new_name = arguments.value("new_name", std::string());
    // 对应 Python mcp_bridge.rename_edits 的第一句：
    //   if nav is None or not nav.valid_identifier(str(new_name)): raise ToolError(...)
    if (!server::edit::IsValidVerilogIdentifier(new_name)) {
        throw ToolError("Not a valid Verilog identifier: " + new_name);
    }

    const project::Project& proj = ProjectFor(arguments);
    const std::string path = ResolveFile(arguments.value("file", std::string()), proj);
    int line = 1;
    int column = 1;
    if (const auto it = arguments.find("line"); it != arguments.end() && it->is_number_integer()) {
        line = it->get<int>();
    }
    if (const auto it = arguments.find("column"); it != arguments.end() && it->is_number_integer()) {
        column = it->get<int>();
    }

    const Json renamed = FeatureServer().FeatureRename(path, std::max(0, line - 1),
                                                       std::max(0, column - 1), new_name);
    // Python: if not result: return {"ok": True, "changes": {}}
    Json changes = Json::object();
    if (renamed.is_object() && renamed.contains("changes") && renamed["changes"].is_object()) {
        for (auto it = renamed["changes"].begin(); it != renamed["changes"].end(); ++it) {
            Json edits = Json::array();
            if (it.value().is_array()) {
                for (const Json& edit : it.value()) {
                    Json item = Json::object();
                    // Python 的 rename_edits 把 range 转成 **1-based**（与 code_actions 不同！）
                    item["range"] = OneBasedRange(edit.value("range", Json::object()));
                    item["newText"] = edit.value("newText", std::string());
                    edits.push_back(std::move(item));
                }
            }
            changes[platform::uri_to_path(it.key())] = std::move(edits);
        }
    }

    Json out = Json::object();
    out["ok"] = true;
    out["changes"] = std::move(changes);
    return out;
}
}  // namespace fpgalsp::mcp
