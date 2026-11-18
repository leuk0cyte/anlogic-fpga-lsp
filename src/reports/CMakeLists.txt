# =============================================================================
#  src/reports/ —— TD 报告解析（QoR / Timing / Area / FlowStatus / 产物）
# =============================================================================
#  对应参考实现：Y:\fpga-lsp\fpgalsp\reports.py + timing_map.py
#
#  约定：
#   * 用 re2 而不是 std::regex —— 真实报告可达数 MB（final_timing.rpt 366 KB、
#     行数 3928），std::regex 的正则编译与匹配开销不可接受。
#   * 所有公开函数 never-throw；畸形输入返回空结构 + warnings。
# =============================================================================

add_library(fpga_lsp_reports STATIC
    report_text.cpp
    qor.cpp
    area.cpp
    flow_status.cpp
    timing.cpp
    artifacts.cpp
    reports_aggregate.cpp
)

add_library(fpga_lsp::reports ALIAS fpga_lsp_reports)

target_include_directories(fpga_lsp_reports PUBLIC
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(fpga_lsp_reports PUBLIC
    fpga_lsp::warnings
    # 文件读取走 platform::read_file_utf8（剥 BOM + 非法字节替换成 U+FFFD，
    # 等价 Python 的 encoding="utf-8", errors="replace"）
    fpga_lsp::platform
    # 输出结构直接是 nlohmann::json（server 层不再做一次转换）
    fpga_lsp::jsonrpc
    # BuildReports(const project::Project&) 需要一个解析好的 .al 工程
    fpga_lsp::project
    re2::re2
)

target_compile_features(fpga_lsp_reports PUBLIC cxx_std_23)