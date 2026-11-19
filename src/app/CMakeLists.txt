# =============================================================================
#  src/app/ —— 可执行入口
# =============================================================================

add_executable(fpgalsp-native
    main.cpp
    mcp_tcp.cpp
)

target_include_directories(fpgalsp-native PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(fpgalsp-native PRIVATE
    fpga_lsp::warnings
    fpga_lsp::jsonrpc
    fpga_lsp::platform
    fpga_lsp::server
    # MCP 模式（--mcp）：39 个工具；依赖 feature 层，不依赖 server
    fpga_lsp::mcp
)

if(WIN32)
    target_link_libraries(fpgalsp-native PRIVATE ws2_32)
endif()

target_compile_features(fpgalsp-native PRIVATE cxx_std_23)

# MSVC：可执行文件用静态 CRT 时保持与库一致（顶层已全局设置 CMAKE_MSVC_RUNTIME_LIBRARY）
set_target_properties(fpgalsp-native PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)