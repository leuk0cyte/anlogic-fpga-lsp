// =============================================================================
//  src/constraints/adc.h —— ADC 引脚约束（.adc）解析 / 诊断 / 补全辅助
// =============================================================================
//  参考实现：Y:\fpga-lsp\fpgalsp\adc.py（913 行）+ CONTRACT.md §"adc.py"
//
//  `.adc` 是**行式**格式，一行一条命令：
//      set_pin_assignment<TAB>{ w[0] }<TAB>{ LOCATION = P76; IOSTANDARD = LVCMOS33; }
//  · 端口名在**第一个** `{...}` 里，可能带空格填充（`{ di[0] }`）
//  · 属性在**第二个** `{...}` 里，是 `KEY = VALUE;` 对
//  · 空行、`#` 注释、`//` 注释忽略
//  · 只有一个 `{}` 组时：组内含 `=` 就当属性组，否则当端口组（Python 同规则）
//
//  命名空间刻意做成 `constraints::adc`，与 Python 模块一一对应：
//      Python                        C++
//      adc.parse(text)          ->   adc::Parse(text)
//      adc.coverage(...)        ->   adc::Coverage(...)   (返回 CoverageReport)
//      adc.diagnostics(...)     ->   adc::Diagnostics(...)
//      adc.context(...)         ->   adc::Context(...)
//      adc.word_at(...)         ->   adc::WordAt(...)
//  这样"这段 C++ 对应哪段 Python"从名字就能看出来。
//
//  ---- 本文件的三条硬约束 ----
//
//  1. **不抛异常。**
//     这是 CONTRACT.md 的完整性要求，也是语言服务器的底线：一个畸形 `.adc`
//     必须变成诊断，而不是让整个进程消失。所有入口对任意字节输入都有定义行为；
//     解析失败的行被跳过（Python 也是这样）。
//
//  2. **诊断严重度按 Python / CONTRACT.md 逐值复现，不要"按直觉调"。**
//     容易调错的几处（都有单测钉死）：
//       · 未知命令 ADC-CMD           -> 3 (Information)  ← 不是 warning
//       · 未声明的端口 USR-8052      -> 1 (Error)        ← 不是 warning
//       · 未知 IO 标准 ADC-IOSTD     -> 2 (Warning)
//       · 覆盖率 ADC-COVERAGE        -> 3 (Information)  ← 见第 3 条
//     task 描述里"未知命令→warning / 未声明端口→warning"是概要说法；
//     CONTRACT.md 的表格与 Python 实现才是逐字段验收依据。
//
//  3. **覆盖率只产出「一条」info 级摘要（ADC-COVERAGE）。**
//     如果一个未约束完的工程给每个未约束端口/每一位都发一条诊断，Problems
//     面板会被几十条噪音淹没，用户会把整个功能关掉。所以：
//       · 逐条等价的部分（BIT-8123 等）留在 `Diagnostics()` 里，与 Python 一致
//       · 覆盖率概览单独走 `CoverageDiagnostic()`，**一个文档最多一条**，
//         severity 恒为 3，且顶层端口全部约束时**不产生**诊断
//     Python 版没有 ADC-COVERAGE（它把覆盖率塞进 server.py 的 BIT-8123 里），
//     见 tests/PYTHON-BUGS.md 的登记。
//
//  ---- 与 Python 的已知差异（全部有意，改动前先读） ----
//    · 行分隔符只认单字节（见 text_util.h）
//    · `PortBits` 对荒谬位宽有上限（kMaxPortBits，防 OOM）
// =============================================================================

#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "constraints/port.h"
#include "lsp/diagnostic.h"

