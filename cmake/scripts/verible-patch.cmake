# =============================================================================
#  cmake/scripts/verible-patch.cmake — Verible 生成物文本变换（纯 CMake，无 sed / 无 bash）
# =============================================================================
#  调用形式（以 `cmake -P` 执行，一次处理一条规则）：
#
#      cmake -DMODE=<g4|g5|g6a|g6b> -DIN=<输入> -DOUT=<输出> \
#            [-DSTRICT_ANCHORS=ON|OFF] -P cmake/scripts/verible-patch.cmake
#
#  兼容旧变量名（早期版本用的名字，保留作别名，避免破坏已有调用）：
#      -DACTION 等价 -DMODE        （取值为下列 MAKERULE_* 名）
#      -DINPUT  等价 -DIN
#      -DOUTPUT 等价 -DOUT
#
#  MODE 与原始 Bazel 规则的对应关系：
#    g4  <- verible/verilog/parser/BUILD  : gen-verilog-token-enum
#           输入 verilog.tab.hh  -> 输出 verilog-token-enum.h
#    g5  <- verible/verilog/parser/BUILD  : verilog-parse-interface
#           输入 verilog.tab.hh  -> 输出 verilog-parse-interface.h
#    g6a <- verible/common/parser/yacc.bzl: std_move_parser_symbols
#           输入 verilog.tab.cc  -> 输出 verilog-moved.tab.cc
#    g6b <- verible/common/parser/yacc.bzl: record_recovered_syntax_errors
#           输入 verilog-moved.tab.cc -> 输出 verilog-final.tab.cc
#
#  退出码：成功 0；FATAL_ERROR 时为非零（CMake 的默认行为），可直接用于
#  add_custom_command 的失败判定。
#
# =============================================================================
#  一、为什么是"逐行"语义
# =============================================================================
#  原始规则全部由 sed 实现，sed 的 s/// 是【按行独立】作用的。本脚本的替换模式
#  （G6a/G6b）都用 "[^\n]*" 限定匹配范围，保证**绝不跨行**——若不限定，`.*` 在整段
#  文本上会吞掉换行符，结果与原规则不同。
#
# =============================================================================
#  二、CMake 的坑（务必看清，改动前先读）
# =============================================================================
#  ★ 1. 绝不能用 list() 保存源代码行。
#      CMake 的 list 以 ';' 作分隔符；用 '\;' 转义在行内本身含反斜杠时会失效
#      （bison 输出里有大量 `#line ... "C:\\\\..."` 和行尾续行符 '\'）。
#      实测：27137 行被 list(APPEND) 静默压成 671 行，文件损坏但不报错。
#      本脚本因此全程只用 string(...)，不出现任何 list() 处理代码行。
#
#  ★ 2. 行尾符（EOL）。
#      file(READ) 把 CRLF 归一化为 LF（读出的文本里没有 '\r'）；
#      file(WRITE) 又把 '\n' 写成 CRLF。二者互逆，因此：
#         输入 CRLF（bison 的 .tab.cc/.tab.hh）-> 输出仍是 CRLF；
#         输入 LF  （flex 的 .yy.cc）         -> 输出变成 CRLF。
#      这不影响编译（MSVC 预处理器与 #line 完全接受 CRLF），换来的是输出与
#      工具链 EOL 行为无关的确定性。绝不要把 "\r\n" 写进内容，否则得到 0d 0d 0a。
#
#  ★ 3. 宏/函数的参数会被再解析一遍转义。
#      所以向宏传参一律传【变量名】，让宏内部用 ${${var}} 原样取值；
#      直接把含反斜杠的字面量传进宏会触发 "Invalid character escape '\U'"。
#
#  ★ 4. string(FIND) 会匹配到【行中间】出现的子串，不能用来定位"某一行"。
#      实测：想找 "#ifndef VERILOG_TOKENTYPE" 所在行，FIND 定位到了 include guard
#      那一行（行的中间恰好含该子串），于是删错了区域。
#      正确定位方式：用行首锚定的正则 "(^|\n)([ \t]*#ifndef ...)" 去找。
#
#  ★ 5. "第一个 #endif" != "匹配块对应的 #endif"。
#      bison 生成的头文件里，枚举段之前已有 include guard 与 VERILOG_DEBUG 等
#      #if/#endif 块，且枚举段内部还可能嵌套 #ifndef。直接取"向后第一个 #endif"
#      会停在 include guard 的 #endif（实测第 54 行而非第 543 行），
#      把文件头整段误删。必须做【嵌套深度计数】找同层 #endif。
#
#  ★ 6. 差一错误会让断言恒为 0：strlen("++yynerrs;") 是 **10** 不是 11。
#      写成 11 时，"用长度差算命中数"的结果永远是 0 —— 这会同时导致
#      ①"hits=0 即失败"的规则误报  ②"count>0" 断言直接拦住。
#      教训：任何"除以单位长度"的计数写法都要先用已知样本验证。
#
#  ★ 7. 替换文本里的捕获组只能含"格式化外壳"，绝不能含被匹配语句本身。
#      实测：把 ([ \t]*\+\+yynerrs;[^\n]*) 整体捕获再回填，会生成
#      "++yynerrs;param->RecordSyntaxError(yylval);" 这种把新语句拼到旧语句
#      同一行的坏代码。捕获组要只捕获"行边界 + 缩进"，语句本体重新写一遍。
#
#  ★ 8. 性能陷阱：用 "FIND + SUBSTRING 反复切片" 遍历大文本是 O(n^2)。
#      27000 行 / 1.6 MB 的 .tab.cc 实测要 40 s；改成一次 REGEX REPLACE 后 <1 s。
#      同理，"数行数"也要用 REGEX REPLACE 删换行后取长度差，而不是循环切片。
#
#  这些坑的共同点值得记住：**CMake 的文本处理原语（list/FIND/SUBSTRING/正则）
#  都会"静默出错"——返回值看起来正常，产物却已经错了。**
#  所以必须对着真实产物做断言验证，不能相信"脚本跑完没报错"。
#
# =============================================================================
#  三、失败策略：禁止"静默失效"
# =============================================================================
#  G6b（record_recovered_syntax_errors）是 Verible 错误恢复能力的关键补丁：
#  ParserParam::RecordSyntaxError() 记录的 token 决定最终诊断消息的条数
#  （recovered_syntax_errors_ -> RejectedTokens() -> FileAnalyzer 打印
#   "file:line:col syntax error at token ..."）。
#  一旦 win_bison 升级导致 `++yynerrs;` 写法变化，正则匹配 0 次 —— 在 sed 里这是
#  **静默**的：代码照样编译通过、token 序列照样一致，但所有语法错误记录悄悄消失。
#  因此本脚本对每个 MODE 都做锚点断言（STRICT_ANCHORS=ON，默认）：
#    * g6a / g6b: 命中数必须 > 0，否则 FATAL_ERROR；
#    * g4       : 必须找到 '#ifndef VERILOG_TOKENTYPE' 及其后的 '#endif'；
#  构建侧回归测试（tests/verible-generated/）会再校验最终文件里的真实出现次数，
#  形成"生成期 + 测试期"双保险。
#
# =============================================================================
#  四、产物第一行（诊断头）
# =============================================================================
#      // verible-patch: g4 hits=<命中计数> lines=<输入行数>
#  回归测试读这一行交叉验证命中数。该行是合法 C 注释，不影响编译。
#
#  ★ 这个头**只含纯数字，不含路径/时间戳/主机名**，这是刻意设计：
#    它跨机器、跨构建目录完全稳定，因此可以直接参与逐字节对比。
#    请勿往里加任何机器相关信息 —— 一旦加了，整个诊断头就没法用于对比。
#
#  ★ 与"第 2 行"的区别（对比产物时务必分清）：
#    第 1 行 `// verible-patch: ...`                   <- 本工具加的，跨机器稳定，可直接比
#    第 2 行 `// DO NOT EDIT -- generated from <路径>`   <- **原 Bazel 规则本来就有**
#        （BUILD 里写的是 `echo '// DO NOT EDIT -- generated from $<'`），含绝对路径，
#        跨目录对比时需要把 <路径> 归一化为 <INPUT>。
#    第 2 行的路径问题是官方规则固有的，不是本次移植引入的，不影响与 Bazel 的等价性。
#    G6 的产物（.tab.cc）不产生第 2 行（原规则没有 echo），只有第 1 行诊断头。
#
# =============================================================================
#  五、已验证的工具链版本（本脚本的锚点指纹基于此，换版本必须重新验证）
# =============================================================================
#   win_flex.exe   2.6.4                 (winflexbison scoop 包)
#   win_bison.exe  GNU Bison 3.8.2
#   FlexLexer.h    随 winflexbison 同目录
#   路径：%LOCALAPPDATA%\scoop\apps\winflexbison\current\
#   CMake 4.5.0 / Ninja 1.13.0 / MSVC (VS 18 Community)
#
#   在该版本下的实测锚点计数（写死在这里便于对照）：
#     g4 : hits=1     （tab.hh 558 行；枚举段第 63 行 -> 第 543 行；产物 547 行）
#     g5 : hits=1     （产物 79 行：header + STYPE + prototype + guard）
#     g6a: hits=5     （= yylval;×2、= yyval;×1、(= yyval_default)×1、
#                       yyval = yyvsp[1-yylen];×1）
#     g6b: hits=2     （两处 ++yynerrs;，缩进分别 6 空格 / 2 空格）
#
#   升级 win_bison 后请重新跑 tests 里的生成物回归用例；
#   若计数变化但未归零，"hits>0" 断言不会拦住，需要靠 tests 里的
#   数量/结构断言（verible-gen-check.cmake）与人眼确认。
# =============================================================================

