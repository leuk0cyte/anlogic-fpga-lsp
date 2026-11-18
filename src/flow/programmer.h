// =============================================================================
//  src/flow/programmer.h —— BitWizard 烧录后端（唯一会碰真实硬件的代码路径）
// =============================================================================
//  对应参考实现：Y:\fpga-lsp\fpgalsp\program.py
//
//  ---- 安全模型（三条硬规则，出错会烧坏硬件，不能松）----
//  1. `list_cable` 为空 -> **直接拒绝**，一条烧录命令都不下发。
//  2. `program` 只在 `confirm: true` 时才执行；缺失/为 false 即拒绝。
//     （这一条是在 Python 版之上的**加强**：Python 的 program() 没有 confirm 参数，
//      确认放在 UI 层；把确认下沉到后端才可能保证"任何调用路径都拦得住"。）
//  3. 器件不匹配 -> 拒绝。
//     * 不同**系列**（工程 EF2L45 / 在线 PH1A100）：**即使 allow_mismatch=true 也拒绝**，
//       必须由客户端弹二次确认后走显式的强制通道。
//     * 同系列不同型号（EF2L45 vs EF2M45）：Python 允许 allow_mismatch 放行，保持一致。
//     区分依据是 program.py::compare_devices 的 series/letters 两级判定。
//
//  ---- 实测命令（TD 2026.2 的 BitWizard CLI）----
//      list_cable
//      read_device_id
//      program {<abs .bit>}                                  ; SRAM / JTAG
//      program_spi {<abs .bit>}                              ; Flash via SPI
//      svfgen -bit {<abs>} -svf {<abs>} -scan_mode prog_sram
// =============================================================================

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "jsonrpc/json_util.h"
#include "project/al_project.h"
#include "td/td_install.h"

namespace fpgalsp::flow {

// -----------------------------------------------------------------------------
// 纯解析 / 判定（可在单测里直接覆盖，不需要硬件）
// -----------------------------------------------------------------------------

struct CableInfo {
    int index = 0;
    std::string device;  // 如 "PH1A100"

    jsonrpc::Json ToJson() const;
};

// `list_cable` 输出 -> 电缆列表。空列表 = 没接板子。
// 只认含 "Connect device" 的行，形如 "0: PH1A100"。
std::vector<CableInfo> ParseCables(std::string_view text);

// `read_device_id` 输出 -> 在线器件 ID（读不到返回空串）。
// 规则：整行只有器件 ID 一个 token（无 ':'、无空格），形如 PH1A100，且不是 "PRJ-123" 这类错误码。
std::string ParseDeviceId(std::string_view text);

// EF2L45LG144B -> EF2L；PH1A100 -> PH1A；EF2M45LG100 -> EF2M
std::string SeriesOf(std::string_view part);
// EF2L45LG144B -> EF2；PH1A100 -> PH1（只取开头字母）
std::string LettersOf(std::string_view part);

struct DeviceComparison {
    bool match = false;
    // true 表示"同系列不同型号"这类**可告警但可由用户确认**的情况
    bool warn = false;
    std::string reason;
};

// 工程目标器件 vs 在线器件是否安全
DeviceComparison CompareDevices(std::string_view expected, std::string_view online);

struct BitCheck {
    bool ok = false;
    std::string reason;
    int64_t size = 0;
};

// .bit 文件存在性 / 扩展名 / 大小
BitCheck CheckBit(std::string_view path);

// BitWizard 输出里是否有 ERROR / CRITICAL-WARNING
bool HasBwError(std::string_view text);

// 工程里最新的 .bit（best_result 优先，其次 phy_1，按 mtime 降序）
std::string DefaultBit(const project::Project& project);

// -----------------------------------------------------------------------------
// 会话抽象（真实实现包 bw_commands_prompt.exe；单测注入假实现）
// -----------------------------------------------------------------------------

class BwSessionLike {
public:
    virtual ~BwSessionLike() = default;
    virtual std::string Run(std::string_view tcl, double timeout_seconds) = 0;
    virtual void Kill() = 0;
    virtual void Close() = 0;
    virtual std::string LastError() const { return {}; }
};

using BwSessionFactory = std::function<std::unique_ptr<BwSessionLike>(const std::string& cwd)>;

struct ProgramOptions {
    std::string cwd;                  // BitWizard 的 cwd，默认 project.dir
    std::string bit_path;             // 空 = 用工程里最新的 .bit
    std::string mode = "sram";        // "sram" | "flash"
    bool confirm = false;             // ← 硬要求，必须显式为 true
    bool allow_mismatch = false;      // 只对"同系列不同型号"生效
    double list_cable_timeout = 60.0;
    double device_id_timeout = 60.0;
    double program_timeout = 900.0;
    double svf_timeout = 600.0;
    std::string scan_mode = "prog_sram";  // svfgen 用
    std::string svf_path;                 // 空 = 与 .bit 同目录同名 .svf
    BwSessionFactory session_factory;     // 空 = 真实 BitWizard
};

// 只读预检（对应 Python inspect）：cables / device_id / match / reason / bit_check
jsonrpc::Json Inspect(const td::TdChoice& td, const project::Project& project,
                      const ProgramOptions& options = {});

// 烧录（sram / flash）。返回 {ok, refused?, stage?, device_id, expected, bit, command, log, error}
jsonrpc::Json Program(const td::TdChoice& td, const project::Project& project,
                      const ProgramOptions& options);

// 纯文件操作：.bit -> .svf（不碰硬件）
jsonrpc::Json GenerateSvf(const td::TdChoice& td, const project::Project& project,
                          const ProgramOptions& options);

// 拒绝时构造统一形状的返回（测试直接比对这几个字段）
jsonrpc::Json Refusal(std::string_view stage, std::string_view reason);

}  // namespace fpgalsp::flow