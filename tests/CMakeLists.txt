# =============================================================================
#  tests/ —— 统一测试根目录
# =============================================================================
#  子目录：
#    unit/        阶段2-α 平台层 + JSON-RPC 的 GoogleTest 单测
#    gate-corpus/ 阶段0 Verible 门禁语料（gate-verify）
#    golden/      vendored 裁剪基准（verible-prune）
# =============================================================================

# 测试夹具的临时根：**与构建目录绑定**，不用系统 %TEMP%。
#   原因：不少夹具用固定名目录（%TEMP%\fpgalsp_b1a_test 等）。本机可以同时存在两棵
#   构建树（build/ 与 build-dev/），两边同时跑 ctest 时会在 %TEMP% 里抢同一个夹具目录，
#   表现为"FileCount 只有 1/2、file_updates==0"这种看着像产品 bug 的假失败。
#   这里只负责把目录建出来；把 TMP/TEMP 指过来的是 CMakePresets.json 的 testPresets
#   （每个 preset 指向自己构建目录下的 Testing/tmp）。
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/Testing/tmp")

# 单元测试（不依赖 Verible，始终可用）
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unit/CMakeLists.txt")
    add_subdirectory(unit)
endif()

# 性能探针（tests/perf/probe_*.cpp -> build/bin/perf_<area>.exe）
# 注意：**不注册 ctest**。性能数字受机器负载影响，不能进门禁。
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/perf/CMakeLists.txt")
    add_subdirectory(perf)
endif()

# =============================================================================
#  阶段 0 门禁：cst_dump vs 官方 verible-verilog-syntax 的 token 一致性
# =============================================================================
#  为什么注册在这里而不是 src/tools/cst_dump/：
#    顶层 CMakeLists.txt 的 `add_subdirectory(src)` 在 `enable_testing()` 之前，
#    src/ 这一支不会生成 CTestTestfile.cmake，add_test 到那里会“注册成功但
#    ctest 看不到”。tests/ 在 enable_testing() 之后处理，收集链完整。
#    src/tools/cst_dump/CMakeLists.txt 只负责导出路径属性。
#
#  触发方式：ctest -L gate
#    这两个用例默认不在 `ctest`（无 -L）里跑，因为需要官方基准二进制，
#    且全语料跑一遍较慢。
# =============================================================================
get_property(_gate_cst_dump GLOBAL PROPERTY FPGA_LSP_CST_DUMP_EXE)
get_property(_gate_official GLOBAL PROPERTY FPGA_LSP_OFFICIAL_SYNTAX)

