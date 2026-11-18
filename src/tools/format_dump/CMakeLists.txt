# =============================================================================
#  format_dump —— Verible formatter 行为导出工具（阶段 1 formatter 门禁）
# =============================================================================
#  被 src/CMakeLists.txt 以 add_subdirectory(tools/format_dump) 引入。
#
#  依赖：cmake/Verible.cmake 提供的聚合 target `verible_formatter_libs`
#        （= formatter + format-style-init + basic-format-style-init + 传递依赖）。
#
#  与 cst_dump 的设计对照：
#    cst_dump    导出 Verible **解析**行为（token / CST / 错误消息）
#    format_dump 导出 Verible **格式化**行为（逐字节输出）
#  两者都是「门禁的我方端点」，都遵循同一条方法论：**不重写输出，复用官方同一套函数**。
# =============================================================================

if(NOT TARGET verible_formatter_libs)
    message(FATAL_ERROR
        "format_dump: 未找到聚合 target `verible_formatter_libs`。\n"
        "  该 target 由 cmake/Verible.cmake 提供（在 verible_parser_libs 之后定义）；"
        "若改了名字，请同步修改本文件中的链接名。")
endif()

add_executable(format_dump
    main.cpp
)

# 本项目自有代码使用本仓库统一的告警策略，而不是 Verible 的 /W0。
if(COMMAND fpga_lsp_default_warnings)
    fpga_lsp_default_warnings(format_dump)
endif()

target_compile_features(format_dump PRIVATE cxx_std_23)

target_link_libraries(format_dump PRIVATE
    verible_formatter_libs
    # InitCommandLine（与官方 verilog-format.cc 用同一个入口）
    verible::common::util::init-command-line
    # absl flag 解析（InitCommandLine 内部依赖）
    absl::flags_parse
)

# -----------------------------------------------------------------------------
# ⚠ 必须显式 /WHOLEARCHIVE 三个解析链路归档（实测踩到，记录原因）
# -----------------------------------------------------------------------------
# 现象：只 link verible_formatter_libs（它 INTERFACE 传递了 verible_parser_libs，
#       而后者已经带了 /WHOLEARCHIVE 选项）仍然报
#         LNK2001: verilog::verilog_parse / verilog_debug / verilog_symbol_name
#         LNK2019: verilogFlexLexer::yy_pop_state / VerilogLexer::yylex
#
# 原因：INTERFACE 的 link options 在 CMake 生成的链接行里位置较前，
#       而 format_dump 又额外引入了 formatting 相关归档；这些归档在
#       parser 归档**之前**被处理，此时 /WHOLEARCHIVE 尚未生效，
#       MSVC 就按"无人引用"把 .tab.cc / flex 生成的成员丢掉了。
#       只 link verible_parser_libs 的 cst_dump 不触发这个顺序问题，
#       所以这个坑只在引入 formatter 后才出现。
#
# 处置：像 verible_parser_libs 那样，在本 target 上再声明一次
#       「link 一次（建立构建依赖，避免 LNK1356）+ WHOLEARCHIVE（强制全链）」。
#       重复声明无害（MSVC 对同一归档多次 /WHOLEARCHIVE 是幂等的）。
if(MSVC)
    foreach(_wa IN ITEMS
            verible__verilog__parser__verilog-y-cc
            verible__verilog__parser__verilog-lexer
            verible__common__analysis__command-file-lexer)
        if(TARGET ${_wa})
            target_link_libraries(format_dump PRIVATE "${_wa}")
            target_link_options(format_dump PRIVATE
                "/WHOLEARCHIVE:$<TARGET_FILE:${_wa}>")
        endif()
    endforeach()
endif()

# Verible 源码根目录需要作为 include 根（头文件以 "verible/..." 开头）。
target_include_directories(format_dump PRIVATE
    "${CMAKE_SOURCE_DIR}/thirdparty/verible"
)

# -----------------------------------------------------------------------------
# 官方基准二进制路径（门禁 ground truth）
# -----------------------------------------------------------------------------
set(FPGA_LSP_OFFICIAL_FORMAT "" CACHE FILEPATH
    "官方 verible-verilog-format 二进制路径（阶段1 formatter 门禁基准）")
if(NOT FPGA_LSP_OFFICIAL_FORMAT)
    set(_default_official_format
        "%USERPROFILE%/verible/verible-verilog-format.exe")
    if(EXISTS "${_default_official_format}")
        set(FPGA_LSP_OFFICIAL_FORMAT "${_default_official_format}")
    endif()
endif()

# -----------------------------------------------------------------------------
# 门禁 CTest 注册（委托给 tests/CMakeLists.txt）
# -----------------------------------------------------------------------------
# 与 cst_dump 相同的原因：顶层 `add_subdirectory(src)` 早于 `enable_testing()`，
# 在 src/ 下 add_test() 不会生成 CTestTestfile，收集链断在 src 这一层。
# 因此这里只导出全局属性，由 tests/CMakeLists.txt 调用 add_test。
# -----------------------------------------------------------------------------
if(BUILD_TESTING AND EXISTS "${FPGA_LSP_OFFICIAL_FORMAT}")
    set_property(GLOBAL PROPERTY FPGA_LSP_FORMAT_DUMP_EXE
        "$<TARGET_FILE:format_dump>")
    set_property(GLOBAL PROPERTY FPGA_LSP_OFFICIAL_FORMAT
        "${FPGA_LSP_OFFICIAL_FORMAT}")
    message(STATUS
        "format_dump: 已导出门禁路径属性，用例由 tests/CMakeLists.txt 注册"
        "（ctest -L formatgate 触发）")
else()
    message(STATUS
        "format_dump: 不注册门禁用例"
        "（BUILD_TESTING=${BUILD_TESTING}, "
        "official='${FPGA_LSP_OFFICIAL_FORMAT}'）")
endif()