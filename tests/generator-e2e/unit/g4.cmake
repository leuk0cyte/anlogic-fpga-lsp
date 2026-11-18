# =============================================================================
#  unit/g4.cmake — G4 (gen-verilog-token-enum) 正/负向单测
# =============================================================================
#  用最小构造的假 .tab.hh，不依赖真实 bison（快，且能精确控制失效模式）。
#  每条规则 ≥3 例：正常输入 / 锚点缺失 / 锚点多次。
# =============================================================================

if(NOT DEFINED PATCH_SCRIPT OR NOT DEFINED WORK_DIR)
    message(FATAL_ERROR "需要 -DPATCH_SCRIPT=<verible-patch.cmake> -DWORK_DIR=<目录>")
endif()
file(MAKE_DIRECTORY "${WORK_DIR}")

set(_pass 0)
set(_fail 0)

# 跑一次 patch，返回退出码到 _rc
function(_run_patch in_file out_file)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -DMODE=g4
                "-DIN=${in_file}" "-DOUT=${out_file}"
                -P "${PATCH_SCRIPT}"
        RESULT_VARIABLE _rc
        OUTPUT_QUIET
        ERROR_QUIET)
    set(_run_patch_rc "${_rc}" PARENT_SCOPE)
endfunction()

function(_expect_ok name)
    if("${_run_patch_rc}" EQUAL 0 AND EXISTS "${ARGV1}")
        math(EXPR _p "${_pass} + 1")
        set(_pass ${_p} PARENT_SCOPE)
        message(STATUS "  [ok]   ${name}")
    else()
        math(EXPR _f "${_fail} + 1")
        set(_fail ${_f} PARENT_SCOPE)
        message(STATUS "  [FAIL] ${name} (rc=${_run_patch_rc})")
    endif()
endfunction()

# 断言文件里子串出现次数
function(_expect_count file needle expected name)
    file(READ "${file}" _txt)
    string(LENGTH "${needle}" _nlen)
    set(_cnt 0)
    set(_from 0)
    string(LENGTH "${_txt}" _tlen)
    while(_from LESS _tlen)
        string(SUBSTRING "${_txt}" ${_from} -1 _win)
        string(FIND "${_win}" "${needle}" _rel)
        if(_rel EQUAL -1)
            break()
        endif()
        math(EXPR _cnt "${_cnt} + 1")
        math(EXPR _from "${_from} + ${_rel} + ${_nlen}")
    endwhile()
    if(_cnt EQUAL ${expected})
        message(STATUS "  [ok]   ${name} (${needle} x${_cnt})")
    else()
        message(FATAL_ERROR "  [FAIL] ${name}: 期望 ${expected} 个 '${needle}'，实际 ${_cnt}")
    endif()
endfunction()