cmake_minimum_required(VERSION 3.24)

# -----------------------------------------------------------------------------
# 参数归一化：支持新名 (MODE/IN/OUT) 与旧名 (ACTION/INPUT/OUTPUT)
# -----------------------------------------------------------------------------
if(DEFINED MODE AND NOT DEFINED ACTION)
    set(ACTION "${MODE}")
endif()
if(DEFINED IN AND NOT DEFINED INPUT)
    set(INPUT "${IN}")
endif()
if(DEFINED OUT AND NOT DEFINED OUTPUT)
    set(OUTPUT "${OUT}")
endif()

# -----------------------------------------------------------------------------
#  verify_flex：校验 win_flex 产出里带的是**它自己的** lexer 前缀。
#  背景（真实事故）：两个 flex 输出互相覆盖过 —— build/ 的 verilog.yy.cc 里出现
#  `#define yyFlexLexer veribleCommandFileFlexLexer`，症状是链接期一串看不懂的
#  LNK2005（veribleCommandFilealloc/free/... 在两个 lexer 库里重复定义）。
#  在生成后立刻断言前缀，能把这类"生成物坏了"的问题挡在编译之前。
#  用法：cmake -DMODE=verify_flex -DIN=<生成文件> -DEXPECT=<前缀> -P 本文件
# -----------------------------------------------------------------------------
if(ACTION STREQUAL "verify_flex")
    if(NOT DEFINED INPUT OR "${INPUT}" STREQUAL "")
        message(FATAL_ERROR "verify_flex: 缺少 -DIN=<生成文件>")
    endif()
    if(NOT DEFINED EXPECT OR "${EXPECT}" STREQUAL "")
        message(FATAL_ERROR "verify_flex: 缺少 -DEXPECT=<期望的 yyFlexLexer 前缀>")
    endif()
    if(NOT EXISTS "${INPUT}")
        message(FATAL_ERROR "verify_flex: 文件不存在：${INPUT}")
    endif()
    file(READ "${INPUT}" _verify_text)
    if(NOT _verify_text MATCHES "define[ \t]+yyFlexLexer[ \t]+${EXPECT}")
        string(REGEX MATCH "define[ \t]+yyFlexLexer[ \t]+[A-Za-z_0-9]+" _verify_got
               "${_verify_text}")
        message(FATAL_ERROR
            "verible 代码生成校验失败：${INPUT}\n"
            "  期望 '#define yyFlexLexer ${EXPECT}'，实际 '${_verify_got}'。\n"
            "  这是 flex 输出被互相覆盖的典型症状（G1/G3 并行生成时发生）。\n"
            "  修法：删除该构建目录下的 verible/gen 后重新构建：\n"
            "      rmdir /s /q <build>\\verible\\gen")
    endif()
    return()
