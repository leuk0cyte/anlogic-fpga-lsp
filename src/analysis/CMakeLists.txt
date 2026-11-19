# =============================================================================
#  src/analysis/ —— 流程后分析（DRC / CDC）+ 工程版本同步
# =============================================================================
#  对应参考实现：Y:\fpga-lsp\fpgalsp\analysis.py（run/newest_routed_db/cache）
#                与 Y:\fpga-lsp\fpgalsp\server.py::on_workspace_executeCommand
#                （fpgalsp.updateProject）
#
#  约定：
#   * 报告解析用 re2（DRC 报告可能很大；std::regex 有栈溢出/回溯风险），
#     与 src/reports/ 保持同一选择。
#   * 解析层 never-throw；只有真正启动 TD 的入口才可能抛，并在入口处兜底成
#     `{ok:false, error}`。
#   * 只依赖 td / project / platform / jsonrpc 四个下层库 + re2，不反向依赖 server。
# =============================================================================

add_library(fpga_lsp_analysis STATIC
    analysis.cpp
)

add_library(fpga_lsp::analysis ALIAS fpga_lsp_analysis)

target_include_directories(fpga_lsp_analysis PUBLIC
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(fpga_lsp_analysis PUBLIC
    fpga_lsp::warnings
    fpga_lsp::platform
    fpga_lsp::jsonrpc
    fpga_lsp::project
    # 短生命周期 TD 会话（TdSession）+ 器件导入（EnsureDevice）+ TD 输出判错（HasError）
    fpga_lsp::td
    re2::re2
)

target_compile_features(fpga_lsp_analysis PUBLIC cxx_std_23)