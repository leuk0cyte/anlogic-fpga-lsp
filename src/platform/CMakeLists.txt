# =============================================================================
#  src/platform/ —— 平台层（UTF-16 位置换算 / 路径 / 日志 / 编码）
# =============================================================================

add_library(fpga_lsp_platform STATIC
    utf16.cpp
    path.cpp
    log.cpp
    encoding.cpp
    write_service.cpp
)

add_library(fpga_lsp::platform ALIAS fpga_lsp_platform)

target_include_directories(fpga_lsp_platform PUBLIC
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(fpga_lsp_platform PUBLIC
    fpga_lsp::warnings
    spdlog::spdlog
    simdutf::simdutf
)

target_compile_features(fpga_lsp_platform PUBLIC cxx_std_23)