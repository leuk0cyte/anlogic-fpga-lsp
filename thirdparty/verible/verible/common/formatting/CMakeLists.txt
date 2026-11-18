# =============================================================================
#  GENERATED FILE - DO NOT EDIT
# =============================================================================
#  由 tools/gen_verible_cmake.py 从同目录的 Bazel BUILD 自动转译而来。
#  手工修改会在下次运行生成器时被覆盖；要改行为请改生成器本身。
#
#  源包: //verible/common/formatting
#
#  目录里也没有 BUILD 生成物时，本文件只声明库，不在这里编测试
#  （阶段 0 不编 Verible 自带的 225 个 cc_test）。
# =============================================================================
# ---------------------------------------------------------------------------
# 本包有 11 个 cc_test —— 按 CLAUDE 约定不编译 Verible 自带测试；
# 需要时用 `--with-tests` 重新生成。
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# verible::common::formatting::align   (静态库)   <- BUILD: cc_library(name = "align")
# ---------------------------------------------------------------------------
add_library(verible__common__formatting__align STATIC "align.cc")
target_sources(verible__common__formatting__align PRIVATE "align.h")

add_library(verible::common::formatting::align ALIAS verible__common__formatting__align)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__formatting__align PRIVATE verible_common_opts)

target_link_libraries(verible__common__formatting__align PUBLIC
    verible::common::formatting::format-token
    verible::common::formatting::token-partition-tree
    verible::common::formatting::unwrapped-line
    verible::common::strings::display-utils
    verible::common::strings::position
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::token-info
    verible::common::text::tree-context-visitor
    verible::common::text::tree-utils
    verible::common::util::container-iterator-range
    verible::common::util::enum-flags
    verible::common::util::iterator-adaptors
    verible::common::util::iterator-range
    verible::common::util::logging
    verible::common::util::range
    verible::common::util::tree-operations
    verible::common::util::vector-tree
    verible::common::util::vector-tree-iterators
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::common::formatting::basic-format-style   (静态库)   <- BUILD: cc_library(name = "basic-format-style")
# ---------------------------------------------------------------------------
add_library(verible__common__formatting__basic-format-style STATIC "basic-format-style.cc")
target_sources(verible__common__formatting__basic-format-style PRIVATE "basic-format-style.h")

add_library(verible::common::formatting::basic-format-style ALIAS verible__common__formatting__basic-format-style)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__formatting__basic-format-style PRIVATE verible_common_opts)

target_link_libraries(verible__common__formatting__basic-format-style PUBLIC
    verible::common::util::enum-flags
)

# ---------------------------------------------------------------------------
# verible::common::formatting::basic-format-style-init   (静态库)   <- BUILD: cc_library(name = "basic-format-style-init")
# ---------------------------------------------------------------------------
add_library(verible__common__formatting__basic-format-style-init STATIC "basic-format-style-init.cc")
target_sources(verible__common__formatting__basic-format-style-init PRIVATE "basic-format-style-init.h")

add_library(verible::common::formatting::basic-format-style-init ALIAS verible__common__formatting__basic-format-style-init)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__formatting__basic-format-style-init PRIVATE verible_common_opts)

target_link_libraries(verible__common__formatting__basic-format-style-init PUBLIC
    verible::common::formatting::basic-format-style
    absl::flags
)

# ---------------------------------------------------------------------------
# verible::common::formatting::format-token   (静态库)   <- BUILD: cc_library(name = "format-token")
# ---------------------------------------------------------------------------
add_library(verible__common__formatting__format-token STATIC "format-token.cc")
target_sources(verible__common__formatting__format-token PRIVATE "format-token.h")

add_library(verible::common::formatting::format-token ALIAS verible__common__formatting__format-token)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__formatting__format-token PRIVATE verible_common_opts)

target_link_libraries(verible__common__formatting__format-token PUBLIC
    verible::common::strings::display-utils
    verible::common::strings::position
    verible::common::strings::range
    verible::common::text::concrete-syntax-leaf
    verible::common::text::token-info
    verible::common::util::container-iterator-range
    verible::common::util::interval
    verible::common::util::iterator-range
    verible::common::util::logging
    verible::common::util::range
    verible::common::util::spacer
    absl::core_headers
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::common::formatting::layout-optimizer   (静态库)   <- BUILD: cc_library(name = "layout-optimizer")
# ---------------------------------------------------------------------------
add_library(verible__common__formatting__layout-optimizer STATIC "layout-optimizer.cc" "layout-optimizer-internal.h")
target_sources(verible__common__formatting__layout-optimizer PRIVATE "layout-optimizer.h")

add_library(verible::common::formatting::layout-optimizer ALIAS verible__common__formatting__layout-optimizer)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__formatting__layout-optimizer PRIVATE verible_common_opts)