endif()

foreach(_required IN ITEMS ACTION INPUT OUTPUT)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR
            "verible-patch.cmake: 缺少必填参数。\n"
            "  用法: cmake -DMODE=<g4|g5|g6a|g6b> -DIN=<输入> -DOUT=<输出> -P 本文件")
    endif()
endforeach()

# 新名 -> 内部 MAKERULE_* 名
if(ACTION STREQUAL "g4")
    set(ACTION "MAKERULE_GEN_TOKEN_ENUM")
elseif(ACTION STREQUAL "g5")
    set(ACTION "MAKERULE_GEN_PARSE_IFACE")
elseif(ACTION STREQUAL "g6a")
    set(ACTION "MAKERULE_MOVE_SYMBOLS")
elseif(ACTION STREQUAL "g6b")
    set(ACTION "MAKERULE_RECORD_SYNTAX_ERRORS")
endif()

# 诊断头里用的短名
if(ACTION STREQUAL "MAKERULE_GEN_TOKEN_ENUM")
    set(_mode_tag "g4")
elseif(ACTION STREQUAL "MAKERULE_GEN_PARSE_IFACE")
    set(_mode_tag "g5")
elseif(ACTION STREQUAL "MAKERULE_MOVE_SYMBOLS")
    set(_mode_tag "g6a")
elseif(ACTION STREQUAL "MAKERULE_RECORD_SYNTAX_ERRORS")
    set(_mode_tag "g6b")
else()
    message(FATAL_ERROR
        "verible-patch.cmake: 未知 MODE='${ACTION}'。"
        " 只支持 g4 / g5 / g6a / g6b（或对应旧名 MAKERULE_*）")
endif()

if(NOT DEFINED STRICT_ANCHORS)
    set(STRICT_ANCHORS ON)
endif()

if(NOT EXISTS "${INPUT}")
    message(FATAL_ERROR "verible-patch.cmake: 输入文件不存在：${INPUT}")
endif()

# -----------------------------------------------------------------------------
# 读入全文（file(READ) 已把 CRLF 归一化为 LF，文本中不含 '\r'）
# -----------------------------------------------------------------------------
file(READ "${INPUT}" _text)
string(LENGTH "${_text}" _text_len)

# 输入行数（用于诊断头与合理性检查）
# ★ 绝不用 "FIND + SUBSTRING 反复切片" 来数行：每次切片都会复制剩余全部文本，
#   27000 行 / 1.6 MB 实测要 40 s，这是本脚本最大的性能陷阱。
#   改用一次 REGEX REPLACE 把所有换行删掉，用长度差算行数，O(n)。
string(REGEX REPLACE "\n" "" _no_nl "${_text}")
string(LENGTH "${_no_nl}" _no_nl_len)
math(EXPR _nlines "${_text_len} - ${_no_nl_len}")
# 末行没有换行符时也算一行（注意：短字符串上用负下标取字符在旧版 CMake 上会报
# "begin index: -1 is out of range"，所以用"文本是否以换行结尾"来判断）
if(_text_len GREATER 0)
    string(REGEX MATCH "\n$" _ends_with_nl "${_text}")
    if(_ends_with_nl STREQUAL "")
        math(EXPR _nlines "${_nlines} + 1")
    endif()
endif()

set(_hits 0)
set(_result "${_text}")

