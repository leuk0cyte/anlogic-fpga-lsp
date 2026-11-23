# =============================================================================
#  cmake/scripts/verible-generators.cmake — Verible flex/bison 代码生成编排
# =============================================================================
#  本文件负责把 G1–G6 六条生成规则编排成 CMake 构建步骤。
#
#  它**只做编排**，不实现文本变换——变换逻辑在 cmake/scripts/verible-patch.cmake。
#  分工：
#    · verible-generators.cmake  调 win_flex / win_bison，串起 G1–G6，产出挂 target
#    · verible-patch.cmake       G4/G5/G6a/G6b 的纯文本变换（cmake -P 独立可跑）
#    · verible-gen-check.cmake   生成物回归断言（CTest 用）
#    · verible-gen-baseline.cmake 计数基线（第 2 层防线）
#
# =============================================================================
#  一、规则一览（与原始 Bazel genrule 一一对应）
# =============================================================================
#   G1  verilog.lex            -> verilog.yy.cc            (win_flex)
#   G2  verilog.y              -> verilog.tab.hh / .tab.cc / .output / .dot  (win_bison)
#   G3  command_file.lex       -> command-file.yy.cc       (win_flex)
#   G4  verilog.tab.hh         -> verilog-token-enum.h     (patch g4)
#   G5  verilog.tab.hh         -> verilog-parse-interface.h(patch g5)
#   G6a verilog.tab.cc         -> verilog-moved.tab.cc     (patch g6a)
#   G6b verilog-moved.tab.cc   -> verilog-final.tab.cc     (patch g6b)
#
# =============================================================================
#  二、依赖链（★ 必须精确，否则增量构建会静默产出错误内容）
# =============================================================================
#   G1 <- verilog.lex
#   G2 <- verilog.y
#   G3 <- command_file.lex
#   G4 <- G2 的 .tab.hh
#   G5 <- G2 的 .tab.hh
#   G6a<- G2 的 .tab.cc
#   G6b<- **G6a 的产物**（不是 .tab.cc！）
#
#   ★ G6b 若误写成依赖 .tab.cc，ninja 增量构建时可能跳过 G6a，
#     于是 G6b 基于**未打补丁**的 .tab.cc 生成 —— 产物有内容、能编译，
#     但 std::move 全没了（运行期回退）且 RecordSyntaxError 可能被插到错误上下文。
#     这类"有内容但内容错"正是本工程反复踩到的静默错误。
#
# =============================================================================
#  三、输出布局（镜像源码树，因为 include 写的是 "verible/verilog/parser/xxx"）
# =============================================================================
#     ${VERIBLE_GEN_ROOT}/verible/verilog/parser/verilog.yy.cc
#     ${VERIBLE_GEN_ROOT}/verible/verilog/parser/verilog.tab.hh
#     ... 以此类推
#   其中 VERIBLE_GEN_ROOT 默认 ${CMAKE_BINARY_DIR}/verible/gen
#   使用时把 ${VERIBLE_GEN_ROOT} 加进 include 路径，即可用
#   #include "verible/verilog/parser/verilog-token-enum.h" 命中生成物。
#
# =============================================================================
#  四、可独立使用（tests/generator-e2e 就是这么用的）
# =============================================================================
#   本文件不假设自己处在主工程里。它通过 include() 被引入，只依赖：
#     · VERIBLE_SRC_DIR          vendored verible 源码根（含 verible/ 子目录）
#     · VERIBLE_GEN_ROOT         输出根（默认 ${CMAKE_BINARY_DIR}/verible/gen）
#     · WIN_FLEX / WIN_BISON     工具路径（有默认值，可被 cache 覆盖）
#   因此一个独立的 `project(...)` 也能 include 它并 build。
# =============================================================================

