// =============================================================================
//  src/td/td_device.cpp
// =============================================================================

#include "td/td_device.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "td/td_output.h"

namespace fpgalsp::td {
namespace {

std::string ToUpper(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

std::string ToLower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string Strip(std::string_view s) {
    std::size_t b = 0, e = s.size();
    auto sp = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
    };
    while (b < e && sp(s[b])) ++b;
    while (e > b && sp(s[e - 1])) --e;
    return std::string(s.substr(b, e - b));
}

std::optional<int> IntOrNone(std::string_view value) {
    const std::string s = Strip(value);
    if (s.empty()) return std::nullopt;
    try {
        std::size_t consumed = 0;
        const long long v = std::stoll(s, &consumed);
        if (consumed != s.size()) return std::nullopt;
        if (v < -2147483648LL || v > 2147483647LL) return std::nullopt;
        return static_cast<int>(v);
    } catch (...) {
        return std::nullopt;
    }
}

// 对应 Python re.fullmatch(r"\d+", cells[0] or "")
bool IsAllDigits(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

std::vector<std::string> Split(std::string_view s, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        const std::size_t p = s.find(sep, start);
        if (p == std::string_view::npos) {
            out.emplace_back(s.substr(start));
            break;
        }
        out.emplace_back(s.substr(start, p - start));
        start = p + 1;
    }
    return out;
}

// 对应 Python re.findall(r"[Pp]\d+", text)
std::vector<std::string> FindPins(std::string_view text) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != 'P' && text[i] != 'p') continue;
        std::size_t j = i + 1;
        const std::size_t digits_start = j;
        while (j < text.size() && text[j] >= '0' && text[j] <= '9') ++j;
        if (j > digits_start) {
            out.emplace_back(text.substr(i, j - i));
            i = j - 1;
        }
    }
    return out;
}

}  // namespace

// -----------------------------------------------------------------------------
// DB_PREFIXES
// -----------------------------------------------------------------------------

std::vector<std::string> DbPrefixesForFamily(std::string_view family) {
    const std::string fam = ToUpper(family);
    // 逐字对应 Python 的 DB_PREFIXES
    if (fam == "EF2") return {"ef2", "ef3", "ef4"};
    if (fam == "EF3") return {"ef3", "ef4", "ef2"};
    if (fam == "EF4") return {"ef4", "ef3"};
    if (fam == "EF5") return {"ef5", "ef4"};
    if (fam == "EG4") return {"eagle", "eg4"};
    if (fam == "PH1") return {"ph1"};
    if (fam == "PH1P") return {"ph1p", "ph1"};
    if (fam == "PH2") return {"ph2"};
    if (fam == "DR1") return {"dr1"};
    if (fam == "SF1") return {"sf1"};
    if (fam == "SF2") return {"sf2"};
    return {};  // Python: DB_PREFIXES.get(fam, ())
}

std::vector<std::string> DbCandidates(std::string_view td_root, std::string_view family) {
    std::vector<std::string> dbs;
    try {
        const std::filesystem::path arch{std::string(td_root) + "/arch"};
        if (!std::filesystem::is_directory(arch)) return {};
        for (const auto& entry : std::filesystem::directory_iterator(arch)) {
            const std::string name = entry.path().filename().string();
            if (ToLower(name).size() > 3 &&
                ToLower(name).compare(ToLower(name).size() - 3, 3, ".db") == 0) {
                dbs.push_back(name);
            }
        }
    } catch (...) {
        return {};
    }

    const auto prefixes = DbPrefixesForFamily(family);
    std::vector<std::string> ordered;
    std::set<std::string> seen;

    auto push = [&](const std::string& name) {
        if (!name.empty() && seen.find(name) == seen.end()) {
            seen.insert(name);
            ordered.push_back(name);
        }
    };

    // 第一步：按前缀优先级挑出「<prefix>_ 开头」的
    for (const auto& prefix : prefixes) {
        for (const auto& db : dbs) {
            if (db.size() > prefix.size() + 1 &&
                ToLower(db).compare(0, prefix.size(), prefix) == 0 && db[prefix.size()] == '_') {
                push(db);
            }
        }
    }
    // 第二步：其余全部按字典序补上（对应 Python: for db in sorted(dbs): push(db)）
    std::vector<std::string> sorted = dbs;
    std::sort(sorted.begin(), sorted.end());
    for (const auto& db : sorted) push(db);

    return ordered;
}

// -----------------------------------------------------------------------------
// 表格解析
// -----------------------------------------------------------------------------

std::vector<std::vector<std::string>> ParsePipeTable(const std::vector<std::string>& lines) {
    // 对应 Python:
    //   ^[A-Z]{2,6}-\d{3,5}\s*:   -> 首单元取冒号后的部分
    //   ^[A-Z]{2,6}-\d{3,5}$      -> 整个首单元丢掉
    static const std::regex kPrefixColon(R"(^([A-Z]{2,6}\-\d{3,5})\s*:)");
    static const std::regex kPrefixOnly(R"(^[A-Z]{2,6}\-\d{3,5}$)");

    std::vector<std::vector<std::string>> rows;
    for (const auto& raw : lines) {
        const std::string line = Strip(raw);
        if (line.find('|') == std::string::npos) continue;

        // 至少 2 个 '|'
        if (std::count(line.begin(), line.end(), '|') < 2) continue;

        std::vector<std::string> cells;
        for (auto& c : Split(line, '|')) cells.push_back(Strip(c));
        if (cells.empty()) continue;

        std::smatch m;
        if (std::regex_search(cells[0], m, kPrefixColon) && m.position(0) == 0) {
            // 去掉 "RUN-1001 :" 前缀
            const std::size_t colon = cells[0].find(':');
            cells[0] = Strip(cells[0].substr(colon + 1));
        } else if (std::regex_match(cells[0], kPrefixOnly)) {
            cells.erase(cells.begin());
        }

        rows.push_back(std::move(cells));
    }
    return rows;
}

