# =============================================================================
#  cst_dump —— Verible 解析行为导出工具（阶段 0 门禁）
# =============================================================================
#  被 src/CMakeLists.txt 以 add_subdirectory(tools/cst_dump) 引入。
#
#  依赖：cmake/Verible.cmake 提供的聚合 target `verible_parser_libs`
#        （= lexer + parser + CST + analysis + preprocessor + absl 传递依赖）。
#
#  说明：
#    * 本 target 只在 FPGA_LSP_VERIBLE_ENABLED=ON 时才会被 add_subdirectory，
#      见 src/CMakeLists.txt 中的条件守卫。
#    * 输出到顶层统一定义的 ${CMAKE_BINARY_DIR}/bin（CMAKE_RUNTIME_OUTPUT_DIRECTORY），
#      即 build/bin/cst_dump.exe。
# =============================================================================

if(NOT TARGET verible_parser_libs)
    message(FATAL_ERROR
        "cst_dump: 未找到聚合 target `verible_parser_libs`。\n"
        "  该 target 由 cmake/Verible.cmake 提供；若 0-B 改了名字，"
        "请同步修改 ${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt 中的链接名。")
endif()

# -----------------------------------------------------------------------------
# 官方基准二进制路径（门禁 ground truth）
# -----------------------------------------------------------------------------
set(FPGA_LSP_OFFICIAL_SYNTAX "" CACHE FILEPATH
    "官方 verible-verilog-syntax 二进制路径（阶段0门禁基准）")
if(NOT FPGA_LSP_OFFICIAL_SYNTAX)
    set(_default_official
        "C:/tools/verible/verible-verilog-syntax.exe")
    if(EXISTS "${_default_official}")
        set(FPGA_LSP_OFFICIAL_SYNTAX "${_default_official}")
    endif()
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)

add_executable(cst_dump
    main.cpp
)

# 本项目自有代码使用本仓库统一的告警策略，而不是 Verible 的 /W0。
if(COMMAND fpga_lsp_default_warnings)
    fpga_lsp_default_warnings(cst_dump)
endif()

target_compile_features(cst_dump PRIVATE cxx_std_23)

target_link_libraries(cst_dump PRIVATE
    verible_parser_libs
)

# Verible 源码根目录需要作为 include 根（头文件以 "verible/..." 开头）。
# verible_parser_libs 是 INTERFACE 聚合 target，其 include 目录由各静态库
# PUBLIC 传播；这里再显式兜底一次，避免聚合 target 未传播时编译失败。
target_include_directories(cst_dump PRIVATE
    "${CMAKE_SOURCE_DIR}/thirdparty/verible"
)

# ---- 门禁对比用例 -----------------------------------------------------------
# 需要官方基准二进制 + Python3；两者任一缺失则跳过注册（不阻塞构建）。
set(FPGA_LSP_OFFICIAL_SYNTAX "" CACHE FILEPATH
    "官方 verible-verilog-syntax 二进制路径（阶段0门禁基准）")
if(NOT FPGA_LSP_OFFICIAL_SYNTAX)
    set(_default_official
        "C:/tools/verible/verible-verilog-syntax.exe")
    if(EXISTS "${_default_official}")
        set(FPGA_LSP_OFFICIAL_SYNTAX "${_default_official}")
    endif()
endif()

# -----------------------------------------------------------------------------
# 门禁 CTest 注册（委托给 tests/CMakeLists.txt）
# -----------------------------------------------------------------------------
# 背景（踩过的坑，记录以免后人重犯）：
#   顶层 CMakeLists.txt 里 `add_subdirectory(src)` 出现在 `include(CTest)` /
#   `enable_testing()` **之前**（src 在第 100 行左右，CTest 在 166 行）。
#   后果：处理 src/ 这一支时测试还没启用，即使在本目录 add_test()，
#   CMake **不会**为 src/ 及其子目录生成 CTestTestfile.cmake，
#   ctest 完全看不到这些用例（`ctest -N` 显示 0 个）。
#   在本目录里自己 include(CTest)+enable_testing() 也只能让
#   src/tools/cst_dump/CTestTestfile.cmake 被生成，但 src/CTestTestfile.cmake
#   仍然缺失（gen 阶段已决定不为 src 生成），收集链断在 src 这一层。
#
# 解法：把门禁用例注册放在 `tests/` 目录（它在 enable_testing() 之后处理，
#   收集链完整）。本文件只负责把「cst_dump 可执行文件路径 + 官方基准路径 +
#   语料路径」作为全局属性交给 tests/CMakeLists.txt，由后者调用 add_test。
#   这样 cst_dump 保持自描述，又不依赖顶层文件顺序的修改。
# -----------------------------------------------------------------------------
if(BUILD_TESTING AND EXISTS "${FPGA_LSP_OFFICIAL_SYNTAX}")
    set_property(GLOBAL PROPERTY FPGA_LSP_CST_DUMP_EXE
        "$<TARGET_FILE:cst_dump>")
    set_property(GLOBAL PROPERTY FPGA_LSP_OFFICIAL_SYNTAX
        "${FPGA_LSP_OFFICIAL_SYNTAX}")
    message(STATUS
        "cst_dump: 已导出门禁路径属性，用例由 tests/CMakeLists.txt 注册"
        "（ctest -L gate 触发）")
else()
    message(STATUS
        "cst_dump: 不注册门禁用例"
        "（BUILD_TESTING=${BUILD_TESTING}, "
        "official='${FPGA_LSP_OFFICIAL_SYNTAX}'）")
endif()