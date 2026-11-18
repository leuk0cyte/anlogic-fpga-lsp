# =============================================================================
#  src/hdl/ —— HDL 符号索引（Verible 词法 + hdl.py 的结构识别算法）
# =============================================================================
#  依赖方向：hdl -> verible(解析链路) + platform + project
#  不依赖 server（server 反过来依赖 hdl），保证可以单独单测。
# =============================================================================

add_library(fpga_lsp_hdl STATIC
    hdl_index.cpp
)

add_library(fpga_lsp::hdl ALIAS fpga_lsp_hdl)

target_include_directories(fpga_lsp_hdl PUBLIC
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(fpga_lsp_hdl PUBLIC
    fpga_lsp::warnings
    fpga_lsp::platform
    fpga_lsp::project
    # td::IpGeneratedHdl：.ipc -> 同名 HDL 的定位逻辑复用 td 层实现（只调用，不改语义）
    fpga_lsp::td
    # VHDL entity 提取 / 错误消息解析用 re2
    re2::re2
)

# Verible 解析链路（VerilogAnalyzer + CST + 其传递依赖）。
# HDL 索引没有 Verible 就只剩 VHDL entity 提取 —— 那不是"能力降级"而是"功能缺失"，
# 所以这里直接 FATAL_ERROR，避免出现一个看起来正常却扫不出符号的索引。
if(NOT TARGET verible_parser_libs)
    message(FATAL_ERROR
        "\n"
        "  src/hdl 需要 verible_parser_libs（即 FPGA_LSP_VERIBLE_ENABLED=ON）。\n"
        "  HDL 索引的 token 全部来自 Verible；没有它就无法实现 moduleTree/符号类 handler。\n"
        "  请用 preset 配置：cmake --preset windows-msvc-release\n")
endif()
target_link_libraries(fpga_lsp_hdl PUBLIC verible_parser_libs)

target_compile_features(fpga_lsp_hdl PUBLIC cxx_std_23)