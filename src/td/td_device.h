// =============================================================================
//  src/td/td_device.h —— 设备库候选与引脚查询解析
// =============================================================================
//  对应参考实现：%PYREF%\fpgalsp\td.py
//      DB_PREFIXES / _db_candidates / _parse_pipe_table / parse_fio /
//      parse_clock_io / parse_lvds / parse_cfg_io / _cache_key
//
//  ⚠ 实测纠正（详见 tests/td/PYTHON-PARITY.md §6.1）：
//     import_device 的参数是**裸数据库文件名**（如 "ef2_4.db"），
//     不是完整路径、也不是 family 名。
//       完整路径 -> USR-8316 ERROR: invalid speed grade setting "speed -2"
//       family 名 -> USR-8001 ERROR: Invalid device_db name
//     正确形态   -> import_device ef2_4.db -package EF2L45LG144B
// =============================================================================

#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fpgalsp::td {

inline constexpr int kSeverityWarningConst = 2;

// 一个可用的封装引脚
struct Pin {
    std::string pe;       // 规范化成大写，如 "P87"（显示用）
    std::string pe_raw;   // TD 原样，如 "p87"
    std::string location; // 如 "pb1b"
    std::string type;     // 如 "fio"
    std::optional<int> int_bank;
    std::optional<int> ext_bank;
    bool clock_capable = false;
    bool lvds = false;
    std::optional<std::string> clock_kind;  // 如 "dpclkio_1"
    std::optional<std::string> lvds_type;   // 如 "LVDS"
};

// 专用功能引脚（黑名单，禁止当普通 I/O 用）
struct DedicatedPin {
    std::string role;  // 如 "programn"
    std::string pe;    // 如 "P119"
};

// 设备信息（对应 Python load_device_info 的返回 data）
struct DeviceInfo {
    std::string device;
    std::string family;
    std::string package;
    std::string db;  // 成功使用的 db 文件名
    // 键是 pe 的小写形式（对应 Python：pins[pe.lower()]）
    std::map<std::string, Pin> pins;
    std::vector<DedicatedPin> dedicated;
};

// family -> 数据库文件名前缀（best first）
// 对应 Python 的 DB_PREFIXES
std::vector<std::string> DbPrefixesForFamily(std::string_view family);

// 对应 Python _db_candidates:
//   列出 <td_root>/arch/*.db，按「family 前缀优先、其余按字典序」排序。
std::vector<std::string> DbCandidates(std::string_view td_root, std::string_view family,
                                      std::string_view device = {});

// 表行切分（对应 Python _parse_pipe_table）
//   只保留含至少 2 个 '|' 的行；剔除以 "XXX-1234 :" 或 "XXX-1234" 开头的首单元。
std::vector<std::vector<std::string>> ParsePipeTable(const std::vector<std::string>& lines);

// 从原始文本直接解析（内部先 CleanLines）
std::vector<std::vector<std::string>> ParsePipeTableText(std::string_view text);

// 解析 query_current_package 的四种输出
// 返回 map<pe_lower, Pin>
std::map<std::string, Pin> ParseFio(std::string_view text);

// 就地修补 pins 的 clock_capable / clock_kind
void ParseClockIo(std::string_view text, std::map<std::string, Pin>& pins);

// 就地修补 pins 的 lvds / lvds_type
void ParseLvds(std::string_view text, std::map<std::string, Pin>& pins);

// dedicated 引脚列表
std::vector<DedicatedPin> ParseCfgIo(std::string_view text);

// 拼装完整的 DeviceInfo（依次应用四种解析）
DeviceInfo BuildDeviceInfo(std::string_view fio_text, std::string_view clock_text,
                           std::string_view lvds_text, std::string_view cfg_text);

// speed 参数：TD 写 "-2"，Tcl 要 "2"（去掉正负号）
// 返回形如 " -speed 2" 的后缀；speed 为空时返回空串（对应 Python load_device_info）
std::string SpeedArgSuffix(const std::optional<std::string>& speed);

// 构造 import_device 命令行（不含引号处理，调用方按需加 {}）
std::string MakeImportDeviceCommand(std::string_view db_file, std::string_view package,
                                    const std::optional<std::string>& speed);

// 缓存文件名（对应 Python _cache_key）：
//    device_family_speed.json，非 [A-Za-z0-9_.-] 的字符替换成 '_'
std::string DeviceCacheKey(std::string_view device, std::string_view family,
                           const std::optional<std::string>& speed);

// -----------------------------------------------------------------------------
// P2-B⑤：设备摘要**跨进程**落盘缓存
// -----------------------------------------------------------------------------
//  实测（doc/audit/2026-11-22/perf2/MCP-TD.md）：MCP 冷调用里的
//  `import_device + 4×query_current_package` ≈ 350 ms，而设备表在我们这边只存在
//  `Bridge::devices_` 内存里 —— 每个新进程都要重付一次。
//
//  这里只存**设备摘要字段**（DeviceInfo 的各字段值），**不存 TD 的原始输出**。
//  文件：<state_dir>/cache/td_device_summary.json
//      { "v": 1, "entries": { "<key>": { "device","family","package","db",
//                                      "pins": [[k,pe,pe_raw,location,type,
//                                                intBank,extBank,clock,lvds,
//                                                clockKind,lvdsType], ...],
//                                      "dedicated": [[role,pe], ...] } } }
//  失效语义（任一即未命中，回落现查并重写）：
//    * key 里含 td_root + td_version -> 换安装 / TD 升级自动失效；
//    * key 里含 device/family/speed -> .al 里器件变了自动失效；
//    * 文件缺失 / JSON 损坏 / 字段类型不对 / 没有引脚 -> 未命中；
//    * "v" 不匹配 -> 未命中（日后改格式不必写迁移）。
std::string DeviceSummaryCacheKey(std::string_view td_root, std::string_view td_version,
                                  std::string_view device, std::string_view family,
                                  std::string_view speed);

// <state_dir>/cache/td_device_summary.json（state_dir 为空 -> 空串）
std::string DeviceSummaryCachePath(std::string_view state_dir);

// 单条摘要的 JSON 文本（纯函数，便于测试/诊断；不含 key 与文件外壳）。
std::string EncodeDeviceSummary(const DeviceInfo& info);

// 反序列化单条摘要；任何异常/字段缺失/类型不符都返回 nullopt（= 缓存未命中）。
std::optional<DeviceInfo> DecodeDeviceSummary(std::string_view json_text);

// 只含一条 entry 的整份缓存文件文本（测试种缓存用；生产写盘走 StoreDeviceSummaryCache）。
std::string DeviceSummaryCacheFileText(std::string_view key, const DeviceInfo& info);

// 读缓存：文件/条目缺失、损坏、版本不符都返回 nullopt，**从不抛**。
std::optional<DeviceInfo> LoadDeviceSummaryCache(std::string_view state_dir,
                                                 std::string_view key);

// 写缓存（保留文件里其它条目，最多 8 条）。
//   只写 state_dir（经 platform::writes::Write，受写服务门禁约束）；
//   失败返回 false 并留 debug 日志，**绝不抛** —— 缓存是纯优化，不能变成新的失败点。
bool StoreDeviceSummaryCache(std::string_view project_dir, std::string_view state_dir,
                             std::string_view key, const DeviceInfo& info);

}  // namespace fpgalsp::td