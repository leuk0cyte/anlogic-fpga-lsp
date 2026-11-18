# =============================================================================
#  unit/g5.cmake — G5 (verilog-parse-interface) 正/负向单测
# =============================================================================
#  G5 的语义：删掉 "#ifndef <YY|VERILOG>_TOKENTYPE" 到同层 "#endif" 的整段，
#  其余保留（含文件自带的 include guard）。
#
#  三类变异（见 GENERATOR-NOTES）：
#    缺失 —— 锚点不存在 -> 必须 FATAL_ERROR（空操作是最危险的失效模式）
#    多余 —— 不该删的内容被删除（如 parser prototype）
#    错位 —— 删错区间（include guard 被误删 / 枚举段没删干净）
# =============================================================================

if(NOT DEFINED PATCH_SCRIPT OR NOT DEFINED WORK_DIR)
    message(FATAL_ERROR "需要 -DPATCH_SCRIPT=<> -DWORK_DIR=<>")
endif()
file(MAKE_DIRECTORY "${WORK_DIR}")

function(_run_g5 in_file out_file)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -DMODE=g5
                "-DIN=${in_file}" "-DOUT=${out_file}"
                -P "${PATCH_SCRIPT}"
        RESULT_VARIABLE _rc
        OUTPUT_QUIET ERROR_QUIET)
    set(_rc_val "${_rc}" PARENT_SCOPE)
endfunction()

function(_cnt file needle out)
    file(READ "${file}" _txt)
    string(LENGTH "${needle}" _nlen)
    set(_c 0)
    set(_i 0)
    string(LENGTH "${_txt}" _tlen)
    while(_i LESS _tlen)
        string(SUBSTRING "${_txt}" ${_i} -1 _w)
        string(FIND "${_w}" "${needle}" _r)
        if(_r EQUAL -1)
            break()
        endif()
        math(EXPR _c "${_c} + 1")
        math(EXPR _i "${_i} + ${_r} + ${_nlen}")
    endwhile()
    set(${out} ${_c} PARENT_SCOPE)
endfunction()

function(_must_eq actual expected what)
    if(NOT "${actual}" EQUAL "${expected}")
        message(FATAL_ERROR "  [FAIL] ${what}: 期望 ${expected}，实际 ${actual}")
    endif()
    message(STATUS "  [ok]   ${what} (${actual})")
endfunction()

