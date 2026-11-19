// =============================================================================
//  tests/unit/mcp_tools_test.cpp —— 39 个工具表的契约不变量
// =============================================================================
//  这一层不测"某个工具返回什么"，只测**工具表本身**是否满足契约：
//    · 数量 / 名字 / 顺序与 Python 参考实现一致
//    · 每个 inputSchema 的结构合法（object/properties/required/additionalProperties）
//    · required 里的每个键都在 properties 里
//    · annotations 由 requires 推出（只读 vs 破坏性）
//    · 可见性过滤（默认隐藏 flow/write）
//
//  schema 的**逐字段**比对在 tests/compare_mcp_schema.py（驱动真实二进制），
//  这里只做不需要 Python 的不变量 —— 两条防线互相独立。
// =============================================================================

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "mcp/tools.h"

namespace {

using fpgalsp::mcp::Json;
using fpgalsp::mcp::Tool;

// 与 Python mcp.TOOLS 完全一致的 39 个名字（顺序也是契约的一部分）
const char* const kExpectedNames[] = {
    "fpga_project_list",
    "fpga_project_info",
    "fpga_project_files",
    "fpga_project_structure",
    "fpga_project_reports",
    "fpga_flow_status",
    "fpga_flow_run",
    "fpga_flow_cancel",
    "fpga_analysis_run",
    "fpga_strategy_compare",
    "fpga_project_update",
    "fpga_project_add_source",
    "fpga_project_rename",
    "fpga_document_symbols",
    "fpga_workspace_symbols",
    "fpga_search_hierarchy",
    "fpga_definition",
    "fpga_references",
    "fpga_hover",
    "fpga_symbol_path",
    "fpga_call_hierarchy",
    "fpga_completion",
    "fpga_code_actions",
    "fpga_rename_edits",
    "fpga_format",
    "fpga_instantiation",
    "fpga_module_info",
    "fpga_duplicate_modules",
    "fpga_diagnostics",
    "fpga_device_info",
    "fpga_constraints_status",
    "fpga_td_info",
    // P-B5：job 是 runId 的稳定别名（Python 版没有这两个工具）
    "fpga_job_status",
    "fpga_job_cancel",
    "fpga_cables",
    "fpga_flow_wait",
    "fpga_flow_log",
    "fpga_flow_issues",
    "fpga_read_file",
    "fpga_search_text",
    "fpga_constraint_audit",
};

// 递归检查 schema 里所有 "type": "array" 都有 items
void CheckArraysHaveItems(const Json& schema, const std::string& path) {
    if (schema.is_object()) {
        if (schema.contains("type") && schema["type"].is_string() &&
            schema["type"].get<std::string>() == "array") {
            EXPECT_TRUE(schema.contains("items")) << path;
        }
        for (auto it = schema.begin(); it != schema.end(); ++it) {
            CheckArraysHaveItems(it.value(), path + "." + it.key());
        }
    } else if (schema.is_array()) {
        for (size_t index = 0; index < schema.size(); ++index) {
            CheckArraysHaveItems(schema[index], path + "[" + std::to_string(index) + "]");
        }
    }
}

// 递归收集所有 enum 值，确认它们是字符串
void CheckEnumsAreStrings(const Json& schema, const std::string& path) {
    if (schema.is_object()) {
        if (schema.contains("enum")) {
            ASSERT_TRUE(schema["enum"].is_array()) << path;
            for (const Json& value : schema["enum"]) {
                EXPECT_TRUE(value.is_string()) << path;
            }
        }
        for (auto it = schema.begin(); it != schema.end(); ++it) {
            CheckEnumsAreStrings(it.value(), path + "." + it.key());
        }
    } else if (schema.is_array()) {
        for (size_t index = 0; index < schema.size(); ++index) {
            CheckEnumsAreStrings(schema[index], path + "[" + std::to_string(index) + "]");
        }
    }
}

}  // namespace

TEST(McpTools, TableHas41Tools) {
    // 39 个 Python 对齐工具 + P-B5 的 fpga_job_status/fpga_job_cancel
    EXPECT_EQ(fpgalsp::mcp::AllTools().size(), 41u);
}

