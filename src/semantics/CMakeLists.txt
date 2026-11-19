# =============================================================================
#  src/semantics/ —— 语义层纯逻辑（LSP 与 MCP 共用的唯一实现）
# =============================================================================
#  OWNER: task B2 (parity-lsp)。
#
#  依赖方向：semantics -> hdl（索引）+ platform（UTF-16 换算）+ verible 解析链
#  不依赖 server —— 这样 unit.semantics 可以直接测纯函数。
# =============================================================================

add_library(fpga_lsp_semantics STATIC
    semantics.cpp
)

add_library(fpga_lsp::semantics ALIAS fpga_lsp_semantics)

target_include_directories(fpga_lsp_semantics PUBLIC
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(fpga_lsp_semantics PUBLIC
    fpga_lsp::warnings
    fpga_lsp::platform
    fpga_lsp::hdl
    # Verible 原始词法流（verilog::VerilogLexer）：语义高亮的 operator/comment token
    # 必须与 Python 的 `verible-verilog-syntax --printrawtokens` 同源。
    verible_parser_libs
)

target_compile_features(fpga_lsp_semantics PUBLIC cxx_std_23)