# =============================================================================
#  cmake/scripts/verible-gen-check.cmake — 生成物回归断言（CTest 用）
# =============================================================================
#  作用：校验 flex/bison 生成链路（G4/G5/G6a/G6b）的产物是否**真的**打了补丁。
#
#  这是防止 win_bison 升级导致补丁静默失效的唯一保障：补丁失配后代码仍能编译
#  通过，但 Verible 会悄悄丢失语法错误记录（语言服务器诊断功能退化）。
#
#  调用方式（由 CTest 注册，也可手工跑）：
#      cmake -DCHECK=<项目> -DGEN_DIR=<生成物根目录>
#            [-DEXPECT_MOVED_HITS=5] [-DEXPECT_SYNTAX_ERROR_HITS=2]
#            -P cmake/scripts/verible-gen-check.cmake
#
#  CHECK 取值：
#      g4        verilog-token-enum.h      : 含 VERILOG_TOKENTYPE，不含 YYTOKENTYPE
#      g5        verilog-parse-interface.h : 不含令牌枚举段，保留 parser prototype
#      g6a       verilog-final.tab.cc      : 4 类锚点均已处理，无未处理的 "= yylval;"
#      g6b       verilog-final.tab.cc      : RecordSyntaxError 数与 ++yynerrs; 数**相等且 >0**
#      g6b-pair  verilog.tab.cc vs verilog-final.tab.cc :
#                补丁把 "++yynerrs;" 行数从 N 增到 N+K，且 K == N（每处插一组）
#      files     <gen> 下 9 个必需产物是否齐全
#      versions  记录 win_flex / win_bison 版本（不失败，仅输出）
#
#  设计原则：
#    * 每一条断言都必须能区分"真的通过了"和"文件不存在/空操作"。
#    * g6b 特别处理 0==0 假绿：两边都是 0 时**必须失败**。
# =============================================================================

cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED CHECK)
    message(FATAL_ERROR "verible-gen-check.cmake: 缺少 -DCHECK=<项目>")
endif()
if(NOT DEFINED GEN_DIR)
    message(FATAL_ERROR "verible-gen-check.cmake: 缺少 -DGEN_DIR=<生成物根目录>")
endif()

# 生成物应当按镜像源码树的子目录放置：<gen>/verible/verilog/parser/xxx.h
# （因为 verible 源码里 include 写的是 "verible/verilog/parser/xxx.h"，include 根是 <gen>）
# 为兼容不同布局，依次尝试：
#   <gen>/verible/verilog/parser/    <- 正式布局
#   <gen>/verilog/verilog/parser/    <- 早期/测试布局
#   <gen>/                           <- 平铺
set(_PDIRS
    "${GEN_DIR}/verible/verilog/parser"
    "${GEN_DIR}/verilog/verilog/parser"
    "${GEN_DIR}")
# 有些产物（command-file.yy.cc）在 common/analysis 下，不在 parser 目录
set(_ADIRS
    "${GEN_DIR}/verible/common/analysis"
    "${GEN_DIR}/verilog/common/analysis"
    "${GEN_DIR}")
set(_P "${GEN_DIR}/verible/verilog/parser")

