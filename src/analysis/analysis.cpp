// =============================================================================
//  src/analysis/analysis.cpp —— DRC/CDC 分析 + project_update 的移植
// =============================================================================
//  参考实现逐段对照：
//    analysis.py:22-37   _runs_dir / newest_routed_db   -> NewestRoutedDb
//    analysis.py:40-49   cache_dir / cached             -> CacheDir / Cached
//    analysis.py:52-128  run                            -> Run
//    analysis.py:131-135 _first_error                   -> FirstError
//    reports.py:598-624  parse_drc                      -> ParseDrc
//    reports.py:627-645  parse_cdc                      -> ParseCdc
//    server.py:2230-2243 fpgalsp.updateProject          -> UpdateProject
//
//  与 Python 的**有意差异**（都写进了 B5-REPORT.md）：
//    1) 器件导入：Python 先 load_device_info（import_device + 4 条 query）拿到 db 名，
//       再 ensure_device(db)；这里直接调 td::EnsureDevice(project, session, root)，
//       由它按候选顺序 import_device 直到成功。成功路径下最终 import 的是同一个器件库，
//       失败路径下多一次降级尝试（更宽容，不会更差）。
//    2) 异常文本：Python 是 "<类型名>: <消息>"（TdError/OSError…）；C++ 只有 TdError 与
//       其它异常两类，分别写成 "TdError: …" / "Exception: …"。
//    3) NewestRoutedDb 的 mtime 相同时按路径字典序兜底（Python 依赖 os.listdir 顺序）。
// =============================================================================

#include "analysis/analysis.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <re2/re2.h>

#include "platform/encoding.h"
#include "platform/path.h"
#include "td/td_diagnostics.h"  // EnsureDevice
#include "td/td_install.h"      // FindTd / TdChoice
#include "td/td_output.h"       // HasError
#include "td/td_session.h"      // TdSession / TdSessionOptions

namespace fpgalsp::analysis {
namespace {

namespace fs = std::filesystem;

constexpr double kImportDbTimeout = 300.0;
constexpr double kDrcTimeout = 900.0;
constexpr double kCdcTimeout = 900.0;
constexpr double kCheckVersionTimeout = 120.0;
constexpr double kOpenProjectUpdateTimeout = 180.0;

// 对应 reports.DRC_RULE_RE / CDC_COUNT_RE（逐字复制 Python 的模式）
constexpr const char* kDrcRulePattern =
    R"(^\|?\s*([A-Z]{2,6}-\d{3,5})\s*\|\s*(ERROR|CRITICAL-WARNING|WARNING|INFO)\s*\|\s*(\d+)\s*\|?\s*$)";
constexpr const char* kDrcDetailPattern = R"(^[A-Z]{2,6}-\d{3,5})";
constexpr const char* kCdcCountPattern =
    R"((Safe|Unsafe|Unknown|No\s+ASYNC_REG)\s*[:=|]?\s*(\d+))";

// Python time.time()：epoch 秒（浮点）。保留毫秒，够 agent 判断"新鲜度"。
double NowSeconds() {
    using clock = std::chrono::system_clock;
    const auto now = clock::now().time_since_epoch();
    return std::chrono::duration<double>(now).count();
}

double RoundOne(double value) { return std::round(value * 10.0) / 10.0; }

double SecondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

std::string Trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r' ||
                           text[begin] == '\n')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                           text[end - 1] == '\r' || text[end - 1] == '\n')) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

void RightTrimInPlace(std::string& text) {
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ' ||
                             text.back() == '\t')) {
        text.pop_back();
    }
}

// 对应 Python text.replace("\r\n", "\n").replace("\r", "\n").split("\n")
std::vector<std::string> SplitLinesKeepEmpty(std::string_view text) {
    std::string normalized(text);
    std::string replaced;
    replaced.reserve(normalized.size());
    for (std::size_t index = 0; index < normalized.size(); ++index) {
        const char ch = normalized[index];
        if (ch == '\r') {
            replaced.push_back('\n');
            if (index + 1 < normalized.size() && normalized[index + 1] == '\n') ++index;
        } else {
            replaced.push_back(ch);
        }
    }
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = replaced.find('\n', start);
        if (pos == std::string::npos) {
            lines.push_back(replaced.substr(start));
            break;
        }
        lines.push_back(replaced.substr(start, pos - start));
        start = pos + 1;
    }
    return lines;
}

