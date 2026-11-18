// =============================================================================
//  src/mcp/tools.h —— MCP 39 个工具的定义（名字 / 标题 / 描述 / inputSchema）
// =============================================================================
//  参考实现：Y:\fpga-lsp\fpgalsp\mcp.py 的 TOOLS 表（第 75-287 行）
//
//  ---- 为什么 schema 必须逐字段一致 ----
//    inputSchema 是 **AI agent 的实际契约**：agent 按 schema 生成参数，按 required
//    判断该不该传，按 enum 收窄取值。schema 不一致 -> agent 传错参数 -> 工具调用
//    失败，而在 agent 侧表现成"这个工具不好用"，几乎无法归因到 schema 漂移。
//    所以本文件的内容由 `tests/compare_mcp_schema.py` 逐字段守护（39 个工具
//    的 name/title/description/inputSchema/annotations 全部比对）。
//
//  ---- 与 Python 的差异（唯一一处，刻意的）----
//    Python 的 dict 保插入序，C++ 的 nlohmann::json 默认按 key 排序。因此
//    `tools/list` 的 JSON **文本**顺序不同，但**语义（字段集合/取值）完全一致**。
//    对比脚本按 canonical（sort_keys）比较，数组顺序（enum 的顺序）仍然严格比对。
//
//  ---- 可见性 ----
//    Python `visible_tools()`：requires=="flow" 需要 --allow-flow、
//    requires=="write" 需要 --allow-write，并且在输出里**剥掉**
//    handler / requires 两个内部字段（agent 不该看到实现细节）。
//    annotations 由 requires 推出：readOnlyHint = (requires 为空)，
//    destructiveHint = (requires 非空)。
// =============================================================================

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace fpgalsp::mcp {

using Json = nlohmann::json;

struct Tool {
    std::string name;
    std::string title;
    std::string description;
    Json input_schema;
    std::string handler;  // 内部字段：不输出给客户端
    std::string requires_flag;  // "" | "flow" | "write"；内部字段（Python 叫 requires，C++20 里是关键字）
};

// -----------------------------------------------------------------------------
// schema 构造器（逐字对应 Python mcp.py 的 _string/_integer/_boolean/_array/_schema）
// -----------------------------------------------------------------------------

// _string(description, enum=None)
Json Str(std::string description, std::vector<std::string> enum_values = {});

// _integer(description, minimum=None, maximum=None)
Json Int(std::string description, std::optional<int> minimum = std::nullopt,
         std::optional<int> maximum = std::nullopt);

// _boolean(description)
Json Bool(std::string description);

// _array(description, item=None)；item 缺省是 {"type": "string"}（见 StrNoDesc）
Json Arr(std::string description, Json items);

// Python `_array(description)` 的默认 items：**没有** description 字段
//（与 Str("") 不同 —— 后者会多出一个 "description": ""，schema 就不一致了）
Json StrNoDesc();

// _schema(properties, required)：恒有 additionalProperties = false
Json Schema(std::vector<std::pair<std::string, Json>> properties,
            std::vector<std::string> required = {});

// -----------------------------------------------------------------------------
// 工具表
// -----------------------------------------------------------------------------

const std::vector<Tool>& AllTools();

// 按名字查找；找不到返回 nullptr（对应 Python tools.get(name) -> None）
const Tool* FindTool(std::string_view name);

// 对应 Python `McpServer.visible_tools()`
std::vector<Json> VisibleTools(bool allow_flow, bool allow_write);

// 单个工具对外的 JSON（剥掉 handler/requires，补 annotations）
Json ToolToJson(const Tool& tool);

}  // namespace fpgalsp::mcp