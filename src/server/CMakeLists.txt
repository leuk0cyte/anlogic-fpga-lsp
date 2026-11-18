# =============================================================================
#  src/server/ —— LSP 服务主体（编排层，无 Verible 依赖）
# =============================================================================
#  职责：把 platform / jsonrpc / lsp / project / td 各层接成"编辑器可用"的闭环。
#  这一层只做**编排**，不重复实现下层语义（见 lsp_server.h 文件头）。
# =============================================================================

add_library(fpga_lsp_server STATIC
    lsp_server.cpp
    commands.cpp
    features.cpp
    hdl_handlers.cpp
)

add_library(fpga_lsp::server ALIAS fpga_lsp_server)

target_include_directories(fpga_lsp_server PUBLIC
    "${CMAKE_SOURCE_DIR}/src"
)

# PUBLIC：app 只需要链接 fpga_lsp::server 就能拿到全部下层能力
target_link_libraries(fpga_lsp_server PUBLIC
    fpga_lsp::warnings
    fpga_lsp::platform
    fpga_lsp::jsonrpc
    fpga_lsp::lsp
    fpga_lsp::project
    fpga_lsp::td
    # HDL 符号索引（符号类 handler：moduleTree / documentSymbol / symbol / definition / hover / completion）
    fpga_lsp::hdl
    re2::re2
)

# flow 引擎（runFlow / cancelFlow / compareStrategies / program / svfgen）。
# 用 if(TARGET) 包住：flow 模块比 server 后落地，缺它时 server 仍应可构建。
if(TARGET fpga_lsp::flow)
    target_link_libraries(fpga_lsp_server PUBLIC fpga_lsp::flow)
else()
    message(STATUS "src/server: 未找到 fpga_lsp::flow，flow 命令保持占位")
endif()

# 内嵌 Verible 的可选能力桥（formatter）。
#   链接它就自动获得 FPGA_LSP_HAVE_VERIBLE_BRIDGE=1（INTERFACE 定义），
#   于是 textDocument/formatting 会走真实 Verible formatter 而不是返回 []。
#   未链接时（FPGA_LSP_VERIBLE_ENABLED=OFF）保持原来的"合法空结果"。
if(TARGET fpga_lsp::verible_bridge)
    target_link_libraries(fpga_lsp_server PUBLIC fpga_lsp::verible_bridge)
    message(STATUS "src/server: formatter 桥已接入（textDocument/formatting 为真实实现）")
else()
    message(STATUS "src/server: 无 formatter 桥 → textDocument/formatting 返回空编辑")
endif()

target_compile_features(fpga_lsp_server PUBLIC cxx_std_23)

target_compile_definitions(fpga_lsp_server PRIVATE
    FPGA_LSP_VERSION="0.1.0"
)