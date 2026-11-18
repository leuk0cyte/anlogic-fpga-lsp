# =============================================================================
#  src/jsonrpc/ —— JSON-RPC 传输与分派层
# =============================================================================

add_library(fpga_lsp_jsonrpc STATIC
    json_util.cpp
    transport.cpp
    dispatcher.cpp
)

add_library(fpga_lsp::jsonrpc ALIAS fpga_lsp_jsonrpc)

target_include_directories(fpga_lsp_jsonrpc PUBLIC
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(fpga_lsp_jsonrpc PUBLIC
    fpga_lsp::warnings
    fpga_lsp::platform
    nlohmann_json::nlohmann_json
)

target_compile_features(fpga_lsp_jsonrpc PUBLIC cxx_std_23)