# =============================================================================
#  G4: gen-verilog-token-enum
# =============================================================================
# 原始规则（verible/verilog/parser/BUILD）：
#   { echo '#ifndef VERIBLE_VERILOG_TOKEN_ENUM_H_' ;
#     echo '#define VERIBLE_VERILOG_TOKEN_ENUM_H_' ;
#     echo '// DO NOT EDIT -- generated from $<' ;
#     sed -n '/#ifndef VERILOG_TOKENTYPE/,/#endif/p' $< ;
#     echo '#endif  // VERIBLE_VERILOG_VERILOG_TOKEN_ENUM_H_' ;} > $@
#
# 语义：sed -n + 地址区间 => 从匹配 "#ifndef VERILOG_TOKENTYPE" 的行开始打印，
#       直到之后第一个匹配 "#endif" 的行为止（两端都含）。区间一旦打开，后续行
#       一律输出，即使再次匹配起始模式也**不会**重开区间。本实现严格保持。
# =============================================================================
# 语义：sed -n 的地址区间 => 从匹配 "#ifndef VERILOG_TOKENTYPE" 的行开始，
#       到其后第一个 "#endif" 为止（两端都含），外面包一层 VERIBLE_VERILOG_TOKEN_ENUM_H_ 守卫。
#
# ★★★ 这里踩过一个致命的坑，务必看清 ★★★
#   用 string(REGEX MATCH "(^|\n)([ \t]*#ifndef +VERILOG_TOKENTYPE)" ...) 定位起点是错的：
#   CMake 正则的 `^` **在整段文本上只匹配字符串开头**，而 `(^|\n)` 里 `^` 的
#   空匹配会让整体在位置 0 就"命中"，即使那里根本没有 #ifndef。
#   实测后果：切片从**文件第 1 行**开始，把 include guard（第 38 行）和
#   VERILOG_DEBUG 段一起带了进来 —— 而它们的 #endif 不在切片内，
#   于是产出的 verilog-token-enum.h 变成 "#if 7 个 / #endif 6 个" 的**失衡文件**，
#   下游编译报 C1070: #if/#endif 对不匹配。
#
#   正确做法：老老实实逐行扫描（头文件只有 ~560 行，逐行代价可忽略），
#   用行级正则判断"这一行是不是 #ifndef VERILOG_TOKENTYPE"，
#   再用**嵌套深度计数**找到配对的那个 #endif。
#   注意区分 "#ifndef VERILOG_TOKENTYPE"（第 63 行）与
#   "# define VERILOG_TOKENTYPE"（第 64 行）—— 别把宏定义当成 guard。
#
#   正确切片：tab.hh 第 63 行 ~ 第 543 行，共 481 行；
#   加上外层 3 行头 + 1 行尾 => 约 485 行。产出若是 547 行，说明切错了。
# =============================================================================
if(ACTION STREQUAL "MAKERULE_GEN_TOKEN_ENUM")

    # 逐行扫描，行级判断（不用整段正则的 ^，见上面的坑）
    set(_found_begin FALSE)
    set(_found_end FALSE)
    set(_depth 0)
    set(_capturing FALSE)
    set(_segment "")

    set(_rest "${_text}")
    while(TRUE)
        string(FIND "${_rest}" "\n" _nl)
        if(_nl EQUAL -1)
            set(_line "${_rest}")
            set(_has_more FALSE)
        else()
            string(SUBSTRING "${_rest}" 0 ${_nl} _line)
            math(EXPR _next "${_nl} + 1")
            string(SUBSTRING "${_rest}" ${_next} -1 _rest)
            set(_has_more TRUE)
        endif()

        if(NOT _capturing)
            # 起始行：行首（可带空白）#ifndef <空白>VERILOG_TOKENTYPE
            # 注意不能匹配 "# define VERILOG_TOKENTYPE" —— 那个是 #define 不是 #ifndef
            if(_line MATCHES "^[ \t]*#[ \t]*ifndef[ \t]+VERILOG_TOKENTYPE[ \t]*$")
                set(_capturing TRUE)
                set(_found_begin TRUE)
                set(_depth 0)
                string(APPEND _segment "${_line}\n")
            endif()
        else()
            string(APPEND _segment "${_line}\n")

            # 深度计数：只认行首的预处理指令
            if(_line MATCHES "^[ \t]*#[ \t]*if")
                math(EXPR _depth "${_depth} + 1")
            elseif(_line MATCHES "^[ \t]*#[ \t]*endif")
                if(_depth EQUAL 0)
                    set(_found_end TRUE)
                    set(_capturing FALSE)
                    break()
                endif()
                math(EXPR _depth "${_depth} - 1")
            endif()
        endif()

        if(NOT _has_more)
            break()
        endif()
    endwhile()

    if(NOT _found_begin OR NOT _found_end)
        if(STRICT_ANCHORS)
            message(FATAL_ERROR
                "verible-patch: [g4 gen-verilog-token-enum] 在 ${INPUT} 中找不到"
                " '#ifndef VERILOG_TOKENTYPE' ... 同层 '#endif' 区间"
                "（找到起点=${_found_begin}，找到配对终点=${_found_end}）。\n"
                "  win_bison 版本变化可能改变了头文件的 token 枚举写法。")
        endif()
        set(_result "")
    else()
        set(_result "#ifndef VERIBLE_VERILOG_TOKEN_ENUM_H_\n")
        string(APPEND _result "#define VERIBLE_VERILOG_TOKEN_ENUM_H_\n")
        string(APPEND _result "// DO NOT EDIT -- generated from ${INPUT}\n")
        string(APPEND _result "${_segment}")
        string(APPEND _result "#endif  // VERIBLE_VERILOG_VERILOG_TOKEN_ENUM_H_\n")
        set(_hits 1)
    endif()
