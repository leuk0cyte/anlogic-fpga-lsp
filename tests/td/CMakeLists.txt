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

# -----------------------------------------------------------------------------
# A5 / task-25：单飞队列 / 取消 / 去抖 / 按文件切分（**不需要真实 TD**）
#   用假 runner 把策略与 TD 进程解耦，毫秒级，标 LABEL "unit" 让默认 ctest 也跑。
# -----------------------------------------------------------------------------
add_executable(unit_td_service td_service_test.cpp)
target_include_directories(unit_td_service PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(unit_td_service PRIVATE
    fpga_lsp::warnings fpga_lsp::td fpga_lsp::project fpga_lsp::lsp
    GTest::gtest GTest::gtest_main)
target_compile_features(unit_td_service PRIVATE cxx_std_23)

add_test(NAME unit.td_service COMMAND unit_td_service)
set_tests_properties(unit.td_service PROPERTIES
    LABELS "unit;td-unit"
    TIMEOUT 120)

# -----------------------------------------------------------------------------
# A5 / task-25：分段计时 + 预热评估的真机基准（**需要真实 TD**，默认不跑）
#   ctest -L bench --output-on-failure
# -----------------------------------------------------------------------------
add_executable(td_bench td_bench.cpp)
target_include_directories(td_bench PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(td_bench PRIVATE
    fpga_lsp::warnings fpga_lsp::td fpga_lsp::project fpga_lsp::lsp
    fpga_lsp::platform)
target_compile_features(td_bench PRIVATE cxx_std_23)
target_compile_definitions(td_bench PRIVATE
    FPGA_LSP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

add_test(NAME td.bench COMMAND td_bench)
set_tests_properties(td.bench PROPERTIES
    LABELS "bench;td-process-bench"
    TIMEOUT 1800
    RUN_SERIAL TRUE
    RESOURCE_LOCK "td-process")

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

# -----------------------------------------------------------------------------
#  release 分支：这些用例需要真实 TD 安装 + 自备工程（会真跑综合/布线）。
#  未设置 FPGA_LSP_TEST_PROJECT 时标记为跳过。
# -----------------------------------------------------------------------------
if(NOT DEFINED ENV{FPGA_LSP_TEST_PROJECT})
    message(STATUS "tests/td: 未设置 FPGA_LSP_TEST_PROJECT —— 真机 TD 用例标记为跳过")
    foreach(_td IN ITEMS td.flow td.diagnostics td.session td.bench)
        if(TEST ${_td})
            set_tests_properties(${_td} PROPERTIES DISABLED TRUE)
        endif()
    endforeach()
endif()
