# =============================================================================
#  GENERATED FILE - DO NOT EDIT
# =============================================================================
#  由 tools/gen_verible_cmake.py 从同目录的 Bazel BUILD 自动转译而来。
#  手工修改会在下次运行生成器时被覆盖；要改行为请改生成器本身。
#
#  源包: //verible/verilog/CST
#
#  目录里也没有 BUILD 生成物时，本文件只声明库，不在这里编测试
#  （阶段 0 不编 Verible 自带的 225 个 cc_test）。
# =============================================================================
# ---------------------------------------------------------------------------
# BUILD 里的 genrule（Bazel 的 sed/grep 在 Windows 上不可用，改用 cmake -P）
# ---------------------------------------------------------------------------
# genrule(verilog-nonterminals-foreach-gen) -> verilog_nonterminals_foreach-gen.inc
#   从 verilog-nonterminals.h 的 BEGIN GENERATE / END GENERATE 段
#   抽出 CONSIDER(...) 列表。
#   实现：cmake/scripts/verible-nonterminals.cmake（cmake -P 脚本）
#   挂接：cmake/Verible.cmake 的 add_custom_command（本文件不重复声明，
#         否则会与 Verible.cmake 里的 OUTPUT 撞车）

# ---------------------------------------------------------------------------
# 本包有 26 个 cc_test —— 按 CLAUDE 约定不编译 Verible 自带测试；
# 需要时用 `--with-tests` 重新生成。
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# verible::verilog::CST::verilog-nonterminals   (静态库)   <- BUILD: cc_library(name = "verilog-nonterminals")
# ---------------------------------------------------------------------------
add_library(verible__verilog__CST__verilog-nonterminals STATIC "verilog-nonterminals.cc")
target_sources(verible__verilog__CST__verilog-nonterminals PRIVATE "verilog-nonterminals.h" "verilog_nonterminals_foreach.inc")

# 构建期生成的文件（configure 时还不存在，由 cmake/Verible.cmake 挂接）：
set_property(TARGET verible__verilog__CST__verilog-nonterminals PROPERTY verible_generated_sources "verilog_nonterminals_foreach-gen.inc")

add_library(verible::verilog::CST::verilog-nonterminals ALIAS verible__verilog__CST__verilog-nonterminals)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__CST__verilog-nonterminals PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__CST__verilog-nonterminals PUBLIC
    verible::common::text::constants
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::verilog::CST::verilog-matchers   (静态库)   <- BUILD: cc_library(name = "verilog-matchers")
# ---------------------------------------------------------------------------
add_library(verible__verilog__CST__verilog-matchers STATIC "verilog-matchers.cc" "verilog_nonterminals_foreach.inc")
target_sources(verible__verilog__CST__verilog-matchers PRIVATE "verilog-matchers.h")

# 构建期生成的文件（configure 时还不存在，由 cmake/Verible.cmake 挂接）：
set_property(TARGET verible__verilog__CST__verilog-matchers PROPERTY verible_generated_sources "verilog_nonterminals_foreach-gen.inc")

add_library(verible::verilog::CST::verilog-matchers ALIAS verible__verilog__CST__verilog-matchers)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__CST__verilog-matchers PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__CST__verilog-matchers PUBLIC
    verible::verilog::CST::verilog-nonterminals
    verible::common::analysis::matcher::matcher-builders
    verible::common::text::symbol
    verible::verilog::parser::verilog-token-enum
)

# ---------------------------------------------------------------------------
# verible::verilog::CST::verilog-treebuilder-utils   (静态库)   <- BUILD: cc_library(name = "verilog-treebuilder-utils")
# ---------------------------------------------------------------------------
add_library(verible__verilog__CST__verilog-treebuilder-utils STATIC "verilog-treebuilder-utils.cc")
target_sources(verible__verilog__CST__verilog-treebuilder-utils PRIVATE "verilog-treebuilder-utils.h")

add_library(verible::verilog::CST::verilog-treebuilder-utils ALIAS verible__verilog__CST__verilog-treebuilder-utils)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__CST__verilog-treebuilder-utils PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__CST__verilog-treebuilder-utils PUBLIC
    verible::verilog::CST::verilog-nonterminals
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::token-info
    verible::common::text::tree-utils
    verible::common::util::logging
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::verilog::CST::match-test-utils   (静态库)   <- BUILD: cc_library(name = "match-test-utils")
# ---------------------------------------------------------------------------
add_library(verible__verilog__CST__match-test-utils STATIC "match-test-utils.cc")
target_sources(verible__verilog__CST__match-test-utils PRIVATE "match-test-utils.h")