# =============================================================================
elseif(ACTION STREQUAL "MAKERULE_GEN_PARSE_IFACE")

    # 定位要删除的枚举段：从 "#ifndef (YY|VERILOG)_TOKENTYPE" 所在行的行首，
    # 到其后第一个 "#endif" 所在行的行尾（含换行）。
    # 只用 FIND + 两次 SUBSTRING 定位边界，不做逐行循环，O(n)。
    set(_anchor_pat "#ifndef +(YYTOKENTYPE|VERILOG_TOKENTYPE)")
    string(REGEX MATCH "${_anchor_pat}" _anchor_hit "${_text}")

    set(_deleted 0)
    set(_body "${_text}")
    if(NOT _anchor_hit STREQUAL "")
        # 1) 锚点位置 -> 所在行行首
        #    ★ 必须用行首锚定的正则去找，不能用 string(FIND "[_anchor_hit]")：
        #      行的中间也可能出现 "#ifndef VERILOG_TOKENTYPE" 之类子串，
        #      实测 FIND 会定位到 include guard 那一行，导致删错区域。
        string(REGEX MATCH "(^|\n)([ \t]*#ifndef +(YYTOKENTYPE|VERILOG_TOKENTYPE))" _anchor_line "${_text}")
        if(_anchor_line STREQUAL "")
            message(FATAL_ERROR
                "verible-patch: [g5 verilog-parse-interface] 无法定位 token 枚举段的行首。")
        endif()
        string(FIND "${_text}" "${_anchor_line}" _line_hit_pos)
        # _anchor_line 形如 "\n  #ifndef VERILOG_TOKENTYPE"，取 #ifndef 前的换行位置
        string(FIND "${_anchor_line}" "#ifndef" _kw_rel)
        math(EXPR _a_pos "${_line_hit_pos} + ${_kw_rel}")
        string(SUBSTRING "${_text}" 0 ${_line_hit_pos} _before)
        set(_del_from ${_line_hit_pos})
        if(_a_pos GREATER _line_hit_pos)
            # 说明匹配带前导 "\n"，枚举段从该换行之后开始
            math(EXPR _del_from "${_line_hit_pos} + 1")
        endif()

        # 2) 从锚点行之后逐行推进，找到与之**同层**的 #endif。
        #    必须做嵌套计数：枚举段里本身就有 "#ifndef VERILOG_X"，而其前面还有
        #    include guard / VERILOG_DEBUG 等块，直接取"第一个 #endif"会停在错误的
        #    位置（实测会停在 include guard 的 #endif，把文件头一起删掉）。
        #    逐行推进只用于头文件（约 560 行），代价可忽略。
        string(SUBSTRING "${_text}" ${_a_pos} -1 _tail)
        set(_depth 0)
        set(_scan "${_tail}")
        set(_line_start 0)
        set(_del_to -1)
        set(_consumed 0)
        while(TRUE)
            string(FIND "${_scan}" "\n" _nl)
            if(_nl EQUAL -1)
                set(_cur "${_scan}")
                set(_eol "")
                set(_advance 0)
                set(_more FALSE)
            else()
                string(SUBSTRING "${_scan}" 0 ${_nl} _cur)
                math(EXPR _advance "${_nl} + 1")
                set(_eol "\n")
                set(_more TRUE)
            endif()

            # 跳过锚点所在的第一行（它就是 #ifndef 本身）
            if(_line_start GREATER 0)
                if(_cur MATCHES "^[ \t]*#[ \t]*if")
                    math(EXPR _depth "${_depth} + 1")
                elseif(_cur MATCHES "^[ \t]*#[ \t]*endif")
                    if(_depth EQUAL 0)
                        math(EXPR _del_to "${_a_pos} + ${_line_start} + ${_nl} + 1")
                        if(_nl EQUAL -1)
                            set(_del_to ${_text_len})
                        endif()
                        break()
                    endif()
                    math(EXPR _depth "${_depth} - 1")
                endif()
            endif()

            if(NOT _more)
                break()
            endif()
            math(EXPR _line_start "${_line_start} + ${_advance}")
            string(SUBSTRING "${_scan}" ${_advance} -1 _scan)
        endwhile()

        if(_del_to LESS 0)
            message(FATAL_ERROR
                "verible-patch: [g5 verilog-parse-interface] ${INPUT} 中 token 枚举段"
                " '#ifndef (YY|VERILOG)_TOKENTYPE' 之后找不到同层的 '#endif'，"
                "拒绝产出可能被截断的头文件。")
        endif()

        # 3) 删除 [_del_from, _del_to)
        string(SUBSTRING "${_text}" 0 ${_del_from} _head_part)
        string(SUBSTRING "${_text}" ${_del_to} -1 _tail_part)
        set(_body "${_head_part}${_tail_part}")
        set(_deleted 1)
    endif()

    set(_hits ${_deleted})
    set(_result "// DO NOT EDIT -- generated from ${INPUT}\n${_body}")

    # ★ 命中 0 次 = 失败。空操作会让本头文件退化成 verilog.tab.hh 的拷贝，
    #   把 enum verilog_tokentype 一起带进来，违背拆分意图且埋下重复定义隐患。
    if(_deleted EQUAL 0 AND STRICT_ANCHORS)
        message(FATAL_ERROR
            "verible-patch: [g5 verilog-parse-interface] 在 ${INPUT} 中找不到 token 枚举段！\n"
            "  预期锚点：一行以 '#ifndef VERILOG_TOKENTYPE'（或旧名 '#ifndef YYTOKENTYPE'）"
            " 开始、直到其后第一个 '#endif' 的枚举定义块。\n"
            "  命中 0 次意味着本次变换是空操作 —— 输出会退化成 verilog.tab.hh 的原样拷贝，"
            "枚举段没有被拆出去。\n"
            "  请核对 win_bison 版本（本工程验证版本：GNU Bison 3.8.2）与 verilog.y 的"
            " `%define api.prefix` 设置。")
    endif()