TEST(McpTools, NamesMatchDeclaredOrder) {
    const std::vector<Tool>& tools = fpgalsp::mcp::AllTools();
    ASSERT_EQ(tools.size(), std::size(kExpectedNames));
    for (size_t index = 0; index < tools.size(); ++index) {
        EXPECT_EQ(tools[index].name, kExpectedNames[index]) << "第 " << index << " 个工具";
    }
}

TEST(McpTools, NamesAreUnique) {
    std::set<std::string> names;
    for (const Tool& tool : fpgalsp::mcp::AllTools()) {
        EXPECT_TRUE(names.insert(tool.name).second) << tool.name;
    }
    EXPECT_EQ(names.size(), 41u);
}

TEST(McpTools, EveryToolHasTitleAndDescription) {
    for (const Tool& tool : fpgalsp::mcp::AllTools()) {
        EXPECT_FALSE(tool.title.empty()) << tool.name;
        EXPECT_FALSE(tool.description.empty()) << tool.name;
        EXPECT_EQ(tool.title.find('\n'), std::string::npos) << tool.name;
    }
}

TEST(McpTools, EverySchemaIsObjectWithProperties) {
    for (const Tool& tool : fpgalsp::mcp::AllTools()) {
        const Json& schema = tool.input_schema;
        ASSERT_TRUE(schema.is_object()) << tool.name;
        EXPECT_EQ(schema.at("type").get<std::string>(), "object") << tool.name;
        EXPECT_TRUE(schema.contains("properties")) << tool.name;
        EXPECT_TRUE(schema.at("properties").is_object()) << tool.name;
        EXPECT_TRUE(schema.contains("required")) << tool.name;
        EXPECT_TRUE(schema.at("required").is_array()) << tool.name;
    }
}

TEST(McpTools, AdditionalPropertiesIsAlwaysFalse) {
    for (const Tool& tool : fpgalsp::mcp::AllTools()) {
        ASSERT_TRUE(tool.input_schema.contains("additionalProperties")) << tool.name;
        EXPECT_FALSE(tool.input_schema.at("additionalProperties").get<bool>()) << tool.name;
    }
}

TEST(McpTools, RequiredKeysExistInProperties) {
    for (const Tool& tool : fpgalsp::mcp::AllTools()) {
        const Json& properties = tool.input_schema.at("properties");
        for (const Json& key : tool.input_schema.at("required")) {
            ASSERT_TRUE(key.is_string()) << tool.name;
            const std::string name = key.get<std::string>();
            EXPECT_TRUE(properties.contains(name)) << tool.name << " 的 required 键 " << name;
        }
    }
}

TEST(McpTools, ArraysAlwaysDeclareItems) {
    for (const Tool& tool : fpgalsp::mcp::AllTools()) CheckArraysHaveItems(tool.input_schema, tool.name);
}

TEST(McpTools, EnumsAreStringArrays) {
    for (const Tool& tool : fpgalsp::mcp::AllTools()) CheckEnumsAreStrings(tool.input_schema, tool.name);
}

TEST(McpTools, IntegerBoundsAreSane) {
    for (const Tool& tool : fpgalsp::mcp::AllTools()) {
        for (const auto& [name, prop] : tool.input_schema.at("properties").items()) {
            if (!prop.is_object() || !prop.contains("type")) continue;
            if (prop["type"].get<std::string>() != "integer") continue;
            if (prop.contains("minimum")) {
                EXPECT_GE(prop["minimum"].get<int>(), 0) << tool.name << "." << name;
            }
            if (prop.contains("minimum") && prop.contains("maximum")) {
                EXPECT_LE(prop["minimum"].get<int>(), prop["maximum"].get<int>())
                    << tool.name << "." << name;
            }
        }
    }
}

TEST(McpTools, AnnotationsFollowRequires) {
    for (const Tool& tool : fpgalsp::mcp::AllTools()) {
        const Json json = fpgalsp::mcp::ToolToJson(tool);
        ASSERT_TRUE(json.contains("annotations")) << tool.name;
        const Json& annotations = json.at("annotations");
        const bool read_only = annotations.at("readOnlyHint").get<bool>();
        const bool destructive = annotations.at("destructiveHint").get<bool>();
        if (tool.requires_flag.empty()) {
            EXPECT_TRUE(read_only) << tool.name;
            EXPECT_FALSE(destructive) << tool.name;
        } else {
            EXPECT_FALSE(read_only) << tool.name;
            EXPECT_TRUE(destructive) << tool.name;
        }
    }
}

