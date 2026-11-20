# =============================================================================
#  src/flow/ —— Flow 引擎（一键综合/布线）+ BitWizard 烧录 + 策略对比
# =============================================================================
#  对应参考实现：Y:\fpga-lsp\fpgalsp\flow.py / strategy.py / program.py
#
#  约定：
#   * 不抛异常：所有失败都体现在返回的 JSON / FlowState.error 里
#     （worker 跑在独立线程里，异常逃出去会 terminate 进程）。
#   * 纯判定逻辑放 flow_logic.cpp，进程驱动放 flow_runner.cpp ——
#     PRJ-8806 三条件、stall、完成标记这些能在毫秒级单测里覆盖。
#   * 用 re2，不用 std::regex。
# =============================================================================

add_library(fpga_lsp_flow STATIC
    flow_logic.cpp
    flow_runner.cpp
    programmer.cpp
    probe.cpp
    strategy.cpp
)

add_library(fpga_lsp::flow ALIAS fpga_lsp_flow)

target_include_directories(fpga_lsp_flow PUBLIC
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(fpga_lsp_flow PUBLIC
    fpga_lsp::warnings
    fpga_lsp::platform
    fpga_lsp::jsonrpc
    fpga_lsp::project
    # flow 跑完直接调 BuildReports 汇总报告（flow 与 reports 强耦合）
    fpga_lsp::reports
    # 独立 TD 会话 / import_device / 输出判定
    fpga_lsp::td
    re2::re2
)

target_compile_features(fpga_lsp_flow PUBLIC cxx_std_23)