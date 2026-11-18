# =============================================================================
#  unit/g6.cmake — G6a (std_move_parser_symbols) + G6b (record_recovered_syntax_errors) 单测
# =============================================================================
#  这是整条链路最关键的一对规则：
#    · G6a 失效 -> unique_ptr 拷贝赋值被 delete -> 编译报 C2280（不会静默）
#    · G6b 失效 -> 代码仍编译通过，但**静默丢失所有语法错误记录**
#                  -> 语言服务器诊断功能退化（★ 最危险）
#
#  所以 G6b 的三类变异都要覆盖：
#    缺失 —— 没有 ++yynerrs; -> 必须 FATAL_ERROR
#    多余 —— 插入次数多于锚点数
#    错位 —— 插入块被拼接到同一行 / 顺序错 / 丢了 end 标记
# =============================================================================

if(NOT DEFINED PATCH_SCRIPT OR NOT DEFINED WORK_DIR)
    message(FATAL_ERROR "需要 -DPATCH_SCRIPT=<> -DWORK_DIR=<>")
endif()
file(MAKE_DIRECTORY "${WORK_DIR}")

function(_run mode in_file out_file)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DMODE=${mode}"
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

# =============================================================================
#  G6a：四类锚点
# =============================================================================

# 例 1：四类锚点各出现 —— 逐类验证替换
set(_a1 "${WORK_DIR}/g6a_normal.cc")
file(WRITE "${_a1}" "\
static YYSTYPE yyval_default;
YYSTYPE yylval (= yyval_default);
int yynerrs = 0;
case 1:
  *++yyvsp = yylval;
  yyval = yyvsp[1-yylen];
  *++yyvsp = yyval;
break;
")
_run(g6a "${_a1}" "${WORK_DIR}/g6a_normal_out.cc")
_must_eq("${_rc_val}" 0 "G6a 正常输入 -> 成功")
_cnt("${WORK_DIR}/g6a_normal_out.cc" "std::move(yylval)" _n1)
_must_eq(${_n1} 1 "= yylval; -> std::move(yylval)")
_cnt("${WORK_DIR}/g6a_normal_out.cc" "std::move(yyval)" _n2)
_must_eq(${_n2} 1 "= yyval; -> std::move(yyval)")
_cnt("${WORK_DIR}/g6a_normal_out.cc" "std::move(yyval_default)" _n3)
_must_eq(${_n3} 1 "(= yyval_default) -> std::move(yyval_default)")
_cnt("${WORK_DIR}/g6a_normal_out.cc" "// yyval = yyvsp[1-yylen];" _n4)
_must_eq(${_n4} 1 "yyval 复位语句被注释掉")
_cnt("${WORK_DIR}/g6a_normal_out.cc" "= yylval;" _n5)
_must_eq(${_n5} 0 "无未处理的 '= yylval;' 残留")
_cnt("${WORK_DIR}/g6a_normal_out.cc" "= yyval;" _n6)
_must_eq(${_n6} 0 "无未处理的 '= yyval;' 残留")

# 例 2：同一行出现两个相同锚点 —— **必须 FATAL_ERROR**（设计变更，见下）
#   历史：早期实现逐行替换"每行第一个"，与 sed 语义一致。
#   但逐行实现有 O(n²) 性能问题（string(APPEND) 在 27001 行循环里要 22 s），
#   现改为"全局替换 + 前置校验"：
#     · 全局替换是 O(n)，1 ms 级
#     · 若发现"同一行内有重复锚点"，全局替换会与 sed（每行只改第一个）语义不同，
#       因此 **FATAL_ERROR 拒绝处理**，而不是静默产出结果不一致的文件
#   bison 3.8.2 的真实 .tab.cc 里不存在这种情况（已实测 0 行），所以正常构建不受影响。
set(_a2 "${WORK_DIR}/g6a_multi.cc")
file(WRITE "${_a2}" "  a = yylval; b = yylval;\n")
_run(g6a "${_a2}" "${WORK_DIR}/g6a_multi_out.cc")
if(NOT _rc_val EQUAL 0)
    message(STATUS
        "  [ok]   同一行重复锚点 -> 按预期 FATAL_ERROR (rc=${_rc_val})，"
        "避免与 sed 的'每行第一个'语义不一致")
else()
    message(FATAL_ERROR
        "  [FAIL] 同一行有重复锚点时必须 FATAL_ERROR（全局替换会改两处，而 sed 只改一处）。")
endif()

# 例 3：锚点缺失 -> FATAL_ERROR
set(_a3 "${WORK_DIR}/g6a_none.cc")
file(WRITE "${_a3}" "int main(void) { return 0; }\n")
_run(g6a "${_a3}" "${WORK_DIR}/g6a_none_out.cc")
if(NOT _rc_val EQUAL 0)
    message(STATUS "  [ok]   G6a 锚点缺失 -> 按预期 FATAL_ERROR (rc=${_rc_val})")
else()
    message(FATAL_ERROR "  [FAIL] G6a 锚点缺失时必须 FATAL_ERROR，实际 rc=0")
endif()

# 例 4：多行多锚点（模拟真实 bison 的 2 处 = yylval;）
set(_a4 "${WORK_DIR}/g6a_two.cc")
file(WRITE "${_a4}" "\
line1
  *++yyvsp = yylval;
line3
  *++yyvsp = yylval;
line5
")
_run(g6a "${_a4}" "${WORK_DIR}/g6a_two_out.cc")
_must_eq("${_rc_val}" 0 "G6a 两处锚点 -> 成功")
_cnt("${WORK_DIR}/g6a_two_out.cc" "std::move(yylval)" _n7)
_must_eq(${_n7} 2 "两处都替换（不遗漏）")

# =============================================================================
#  G6b：RecordSyntaxError 插入
# =============================================================================

# 例 5：两处 yynerrs，缩进不同 —— 插入必须继承各自缩进
set(_b1 "${WORK_DIR}/g6b_normal.cc")
file(WRITE "${_b1}" "\
int f(void) {
  if (!yyerrstatus)
    {
      ++yynerrs;
      yyerror(param, \"syntax error\");
    }
  ++yynerrs;
  return 0;
}
")
_run(g6b "${_b1}" "${WORK_DIR}/g6b_normal_out.cc")
_must_eq("${_rc_val}" 0 "G6b 正常输入 -> 成功")
_cnt("${WORK_DIR}/g6b_normal_out.cc" "param->RecordSyntaxError(yylval);" _m1)
_must_eq(${_m1} 2 "插入 2 组")
_cnt("${WORK_DIR}/g6b_normal_out.cc" "Automatically patched by >>record_recovered_syntax_errors<< rule:" _m2)
_must_eq(${_m2} 2 "patch 标记 2 个")
_cnt("${WORK_DIR}/g6b_normal_out.cc" "// end of automatic patch" _m3)
_must_eq(${_m3} 2 "end 标记 2 个")
_cnt("${WORK_DIR}/g6b_normal_out.cc" "++yynerrs;" _m4)
_must_eq(${_m4} 2 "原语句未被破坏（仍是 2 处）")

# 缩进继承：6 空格那处应插入 6 空格缩进
file(READ "${WORK_DIR}/g6b_normal_out.cc" _bt)
if(_bt MATCHES "\n      // Automatically patched by")
    message(STATUS "  [ok]   插入行继承 6 空格缩进")
else()
    message(FATAL_ERROR
        "  [FAIL] 插入行没有继承原行的缩进（与 sed a\\ 的语义不一致）。\n"
        "         实际内容：${_bt}")
endif()

# 结构完整性：每一组必须是 yynerrs -> marker -> RecordSyntaxError -> end
string(ASCII 1 _S)
set(_pat "((^|\n)[ \t]*\\+\\+yynerrs;[^\n]*\n[ \t]*// Automatically patched by >>record_recovered_syntax_errors<< rule:\n[ \t]*param->RecordSyntaxError\\(yylval\\);\n[ \t]*// end of automatic patch)")
string(REGEX REPLACE "${_pat}" "\\2${_S}" _marked "${_bt}")
string(REGEX REPLACE "[^${_S}]" "" _only "${_marked}")
string(LENGTH "${_only}" _nok)
_must_eq(${_nok} 2 "2 组补丁块结构完整（yynerrs -> 标记 -> Record -> end）")

# 例 6：锚点缺失 -> 必须 FATAL_ERROR（★ 防静默失效的核心）
set(_b2 "${WORK_DIR}/g6b_none.cc")
file(WRITE "${_b2}" "int main(void) { return 0; }\n")
_run(g6b "${_b2}" "${WORK_DIR}/g6b_none_out.cc")
if(NOT _rc_val EQUAL 0)
    message(STATUS "  [ok]   G6b 锚点缺失 -> 按预期 FATAL_ERROR (rc=${_rc_val})")
else()
    message(FATAL_ERROR
        "  [FAIL] G6b 锚点缺失时必须 FATAL_ERROR，实际 rc=0。\n"
        "         这正是最危险的'静默失效'：代码仍能编译，但语法错误记录全丢。")
endif()

# 例 7：锚点已打过补丁（幂等性检查）—— 不应重复插入
set(_b3 "${WORK_DIR}/g6b_twice.cc")
_run(g6b "${WORK_DIR}/g6b_normal_out.cc" "${WORK_DIR}/g6b_twice_out.cc")
_must_eq("${_rc_val}" 0 "G6b 对已打补丁的文件再跑 -> 成功")
_cnt("${WORK_DIR}/g6b_twice_out.cc" "param->RecordSyntaxError(yylval);" _m5)
_must_eq(${_m5} 2 "不会重复插入（仍是 2 处，锚点未被二次命中）")

message(STATUS "g6 单测：全部通过")