namespace fpgalsp::constraints {
namespace adc {

// ADC 认识的命令（Python `KNOWN_COMMANDS`）。
// ⚠ Python 的**补全**列表里还有 `set_ippin_assignment`，但白名单里没有它 ——
//   用户照着补全写出来会立刻收到 ADC-CMD。忠实复现并登记（PYTHON-BUGS.md）。
const std::set<std::string>& KnownCommands();
bool IsKnownCommand(std::string_view command);

// TD 认可的 IO 标准（Python `KNOWN_IO_STANDARDS`，**71 项逐字复制**）。
// 白名单漏项会让用户的正确工程报 warning，是最伤信任的一类误报，
// 所以这里不做"精简"；集合内容有单测逐项比对 Python 的取值。
const std::set<std::string>& KnownIoStandards();
bool IsKnownIoStandard(std::string_view standard);  // 大小写不敏感（同 Python .upper()）

// PULLTYPE 取值（Python `KNOWN_PULLTYPES`）。
// ⚠ Python 定义了它却**从不校验** PULLTYPE 值：`PULLTYPE = PULLUPX` 静默通过，
//   直到 bitgen 才报错。忠实复现（不新增诊断），登记见 PYTHON-BUGS.md。
const std::set<std::string>& KnownPulltypes();
const std::set<std::string>& KnownAttrKeys();

// 一条 `KEY = VALUE;` 属性（保留原始 key 大小写与两段 span，供 hover 用）
struct AttrItem {
    std::string key;        // 原文（如 "Location"）
    std::string key_upper;  // 归一化（"LOCATION"）
    std::string value;      // 去掉两端空白后的值
    Span key_span;
    Span value_span;
};

// 一行 `set_pin_assignment`。字段与 CONTRACT.md 的 Entry 一一对应。
struct Entry {
    int line = 0;
    int character = 0;  // 行首第一个非空白字符的列
    std::string raw;    // 原始整行（不含行结束符）
    std::string command;
    std::string port;   // 第一个 {} 组内去空白后的文本，如 "w[0]"
    std::string base;   // 端口名（去掉 bit 下标），如 "w"
    std::optional<int> bit;  // bit 下标；无下标为 nullopt

