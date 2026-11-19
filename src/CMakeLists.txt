# =============================================================================
#  src/ — fpga-lsp-cpp 本体源码
# =============================================================================
#  阶段 0-C 只建目录骨架；具体 target 由后续任务按目录补充。
#
#  约定：
#   * 本项目自有代码统一 link `fpga_lsp::warnings`（等价于
#     fpga_lsp_default_warnings()，/W4 + /permissive- + UTF-8），
#     **不要**把它用到 thirdparty 上（第三方向来是 /W0）。
#   * Verible 相关目标必须用 if(FPGA_LSP_VERIBLE_ENABLED) 包住，因为
#     Verible 未接入时 fpga_lsp::verible 只是一个空的 INTERFACE target。
# =============================================================================

# -----------------------------------------------------------------------------
# 统一的警告/编译特性载体（供本项目自有代码 link）
# -----------------------------------------------------------------------------
if(NOT TARGET fpga_lsp_warnings)
    add_library(fpga_lsp_warnings INTERFACE)
    if(MSVC)
        target_compile_options(fpga_lsp_warnings INTERFACE
            /W4
            /permissive-
            /utf-8
            /Zc:__cplusplus
            /Zc:preprocessor
            /wd4324   # structure was padded due to alignment specifier
        )
    endif()
    target_compile_features(fpga_lsp_warnings INTERFACE cxx_std_23)
endif()
if(NOT TARGET fpga_lsp::warnings)
    add_library(fpga_lsp::warnings ALIAS fpga_lsp_warnings)
endif()

message(STATUS "src/: 目标骨架已就位")

# -----------------------------------------------------------------------------
#  可选的 LTO（/GL + /LTCG）—— 只作用于**本项目自有代码**，不动 thirdparty
# -----------------------------------------------------------------------------
#  为什么不是直接用 CMAKE_INTERPROCEDURAL_OPTIMIZATION：那样连 Verible 的
#  flex 生成 lexer 一起进 /GL，链接期会稳定失败 35 个未解析符号
#  （verilogFlexLexer::* / veribleCommandFileFlexLexer::* / verilog_parse /
#   verilog_debug / verilog_symbol_name），/WHOLEARCHIVE 也救不回。
#  复现记录见 audit/2026-11-19/static/LTCG.md 与
#  audit/2026-11-18/phase2/B2-B3-REPORT.md（两次历史尝试）。
#
#  这里的做法：只给 src/ 子树（自有代码）加 /GL，thirdparty 保持原生机器码，
#  链接期再加 /LTCG。MSVC 支持 /GL 对象与原生对象混合链接，第三方目标不进
#  LTO 图，因此无需改造 cmake/Verible.cmake 的生成物。
#
#  默认 OFF：这是可选优化，不是构建正确性的必需项。
option(FPGA_LSP_LTO_OWN_CODE "只对本项目自有代码启用 LTO（/GL + /LTCG）" OFF)
if(FPGA_LSP_LTO_OWN_CODE AND MSVC)
    add_compile_options($<$<CONFIG:Release>:/GL>)
    add_link_options(
        $<$<CONFIG:Release>:/LTCG>
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )
    # /GL 会写 IL 而非机器码，目标文件变大、链接变慢，但跨 TU 内联/去虚化有机会
    # 抵消。是否值得由构建耗时与二进制体积的 A/B 决定（见 LTCG.md 的实测表）。
    message(STATUS "src/: LTO 仅对自有代码启用（/GL + /LTCG，thirdparty 保持原生）")
endif()

# -----------------------------------------------------------------------------
# 已接入的子目录
# -----------------------------------------------------------------------------
# 阶段 2-α：平台层 + JSON-RPC 传输层（不依赖 Verible）
add_subdirectory(platform)
add_subdirectory(jsonrpc)
add_subdirectory(app)

# 阶段 2-β：文档同步 + .al 工程模型（不依赖 Verible）
add_subdirectory(lsp)
add_subdirectory(project)

# 阶段 4-α：TangDynasty 会话层（对真实 TD 进程的封装）
add_subdirectory(td)

# 阶段 4-γ：TD 报告解析（QoR / Timing / Area / FlowStatus / 产物探测）
add_subdirectory(reports)

# Phase B：DRC/CDC 后流程分析（src/analysis，B5 新增；server 的 FeatureRunAnalysis 依赖）
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/analysis/CMakeLists.txt")
    add_subdirectory(analysis)
endif()

# 阶段 4-δ：Flow 引擎（一键综合/布线）+ BitWizard 烧录 + 策略对比
add_subdirectory(flow)

