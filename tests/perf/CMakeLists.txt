# =============================================================================
#  tests/perf/ —— 性能探针（不进 ctest）
# =============================================================================
#  目的：给"当前瓶颈在哪"提供可复现的进程内计时，不依赖外部 profiler。
#
#  用法：
#    1. 在本目录放 probe_<area>.cpp，实现 int main()（include "perf_common.h"）
#    2. cmake --build build --target perf_<area>
#    3. build/bin/perf_<area>.exe --iters 50
#
#  约定：
#    * 不 add_test()。性能数字受机器负载影响，不能当门禁。
#    * 文件名即目标名：probe_index.cpp -> perf_index。
#    * 多个 probe 之间互不依赖；不要在这里放共享状态实现（共享代码放
#      perf_common.h，或者各自 probe 内部 static）。
# =============================================================================

file(GLOB _fpga_perf_probes CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/probe_*.cpp")

foreach(_fpga_probe_src IN LISTS _fpga_perf_probes)
    get_filename_component(_fpga_probe_file "${_fpga_probe_src}" NAME_WE)   # probe_index
    string(SUBSTRING "${_fpga_probe_file}" 6 -1 _fpga_probe_area)           # index

    add_executable(perf_${_fpga_probe_area} "${_fpga_probe_src}")
    target_include_directories(perf_${_fpga_probe_area} PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(perf_${_fpga_probe_area} PRIVATE
        fpga_lsp::warnings
        fpga_lsp::platform
        fpga_lsp::jsonrpc
        fpga_lsp::lsp
        fpga_lsp::project
        fpga_lsp::td
        fpga_lsp::reports
        fpga_lsp::hdl
        fpga_lsp::analysis
        fpga_lsp::semantics
        fpga_lsp::flow
        fpga_lsp::constraints
        fpga_lsp::verible_bridge
        fpga_lsp::server
        fpga_lsp::mcp)
    target_compile_features(perf_${_fpga_probe_area} PRIVATE cxx_std_23)
    target_compile_definitions(perf_${_fpga_probe_area} PRIVATE
        FPGA_LSP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endforeach()

if(_fpga_perf_probes)
    list(LENGTH _fpga_perf_probes _fpga_perf_count)
    message(STATUS "tests/perf: 已注册 ${_fpga_perf_count} 个性能探针（不进 ctest）")
endif()