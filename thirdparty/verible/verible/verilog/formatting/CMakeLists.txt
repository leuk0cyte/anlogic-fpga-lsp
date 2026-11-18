# =============================================================================
#  GENERATED FILE - DO NOT EDIT
# =============================================================================
#  由 tools/gen_verible_cmake.py 从同目录的 Bazel BUILD 自动转译而来。
#  手工修改会在下次运行生成器时被覆盖；要改行为请改生成器本身。
#
#  源包: //verible/verilog/formatting
#
#  目录里也没有 BUILD 生成物时，本文件只声明库，不在这里编测试
#  （阶段 0 不编 Verible 自带的 225 个 cc_test）。
# =============================================================================
# ---------------------------------------------------------------------------
# 本包有 14 个 cc_test —— 按 CLAUDE 约定不编译 Verible 自带测试；
# 需要时用 `--with-tests` 重新生成。
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# verible::verilog::formatting::align   (静态库)   <- BUILD: cc_library(name = "align")
# ---------------------------------------------------------------------------
add_library(verible__verilog__formatting__align STATIC "align.cc")
target_sources(verible__verilog__formatting__align PRIVATE "align.h")

add_library(verible::verilog::formatting::align ALIAS verible__verilog__formatting__align)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__formatting__align PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__formatting__align PUBLIC
    verible::verilog::formatting::format-style
    verible::common::formatting::align
    verible::common::formatting::format-token
    verible::common::formatting::token-partition-tree
    verible::common::formatting::unwrapped-line
    verible::common::strings::position
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::token-info
    verible::common::text::tree-context-visitor
    verible::common::text::tree-utils
    verible::common::util::logging
    verible::common::util::value-saver
    verible::verilog::CST::context-functions
    verible::verilog::CST::declaration
    verible::verilog::CST::verilog-nonterminals
    verible::verilog::parser::verilog-token-classifications
    verible::verilog::parser::verilog-token-enum
    absl::die_if_null
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::verilog::formatting::tree-unwrapper   (静态库)   <- BUILD: cc_library(name = "tree-unwrapper")
# ---------------------------------------------------------------------------
add_library(verible__verilog__formatting__tree-unwrapper STATIC "tree-unwrapper.cc")
target_sources(verible__verilog__formatting__tree-unwrapper PRIVATE "tree-unwrapper.h")

add_library(verible::verilog::formatting::tree-unwrapper ALIAS verible__verilog__formatting__tree-unwrapper)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__formatting__tree-unwrapper PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__formatting__tree-unwrapper PUBLIC
    verible::verilog::formatting::format-style
    verible::verilog::formatting::verilog-token
    verible::common::formatting::format-token
    verible::common::formatting::token-partition-tree
    verible::common::formatting::tree-unwrapper
    verible::common::formatting::unwrapped-line
    verible::common::strings::display-utils
    verible::common::strings::range
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::constants
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::text-structure
    verible::common::text::token-info
    verible::common::text::token-stream-view
    verible::common::text::tree-utils
    verible::common::util::container-iterator-range
    verible::common::util::enum-flags
    verible::common::util::iterator-range
    verible::common::util::logging
    verible::common::util::tree-operations
    verible::verilog::CST::declaration
    verible::verilog::CST::macro
    verible::verilog::CST::statement
    verible::verilog::CST::verilog-nonterminals
    verible::verilog::parser::verilog-parser
    verible::verilog::parser::verilog-token-classifications
    verible::verilog::parser::verilog-token-enum
    absl::core_headers
    absl::die_if_null
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::verilog::formatting::verilog-token   (静态库)   <- BUILD: cc_library(name = "verilog-token")
# ---------------------------------------------------------------------------
add_library(verible__verilog__formatting__verilog-token STATIC "verilog-token.cc")
target_sources(verible__verilog__formatting__verilog-token PRIVATE "verilog-token.h")

add_library(verible::verilog::formatting::verilog-token ALIAS verible__verilog__formatting__verilog-token)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__formatting__verilog-token PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__formatting__verilog-token PUBLIC
    verible::common::util::container-util
    verible::verilog::parser::verilog-token-enum
    absl::node_hash_map
)

# ---------------------------------------------------------------------------
# verible::verilog::formatting::formatter   (静态库)   <- BUILD: cc_library(name = "formatter")
# ---------------------------------------------------------------------------
add_library(verible__verilog__formatting__formatter STATIC "formatter.cc")
target_sources(verible__verilog__formatting__formatter PRIVATE "formatter.h")

