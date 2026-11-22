// =============================================================================
//  src/td/td_device.cpp
// =============================================================================

#include "td/td_device.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "td/td_output.h"
#include "td/tcl_quote.h"

#include "platform/encoding.h"
#include "platform/never_throw.h"
#include "platform/path.h"
#include "platform/text.h"
#include "platform/write_service.h"
#include <format>
namespace fpgalsp::td {
namespace {

using platform::ToUpper;

using platform::ToLower;

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
    return std::ranges::all_of(s, [](char c) { return c >= '0' && c <= '9'; });
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

// 对应 Python `_db_candidates` 里的
//   re.search(r"(?:L|X)(\d+)|_(\d+)", project.device)
// 返回型号里的容量数字（EF2L45LG144B -> 45）；没有则空串。
std::string DeviceSizeNumber(std::string_view device) {
    const auto digits_from = [&](std::size_t start) -> std::string {
        std::size_t end = start;
        while (end < device.size() && std::isdigit(static_cast<unsigned char>(device[end]))) ++end;
        return end > start ? std::string(device.substr(start, end - start)) : std::string();
    };
    for (std::size_t i = 0; i + 1 < device.size(); ++i) {
        const char c = device[i];
        if (c == 'L' || c == 'l' || c == 'X' || c == 'x') {
            std::string number = digits_from(i + 1);
            if (!number.empty()) return number;
        }
    }
    for (std::size_t i = 0; i + 1 < device.size(); ++i) {
        if (device[i] == '_') {
            std::string number = digits_from(i + 1);
            if (!number.empty()) return number;
        }
    }
    return {};
}
std::vector<std::string> DbCandidates(std::string_view td_root, std::string_view family,
                                      std::string_view device) {
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

    // Python 在 prefix 循环里用 `sorted(dbs)`；目录遍历顺序是文件系统顺序，
    // 不排序会让"同前缀的多个 db 谁先被试"与参考实现不一致。
    std::ranges::sort(dbs);

    const auto prefixes = DbPrefixesForFamily(family);
    // Python: re.search(r"(?:L|X)(\d+)|_(\d+)", project.device) -> 型号里的容量数字
    // （EF2L45LG144B -> 45）。有该数字时，`<prefix>_<size>` 开头的 db 优先——
    // 这正是 Python `_db_candidates` 的第一层优先，C++ 之前漏了，导致同前缀有多个
    // db 时要多试几个（每次失败 import 约 15 ms）。
    const std::string size = DeviceSizeNumber(device);
    std::vector<std::string> ordered;
    std::set<std::string> seen;

    auto push = [&](const std::string& name) {
        if (!name.empty() && !seen.contains(name)) {
            seen.insert(name);
            ordered.push_back(name);
        }
    };

    for (const auto& prefix : prefixes) {
        if (!size.empty()) {
            const std::string want = std::format("{}_{}", prefix, size);
            for (const auto& db : dbs) {
                if (ToLower(db).starts_with(want)) push(db);
            }
        }
        const std::string prefix_underscore = prefix + "_";
        for (const auto& db : dbs) {
            if (ToLower(db).starts_with(prefix_underscore)) {
                push(db);
            }
        }
    }
    // 最后：其余全部按字典序补上（对应 Python: for db in sorted(dbs): push(db)）
    for (const auto& db : dbs) push(db);

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
        if (!line.contains('|')) continue;

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

        if (!ToLower(setting).contains("dedicate")) continue;

        for (const auto& pe : Split(ios, '/')) {
            const std::string cleaned = Strip(pe);
            if (!cleaned.empty()) {
                dedicated.push_back({.role=option, .pe=cleaned});
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
    // ⚠ B7 / S-01：<Speed> 来自 .al，不是可信内容。非法值（含 `;` `[` 等）宁可不传
    //   -speed，也不让它成为命令行的一部分。
    if (!IsTclToken(value)) return {};
    return " -speed " + value;
}

std::string MakeImportDeviceCommand(std::string_view db_file, std::string_view package,
                                    const std::optional<std::string>& speed) {
    // ⚠ B7 / S-01：db_file 与 package 都可能来自 .al（<HardWare><Device>）或调用方，
    //   一律经 TclQuote 变成单个 Tcl 单词。正常值（ef2_4.db / EF2L45LG144B）输出与
    //   旧实现逐字节相同（裸写），所以 TD 行为与既有单测期望不变；
    //   含 `;` `[` `$` `{` `}` 等 Tcl 元字符时会走引号/转义，不再是一条新命令。
    return "import_device " + TclQuote(db_file) + " -package " + TclQuote(package) +
           SpeedArgSuffix(speed);
}

std::string DeviceCacheKey(std::string_view device, std::string_view family,
                           const std::optional<std::string>& speed) {
    // 对应 Python:
    //   re.sub(r"[^A-Za-z0-9_.-]+", "_", "%s_%s_%s" % (device, family, speed or "na"))
    std::string raw(device);
    raw += '_';
    raw += std::string(family);
    raw += '_';
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

// -----------------------------------------------------------------------------
//  P2-B⑤：设备摘要落盘缓存（跨进程）
// -----------------------------------------------------------------------------
//  为什么：`import_device + 4×query_current_package` 实测 ≈ 350 ms（MCP-TD.md），
//  而设备表只在 Bridge 的内存里，每次新进程都重来。缓存只存摘要字段，
//  不存 TD 原始输出（原始输出体积大，且解析逻辑变化后无法复用）。
namespace {

using Json = nlohmann::json;

constexpr int kDeviceSummaryCacheVersion = 1;
constexpr std::size_t kDeviceSummaryCacheMaxEntries = 8;

// Pin 在 JSON 数组里的字段顺序（压缩成数组：200 个引脚时比对象形式小一半、解析更快）
constexpr std::size_t kPinFieldCount = 11;

bool ReadString(const Json& value, std::string* out) {
    if (!value.is_string()) return false;
    *out = value.get<std::string>();
    return true;
}

bool ReadOptionalString(const Json& value, std::optional<std::string>* out) {
    if (value.is_null()) {
        out->reset();
        return true;
    }
    if (!value.is_string()) return false;
    *out = value.get<std::string>();
    return true;
}

bool ReadOptionalInt(const Json& value, std::optional<int>* out) {
    if (value.is_null()) {
        out->reset();
        return true;
    }
    if (!value.is_number_integer()) return false;
    *out = value.get<int>();
    return true;
}

bool ReadBool(const Json& value, bool* out) {
    if (!value.is_boolean()) return false;
    *out = value.get<bool>();
    return true;
}

Json NullableString(const std::optional<std::string>& value) {
    return value.has_value() ? Json(*value) : Json(nullptr);
}

Json NullableInt(const std::optional<int>& value) {
    return value.has_value() ? Json(*value) : Json(nullptr);
}

std::string DumpCacheFile(const Json& entries) {
    Json root = Json::object();
    root["v"] = kDeviceSummaryCacheVersion;
    root["entries"] = entries;
    return root.dump();
}

// 文件里现有条目（损坏 / 版本不符 -> 空表，按重建处理）
Json ReadExistingEntries(const std::string& path) {
    const std::optional<std::string> text = platform::try_read_file_utf8(path);
    if (!text.has_value()) return Json::object();
    try {
        const Json parsed = Json::parse(*text);
        if (!parsed.is_object()) return Json::object();
        if (parsed.value("v", 0) != kDeviceSummaryCacheVersion) return Json::object();
        const auto entries = parsed.find("entries");
        if (entries == parsed.end() || !entries->is_object()) return Json::object();
        return *entries;
    } catch (...) {
        return Json::object();  // 损坏：当空表重建
    }
}

}  // namespace

std::string DeviceSummaryCacheKey(std::string_view td_root, std::string_view td_version,
                                  std::string_view device, std::string_view family,
                                  std::string_view speed) {
    // td_root + td_version 让"换安装 / TD 升级"自动失效；
    // device/family/speed 让".al 里器件变了"自动失效。
    std::string key(td_root);
    key += '|';
    key += td_version;
    key += '|';
    key += device;
    key += '|';
    key += family;
    key += '|';
    key += speed.empty() ? std::string("-") : std::string(speed);
    return key;
}

std::string DeviceSummaryCachePath(std::string_view state_dir) {
    if (state_dir.empty()) return {};
    return platform::path_join(state_dir, "cache/td_device_summary.json");
}

std::string EncodeDeviceSummary(const DeviceInfo& info) {
    Json pins = Json::array();
    for (const auto& [key, pin] : info.pins) {
        Json item = Json::array();
        item.push_back(key);
        item.push_back(pin.pe);
        item.push_back(pin.pe_raw);
        item.push_back(pin.location);
        item.push_back(pin.type);
        item.push_back(NullableInt(pin.int_bank));
        item.push_back(NullableInt(pin.ext_bank));
        item.push_back(pin.clock_capable);
        item.push_back(pin.lvds);
        item.push_back(NullableString(pin.clock_kind));
        item.push_back(NullableString(pin.lvds_type));
        pins.push_back(std::move(item));
    }
    Json dedicated = Json::array();
    for (const DedicatedPin& pin : info.dedicated) {
        dedicated.push_back(Json::array({pin.role, pin.pe}));
    }
    Json entry = Json::object();
    entry["device"] = info.device;
    entry["family"] = info.family;
    entry["package"] = info.package;
    entry["db"] = info.db;
    entry["pins"] = std::move(pins);
    entry["dedicated"] = std::move(dedicated);
    return entry.dump();
}

std::optional<DeviceInfo> DecodeDeviceSummary(std::string_view json_text) {
    try {
        const Json parsed = Json::parse(json_text.begin(), json_text.end());
        if (!parsed.is_object()) return std::nullopt;
        DeviceInfo info;
        if (!ReadString(parsed.at("device"), &info.device)) return std::nullopt;
        if (!ReadString(parsed.at("family"), &info.family)) return std::nullopt;
        if (!ReadString(parsed.at("package"), &info.package)) return std::nullopt;
        if (!ReadString(parsed.at("db"), &info.db)) return std::nullopt;

        const Json& pins = parsed.at("pins");
        if (!pins.is_array()) return std::nullopt;
        for (const Json& item : pins) {
            if (!item.is_array() || item.size() != kPinFieldCount) return std::nullopt;
            std::string key;
            Pin pin;
            if (!ReadString(item.at(0), &key)) return std::nullopt;
            if (!ReadString(item.at(1), &pin.pe)) return std::nullopt;
            if (!ReadString(item.at(2), &pin.pe_raw)) return std::nullopt;
            if (!ReadString(item.at(3), &pin.location)) return std::nullopt;
            if (!ReadString(item.at(4), &pin.type)) return std::nullopt;
            if (!ReadOptionalInt(item.at(5), &pin.int_bank)) return std::nullopt;
            if (!ReadOptionalInt(item.at(6), &pin.ext_bank)) return std::nullopt;
            if (!ReadBool(item.at(7), &pin.clock_capable)) return std::nullopt;
            if (!ReadBool(item.at(8), &pin.lvds)) return std::nullopt;
            if (!ReadOptionalString(item.at(9), &pin.clock_kind)) return std::nullopt;
            if (!ReadOptionalString(item.at(10), &pin.lvds_type)) return std::nullopt;
            info.pins[key] = std::move(pin);
        }
        const Json& dedicated = parsed.at("dedicated");
        if (!dedicated.is_array()) return std::nullopt;
        for (const Json& item : dedicated) {
            if (!item.is_array() || item.size() != 2) return std::nullopt;
            DedicatedPin pin;
            if (!ReadString(item.at(0), &pin.role)) return std::nullopt;
            if (!ReadString(item.at(1), &pin.pe)) return std::nullopt;
            info.dedicated.push_back(std::move(pin));
        }
        // 与现查一致：没有引脚 = 无效设备表（现查路径此时会抛错，缓存不能更"宽容"）
        if (info.pins.empty()) return std::nullopt;
        return info;
    } catch (...) {
        return std::nullopt;  // 损坏 = 未命中
    }
}

std::string DeviceSummaryCacheFileText(std::string_view key, const DeviceInfo& info) {
    Json entries = Json::object();
    entries[std::string(key)] = Json::parse(EncodeDeviceSummary(info));
    return DumpCacheFile(entries);
}

std::optional<DeviceInfo> LoadDeviceSummaryCache(std::string_view state_dir,
                                                 std::string_view key) {
    if (state_dir.empty() || key.empty()) return std::nullopt;
    const std::string path = DeviceSummaryCachePath(state_dir);
    const std::optional<std::string> text = platform::try_read_file_utf8(path);
    if (!text.has_value()) return std::nullopt;
    try {
        const Json parsed = Json::parse(*text);
        if (!parsed.is_object()) return std::nullopt;
        if (parsed.value("v", 0) != kDeviceSummaryCacheVersion) return std::nullopt;
        const auto entries = parsed.find("entries");
        if (entries == parsed.end() || !entries->is_object()) return std::nullopt;
        const auto entry = entries->find(std::string(key));
        if (entry == entries->end()) return std::nullopt;
        return DecodeDeviceSummary(entry->dump());
    } catch (...) {
        return std::nullopt;  // 损坏 = 未命中（调用方会回落现查并重写）
    }
}

bool StoreDeviceSummaryCache(std::string_view project_dir, std::string_view state_dir,
                             std::string_view key, const DeviceInfo& info) {
    if (state_dir.empty() || key.empty()) return false;
    const std::string path = DeviceSummaryCachePath(state_dir);
    try {
        Json entries = ReadExistingEntries(path);
        entries[std::string(key)] = Json::parse(EncodeDeviceSummary(info));
        if (entries.size() > kDeviceSummaryCacheMaxEntries) {
            entries = Json::object();
            entries[std::string(key)] = Json::parse(EncodeDeviceSummary(info));
        }
        // 统一写服务：state_dir + 原子写 + 审计（门禁要求，不得自己写文件）。
        const platform::writes::Result result = platform::writes::Write(
            project_dir, path, DumpCacheFile(entries),
            platform::writes::Options{.caller = "mcp.device_summary"});
        if (!result.ok) {
            // 缓存是纯优化：失败只留 debug 日志，绝不阻断调用方主流程。
            platform::IgnoreResult(result);
            return false;
        }
        return true;
    } catch (...) {
        // 序列化 / 内存分配都失败：保持"绝不抛"契约（写盘失败本身已由 IgnoreResult 记日志）。
        return false;
    }
}

}  // namespace fpgalsp::td