# -----------------------------------------------------------------------------
# 参数与默认值
# -----------------------------------------------------------------------------
if(NOT DEFINED VERIBLE_SRC_DIR)
    if(DEFINED FPGA_LSP_VERIBLE_DIR)
        set(VERIBLE_SRC_DIR "${FPGA_LSP_VERIBLE_DIR}")
    else()
        set(VERIBLE_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../thirdparty/verible")
    endif()
endif()
get_filename_component(VERIBLE_SRC_DIR "${VERIBLE_SRC_DIR}" ABSOLUTE)

if(NOT DEFINED VERIBLE_GEN_ROOT)
    set(VERIBLE_GEN_ROOT "${CMAKE_BINARY_DIR}/verible/gen")
endif()
get_filename_component(VERIBLE_GEN_ROOT "${VERIBLE_GEN_ROOT}" ABSOLUTE)

# winflexbison 安装目录：优先用显式指定的，否则从 win_flex 路径反推
if(NOT DEFINED WINFLEXBISON_DIR)
    set(WINFLEXBISON_DIR "%USERPROFILE%/scoop/apps/winflexbison/current"
        CACHE PATH "winflexbison 安装目录（含 win_flex.exe / win_bison.exe / FlexLexer.h）")
endif()

set(WIN_FLEX "${WINFLEXBISON_DIR}/win_flex.exe"
    CACHE FILEPATH "win_flex.exe 路径")
set(WIN_BISON "${WINFLEXBISON_DIR}/win_bison.exe"
    CACHE FILEPATH "win_bison.exe 路径")

# FlexLexer.h 所在目录（verilog-lexer.h 里写的是 #include <FlexLexer.h>）
if(NOT DEFINED VERIBLE_FLEX_LEXER_INCLUDE_DIR)
    set(VERIBLE_FLEX_LEXER_INCLUDE_DIR "${WINFLEXBISON_DIR}"
        CACHE PATH "FlexLexer.h 所在目录（供 #include <FlexLexer.h> 使用）")
endif()

# -----------------------------------------------------------------------------
# 存在性检查（清晰的中文错误，而不是让构建在某个 obscure 的地方失败）
# -----------------------------------------------------------------------------
foreach(_tool_var IN ITEMS WIN_FLEX WIN_BISON)
    if(NOT EXISTS "${${_tool_var}}")
        message(FATAL_ERROR
            "Verible 代码生成：找不到 ${_tool_var} = ${${_tool_var}}\n"
            "  请安装 winflexbison（scoop install winflexbison），或设置：\n"
            "    -DWINFLEXBISON_DIR=<安装目录>\n"
            "    -D${_tool_var}=<可执行文件完整路径>")
    endif()
endforeach()

if(NOT EXISTS "${VERIBLE_FLEX_LEXER_INCLUDE_DIR}/FlexLexer.h")
    message(FATAL_ERROR
        "Verible 代码生成：在 ${VERIBLE_FLEX_LEXER_INCLUDE_DIR} 下找不到 FlexLexer.h。\n"
        "  verilog.lex 里是 %option c++，生成的代码会 #include <FlexLexer.h>。\n"
        "  请设置 -DVERIBLE_FLEX_LEXER_INCLUDE_DIR=<FlexLexer.h 所在目录>")
endif()

foreach(_src IN ITEMS
        "verible/verilog/parser/verilog.lex"
        "verible/verilog/parser/verilog.y"
        "verible/common/analysis/command_file.lex")
    if(NOT EXISTS "${VERIBLE_SRC_DIR}/${_src}")
        message(FATAL_ERROR
            "Verible 代码生成：找不到源文件 ${VERIBLE_SRC_DIR}/${_src}\n"
            "  请检查 -DVERIBLE_SRC_DIR=<vendored verible 根目录>")
    endif()
endforeach()

if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/verible-patch.cmake")
    message(FATAL_ERROR
        "Verible 代码生成：找不到 ${CMAKE_CURRENT_LIST_DIR}/verible-patch.cmake")
endif()

# -----------------------------------------------------------------------------
# 工具版本软断言（第 3 层防线）
#   用 WARNING 而非 FATAL_ERROR：升级不一定错，但必须让人看见。
#   如果这里 FATAL，人们会去改断言而不是去确认产物 —— 那才是真正的风险。
# -----------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/verible-gen-baseline.cmake")
    set(_baseline_file "${CMAKE_CURRENT_LIST_DIR}/verible-gen-baseline.cmake")
    if(DEFINED VERIBLE_BASELINE_BISON_VERSION)
        # 已经被 include 过
        set(_have_baseline TRUE)
    else()
        include("${_baseline_file}")
        set(_have_baseline TRUE)
    endif()
else()
    set(_have_baseline FALSE)
endif()

if(_have_baseline)
    execute_process(COMMAND "${WIN_BISON}" --version
                    OUTPUT_VARIABLE _bison_ver_raw ERROR_QUIET
                    RESULT_VARIABLE _bison_rc)
    # win_bison --version 输出形如 "bison (GNU Bison) 3.8.2\n..."
    if(_bison_ver_raw MATCHES "([0-9]+\\.[0-9]+(\\.[0-9]+)?)")
        set(_bison_ver "${CMAKE_MATCH_1}")
    else()
        set(_bison_ver "unknown")
    endif()
    set(VERIBLE_DETECTED_BISON_VERSION "${_bison_ver}" CACHE INTERNAL "detected win_bison version")
    if(NOT _bison_ver MATCHES "^${VERIBLE_BASELINE_BISON_VERSION}$")
        message(WARNING
            "Verible 代码生成：win_bison 版本 ${_bison_ver} 与本工程验证版本 "
            "${VERIBLE_BASELINE_BISON_VERSION} 不一致。\n"
            "  补丁锚点（G4/G5/G6a/G6b）可能已经对不上。构建会继续，但请：\n"
            "    1) 跑 ctest -L generator 确认生成物断言仍全绿\n"
            "    2) 若数量变化，逐项确认后更新 cmake/scripts/verible-gen-baseline.cmake\n"
            "       （更新基线必须能说明'为什么变了'，否则视为为了变绿而改基线）")
    endif()

    execute_process(COMMAND "${WIN_FLEX}" --version
                    OUTPUT_VARIABLE _flex_ver_raw ERROR_QUIET)
    if(_flex_ver_raw MATCHES "([0-9]+\\.[0-9]+(\\.[0-9]+)?)")
        set(_flex_ver "${CMAKE_MATCH_1}")
    else()
        set(_flex_ver "unknown")
    endif()
    set(VERIBLE_DETECTED_FLEX_VERSION "${_flex_ver}" CACHE INTERNAL "detected win_flex version")
    if(NOT _flex_ver MATCHES "^${VERIBLE_BASELINE_FLEX_VERSION}$")
        message(WARNING
            "Verible 代码生成：win_flex 版本 ${_flex_ver} 与本工程验证版本 "
            "${VERIBLE_BASELINE_FLEX_VERSION} 不一致，请确认 .yy.cc 产物仍可编译。")
    endif()
endif()

# -----------------------------------------------------------------------------
# 输出路径
# -----------------------------------------------------------------------------
set(_gp  "${VERIBLE_GEN_ROOT}/verible/verilog/parser")     # 镜像 verible/verilog/parser
set(_ga  "${VERIBLE_GEN_ROOT}/verible/common/analysis")    # 镜像 verible/common/analysis

set(VG_VERILOG_YY_CC        "${_gp}/verilog.yy.cc")
set(VG_VERILOG_TAB_HH       "${_gp}/verilog.tab.hh")
set(VG_VERILOG_TAB_CC       "${_gp}/verilog.tab.cc")
set(VG_VERILOG_OUTPUT       "${_gp}/verilog.output")
set(VG_VERILOG_DOT          "${_gp}/verilog.dot")
set(VG_TOKEN_ENUM_H         "${_gp}/verilog-token-enum.h")
set(VG_PARSE_INTERFACE_H    "${_gp}/verilog-parse-interface.h")
set(VG_MOVED_TAB_CC         "${_gp}/verilog-moved.tab.cc")
set(VG_FINAL_TAB_CC         "${_gp}/verilog-final.tab.cc")
set(VG_COMMAND_FILE_YY_CC   "${_ga}/command-file.yy.cc")
# 生成物校验戳（G1/G3 各一个）：flex 输出必须带自己的 lexer 前缀，见 verify_flex 注释。
set(VG_VERILOG_YY_CHECK      "${_gp}/verilog.yy.cc.verified")
set(VG_COMMAND_FILE_YY_CHECK "${_ga}/command-file.yy.cc.verified")

# 导出给需要使用生成物的模块
set(VERIBLE_GEN_VERILOG_PARSER_DIR "${_gp}" CACHE INTERNAL "生成物: verilog/parser 目录")
set(VERIBLE_GEN_COMMON_ANALYSIS_DIR "${_ga}" CACHE INTERNAL "生成物: common/analysis 目录")

file(MAKE_DIRECTORY "${_gp}" "${_ga}")

# -----------------------------------------------------------------------------
# ★ 路径分隔符：**必须用正斜杠**（CMake 的默认形式），不要转成反斜杠。
#
#   实测踩坑记录（CMake 4.5 + winflexbison 2.6.4/3.8.2，Ninja）：
#     · 反斜杠路径 + Ninja 的 \" 转义 = 灾难：
#         win_bison --report-file=\"Y:\dir\o.output\"   -> cannot open
#       原因是 `\o.output"` 里的反斜杠紧邻闭合引号，被 win_bison 的参数解析
#       吃掉了，路径变成 `...parser\o.output"` 而找不到。
#       单独在 cmd 里手敲同样的命令能成功，所以这个坑**只在经过 Ninja+cmd 转义时**
#       出现，很难靠手工复现察觉。
#     · 正斜杠 + 正常引号，只要**输出目录已存在**，两个工具都工作正常。
#
#   因此本文件：
#     1) 一律传正斜杠路径（不要 file(TO_NATIVE_PATH)）
#     2) 用 file(MAKE_DIRECTORY) 预先建好输出目录（见上）
#        —— 早期我曾误判"正斜杠不被接受"，真正原因是目录当时还没建。
# -----------------------------------------------------------------------------

# -----------------------------------------------------------------------------
# 公共：调 cmake -P 跑一条 patch 规则
#   add_custom_command 里不能直接传分号列表，这里用 -D 逐个传。
# -----------------------------------------------------------------------------
# ★★ 引号陷阱（本任务最花时间的一个 bug，务必看清）★★
#
#   错误写法：
#       COMMAND "${WIN_FLEX}" --outfile="${VG_VERILOG_YY_CC}" ...
#       VERBATIM
#   生成的 build.ninja 里是：
#       cmd.exe /C "... && win_flex.exe --outfile=\"Y:/.../verilog.yy.cc\" ..."
#   那层 \" 是给 cmd.exe 的，但 **win_flex / win_bison 收到的 argv 里引号是
#   参数内容的一部分**：
#       argv[1] == --outfile="Y:/.../verilog.yy.cc"     ← 含引号字符！
#   于是工具尝试创建一个**文件名带引号**的文件 -> "could not create" / "cannot open"。
#
#   实测：同一命令行**去掉那层转义**在 cmd 里手工跑就成功（exit=0）。
#   所以"手工能跑、ninja 跑不了"的差异完全来自这一层引号。
#
#   修法（本项目采用）：**参数里不写引号**，让 VERBATIM 自己决定要不要加：
#       COMMAND "${WIN_FLEX}" "--outfile=${VG_VERILOG_YY_CC}" ...
#   注意：整个 "--outfile=..." 用一个 CMake 字符串包起来（这样它是一个完整参数，
#   CMake 展开后仍是单个 token），而**不要在值两侧加 \"。
#   本工程路径不含空格，所以 VERBATIM 最终不会加引号，工具拿到的就是干净路径。
#
# ★★ 目录必须预先创建 ★★
#   win_bison 一次要往同一目录写 4 个文件，任何一个写不了就整条失败。
#   除了配置期 file(MAKE_DIRECTORY)，每条命令的第一个 COMMAND 还要
#   ${CMAKE_COMMAND} -E make_directory，防止并行构建/清理时目录短暂缺失。
# -----------------------------------------------------------------------------
set(_patch_script "${CMAKE_CURRENT_LIST_DIR}/verible-patch.cmake")

# =============================================================================
#  G1: verilog.lex -> verilog.yy.cc
# =============================================================================
add_custom_command(
    OUTPUT  "${VG_VERILOG_YY_CC}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_gp}"
    COMMAND "${WIN_FLEX}" "--outfile=${VG_VERILOG_YY_CC}"
            "${VERIBLE_SRC_DIR}/verible/verilog/parser/verilog.lex"
    DEPENDS "${VERIBLE_SRC_DIR}/verible/verilog/parser/verilog.lex"
            "${WIN_FLEX}"
    COMMENT "Verible G1: win_flex verilog.lex -> verilog.yy.cc"
    VERBATIM
)

# G1-check：生成物必须带 verilog 自己的前缀（详见 verible-patch.cmake 的 verify_flex）
add_custom_command(
    OUTPUT  "${VG_VERILOG_YY_CHECK}"
    COMMAND "${CMAKE_COMMAND}" -DMODE=verify_flex
            "-DIN=${VG_VERILOG_YY_CC}" "-DEXPECT=verilogFlexLexer"
            -P "${_patch_script}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${VG_VERILOG_YY_CHECK}"
    DEPENDS "${VG_VERILOG_YY_CC}" "${_patch_script}"
    COMMENT "Verible G1-check: verilog.yy.cc 前缀校验"
    VERBATIM
)

# =============================================================================
#  G2: verilog.y -> .tab.hh / .tab.cc / .output / .dot
# =============================================================================
add_custom_command(
    OUTPUT  "${VG_VERILOG_TAB_HH}" "${VG_VERILOG_TAB_CC}"
            "${VG_VERILOG_OUTPUT}" "${VG_VERILOG_DOT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_gp}"
    COMMAND "${WIN_BISON}"
            "--defines=${VG_VERILOG_TAB_HH}"
            "--output-file=${VG_VERILOG_TAB_CC}"
            "--report-file=${VG_VERILOG_OUTPUT}"
            "--graph=${VG_VERILOG_DOT}"
            "${VERIBLE_SRC_DIR}/verible/verilog/parser/verilog.y"
    DEPENDS "${VERIBLE_SRC_DIR}/verible/verilog/parser/verilog.y"
            "${WIN_BISON}"
    COMMENT "Verible G2: win_bison verilog.y -> verilog.tab.hh/.cc/.output/.dot"
    VERBATIM
)

# =============================================================================
#  G3: command_file.lex -> command-file.yy.cc
# =============================================================================
add_custom_command(
    OUTPUT  "${VG_COMMAND_FILE_YY_CC}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_ga}"
    COMMAND "${WIN_FLEX}" "--outfile=${VG_COMMAND_FILE_YY_CC}"
            "${VERIBLE_SRC_DIR}/verible/common/analysis/command_file.lex"
    DEPENDS "${VERIBLE_SRC_DIR}/verible/common/analysis/command_file.lex"
            "${WIN_FLEX}"
    COMMENT "Verible G3: win_flex command_file.lex -> command-file.yy.cc"
    VERBATIM
)

# G3-check：同上（两个 flex 输出互相覆盖时，这里会先报出可读的错误）
add_custom_command(
    OUTPUT  "${VG_COMMAND_FILE_YY_CHECK}"
    COMMAND "${CMAKE_COMMAND}" -DMODE=verify_flex
            "-DIN=${VG_COMMAND_FILE_YY_CC}" "-DEXPECT=veribleCommandFileFlexLexer"
            -P "${_patch_script}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${VG_COMMAND_FILE_YY_CHECK}"
    DEPENDS "${VG_COMMAND_FILE_YY_CC}" "${_patch_script}"
    COMMENT "Verible G3-check: command-file.yy.cc 前缀校验"
    VERBATIM
)

# =============================================================================
#  G4: verilog.tab.hh -> verilog-token-enum.h
# =============================================================================
add_custom_command(
    OUTPUT  "${VG_TOKEN_ENUM_H}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_gp}"
    COMMAND "${CMAKE_COMMAND}"
            -DMODE=g4
            "-DIN=${VG_VERILOG_TAB_HH}"
            "-DOUT=${VG_TOKEN_ENUM_H}"
            -P "${_patch_script}"
    DEPENDS "${VG_VERILOG_TAB_HH}"            # ★ 依赖 G2 的头文件
            "${_patch_script}"
    COMMENT "Verible G4: 抽取 token 枚举 -> verilog-token-enum.h"
    VERBATIM
)

# =============================================================================
#  G5: verilog.tab.hh -> verilog-parse-interface.h
# =============================================================================
add_custom_command(
    OUTPUT  "${VG_PARSE_INTERFACE_H}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_gp}"
    COMMAND "${CMAKE_COMMAND}"
            -DMODE=g5
            "-DIN=${VG_VERILOG_TAB_HH}"
            "-DOUT=${VG_PARSE_INTERFACE_H}"
            -P "${_patch_script}"
    DEPENDS "${VG_VERILOG_TAB_HH}"            # ★ 依赖 G2 的头文件
            "${_patch_script}"
    COMMENT "Verible G5: 剥离枚举段 -> verilog-parse-interface.h"
    VERBATIM
)

# =============================================================================
#  G6a: verilog.tab.cc -> verilog-moved.tab.cc
# =============================================================================
add_custom_command(
    OUTPUT  "${VG_MOVED_TAB_CC}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_gp}"
    COMMAND "${CMAKE_COMMAND}"
            -DMODE=g6a
            "-DIN=${VG_VERILOG_TAB_CC}"
            "-DOUT=${VG_MOVED_TAB_CC}"
            -P "${_patch_script}"
    DEPENDS "${VG_VERILOG_TAB_CC}"            # ★ 依赖 G2 的源文件
            "${_patch_script}"
    COMMENT "Verible G6a: std::move 补丁 -> verilog-moved.tab.cc"
    VERBATIM
)

# =============================================================================
#  G6b: verilog-moved.tab.cc -> verilog-final.tab.cc
# =============================================================================
add_custom_command(
    OUTPUT  "${VG_FINAL_TAB_CC}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_gp}"
    COMMAND "${CMAKE_COMMAND}"
            -DMODE=g6b
            "-DIN=${VG_MOVED_TAB_CC}"
            "-DOUT=${VG_FINAL_TAB_CC}"
            -P "${_patch_script}"
    DEPENDS "${VG_MOVED_TAB_CC}"              # ★★ 必须是 G6a 的输出，不是 .tab.cc
            "${_patch_script}"
    COMMENT "Verible G6b: RecordSyntaxError 补丁 -> verilog-final.tab.cc"
    VERBATIM
)

# =============================================================================
#  配置期预生成 —— 修复「全新构建目录首次 configure 挂不上生成物」
# =============================================================================
# 背景（实测，见 doc/audit/2026-11-19/static/LTCG.md §3）：
#   cmake/Verible.cmake 的 verible_mount_generated_sources() 只在 configure 期
#   用 EXISTS 判断生成物是否就绪，就绪才 target_sources() 挂到库上。而 G1–G6b
#   这些规则是**构建期** add_custom_command，首次 configure 时产物还不存在
#   → 挂接被跳过 → verilog-lexer / verilog-y-cc 两个库只含空占位源
#   （实测 verilog-y-cc.lib 只有 1 KB，正常约 2 MB）→ 链接期缺 35 个符号：
#     verilogFlexLexer::* / veribleCommandFileFlexLexer::*
#     verilog_parse / verilog_debug / verilog_symbol_name
#   这个缺陷与 LTO 无关：不开 LTO 的全新目录构建同样失败。
#
# 修法：在 configure 期就把**缺失的**产物补出来（已存在则完全跳过，不增加开销），
#   这样同一次 configure 里 verible_mount_generated_sources() 就能挂上，
#   `cmake --preset ... && cmake --build ...` 一条链即可成功。
#
# 命令与下面的 G1–G6b 自定义命令**逐字一致**（同样的工具、同样的参数、同样的
#   依赖顺序）；这里是"配置期补跑"，add_custom_command 仍是"构建期增量"的权威。
#   两者产出同名文件，Ninja 看到产物已是最新就不再重跑。
# =============================================================================
option(VERIBLE_PREGENERATE_AT_CONFIGURE
       "configure 期补生成缺失的 flex/bison 产物（全新构建目录必需）" ON)

if(VERIBLE_PREGENERATE_AT_CONFIGURE)
    set(_vg_missing FALSE)
    foreach(_f IN ITEMS
            "${VG_VERILOG_YY_CC}" "${VG_VERILOG_TAB_HH}" "${VG_VERILOG_TAB_CC}"
            "${VG_TOKEN_ENUM_H}" "${VG_PARSE_INTERFACE_H}"
            "${VG_FINAL_TAB_CC}" "${VG_COMMAND_FILE_YY_CC}")
        if(NOT EXISTS "${_f}")
            set(_vg_missing TRUE)
        endif()
    endforeach()

    if(_vg_missing)
        message(STATUS "Verible: 配置期补生成缺失的 flex/bison 产物（首次 configure 必要步骤）")

        # 宏而不是函数：需要在当前作用域里累计失败标记。
        macro(_vg_pregen_run _vg_what)
            execute_process(COMMAND ${ARGN}
                            RESULT_VARIABLE _vg_rc
                            OUTPUT_VARIABLE _vg_out
                            ERROR_VARIABLE _vg_err)
            if(NOT _vg_rc EQUAL 0)
                set(_vg_pregen_failed TRUE)
                message(WARNING
                    "Verible 预生成失败：${_vg_what}（rc=${_vg_rc}）\n${_vg_err}")
            else()
                message(STATUS "  · ${_vg_what}")
            endif()
        endmacro()

        set(_vg_pregen_failed FALSE)
        file(MAKE_DIRECTORY "${_gp}" "${_ga}")

        # G1
        if(NOT EXISTS "${VG_VERILOG_YY_CC}")
            _vg_pregen_run("G1 win_flex verilog.lex -> verilog.yy.cc"
                "${WIN_FLEX}" "--outfile=${VG_VERILOG_YY_CC}"
                "${VERIBLE_SRC_DIR}/verible/verilog/parser/verilog.lex")
        endif()

        # G2（一次产出 .tab.hh/.tab.cc/.output/.dot，四个缺一不可升级）
        if(NOT EXISTS "${VG_VERILOG_TAB_HH}" OR NOT EXISTS "${VG_VERILOG_TAB_CC}")
            _vg_pregen_run("G2 win_bison verilog.y -> verilog.tab.hh/.cc"
                "${WIN_BISON}"
                "--defines=${VG_VERILOG_TAB_HH}"
                "--output-file=${VG_VERILOG_TAB_CC}"
                "--report-file=${VG_VERILOG_OUTPUT}"
                "--graph=${VG_VERILOG_DOT}"
                "${VERIBLE_SRC_DIR}/verible/verilog/parser/verilog.y")
        endif()

        # G3
        if(NOT EXISTS "${VG_COMMAND_FILE_YY_CC}")
            _vg_pregen_run("G3 win_flex command_file.lex -> command-file.yy.cc"
                "${WIN_FLEX}" "--outfile=${VG_COMMAND_FILE_YY_CC}"
                "${VERIBLE_SRC_DIR}/verible/common/analysis/command_file.lex")
        endif()

        # G4 / G5（依赖 G2 的头文件）
        if(NOT EXISTS "${VG_TOKEN_ENUM_H}")
            _vg_pregen_run("G4 token 枚举 -> verilog-token-enum.h"
                "${CMAKE_COMMAND}" -DMODE=g4
                "-DIN=${VG_VERILOG_TAB_HH}" "-DOUT=${VG_TOKEN_ENUM_H}"
                -P "${_patch_script}")
        endif()
        if(NOT EXISTS "${VG_PARSE_INTERFACE_H}")
            _vg_pregen_run("G5 剥离枚举段 -> verilog-parse-interface.h"
                "${CMAKE_COMMAND}" -DMODE=g5
                "-DIN=${VG_VERILOG_TAB_HH}" "-DOUT=${VG_PARSE_INTERFACE_H}"
                -P "${_patch_script}")
        endif()

        # G6a / G6b（G6b 必须吃 G6a 的产物，不是 .tab.cc —— 见上文 ★ 注释）
        if(NOT EXISTS "${VG_MOVED_TAB_CC}")
            _vg_pregen_run("G6a std::move 补丁 -> verilog-moved.tab.cc"
                "${CMAKE_COMMAND}" -DMODE=g6a
                "-DIN=${VG_VERILOG_TAB_CC}" "-DOUT=${VG_MOVED_TAB_CC}"
                -P "${_patch_script}")
        endif()
        if(NOT EXISTS "${VG_FINAL_TAB_CC}")
            _vg_pregen_run("G6b RecordSyntaxError 补丁 -> verilog-final.tab.cc"
                "${CMAKE_COMMAND}" -DMODE=g6b
                "-DIN=${VG_MOVED_TAB_CC}" "-DOUT=${VG_FINAL_TAB_CC}"
                -P "${_patch_script}")
        endif()

        if(_vg_pregen_failed)
            message(FATAL_ERROR
                "Verible: 配置期预生成失败。请先 `cmake --build . --target verible_generated`，"
                "再重新 configure；若仍失败，检查 winflexbison 安装与 "
                "cmake/scripts/verible-gen-baseline.cmake 的版本基线。")
        endif()
        message(STATUS "Verible: 配置期预生成完成，生成物将在本次 configure 内挂接到 target")
    endif()
endif()

# -----------------------------------------------------------------------------
# 汇总 target：让 `cmake --build . --target verible_generated` 一次产出全部
# -----------------------------------------------------------------------------
if(NOT TARGET verible_generated)
    add_custom_target(verible_generated ALL
        DEPENDS
            "${VG_VERILOG_YY_CC}"
            "${VG_VERILOG_TAB_HH}"
            "${VG_VERILOG_TAB_CC}"
            "${VG_VERILOG_OUTPUT}"
            "${VG_VERILOG_DOT}"
            "${VG_TOKEN_ENUM_H}"
            "${VG_PARSE_INTERFACE_H}"
            "${VG_MOVED_TAB_CC}"
            "${VG_FINAL_TAB_CC}"
            "${VG_COMMAND_FILE_YY_CC}"
            "${VG_VERILOG_YY_CHECK}"        # G1 前缀校验（必须先跑生成）
            "${VG_COMMAND_FILE_YY_CHECK}"   # G3 前缀校验
        COMMENT "Verible: 全部生成物（G1–G6）"
        VERBATIM
    )
    # ★ 设成 ALL：否则独立工程里 `cmake --build` 会说 "no work to do"，
    #   产物一个都不生成，而构建返回成功 —— 又是一次"静默无事发生"。
    #   主工程里也会因此保证生成物总是被构建。
endif()

# -----------------------------------------------------------------------------
# 把生成物挂到已有 target 上（如果主工程已经建了那些库）
#   · verible__verilog__parser__verilog-y-cc 需要 verilog-final.tab.cc
#   · verible__verilog__parser__verilog-lexer 需要 verilog.yy.cc
#   · common/analysis 的 command-file-lexer 需要 command-file.yy.cc
#   采用 add_dependencies（保证顺序）而不是 target_sources：
#   源文件的挂接由 cmake/Verible.cmake 通过 verible_generated_sources 属性完成，
#   本文件只负责"确保这些 target 在编译前先跑完生成"。
# -----------------------------------------------------------------------------
set(_gen_dep_targets
    verible__verilog__parser__verilog-y-cc
    verible__verilog__parser__verilog-lexer
    verible__verilog__parser__verilog-lexer
    verible__common__analysis__command-file-lexer
)

foreach(_t IN ITEMS
        verible__verilog__parser__verilog-y-cc
        verible__verilog__parser__verilog-lexer
        verible__common__analysis__command-file-lexer)
    if(TARGET ${_t})
        add_dependencies(${_t} verible_generated)
    endif()
endforeach()

# 记录在案，方便调试
set(VERIBLE_GENERATORS_CMAKE "${CMAKE_CURRENT_LIST_DIR}/verible-generators.cmake"
    CACHE INTERNAL "本次生效的生成器编排文件")

message(STATUS "Verible 代码生成：已编排 G1–G6，输出到 ${VERIBLE_GEN_ROOT}")
message(STATUS "  win_flex  = ${WIN_FLEX}")
message(STATUS "  win_bison = ${WIN_BISON}")
message(STATUS "  FlexLexer = ${VERIBLE_FLEX_LEXER_INCLUDE_DIR}/FlexLexer.h")