// Python str.strip("|")：只去掉**两端**的 '|'（不是 split 后的再 strip）
std::vector<std::string> SplitPipeRow(const std::string& line) {
    std::size_t begin = 0;
    std::size_t end = line.size();
    while (begin < end && line[begin] == '|') ++begin;
    while (end > begin && line[end - 1] == '|') --end;
    const std::string body = line.substr(begin, end - begin);

    std::vector<std::string> cells;
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = body.find('|', start);
        if (pos == std::string::npos) {
            cells.push_back(Trim(body.substr(start)));
            break;
        }
        cells.push_back(Trim(body.substr(start, pos - start)));
        start = pos + 1;
    }
    return cells;
}

bool AllDashOrSpace(const std::string& cell) {
    for (const char ch : cell) {
        if (ch != '-' && ch != ' ') return false;
    }
    return true;
}

bool AnyNonEmpty(const std::vector<std::string>& cells) {
    for (const std::string& cell : cells) {
        if (!cell.empty()) return true;
    }
    return false;
}

std::string ReplaceBackslashes(std::string text) {
    std::replace(text.begin(), text.end(), '\\', '/');
    return text;
}

// Python 的 dict.update：把 source 的键逐个覆盖到 target（保持 target 其余键）
void MergeInto(Json& target, const Json& source) {
    if (!target.is_object() || !source.is_object()) return;
    for (auto it = source.begin(); it != source.end(); ++it) target[it.key()] = it.value();
}

std::string ExceptionText(const std::exception& exc) {
    if (dynamic_cast<const td::TdError*>(&exc) != nullptr) {
        return std::string("TdError: ") + exc.what();
    }
    return std::string("Exception: ") + exc.what();
}

// C++20 起 u8path 已弃用；这里用 char8_t 构造，保持"UTF-8 输入 -> 原生路径"的语义
// （工程路径可能含中文，不能用 path(std::string) 的窄字符转换）。
fs::path AsPath(const std::string& utf8) {
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

bool IsRegularFile(const std::string& path) {
    std::error_code ec;
    return fs::is_regular_file(AsPath(path), ec);
}

std::string ReadTextOrEmpty(const std::string& path) {
    const std::optional<std::string> text = platform::try_read_file_utf8(path);
    return text.value_or(std::string());
}

}  // namespace

// -----------------------------------------------------------------------------
// 路径 / 缓存
// -----------------------------------------------------------------------------

std::string NewestRoutedDb(const project::Project& project) {
    if (project.dir.empty() || project.name.empty()) return std::string();
    const std::string phy = platform::normalize_path(
        project.dir + "/" + project.name + "_Runs/phy_1");

    struct Candidate {
        fs::file_time_type mtime;
        std::string path;
    };
    std::vector<Candidate> candidates;
    std::error_code ec;
    fs::directory_iterator iterator(AsPath(phy), ec);
    if (ec) return std::string();
    for (const fs::directory_entry& entry : iterator) {
        const std::string name = entry.path().filename().string();
        // Python: name.endswith(("_pr.db", "_route.db")) —— "_pr.db" 是 6 个字符
        const bool ends_pr = name.size() >= 6 && name.compare(name.size() - 6, 6, "_pr.db") == 0;
        const bool ends_route =
            name.size() >= 9 && name.compare(name.size() - 9, 9, "_route.db") == 0;
        if (!ends_pr && !ends_route) continue;
        std::error_code type_ec;
        if (!entry.is_regular_file(type_ec) || type_ec) continue;
        std::error_code time_ec;
        const fs::file_time_type mtime = entry.last_write_time(time_ec);
        if (time_ec) continue;
        candidates.push_back({mtime, entry.path().generic_string()});
    }
    if (candidates.empty()) return std::string();
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
        if (left.mtime != right.mtime) return left.mtime > right.mtime;  // 最新在前
        return left.path < right.path;                                    // 同 mtime：稳定兜底
    });
    return candidates.front().path;
}

std::string CacheDir(const project::Project& project) {
    if (project.dir.empty()) return std::string();
    return platform::path_join(platform::state_dir(project.dir), "cache");
}