add_library(verible::verilog::CST::match-test-utils ALIAS verible__verilog__CST__match-test-utils)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__CST__match-test-utils PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__CST__match-test-utils PUBLIC
    verible::common::analysis::syntax-tree-search
    verible::common::analysis::syntax-tree-search-test-utils
    verible::common::text::text-structure
    verible::verilog::analysis::verilog-analyzer
    GTest::gtest
)

# BUILD: testonly = 1（本阶段不编译测试，库照常生成以便被引用）

# ---------------------------------------------------------------------------
# verible::verilog::CST::constraints   (静态库)   <- BUILD: cc_library(name = "constraints")
# ---------------------------------------------------------------------------
add_library(verible__verilog__CST__constraints STATIC "constraints.cc")
target_sources(verible__verilog__CST__constraints PRIVATE "constraints.h")

add_library(verible::verilog::CST::constraints ALIAS verible__verilog__CST__constraints)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__CST__constraints PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__CST__constraints PUBLIC
    verible::verilog::CST::identifier
    verible::verilog::CST::verilog-matchers
    verible::verilog::CST::verilog-nonterminals
    verible::common::analysis::syntax-tree-search
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::token-info
    verible::common::text::tree-utils
)

# ---------------------------------------------------------------------------
# verible::verilog::CST::DPI   (静态库)   <- BUILD: cc_library(name = "DPI")
# ---------------------------------------------------------------------------
add_library(verible__verilog__CST__DPI STATIC "DPI.cc")
target_sources(verible__verilog__CST__DPI PRIVATE "DPI.h")

add_library(verible::verilog::CST::DPI ALIAS verible__verilog__CST__DPI)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__CST__DPI PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__CST__DPI PUBLIC
    verible::verilog::CST::verilog-matchers
    verible::verilog::CST::verilog-nonterminals
    verible::common::analysis::syntax-tree-search
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::symbol-ptr
    verible::common::text::tree-utils
    verible::common::util::logging
    verible::verilog::parser::verilog-token-classifications
    verible::verilog::parser::verilog-token-enum
)

# ---------------------------------------------------------------------------
# verible::verilog::CST::declaration   (静态库)   <- BUILD: cc_library(name = "declaration")
# ---------------------------------------------------------------------------
add_library(verible__verilog__CST__declaration STATIC "declaration.cc")
target_sources(verible__verilog__CST__declaration PRIVATE "declaration.h")

add_library(verible::verilog::CST::declaration ALIAS verible__verilog__CST__declaration)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__CST__declaration PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__CST__declaration PUBLIC
    verible::verilog::CST::identifier
    verible::verilog::CST::type
    verible::verilog::CST::verilog-matchers
    verible::verilog::CST::verilog-nonterminals
    verible::common::analysis::syntax-tree-search
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::constants
    verible::common::text::symbol
    verible::common::text::symbol-ptr
    verible::common::text::token-info
    verible::common::text::tree-utils
    verible::common::util::container-util
    verible::verilog::parser::verilog-token-enum
)

# ---------------------------------------------------------------------------
# verible::verilog::CST::dimensions   (静态库)   <- BUILD: cc_library(name = "dimensions")
# ---------------------------------------------------------------------------
add_library(verible__verilog__CST__dimensions STATIC "dimensions.cc")
target_sources(verible__verilog__CST__dimensions PRIVATE "dimensions.h")

add_library(verible::verilog::CST::dimensions ALIAS verible__verilog__CST__dimensions)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__CST__dimensions PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__CST__dimensions PUBLIC
    verible::verilog::CST::verilog-matchers
    verible::verilog::CST::verilog-nonterminals
    verible::common::analysis::syntax-tree-search
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::tree-utils
)

# ---------------------------------------------------------------------------
# verible::verilog::CST::expression   (静态库)   <- BUILD: cc_library(name = "expression")
# ---------------------------------------------------------------------------
add_library(verible__verilog__CST__expression STATIC "expression.cc")
target_sources(verible__verilog__CST__expression PRIVATE "expression.h")

