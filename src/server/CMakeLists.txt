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
    # Phase B：能力层的唯一实现（每个文件一个 owner；B1 先放 stub）
    features_semantics.cpp
    features_code_actions.cpp
    features_rename.cpp
    features_call_hierarchy.cpp
    features_constraints.cpp
    features_analysis.cpp
    features_commands.cpp
    ide_features.cpp
    async_dispatcher.cpp
    report_store.cpp
    index_service.cpp
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

# DRC/CDC 后流程分析（Phase B / B5）。features_analysis.cpp 通过它实现 runAnalysis。
if(TARGET fpga_lsp::analysis)
    target_link_libraries(fpga_lsp_server PUBLIC fpga_lsp::analysis)
    message(STATUS "src/server: analysis 已接入（fpgalsp.runAnalysis 为真实实现）")
else()
    message(STATUS "src/server: 未找到 fpga_lsp::analysis，runAnalysis 保持 stub")
endif()

# 语义高亮 / 折叠（Phase B / B2）。FeatureSemanticTokens 依赖它。
if(TARGET fpga_lsp::semantics)
    target_link_libraries(fpga_lsp_server PUBLIC fpga_lsp::semantics)
    message(STATUS "src/server: semantics 已接入（semanticTokens/foldingRange 为真实实现）")
else()
    message(STATUS "src/server: 未找到 fpga_lsp::semantics，语义高亮保持 stub")
endif()

# .adc/.sdc 符号（Phase B / B10）。features_constraints.cpp 直接调 constraints::adc/sdc。
if(TARGET fpga_lsp::constraints)
    target_link_libraries(fpga_lsp_server PUBLIC fpga_lsp::constraints)
    message(STATUS "src/server: constraints 已接入（.adc/.sdc 符号/悬停/补全为真实实现）")
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