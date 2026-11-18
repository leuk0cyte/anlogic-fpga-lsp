# =============================================================================
#  GENERATED FILE - DO NOT EDIT
# =============================================================================
#  由 tools/gen_verible_cmake.py 从同目录的 Bazel BUILD 自动转译而来。
#  手工修改会在下次运行生成器时被覆盖；要改行为请改生成器本身。
#
#  源包: //verible/verilog/parser
#
#  目录里也没有 BUILD 生成物时，本文件只声明库，不在这里编测试
#  （阶段 0 不编 Verible 自带的 225 个 cc_test）。
# =============================================================================
# ---------------------------------------------------------------------------
# BUILD 里的 genrule（Bazel 的 sed/grep 在 Windows 上不可用，改用 cmake -P）
# ---------------------------------------------------------------------------
# genrule(gen-verilog-token-enum) -> verilog-token-enum.h
#   TODO(阶段 0-D)：该生成规则属于 flex/bison 链路，
#   由 cmake/scripts/ 实现，本脚本不重复生成。

# genrule(verilog-parse-interface) -> verilog-parse-interface.h
#   TODO(阶段 0-D)：该生成规则属于 flex/bison 链路，
#   由 cmake/scripts/ 实现，本脚本不重复生成。

# ---------------------------------------------------------------------------
# 本包有 4 个 cc_test —— 按 CLAUDE 约定不编译 Verible 自带测试；
# 需要时用 `--with-tests` 重新生成。
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# verible::verilog::parser::verilog-lexer   (静态库)   <- BUILD: cc_library(name = "verilog-lexer")
# ---------------------------------------------------------------------------
add_library(verible__verilog__parser__verilog-lexer STATIC "verilog-lexer.cc")
target_sources(verible__verilog__parser__verilog-lexer PRIVATE "verilog-lexer.h")

# 构建期生成的文件（configure 时还不存在，由 cmake/Verible.cmake 挂接）：
set_property(TARGET verible__verilog__parser__verilog-lexer PROPERTY verible_generated_sources "verilog.yy.cc")

add_library(verible::verilog::parser::verilog-lexer ALIAS verible__verilog__parser__verilog-lexer)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__parser__verilog-lexer PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__parser__verilog-lexer PUBLIC
    verible::verilog::parser::verilog-token-enum
    verible::common::lexer::flex-lexer-adapter
    verible::common::text::token-info
)

# 以下 BUILD 依赖指向阶段 1 才纳入的包，本阶段不链接（否则 target 不存在）：
#   - //bazel:flex

# ---------------------------------------------------------------------------
# verible::verilog::parser::verilog-token-enum   (纯头文件库)   <- BUILD: cc_library(name = "verilog-token-enum")
# ---------------------------------------------------------------------------
add_library(verible__verilog__parser__verilog-token-enum INTERFACE)

# 构建期生成的文件（configure 时还不存在，由 cmake/Verible.cmake 挂接）：
set_property(TARGET verible__verilog__parser__verilog-token-enum PROPERTY verible_generated_headers "verilog-token-enum.h")

add_library(verible::verilog::parser::verilog-token-enum ALIAS verible__verilog__parser__verilog-token-enum)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__parser__verilog-token-enum INTERFACE verible_common_opts)

# ---------------------------------------------------------------------------
# verible::verilog::parser::verilog-y-cc   (静态库)   <- BUILD: cc_library(name = "verilog-y-cc")
# ---------------------------------------------------------------------------
add_library(verible__verilog__parser__verilog-y-cc STATIC "${VERIBLE_PLACEHOLDER_SRC}")
#  ^ 空壳：真正的源文件由 cmake/Verible.cmake 在生成规则就绪后 target_sources 补挂