add_library(verible::verilog::formatting::formatter ALIAS verible__verilog__formatting__formatter)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__formatting__formatter PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__formatting__formatter PUBLIC
    verible::verilog::formatting::align
    verible::verilog::formatting::comment-controls
    verible::verilog::formatting::format-style
    verible::verilog::formatting::token-annotator
    verible::verilog::formatting::tree-unwrapper
    verible::common::formatting::basic-format-style
    verible::common::formatting::format-token
    verible::common::formatting::layout-optimizer
    verible::common::formatting::line-wrap-searcher
    verible::common::formatting::token-partition-tree
    verible::common::formatting::unwrapped-line
    verible::common::formatting::verification
    verible::common::strings::diff
    verible::common::strings::line-column-map
    verible::common::strings::position
    verible::common::strings::range
    verible::common::text::line-terminator
    verible::common::text::symbol
    verible::common::text::text-structure
    verible::common::text::token-info
    verible::common::text::tree-utils
    verible::common::util::expandable-tree-view
    verible::common::util::interval
    verible::common::util::interval-set
    verible::common::util::logging
    verible::common::util::spacer
    verible::common::util::tree-operations
    verible::common::util::vector-tree
    verible::common::util::vector-tree-iterators
    verible::verilog::CST::declaration
    verible::verilog::CST::verilog-nonterminals
    verible::verilog::analysis::verilog-analyzer
    verible::verilog::analysis::verilog-equivalence
    verible::verilog::parser::verilog-token-enum
    verible::verilog::preprocessor::verilog-preprocess
    absl::die_if_null
    absl::status
    absl::statusor
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::verilog::formatting::formatter-test-utils   (纯头文件库)   <- BUILD: cc_library(name = "formatter-test-utils")
# ---------------------------------------------------------------------------
add_library(verible__verilog__formatting__formatter-test-utils INTERFACE)
target_sources(verible__verilog__formatting__formatter-test-utils INTERFACE "formatter-test-utils.h")

add_library(verible::verilog::formatting::formatter-test-utils ALIAS verible__verilog__formatting__formatter-test-utils)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__formatting__formatter-test-utils INTERFACE verible_common_opts)

target_link_libraries(verible__verilog__formatting__formatter-test-utils INTERFACE
    verible::verilog::formatting::format-style
    verible::verilog::formatting::formatter
    verible::common::util::logging
    GTest::gtest
)

# BUILD: testonly = 1（本阶段不编译测试，库照常生成以便被引用）

# ---------------------------------------------------------------------------
# verible::verilog::formatting::comment-controls   (静态库)   <- BUILD: cc_library(name = "comment-controls")
# ---------------------------------------------------------------------------
add_library(verible__verilog__formatting__comment-controls STATIC "comment-controls.cc")
target_sources(verible__verilog__formatting__comment-controls PRIVATE "comment-controls.h")

add_library(verible::verilog::formatting::comment-controls ALIAS verible__verilog__formatting__comment-controls)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__formatting__comment-controls PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__formatting__comment-controls PUBLIC
    verible::common::strings::comment-utils
    verible::common::strings::display-utils
    verible::common::strings::line-column-map
    verible::common::strings::position
    verible::common::text::line-terminator
    verible::common::text::token-info
    verible::common::text::token-stream-view
    verible::common::util::logging
    verible::common::util::range
    verible::verilog::parser::verilog-parser
    verible::verilog::parser::verilog-token-classifications
    verible::verilog::parser::verilog-token-enum
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::verilog::formatting::format-style   (纯头文件库)   <- BUILD: cc_library(name = "format-style")
# ---------------------------------------------------------------------------
add_library(verible__verilog__formatting__format-style INTERFACE)
target_sources(verible__verilog__formatting__format-style INTERFACE "format-style.h")

add_library(verible::verilog::formatting::format-style ALIAS verible__verilog__formatting__format-style)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__formatting__format-style INTERFACE verible_common_opts)

target_link_libraries(verible__verilog__formatting__format-style INTERFACE
    verible::common::formatting::align
    verible::common::formatting::basic-format-style
)

# ---------------------------------------------------------------------------
# verible::verilog::formatting::format-style-init   (静态库)   <- BUILD: cc_library(name = "format-style-init")
# ---------------------------------------------------------------------------
add_library(verible__verilog__formatting__format-style-init STATIC "format-style-init.cc")
target_sources(verible__verilog__formatting__format-style-init PRIVATE "format-style-init.h")

add_library(verible::verilog::formatting::format-style-init ALIAS verible__verilog__formatting__format-style-init)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__formatting__format-style-init PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__formatting__format-style-init PUBLIC
    verible::verilog::formatting::format-style
    verible::common::formatting::align
    verible::common::formatting::basic-format-style
    verible::common::formatting::basic-format-style-init
    verible::common::util::enum-flags
    absl::flags
)

# ---------------------------------------------------------------------------
# verible::verilog::formatting::token-annotator   (静态库)   <- BUILD: cc_library(name = "token-annotator")
# ---------------------------------------------------------------------------
add_library(verible__verilog__formatting__token-annotator STATIC "token-annotator.cc")
target_sources(verible__verilog__formatting__token-annotator PRIVATE "token-annotator.h")

add_library(verible::verilog::formatting::token-annotator ALIAS verible__verilog__formatting__token-annotator)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__formatting__token-annotator PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__formatting__token-annotator PUBLIC
    verible::verilog::formatting::format-style
    verible::verilog::formatting::verilog-token
    verible::common::formatting::format-token
    verible::common::formatting::tree-annotator
    verible::common::strings::range
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::text-structure
    verible::common::text::token-info
    verible::common::util::logging
    verible::common::util::with-reason
    verible::verilog::CST::verilog-nonterminals
    verible::verilog::parser::verilog-parser
    verible::verilog::parser::verilog-token-classifications
    verible::verilog::parser::verilog-token-enum
)