add_library(verible::verilog::CST::expression ALIAS verible__verilog__CST__expression)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__CST__expression PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__CST__expression PUBLIC
    verible::verilog::CST::type
    verible::verilog::CST::verilog-matchers
    verible::verilog::CST::verilog-nonterminals
    verible::common::analysis::syntax-tree-search
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::token-info
    verible::common::text::tree-utils
    verible::common::util::casts
    verible::verilog::parser::verilog-token-classifications
    verible::verilog::parser::verilog-token-enum
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::verilog::CST::functions   (静态库)   <- BUILD: cc_library(name = "functions")
# ---------------------------------------------------------------------------
add_library(verible__verilog__CST__functions STATIC "functions.cc")
target_sources(verible__verilog__CST__functions PRIVATE "functions.h")

add_library(verible::verilog::CST::functions ALIAS verible__verilog__CST__functions)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__CST__functions PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__CST__functions PUBLIC
    verible::verilog::CST::identifier
    verible::verilog::CST::type
    verible::verilog::CST::verilog-matchers
    verible::verilog::CST::verilog-nonterminals
    verible::verilog::CST::verilog-treebuilder-utils
    verible::common::analysis::syntax-tree-search
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::tree-utils
)

# ---------------------------------------------------------------------------
# verible::verilog::CST::identifier   (静态库)   <- BUILD: cc_library(name = "identifier")
# ---------------------------------------------------------------------------
add_library(verible__verilog__CST__identifier STATIC "identifier.cc")
target_sources(verible__verilog__CST__identifier PRIVATE "identifier.h")

add_library(verible::verilog::CST::identifier ALIAS verible__verilog__CST__identifier)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__CST__identifier PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__CST__identifier PUBLIC
    verible::verilog::CST::verilog-matchers
    verible::verilog::CST::verilog-nonterminals
    verible::common::analysis::syntax-tree-search
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::tree-utils
    verible::common::util::casts
    verible::common::util::logging
    verible::verilog::parser::verilog-token-classifications
    verible::verilog::parser::verilog-token-enum
)

# ---------------------------------------------------------------------------
# verible::verilog::CST::macro   (静态库)   <- BUILD: cc_library(name = "macro")
# ---------------------------------------------------------------------------
add_library(verible__verilog__CST__macro STATIC "macro.cc")
target_sources(verible__verilog__CST__macro PRIVATE "macro.h")

add_library(verible::verilog::CST::macro ALIAS verible__verilog__CST__macro)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__CST__macro PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__CST__macro PUBLIC
    verible::verilog::CST::verilog-matchers
    verible::verilog::CST::verilog-nonterminals
    verible::common::analysis::syntax-tree-search
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::token-info
    verible::common::text::tree-utils
    verible::common::text::visitors
    verible::verilog::parser::verilog-token-enum
    absl::die_if_null
)

# ---------------------------------------------------------------------------
# verible::verilog::CST::class   (静态库)   <- BUILD: cc_library(name = "class")
# ---------------------------------------------------------------------------
add_library(verible__verilog__CST__class STATIC "class.cc")
target_sources(verible__verilog__CST__class PRIVATE "class.h")

add_library(verible::verilog::CST::class ALIAS verible__verilog__CST__class)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__CST__class PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__CST__class PUBLIC
    verible::verilog::CST::identifier
    verible::verilog::CST::verilog-matchers
    verible::verilog::CST::verilog-nonterminals
    verible::common::analysis::syntax-tree-search
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::tree-utils
    verible::common::text::visitors
)

# ---------------------------------------------------------------------------
# verible::verilog::CST::module   (静态库)   <- BUILD: cc_library(name = "module")
# ---------------------------------------------------------------------------
add_library(verible__verilog__CST__module STATIC "module.cc")
target_sources(verible__verilog__CST__module PRIVATE "module.h")

add_library(verible::verilog::CST::module ALIAS verible__verilog__CST__module)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__CST__module PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__CST__module PUBLIC
    verible::verilog::CST::verilog-matchers
    verible::verilog::CST::verilog-nonterminals
    verible::common::analysis::syntax-tree-search
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::token-info
    verible::common::text::tree-utils
    verible::common::util::logging
)

# ---------------------------------------------------------------------------
# verible::verilog::CST::net   (静态库)   <- BUILD: cc_library(name = "net")
# ---------------------------------------------------------------------------
add_library(verible__verilog__CST__net STATIC "net.cc")
target_sources(verible__verilog__CST__net PRIVATE "net.h")

add_library(verible::verilog::CST::net ALIAS verible__verilog__CST__net)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__CST__net PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__CST__net PUBLIC
    verible::verilog::CST::identifier
    verible::verilog::CST::verilog-matchers
    verible::verilog::CST::verilog-nonterminals
    verible::common::analysis::syntax-tree-search
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::inner-match-handlers
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::token-info
    verible::common::text::tree-utils
)