target_link_libraries(verible__common__formatting__layout-optimizer PUBLIC
    verible::common::formatting::basic-format-style
    verible::common::formatting::format-token
    verible::common::formatting::token-partition-tree
    verible::common::formatting::unwrapped-line
    verible::common::util::container-iterator-range
    verible::common::util::iterator-range
    verible::common::util::logging
    verible::common::util::tree-operations
    verible::common::util::value-saver
    verible::common::util::vector-tree
    absl::fixed_array
    absl::log
    absl::vlog_is_on
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::common::formatting::token-partition-tree   (静态库)   <- BUILD: cc_library(name = "token-partition-tree")
# ---------------------------------------------------------------------------
add_library(verible__common__formatting__token-partition-tree STATIC "token-partition-tree.cc")
target_sources(verible__common__formatting__token-partition-tree PRIVATE "token-partition-tree.h")

add_library(verible::common::formatting::token-partition-tree ALIAS verible__common__formatting__token-partition-tree)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__formatting__token-partition-tree PRIVATE verible_common_opts)

target_link_libraries(verible__common__formatting__token-partition-tree PUBLIC
    verible::common::formatting::basic-format-style
    verible::common::formatting::format-token
    verible::common::formatting::line-wrap-searcher
    verible::common::formatting::unwrapped-line
    verible::common::strings::display-utils
    verible::common::strings::position
    verible::common::strings::range
    verible::common::util::algorithm
    verible::common::util::container-iterator-range
    verible::common::util::iterator-adaptors
    verible::common::util::iterator-range
    verible::common::util::logging
    verible::common::util::spacer
    verible::common::util::top-n
    verible::common::util::tree-operations
    verible::common::util::vector-tree
    absl::die_if_null
)

# ---------------------------------------------------------------------------
# verible::common::formatting::token-partition-tree-test-utils   (静态库)   <- BUILD: cc_library(name = "token-partition-tree-test-utils")
# ---------------------------------------------------------------------------
add_library(verible__common__formatting__token-partition-tree-test-utils STATIC "token-partition-tree-test-utils.cc")
target_sources(verible__common__formatting__token-partition-tree-test-utils PRIVATE "token-partition-tree-test-utils.h")

add_library(verible::common::formatting::token-partition-tree-test-utils ALIAS verible__common__formatting__token-partition-tree-test-utils)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__formatting__token-partition-tree-test-utils PRIVATE verible_common_opts)

target_link_libraries(verible__common__formatting__token-partition-tree-test-utils PUBLIC
    verible::common::formatting::format-token
    verible::common::formatting::token-partition-tree
    verible::common::formatting::unwrapped-line
    verible::common::util::logging
    verible::common::util::tree-operations
    GTest::gtest
    GTest::gtest_main
)

# BUILD: testonly = 1（本阶段不编译测试，库照常生成以便被引用）

# ---------------------------------------------------------------------------
# verible::common::formatting::unwrapped-line   (静态库)   <- BUILD: cc_library(name = "unwrapped-line")
# ---------------------------------------------------------------------------
add_library(verible__common__formatting__unwrapped-line STATIC "unwrapped-line.cc")
target_sources(verible__common__formatting__unwrapped-line PRIVATE "unwrapped-line.h")

add_library(verible::common::formatting::unwrapped-line ALIAS verible__common__formatting__unwrapped-line)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__formatting__unwrapped-line PRIVATE verible_common_opts)

target_link_libraries(verible__common__formatting__unwrapped-line PUBLIC
    verible::common::formatting::format-token
    verible::common::strings::display-utils
    verible::common::text::symbol
    verible::common::text::token-info
    verible::common::text::tree-utils
    verible::common::util::container-iterator-range
    verible::common::util::iterator-range
    verible::common::util::logging
    verible::common::util::spacer
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::common::formatting::unwrapped-line-test-utils   (静态库)   <- BUILD: cc_library(name = "unwrapped-line-test-utils")
# ---------------------------------------------------------------------------
add_library(verible__common__formatting__unwrapped-line-test-utils STATIC "unwrapped-line-test-utils.cc")
target_sources(verible__common__formatting__unwrapped-line-test-utils PRIVATE "unwrapped-line-test-utils.h")

add_library(verible::common::formatting::unwrapped-line-test-utils ALIAS verible__common__formatting__unwrapped-line-test-utils)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__formatting__unwrapped-line-test-utils PRIVATE verible_common_opts)

