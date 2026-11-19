// =============================================================================
//  src/hdl/primitives.cpp —— 器件原语 / Verilog 关键字表的唯一实现
// =============================================================================
//  数据在 data/primitives.inc（生成文件，见该文件头注释与 scratch/gen_b10_data.py）。
//
//  为什么用 `std::set` 而不是哈希表：
//    completion 要 `for name in sorted(...)` 的顺序，moduleTree 要稳定顺序的日志；
//    std::set 天然有序，且 745 个短字符串的插入成本可以忽略（进程内只建一次）。
//  再维护一个"小写集合"用于大小写不敏感判断：Python 侧是
//    `{n.lower() for n in primitives}`，即先整体小写再查表 —— 逐位等价。
// =============================================================================

#include "hdl/primitives.h"

#include <algorithm>
#include <cstddef>

namespace fpgalsp::hdl {
namespace {

// 生成数据：kDevicePrimitiveNames（已排序）/ kVerilogKeywords（Python 原顺序）
#include "hdl/data/primitives.inc"

std::string LowerAscii(std::string_view text) {
    std::string lowered(text);
    for (char& c : lowered) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return lowered;
}

const std::set<std::string>& PrimitiveSet() {
    static const std::set<std::string> set = [] {
        std::set<std::string> built;
        // 逐个 emplace：std::string 从 std::string_view 构造是 explicit 的，
        // 不能用 insert(first, last) 的范围版本。
        for (const std::string_view name : kDevicePrimitiveNames) built.emplace(name);
        return built;
    }();
    return set;
}

const std::set<std::string>& LowerPrimitiveSet() {
    static const std::set<std::string> set = [] {
        std::set<std::string> built;
        for (const std::string& name : PrimitiveSet()) built.insert(LowerAscii(name));
        return built;
    }();
    return set;
}

}  // namespace

const std::set<std::string>& DevicePrimitives() { return PrimitiveSet(); }

bool IsDevicePrimitive(std::string_view name) {
    if (name.empty()) return false;
    return LowerPrimitiveSet().count(LowerAscii(name)) != 0;
}

const std::vector<std::string>& VerilogKeywords() {
    static const std::vector<std::string> keywords = [] {
        std::vector<std::string> built;
        built.reserve(std::size(kVerilogKeywords));
        for (const std::string_view keyword : kVerilogKeywords) built.emplace_back(keyword);
        return built;
    }();
    return keywords;
}

}  // namespace fpgalsp::hdl