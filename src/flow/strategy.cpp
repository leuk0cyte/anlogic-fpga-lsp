// =============================================================================
//  src/flow/strategy.cpp —— 多策略对比
// =============================================================================

#include "flow/strategy.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <re2/re2.h>

#include "flow/flow_logic.h"
#include "platform/encoding.h"
#include "reports/qor.h"
#include "reports/report_text.h"
#include "td/td_device.h"
#include "td/td_output.h"
#include "td/td_session.h"
#include "td/tcl_quote.h"

namespace fpgalsp::flow {
namespace {

namespace fs = std::filesystem;

std::string Slashes(std::string text) {
    for (char& c : text) {
        if (c == '\\') c = '/';
    }
    return text;
}

std::string Join(std::string_view dir, std::string_view name) {
    if (dir.empty()) return std::string(name);
    std::string out(dir);
    if (out.back() != '/' && out.back() != '\\') out.push_back('/');
    out += name;
    return Slashes(std::move(out));
}

bool IsFile(std::string_view path) {
    if (path.empty()) return false;
    std::error_code ec;
    return fs::is_regular_file(fs::path(std::string(path)), ec) && !ec;
}

bool IsDirectory(std::string_view path) {
    if (path.empty()) return false;
    std::error_code ec;
    return fs::is_directory(fs::path(std::string(path)), ec) && !ec;
}

double Now() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration<double>(now).count();
}

std::string Upper(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

// 对应 Python shutil.ignore_patterns(".fpgalsp","log","*.log","*_Runs","*.dmp")
bool ShouldSkip(const std::string& name, bool is_directory) {
    if (is_directory && (name == ".fpgalsp" || name == "log")) return true;
    if (name.size() >= 6 && name.compare(name.size() - 6, 6, "_Runs") == 0) return true;
    const auto ends_with = [&name](std::string_view suffix) {
        return name.size() >= suffix.size() &&
               name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    return ends_with(".log") || ends_with(".dmp");
}

void CopyProjectTree(const std::string& from, const std::string& to, std::error_code& ec) {
    fs::create_directories(fs::path(to), ec);
    if (ec) return;
    fs::directory_iterator it(fs::path(from), ec);
    if (ec) return;
    const fs::directory_iterator end;
    while (it != end) {
        std::error_code status_ec;
        const bool is_dir = fs::is_directory(it->path(), status_ec);
        const std::string name = it->path().filename().string();
        if (!ShouldSkip(name, is_dir)) {
            const std::string target = Join(to, name);
            if (is_dir) {
                CopyProjectTree(it->path().string(), target, ec);
                if (ec) return;
            } else {
                std::error_code copy_ec;
                fs::copy_file(it->path(), fs::path(target),
                              fs::copy_options::overwrite_existing, copy_ec);
            }
        }
        it.increment(ec);
        if (ec) return;
    }
}

std::optional<int64_t> MetricInt(const jsonrpc::Json& metrics_object, std::string_view key) {
    if (!metrics_object.is_object()) return std::nullopt;
    const auto it = metrics_object.find(std::string(key));
    if (it == metrics_object.end()) return std::nullopt;
    const std::string raw = it->is_string() ? it->get<std::string>() : it->dump();
    std::string digits;
    for (const char c : raw) {
        if (c == ',') continue;  // Python 会去掉千分位逗号
        digits.push_back(c);
    }
    int64_t value = 0;
    if (!reports::ParseInt(digits, value)) return std::nullopt;
    return value;
}

std::string MetricString(const jsonrpc::Json& metrics_object, std::string_view key) {
    if (!metrics_object.is_object()) return {};
    const auto it = metrics_object.find(std::string(key));
    if (it == metrics_object.end()) return {};
    return it->is_string() ? it->get<std::string>() : it->dump();
}

// 把 [{group,key,value}] 折成 {key: value}（后者覆盖前者，与 Python dict 一致）
jsonrpc::Json MetricsObject(const jsonrpc::Json& qor) {
    jsonrpc::Json metrics = jsonrpc::Json::object();
    const auto list = qor.find("metrics");
    if (list == qor.end() || !list->is_array()) return metrics;
    for (const auto& item : *list) {
        if (!item.is_object()) continue;
        const auto key = item.find("key");
        const auto value = item.find("value");
        if (key == item.end() || value == item.end()) continue;
        metrics[key->dump()] = *value;
    }
    // key->dump() 会带引号，重建一次干净的键
    jsonrpc::Json clean = jsonrpc::Json::object();
    for (const auto& item : *list) {
        if (!item.is_object()) continue;
        const auto key = item.find("key");
        const auto value = item.find("value");
        if (key == item.end() || value == item.end()) continue;
        if (!key->is_string()) continue;
        clean[key->get<std::string>()] = *value;
    }
    return clean;
}

void PruneWorkDirs(std::string_view root, int keep) {
    if (keep <= 0) return;
    std::vector<std::string> names;
    std::error_code ec;
    fs::directory_iterator it(fs::path(std::string(root)), ec);
    if (ec) return;
    const fs::directory_iterator end;
    while (it != end) {
        std::error_code status_ec;
        if (fs::is_directory(it->path(), status_ec) && !status_ec) {
            names.push_back(it->path().filename().string());
        }
        it.increment(ec);
        if (ec) break;
    }
    std::sort(names.begin(), names.end());  // 目录名是时间戳，字典序即时间序
    while (static_cast<int>(names.size()) > keep) {
        std::error_code remove_ec;
        fs::remove_all(fs::path(Join(root, names.front())), remove_ec);
        names.erase(names.begin());
    }
}

}  // namespace

const std::vector<std::string>& DefaultStrategies() {
    static const std::vector<std::string> kDefaults = {"TimingHigh", "CongestionHigh",
                                                       "AreaOptimize"};
    return kDefaults;
}

std::string StrategyFileForFamily(std::string_view family) {
    const std::string upper = Upper(family);
    if (upper == "PH1" || upper == "PH1P") return "official_strategy_ph1.json";
    if (upper == "PH2" || upper == "PH2A") return "official_strategy_ph2.json";
    return "official_strategy_cpld.json";
}

std::vector<std::string> AvailableStrategies(std::string_view td_root, std::string_view family) {
    std::vector<std::string> names;
    if (td_root.empty()) return names;
    const std::string path = Join(Join(Join(td_root, "doc"), "strategies"),
                                  StrategyFileForFamily(family));
    const std::string text = reports::ReadReportFile(path);
    if (text.empty()) return names;

    const jsonrpc::Json parsed = jsonrpc::Json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!parsed.is_object()) return names;
    for (auto it = parsed.begin(); it != parsed.end(); ++it) names.push_back(it.key());
    std::sort(names.begin(), names.end());
    return names;
}

jsonrpc::Json QorNumbers::ToJson() const {
    jsonrpc::Json out = jsonrpc::Json::object();
    out["setup_wns_ps"] = setup_wns_ps.has_value() ? jsonrpc::Json(*setup_wns_ps)
                                                   : jsonrpc::Json(nullptr);
    out["setup_tns_ps"] = setup_tns_ps.has_value() ? jsonrpc::Json(*setup_tns_ps)
                                                   : jsonrpc::Json(nullptr);
    out["hold_wns_ps"] = hold_wns_ps.has_value() ? jsonrpc::Json(*hold_wns_ps)
                                                 : jsonrpc::Json(nullptr);
    out["luts"] = luts.has_value() ? jsonrpc::Json(*luts) : jsonrpc::Json(nullptr);
    out["registers"] = registers.has_value() ? jsonrpc::Json(*registers) : jsonrpc::Json(nullptr);
    out["slices"] = slices.has_value() ? jsonrpc::Json(*slices) : jsonrpc::Json(nullptr);
    out["slice_pct"] = slice_pct;
    return out;
}

QorNumbers QorNumbersFromReport(const jsonrpc::Json& qor) {
    const jsonrpc::Json metrics = MetricsObject(qor);
    QorNumbers numbers;
    numbers.setup_wns_ps = MetricInt(metrics, "Setup WNS");
    numbers.setup_tns_ps = MetricInt(metrics, "Setup TNS");
    numbers.hold_wns_ps = MetricInt(metrics, "Hold WNS");
    // EF2 打 "#luts"，PH1 打 "#lut"：两个都试，优先 "#luts"（与 Python 一致）
    numbers.luts = MetricInt(metrics, "#luts");
    if (!numbers.luts.has_value()) numbers.luts = MetricInt(metrics, "#lut");
    numbers.registers = MetricInt(metrics, "#registers");
    numbers.slices = MetricInt(metrics, "#slices");
    numbers.slice_pct = MetricString(metrics, "slices percentage");
    if (numbers.slice_pct.empty()) numbers.slice_pct = MetricString(metrics, "slice percentage");
    return numbers;
}

std::vector<std::string> ChooseStrategies(const std::vector<std::string>& requested,
                                          const std::vector<std::string>& available,
                                          std::vector<std::string>* unknown) {
    if (unknown != nullptr) unknown->clear();
    std::vector<std::string> wanted = requested;
    if (wanted.empty()) {
        for (const std::string& name : DefaultStrategies()) {
            if (available.empty()) continue;
            if (std::find(available.begin(), available.end(), name) != available.end()) {
                wanted.push_back(name);
            }
        }
        if (wanted.empty()) {
            wanted.assign(available.begin(),
                          available.begin() + std::min<std::size_t>(2, available.size()));
        }
    }
    if (unknown != nullptr && !available.empty()) {
        for (const std::string& name : wanted) {
            if (std::find(available.begin(), available.end(), name) == available.end()) {
                unknown->push_back(name);
            }
        }
    }
    return wanted;
}

std::string ConstraintSetOf(std::string_view al_path) {
    const std::string text = reports::ReadReportFile(al_path);
    if (text.empty()) return "constraint_1";
    // 注意用自定义分隔符 R"re(...)re"：模式里含 `)"`，默认分隔符会提前结束原始字符串
    static const RE2 kFileSetRe(R"re(<FileSet\s+Name="([^"]+)"\s+Type="ConstrainFiles")re");
    std::string name;
    if (RE2::PartialMatch(text, kFileSetRe, &name)) return name;
    return "constraint_1";
}

jsonrpc::Json CompareStrategies(const project::Project& project, const td::TdChoice& td,
                                const std::vector<std::string>& requested,
                                const CompareOptions& options) {
    jsonrpc::Json result = jsonrpc::Json::object();
    result["ok"] = false;
    result["runs"] = jsonrpc::Json::array();
    result["elapsed"] = 0.0;
    result["generatedAt"] = Now();

    const std::vector<std::string> available = AvailableStrategies(td.root, project.family);
    result["available"] = available;

    std::vector<std::string> unknown;
    const std::vector<std::string> wanted = ChooseStrategies(requested, available, &unknown);
    result["strategies"] = wanted;

    if (!unknown.empty()) {
        std::string unknown_list;
        for (const std::string& name : unknown) {
            if (!unknown_list.empty()) unknown_list += ", ";
            unknown_list += name;
        }
        std::string available_list;
        for (std::size_t i = 0; i < available.size() && i < 12; ++i) {
            if (!available_list.empty()) available_list += ", ";
            available_list += available[i];
        }
        result["error"] = "TD does not provide these strategies: " + unknown_list +
                          " (available: " + available_list + ")";
        return result;
    }
    if (wanted.empty()) {
        result["error"] = "no strategies available (doc/strategies/*.json not found)";
        return result;
    }

    const std::string work_root =
        options.work_root.empty() ? Join(Join(project.dir, ".fpgalsp"), "compare")
                                  : options.work_root;
    // 子目录名 = 本地时间戳（与 Python 的 "%Y%m%d_%H%M%S" 同构，字典序即时间序）
    char stamp[32] = "run";
    {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        if (localtime_s(&local, &now) == 0) {
            (void)std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &local);
        }
    }
    const std::string stamp_dir = Join(work_root, stamp);
    const std::string work = Join(stamp_dir, "prj");
    result["work"] = Slashes(stamp_dir);

    const double started = Now();

    // ---- 复制工程到临时目录（不污染用户的 .al）----
    std::error_code ec;
    CopyProjectTree(project.dir, work, ec);
    if (ec) {
        result["error"] = "Failed to copy the project: " + ec.message();
        return result;
    }
    const std::string al_copy = Slashes(Join(work, project.name + ".al"));
    // ⚠ P-7b：TD 拿 .al 根节点的 Path 当**工程根**。不改写的话，`phy_cmp_N` 的产物
    //   会写回**用户自己的工程**目录 —— 那样"在副本上跑"就完全失去意义了。
    if (!RewriteProjectPath(al_copy, work)) {
        result["error"] = "Failed to rewrite the project Path in the copy: " + al_copy;
        return result;
    }
    // ⚠ B7 / S-01：约束集名来自副本 .al 的 <FileSet Name="...">，是不可信内容。
    //   它不是标识符以外的任何东西，非法即安全失败（不发送任何 TD 命令）。
    const std::string constraint = ConstraintSetOf(al_copy);
    if (!td::IsTclToken(constraint)) {
        result["error"] = "Constraint set name \"" + constraint +
                          "\" contains characters that cannot be passed to TD";
        return result;
    }
    for (const std::string& name : wanted) {
        if (!td::IsTclToken(name)) {
            result["error"] = "Strategy name \"" + name +
                              "\" contains characters that cannot be passed to TD";
            return result;
        }
    }

    std::vector<std::string> run_names;
    for (std::size_t i = 0; i < wanted.size(); ++i) {
        run_names.push_back("phy_cmp_" + std::to_string(i + 1));
    }

    auto session_error = [&result](const std::string& message) {
        result["error"] = message;
        result["elapsed"] = 0.0;
        return result;
    };

    // ---- 驱动 TD ----
    std::unique_ptr<FlowSession> session;
    if (options.session_factory) {
        session = options.session_factory(Join(stamp_dir, "td"));
    }
    if (!session) {
        // 真实会话：td::TdSession 适配成 FlowSession
        try {
            td::TdSessionOptions session_options;
            session_options.work_dir = Join(stamp_dir, "td");
            auto real = std::make_unique<td::TdSession>(td, session_options);
            struct Adapter final : FlowSession {
                std::unique_ptr<td::TdSession> inner;
                std::string error;
                std::string Run(std::string_view tcl, double timeout_seconds) override {
                    try {
                        return inner->Run(tcl, timeout_seconds);
                    } catch (const std::exception& exc) {
                        error = exc.what();
                        return {};
                    }
                }
                void Kill() override {
                    try {
                        inner->Kill();
                    } catch (...) {
                    }
                }
                void Close() override {
                    try {
                        inner->Close();
                    } catch (...) {
                    }
                }
                std::string LastError() const override { return error; }
            };
            auto adapter = std::make_unique<Adapter>();
            adapter->inner = std::move(real);
            session = std::move(adapter);
        } catch (const std::exception& exc) {
            return session_error(std::string("cannot start TD: ") + exc.what());
        }
    }
    if (!session) return session_error("cannot start TD session");

    const auto run = [&session](const std::string& tcl, double timeout) {
        return session->Run(tcl, timeout);
    };

    // ---- 设备导入 ----
    {
        std::string last_error;
        bool imported = false;
        for (const std::string& db : td::DbCandidates(td.root, project.family)) {
            const std::string out = run(td::MakeImportDeviceCommand(db, project.device, project.speed),
                                        options.command_timeout_seconds);
            if (!td::HasError(out)) {
                imported = true;
                break;
            }
            last_error = FirstErrorLine(out);
        }
        if (!imported) {
            session->Close();
            return session_error("import_device failed: " + last_error);
        }
    }

    run("open_project " + td::TclQuoteGrouped(al_copy), options.command_timeout_seconds);

    // ---- 基线综合 ----
    run("reset_runs { syn_1 }", 120.0);
    run("launch_runs { syn_1 } -jobs 1", options.command_timeout_seconds);
    std::string out = run("wait_run syn_1 -quiet", options.timeout_seconds);
    if (td::HasError(out)) {
        session->Close();
        return session_error("Synthesis failed: " + FirstErrorLine(out));
    }

    // ---- 建 run + 设策略 ----
    for (std::size_t i = 0; i < run_names.size(); ++i) {
        out = run("create_run -constrset " + td::TclQuote(constraint) + " -syn_run syn_1 " +
                      td::TclQuote(run_names[i]),
                  options.command_timeout_seconds);
        if (td::HasError(out)) {
            session->Close();
            return session_error("create_run " + run_names[i] + " failed: " + FirstErrorLine(out));
        }
        out = run("set_strategy -run " + td::TclQuote(run_names[i]) + " " + td::TclQuote(wanted[i]),
                  options.command_timeout_seconds);
        if (td::HasError(out)) {
            session->Close();
            return session_error("set_strategy " + wanted[i] + " failed: " + FirstErrorLine(out));
        }
    }

    std::string launch = "launch_runs {";
    for (const std::string& name : run_names) {
        launch += " " + name;
    }
    launch += " } -jobs " + std::to_string(options.jobs > 0 ? options.jobs : 1);
    out = run(launch, 600.0);
    if (td::HasError(out)) {
        session->Close();
        return session_error("launch_runs failed: " + FirstErrorLine(out));
    }

    jsonrpc::Json runs_json = jsonrpc::Json::array();
    std::vector<jsonrpc::Json> run_entries;
    for (const std::string& name : run_names) {
        out = run("wait_run " + name + " -quiet", options.timeout_seconds);
        if (td::HasError(out)) {
            jsonrpc::Json entry = jsonrpc::Json::object();
            entry["run"] = name;
            entry["ok"] = false;
            entry["error"] = FirstErrorLine(out);
            run_entries.push_back(std::move(entry));
        }
    }
    session->Close();

    // ---- 收集 QoR ----
    const std::string runs_base = Join(work, project.name + "_Runs");
    std::vector<jsonrpc::Json> collected;
    for (std::size_t i = 0; i < run_names.size(); ++i) {
        const std::string& name = run_names[i];
        std::string qor_path = Join(Join(runs_base, name), "route.qor");
        if (!IsFile(qor_path)) qor_path = Join(Join(runs_base, name), "place.qor");

        jsonrpc::Json entry = jsonrpc::Json::object();
        entry["run"] = name;
        entry["strategy"] = wanted[i];
        for (const jsonrpc::Json& existing : run_entries) {
            if (existing.value("run", std::string()) == name) {
                entry["ok"] = existing.value("ok", false);
                if (existing.contains("error")) entry["error"] = existing["error"];
            }
        }
        if (IsFile(qor_path)) {
            const reports::QorReport parsed =
                reports::ParseQor(reports::ReadReportFile(qor_path), "route");
            const jsonrpc::Json qor_json = parsed.ToJson();
            const QorNumbers numbers = QorNumbersFromReport(qor_json);
            const jsonrpc::Json numbers_json = numbers.ToJson();
            for (auto it = numbers_json.begin(); it != numbers_json.end(); ++it) {
                entry[it.key()] = it.value();
            }
            jsonrpc::Json suggestions = jsonrpc::Json::array();
            for (std::size_t s = 0; s < parsed.suggestions.size() && s < 4; ++s) {
                suggestions.push_back(parsed.suggestions[s].text);
            }
            entry["suggestions"] = std::move(suggestions);
            entry["qor_file"] = Slashes(qor_path);
            entry["ok"] = true;
        } else {
            entry["ok"] = false;
            if (!entry.contains("error")) entry["error"] = "No route.qor was generated";
        }
        collected.push_back(std::move(entry));
    }
    for (jsonrpc::Json& entry : collected) runs_json.push_back(std::move(entry));
    result["runs"] = std::move(runs_json);

    // ---- 挑最好的（setup_wns_ps 最大）----
    bool any_ok = false;
    const jsonrpc::Json* best = nullptr;
    int64_t best_wns = 0;
    for (const jsonrpc::Json& entry : result["runs"]) {
        if (!entry.value("ok", false)) continue;
        const auto wns = entry.find("setup_wns_ps");
        if (wns == entry.end() || !wns->is_number()) continue;
        any_ok = true;
        const int64_t value = wns->get<int64_t>();
        if (best == nullptr || value > best_wns) {
            best = &entry;
            best_wns = value;
        }
    }
    if (best != nullptr) {
        jsonrpc::Json best_json = jsonrpc::Json::object();
        best_json["run"] = best->value("run", std::string());
        best_json["strategy"] = best->value("strategy", std::string());
        best_json["setup_wns_ps"] = best_wns;
        if (best->contains("luts")) best_json["luts"] = (*best)["luts"];
        result["best"] = std::move(best_json);
    }
    result["ok"] = any_ok;
    result["elapsed"] = std::round((Now() - started) * 10.0) / 10.0;

    PruneWorkDirs(work_root, options.keep_work_dirs);
    return result;
}

}  // namespace fpgalsp::flow