    // 属性：大写 key -> 去空白后的值。**后写的覆盖先写的**（TD 的顺序解析语义）。
    std::map<std::string, std::string> attrs;
    // 属性值 span：大写 key -> 值在行内的位置
    std::map<std::string, Span> attr_spans;
    Span port_span;
    Span command_span;
    std::vector<AttrItem> attr_items;  // 含重复项，供 ADC-ATTR 诊断使用
};

// 解析整篇 `.adc`。**不抛异常**；畸形行被跳过。
std::vector<Entry> Parse(std::string_view text);

// 解析单行（`Parse` 的内部步骤，单独暴露便于单测与补全使用）。
std::optional<Entry> ParseLine(std::string_view raw, int line);

// 把 `pwmG[5]` 拆成 ("pwmG", 5)；`clk` -> ("clk", nullopt)。
// 畸形文本走 best-effort：取开头标识符，无标识符则原样返回（Python 同）。
struct PortName {
    std::string base;
    std::optional<int> bit;
};
PortName SplitPort(std::string_view port);

// -----------------------------------------------------------------------------
//  覆盖率
// -----------------------------------------------------------------------------

struct DuplicatePort {
    std::string port;
    std::vector<int> lines;  // 升序
};

struct DuplicatePin {
    std::string pin;
    std::vector<std::string> ports;
    std::vector<int> lines;  // 升序
};

struct CoverageReport {
    std::map<std::string, std::vector<int>> covered;  // 端口 -> 已约束 bit（标量为 []）
    std::map<std::string, std::vector<int>> missing;  // 端口 -> 缺的 bit（标量为 []）
    std::vector<std::string> unconstrained_ports;      // 完全没约束的端口（声明顺序）
    std::vector<Entry> extra_ports;                    // 顶层模块里不存在的端口的条目
    std::vector<DuplicatePort> duplicate_ports;
    std::vector<DuplicatePin> duplicate_pins;
    int total_bits = 0;
    int constrained_bits = 0;
};

// `top` 参数仅为与 Python 签名保持一致（Python 版**没有用它**，覆盖率始终按
// 传入的 ports 计算）。保留是为了让"传了 top 就会按顶层过滤"的误解不会被写进调用点。
CoverageReport Coverage(const std::vector<Entry>& entries, const std::vector<Port>& ports,
                        std::string_view top = {});

// -----------------------------------------------------------------------------
//  诊断
// -----------------------------------------------------------------------------

// 与 Python `adc.diagnostics(entries, ports, device, text)` **逐字段等价**。
//   · device == nullptr  <=>  Python 的 device is None（跳过引脚存在性检查）
//   · text 参数 Python 版未使用，这里保留同名参数只为签名对照
//   · 不产出 ADC-COVERAGE（见下）；需要它请用带 DiagnosticsOptions 的重载
std::vector<lsp::Diagnostic> Diagnostics(const std::vector<Entry>& entries,
                                         const std::vector<Port>& ports,
                                         const Json* device = nullptr,
                                         [[maybe_unused]] std::string_view text = {});

struct DiagnosticsOptions {
    const Json* device = nullptr;
    std::string_view text;
    // 打开后追加**一条** info 级 ADC-COVERAGE 摘要（顶层端口全部约束时不加）
    bool include_coverage = false;
    std::string top;  // 只用于覆盖率消息里点出顶层模块名
};

std::vector<lsp::Diagnostic> Diagnostics(const std::vector<Entry>& entries,
                                         const std::vector<Port>& ports,
                                         const DiagnosticsOptions& options);

// 覆盖率摘要诊断（`ADC-COVERAGE`，severity 恒为 3 = Information）。
//   · 顶层端口全部约束 -> nullopt（正常工程不该有任何噪音）
//   · 否则 -> 一条，range 落在 (0,0)-(0,1)，message 里给统计与前若干未约束端口
// 单独成函数而不是塞进 Diagnostics，是为了让"与 Python 逐字段对照"保持干净。
std::optional<lsp::Diagnostic> CoverageDiagnostic(const CoverageReport& coverage,
                                                  std::string_view top = {});

// -----------------------------------------------------------------------------
//  光标辅助（hover / 补全用）
// -----------------------------------------------------------------------------

struct CursorContext {
    std::string kind;  // "command" | "port" | "attr_key" | "pin" | "iostandard" | "pulltype"
    Span span;
};

std::optional<CursorContext> Context(std::string_view text, int line, int character);

struct Word {
    std::string word;
    int line = 0;
    int start = 0;
    int end = 0;
};

std::optional<Word> WordAt(std::string_view text, int line, int character);

// -----------------------------------------------------------------------------
//  LSP 符号 / hover / 补全（纯函数；对应 server.py 的 `.adc` 分支）
// -----------------------------------------------------------------------------
//  JSON 形状逐字段对齐 Python（documentSymbol 47 条 / hover markdown / completion
//  {"isIncomplete":false,"items":[…]}）。三个函数都**不读磁盘、不抛异常**。
//
//  ports 是可选顶层端口表（Python 的 `_top_ports`）：只用于 `port` 上下文的
//  hover / completion。传空表时行为与 Python 侧"拿不到端口表"一致。
Json DocumentSymbols(std::string_view text);
Json Hover(std::string_view text, int line, int character,
           const std::vector<Port>& ports = {});
Json Completion(std::string_view text, int line, int character,
                const std::vector<Port>& ports = {});

// -----------------------------------------------------------------------------
//  JSON 序列化（与 Python dict 的 JSON 形状一致；用于与参考实现逐字段 diff）
// -----------------------------------------------------------------------------

Json ToJson(const Entry& entry);
Json ToJson(const std::vector<Entry>& entries);
Json ToJson(const CoverageReport& coverage);

}  // namespace adc
}  // namespace fpgalsp::constraints