# ---------------------------------------------------------------------------
# verible::verilog::CST::data   (静态库)   <- BUILD: cc_library(name = "data")
# ---------------------------------------------------------------------------
add_library(verible__verilog__CST__data STATIC "data.cc")
target_sources(verible__verilog__CST__data PRIVATE "data.h")

add_library(verible::verilog::CST::data ALIAS verible__verilog__CST__data)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__CST__data PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__CST__data PUBLIC
    verible::verilog::CST::identifier
    verible::verilog::CST::verilog-matchers
    verible::verilog::CST::verilog-nonterminals
    verible::common::analysis::syntax-tree-search
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::inner-match-handlers
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::token-info
    verible::common::text::tree-utils
)

# ---------------------------------------------------------------------------
# verible::verilog::CST::package   (静态库)   <- BUILD: cc_library(name = "package")
# ---------------------------------------------------------------------------
add_library(verible__verilog__CST__package STATIC "package.cc")
target_sources(verible__verilog__CST__package PRIVATE "package.h")

add_library(verible::verilog::CST::package ALIAS verible__verilog__CST__package)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__CST__package PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__CST__package PUBLIC
    verible::verilog::CST::verilog-matchers
    verible::verilog::CST::verilog-nonterminals
    verible::common::analysis::syntax-tree-search
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::token-info
    verible::common::text::tree-utils
    verible::verilog::parser::verilog-token-enum
)

# ---------------------------------------------------------------------------
# verible::verilog::CST::parameters   (静态库)   <- BUILD: cc_library(name = "parameters")
# ---------------------------------------------------------------------------
add_library(verible__verilog__CST__parameters STATIC "parameters.cc")
target_sources(verible__verilog__CST__parameters PRIVATE "parameters.h")

add_library(verible::verilog::CST::parameters ALIAS verible__verilog__CST__parameters)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__CST__parameters PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__CST__parameters PUBLIC
    verible::verilog::CST::identifier
    verible::verilog::CST::verilog-matchers
    verible::verilog::CST::verilog-nonterminals
    verible::common::analysis::syntax-tree-search
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::token-info
    verible::common::text::tree-utils
    verible::common::util::casts
    verible::common::util::logging
    verible::verilog::parser::verilog-token-enum
    absl::die_if_null
)

# ---------------------------------------------------------------------------
# verible::verilog::CST::port   (静态库)   <- BUILD: cc_library(name = "port")
# ---------------------------------------------------------------------------
add_library(verible__verilog__CST__port STATIC "port.cc")
target_sources(verible__verilog__CST__port PRIVATE "port.h")

add_library(verible::verilog::CST::port ALIAS verible__verilog__CST__port)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__CST__port PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__CST__port PUBLIC
    verible::verilog::CST::identifier
    verible::verilog::CST::verilog-matchers
    verible::verilog::CST::verilog-nonterminals
    verible::common::analysis::syntax-tree-search
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::tree-utils
    verible::common::util::logging
)

# ---------------------------------------------------------------------------
# verible::verilog::CST::seq-block   (静态库)   <- BUILD: cc_library(name = "seq-block")
# ---------------------------------------------------------------------------
add_library(verible__verilog__CST__seq-block STATIC "seq-block.cc")
target_sources(verible__verilog__CST__seq-block PRIVATE "seq-block.h")

add_library(verible::verilog::CST::seq-block ALIAS verible__verilog__CST__seq-block)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__CST__seq-block PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__CST__seq-block PUBLIC
    verible::verilog::CST::verilog-nonterminals
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::token-info
    verible::common::text::tree-utils
    verible::common::util::logging
    verible::verilog::parser::verilog-token-enum
    absl::die_if_null
)

# ---------------------------------------------------------------------------
# verible::verilog::CST::statement   (静态库)   <- BUILD: cc_library(name = "statement")
# ---------------------------------------------------------------------------
add_library(verible__verilog__CST__statement STATIC "statement.cc")
target_sources(verible__verilog__CST__statement PRIVATE "statement.h")

add_library(verible::verilog::CST::statement ALIAS verible__verilog__CST__statement)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__CST__statement PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__CST__statement PUBLIC
    verible::verilog::CST::declaration
    verible::verilog::CST::identifier
    verible::verilog::CST::type
    verible::verilog::CST::verilog-matchers
    verible::verilog::CST::verilog-nonterminals
    verible::common::analysis::syntax-tree-search
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::tree-utils
    verible::common::text::visitors
    verible::common::util::casts
    verible::verilog::parser::verilog-token-enum
)

