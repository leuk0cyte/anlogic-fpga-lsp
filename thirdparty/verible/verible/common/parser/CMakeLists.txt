# =============================================================================
#  GENERATED FILE - DO NOT EDIT
# =============================================================================
#  由 tools/gen_verible_cmake.py 从同目录的 Bazel BUILD 自动转译而来。
#  手工修改会在下次运行生成器时被覆盖；要改行为请改生成器本身。
#
#  源包: //verible/common/parser
#
#  目录里也没有 BUILD 生成物时，本文件只声明库，不在这里编测试
#  （阶段 0 不编 Verible 自带的 225 个 cc_test）。
# =============================================================================
# ---------------------------------------------------------------------------
# 本包有 1 个 cc_test —— 按 CLAUDE 约定不编译 Verible 自带测试；
# 需要时用 `--with-tests` 重新生成。
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# verible::common::parser::bison-parser-adapter   (纯头文件库)   <- BUILD: cc_library(name = "bison-parser-adapter")
# ---------------------------------------------------------------------------
add_library(verible__common__parser__bison-parser-adapter INTERFACE)
target_sources(verible__common__parser__bison-parser-adapter INTERFACE "bison-parser-adapter.h")

add_library(verible::common::parser::bison-parser-adapter ALIAS verible__common__parser__bison-parser-adapter)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__parser__bison-parser-adapter INTERFACE verible_common_opts)

target_link_libraries(verible__common__parser__bison-parser-adapter INTERFACE
    verible::common::parser::parse
    verible::common::parser::parser-param
    verible::common::lexer::token-generator
    verible::common::text::concrete-syntax-tree
    verible::common::text::token-info
    verible::common::util::logging
    absl::status
)

# ---------------------------------------------------------------------------
# verible::common::parser::bison-parser-common   (静态库)   <- BUILD: cc_library(name = "bison-parser-common")
# ---------------------------------------------------------------------------
add_library(verible__common__parser__bison-parser-common STATIC "bison-parser-common.cc")
target_sources(verible__common__parser__bison-parser-common PRIVATE "bison-parser-common.h")

add_library(verible::common::parser::bison-parser-common ALIAS verible__common__parser__bison-parser-common)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__parser__bison-parser-common PRIVATE verible_common_opts)

target_link_libraries(verible__common__parser__bison-parser-common PUBLIC
    verible::common::parser::parser-param
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::token-info
    verible::common::util::logging
)

# ---------------------------------------------------------------------------
# verible::common::parser::parse   (纯头文件库)   <- BUILD: cc_library(name = "parse")
# ---------------------------------------------------------------------------
add_library(verible__common__parser__parse INTERFACE)
target_sources(verible__common__parser__parse INTERFACE "parse.h")

add_library(verible::common::parser::parse ALIAS verible__common__parser__parse)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__parser__parse INTERFACE verible_common_opts)

target_link_libraries(verible__common__parser__parse INTERFACE
    verible::common::text::concrete-syntax-tree
    verible::common::text::token-info
    absl::status
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::common::parser::parser-param   (静态库)   <- BUILD: cc_library(name = "parser-param")
# ---------------------------------------------------------------------------
add_library(verible__common__parser__parser-param STATIC "parser-param.cc")
target_sources(verible__common__parser__parser-param PRIVATE "parser-param.h")

add_library(verible::common::parser::parser-param ALIAS verible__common__parser__parser-param)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__parser__parser-param PRIVATE verible_common_opts)

target_link_libraries(verible__common__parser__parser-param PUBLIC
    verible::common::lexer::token-generator
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::token-info
    verible::common::util::casts
    verible::common::util::logging
)

# ---------------------------------------------------------------------------
# verible::common::parser::parser-test-util   (纯头文件库)   <- BUILD: cc_library(name = "parser-test-util")
# ---------------------------------------------------------------------------
add_library(verible__common__parser__parser-test-util INTERFACE)
target_sources(verible__common__parser__parser-test-util INTERFACE "parser-test-util.h")

add_library(verible::common::parser::parser-test-util ALIAS verible__common__parser__parser-test-util)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__parser__parser-test-util INTERFACE verible_common_opts)

target_link_libraries(verible__common__parser__parser-test-util INTERFACE
    verible::common::analysis::matcher::descent-path
    verible::common::text::parser-verifier
    verible::common::text::symbol
    verible::common::text::token-info-test-util
    verible::common::util::logging
    absl::die_if_null
    absl::status
    GTest::gtest
)

# BUILD: testonly = 1（本阶段不编译测试，库照常生成以便被引用）