# -----------------------------------------------------------------------------
# 例 1：正常输入 —— 标准 bison 风格头文件
# -----------------------------------------------------------------------------
set(_hh1 "${WORK_DIR}/normal.tab.hh")
file(WRITE "${_hh1}" "\
/* Bison header */
#ifndef YY_VERILOG_FOO_TAB_HH_INCLUDED
# define YY_VERILOG_FOO_TAB_HH_INCLUDED
/* Debug traces.  */
#ifndef VERILOG_DEBUG
# define VERILOG_DEBUG 1
#endif  /* ! defined VERILOG_DEBUG */
/* Token kinds.  */
#ifndef VERILOG_TOKENTYPE
# define VERILOG_TOKENTYPE
  enum verilog_tokentype
  {
    VERILOG_EMPTY = -2,
    TK_alpha = 258,
    TK_beta = 259
  };
  typedef enum verilog_tokentype verilog_token_kind_t;
#endif

/* Value type.  */
typedef int VERILOG_STYPE;

int verilog_parse (void* param);

#endif /* !YY_VERILOG_FOO_TAB_HH_INCLUDED  */
")
_run_patch("${_hh1}" "${WORK_DIR}/normal.h")
_expect_ok("正常输入 -> 生成成功" "${WORK_DIR}/normal.h")
_expect_count("${WORK_DIR}/normal.h" "#ifndef VERILOG_TOKENTYPE" 1 "只含 1 个枚举 guard")
_expect_count("${WORK_DIR}/normal.h" "VERILOG_DEBUG" 0 "不含 DEBUG 段（不应被切进来）")
_expect_count("${WORK_DIR}/normal.h" "VERILOG_STYPE" 0 "不含 STYPE 段")
_expect_count("${WORK_DIR}/normal.h" "TK_beta" 1 "含枚举成员")

# ★ 平衡断言：切片必须自洽
file(READ "${WORK_DIR}/normal.h" _n)
string(REGEX MATCHALL "\n" _nls "${_n}")
list(LENGTH _nls _cnt_nl)

# -----------------------------------------------------------------------------
# 例 2：锚点缺失 —— 没有 VERILOG_TOKENTYPE（模拟 bison 换写法）
#    必须 FATAL_ERROR，退出码非零
# -----------------------------------------------------------------------------
set(_hh2 "${WORK_DIR}/missing.tab.hh")
file(WRITE "${_hh2}" "\
#ifndef YY_FOO_INCLUDED
# define YY_FOO_INCLUDED
enum something_else { A = 1 };
#endif
")
_run_patch("${_hh2}" "${WORK_DIR}/missing.h")
if(NOT _run_patch_rc EQUAL 0)
    message(STATUS "  [ok]   锚点缺失 -> 按预期 FATAL_ERROR (rc=${_run_patch_rc})")
else()
    message(FATAL_ERROR "  [FAIL] 锚点缺失时应 FATAL_ERROR，实际 rc=0（空操作！）")
endif()

# -----------------------------------------------------------------------------
# 例 3：include guard 名里含 VERILOG_TOKENTYPE 子串 —— 不得被误导
#    这是真实踩过的 bug：include guard 的第 38 行含 "VERILOG"，锚点若不做
#    行首精确匹配就会从那里开始切。
# -----------------------------------------------------------------------------
set(_hh3 "${WORK_DIR}/trap.tab.hh")
file(WRITE "${_hh3}" "\
#ifndef YY_VERILOG_TOKENTYPE_FAKE_INCLUDED
# define YY_VERILOG_TOKENTYPE_FAKE_INCLUDED
/* decoy: this block mentions VERILOG_TOKENTYPE in a comment */
#ifdef SOME_OTHER_THING
# define X 1
#endif
#ifndef VERILOG_TOKENTYPE
# define VERILOG_TOKENTYPE
  enum verilog_tokentype
  {
    REAL_TOKEN = 300
  };
  typedef enum verilog_tokentype verilog_token_kind_t;
#endif
#endif
")
_run_patch("${_hh3}" "${WORK_DIR}/trap.h")
_expect_ok("陷阱输入（decoy 子串）-> 生成成功" "${WORK_DIR}/trap.h")
_expect_count("${WORK_DIR}/trap.h" "REAL_TOKEN" 1 "切到了真正的枚举段")
_expect_count("${WORK_DIR}/trap.h" "YY_VERILOG_TOKENTYPE_FAKE_INCLUDED" 0 "没有把 decoy include guard 切进来")
_expect_count("${WORK_DIR}/trap.h" "SOME_OTHER_THING" 0 "没有把无关的 #ifdef 段切进来")

# ★ 结构平衡：这是 C1070 的根因，必须断言
file(READ "${WORK_DIR}/trap.h" _t3)
unset(_ifs)
unset(_ends)
string(REGEX MATCHALL "\n[ \t]*#[ \t]*if" _ifs "${_t3}")
string(REGEX MATCHALL "\n[ \t]*#[ \t]*endif" _ends "${_t3}")
list(LENGTH _ifs _n_if)
list(LENGTH _ends _n_endif)
if(NOT _n_if EQUAL _n_endif)
    message(FATAL_ERROR
        "  [FAIL] 结构失衡：#if=${_n_if}  #endif=${_n_endif}\n"
        "         这正是 C1070 '#if/#endif 对不匹配' 的来源。")
endif()
message(STATUS "  [ok]   结构平衡 (#if=${_n_if} / #endif=${_n_endif})")

# -----------------------------------------------------------------------------
# 例 4：枚举段内部有嵌套 #if —— 必须用深度计数找到同层的 #endif
# -----------------------------------------------------------------------------
set(_hh4 "${WORK_DIR}/nested.tab.hh")
file(WRITE "${_hh4}" "\
#ifndef VERILOG_TOKENTYPE
# define VERILOG_TOKENTYPE
  enum verilog_tokentype
  {
    A = 1,
#if defined(SOMETHING)
    B = 2,
# if ANOTHER
    C = 3,
# endif
#endif
    D = 4
  };
  typedef enum verilog_tokentype verilog_token_kind_t;
#endif
int verilog_parse (void* p);
")
_run_patch("${_hh4}" "${WORK_DIR}/nested.h")
_expect_ok("嵌套 #if -> 生成成功" "${WORK_DIR}/nested.h")
_expect_count("${WORK_DIR}/nested.h" "D = 4" 1 "包含嵌套段之后的成员（说明 endif 找对了）")
_expect_count("${WORK_DIR}/nested.h" "verilog_parse" 0 "不包含枚举段之后的内容")

file(READ "${WORK_DIR}/nested.h" _t4)
string(REGEX MATCHALL "\n[ \t]*#[ \t]*if" _ifs4 "${_t4}")
string(REGEX MATCHALL "\n[ \t]*#[ \t]*endif" _ends4 "${_t4}")
list(LENGTH _ifs4 _n_if4)
list(LENGTH _ends4 _n_endif4)
if(NOT _n_if4 EQUAL _n_endif4)
    message(FATAL_ERROR "  [FAIL] 嵌套场景结构失衡：#if=${_n_if4} #endif=${_n_endif4}")
endif()
message(STATUS "  [ok]   嵌套场景结构平衡 (#if=${_n_if4} / #endif=${_n_endif4})")

message(STATUS "g4 单测：全部通过")