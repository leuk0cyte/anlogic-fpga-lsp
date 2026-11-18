# =============================================================================
#  GENERATED FILE - DO NOT EDIT
# =============================================================================
#  由 tools/gen_verible_cmake.py 从同目录的 Bazel BUILD 自动转译而来。
#  手工修改会在下次运行生成器时被覆盖；要改行为请改生成器本身。
#
#  源包: //verible/common/text
#
#  目录里也没有 BUILD 生成物时，本文件只声明库，不在这里编测试
#  （阶段 0 不编 Verible 自带的 225 个 cc_test）。
# =============================================================================
# ---------------------------------------------------------------------------
# 本包有 16 个 cc_test —— 按 CLAUDE 约定不编译 Verible 自带测试；
# 需要时用 `--with-tests` 重新生成。
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# verible::common::text::token-info   (静态库)   <- BUILD: cc_library(name = "token-info")
# ---------------------------------------------------------------------------
add_library(verible__common__text__token-info STATIC "token-info.cc")
target_sources(verible__common__text__token-info PRIVATE "token-info.h")

add_library(verible::common::text::token-info ALIAS verible__common__text__token-info)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__text__token-info PRIVATE verible_common_opts)

target_link_libraries(verible__common__text__token-info PUBLIC
    verible::common::text::constants
    verible::common::strings::rebase
    verible::common::util::iterator-range
    verible::common::util::logging
    verible::common::util::range
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::common::text::token-info-json   (静态库)   <- BUILD: cc_library(name = "token-info-json")
# ---------------------------------------------------------------------------
add_library(verible__common__text__token-info-json STATIC "token-info-json.cc")
target_sources(verible__common__text__token-info-json PRIVATE "token-info-json.h")

add_library(verible::common::text::token-info-json ALIAS verible__common__text__token-info-json)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__text__token-info-json PRIVATE verible_common_opts)

target_link_libraries(verible__common__text__token-info-json PUBLIC
    verible::common::text::token-info
    nlohmann_json::nlohmann_json
)

# ---------------------------------------------------------------------------
# verible::common::text::token-info-test-util   (静态库)   <- BUILD: cc_library(name = "token-info-test-util")
# ---------------------------------------------------------------------------
add_library(verible__common__text__token-info-test-util STATIC "token-info-test-util.cc")
target_sources(verible__common__text__token-info-test-util PRIVATE "token-info-test-util.h")

add_library(verible::common::text::token-info-test-util ALIAS verible__common__text__token-info-test-util)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__text__token-info-test-util PRIVATE verible_common_opts)

target_link_libraries(verible__common__text__token-info-test-util PUBLIC
    verible::common::text::token-info
    verible::common::util::logging
)

# BUILD: testonly = 1（本阶段不编译测试，库照常生成以便被引用）

# ---------------------------------------------------------------------------
# verible::common::text::line-terminator   (静态库)   <- BUILD: cc_library(name = "line-terminator")
# ---------------------------------------------------------------------------
add_library(verible__common__text__line-terminator STATIC "line-terminator.cc")
target_sources(verible__common__text__line-terminator PRIVATE "line-terminator.h")

add_library(verible::common::text::line-terminator ALIAS verible__common__text__line-terminator)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__text__line-terminator PRIVATE verible_common_opts)

# ---------------------------------------------------------------------------
# verible::common::text::token-stream-view   (静态库)   <- BUILD: cc_library(name = "token-stream-view")
# ---------------------------------------------------------------------------
add_library(verible__common__text__token-stream-view STATIC "token-stream-view.cc")
target_sources(verible__common__text__token-stream-view PRIVATE "token-stream-view.h")

add_library(verible::common::text::token-stream-view ALIAS verible__common__text__token-stream-view)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__text__token-stream-view PRIVATE verible_common_opts)

target_link_libraries(verible__common__text__token-stream-view PUBLIC
    verible::common::text::token-info
    verible::common::util::iterator-range
)