function(_find_gen name out_var)
    foreach(_d IN LISTS _PDIRS _ADIRS)
        if(EXISTS "${_d}/${name}")
            set(${out_var} "${_d}/${name}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_var} "" PARENT_SCOPE)
endfunction()

function(_require_file path what)
    # ★ 必须用 "${path}" 而不是 path：后者比较的是字面量 "path"，
    #   在任何输入下都为假，于是这个函数会把"文件存在"也判成失败。
    #   这是 CMake 里最典型的静默出错之一（if 的字符串比较不会解引用变量）。
    if("${path}" STREQUAL "" OR NOT EXISTS "${path}")
        message(FATAL_ERROR "${what} 不存在（期望在 ${_P} 或 ${GEN_DIR} 下）。生成链路没有跑？")
    endif()
endfunction()

# 统计子串出现次数（逐次 FIND + 偏移前进，不复制大字符串，避免 O(n^2)）
function(_count_substr file needle out_var)
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
    set(${out_var} ${_cnt} PARENT_SCOPE)
endfunction()

# -----------------------------------------------------------------------------
#  g4: verilog-token-enum.h
# -----------------------------------------------------------------------------
if(CHECK STREQUAL "g4")
    _find_gen("verilog-token-enum.h" _f)
    _require_file("${_f}" "verilog-token-enum.h")

    file(READ "${_f}" _t)

    if(NOT _t MATCHES "VERILOG_TOKENTYPE")
        message(FATAL_ERROR
            "G4 失败：${_f} 不含 'VERILOG_TOKENTYPE' 枚举定义。\n"
            "  说明没有从 verilog.tab.hh 正确抽取 token 枚举段。")
    endif()
    if(_t MATCHES "YYTOKENTYPE")
        message(FATAL_ERROR
            "G4 失败：${_f} 含有 'YYTOKENTYPE'。\n"
            "  该头文件应当只包含 VERILOG_TOKENTYPE 枚举（bison 已用 %define api.prefix）。")
    endif()
    # 必须自带 guard
    if(NOT _t MATCHES "VERIBLE_VERILOG_TOKEN_ENUM_H_")
        message(FATAL_ERROR "G4 失败：${_f} 缺少自身的 include guard VERIBLE_VERILOG_TOKEN_ENUM_H_。")
    endif()
    # 必须含 enum 定义体与 enum 类型别名，否则是空壳
    if(NOT _t MATCHES "enum verilog_tokentype")
        message(FATAL_ERROR "G4 失败：${_f} 缺少 'enum verilog_tokentype' 定义体。")
    endif()
    if(NOT _t MATCHES "verilog_token_kind_t")
        message(FATAL_ERROR "G4 失败：${_f} 缺少 'typedef ... verilog_token_kind_t'。")
    endif()

    # ★★ #if / #endif 必须平衡 ★★
    # 历史事故：G4 的起点定位错了（把 include guard 也切了进来），产出
    # "#if 7 / #endif 6" 的失衡文件，下游报 C1070: #if/#endif 对不匹配。
    # 这里的断言让这类错误在生成期就被抓住，而不是等编译时报一个指错位置的消息。
    string(REGEX REPLACE "[^#]" "" _hash_only "${_t}")
    set(_n_if 0)
    set(_n_endif 0)
    set(_rest "${_t}")
    while(TRUE)
        string(FIND "${_rest}" "\n" _nl)
        if(_nl EQUAL -1)
            set(_line "${_rest}")
            set(_more FALSE)
        else()
            string(SUBSTRING "${_rest}" 0 ${_nl} _line)
            math(EXPR _nx "${_nl} + 1")
            string(SUBSTRING "${_rest}" ${_nx} -1 _rest)
            set(_more TRUE)
        endif()
        if(_line MATCHES "^[ \t]*#[ \t]*if")
            math(EXPR _n_if "${_n_if} + 1")
        elseif(_line MATCHES "^[ \t]*#[ \t]*endif")
            math(EXPR _n_endif "${_n_endif} + 1")
        endif()
        if(NOT _more)
            break()
        endif()
    endwhile()
    if(NOT _n_if EQUAL _n_endif)
        message(FATAL_ERROR
            "G4 失败：${_f} 的 #if 与 #endif 数量不匹配（#if=${_n_if}， #endif=${_n_endif}）。\n"
            "  这会让所有 include 它的文件报 'C1070: #if/#endif 对不匹配'，"
            "而且报错位置会指向**引用方**，很难定位到生成器。\n"
            "  典型原因：G4 的起点定位错位，把 verilog.tab.hh 的 include guard 或"
            " VERILOG_DEBUG 段一起切了进来（它们的 #endif 不在切片内）。\n"
            "  正确的切片是 tab.hh 第 63 行到第 543 行，总产物约 485 行；"
            "若产物明显更长（如 547 行）即是此问题。")
    endif()

    # 校验行数在合理区间（约 485 行）：明显超出说明切片范围错了
    string(REGEX REPLACE "\n" "" _no_nl "${_t}")
    string(LENGTH "${_t}" _t_len)
    string(LENGTH "${_no_nl}" _nn_len)
    math(EXPR _nlines "${_t_len} - ${_nn_len}")
    if(_nlines GREATER 600)
        message(FATAL_ERROR
            "G4 失败：产物 ${_nlines} 行，明显超出预期（约 485 行）。\n"
            "  几乎可以确定切片起点错位，把不该包含的段带进来了。")
    endif()

    message(STATUS
        "PASS g4: verilog-token-enum.h 含 VERILOG_TOKENTYPE 枚举、无 YYTOKENTYPE、"
        "自带 guard、#if/#endif 平衡(${_n_if}/${_n_endif})、${_nlines} 行")
endif()

# -----------------------------------------------------------------------------
#  g5: verilog-parse-interface.h
# -----------------------------------------------------------------------------
if(CHECK STREQUAL "g5")
    _find_gen("verilog-parse-interface.h" _f)
    _require_file("${_f}" "verilog-parse-interface.h")

    file(READ "${_f}" _t)

    # 枚举段必须已被删除
    if(_t MATCHES "VERILOG_TOKENTYPE")
        message(FATAL_ERROR
            "G5 失败：${_f} 仍含 'VERILOG_TOKENTYPE' 枚举段。\n"
            "  说明 sed 的 /#ifndef ...[,]/#endif/d 等价规则没有生效（空操作）：\n"
            "  parse-interface 退化成 verilog.tab.hh 的拷贝，违背上游拆分头文件的意图。")
    endif()
    if(_t MATCHES "enum verilog_tokentype")
        message(FATAL_ERROR
            "G5 失败：${_f} 仍含 'enum verilog_tokentype' 定义。枚举段没有被拆出去。")
    endif()

    # parser prototype 必须保留，否则这个头文件没用了
    if(NOT _t MATCHES "int verilog_parse")
        message(FATAL_ERROR
            "G5 失败：${_f} 不含 'int verilog_parse(...)' 原型。\n"
            "  说明删多了——parser 接口被误删。")
    endif()
    # 语义值类型定义必须保留
    if(NOT _t MATCHES "VERILOG_STYPE")
        message(FATAL_ERROR "G5 失败：${_f} 不含 'VERILOG_STYPE'（语义值类型定义）。")
    endif()
    # %code requires 引入的依赖必须保留
    if(NOT _t MATCHES "parser-param\\.h")
        message(FATAL_ERROR
            "G5 失败：${_f} 不含 '#include \"verible/common/parser/parser-param.h\"'，"
            "parser prototype 的参数类型会没有定义。")
    endif()
    # 文件自带的 include guard 必须保留
    if(NOT _t MATCHES "YY_VERILOG_.*_INCLUDED")
        message(FATAL_ERROR "G5 失败：${_f} 丢失了原 verilog.tab.hh 的 include guard。")
    endif()

    message(STATUS "PASS g5: verilog-parse-interface.h 已剥离枚举段，保留 prototype/STYPE/依赖/guard")
endif()

# -----------------------------------------------------------------------------
#  g6a: verilog-final.tab.cc —— std::move 补丁
# -----------------------------------------------------------------------------
if(CHECK STREQUAL "g6a")
    _find_gen("verilog-final.tab.cc" _f)
    _require_file("${_f}" "verilog-final.tab.cc")

    file(READ "${_f}" _t)

    _count_substr("${_f}" "std::move(yylval)" _n_yylval)
    _count_substr("${_f}" "std::move(yyval)" _n_yyval)
    _count_substr("${_f}" "std::move(yyval_default)" _n_def)
    _count_substr("${_f}" "// yyval = yyvsp[1-yylen];" _n_commented)

    message(STATUS
        "g6a 统计: std::move(yylval)=${_n_yylval}  std::move(yyval)=${_n_yyval}  "
        "std::move(yyval_default)=${_n_def}  已注释的 yyval 复位=${_n_commented}")

    # 每一类都必须至少出现 1 次（0 次 = 该类补丁没生效）
    foreach(_pair IN ITEMS "std::move(yylval):${_n_yylval}" "std::move(yyval):${_n_yyval}"
                           "std::move(yyval_default):${_n_def}" "// yyval = yyvsp[1-yylen];:${_n_commented}")
        string(FIND "${_pair}" ":" _cpos)
        string(SUBSTRING "${_pair}" 0 ${_cpos} _name)
        math(EXPR _cpos1 "${_cpos} + 1")
        string(SUBSTRING "${_pair}" ${_cpos1} -1 _cnt)
        if(_cnt LESS 1)
            message(FATAL_ERROR
                "G6a 失败：最终文件里 '${_name}' 出现 0 次。\n"
                "  该类 std::move 补丁没有生效。注意：这会表现为 MSVC 编译报 C2280"
                "（unique_ptr 拷贝赋值被 delete），请检查 win_bison 版本。")
        endif()
    endforeach()

    # 未处理的赋值不得残留：'= yylval;' 与 '= yyval;' 都应当已被改写
    _count_substr("${_f}" "= yylval;" _n_raw_yylval)
    _count_substr("${_f}" "= yyval;" _n_raw_yyval)
    if(_n_raw_yylval GREATER 0)
        message(FATAL_ERROR
            "G6a 失败：最终文件里仍有 ${_n_raw_yylval} 处未处理的 '= yylval;'。")
    endif()
    if(_n_raw_yyval GREATER 0)
        message(FATAL_ERROR
            "G6a 失败：最终文件里仍有 ${_n_raw_yyval} 处未处理的 '= yyval;'。")
    endif()

    # 'yyval = yyvsp[1-yylen];' 不能再出现未注释形式（注释后是 "// yyval = ..."）
    # 用 "yyval = yyvsp[1-yylen];" 计数，因为注释行里也含该子串，等价于
    #   count("yyval = yyvsp[1-yylen];") == count("// yyval = yyvsp[1-yylen];")
    _count_substr("${_f}" "yyval = yyvsp[1-yylen];" _n_addr_all)
    if(NOT _n_addr_all EQUAL _n_commented)
        message(FATAL_ERROR
            "G6a 失败：'yyval = yyvsp[1-yylen];' 出现 ${_n_addr_all} 次，"
            "但只有 ${_n_commented} 次被 '// ' 注释掉 —— 存在未注释的复位语句。")
    endif()

    message(STATUS "PASS g6a: 4 类 std::move 补丁全部生效，无未处理赋值残留")
endif()

# -----------------------------------------------------------------------------
#  g6b: verilog-final.tab.cc —— RecordSyntaxError 补丁   ★ 最关键 ★
# -----------------------------------------------------------------------------
if(CHECK STREQUAL "g6b")
    _find_gen("verilog-final.tab.cc" _f)
    _require_file("${_f}" "verilog-final.tab.cc")

    _count_substr("${_f}" "++yynerrs;" _n_nerrs)
    _count_substr("${_f}" "param->RecordSyntaxError(yylval);" _n_rec)
    _count_substr("${_f}" "Automatically patched by >>record_recovered_syntax_errors<< rule:" _n_marker)
    _count_substr("${_f}" "// end of automatic patch" _n_end)

    message(STATUS
        "g6b 统计: ++yynerrs;=${_n_nerrs}  RecordSyntaxError=${_n_rec}  "
        "patch 标记=${_n_marker}  end 标记=${_n_end}")

    # ★ 防 0==0 假绿：如果生成的代码里根本没有 ++yynerrs;，说明 bison 换写法了，
    #   此时"两边都是 0"会让相等断言通过但补丁实际失效。必须先要求 > 0。
    if(_n_nerrs LESS 1)
        message(FATAL_ERROR
            "G6b 失败：在 ${_f} 里找不到任何 '++yynerrs;'。\n"
            "  win_bison 版本可能变了（本工程验证版本：GNU Bison 3.8.2），\n"
            "  需要更新 cmake/scripts/verible-patch.cmake 的 G6b 锚点。\n"
            "  注意：不修的话两边计数都是 0，'相等' 断言会假通过。")
    endif()

    if(_n_rec LESS 1)
        message(FATAL_ERROR
            "G6b 失败：在 ${_f} 里找不到任何 'param->RecordSyntaxError(yylval);'。\n"
            "  补丁完全没有生效 —— Verible 会静默丢失所有语法错误记录，"
            "语言服务器诊断功能失效。")
    endif()

    # 核心断言：插入次数必须与 ++yynerrs; 出现次数**完全相等**
    if(NOT _n_rec EQUAL _n_nerrs)
        message(FATAL_ERROR
            "G6b 失败：RecordSyntaxError 插入次数(${_n_rec}) != ++yynerrs; 出现次数(${_n_nerrs})。\n"
            "  每一处 ++yynerrs; 之后都必须紧跟一组补丁，否则部分错误路径不会记录 token。")
    endif()

    # 标记行与 RecordSyntaxError 必须一一对应（格式完整性）
    if(NOT _n_marker EQUAL _n_rec)
        message(FATAL_ERROR
            "G6b 失败：patch 标记行(${_n_marker}) != RecordSyntaxError(${_n_rec})。\n"
            "  插入块结构不完整。")
    endif()
    if(NOT _n_end EQUAL _n_rec)
        message(FATAL_ERROR
            "G6b 失败：'// end of automatic patch'(${_n_end}) != RecordSyntaxError(${_n_rec})。\n"
            "  插入块结构不完整。")
    endif()

    # 结构断言（★ 比数量断言更强，且不受行号偏移影响）：
    #   对每一处 RecordSyntaxError：
    #     L-1 必须是 "// Automatically patched ..." 标记行
    #     L-2 必须含 "++yynerrs;"
    #     L+1 必须是 "// end of automatic patch"
    # 这三条合起来才等价于 sed a\ 的语义。只断言数量无法发现"插错位置/顺序错"，
    # 而那正是 \\2 回填 bug 产生过的坏代码形态。
    #
    # 实现：逐行扫描（.tab.cc 有 27000 行，用 string(SUBSTRING) 反复切片是 O(n^2)，
    # 所以这里用"一次性切分成行数组"的思路——但因为 list 会吞 ';'，改为
    # 用 REGEX REPLACE 把每个结构块整体替换成一个单字符标记，用长度差计数。
    file(READ "${_f}" _t)

    # 每匹配一个"完整的 4 行结构块"，就替换成一个哨兵字符（ASCII 0x01）。
    #   不能用普通字母（如 'X'）做哨兵：源文件里到处是 'X'，计数会完全失真
    #   （实测用 'X' 数出了 85 个）。
    string(ASCII 1 _SENT)
    set(_good_pat "((^|\n)[ \t]*\\+\\+yynerrs;[^\n]*\n[ \t]*// Automatically patched by >>record_recovered_syntax_errors<< rule:\n[ \t]*param->RecordSyntaxError\\(yylval\\);\n[ \t]*// end of automatic patch)")
    string(REGEX REPLACE "${_good_pat}" "\\2${_SENT}" _marked "${_t}")

    # 数哨兵数量：把非哨兵字符全部删掉，剩下的长度就是块数。
    string(REGEX REPLACE "[^${_SENT}]" "" _hits_s "${_marked}")
    string(LENGTH "${_hits_s}" _struct_ok)

    if(NOT _struct_ok EQUAL _n_rec)
        message(FATAL_ERROR
            "G6b 结构断言失败：结构完整的补丁块(${_struct_ok}) != RecordSyntaxError 出现次数(${_n_rec})。\n"
            "  期望每一处补丁都严格是这个形态（缩进可任意）：\n"
            "      ++yynerrs;\n"
            "      // Automatically patched by >>record_recovered_syntax_errors<< rule:\n"
            "      param->RecordSyntaxError(yylval);\n"
            "      // end of automatic patch\n"
            "  不匹配通常意味着：补丁插错位置、插入块顺序被打乱、"
            "或插入行被拼接到语句同一行（回填捕获组用错的典型症状）。")
    endif()

    message(STATUS
        "PASS g6b: RecordSyntaxError(${_n_rec}) == ++yynerrs;(${_n_nerrs}) 且 > 0；"
        "${_struct_ok} 组补丁块结构完整（yynerrs → 标记 → RecordSyntaxError → end）")
endif()

# -----------------------------------------------------------------------------
#  g6b-pair: 补丁前后对照（证明"插入"而不是"替换"）
# -----------------------------------------------------------------------------
if(CHECK STREQUAL "g6b-pair")
    _find_gen("verilog.tab.cc" _f_raw)
    _find_gen("verilog-final.tab.cc" _f_final)
    _require_file("${_f_raw}" "verilog.tab.cc（G2 原始产物）")
    _require_file("${_f_final}" "verilog-final.tab.cc（G6 最终产物）")

    _count_substr("${_f_raw}" "++yynerrs;" _n_raw)
    _count_substr("${_f_final}" "++yynerrs;" _n_final)
    _count_substr("${_f_final}" "param->RecordSyntaxError(yylval);" _n_rec)
    _count_substr("${_f_raw}" "param->RecordSyntaxError(yylval);" _n_rec_raw)

    message(STATUS
        "g6b-pair: 原始 ++yynerrs;=${_n_raw} / 最终 ${_n_final}；"
        "原始 RecordSyntaxError=${_n_rec_raw} / 最终 ${_n_rec}")

    if(_n_raw LESS 1)
        message(FATAL_ERROR "g6b-pair 失败：原始 verilog.tab.cc 里没有 '++yynerrs;'，bison 版本可能变了。")
    endif()
    if(_n_rec_raw GREATER 0)
        message(FATAL_ERROR
            "g6b-pair 失败：未打补丁的 verilog.tab.cc 里已经含有 RecordSyntaxError，"
            "补丁前后无法区分，说明输入被污染了。")
    endif()
    if(NOT _n_rec EQUAL _n_raw)
        message(FATAL_ERROR
            "g6b-pair 失败：最终文件插入 ${_n_rec} 组，但原始文件只有 ${_n_raw} 处 ++yynerrs;。")
    endif()

    message(STATUS "PASS g6b-pair: 原始 ${_n_raw} 处 yynerrs 均插入了补丁（最终 RecordSyntaxError=${_n_rec}）")
endif()

# -----------------------------------------------------------------------------
#  files: 9 个必需产物齐全
# -----------------------------------------------------------------------------
if(CHECK STREQUAL "files")
    # ★ 这里只列**本生成器（G1–G6）负责**的产物。
    #   verilog_nonterminals_foreach-gen.inc 属于 CST 包的独立 genrule
    #   （cmake/scripts/verible-nonterminals.cmake，由 verible-build 接入），
    #   不属于 G1–G6，所以不在此断言 —— 否则本会因别人的产物缺失而误报。
    set(_required
        "verible/verilog/parser/verilog.yy.cc"
        "verible/verilog/parser/verilog.tab.hh"
        "verible/verilog/parser/verilog.tab.cc"
        "verible/verilog/parser/verilog.output"
        "verible/verilog/parser/verilog.dot"
        "verible/verilog/parser/verilog-moved.tab.cc"
        "verible/verilog/parser/verilog-final.tab.cc"
        "verible/verilog/parser/verilog-token-enum.h"
        "verible/verilog/parser/verilog-parse-interface.h"
        "verible/common/analysis/command-file.yy.cc"
    )
    # 同样兼容早期布局（<gen>/verilog/...）
    # 注意：_missing 必须显式置空串（set(_missing) 后 STREQUAL "" 判断会因为
    # 未定义而走 NOT 分支，但更稳妥是显式赋空）。这里用显式空串。
    set(_missing "")
    foreach(_r IN LISTS _required)
        set(_found FALSE)

        if(EXISTS "${GEN_DIR}/${_r}")
            set(_found TRUE)
        endif()
        if(NOT _found AND EXISTS "${GEN_DIR}/verilog/${_r}")
            set(_found TRUE)
        endif()

        if(NOT _found)
            string(APPEND _missing "\n    ${_r}")
        endif()
    endforeach()
    if(NOT _missing STREQUAL "")
        message(FATAL_ERROR "生成物不齐，缺少：${_missing}")
    endif()
    message(STATUS "PASS files: ${GEN_DIR} 下全部必需生成物齐全")
endif()

# -----------------------------------------------------------------------------
#  versions: 打印工具版本（不失败）
# -----------------------------------------------------------------------------
if(CHECK STREQUAL "versions")
    if(DEFINED WIN_FLEX AND EXISTS "${WIN_FLEX}")
        execute_process(COMMAND "${WIN_FLEX}" --version
                        OUTPUT_VARIABLE _fv ERROR_QUIET)
        string(STRIP "${_fv}" _fv)
        message(STATUS "win_flex: ${_fv}")
    endif()
    if(DEFINED WIN_BISON AND EXISTS "${WIN_BISON}")
        execute_process(COMMAND "${WIN_BISON}" --version
                        OUTPUT_VARIABLE _bv ERROR_QUIET)
        string(STRIP "${_bv}" _bv)
        message(STATUS "win_bison: ${_bv}")
    endif()
    message(STATUS "PASS versions（仅记录）")
endif()

# -----------------------------------------------------------------------------
#  第 1 层：差分断言 —— 不依赖任何固化常量
#    "补丁把每一处锚点都处理了"成立就绿；工具升级后自动适应，不制造假阳性。
# -----------------------------------------------------------------------------
if(CHECK STREQUAL "g6a-delta")
    _find_gen("verilog.tab.cc" _f_raw)
    _find_gen("verilog-final.tab.cc" _f_fin)
    _require_file("${_f_raw}" "verilog.tab.cc（G2 原始产物）")
    _require_file("${_f_fin}" "verilog-final.tab.cc（G6 最终产物）")

    # 原始文件中每类锚点的出现次数（现场数出，不固化）
    _count_substr("${_f_raw}" "= yylval;"              _raw_yylval)
    _count_substr("${_f_raw}" "= yyval;"               _raw_yyval)
    _count_substr("${_f_raw}" "(= yyval_default)"      _raw_default)
    _count_substr("${_f_raw}" "yyval = yyvsp[1-yylen];" _raw_reset)

    # 最终文件中对应的已处理形态
    _count_substr("${_f_fin}" "std::move(yylval)"          _fin_yylval)
    _count_substr("${_f_fin}" "std::move(yyval)"           _fin_yyval)
    _count_substr("${_f_fin}" "std::move(yyval_default)"   _fin_default)
    _count_substr("${_f_fin}" "// yyval = yyvsp[1-yylen];" _fin_reset)

    message(STATUS
        "g6a-delta 原始: yylval=${_raw_yylval} yyval=${_raw_yyval} "
        "default=${_raw_default} reset=${_raw_reset}")
    message(STATUS
        "g6a-delta 最终: yylval=${_fin_yylval} yyval=${_fin_yyval} "
        "default=${_fin_default} reset=${_fin_reset}")

    if(_raw_yylval LESS 1 OR _raw_yyval LESS 1 OR _raw_default LESS 1 OR _raw_reset LESS 1)
        message(FATAL_ERROR
            "g6a-delta 失败：原始 verilog.tab.cc 里某类锚点数量为 0"
            "（yylval=${_raw_yylval} yyval=${_raw_yyval} default=${_raw_default} reset=${_raw_reset}）。\n"
            "  bison 输出结构与预期不同，G6a 补丁可能整体失效。")
    endif()

    # 逐类比较（用平行 list，避免把含 ';' 的字符串塞进 CMake list）
    set(_names  "= yylval;" "= yyval;" "(= yyval_default)" "yyval = yyvsp[1-yylen];")
    set(_names2 "std::move(yylval)" "std::move(yyval)" "std::move(yyval_default)" "// yyval = yyvsp[1-yylen];")
    set(_raws   ${_raw_yylval} ${_raw_yyval} ${_raw_default} ${_raw_reset})
    set(_fins   ${_fin_yylval} ${_fin_yyval} ${_fin_default} ${_fin_reset})

    set(_i 0)
    while(_i LESS 4)
        list(GET _names  ${_i} _na)
        list(GET _names2 ${_i} _nb)
        list(GET _raws   ${_i} _nr)
        list(GET _fins   ${_i} _nf)
        if(NOT _nr EQUAL _nf)
            message(FATAL_ERROR
                "g6a-delta 失败：原始锚点 '${_na}' 有 ${_nr} 处，"
                "最终形态 '${_nb}' 只有 ${_nf} 处。\n"
                "  补丁没有覆盖每一处锚点（差分断言，不依赖固化基线）。")
        endif()
        math(EXPR _i "${_i} + 1")
    endwhile()

    message(STATUS "PASS g6a-delta: 四类锚点的原始数与最终处理数一一相等")
endif()

# -----------------------------------------------------------------------------
#  第 2 层：计数基线 —— 抓"数量变了但没归零"
# -----------------------------------------------------------------------------
if(CHECK STREQUAL "baseline")
    if(NOT DEFINED BASELINE_FILE OR NOT EXISTS "${BASELINE_FILE}")
        message(FATAL_ERROR "baseline 检查需要 -DBASELINE_FILE=<verible-gen-baseline.cmake>")
    endif()
    include("${BASELINE_FILE}")

    _find_gen("verilog.tab.hh" _f_hh)
    _find_gen("verilog.tab.cc" _f_raw)
    _find_gen("verilog-final.tab.cc" _f_fin)
    _require_file("${_f_hh}" "verilog.tab.hh")
    _require_file("${_f_raw}" "verilog.tab.cc")
    _require_file("${_f_fin}" "verilog-final.tab.cc")

    # 实际值
    _count_substr("${_f_hh}"  "#ifndef VERILOG_TOKENTYPE"       _act_g4)
    _count_substr("${_f_hh}"  "#ifndef VERILOG_TOKENTYPE"       _act_g5)  # 同一锚点，被 G5 删除
    _count_substr("${_f_fin}" "std::move(yylval)"               _act_yylval)
    _count_substr("${_f_fin}" "std::move(yyval)"                _act_yyval)
    _count_substr("${_f_fin}" "std::move(yyval_default)"        _act_default)
    _count_substr("${_f_fin}" "// yyval = yyvsp[1-yylen];"      _act_comment)
    _count_substr("${_f_raw}" "++yynerrs;"                      _act_nerrs)

    set(_failures "")
    # ★ 参数是【变量名】，宏内部必须显式解引用 ${${actual}}。
    #   直接写 ${actual} 只会展开成变量名字面量（" _act_yylval"），
    #   于是 `if(NOT _act_yylval EQUAL 2)` 变成字符串比较，永远不等 —— 假阳性。
    #   （实测：诊断信息里出现 "实际=_act_yylval" 就是这个问题。）
    macro(_cmp_baseline label actual_var expected)
        set(_av "${${actual_var}}")
        if(NOT "${_av}" EQUAL "${expected}")
            string(APPEND _failures
                "\n    ${label}: 期望 ${expected}，实际 ${_av}")
        endif()
        message(STATUS "  基线 ${label}: 期望=${expected} 实际=${_av}")
    endmacro()

    message(STATUS "g4/g5 枚举块: 期望=${VERIBLE_BASELINE_G4_ENUM_BLOCKS}")
    _cmp_baseline("g6a.move_yylval"        _act_yylval  ${VERIBLE_BASELINE_G6A_MOVE_YYLVAL})
    _cmp_baseline("g6a.move_yyval"         _act_yyval   ${VERIBLE_BASELINE_G6A_MOVE_YYVAL})
    _cmp_baseline("g6a.move_yyval_default" _act_default ${VERIBLE_BASELINE_G6A_MOVE_DEFAULT})
    _cmp_baseline("g6a.commented"          _act_comment ${VERIBLE_BASELINE_G6A_COMMENTED})
    _cmp_baseline("g6b.yynerrs"            _act_nerrs   ${VERIBLE_BASELINE_G6B_YYNERRS})

    if(NOT _act_g4 EQUAL VERIBLE_BASELINE_G4_ENUM_BLOCKS)
        string(APPEND _failures
            "\n    g4.blocks: 期望 ${VERIBLE_BASELINE_G4_ENUM_BLOCKS}，实际 ${_act_g4}")
    endif()

    if(NOT _failures STREQUAL "")
        message(FATAL_ERROR
            "计数基线与实际产物不符：${_failures}\n\n"
            "  ★ 不要直接把基线数字改掉！先回答'为什么变了'：\n"
            "    · 是否升级了 win_bison / win_flex？（当前基线：bison ${VERIBLE_BASELINE_BISON_VERSION} /"
            " flex ${VERIBLE_BASELINE_FLEX_VERSION}）\n"
            "    · verilog.y / verilog.lex 是否有改动？\n"
            "  ★ 如果 diff 里只改了基线文件、没有任何工具版本或 .y/.lex 改动，\n"
            "    这次基线更新很可能是'为了让红的构建变绿'，应当被 review 拒绝。\n"
            "  ★ 若确认是工具升级导致的合理变化，请连同 GENERATOR-NOTES.md 一起更新。")
    endif()

    message(STATUS "PASS baseline: 全部计数与 verible-gen-baseline.cmake 一致")
endif()

# -----------------------------------------------------------------------------
#  G6b 结构完整性：补丁必须"插在 yynerrs 之后"
#    只断言数量抓不到"插到别处"，必须断言结构。
# -----------------------------------------------------------------------------
if(CHECK STREQUAL "g6b-structure")
    _find_gen("verilog-final.tab.cc" _f)
    _require_file("${_f}" "verilog-final.tab.cc")
    file(READ "${_f}" _t)

    _count_substr("${_f}" "param->RecordSyntaxError(yylval);" _n_rec)

    # 完整结构块计数（同一份逻辑在 g6b 里也有一份；这里独立再做一遍，
    # 保证"结构"这一维度可以被单独触发/单独调试）
    string(ASCII 1 _SENT)
    set(_pat "((^|\n)[ \t]*\\+\\+yynerrs;[^\n]*\n[ \t]*// Automatically patched by >>record_recovered_syntax_errors<< rule:\n[ \t]*param->RecordSyntaxError\\(yylval\\);\n[ \t]*// end of automatic patch)")
    string(REGEX REPLACE "${_pat}" "\\2${_SENT}" _marked "${_t}")
    string(REGEX REPLACE "[^${_SENT}]" "" _only "${_marked}")
    string(LENGTH "${_only}" _ok)

    if(_n_rec LESS 1)
        message(FATAL_ERROR "g6b-structure 失败：找不到 RecordSyntaxError，补丁未生效。")
    endif()
    if(NOT _ok EQUAL _n_rec)
        message(FATAL_ERROR
            "g6b-structure 失败：结构完整的补丁块 ${_ok} 个，但 RecordSyntaxError 有 ${_n_rec} 处。\n"
            "  期望每一处都是：\n"
            "      ++yynerrs;\n"
            "      // Automatically patched by >>record_recovered_syntax_errors<< rule:\n"
            "      param->RecordSyntaxError(yylval);\n"
            "      // end of automatic patch\n"
            "  数量对但结构不对，通常意味着：插入块被挪到别处、顺序被打乱、"
            "或插入行被拼接到语句同一行。")
    endif()
    message(STATUS "PASS g6b-structure: ${_ok} 组补丁块结构完整且位于 yynerrs 之后")
endif()

# -----------------------------------------------------------------------------
#  G1/G3 lexer 产物可辨识
# -----------------------------------------------------------------------------
if(CHECK STREQUAL "lexer")
    _find_gen("verilog.yy.cc" _f1)
    _find_gen("command-file.yy.cc" _f3)
    _require_file("${_f1}" "verilog.yy.cc（G1）")
    _require_file("${_f3}" "command-file.yy.cc（G3）")

    file(READ "${_f1}" _t1)
    # verilog.lex: %option yyclass="verilog::VerilogLexer" / prefix="verilog"
    if(NOT _t1 MATCHES "verilog::VerilogLexer")
        message(FATAL_ERROR
            "lexer 失败：verilog.yy.cc 里找不到 'verilog::VerilogLexer'。\n"
            "  verilog.lex 的 %option yyclass 没有生效，产物不是预期形态。")
    endif()
    if(NOT _t1 MATCHES "define yyFlexLexer verilogFlexLexer")
        message(FATAL_ERROR
            "lexer 失败：verilog.yy.cc 缺少 '#define yyFlexLexer verilogFlexLexer'"
            "（%option prefix=\"verilog\" 未生效）。")
    endif()

    file(READ "${_f3}" _t3)
    if(NOT _t3 MATCHES "veribleCommandFileFlexLexer")
        message(FATAL_ERROR
            "lexer 失败：command-file.yy.cc 缺少 'veribleCommandFileFlexLexer'。\n"
            "  command_file.lex 的 %option prefix=\"veribleCommandFile\" 未生效。")
    endif()
    if(NOT _t3 MATCHES "verible::CommandFileLexer")
        message(FATAL_ERROR
            "lexer 失败：command-file.yy.cc 缺少 'verible::CommandFileLexer'（%option yyclass）。")
    endif()

    message(STATUS "PASS lexer: G1/G3 产物的 prefix 与 yyclass 均正确")
endif()