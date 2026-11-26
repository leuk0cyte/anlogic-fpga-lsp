// =============================================================================
//  src/hdl/primitives.h —— 器件原语 / Verilog 关键字表（vendored TD 数据）
// =============================================================================
//  对应参考实现：%PYREF%\fpgalsp\server.py
//    · `_primitives()`           -> DevicePrimitives() / IsDevicePrimitive()
//    · `VERILOG_KEYWORDS`        -> VerilogKeywords()
//
//  这两张表在两处被用到，而且**必须只有一份**：
//    1. `textDocument/completion`（745 条 kind=3 "Device primitive" + 48 条 kind=14 关键字）
//       —— Python 在同一位置是 814 项（21 模块 + 745 原语 + 48 关键字）。
//    2. `fpgaLsp/moduleTree` 的 `includePrimitives=false`
//       —— Python 用 `child_module.lower() in primitives` 决定不展开哪些黑盒；
//          C++ 早先用 `IsPrimitiveGate()`（只有 26 个 IEEE 门），于是 IP 文件里的
//          `EF2_LOGIC_BUFG` / `EF2_PHY_PLL` 被展开成 2 个假的黑盒节点（36 vs 34）。
//
//  数据来源是**编译期嵌入**的 `data/primitives.inc`（生成器 scratch/gen_b10_data.py，
//  数据源 `%PYREF%\fpgalsp\data\primitives.json`）。刻意不做运行时读文件：
//  语言服务器不该依赖 Python 仓库的安装路径存在。
//
//  ⚠ 计数口径是契约（探针按集合/计数比对），改动前先看 data/primitives.inc 的头注释。
// =============================================================================

#pragma once

#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace fpgalsp::hdl {

// 745 项：GENERIC_GATES ∪ primitives.json 各 family 的 cells ∪ json.generic_gates。
// 返回**已排序**的集合（Python: `for name in sorted(self._primitives())`）。
const std::set<std::string>& DevicePrimitives();

// 大小写不敏感的成员判断（Python: `child_module.lower() in primitives`）。
// moduleTree 的黑盒判定用它，别再用"名字像不像门电路"的启发式。
bool IsDevicePrimitive(std::string_view name);

// 48 项 Verilog 关键字（Python VERILOG_KEYWORDS **原样**，含重复的 `casex`）。
// 重复是**有意复现**：Python 补全列表里 casex 出现两次，探针按 kind 计数 48。
const std::vector<std::string>& VerilogKeywords();

}  // namespace fpgalsp::hdl