std::optional<Json> Cached(const project::Project& project) {
    const std::string cache = CacheDir(project);
    if (cache.empty()) return std::nullopt;
    const std::string path = cache + "/analysis.json";
    if (!IsRegularFile(path)) return std::nullopt;
    const std::string text = ReadTextOrEmpty(path);
    if (text.empty()) return std::nullopt;
    Json parsed = Json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) return std::nullopt;
    return parsed;
}

// -----------------------------------------------------------------------------
// 纯解析
// -----------------------------------------------------------------------------

Json ParseDrc(std::string_view text) {
    const RE2 rule_re(kDrcRulePattern);
    const RE2 detail_re(kDrcDetailPattern);

    Json rules = Json::array();
    Json details = Json::array();
    std::string section;

    for (std::string line : SplitLinesKeepEmpty(text)) {
        RightTrimInPlace(line);
        const std::string stripped = Trim(line);
        if (stripped.empty()) continue;

        std::string upper = stripped;
        std::transform(upper.begin(), upper.end(), upper.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
        if (upper.rfind("REPORT SUMMARY", 0) == 0) {
            section = "summary";
            continue;
        }
        if (upper.rfind("REPORT DETAILS", 0) == 0) {
            section = "details";
            continue;
        }

        if (section == "summary") {
            std::string rule;
            std::string severity;
            std::string count;
            if (RE2::FullMatch(stripped, rule_re, &rule, &severity, &count)) {
                Json item = Json::object();
                item["rule"] = rule;
                item["severity"] = severity;
                item["count"] = std::atoi(count.c_str());
                rules.push_back(std::move(item));
            }
        } else if (section == "details") {
            // Python：第一条明细开始后，后续每个非空行都收（不再要求匹配规则号前缀）
            if (RE2::PartialMatch(stripped, detail_re) || !details.empty()) {
                details.push_back(stripped.substr(0, 300));
            }
        }
    }

    int total = 0;
    for (const Json& item : rules) total += item.value("count", 0);

    Json out = Json::object();
    out["violations"] = total;
    out["rules"] = std::move(rules);
    if (details.size() > 200) {
        Json capped = Json::array();
        for (std::size_t index = 0; index < 200; ++index) capped.push_back(details[index]);
        details = std::move(capped);
    }
    out["details"] = std::move(details);
    out["clean"] = total == 0;
    return out;
}

Json ParseCdc(std::string_view text) {
    const RE2 count_re(kCdcCountPattern);

    Json rows = Json::array();
    for (std::string line : SplitLinesKeepEmpty(text)) {
        const std::string stripped = Trim(line);
        if (stripped.empty()) continue;
        if (stripped.rfind("---", 0) == 0) continue;
        const std::size_t pipes = static_cast<std::size_t>(
            std::count(stripped.begin(), stripped.end(), '|'));
        if (pipes < 3) continue;

        std::vector<std::string> cells = SplitPipeRow(stripped);
        if (!AnyNonEmpty(cells)) continue;
        bool all_dash = true;
        for (const std::string& cell : cells) {
            if (!AllDashOrSpace(cell)) {
                all_dash = false;
                break;
            }
        }
        if (all_dash) continue;

        if (rows.size() < 200) {
            Json row = Json::array();
            for (const std::string& cell : cells) row.push_back(cell);
            rows.push_back(std::move(row));
        }
    }

    // counts：对**整段文本**做 finditer（Python 是 re.finditer(text)，不是逐行）
    Json counts = Json::object();
    re2::StringPiece input(text.data(), text.size());
    std::string label;
    std::string number;
    while (RE2::FindAndConsume(&input, count_re, &label, &number)) {
        std::string key;
        key.reserve(label.size());
        bool previous_space = false;
        for (const char ch : label) {
            const bool is_space = ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' ||
                                  ch == '\f' || ch == '\v';
            if (is_space) {
                if (!previous_space) key.push_back('_');
                previous_space = true;
            } else {
                key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
                previous_space = false;
            }
        }
        const int value = std::atoi(number.c_str());
        const int previous = counts.contains(key) ? counts[key].get<int>() : 0;
        counts[key] = previous + value;
    }

    const int unsafe_count = counts.contains("unsafe") ? counts["unsafe"].get<int>() : 0;
    const int unknown_count = counts.contains("unknown") ? counts["unknown"].get<int>() : 0;

    Json out = Json::object();
    out["rows"] = std::move(rows);
    out["counts"] = std::move(counts);
    out["clean"] = unsafe_count == 0 && unknown_count == 0;
    return out;
}

std::string FirstError(std::string_view text) {
    for (const std::string& line : SplitLinesKeepEmpty(text)) {
        if (line.find("ERROR") != std::string::npos ||
            line.find("CRITICAL-WARNING") != std::string::npos) {
            return Trim(line);
        }
    }
    return std::string();
}

std::string CdcUnsupportedReason(std::string_view device) {
    return "Device (" + std::string(device) + ") does not support check_cdc reports";
}

bool CdcUnsupportedByDevice(std::string_view output) {
    return output.find("does not support") != std::string_view::npos;
}

std::string TailLines(std::string_view text, std::size_t count) {
    if (count == 0) return std::string();
    std::vector<std::string> lines = SplitLinesKeepEmpty(text);
    // Python str.splitlines()：文本以换行结尾时不会产生末尾空串
    if (!lines.empty() && lines.back().empty() &&
        !text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        lines.pop_back();
    }
    if (lines.empty()) return std::string();
    const std::size_t begin = lines.size() > count ? lines.size() - count : 0;
    std::string out;
    for (std::size_t index = begin; index < lines.size(); ++index) {
        if (index != begin) out.push_back('\n');
        out += lines[index];
    }
    return out;
}

// -----------------------------------------------------------------------------
// 入口：DRC / CDC
// -----------------------------------------------------------------------------

Json Run(const project::Project& project, std::string_view td_explicit, const LogFn& log) {
    const auto log_message = [&log](const std::string& message) {
        if (log) log(message);
    };

    Json result = Json::object();
    result["ok"] = false;
    result["generatedAt"] = NowSeconds();
    result["device"] = project.device;
    {
        Json drc = Json::object();
        drc["supported"] = true;
        drc["violations"] = 0;
        drc["rules"] = Json::array();
        drc["details"] = Json::array();
        result["drc"] = std::move(drc);
    }
    {
        Json cdc = Json::object();
        cdc["supported"] = true;
        cdc["rows"] = Json::array();
        cdc["counts"] = Json::object();
        result["cdc"] = std::move(cdc);
    }
    result["files"] = Json::object();
    result["error"] = "";

    const std::string db = NewestRoutedDb(project);
    if (db.empty()) {
        result["error"] = "No placed/routed result yet (run Place & Route / Full Flow first)";
        return result;
    }
    result["db"] = db;

    const std::string cache = CacheDir(project);
    const std::string out_dir = platform::normalize_path(cache + "/analysis");
    platform::ensure_directory(out_dir);
    const std::string drc_path = platform::normalize_path(out_dir + "/drc.rpt");
    const std::string cdc_path = platform::normalize_path(out_dir + "/cdc.rpt");
    for (const std::string& stale : {drc_path, cdc_path}) {
        std::error_code ec;
        fs::remove(AsPath(stale), ec);
    }

    // Python：run_dir = <project>/.fpgalsp/analysis（P-4 行为保持）
    const std::string run_dir = platform::path_join(platform::state_dir(project.dir), "analysis");

    try {
        const std::optional<td::TdChoice> choice = td::FindTd(td_explicit);
        if (!choice.has_value()) {
            result["error"] = "No TangDynasty installation found";
            return result;
        }
        td::TdSessionOptions options;
        options.work_dir = run_dir;
        td::TdSession session(*choice, options);

        const td::EnsureDeviceResult ensured = td::EnsureDevice(project, session, choice->root);
        if (!ensured.ok) {
            result["error"] = "import_device failed: " + ensured.error;
            return result;
        }

        std::string out = session.Run("import_db {" + db + "}", kImportDbTimeout);
        if (td::HasError(out)) {
            result["error"] = "import_db failed: " + FirstError(out);
            session.Close();
            return result;
        }
        result["ok"] = true;

        // ---- DRC ----（对应 analysis.py:85-99）
        std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
        out = session.Run("report_drc -name fpgalsp_drc -file {" + drc_path + "}", kDrcTimeout);
        result["drc"]["elapsed"] = RoundOne(SecondsSince(started));
        if (out.find("ERROR") != std::string::npos && out.find("report_drc") != std::string::npos) {
            result["drc"]["supported"] = false;
            result["drc"]["reason"] = FirstError(out);
        } else if (IsRegularFile(drc_path)) {
            MergeInto(result["drc"], ParseDrc(ReadTextOrEmpty(drc_path)));
            result["files"]["drc"] = drc_path;
        } else {
            result["drc"]["violations"] = 0;
            result["drc"]["clean"] = true;
            const std::string first_error = FirstError(out);
            if (!first_error.empty()) result["drc"]["note"] = first_error;
        }

        // ---- CDC ----（对应 analysis.py:101-114）
        started = std::chrono::steady_clock::now();
        out = session.Run("check_cdc -file {" + cdc_path + "}", kCdcTimeout);
        result["cdc"]["elapsed"] = RoundOne(SecondsSince(started));
        if (CdcUnsupportedByDevice(out)) {
            Json cdc = Json::object();
            cdc["supported"] = false;
            cdc["reason"] = CdcUnsupportedReason(project.device);
            result["cdc"] = std::move(cdc);
        } else if (td::HasError(out)) {
            Json cdc = Json::object();
            cdc["supported"] = false;
            cdc["reason"] = FirstError(out);
            result["cdc"] = std::move(cdc);
        } else if (IsRegularFile(cdc_path)) {
            MergeInto(result["cdc"], ParseCdc(ReadTextOrEmpty(cdc_path)));
            result["files"]["cdc"] = cdc_path;
        } else {
            Json cdc = Json::object();
            cdc["supported"] = true;
            cdc["clean"] = true;
            cdc["rows"] = Json::array();
            cdc["counts"] = Json::object();
            result["cdc"] = std::move(cdc);
        }

        session.Close();
    } catch (const std::exception& exc) {
        result["ok"] = false;
        result["error"] = ExceptionText(exc);
        log_message(std::string("analysis failed: ") + exc.what());
        return result;  // Python 在异常分支也不写缓存
    }

    // 只有完整跑完才写缓存（与 Python 一致：提前 return 的分支不落盘）
    if (!cache.empty()) {
        platform::ensure_directory(cache);
        platform::write_file_utf8(cache + "/analysis.json",
                                  result.dump(-1, ' ', /*ensure_ascii=*/false,
                                              Json::error_handler_t::replace));
    }
    {
        const int violations = result["drc"].value("violations", 0);
        const bool cdc_supported = result["cdc"].value("supported", false);
        log_message("analysis: drc=" + std::to_string(violations) +
                    " cdc=" + (cdc_supported ? "true" : "false"));
    }
    return result;
}

// -----------------------------------------------------------------------------
// 入口：updateProject
// -----------------------------------------------------------------------------

Json UpdateProject(const project::Project& project, std::string_view td_explicit, const LogFn& log) {
    const auto log_message = [&log](const std::string& message) {
        if (log) log(message);
    };

    const std::string al = ReplaceBackslashes(project.al_path);
    try {
        const std::optional<td::TdChoice> choice = td::FindTd(td_explicit);
        if (!choice.has_value()) {
            Json failed = Json::object();
            failed["ok"] = false;
            failed["error"] = "No TangDynasty installation found";
            return failed;
        }
        // Python 用的是服务器常驻会话，其 cwd = <project>/.fpgalsp/run（server.py:_run_dir）
        td::TdSessionOptions options;
        options.work_dir = platform::path_join(platform::state_dir(project.dir), "run");
        td::TdSession session(*choice, options);

        std::string output = session.Run("check_project_version -file {" + al + "}",
                                         kCheckVersionTimeout);
        output += "\n";
        output += session.Run("open_project {" + al + "} -update", kOpenProjectUpdateTimeout);
        session.Close();

        Json result = Json::object();
        result["ok"] = !td::HasError(output);
        result["al"] = al;
        result["output"] = TailLines(output, 30);
        log_message(std::string("updateProject: ok=") + (result["ok"].get<bool>() ? "true" : "false"));
        return result;
    } catch (const std::exception& exc) {
        Json failed = Json::object();
        failed["ok"] = false;
        failed["error"] = exc.what();
        log_message(std::string("updateProject failed: ") + exc.what());
        return failed;
    }
}

}  // namespace fpgalsp::analysis