# 构建期生成的文件（configure 时还不存在，由 cmake/Verible.cmake 挂接）：
set_property(TARGET verible__verilog__parser__verilog-y-cc PROPERTY verible_generated_sources "verilog-final.tab.cc")
set_property(TARGET verible__verilog__parser__verilog-y-cc PROPERTY verible_generated_headers "verilog.tab.hh" "verilog-parse-interface.h")

add_library(verible::verilog::parser::verilog-y-cc ALIAS verible__verilog__parser__verilog-y-cc)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__parser__verilog-y-cc PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__parser__verilog-y-cc PUBLIC
    verible::common::parser::bison-parser-common
    verible::common::parser::parser-param
    verible::common::text::tree-utils
    verible::common::util::casts
    verible::common::util::logging
    verible::verilog::CST::DPI
    verible::verilog::CST::declaration
    verible::verilog::CST::expression
    verible::verilog::CST::functions
    verible::verilog::CST::module
    verible::verilog::CST::parameters
    verible::verilog::CST::port
    verible::verilog::CST::type
    verible::verilog::CST::verilog-nonterminals
    verible::verilog::CST::verilog-treebuilder-utils
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::parser::verilog-parser   (静态库)   <- BUILD: cc_library(name = "verilog-parser")
# ---------------------------------------------------------------------------
add_library(verible__verilog__parser__verilog-parser STATIC "verilog-parser.cc")
target_sources(verible__verilog__parser__verilog-parser PRIVATE "verilog-parser.h")

add_library(verible::verilog::parser::verilog-parser ALIAS verible__verilog__parser__verilog-parser)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__parser__verilog-parser PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__parser__verilog-parser PUBLIC
    verible::verilog::parser::verilog-y-cc
    verible::common::parser::bison-parser-adapter
    verible::common::parser::parse
    verible::common::parser::parser-param
    verible::common::util::value-saver
    absl::flags
)

# ---------------------------------------------------------------------------
# verible::verilog::parser::verilog-lexical-context   (静态库)   <- BUILD: cc_library(name = "verilog-lexical-context")
# ---------------------------------------------------------------------------
add_library(verible__verilog__parser__verilog-lexical-context STATIC "verilog-lexical-context.cc")
target_sources(verible__verilog__parser__verilog-lexical-context PRIVATE "verilog-lexical-context.h")

add_library(verible::verilog::parser::verilog-lexical-context ALIAS verible__verilog__parser__verilog-lexical-context)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__parser__verilog-lexical-context PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__parser__verilog-lexical-context PUBLIC
    verible::verilog::parser::verilog-token-enum
    verible::common::text::token-info
    verible::common::text::token-stream-view
    verible::common::util::logging
    verible::common::util::with-reason
)

# ---------------------------------------------------------------------------
# verible::verilog::parser::verilog-token-classifications   (静态库)   <- BUILD: cc_library(name = "verilog-token-classifications")
# ---------------------------------------------------------------------------
add_library(verible__verilog__parser__verilog-token-classifications STATIC "verilog-token-classifications.cc")
target_sources(verible__verilog__parser__verilog-token-classifications PRIVATE "verilog-token-classifications.h")

add_library(verible::verilog::parser::verilog-token-classifications ALIAS verible__verilog__parser__verilog-token-classifications)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__parser__verilog-token-classifications PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__parser__verilog-token-classifications PUBLIC
    verible::verilog::parser::verilog-token-enum
)

# ---------------------------------------------------------------------------
# verible::verilog::parser::verilog-token   (静态库)   <- BUILD: cc_library(name = "verilog-token")
# ---------------------------------------------------------------------------
add_library(verible__verilog__parser__verilog-token STATIC "verilog-token.cc")
target_sources(verible__verilog__parser__verilog-token PRIVATE "verilog-token.h")

add_library(verible::verilog::parser::verilog-token ALIAS verible__verilog__parser__verilog-token)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__parser__verilog-token PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__parser__verilog-token PUBLIC
    verible::verilog::parser::verilog-parser
    verible::verilog::parser::verilog-token-enum
)