std::vector<std::vector<std::string>> ParsePipeTableText(std::string_view text) {
    return ParsePipeTable(CleanLines(text));
}

// -----------------------------------------------------------------------------
// 四种查询解析
// -----------------------------------------------------------------------------

std::map<std::string, Pin> ParseFio(std::string_view text) {
    std::map<std::string, Pin> pins;

    for (const auto& cells : ParsePipeTableText(text)) {
        if (cells.size() < 6) continue;
        if (!IsAllDigits(cells[0])) continue;

        const std::string pe = Strip(cells[1]);
        if (pe.empty()) continue;

        Pin pin;
        pin.pe = ToUpper(pe);
        pin.pe_raw = pe;
        pin.location = Strip(cells[2]);
        pin.type = Strip(cells[3]);
        pin.int_bank = IntOrNone(cells[4]);
        pin.ext_bank = IntOrNone(cells[5]);

        pins[ToLower(pe)] = std::move(pin);
    }
    return pins;
}

void ParseClockIo(std::string_view text, std::map<std::string, Pin>& pins) {
    for (const auto& cells : ParsePipeTableText(text)) {
        if (cells.size() < 4) continue;
        if (!IsAllDigits(cells[0])) continue;

        const std::string name = Strip(cells[2]);
        if (name.empty()) continue;

        auto it = pins.find(ToLower(name));
        if (it != pins.end()) {
            it->second.clock_capable = true;
            it->second.clock_kind = Strip(cells[3]);
        }
    }
}

void ParseLvds(std::string_view text, std::map<std::string, Pin>& pins) {
    for (const auto& cells : ParsePipeTableText(text)) {
        if (cells.size() < 5) continue;
        if (!IsAllDigits(cells[0])) continue;

        for (const auto& name : FindPins(cells[4])) {
            auto it = pins.find(ToLower(name));
            if (it != pins.end()) {
                it->second.lvds = true;
                it->second.lvds_type = Strip(cells[1]);
            }
        }
    }
}

std::vector<DedicatedPin> ParseCfgIo(std::string_view text) {
    std::vector<DedicatedPin> dedicated;
    for (const auto& cells : ParsePipeTableText(text)) {
        if (cells.size() < 3) continue;

        const std::string option = Strip(cells[0]);
        const std::string ios = Strip(cells[1]);
        const std::string setting = Strip(cells[2]);

        if (ToLower(setting).find("dedicate") == std::string::npos) continue;

        for (const auto& pe : Split(ios, '/')) {
            const std::string cleaned = Strip(pe);
            if (!cleaned.empty()) {
                dedicated.push_back({option, cleaned});
            }
        }
    }
    return dedicated;
}

DeviceInfo BuildDeviceInfo(std::string_view fio_text, std::string_view clock_text,
                           std::string_view lvds_text, std::string_view cfg_text) {
    DeviceInfo info;
    info.pins = ParseFio(fio_text);
    ParseClockIo(clock_text, info.pins);
    ParseLvds(lvds_text, info.pins);
    info.dedicated = ParseCfgIo(cfg_text);
    return info;
}

// -----------------------------------------------------------------------------
// 命令行构造
// -----------------------------------------------------------------------------

std::string SpeedArgSuffix(const std::optional<std::string>& speed) {
    if (!speed.has_value() || speed->empty()) return {};
    // 对应 Python: " -speed %s" % str(project.speed).lstrip("+-")
    std::string value = *speed;
    std::size_t i = 0;
    while (i < value.size() && (value[i] == '+' || value[i] == '-')) ++i;
    value = value.substr(i);
    if (value.empty()) return {};
    return " -speed " + value;
}

std::string MakeImportDeviceCommand(std::string_view db_file, std::string_view package,
                                    const std::optional<std::string>& speed) {
    return "import_device " + std::string(db_file) + " -package " + std::string(package) +
           SpeedArgSuffix(speed);
}

std::string DeviceCacheKey(std::string_view device, std::string_view family,
                           const std::optional<std::string>& speed) {
    // 对应 Python:
    //   re.sub(r"[^A-Za-z0-9_.-]+", "_", "%s_%s_%s" % (device, family, speed or "na"))
    std::string raw(device);
    raw += "_";
    raw += std::string(family);
    raw += "_";
    raw += (speed.has_value() && !speed->empty()) ? *speed : std::string("na");

    std::string safe;
    bool last_was_underscore = false;
    for (char c : raw) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
        if (ok) {
            safe.push_back(c);
            last_was_underscore = (c == '_');
        } else if (!last_was_underscore) {
            safe.push_back('_');
            last_was_underscore = true;
        }
    }
    return "device_" + safe + ".json";
}

}  // namespace fpgalsp::td