# ---------------------------------------------------------------------------
# verible::common::text::symbol-ptr   (纯头文件库)   <- BUILD: cc_library(name = "symbol-ptr")
# ---------------------------------------------------------------------------
add_library(verible__common__text__symbol-ptr INTERFACE)
target_sources(verible__common__text__symbol-ptr INTERFACE "symbol-ptr.h")

add_library(verible::common::text::symbol-ptr ALIAS verible__common__text__symbol-ptr)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__text__symbol-ptr INTERFACE verible_common_opts)

# ---------------------------------------------------------------------------
# verible::common::text::symbol   (静态库)   <- BUILD: cc_library(name = "symbol")
# ---------------------------------------------------------------------------
add_library(verible__common__text__symbol STATIC "symbol.cc")
target_sources(verible__common__text__symbol PRIVATE "symbol.h")

add_library(verible::common::text::symbol ALIAS verible__common__text__symbol)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__text__symbol PRIVATE verible_common_opts)

target_link_libraries(verible__common__text__symbol PUBLIC
    verible::common::text::symbol-ptr
    verible::common::text::token-info
    verible::common::text::visitors
)

# ---------------------------------------------------------------------------
# verible::common::text::concrete-syntax-leaf   (静态库)   <- BUILD: cc_library(name = "concrete-syntax-leaf")
# ---------------------------------------------------------------------------
add_library(verible__common__text__concrete-syntax-leaf STATIC "concrete-syntax-leaf.cc")
target_sources(verible__common__text__concrete-syntax-leaf PRIVATE "concrete-syntax-leaf.h")

add_library(verible::common::text::concrete-syntax-leaf ALIAS verible__common__text__concrete-syntax-leaf)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__text__concrete-syntax-leaf PRIVATE verible_common_opts)

target_link_libraries(verible__common__text__concrete-syntax-leaf PUBLIC
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::token-info
    verible::common::text::tree-compare
    verible::common::text::visitors
    verible::common::util::casts
    verible::common::util::logging
    absl::die_if_null
)

# ---------------------------------------------------------------------------
# verible::common::text::concrete-syntax-tree   (静态库)   <- BUILD: cc_library(name = "concrete-syntax-tree")
# ---------------------------------------------------------------------------
add_library(verible__common__text__concrete-syntax-tree STATIC "concrete-syntax-tree.cc")
target_sources(verible__common__text__concrete-syntax-tree PRIVATE "concrete-syntax-tree.h")

add_library(verible::common::text::concrete-syntax-tree ALIAS verible__common__text__concrete-syntax-tree)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__text__concrete-syntax-tree PRIVATE verible_common_opts)

target_link_libraries(verible__common__text__concrete-syntax-tree PUBLIC
    verible::common::text::constants
    verible::common::text::symbol
    verible::common::text::symbol-ptr
    verible::common::text::tree-compare
    verible::common::text::visitors
    verible::common::util::casts
    verible::common::util::iterator-range
    verible::common::util::logging
    absl::core_headers
    absl::die_if_null
)

# ---------------------------------------------------------------------------
# verible::common::text::config-utils   (静态库)   <- BUILD: cc_library(name = "config-utils")
# ---------------------------------------------------------------------------
add_library(verible__common__text__config-utils STATIC "config-utils.cc")
target_sources(verible__common__text__config-utils PRIVATE "config-utils.h")

add_library(verible::common::text::config-utils ALIAS verible__common__text__config-utils)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__text__config-utils PRIVATE verible_common_opts)

target_link_libraries(verible__common__text__config-utils PUBLIC
    verible::common::util::logging
    absl::status
    absl::strings
    re2::re2
)

# ---------------------------------------------------------------------------
# verible::common::text::visitors   (纯头文件库)   <- BUILD: cc_library(name = "visitors")
# ---------------------------------------------------------------------------
add_library(verible__common__text__visitors INTERFACE)
target_sources(verible__common__text__visitors INTERFACE "visitors.h")