# -----------------------------------------------------------------------------
# 例 1：正常（VERILOG_TOKENTYPE，当前 bison 的写法）
# -----------------------------------------------------------------------------
set(_h1 "${WORK_DIR}/verilog_prefix.tab.hh")
file(WRITE "${_h1}" "\
/* Bison interface */
#ifndef YY_VERILOG_X_TAB_HH_INCLUDED
# define YY_VERILOG_X_TAB_HH_INCLUDED
/* Debug traces.  */
#ifndef VERILOG_DEBUG
# define VERILOG_DEBUG 1
#endif
#include \"verible/common/parser/parser-param.h\"
#ifndef VERILOG_TOKENTYPE
# define VERILOG_TOKENTYPE
  enum verilog_tokentype { TK_a = 1 };
  typedef enum verilog_tokentype verilog_token_kind_t;
#endif
typedef int VERILOG_STYPE;
int verilog_parse (::verible::ParserParam* param);
#endif /* !YY_VERILOG_X_TAB_HH_INCLUDED  */
")
_run_g5("${_h1}" "${WORK_DIR}/out1.h")
_must_eq("${_rc_val}" 0 "正常输入 -> 成功")
_cnt("${WORK_DIR}/out1.h" "VERILOG_TOKENTYPE" _c1)
_must_eq(${_c1} 0 "枚举段已删除（VERILOG_TOKENTYPE 应为 0）")
_cnt("${WORK_DIR}/out1.h" "enum verilog_tokentype" _c2)
_must_eq(${_c2} 0 "enum 定义已删除")
_cnt("${WORK_DIR}/out1.h" "int verilog_parse" _c3)
_must_eq(${_c3} 1 "parser prototype 保留")
_cnt("${WORK_DIR}/out1.h" "VERILOG_STYPE" _c4)
_must_eq(${_c4} 1 "STYPE 保留")
_cnt("${WORK_DIR}/out1.h" "parser-param.h" _c5)
_must_eq(${_c5} 1 "parser-param.h 依赖保留")
_cnt("${WORK_DIR}/out1.h" "YY_VERILOG_X_TAB_HH_INCLUDED" _c6)
# 3 处：#ifndef + # define + `#endif /* !... */` 的注释
_must_eq(${_c6} 3 "文件自带 include guard 完整保留（#ifndef/#define/#endif）")
_cnt("${WORK_DIR}/out1.h" "VERILOG_DEBUG" _c6b)
_must_eq(${_c6b} 2 "DEBUG 段保留（与枚举段无关，不应被删）")

# -----------------------------------------------------------------------------
# 例 2：旧式 YYTOKENTYPE（无 api.prefix 时的写法）—— 也必须能删
# -----------------------------------------------------------------------------
set(_h2 "${WORK_DIR}/legacy.tab.hh")
file(WRITE "${_h2}" "\
#ifndef YY_LEGACY_INCLUDED
# define YY_LEGACY_INCLUDED
#ifndef YYTOKENTYPE
# define YYTOKENTYPE
  enum yytokentype { YY_A = 258 };
#endif
typedef int YYSTYPE;
int yyparse (void* param);
#endif
")
_run_g5("${_h2}" "${WORK_DIR}/out2.h")
_must_eq("${_rc_val}" 0 "旧式 YYTOKENTYPE -> 成功")
_cnt("${WORK_DIR}/out2.h" "YYTOKENTYPE" _c7)
_must_eq(${_c7} 0 "YYTOKENTYPE 枚举段已删除")
_cnt("${WORK_DIR}/out2.h" "yyparse" _c8)
_must_eq(${_c8} 1 "yyparse prototype 保留")

# -----------------------------------------------------------------------------
# 例 3：锚点缺失 -> 必须 FATAL_ERROR（★ 空操作是最危险的失效模式）
#   历史：原 Bazel 的 sed 只认 YYTOKENTYPE，而当前 bison 生成 VERILOG_TOKENTYPE，
#   导致命中 0 次、静默空操作，parse-interface 退化成 tab.hh 原样拷贝。
# -----------------------------------------------------------------------------
set(_h3 "${WORK_DIR}/noanchor.tab.hh")
file(WRITE "${_h3}" "\
#ifndef YY_NOTHING_INCLUDED
# define YY_NOTHING_INCLUDED
int nothing_here (void);
#endif
")
_run_g5("${_h3}" "${WORK_DIR}/out3.h")
if(NOT _rc_val EQUAL 0)
    message(STATUS "  [ok]   锚点缺失 -> 按预期 FATAL_ERROR (rc=${_rc_val})")
else()
    message(FATAL_ERROR
        "  [FAIL] 锚点缺失时必须 FATAL_ERROR，实际 rc=0。\n"
        "         这正是'静默空操作'：parse-interface 会退化成 tab.hh 的拷贝。")
endif()

# -----------------------------------------------------------------------------
# 例 4：错位陷阱 —— include guard 名含 TOKENTYPE 子串，不得误删
# -----------------------------------------------------------------------------
set(_h4 "${WORK_DIR}/decoy.tab.hh")
file(WRITE "${_h4}" "\
#ifndef YY_VERILOG_TOKENTYPE_DECOY_INCLUDED
# define YY_VERILOG_TOKENTYPE_DECOY_INCLUDED
#ifdef KEEP_ME
# define KEPT 1
#endif
#ifndef VERILOG_TOKENTYPE
# define VERILOG_TOKENTYPE
  enum verilog_tokentype { TK_z = 9 };
#endif
int verilog_parse (void* p);
#endif
")
_run_g5("${_h4}" "${WORK_DIR}/out4.h")
_must_eq("${_rc_val}" 0 "decoy 输入 -> 成功")
_cnt("${WORK_DIR}/out4.h" "verilog_tokentype" _c9)
_must_eq(${_c9} 0 "只删枚举段")
_cnt("${WORK_DIR}/out4.h" "KEEP_ME" _c10)
_must_eq(${_c10} 1 "无关的 #ifdef 段保留（没被误删）")
_cnt("${WORK_DIR}/out4.h" "verilog_parse" _c11)
_must_eq(${_c11} 1 "parser prototype 保留")

message(STATUS "g5 单测：全部通过")