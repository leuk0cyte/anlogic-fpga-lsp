# =============================================================================
#  cmake/scripts/verible-nonterminals.cmake
# =============================================================================
#  等价于 thirdparty/verible/verible/verilog/CST/BUILD 里的 genrule：
#
#    genrule(
#        name = "verilog-nonterminals-foreach-gen",
#        srcs = ["verilog-nonterminals.h"],
#        outs = ["verilog_nonterminals_foreach-gen.inc"],
#        cmd  = "sed -n -e '/BEGIN GENERATE/,/END GENERATE/p' $< | "
#               "grep -v GENERATE | "
#               "sed -e 's|^ *|CONSIDER(|' -e 's| =.*,|,|' -e 's|,|)|' > $@",
#    )
#
#  输入（verilog-nonterminals.h 的枚举段）形如：
#      kExpression,
#      kBinaryExpression,
#      kUntagged = verible::kUntagged,
#  输出形如：
#      CONSIDER(kExpression,)
#      CONSIDER(kBinaryExpression,)
#      CONSIDER(kUntagged,)
#
#  直接用 cmake -P 调用，Windows 上没有 sed/grep 也能跑：
#      cmake -DIN=<h> -DOUT=<inc> -P cmake/scripts/verible-nonterminals.cmake
# =============================================================================

if(NOT DEFINED IN OR NOT DEFINED OUT)
    message(FATAL_ERROR "verible-nonterminals.cmake 需要 -DIN=<header> -DOUT=<output>")
endif()
if(NOT EXISTS "${IN}")
    message(FATAL_ERROR "verible-nonterminals.cmake: 输入不存在: ${IN}")
endif()

# 逐行读，保留所有行（含空行），用正则抓 BEGIN/END GENERATE 之间的内容
file(STRINGS "${IN}" _lines)

set(_inside FALSE)
set(_result "")
set(_count 0)

foreach(_line IN LISTS _lines)
    # 1) 找 BEGIN GENERATE 段起点
    if(_line MATCHES "BEGIN GENERATE")
        set(_inside TRUE)
        continue()
    endif()
    if(_line MATCHES "END GENERATE")
        set(_inside FALSE)
        continue()
    endif()
    if(NOT _inside)
        continue()
    endif()

    # 2) 去掉行首空白
    string(REGEX REPLACE "^[ \t]+" "" _s "${_line}")
    # 2b) 去掉行尾 // 注释（源文件里有 "kBaseDigits,  // e.g. 'd 1234" 这种）
    string(REGEX REPLACE "//.*$" "" _s "${_s}")
    string(REGEX REPLACE "[ \t]+$" "" _s "${_s}")
    # 3) 空行 / 纯注释行跳过（Bazel 的 grep -v GENERATE 之后 sed 会把它们变成垃圾；
    #    头文件里这段只有枚举项，跳过空行更稳）
    if(_s STREQUAL "")
        continue()
    endif()
    # 4) 去掉 " = ..." 尾部的显式赋值（对应 sed 's| =.*,|,|'）
    string(REGEX REPLACE "[ \t]*=.*" "" _s "${_s}")
    # 5) 去掉结尾空白与逗号
    string(REGEX REPLACE "[ \t\r\n]+$" "" _s "${_s}")
    string(REGEX REPLACE ",+$" "" _s "${_s}")
    if(_s STREQUAL "")
        continue()
    endif()

    # 6) 包成 CONSIDER(<name>)
    #    对应 sed 's|^ *|CONSIDER(|' 之后再来一次 's|,|)|'：
    #    sed 的 s|,|)| 只替换行内【第一个】逗号，所以
    #      "kExpression,"        -> "CONSIDER(kExpression,)" -> "CONSIDER(kExpression)"
    #      "kUntagged = X,"      -> 先 s| =.*,|,| 变 "kUntagged," -> "CONSIDER(kUntagged)"
    #    即右括号吃掉的是那个分隔逗号，最终形如 CONSIDER(kExpression)。
    string(APPEND _result "CONSIDER(${_s})\n")
    math(EXPR _count "${_count} + 1")
endforeach()

if(_count EQUAL 0)
    message(FATAL_ERROR
        "verible-nonterminals.cmake: 在 ${IN} 里没找到 BEGIN GENERATE / END GENERATE 段内的枚举项。"
        "上游头文件结构可能变了，请检查 genrule 是否仍然适用。")
endif()

# 只在内容变化时写，避免无谓的重编
set(_old "")
if(EXISTS "${OUT}")
    file(READ "${OUT}" _old)
endif()
if(NOT _old STREQUAL _result)
    get_filename_component(_outdir "${OUT}" DIRECTORY)
    file(MAKE_DIRECTORY "${_outdir}")
    file(WRITE "${OUT}" "${_result}")
    message(STATUS "verible-nonterminals.cmake: 生成 ${OUT}（${_count} 项）")
else()
    message(STATUS "verible-nonterminals.cmake: ${OUT} 已是最新（${_count} 项）")
endif()