TEST(McpTools, ToolToJsonStripsInternalFields) {
    for (const Tool& tool : fpgalsp::mcp::AllTools()) {
        const Json json = fpgalsp::mcp::ToolToJson(tool);
        EXPECT_FALSE(json.contains("handler")) << tool.name;
        EXPECT_FALSE(json.contains("requires")) << tool.name;
        EXPECT_TRUE(json.contains("name")) << tool.name;
        EXPECT_TRUE(json.contains("title")) << tool.name;
        EXPECT_TRUE(json.contains("description")) << tool.name;
        EXPECT_TRUE(json.contains("inputSchema")) << tool.name;
    }
}

TEST(McpTools, VisibleToolsDefaultHidesGatedTools) {
    const std::vector<Json> visible = fpgalsp::mcp::VisibleTools(false, false);
    EXPECT_EQ(visible.size(), 33u);
    const std::set<std::string> gated = {"fpga_flow_run",   "fpga_flow_cancel", "fpga_analysis_run",
                                         "fpga_job_cancel",
                                         "fpga_strategy_compare", "fpga_project_update",
                                         "fpga_project_add_source", "fpga_project_rename"};
    for (const Json& tool : visible) {
        EXPECT_EQ(gated.count(tool.at("name").get<std::string>()), 0u)
            << tool.at("name").get<std::string>();
    }
}

TEST(McpTools, VisibleToolsAllIncludesEverything) {
    EXPECT_EQ(fpgalsp::mcp::VisibleTools(true, true).size(), 41u);
    // flow 门控 5 个（flow_run/flow_cancel/job_cancel/analysis_run/strategy_compare）
    // write 门控 3 个（project_update/project_add_source/project_rename）
    EXPECT_EQ(fpgalsp::mcp::VisibleTools(true, false).size(), 38u);
    EXPECT_EQ(fpgalsp::mcp::VisibleTools(false, true).size(), 36u);
}

TEST(McpTools, FindToolReturnsNullForUnknown) {
    EXPECT_EQ(fpgalsp::mcp::FindTool("fpga_nope"), nullptr);
    EXPECT_EQ(fpgalsp::mcp::FindTool(""), nullptr);
    const Tool* tool = fpgalsp::mcp::FindTool("fpga_project_list");
    ASSERT_NE(tool, nullptr);
    EXPECT_EQ(tool->handler, "project_list");
}

TEST(McpTools, EveryToolHasNonEmptyHandler) {
    for (const Tool& tool : fpgalsp::mcp::AllTools()) {
        EXPECT_FALSE(tool.handler.empty()) << tool.name;
    }
}

TEST(McpTools, NoToolDeclaresMetaOrProgressTokenInSchema) {
    // 进度令牌走 params._meta，不该出现在 inputSchema 里（否则 agent 会当参数传）
    for (const Tool& tool : fpgalsp::mcp::AllTools()) {
        const Json& properties = tool.input_schema.at("properties");
        EXPECT_FALSE(properties.contains("_meta")) << tool.name;
        EXPECT_FALSE(properties.contains("progressToken")) << tool.name;
    }
}

TEST(McpTools, SchemaDescriptionsAreSingleLine) {
    const auto check = [](const Json& schema, const std::string& path,
                          auto&& self) -> void {
        if (schema.is_object()) {
            if (schema.contains("description") && schema["description"].is_string()) {
                EXPECT_EQ(schema["description"].get<std::string>().find('\n'), std::string::npos)
                    << path;
            }
            for (auto it = schema.begin(); it != schema.end(); ++it) {
                self(it.value(), path + "." + it.key(), self);
            }
        } else if (schema.is_array()) {
            for (size_t index = 0; index < schema.size(); ++index) {
                self(schema[index], path + "[" + std::to_string(index) + "]", self);
            }
        }
    };
    for (const Tool& tool : fpgalsp::mcp::AllTools()) check(tool.input_schema, tool.name, check);
}