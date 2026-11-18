// =============================================================================
//  src/constraints/port.h —— 顶层端口视图（adc / sdc 共用）
// =============================================================================
//  为什么 ADC 和 SDC 共用一个 Port：
//    两边拿到的都是「顶层模块端口表」，来源是同一个 HDL 索引。如果各定义一个
//    Port，将来索引一改就要改两处，而且 `_port_bits` 的语义（width vs msb/lsb
//    的优先级）正是最容易写出偏差的地方 —— 只留一份实现。
//
//  `PortBits` 逐字复现 Python `adc._port_bits` / `sdc._port_bits`（两者实现相同）：
//    1) width 是 int 且 > 1  → [low, low+width)，low = min(msb, lsb) 或 0
//    2) width 是 int 且 == 1 → []（标量）
//    3) width 缺失/非 int，且 msb/lsb 都存在且不相等 → [min, max]
//    4) 其它 → []（标量）
//  返回值用 std::vector<int> 而不是 (low, high)：调用方普遍要「逐个 bit 判断
//  是否已约束」，用区间反而要多写一遍循环。
// =============================================================================

#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace fpgalsp::constraints {

using Json = nlohmann::json;

// 单行内的字符区间（0-based 行号 + 0-based UTF-16 列）。
// 与 lsp::Range 的区别：这里刻意保留 Python 侧的 int 语义（end 可能小于 start，
// 畸形 .adc 的 `LOCATION = ;` 就是如此），转换到 lsp::Range 时才钳位。
struct Span {
    int line = 0;
    int start = 0;
    int end = 0;
};

// 顶层端口。字段名与 Python 版 HDL 索引的端口 dict 一一对应。
struct Port {
    std::string name;
    std::string direction;
    std::optional<int> msb;
    std::optional<int> lsb;
    // width 用 optional：Python 侧 `port.get("width")` 缺失时**不是** 1，
    // 而是落到 msb/lsb 分支。用 int width = 1 的默认值会改变 (3) 的分支条件。
    std::optional<int> width;
    int line = 0;
    int character = 0;
    std::string file;

    static Port Scalar(std::string name, int line = 0, int character = 0);
    static Port Bus(std::string name, int msb, int lsb, int line = 0, int character = 0);
};

// 宽度的物理上限。Python 用 range() 造列表，宽度写成 1e9 会直接吃光内存；
// 语言服务器不能因为一个畸形端口表就 OOM，所以这里截断（这是**有意的偏差**，
// 只影响荒谬输入，且方向是"少报"而不是"误报"）。
inline constexpr int kMaxPortBits = 65536;

// 端口的全部 bit 下标（升序）；标量端口返回空 vector。
std::vector<int> PortBits(const Port& port);

// name -> 端口。重名时**第一个**胜出（复现 Python `_port_index`）。
// 用 unordered_map 是因为调用方只做查找；需要保持端口声明顺序的地方一律
// 直接遍历原始 vector（诊断排序依赖这个顺序）。
using PortIndex = std::unordered_map<std::string, const Port*>;
PortIndex BuildPortIndex(const std::vector<Port>& ports);

// 从 HDL 索引的 JSON 形状构造（缺失/类型错误一律取默认值，不抛异常）。
Port PortFromJson(const Json& json);
Json PortToJson(const Port& port);

}  // namespace fpgalsp::constraints