# =============================================================================
#  G6a: std_move_parser_symbols
# =============================================================================
# 原始规则（verible/common/parser/yacc.bzl）：
#   sed -e '/= yylval;/s|yylval|std::move(&)|' \
#       -e '/= yyval;/s|yyval|std::move(&)|' \
#       -e '/(= yyval_default)/s|yyval_default|std::move(&)|' \
#       -e '/yyval = yyvsp\[1-yylen\];/s|yyval|// &|' < $< > $@
#
# 要点（全部保持）：
#   * 地址 /regex/ 是无条件正则，匹配行内任意位置；
#   * s|A|B| 不带 g => 每行只替换**第一个** A；
#   * 四条 -e 按 1->4 顺序独立作用于同一行，前一条结果影响后一条判断；
#   * 第 4 条把该行第一个 yyval 换成 "// yyval"，把整条语句注释掉。
# 实现（性能关键）：不用逐行拆分。string(SUBSTRING) 反复切片是 O(n^2)，
#   .tab.cc 有 27001 行 / 1.6 MB，逐行实测 100 s+。改为 4 次整文本 REGEX REPLACE：
#   CMake 正则不是多行模式，"." 不匹配换行，用 (^|\n) 作为行边界捕获，
#   ([^\n]*) 吃掉行内剩余部分。替换文本回填 \1\2，O(n) 一次扫描。
#
#   注意 CMake 正则里 \n 只能出现在字符类中作为真正的换行；在字符类外要写成
#   真正的换行符——本文件用 "\n" 字符串（CMake 会解码为换行符），因此
#   (^|\n) 与 [^\n] 都能正常工作（已实测）。
#
# 目的：verilog.y 里 `%define api.value.type {::verible::SymbolPtr}`，即
#   YYSTYPE = std::unique_ptr<Symbol>。不打补丁时 unique_ptr 的拷贝赋值被 delete，
#   MSVC 报 C2280；打了补丁才能编译。所以 G6a 一旦编译通过就说明补丁正确生效。
# =============================================================================
elseif(ACTION STREQUAL "MAKERULE_MOVE_SYMBOLS")

    # 命中计数必须在**输入**上用逐次 FIND 统计（不能用 MATCHALL 后 list(LENGTH)，
    # 因为 MATCHALL 的结果含 ';' 会被 list 拆开，计数虚高）。
    # 每个锚点单独计数；同一行命中两个不同锚点算 2，与 sed 两条 -e 各替换一次的语义一致。
    #
    # ★ 计数不能写成 "FIND + SUBSTRING 反复切片"：那样每次切片都要复制剩下的整段文本，
    #   27000 行 / 1.6 MB 实测要 36 s。这里用一次 REGEX REPLACE 把"锚点"全部替换成
    #   单个字符，再比较替换前后的长度差 / 来数 —— 也可以直接用 MATCHALL，
    #   但 MATCHALL 的结果含 ';' 会被 list 拆开。
    #   最稳妥的做法：把锚点替换成同一个定长标记，然后用长度差算次数。
    set(_hits 0)
    foreach(_anchor IN ITEMS "= yylval;" "= yyval;" "(= yyval_default)" "yyval = yyvsp[1-yylen];")
        string(LENGTH "${_anchor}" _alen)
        # 把锚点替换为空串，长度差 = 命中次数 * 锚点长度
        if(_anchor STREQUAL "(= yyval_default)")
            set(_pattern "\\(= yyval_default\\)")
        elseif(_anchor STREQUAL "yyval = yyvsp[1-yylen];")
            set(_pattern "yyval = yyvsp\\[1-yylen\\];")
        elseif(_anchor STREQUAL "= yylval;")
            set(_pattern "= yylval;")
        else()
            set(_pattern "= yyval;")
        endif()
        string(REGEX REPLACE "${_pattern}" "" _stripped "${_text}")
        string(LENGTH "${_stripped}" _slen)
        math(EXPR _diff "${_text_len} - ${_slen}")
        if(_alen GREATER 0)
            math(EXPR _cnt "${_diff} / ${_alen}")
            math(EXPR _hits "${_hits} + ${_cnt}")
        endif()
    endforeach()

