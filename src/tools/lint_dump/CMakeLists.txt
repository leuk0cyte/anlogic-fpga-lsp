# =============================================================================
#  lint_dump —— Verible lint 行为导出工具（阶段 1 门禁）
# =============================================================================
#  被 src/CMakeLists.txt 以 add_subdirectory(tools/lint_dump) 引入。
#
#  ⚠️ 本文件最关键的一点：**必须显式链接全部 lint 规则库**
#
#  阶段 1 头号风险：每条 lint 规则靠 VERILOG_REGISTER_LINT_RULE 的 static
#  全局对象在启动时自注册。**漏链某个规则库，那条规则会静默消失** ——
#  程序正常运行、不报错，只是少一条规则。
#
#  MSVC 静态库默认「只取被引用到的成员」，而规则注册对象没有任何外部引用
#  （它们靠静态初始化生效），所以**普通链接会把这些 .obj 全部丢弃**。
#  因此需要：
#    1. lint_dump 显式链接每个 checkers 规则库；
#    2. 对每个规则库加 /WHOLEARCHIVE，强制整库链入。
#
#  这两条缺一不可。缺少 2 就是「规则静默消失」的典型成因。
# =============================================================================

if(NOT TARGET verible_parser_libs)
    message(FATAL_ERROR
        "lint_dump: 未找到聚合 target `verible_parser_libs`（由 cmake/Verible.cmake 提供）")
endif()

add_executable(lint_dump main.cpp)

if(COMMAND fpga_lsp_default_warnings)
    fpga_lsp_default_warnings(lint_dump)
endif()

target_compile_features(lint_dump PRIVATE cxx_std_23)

# -----------------------------------------------------------------------------
# 基础依赖：lint 配置系统 + linter 主体
# -----------------------------------------------------------------------------
target_link_libraries(lint_dump PRIVATE
    verible_parser_libs
    verible::verilog::analysis::verilog-linter
    verible::verilog::analysis::verilog-linter-configuration
    verible::verilog::analysis::lint-rule-registry
    verible::verilog::analysis::default-rules
    verible::verilog::analysis::descriptions
    # InitCommandLine（官方 verilog-lint.cc 也用它做命令行初始化）
    verible::common::util::init-command-line
    absl::flags
    absl::flags_parse
)

target_include_directories(lint_dump PRIVATE
    "${CMAKE_SOURCE_DIR}/thirdparty/verible"
)

# -----------------------------------------------------------------------------
# 全部 lint 规则库 + /WHOLEARCHIVE —— 通过聚合 target `verible_lint_rules_all`
# -----------------------------------------------------------------------------
# ⚠️ 这里踩过一个坑，务必保留这个写法：
#   本文件最初按命名约定自己 foreach 收集规则库并加 /WHOLEARCHIVE，
#   但漏掉了「禁用规则」那一批，导致 --rules-file 只输出 42 行（= 默认启用数），
#   缺 19 条（全是默认禁用的规则）。
#   原因：`GetLintRuleFile()` 里的 `GetAllRuleDescriptions()` 完全依赖
#   VERILOG_REGISTER_LINT_RULE 的静态自注册表；没被链接的规则，连
#   **"被禁用"这件事本身**都不可见，于是既不出现也不报错。
#
#   → 正确做法：链接 cmake/Verible.cmake 提供的聚合 target
#     `verible_lint_rules_all`，它内部封装了「逐个 link + /WHOLEARCHIVE」。
#     下游只写一行，不会再漏。
if(TARGET verible_lint_rules_all)
    target_link_libraries(lint_dump PRIVATE verible_lint_rules_all)
    message(STATUS "lint_dump: 链接 verible_lint_rules_all（全部规则库 + /WHOLEARCHIVE）")
else()
    message(WARNING
        "lint_dump: 未找到 verible_lint_rules_all 聚合 target。\n"
        "  后果：部分 lint 规则会静默消失（静态注册对象被链接器丢弃），\n"
        "        表现为 --rules-file 输出行数 < 61。\n"
        "  解决：确认 cmake/Verible.cmake 已定义该 target，且 checkers 包已纳入构建。")
endif()

# -----------------------------------------------------------------------------
# 门禁路径导出（用例由 tests/CMakeLists.txt 注册）
# -----------------------------------------------------------------------------
set(FPGA_LSP_OFFICIAL_LINT "" CACHE FILEPATH
    "官方 verible-verilog-lint 二进制路径（阶段1门禁基准）")
if(NOT FPGA_LSP_OFFICIAL_LINT)
    set(_default_official_lint
        "C:/tools/verible/verible-verilog-lint.exe")
    if(EXISTS "${_default_official_lint}")
        set(FPGA_LSP_OFFICIAL_LINT "${_default_official_lint}")
    endif()
endif()

if(BUILD_TESTING AND EXISTS "${FPGA_LSP_OFFICIAL_LINT}")
    set_property(GLOBAL PROPERTY FPGA_LSP_LINT_DUMP_EXE "$<TARGET_FILE:lint_dump>")
    set_property(GLOBAL PROPERTY FPGA_LSP_OFFICIAL_LINT "${FPGA_LSP_OFFICIAL_LINT}")
    message(STATUS
        "lint_dump: 已导出门禁路径属性，用例由 tests/CMakeLists.txt 注册"
        "（ctest -L lintgate 触发）")
endif()