# ---------------------------------------------------------------------------
# verible::verilog::CST::tasks   (静态库)   <- BUILD: cc_library(name = "tasks")
# ---------------------------------------------------------------------------
add_library(verible__verilog__CST__tasks STATIC "tasks.cc")
target_sources(verible__verilog__CST__tasks PRIVATE "tasks.h")

add_library(verible::verilog::CST::tasks ALIAS verible__verilog__CST__tasks)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__CST__tasks PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__CST__tasks PUBLIC
    verible::verilog::CST::identifier
    verible::verilog::CST::verilog-matchers
    verible::verilog::CST::verilog-nonterminals
    verible::common::analysis::syntax-tree-search
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::tree-utils
)

# ---------------------------------------------------------------------------
# verible::verilog::CST::type   (静态库)   <- BUILD: cc_library(name = "type")
# ---------------------------------------------------------------------------
add_library(verible__verilog__CST__type STATIC "type.cc")
target_sources(verible__verilog__CST__type PRIVATE "type.h")

add_library(verible::verilog::CST::type ALIAS verible__verilog__CST__type)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__CST__type PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__CST__type PUBLIC
    verible::verilog::CST::identifier
    verible::verilog::CST::verilog-matchers
    verible::verilog::CST::verilog-nonterminals
    verible::common::analysis::syntax-tree-search
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::symbol-ptr
    verible::common::text::tree-utils
    verible::common::util::logging
    verible::verilog::parser::verilog-token-enum
    absl::die_if_null
)

# ---------------------------------------------------------------------------
# verible::verilog::CST::context-functions   (纯头文件库)   <- BUILD: cc_library(name = "context-functions")
# ---------------------------------------------------------------------------
add_library(verible__verilog__CST__context-functions INTERFACE)
target_sources(verible__verilog__CST__context-functions INTERFACE "context-functions.h")

add_library(verible::verilog::CST::context-functions ALIAS verible__verilog__CST__context-functions)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__CST__context-functions INTERFACE verible_common_opts)

target_link_libraries(verible__verilog__CST__context-functions INTERFACE
    verible::verilog::CST::verilog-nonterminals
    verible::common::text::syntax-tree-context
)

# ---------------------------------------------------------------------------
# verible::verilog::CST::numbers   (静态库)   <- BUILD: cc_library(name = "numbers")
# ---------------------------------------------------------------------------
add_library(verible__verilog__CST__numbers STATIC "numbers.cc")
target_sources(verible__verilog__CST__numbers PRIVATE "numbers.h")

add_library(verible::verilog::CST::numbers ALIAS verible__verilog__CST__numbers)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__CST__numbers PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__CST__numbers PUBLIC
    verible::common::util::logging
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::verilog::CST::verilog-tree-print   (静态库)   <- BUILD: cc_library(name = "verilog-tree-print")
# ---------------------------------------------------------------------------
add_library(verible__verilog__CST__verilog-tree-print STATIC "verilog-tree-print.cc")
target_sources(verible__verilog__CST__verilog-tree-print PRIVATE "verilog-tree-print.h")

add_library(verible::verilog::CST::verilog-tree-print ALIAS verible__verilog__CST__verilog-tree-print)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__CST__verilog-tree-print PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__CST__verilog-tree-print PUBLIC
    verible::verilog::CST::verilog-nonterminals
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::token-info
    verible::common::text::tree-utils
    verible::common::util::value-saver
    verible::verilog::parser::verilog-parser
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::verilog::CST::verilog-tree-json   (静态库)   <- BUILD: cc_library(name = "verilog-tree-json")
# ---------------------------------------------------------------------------
add_library(verible__verilog__CST__verilog-tree-json STATIC "verilog-tree-json.cc")
target_sources(verible__verilog__CST__verilog-tree-json PRIVATE "verilog-tree-json.h")

add_library(verible::verilog::CST::verilog-tree-json ALIAS verible__verilog__CST__verilog-tree-json)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__CST__verilog-tree-json PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__CST__verilog-tree-json PUBLIC
    verible::verilog::CST::verilog-nonterminals
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::token-info
    verible::common::text::token-info-json
    verible::common::text::visitors
    verible::common::util::value-saver
    verible::verilog::parser::verilog-token
    verible::verilog::parser::verilog-token-classifications
    verible::verilog::parser::verilog-token-enum
    nlohmann_json::nlohmann_json
)