# 顺序必须严格 1->4（原 sed 的 -e 顺序），每条独立作用于上一轮结果。
    #
    # ★★ 性能教训（这是本任务踩的第三个 CMake 性能坑）★★
    #   CMake 的 string(APPEND) 在长循环里是 **O(n²)**：
    #   实测在 27001 行的循环里拼接字符串要 **22 秒**；
    #   而一次 string(REPLACE) 处理整个 1.6 MB 文件只要 **1 ms**。
    #   早期版本用"逐行 APPEND"实现，g6a 从 0.5 s 退化到 66~71 s，就是这么来的。
    #   => 结论：**绝不要在逐行循环里 string(APPEND) 累积大文本。**
    #
    # ★★ 重复锚点校验 —— 必须避免"每轮 SUBSTRING 剩余全文"的写法 ★★
    #   早期写法在循环里 string(SUBSTRING "${_text}" ${scan} -1 _win)，
    #   每次复制剩余全文 => O(n²)，实测 120 s+ 跑不完。
    #   正确做法：用一次 REPLACE 把锚点全部替换成定长标记，靠**长度差**算出
    #   全局出现次数（O(n)），再对"同一行内重复"做一个便宜的必要条件检查。
    #
    #   关键洞察：若每个锚点的全局出现次数 == "含该锚点的行数"，则每行至多一个。
    #   "含该锚点的行数" 可以通过把锚点替换成 "" 后再数换行差得到 —— 仍然 O(n)。
    #   这里采用更直接的判据：
    #     count(anchor)  <= count(lines containing anchor)
    #   只要 count(anchor) 与"该锚点前后紧邻换行"的关系一致即可判定。
    #   为简洁与可靠，本实现用一个**宽松但足够**的检查：
    #   只有当 count(anchor) > 1 时才去做精确的行内重复检测，
    #   而精确检测只针对极少数命中位置（每类锚点命中 1~2 处），代价可忽略。
    set(_multi_lines 0)
    foreach(_needle IN ITEMS "= yylval;" "= yyval;" "yyval = yyvsp[1-yylen];")
        string(LENGTH "${_needle}" _nlen)
        # 全局计数（O(n)，用 REPLACE + 长度差）
        string(REPLACE "${_needle}" "" _no_needle "${_text}")
        string(LENGTH "${_no_needle}" _nlen_after)
        math(EXPR _diff "${_text_len} - ${_nlen_after}")
        math(EXPR _total "${_diff} / ${_nlen}")
        if(_total LESS 2)
            continue()
        endif()

        # 仅在"全局 >=2"时才逐位置检查是否同行（命中位置很少）
        set(_pos 0)
        set(_prev_abs -1)
        set(_prev_had_nl TRUE)
        while(_pos LESS _text_len)
            # 从 _pos 起找下一个锚点：只 SUBSTRING 一次到"找到为止"，
            # 且因为命中很少，这里最多切 2~3 次
            string(SUBSTRING "${_text}" ${_pos} -1 _win)
            string(FIND "${_win}" "${_needle}" _p)
            if(_p EQUAL -1)
                break()
            endif()
            math(EXPR _abs "${_pos} + ${_p}")
            # 检查"上一处命中"与"本处命中"之间是否有换行
            if(_prev_abs GREATER_EQUAL 0)
                math(EXPR _gap_start "${_prev_abs}")
                math(EXPR _gap_len "${_abs} - ${_prev_abs}")
                string(SUBSTRING "${_text}" ${_gap_start} ${_gap_len} _gap)
                string(FIND "${_gap}" "\n" _gap_nl)
                if(_gap_nl EQUAL -1)
                    math(EXPR _multi_lines "${_multi_lines} + 1")
                endif()
            endif()
            set(_prev_abs ${_abs})
            math(EXPR _pos "${_abs} + ${_nlen}")
            if(_pos GREATER_EQUAL _text_len)
                break()
            endif()
        endwhile()
    endforeach()

    if(_multi_lines GREATER 0 AND STRICT_ANCHORS)
        message(FATAL_ERROR
            "verible-patch: [g6a] 发现 ${_multi_lines} 处'同一行内重复锚点'。\n"
            "  sed 的 s|A|B| 不带 g，每行只替换第一个；而本实现用全局替换，\n"
            "  在有重复锚点的行上会与 sed 语义不同。\n"
            "  win_bison 的 .tab.cc 正常不会出现这种情况，请先核对工具版本。")
    endif()

    # 命中计数（在输入上数，用于诊断头与 hits>0 断言）
    #   用 string(REPLACE) 删掉锚点再按长度差算次数 —— REPLACE 是 O(n) 且很快
    #   （整文件 1 ms），远好于 FIND+SUBSTRING 循环。
    set(_hits 0)
    foreach(_anchor IN ITEMS "= yylval;" "= yyval;" "yyval = yyvsp[1-yylen];")
        string(LENGTH "${_anchor}" _plen)
        string(REPLACE "${_anchor}" "" _tmp "${_text}")
        string(LENGTH "${_tmp}" _tlen2)
        math(EXPR _diff "${_text_len} - ${_tlen2}")
        if(_plen GREATER 0)
            math(EXPR _c "${_diff} / ${_plen}")
            math(EXPR _hits "${_hits} + ${_c}")
        endif()
    endforeach()
    # "(= yyval_default)"
    string(FIND "${_text}" "(= yyval_default)" _dfind)
    if(NOT _dfind EQUAL -1)
        math(EXPR _hits "${_hits} + 1")
    endif()

    # 实际替换：4 次整文本 REGEX REPLACE，O(n)，每次约 1 ms。
    #   顺序严格 1->4：
    #     规则 4 必须最后，因为 "yyval = yyvsp[1-yylen];" 这行本身含 "yyval ="，
    #     若规则 2 先跑会把它误改成 "std::move(yyval) = ..."。
    set(_body "${_text}")
    string(REGEX REPLACE "= yylval;"            "= std::move(yylval);"            _body "${_body}")
    string(REGEX REPLACE "= yyval;"             "= std::move(yyval);"             _body "${_body}")
    string(REGEX REPLACE "\\(= yyval_default\\)" "(= std::move(yyval_default))"    _body "${_body}")
    string(REGEX REPLACE "yyval = yyvsp\\[1-yylen\\];" "// yyval = yyvsp[1-yylen];" _body "${_body}")

    set(_result "${_body}")

    if(_hits EQUAL 0 AND STRICT_ANCHORS)
        message(FATAL_ERROR
            "verible-patch: [g6a std_move_parser_symbols] 在 ${INPUT} 中一行锚点都没命中！\n"
            "  期望至少出现其中之一：'= yylval;' / '= yyval;' / '(= yyval_default)' /"
            " 'yyval = yyvsp[1-yylen];'\n"
            "  说明 win_bison 生成的 .tab.cc 结构与 c6b29d62 期望的不同。\n"
            "  注意：G6a 失效会让 unique_ptr 拷贝赋值被 delete，通常表现为编译报 C2280。")
    endif()

