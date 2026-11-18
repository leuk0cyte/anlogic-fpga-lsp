# =============================================================================
#  src/mcp/ —— MCP server（39 个工具，stdio newline-delimited JSON-RPC）
# =============================================================================
#  参考实现：Y:\fpga-lsp\fpgalsp\mcp.py（协议）+ mcp_bridge.py（实现）
#
#  依赖方向：mcp -> {lsp, project, td, reports, constraints, platform, jsonrpc}
#  刻意**不**依赖 server / hdl：MCP 是独立的对外入口，不该被 LSP 服务层的
#  实现细节绑死；符号类工具在 hdl 索引就绪后经 feature 层接入（见 bridge.h）。
# =============================================================================

add_library(fpga_lsp_mcp STATIC
    bridge.cpp
    mcp_server.cpp
    tools.cpp
)

add_library(fpga_lsp::mcp ALIAS fpga_lsp_mcp)

target_include_directories(fpga_lsp_mcp PUBLIC
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(fpga_lsp_mcp PUBLIC
    fpga_lsp::warnings
    fpga_lsp::platform
    fpga_lsp::jsonrpc
    fpga_lsp::lsp
    fpga_lsp::project
    fpga_lsp::td
    fpga_lsp::reports
    fpga_lsp::constraints
    # 符号类工具（阶段二 A 批）：
    #   hdl    —— module_info / duplicate_modules / search_hierarchy / symbol_path / call_hierarchy
    #   server —— 只为 server::features（features.cpp 编译在 server 库内）：
    #             documentSymbol/workspaceSymbol/definition/hover/references/moduleTree 的
    #             **唯一实现**，LSP handler 与 MCP 工具共用，避免两份 JSON 转换漂移。
    #             只调用 features.h 的无状态纯函数，不构造 LspServer、不碰它的 transport。
    fpga_lsp::hdl
    # flow：flow_run / flow_cancel / flow_wait / strategy_compare / cables 直接使用
    # FlowRunner / CompareStrategies / Inspect（与 LSP 同一实现）。显式声明而不是靠
    # server 传递依赖 —— 否则将来 server 不再链接 flow 时会在这里炸。
    fpga_lsp::flow
    fpga_lsp::server
    re2::re2
    nlohmann_json::nlohmann_json
)

target_compile_features(fpga_lsp_mcp PUBLIC cxx_std_23)