if(BUILD_TESTING AND _gate_cst_dump AND _gate_official
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/compare_cst.py")
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(NOT Python3_Interpreter_FOUND)
        message(WARNING "阶段0门禁: 未找到 Python3 解释器，跳过门禁 CTest 注册")
    else()
        add_test(
            NAME gate_cst_dump_token_consistency
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/compare_cst.py"
                    --cst-dump "${_gate_cst_dump}"
                    --official "${_gate_official}"
                    --corpus "${CMAKE_CURRENT_SOURCE_DIR}/gate-corpus"
                    --pattern ".v,.sv"
                    --report "${CMAKE_BINARY_DIR}/gate-report.json"
        )
        set_tests_properties(gate_cst_dump_token_consistency PROPERTIES
            LABELS "gate"
            TIMEOUT 600
        )

        # 错误恢复专项：专门验证 G6b(RecordSyntaxError) 补丁没有静默失效。
        # 若 G6b 失效，token 仍一致（上面那个用例会通过），但错误消息会丢，
        # 只有这个用例能抓到。
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/gate-corpus/error-cases")
            add_test(
                NAME gate_cst_dump_error_recovery
                COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/compare_cst.py"
                        --cst-dump "${_gate_cst_dump}"
                        --official "${_gate_official}"
                        --corpus "${CMAKE_CURRENT_SOURCE_DIR}/gate-corpus/error-cases"
                        --pattern ".sv"
                        --report "${CMAKE_BINARY_DIR}/gate-report-errors.json"
            )
            set_tests_properties(gate_cst_dump_error_recovery PROPERTIES
                LABELS "gate"
                TIMEOUT 300
            )
        endif()
        message(STATUS "阶段0门禁: 已注册 gate_cst_dump_token_consistency / "
                       "gate_cst_dump_error_recovery（ctest -L gate）")
    endif()
endif()
# =============================================================================
#  阶段 5-β 门禁：MCP 39 个工具的 schema 与 Python 参考实现逐字段一致
# =============================================================================
#  触发方式：ctest -L mcp --output-on-failure
#  为什么单独一条：schema 是 AI agent 的实际契约，漂移会让 agent 传错参数，
#  而失败表现在 agent 侧（"工具不好用"），极难归因。这里把它做成硬门。
if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/compare_mcp_schema.py")
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(NOT Python3_Interpreter_FOUND)
        message(WARNING "MCP schema 门禁: 未找到 Python3 解释器，跳过注册")
    elseif(NOT TARGET fpgalsp-native)
        message(STATUS "MCP schema 门禁: 未找到 fpgalsp-native target，跳过注册")
    else()
        add_test(
            NAME mcp_schema_consistency
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/compare_mcp_schema.py"
                    --exe "$<TARGET_FILE:fpgalsp-native>"
        )
        set_tests_properties(mcp_schema_consistency PROPERTIES
            LABELS "mcp"
            TIMEOUT 180
            RUN_SERIAL TRUE
        )
        message(STATUS "MCP 门禁: 已注册 mcp_schema_consistency（ctest -L mcp）")
    endif()
endif()

# =============================================================================
#  阶段 4-α：TD 会话层集成测试（需真实 TD 进程，用 LABEL "td" 隔离）
# =============================================================================
#  触发方式：ctest -L td --output-on-failure
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/td/CMakeLists.txt")
    add_subdirectory(td)
endif()

# =============================================================================
#  阶段 2-γ：LSP 服务端到端冒烟（真进程 + 真 TD，LABEL "e2e"）
# =============================================================================
#  触发方式：ctest -L e2e --output-on-failure
#  它验证的是单元测试抓不到的东西：wire 上的能力声明、诊断发布形状、
#  exit 后无孤儿 td_commands_prompt.exe（PYTHON-BUGS P-5）。
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/e2e/CMakeLists.txt")
    add_subdirectory(e2e)
endif()

# =============================================================================
#  生成物守卫：check_generated_parser.py（LABEL "generator"）
# =============================================================================
#  触发方式：ctest -L generator --output-on-failure
#
#  防什么：本轮已经出现 4 个"静默正确"的生成器缺陷
#    （VERBATIM / G4 的 ^ 空匹配 / G6a 的 ; 列表分隔符 / y-cc 的 INTERFACE 化），
#    全部是人工偶然发现。这个用例把 bison 3.8.2 的基线固化成断言：
#    G6a 的 6 项计数、G6b 的 RecordSyntaxError 数量、G4/G5 的锚点处理。
#    失败时逐项给出「期望 N / 实际 M」，并专门防 `0 == 0` 假通过。
if(FPGA_LSP_VERIBLE_ENABLED AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/check_generated_parser.py")
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND AND DEFINED VERIBLE_GEN_VERILOG_PARSER_DIR)
        set(_genp "${VERIBLE_GEN_VERILOG_PARSER_DIR}")
        if(EXISTS "${_genp}/verilog-final.tab.cc")
            add_test(
                NAME generator_patch_integrity
                COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/check_generated_parser.py"
                        --generated      "${_genp}/verilog-final.tab.cc"
                        --raw-cc         "${_genp}/verilog.tab.cc"
                        --token-enum     "${_genp}/verilog-token-enum.h"
                        --parse-interface "${_genp}/verilog-parse-interface.h"
                        --yy-cc          "${_genp}/verilog.yy.cc"
            )
            set_tests_properties(generator_patch_integrity PROPERTIES
                LABELS "generator"
                TIMEOUT 120
            )
            message(STATUS "生成物守卫: 已注册 generator_patch_integrity（ctest -L generator）")
        else()
            message(STATUS "生成物守卫: 跳过（${_genp}/verilog-final.tab.cc 尚未生成）")
        endif()
    endif()
endif()

# =============================================================================
#  阶段 1 门禁：lint 等价验证（ctest -L lintgate）
# =============================================================================
#  与 src/tools/lint_dump/CMakeLists.txt 里 export 的全局属性配合使用
#  （和 cst_dump 同款做法：注册必须在 enable_testing() 之后处理的目录，
#    也就是本目录；src/ 那一支不会生成 CTestTestfile.cmake）。
#
#  两个用例：
#    lintgate_rules_file  —— 第 1 层：规则清单 61 行 / 42 启用，逐字节一致
#    lintgate_corpus      —— 第 3 层：102 文件诊断逐字节一致（主判据）
# =============================================================================
get_property(_lint_dump GLOBAL PROPERTY FPGA_LSP_LINT_DUMP_EXE)
get_property(_official_lint GLOBAL PROPERTY FPGA_LSP_OFFICIAL_LINT)

if(BUILD_TESTING AND _lint_dump AND _official_lint
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/compare_lint.py")
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(
            NAME lintgate_rules_file
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/compare_lint.py"
                    --lint-dump "${_lint_dump}"
                    --official "${_official_lint}"
                    --rules-file
                    --corpus "${CMAKE_CURRENT_SOURCE_DIR}/gate-corpus"
        )
        set_tests_properties(lintgate_rules_file PROPERTIES
            LABELS "lintgate" TIMEOUT 120)

        add_test(
            NAME lintgate_corpus
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/compare_lint.py"
                    --lint-dump "${_lint_dump}"
                    --official "${_official_lint}"
                    --corpus "${CMAKE_CURRENT_SOURCE_DIR}/gate-corpus"
                    --pattern ".v,.sv"
        )
        set_tests_properties(lintgate_corpus PROPERTIES
            LABELS "lintgate" TIMEOUT 600)


        # 第 2 层：单规则定向验证（隔离多规则干扰，排除"互补抵消"式错误）
        # 注意：耗时较长（约 40s），因为要对 42 条规则逐条重跑语料。
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/compare_lint_single_rule.py")
            add_test(
                NAME lintgate_single_rule
                COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/compare_lint_single_rule.py"
                        --lint-dump "${_lint_dump}"
                        --official "${_official_lint}"
                        --corpus "${CMAKE_CURRENT_SOURCE_DIR}/gate-corpus"
                        --rules-from-corpus
            )
            set_tests_properties(lintgate_single_rule PROPERTIES
                LABELS "lintgate" TIMEOUT 600)
        endif()
        message(STATUS "阶段1门禁: 已注册 lintgate_rules_file / lintgate_corpus / lintgate_single_rule")
    endif()
else()
    message(STATUS "阶段1门禁: 跳过注册（lint_dump 或官方基准不可用）")
endif()

# =============================================================================
#  阶段 1 门禁：formatter 等价验证（ctest -L formatgate）
# =============================================================================
#  与 src/tools/format_dump/CMakeLists.txt 里 export 的全局属性配合使用。
#
#  用例：
#    formatgate_corpus    —— 102 文件格式化输出逐字节一致（主判据）
#    formatgate_selftest  —— 变异测试：证明对比脚本本身灵敏（关键）
#
#  为什么 selftest 必须单独注册：
#    一个只报"全一致"的脚本是没有价值的 —— 它可能根本没在比较。
#    selftest 做三件事：
#      A 官方 vs 官方         → 证明采集与归一化是确定性的
#      B 官方 vs 我方         → 真正的门禁
#      C 只改我方 --indentation_spaces → 必须发现差异（证明脚本灵敏）
#    C 若"没发现差异"，说明脚本在宽容匹配，门禁结论不可信。
#    ⚠ 变异必须只作用于**一侧**：两边同时改会一起变、一致率仍 100%，那是假绿灯
#      （阶段 1 lint 门禁踩过这个坑，详见 LINT-GATE-REPORT.md）。
# =============================================================================
get_property(_format_dump GLOBAL PROPERTY FPGA_LSP_FORMAT_DUMP_EXE)
get_property(_official_format GLOBAL PROPERTY FPGA_LSP_OFFICIAL_FORMAT)

if(BUILD_TESTING AND _format_dump AND _official_format
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/compare_format.py")
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(
            NAME formatgate_corpus
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/compare_format.py"
                    --ours "${_format_dump}"
                    --official "${_official_format}"
                    --corpus "${CMAKE_CURRENT_SOURCE_DIR}/gate-corpus"
        )
        set_tests_properties(formatgate_corpus PROPERTIES
            LABELS "formatgate" TIMEOUT 900)

        add_test(
            NAME formatgate_selftest
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/compare_format.py"
                    --ours "${_format_dump}"
                    --official "${_official_format}"
                    --corpus "${CMAKE_CURRENT_SOURCE_DIR}/gate-corpus"
                    --selftest
        )
        set_tests_properties(formatgate_selftest PROPERTIES
            LABELS "formatgate" TIMEOUT 300)

        message(STATUS
            "阶段1门禁: 已注册 formatgate_corpus / formatgate_selftest")
    endif()
else()
    message(STATUS "阶段1门禁: 跳过 formatter 门禁注册（format_dump 或官方基准不可用）")
endif()

# -----------------------------------------------------------------------------
#  阶段 1 门禁：formatter **局部（选区）** 格式化（标签仍是 formatgate）
# -----------------------------------------------------------------------------
#  为什么单独一组：
#    官方 --lines N-M 与全文档格式化走的是「同一个 FormatVerilog + 非空
#    LineNumberSet」，但**输出仍是整个文件**、且**不做收敛性检验**；
#    编辑器做"格式化选区"用的是另一个 API（FormatVerilogRange，只输出区间）。
#    两条路径本来就可能不同，必须分别验证。
#
#  实测确认的语义（容易误解，记下来）：
#    `--lines 5-10` 对一个 27 行的文件输出 **37 行（全文）**，不是只输出第 5-10 行。
#
#  用例：
#    formatgate_range           —— 每个文件挑 3 个区间（首/中/尾）逐字节对比
#    formatgate_range_selftest  —— 变异测试：只改我方 range 上界必须被发现
# -----------------------------------------------------------------------------
if(BUILD_TESTING AND _format_dump AND _official_format
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/compare_format_range.py")
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(
            NAME formatgate_range
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/compare_format_range.py"
                    --ours "${_format_dump}"
                    --official "${_official_format}"
                    --corpus "${CMAKE_CURRENT_SOURCE_DIR}/gate-corpus"
        )
        set_tests_properties(formatgate_range PROPERTIES
            LABELS "formatgate" TIMEOUT 1800)

        add_test(
            NAME formatgate_range_selftest
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/compare_format_range.py"
                    --ours "${_format_dump}"
                    --official "${_official_format}"
                    --corpus "${CMAKE_CURRENT_SOURCE_DIR}/gate-corpus"
                    --selftest
        )
        set_tests_properties(formatgate_range_selftest PROPERTIES
            LABELS "formatgate" TIMEOUT 300)

        message(STATUS
            "阶段1门禁: 已注册 formatgate_range / formatgate_range_selftest")
    endif()
endif()