# 阶段 3：HDL 符号索引（Verible 词法 + 结构识别；server 的符号类 handler 依赖它）
#   必须排在 server 之前：server 链接 fpga_lsp::hdl。
add_subdirectory(hdl)

# Phase B：语义高亮 / 折叠（src/semantics，B2 新增；server 的 FeatureSemanticTokens 依赖）
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/semantics/CMakeLists.txt")
    add_subdirectory(semantics)
endif()

# 阶段 5-α：ADC 引脚约束 + SDC 时序约束（纯文本解析，不依赖 Verible / 真实 TD）
#   ⚠ 必须排在 server 之前：server 的 features_constraints.cpp 直接调 constraints::adc/sdc，
#   而 if(TARGET fpga_lsp::constraints) 在 configure 时求值 —— 晚于 server 定义会恒为 FALSE。
add_subdirectory(constraints)

# 内嵌 Verible 的可选能力桥（目前只有 formatter）。
#   ⚠ 必须排在 server 之前：server 需要它来把 textDocument/formatting 接成真实现。
#   只在 FPGA_LSP_VERIBLE_ENABLED=ON 时存在；缺它时 server 会通过
#   FPGA_LSP_HAVE_VERIBLE_BRIDGE 宏回退成"返回空编辑"（见 src/server/CMakeLists.txt）。
if(FPGA_LSP_VERIBLE_ENABLED
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/verible_bridge/CMakeLists.txt")
    add_subdirectory(verible_bridge)
endif()

# 阶段 2-γ：LSP 服务主体（编排 platform/jsonrpc/lsp/project/td；formatter 走可选桥）
add_subdirectory(server)

# 阶段 5-β：MCP server（39 个工具，stdio newline-delimited JSON-RPC）
#   依赖 feature 层（project/td/reports/constraints），**不**依赖 server/hdl：
#   符号类工具在 hdl 索引就绪后经 feature 层接入（见 src/mcp/bridge.h）。
add_subdirectory(mcp)

# -----------------------------------------------------------------------------
# 后续阶段的子目录占位（建好目录后取消注释即可接入）
# -----------------------------------------------------------------------------
# add_subdirectory(common)      # 基础设施：日志 / JSON / UTF / 正则封装
# add_subdirectory(lsp)         # LSP 协议类型与消息分发
# add_subdirectory(verible_bridge)  # 对 fpga_lsp::verible 的 C++ 封装层
# add_subdirectory(hdl)         # HDL 文档模型 / 符号表
# （constraints 已启用，见上方"阶段 5-α"）
# add_subdirectory(project)     # TD 工程 .al 解析
# add_subdirectory(platform)    # （已启用）
# add_subdirectory(td)          # （已启用，见上方阶段 4-α）
# add_subdirectory(flow)        # 综合 / 布局布线流程编排
# add_subdirectory(reports)     # （已启用，见上方阶段 4-γ）
# add_subdirectory(mcp)         # MCP 服务层

# -----------------------------------------------------------------------------
# 阶段 0-E：阶段0门禁工具（与官方 verible-verilog-syntax 做 token 一致性对比）
#   src/tools/cst_dump/ 由 gate-verify 提供；target 名 cst_dump。
#   必须用 FPGA_LSP_VERIBLE_ENABLED 包住：Verible 未接入时该工具会因找不到
#   库而配置失败，从而拖垮整个 build。
# -----------------------------------------------------------------------------
if(FPGA_LSP_VERIBLE_ENABLED AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/cst_dump/CMakeLists.txt")
    add_subdirectory(tools/cst_dump)
endif()

# -----------------------------------------------------------------------------
# 阶段 1-β：lint 等价验证工具（与官方 verible-verilog-lint 做诊断一致性对比）
#   src/tools/lint_dump/ 由 gate-verify 提供；target 名 lint_dump。
#   同样必须用 FPGA_LSP_VERIBLE_ENABLED 包住。
# -----------------------------------------------------------------------------
if(FPGA_LSP_VERIBLE_ENABLED AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/lint_dump/CMakeLists.txt")
    add_subdirectory(tools/lint_dump)
endif()

# -----------------------------------------------------------------------------
# 阶段 1-δ：formatter 等价验证工具（与官方 verible-verilog-format 做逐字节对比）
#   src/tools/format_dump/ ；target 名 format_dump。
#   同样必须用 FPGA_LSP_VERIBLE_ENABLED 包住。
#   与 cst_dump / lint_dump 并列：三个工具分别覆盖 解析 / lint / 格式化 三条链路。
# -----------------------------------------------------------------------------
if(FPGA_LSP_VERIBLE_ENABLED AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/format_dump/CMakeLists.txt")
    add_subdirectory(tools/format_dump)
endif()