add_library(verible::common::text::visitors ALIAS verible__common__text__visitors)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__text__visitors INTERFACE verible_common_opts)

target_link_libraries(verible__common__text__visitors INTERFACE
    verible::common::text::symbol-ptr
)

# ---------------------------------------------------------------------------
# verible::common::text::syntax-tree-context   (纯头文件库)   <- BUILD: cc_library(name = "syntax-tree-context")
# ---------------------------------------------------------------------------
add_library(verible__common__text__syntax-tree-context INTERFACE)
target_sources(verible__common__text__syntax-tree-context INTERFACE "syntax-tree-context.h")

add_library(verible::common::text::syntax-tree-context ALIAS verible__common__text__syntax-tree-context)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__text__syntax-tree-context INTERFACE verible_common_opts)

target_link_libraries(verible__common__text__syntax-tree-context INTERFACE
    verible::common::text::concrete-syntax-tree
    verible::common::util::auto-pop-stack
    verible::common::util::iterator-adaptors
    absl::die_if_null
)

# ---------------------------------------------------------------------------
# verible::common::text::tree-compare   (静态库)   <- BUILD: cc_library(name = "tree-compare")
# ---------------------------------------------------------------------------
add_library(verible__common__text__tree-compare STATIC "tree-compare.cc")
target_sources(verible__common__text__tree-compare PRIVATE "tree-compare.h")

add_library(verible::common::text::tree-compare ALIAS verible__common__text__tree-compare)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__text__tree-compare PRIVATE verible_common_opts)

target_link_libraries(verible__common__text__tree-compare PUBLIC
    verible::common::text::symbol
    verible::common::text::token-info
)

# ---------------------------------------------------------------------------
# verible::common::text::tree-context-visitor   (静态库)   <- BUILD: cc_library(name = "tree-context-visitor")
# ---------------------------------------------------------------------------
add_library(verible__common__text__tree-context-visitor STATIC "tree-context-visitor.cc")
target_sources(verible__common__text__tree-context-visitor PRIVATE "tree-context-visitor.h")

add_library(verible::common::text::tree-context-visitor ALIAS verible__common__text__tree-context-visitor)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__text__tree-context-visitor PRIVATE verible_common_opts)

target_link_libraries(verible__common__text__tree-context-visitor PUBLIC
    verible::common::text::concrete-syntax-tree
    verible::common::text::syntax-tree-context
    verible::common::text::visitors
    verible::common::strings::display-utils
    verible::common::util::logging
)

# ---------------------------------------------------------------------------
# verible::common::text::tree-utils   (静态库)   <- BUILD: cc_library(name = "tree-utils")
# ---------------------------------------------------------------------------
add_library(verible__common__text__tree-utils STATIC "tree-utils.cc")
target_sources(verible__common__text__tree-utils PRIVATE "tree-utils.h")

add_library(verible::common::text::tree-utils ALIAS verible__common__text__tree-utils)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__text__tree-utils PRIVATE verible_common_opts)

target_link_libraries(verible__common__text__tree-utils PUBLIC
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::token-info
    verible::common::text::visitors
    verible::common::util::casts
    verible::common::util::iterator-adaptors
    verible::common::util::logging
    verible::common::util::spacer
    verible::common::util::type-traits
    verible::common::util::value-saver
    absl::check
    absl::die_if_null
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::common::text::tree-builder-test-util   (静态库)   <- BUILD: cc_library(name = "tree-builder-test-util")
# ---------------------------------------------------------------------------
add_library(verible__common__text__tree-builder-test-util STATIC "tree-builder-test-util.cc")
target_sources(verible__common__text__tree-builder-test-util PRIVATE "tree-builder-test-util.h")

add_library(verible::common::text::tree-builder-test-util ALIAS verible__common__text__tree-builder-test-util)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__text__tree-builder-test-util PRIVATE verible_common_opts)

