# =============================================================================
#  tests/unit/ —— 平台层 + JSON-RPC 单元测试
# =============================================================================

if(NOT TARGET GTest::gtest_main)
    message(STATUS "tests/unit: 未找到 GTest，跳过单元测试")
    return()
endif()

function(fpgalsp_add_unit_test name)
    # 可选的额外链接目标（第 2 个参数起的列表），用于只给个别用例加依赖。
    # 例：fpgalsp_add_unit_test(adc fpga_lsp::constraints)
    set(_extra_libs ${ARGN})
    add_executable(unit_${name} ${name}_test.cpp)
    target_include_directories(unit_${name} PRIVATE "${CMAKE_SOURCE_DIR}/src")
    # 注意：不要在基础库里放 fpga_lsp::constraints。
    #   constraints 是整个工程里最容易处于"半成品"状态的模块（新规则通常先写解析再补测试）。
    #   一旦把它放进所有 unit target 的公共依赖，constraints 编译失败会让 **全部** 单测都构建不了，
    #   其他人无法判断"是我改坏了还是别人的模块坏了"。
    #   实测发生过一次：constraints WIP 期间 22 个 unit target 全部无法构建。
    target_link_libraries(unit_${name} PRIVATE
        fpga_lsp::warnings
        fpga_lsp::platform
        fpga_lsp::jsonrpc
        fpga_lsp::lsp
        fpga_lsp::project
        fpga_lsp::td
        fpga_lsp::reports
        ${_extra_libs}
        GTest::gtest
        GTest::gtest_main
    )
    target_compile_features(unit_${name} PRIVATE cxx_std_23)
    # 供需要访问仓库内语料（tests/gate-corpus 等）的用例使用
    target_compile_definitions(unit_${name} PRIVATE
        FPGA_LSP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME unit.${name} COMMAND unit_${name})
    set_tests_properties(unit.${name} PROPERTIES LABELS "unit")
endfunction()

fpgalsp_add_unit_test(utf16)
fpgalsp_add_unit_test(path)
fpgalsp_add_unit_test(encoding)
fpgalsp_add_unit_test(json_util)
fpgalsp_add_unit_test(transport)
fpgalsp_add_unit_test(dispatcher)
fpgalsp_add_unit_test(document_store)
fpgalsp_add_unit_test(al_project)
fpgalsp_add_unit_test(td_install)
fpgalsp_add_unit_test(td_output)
fpgalsp_add_unit_test(td_device)
fpgalsp_add_unit_test(diagnostic)
fpgalsp_add_unit_test(td_diagnostics)

# 阶段 4-γ：TD 报告解析（QoR / Timing / Area / FlowStatus / 产物 / 汇总）
fpgalsp_add_unit_test(reports_qor)
fpgalsp_add_unit_test(reports_area)
fpgalsp_add_unit_test(reports_flow_status)
fpgalsp_add_unit_test(reports_timing)
fpgalsp_add_unit_test(reports_artifacts)
fpgalsp_add_unit_test(reports_aggregate)
fpgalsp_add_unit_test(reports_parity)

# 阶段 4-δ：Flow 引擎 + BitWizard 烧录 + 策略对比
fpgalsp_add_unit_test(flow_logic fpga_lsp::flow)
fpgalsp_add_unit_test(flow_runner fpga_lsp::flow)
fpgalsp_add_unit_test(flow_programmer fpga_lsp::flow)
fpgalsp_add_unit_test(flow_strategy fpga_lsp::flow)

# 阶段 5-α：ADC 引脚约束 + SDC 时序约束（纯文本解析，无外部依赖）
#   adc：真实语料 tests/gate-corpus/corpus_alpha/constrains/pin.adc + Python 金标准逐字段比对
#   sdc：真实语料 top.sdc + 浅层 Tcl 词法/诊断/补全
#   只有这两个用例需要 constraints 库（见上面函数里的说明：不放进公共依赖）
fpgalsp_add_unit_test(adc fpga_lsp::constraints)
fpgalsp_add_unit_test(sdc fpga_lsp::constraints)

# 阶段 3：HDL 索引（Verible 词法 + 结构识别）
#   只给这一个用例加 fpga_lsp::hdl —— 它传递依赖 Verible 解析链路（几十个静态库），
#   放进公共依赖会让每个 unit target 的链接时间都变长。
fpgalsp_add_unit_test(hdl_index fpga_lsp::hdl)

# 阶段 5-β：MCP server（协议层 + 39 个工具表）
#   mcp_server：版本协商 / tools/list 可见性 / required 校验 / isError 语义 / 截断
#   mcp_tools ：39 个工具的 schema 不变量（逐字段对齐见 tests/compare_mcp_schema.py）
#   只给这两个用例加 fpga_lsp::mcp（它传递依赖 constraints/reports/td）。
fpgalsp_add_unit_test(mcp_server fpga_lsp::mcp)
fpgalsp_add_unit_test(mcp_tools fpga_lsp::mcp)
fpgalsp_add_unit_test(mcp_bridge fpga_lsp::mcp)

# Phase B：能力层（LSP 与 MCP 共用实现）。B1 只建骨架，B2/B3/B4/B5/B10 填内容。
#   server 库提供 LspServer::Feature*；hdl 提供索引；constraints 只给约束符号用例。
fpgalsp_add_unit_test(semantics fpga_lsp::server fpga_lsp::hdl)
fpgalsp_add_unit_test(code_actions fpga_lsp::server fpga_lsp::hdl)
fpgalsp_add_unit_test(rename fpga_lsp::server fpga_lsp::hdl)
fpgalsp_add_unit_test(call_hierarchy fpga_lsp::server fpga_lsp::hdl)
fpgalsp_add_unit_test(analysis fpga_lsp::server)
fpgalsp_add_unit_test(constraint_symbols fpga_lsp::server fpga_lsp::constraints fpga_lsp::hdl)

# Phase 2 / A3：报告缓存（ReportStore 需要 server 层）
fpgalsp_add_unit_test(report_store fpga_lsp::server)

# B-A2：executeCommand 的声明与分发表一致（capability 由 ExecuteCommandNames() 生成）
fpgalsp_add_unit_test(commands fpga_lsp::server)

# Phase 2 / A1：增量索引（IndexService + hdl 增量 API）
fpgalsp_add_unit_test(index_service fpga_lsp::server fpga_lsp::hdl)

# T1 / task-50：服务端 lint（LintService 调度 + 注释掩码 + 诊断按来源合并）
fpgalsp_add_unit_test(lint_service fpga_lsp::server fpga_lsp::hdl fpga_lsp::verible_bridge)
