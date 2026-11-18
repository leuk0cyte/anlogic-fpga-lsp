# =============================================================================
#  src/lsp/ —— LSP 协议层（文档同步等）
# =============================================================================

add_library(fpga_lsp_lsp STATIC
    document_store.cpp
    diagnostic.cpp
)

add_library(fpga_lsp::lsp ALIAS fpga_lsp_lsp)

target_include_directories(fpga_lsp_lsp PUBLIC
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(fpga_lsp_lsp PUBLIC
    fpga_lsp::warnings
    fpga_lsp::platform
    nlohmann_json::nlohmann_json
)

target_compile_features(fpga_lsp_lsp PUBLIC cxx_std_23)