target_link_libraries(verible__common__text__tree-builder-test-util PUBLIC
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::tree-utils
    verible::common::util::logging
    absl::die_if_null
)

# BUILD: testonly = 1（本阶段不编译测试，库照常生成以便被引用）

# ---------------------------------------------------------------------------
# verible::common::text::text-structure   (静态库)   <- BUILD: cc_library(name = "text-structure")
# ---------------------------------------------------------------------------
add_library(verible__common__text__text-structure STATIC "text-structure.cc")
target_sources(verible__common__text__text-structure PRIVATE "text-structure.h")

add_library(verible::common::text::text-structure ALIAS verible__common__text__text-structure)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__text__text-structure PRIVATE verible_common_opts)

target_link_libraries(verible__common__text__text-structure PUBLIC
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::token-info
    verible::common::text::token-stream-view
    verible::common::text::tree-utils
    verible::common::strings::line-column-map
    verible::common::strings::mem-block
    verible::common::util::iterator-range
    verible::common::util::logging
    verible::common::util::range
    verible::common::util::status-macros
    absl::die_if_null
    absl::status
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::common::text::text-structure-test-utils   (静态库)   <- BUILD: cc_library(name = "text-structure-test-utils")
# ---------------------------------------------------------------------------
add_library(verible__common__text__text-structure-test-utils STATIC "text-structure-test-utils.cc")
target_sources(verible__common__text__text-structure-test-utils PRIVATE "text-structure-test-utils.h")

add_library(verible::common::text::text-structure-test-utils ALIAS verible__common__text__text-structure-test-utils)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__text__text-structure-test-utils PRIVATE verible_common_opts)

target_link_libraries(verible__common__text__text-structure-test-utils PUBLIC
    verible::common::text::concrete-syntax-tree
    verible::common::text::text-structure
    verible::common::text::token-info
    verible::common::text::token-stream-view
    verible::common::text::tree-builder-test-util
    verible::common::util::logging
    absl::strings
)

# BUILD: testonly = 1（本阶段不编译测试，库照常生成以便被引用）

# ---------------------------------------------------------------------------
# verible::common::text::macro-definition   (静态库)   <- BUILD: cc_library(name = "macro-definition")
# ---------------------------------------------------------------------------
add_library(verible__common__text__macro-definition STATIC "macro-definition.cc")
target_sources(verible__common__text__macro-definition PRIVATE "macro-definition.h")

add_library(verible::common::text::macro-definition ALIAS verible__common__text__macro-definition)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__text__macro-definition PRIVATE verible_common_opts)

target_link_libraries(verible__common__text__macro-definition PUBLIC
    verible::common::text::token-info
    verible::common::util::container-util
    absl::status
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::common::text::parser-verifier   (静态库)   <- BUILD: cc_library(name = "parser-verifier")
# ---------------------------------------------------------------------------
add_library(verible__common__text__parser-verifier STATIC "parser-verifier.cc")
target_sources(verible__common__text__parser-verifier PRIVATE "parser-verifier.h")

add_library(verible::common::text::parser-verifier ALIAS verible__common__text__parser-verifier)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__text__parser-verifier PRIVATE verible_common_opts)

target_link_libraries(verible__common__text__parser-verifier PUBLIC
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::token-info
    verible::common::text::token-stream-view
    verible::common::text::tree-compare
    verible::common::text::visitors
)

# ---------------------------------------------------------------------------
# verible::common::text::constants   (纯头文件库)   <- BUILD: cc_library(name = "constants")
# ---------------------------------------------------------------------------
add_library(verible__common__text__constants INTERFACE)
target_sources(verible__common__text__constants INTERFACE "constants.h")

add_library(verible::common::text::constants ALIAS verible__common__text__constants)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__text__constants INTERFACE verible_common_opts)