target_link_libraries(verible__common__formatting__unwrapped-line-test-utils PUBLIC
    verible::common::formatting::format-token
    verible::common::formatting::unwrapped-line
    verible::common::text::constants
    verible::common::text::token-info
)

# BUILD: testonly = 1（本阶段不编译测试，库照常生成以便被引用）

# ---------------------------------------------------------------------------
# verible::common::formatting::tree-annotator   (静态库)   <- BUILD: cc_library(name = "tree-annotator")
# ---------------------------------------------------------------------------
add_library(verible__common__formatting__tree-annotator STATIC "tree-annotator.cc")
target_sources(verible__common__formatting__tree-annotator PRIVATE "tree-annotator.h")

add_library(verible::common::formatting::tree-annotator ALIAS verible__common__formatting__tree-annotator)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__formatting__tree-annotator PRIVATE verible_common_opts)

target_link_libraries(verible__common__formatting__tree-annotator PUBLIC
    verible::common::formatting::format-token
    verible::common::text::concrete-syntax-leaf
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::token-info
    verible::common::text::tree-context-visitor
)

# ---------------------------------------------------------------------------
# verible::common::formatting::tree-unwrapper   (静态库)   <- BUILD: cc_library(name = "tree-unwrapper")
# ---------------------------------------------------------------------------
add_library(verible__common__formatting__tree-unwrapper STATIC "tree-unwrapper.cc")
target_sources(verible__common__formatting__tree-unwrapper PRIVATE "tree-unwrapper.h")

add_library(verible::common::formatting::tree-unwrapper ALIAS verible__common__formatting__tree-unwrapper)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__formatting__tree-unwrapper PRIVATE verible_common_opts)

target_link_libraries(verible__common__formatting__tree-unwrapper PUBLIC
    verible::common::formatting::format-token
    verible::common::formatting::token-partition-tree
    verible::common::formatting::unwrapped-line
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::text-structure
    verible::common::text::token-info
    verible::common::text::token-stream-view
    verible::common::text::tree-context-visitor
    verible::common::util::logging
    verible::common::util::tree-operations
    verible::common::util::value-saver
    verible::common::util::vector-tree
    absl::die_if_null
)

# ---------------------------------------------------------------------------
# verible::common::formatting::state-node   (静态库)   <- BUILD: cc_library(name = "state-node")
# ---------------------------------------------------------------------------
add_library(verible__common__formatting__state-node STATIC "state-node.cc")
target_sources(verible__common__formatting__state-node PRIVATE "state-node.h")

add_library(verible::common::formatting::state-node ALIAS verible__common__formatting__state-node)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__formatting__state-node PRIVATE verible_common_opts)

target_link_libraries(verible__common__formatting__state-node PUBLIC
    verible::common::formatting::basic-format-style
    verible::common::formatting::format-token
    verible::common::formatting::unwrapped-line
    verible::common::strings::position
    verible::common::text::token-info
    verible::common::util::container-iterator-range
    verible::common::util::iterator-adaptors
    verible::common::util::iterator-range
    verible::common::util::logging
    absl::die_if_null
)

# ---------------------------------------------------------------------------
# verible::common::formatting::line-wrap-searcher   (静态库)   <- BUILD: cc_library(name = "line-wrap-searcher")
# ---------------------------------------------------------------------------
add_library(verible__common__formatting__line-wrap-searcher STATIC "line-wrap-searcher.cc")
target_sources(verible__common__formatting__line-wrap-searcher PRIVATE "line-wrap-searcher.h")

add_library(verible::common::formatting::line-wrap-searcher ALIAS verible__common__formatting__line-wrap-searcher)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__formatting__line-wrap-searcher PRIVATE verible_common_opts)

target_link_libraries(verible__common__formatting__line-wrap-searcher PUBLIC
    verible::common::formatting::basic-format-style
    verible::common::formatting::format-token
    verible::common::formatting::state-node
    verible::common::formatting::unwrapped-line
    verible::common::text::token-info
    verible::common::util::logging
    verible::common::util::spacer
)

# ---------------------------------------------------------------------------
# verible::common::formatting::verification   (静态库)   <- BUILD: cc_library(name = "verification")
# ---------------------------------------------------------------------------
add_library(verible__common__formatting__verification STATIC "verification.cc")
target_sources(verible__common__formatting__verification PRIVATE "verification.h")

add_library(verible::common::formatting::verification ALIAS verible__common__formatting__verification)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__formatting__verification PRIVATE verible_common_opts)

target_link_libraries(verible__common__formatting__verification PUBLIC
    verible::common::strings::diff
    verible::common::strings::position
    absl::status
    absl::strings
)
