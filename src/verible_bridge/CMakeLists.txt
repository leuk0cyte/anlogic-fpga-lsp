# =============================================================================
#  src/verible_bridge/ —— 内嵌 Verible 的可选能力桥
# =============================================================================
#  只有 FPGA_LSP_VERIBLE_ENABLED=ON 时才由 src/CMakeLists.txt add_subdirectory 进来。
#  目的：让 src/server 能用 formatter，又不必把 Verible 的 include 根 / absl 依赖 /
#        /bigobj 等构建细节扩散到服务层。
# =============================================================================

if(NOT TARGET verible_formatter_libs)
    message(FATAL_ERROR
        "verible_bridge: 未找到聚合 target `verible_formatter_libs`。\n"
        "  它由 cmake/Verible.cmake 在 FPGA_LSP_VERIBLE_ENABLED=ON 时定义。")
endif()

add_library(fpga_lsp_verible_bridge STATIC
    format_text.cpp
    lint_text.cpp
)
add_library(fpga_lsp::verible_bridge ALIAS fpga_lsp_verible_bridge)

target_include_directories(fpga_lsp_verible_bridge PUBLIC
    "${CMAKE_SOURCE_DIR}/src"
)
# Verible 的头文件以 "verible/..." 开头，需要 thirdparty/verible 作为 include 根
target_include_directories(fpga_lsp_verible_bridge PRIVATE
    "${CMAKE_SOURCE_DIR}/thirdparty/verible"
)

if(COMMAND fpga_lsp_default_warnings)
    fpga_lsp_default_warnings(fpga_lsp_verible_bridge)
endif()

target_compile_features(fpga_lsp_verible_bridge PUBLIC cxx_std_23)

target_link_libraries(fpga_lsp_verible_bridge PUBLIC
    verible_formatter_libs
)

# lint autofix（B3）：Verible 的 lint 规则靠静态全局对象自注册，必须整库 /WHOLEARCHIVE
# 链入；否则规则静默消失（程序正常、退出码 0、autofix 永远为空）。
if(TARGET verible_lint_rules_all)
    target_link_libraries(fpga_lsp_verible_bridge PUBLIC verible_lint_rules_all)
    message(STATUS "src/verible_bridge: lint autofix 已接入（内嵌规则库 + /WHOLEARCHIVE）")
else()
    message(FATAL_ERROR
        "src/verible_bridge: 未找到 verible_lint_rules_all；lint autofix 需要完整规则库")
endif()

# 让 src/server 能用 `#if defined(FPGA_LSP_HAVE_VERIBLE_BRIDGE)` 决定是否走真实格式化。
# 用 INTERFACE 定义，链接者自动获得。
target_compile_definitions(fpga_lsp_verible_bridge
    INTERFACE FPGA_LSP_HAVE_VERIBLE_BRIDGE=1)

# -----------------------------------------------------------------------------
# ⚠ 与 format_dump 相同的链接顺序坑
# -----------------------------------------------------------------------------
# `verible_parser_libs` 的 /WHOLEARCHIVE 是通过 INTERFACE link options 传播的，
# 但本 target 额外引入了 formatting 归档后，链接顺序变化，MSVC 可能在
# /WHOLEARCHIVE 生效前就按"无人引用"丢掉 .tab.cc / flex 生成的成员，报
#   LNK2001: verilog::verilog_parse / verilog_debug / verilog_symbol_name
#   LNK2019: verilogFlexLexer::yy_pop_state / VerilogLexer::yylex
# 处置：在本 target 上再声明一次「link 一次 + /WHOLEARCHIVE」。
# 详见 src/tools/format_dump/CMakeLists.txt 的同名注释。
if(MSVC)
    foreach(_wa IN ITEMS
            verible__verilog__parser__verilog-y-cc
            verible__verilog__parser__verilog-lexer
            verible__common__analysis__command-file-lexer)
        if(TARGET ${_wa})
            target_link_libraries(fpga_lsp_verible_bridge PUBLIC "${_wa}")
            target_link_options(fpga_lsp_verible_bridge INTERFACE
                "/WHOLEARCHIVE:$<TARGET_FILE:${_wa}>")
        endif()
    endforeach()
endif()

message(STATUS "src/verible_bridge: formatter 桥已启用（全文件格式化，默认 column_limit=160）")