# =============================================================================
#  tests/td/ —— 真实 TD 集成测试
# =============================================================================
#  这些测试会启动真实的 td_commands_prompt.exe，因此标记 LABEL "td"：
#    * 默认 `ctest` 不会跑
#    * `ctest -L td --output-on-failure` 才跑
# =============================================================================

if(NOT TARGET fpga_lsp::td)
    message(STATUS "tests/td: 未找到 fpga_lsp::td，跳过 TD 集成测试")
    return()
endif()

add_executable(unit_td_session td_session_test.cpp)

target_include_directories(unit_td_session PRIVATE "${CMAKE_SOURCE_DIR}/src")

target_link_libraries(unit_td_session PRIVATE
    fpga_lsp::warnings
    fpga_lsp::td
    fpga_lsp::project
    GTest::gtest
    GTest::gtest_main
)

target_compile_features(unit_td_session PRIVATE cxx_std_23)
target_compile_definitions(unit_td_session PRIVATE
    FPGA_LSP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

add_test(NAME td.session COMMAND unit_td_session)
set_tests_properties(td.session PROPERTIES
    LABELS "td"
    TIMEOUT 900
    RUN_SERIAL TRUE
    RESOURCE_LOCK "td-process"  # 同一时刻只跑一个 TD 会话，避免进程计数互相干扰
)

# -----------------------------------------------------------------------------
#  TD 诊断映射真实集成测试（阶段 4-β）
# -----------------------------------------------------------------------------
add_executable(unit_td_diagnostics_integration td_diagnostics_integration_test.cpp)
target_include_directories(unit_td_diagnostics_integration PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(unit_td_diagnostics_integration PRIVATE
    fpga_lsp::warnings fpga_lsp::td fpga_lsp::project fpga_lsp::lsp
    GTest::gtest GTest::gtest_main)
target_compile_features(unit_td_diagnostics_integration PRIVATE cxx_std_23)
target_compile_definitions(unit_td_diagnostics_integration PRIVATE
    FPGA_LSP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

add_test(NAME td.diagnostics COMMAND unit_td_diagnostics_integration)
set_tests_properties(td.diagnostics PROPERTIES
    LABELS "td"
    TIMEOUT 900
    RUN_SERIAL TRUE
    RESOURCE_LOCK "td-process")

# -----------------------------------------------------------------------------
#  Flow 引擎真实集成测试（阶段 4-δ）
#    * 跑一次真实综合（约 10-40 s）
#    * 断言产物、flowStatus 字段、0 个残留 td_commands_prompt.exe
#    * 取消后同样 0 残留（Job Object 终止整棵进程树）
# -----------------------------------------------------------------------------
if(TARGET fpga_lsp::flow)
    add_executable(unit_flow_integration flow_integration_test.cpp)
    target_include_directories(unit_flow_integration PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(unit_flow_integration PRIVATE
        fpga_lsp::warnings fpga_lsp::flow fpga_lsp::reports fpga_lsp::td fpga_lsp::project
        fpga_lsp::platform fpga_lsp::jsonrpc
        GTest::gtest GTest::gtest_main)
    target_compile_features(unit_flow_integration PRIVATE cxx_std_23)
    target_compile_definitions(unit_flow_integration PRIVATE
        FPGA_LSP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

    add_test(NAME td.flow COMMAND unit_flow_integration)
    set_tests_properties(td.flow PROPERTIES
        LABELS "td"
        TIMEOUT 1800
        RUN_SERIAL TRUE
    RESOURCE_LOCK "td-process")
endif()