# =============================================================================
#  G6b: record_recovered_syntax_errors   ★ 最关键，禁止静默失效 ★
# =============================================================================
# 原始规则（verible/common/parser/yacc.bzl）：
#   sed -e '/++yynerrs;/a\
#     // Automatically patched by >>record_recovered_syntax_errors<< rule:\
#     param->RecordSyntaxError(yylval);\
#     // end of automatic patch\
#     ' < $< > $@
#
# 语义：每一处含 "++yynerrs;" 的行**之后**追加三行；sed 的 a\ 保留前导空白，
#       即插入行继承匹配行的缩进。匹配几次就插几组。
#
# 实现：整个文件只做 **一次** REGEX REPLACE，O(n)。
#   行边界用 (^|\n) 捕获；([ \t]*) 捕获该行缩进，回填给插入的三行，
#   复刻 sed a\ 的"继承缩进"行为。
#   注意模式里两个 '+' 都是元字符，必须写成 \\+\\+。
#
# 为什么关键：ParserParam::RecordSyntaxError() 把出错 token 存进
#   recovered_syntax_errors_ -> RejectedTokens() -> FileAnalyzer 生成
#   "file:line:col syntax error at token ..." 诊断。补丁失效 => token 序列仍一致，
#   但**错误消息全部消失**，语言服务器诊断功能静默退化。
# =============================================================================
elseif(ACTION STREQUAL "MAKERULE_RECORD_SYNTAX_ERRORS")

    # 计数：用逐次 FIND + 偏移前进（只维护一个起始下标，不复制大字符串，
    # 因此是 O(n · k) 且常数极小）。不要用"删掉再除长度"，容易被长度算错坑。
    set(_hits 0)
    set(_scan_from 0)
    while(TRUE)
        string(SUBSTRING "${_text}" ${_scan_from} -1 _win)
        string(FIND "${_win}" "++yynerrs;" _rel)
        if(_rel EQUAL -1)
            break()
        endif()
        math(EXPR _hits "${_hits} + 1")
        math(EXPR _scan_from "${_scan_from} + ${_rel} + 10")   # 10 = strlen("++yynerrs;")
        if(_scan_from GREATER_EQUAL _text_len)
            break()
        endif()
    endwhile()

    # 一次替换：在每一行 "++yynerrs;" 之后插入三行，缩进沿用该行缩进。
    #
    # ★ 捕获组只能含"行首 + 缩进"，绝不能把 yynerrs 语句本身也放进捕获组并在
    #   替换文本里回填 —— 否则会把 "++yynerrs;" 拼到插入行前面，生成
    #   "++yynerrs;param->RecordSyntaxError(yylval);" 这种坏代码（实测踩过）。
    #   正确做法：\1 = 行边界(^|\n)，\2 = 缩进([ \t]*)，语句本体用 [^\n]* 消费但
    #   不捕获，然后在后面重新写一遍原语句——这里利用"整行匹配"方式：
    #   匹配 (^|\n)([ \t]*)\+\+yynerrs;([^\n]*)  ->  \1\2++yynerrs;\3\n<ins>
    #
    # ★★ 幂等性保护（重复应用不会重复插入）★★
    #   如果输入里**已经**有补丁标记（说明这个文件被本规则处理过），
    #   就跳过插入，只原样输出。否则"对已打补丁的文件再跑一次"会插入两遍：
    #       正确:  ++yynerrs; \n marker \n Record \n end
    #       重复:  ++yynerrs; \n marker \n Record \n end
    #              marker \n Record \n end        <- 多出来的一组
    #   这在增量构建里会真实发生（例如构建系统重跑了 custom_command 但输入没变），
    #   多出来的重复插入会让同一个错误被记录多次。
    #   实测：单测"对已打补丁的文件再跑"用例抓住过这个问题。
    set(_body "${_text}")
    string(FIND "${_text}" ">>record_recovered_syntax_errors<<" _already)
    if(NOT _already EQUAL -1)
        message(STATUS
            "verible-patch: [g6b] 输入已含补丁标记，跳过插入（幂等保护）。"
            " hits=${_hits}")
    else()
        string(REGEX REPLACE
            "(^|\n)([ \t]*)\\+\\+yynerrs;([^\n]*)"
            "\\1\\2++yynerrs;\\3\n\\2// Automatically patched by >>record_recovered_syntax_errors<< rule:\n\\2param->RecordSyntaxError(yylval);\n\\2// end of automatic patch"
            _body "${_body}")
    endif()

    set(_result "${_body}")

    if(_hits EQUAL 0 AND STRICT_ANCHORS)
        message(FATAL_ERROR
            "verible-patch: [g6b record_recovered_syntax_errors] 在 ${INPUT} 中找不到任何"
            " '++yynerrs;' ！\n\n"
            "  ★ 这是 Verible 错误恢复能力的关键补丁：保证语法错误被记录到 ParserParam。\n"
            "    补丁失配后代码仍能编译通过，但会【静默丢失】所有语法错误记录，"
            "语言服务器的诊断功能将失效。\n"
            "  ★ 请核对 win_bison 版本（本工程验证版本：GNU Bison 3.8.2），必要时更新\n"
            "     cmake/scripts/verible-patch.cmake 的锚点，并在 tests/verible-generated"
            " 补一条用例。")
    endif()

else()
    message(FATAL_ERROR "verible-patch.cmake: 未知 ACTION='${ACTION}'")
endif()

# -----------------------------------------------------------------------------
# 加诊断头并写出（_result 已经是"每行带 \n"的完整正文）
# -----------------------------------------------------------------------------
file(WRITE "${OUTPUT}" "// verible-patch: ${_mode_tag} hits=${_hits} lines=${_nlines}\n${_result}")

message(STATUS "verible-patch: ${_mode_tag}  hits=${_hits}  lines=${_nlines}  -> ${OUTPUT}")
