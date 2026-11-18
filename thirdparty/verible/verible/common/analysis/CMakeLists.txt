# =============================================================================
#  GENERATED FILE - DO NOT EDIT
# =============================================================================
#  由 tools/gen_verible_cmake.py 从同目录的 Bazel BUILD 自动转译而来。
#  手工修改会在下次运行生成器时被覆盖；要改行为请改生成器本身。
#
#  源包: //verible/common/analysis
#
#  目录里也没有 BUILD 生成物时，本文件只声明库，不在这里编测试
#  （阶段 0 不编 Verible 自带的 225 个 cc_test）。
# =============================================================================
# ---------------------------------------------------------------------------
# 本包有 11 个 cc_test —— 按 CLAUDE 约定不编译 Verible 自带测试；
# 需要时用 `--with-tests` 重新生成。
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# verible::common::analysis::citation   (静态库)   <- BUILD: cc_library(name = "citation")
# ---------------------------------------------------------------------------
add_library(verible__common__analysis__citation STATIC "citation.cc")
target_sources(verible__common__analysis__citation PRIVATE "citation.h")

add_library(verible::common::analysis::citation ALIAS verible__common__analysis__citation)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__analysis__citation PRIVATE verible_common_opts)

target_link_libraries(verible__common__analysis__citation PUBLIC
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::common::analysis::lint-rule-status   (静态库)   <- BUILD: cc_library(name = "lint-rule-status")
# ---------------------------------------------------------------------------
add_library(verible__common__analysis__lint-rule-status STATIC "lint-rule-status.cc")
target_sources(verible__common__analysis__lint-rule-status PRIVATE "lint-rule-status.h")

add_library(verible::common::analysis::lint-rule-status ALIAS verible__common__analysis__lint-rule-status)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__analysis__lint-rule-status PRIVATE verible_common_opts)

target_link_libraries(verible__common__analysis__lint-rule-status PUBLIC
    verible::common::analysis::citation
    verible::common::strings::line-column-map
    verible::common::text::concrete-syntax-leaf
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::token-info
    verible::common::text::tree-utils
    verible::common::util::logging
    verible::common::util::spacer
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::common::analysis::lint-rule   (纯头文件库)   <- BUILD: cc_library(name = "lint-rule")
# ---------------------------------------------------------------------------
add_library(verible__common__analysis__lint-rule INTERFACE)
target_sources(verible__common__analysis__lint-rule INTERFACE "lint-rule.h")

add_library(verible::common::analysis::lint-rule ALIAS verible__common__analysis__lint-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__analysis__lint-rule INTERFACE verible_common_opts)

target_link_libraries(verible__common__analysis__lint-rule INTERFACE
    verible::common::analysis::lint-rule-status
    absl::status
)

# ---------------------------------------------------------------------------
# verible::common::analysis::command-file-lexer   (静态库)   <- BUILD: cc_library(name = "command-file-lexer")
# ---------------------------------------------------------------------------
add_library(verible__common__analysis__command-file-lexer STATIC "command-file-lexer.cc")
target_sources(verible__common__analysis__command-file-lexer PRIVATE "command-file-lexer.h")

# 构建期生成的文件（configure 时还不存在，由 cmake/Verible.cmake 挂接）：
set_property(TARGET verible__common__analysis__command-file-lexer PROPERTY verible_generated_sources "command-file.yy.cc")

add_library(verible::common::analysis::command-file-lexer ALIAS verible__common__analysis__command-file-lexer)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__analysis__command-file-lexer PRIVATE verible_common_opts)

target_link_libraries(verible__common__analysis__command-file-lexer PUBLIC
    verible::common::lexer::flex-lexer-adapter
    verible::common::lexer::token-stream-adapter
    verible::common::text::token-info
    verible::common::text::token-stream-view
    verible::common::util::iterator-range
    verible::common::util::logging
)

# 以下 BUILD 依赖指向阶段 1 才纳入的包，本阶段不链接（否则 target 不存在）：
#   - //bazel:flex

# ---------------------------------------------------------------------------
# verible::common::analysis::violation-handler   (静态库)   <- BUILD: cc_library(name = "violation-handler")
# ---------------------------------------------------------------------------
add_library(verible__common__analysis__violation-handler STATIC "violation-handler.cc")
target_sources(verible__common__analysis__violation-handler PRIVATE "violation-handler.h")

add_library(verible::common::analysis::violation-handler ALIAS verible__common__analysis__violation-handler)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__analysis__violation-handler PRIVATE verible_common_opts)

target_link_libraries(verible__common__analysis__violation-handler PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::strings::diff
    verible::common::util::file-util
    verible::common::util::logging
    verible::common::util::user-interaction
    absl::status
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::common::analysis::lint-waiver   (静态库)   <- BUILD: cc_library(name = "lint-waiver")
# ---------------------------------------------------------------------------
add_library(verible__common__analysis__lint-waiver STATIC "lint-waiver.cc")
target_sources(verible__common__analysis__lint-waiver PRIVATE "lint-waiver.h")

add_library(verible::common::analysis::lint-waiver ALIAS verible__common__analysis__lint-waiver)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__analysis__lint-waiver PRIVATE verible_common_opts)

target_link_libraries(verible__common__analysis__lint-waiver PUBLIC
    verible::common::analysis::command-file-lexer
    verible::common::strings::comment-utils
    verible::common::strings::line-column-map
    verible::common::strings::position
    verible::common::text::text-structure
    verible::common::text::token-info
    verible::common::text::token-stream-view
    verible::common::util::container-iterator-range
    verible::common::util::container-util
    verible::common::util::file-util
    verible::common::util::iterator-range
    verible::common::util::logging
    absl::flat_hash_map
    absl::status
    absl::statusor
    absl::strings
    re2::re2
)

# ---------------------------------------------------------------------------
# verible::common::analysis::file-analyzer   (静态库)   <- BUILD: cc_library(name = "file-analyzer")
# ---------------------------------------------------------------------------
add_library(verible__common__analysis__file-analyzer STATIC "file-analyzer.cc")
target_sources(verible__common__analysis__file-analyzer PRIVATE "file-analyzer.h")

add_library(verible::common::analysis::file-analyzer ALIAS verible__common__analysis__file-analyzer)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__analysis__file-analyzer PRIVATE verible_common_opts)

target_link_libraries(verible__common__analysis__file-analyzer PUBLIC
    verible::common::lexer::lexer
    verible::common::lexer::token-stream-adapter
    verible::common::parser::parse
    verible::common::strings::line-column-map
    verible::common::strings::mem-block
    verible::common::text::concrete-syntax-tree
    verible::common::text::text-structure
    verible::common::text::token-info
    verible::common::text::token-stream-view
    verible::common::util::logging
    verible::common::util::spacer
    absl::die_if_null
    absl::status
)

# ---------------------------------------------------------------------------
# verible::common::analysis::linter-test-utils   (静态库)   <- BUILD: cc_library(name = "linter-test-utils")
# ---------------------------------------------------------------------------
add_library(verible__common__analysis__linter-test-utils STATIC "linter-test-utils.cc")
target_sources(verible__common__analysis__linter-test-utils PRIVATE "linter-test-utils.h")

add_library(verible::common::analysis::linter-test-utils ALIAS verible__common__analysis__linter-test-utils)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__analysis__linter-test-utils PRIVATE verible_common_opts)

target_link_libraries(verible__common__analysis__linter-test-utils PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::text::token-info
    verible::common::text::token-info-test-util
    verible::common::util::algorithm
    verible::common::util::logging
    absl::status
    absl::strings
    GTest::gtest
)

# BUILD: testonly = 1（本阶段不编译测试，库照常生成以便被引用）

# ---------------------------------------------------------------------------
# verible::common::analysis::syntax-tree-search-test-utils   (静态库)   <- BUILD: cc_library(name = "syntax-tree-search-test-utils")
# ---------------------------------------------------------------------------
add_library(verible__common__analysis__syntax-tree-search-test-utils STATIC "syntax-tree-search-test-utils.cc")
target_sources(verible__common__analysis__syntax-tree-search-test-utils PRIVATE "syntax-tree-search-test-utils.h")

add_library(verible::common::analysis::syntax-tree-search-test-utils ALIAS verible__common__analysis__syntax-tree-search-test-utils)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__analysis__syntax-tree-search-test-utils PRIVATE verible_common_opts)

target_link_libraries(verible__common__analysis__syntax-tree-search-test-utils PUBLIC
    verible::common::analysis::syntax-tree-search
    verible::common::lexer::lexer-test-util
    verible::common::text::token-info
    verible::common::text::token-info-test-util
    verible::common::text::tree-utils
    verible::common::util::algorithm
    verible::common::util::logging
)

# BUILD: testonly = 1（本阶段不编译测试，库照常生成以便被引用）

# ---------------------------------------------------------------------------
# verible::common::analysis::line-linter   (静态库)   <- BUILD: cc_library(name = "line-linter")
# ---------------------------------------------------------------------------
add_library(verible__common__analysis__line-linter STATIC "line-linter.cc")
target_sources(verible__common__analysis__line-linter PRIVATE "line-linter.h")

add_library(verible::common::analysis::line-linter ALIAS verible__common__analysis__line-linter)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__analysis__line-linter PRIVATE verible_common_opts)

target_link_libraries(verible__common__analysis__line-linter PUBLIC
    verible::common::analysis::line-lint-rule
    verible::common::analysis::lint-rule-status
    verible::common::util::logging
    absl::die_if_null
)

# ---------------------------------------------------------------------------
# verible::common::analysis::line-linter-test-utils   (纯头文件库)   <- BUILD: cc_library(name = "line-linter-test-utils")
# ---------------------------------------------------------------------------
add_library(verible__common__analysis__line-linter-test-utils INTERFACE)
target_sources(verible__common__analysis__line-linter-test-utils INTERFACE "line-linter-test-utils.h")

add_library(verible::common::analysis::line-linter-test-utils ALIAS verible__common__analysis__line-linter-test-utils)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__analysis__line-linter-test-utils INTERFACE verible_common_opts)

target_link_libraries(verible__common__analysis__line-linter-test-utils INTERFACE
    verible::common::analysis::line-lint-rule
    verible::common::analysis::line-linter
    verible::common::analysis::lint-rule-status
    verible::common::analysis::linter-test-utils
    verible::common::text::text-structure
    verible::common::util::logging
)

# BUILD: testonly = 1（本阶段不编译测试，库照常生成以便被引用）

# ---------------------------------------------------------------------------
# verible::common::analysis::line-lint-rule   (纯头文件库)   <- BUILD: cc_library(name = "line-lint-rule")
# ---------------------------------------------------------------------------
add_library(verible__common__analysis__line-lint-rule INTERFACE)
target_sources(verible__common__analysis__line-lint-rule INTERFACE "line-lint-rule.h")

add_library(verible::common::analysis::line-lint-rule ALIAS verible__common__analysis__line-lint-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__analysis__line-lint-rule INTERFACE verible_common_opts)

target_link_libraries(verible__common__analysis__line-lint-rule INTERFACE
    verible::common::analysis::lint-rule
)

# ---------------------------------------------------------------------------
# verible::common::analysis::syntax-tree-linter   (静态库)   <- BUILD: cc_library(name = "syntax-tree-linter")
# ---------------------------------------------------------------------------
add_library(verible__common__analysis__syntax-tree-linter STATIC "syntax-tree-linter.cc")
target_sources(verible__common__analysis__syntax-tree-linter PRIVATE "syntax-tree-linter.h")

add_library(verible::common::analysis::syntax-tree-linter ALIAS verible__common__analysis__syntax-tree-linter)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__analysis__syntax-tree-linter PRIVATE verible_common_opts)

target_link_libraries(verible__common__analysis__syntax-tree-linter PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::tree-context-visitor
    verible::common::util::logging
    absl::die_if_null
)

# ---------------------------------------------------------------------------
# verible::common::analysis::syntax-tree-linter-test-utils   (纯头文件库)   <- BUILD: cc_library(name = "syntax-tree-linter-test-utils")
# ---------------------------------------------------------------------------
add_library(verible__common__analysis__syntax-tree-linter-test-utils INTERFACE)
target_sources(verible__common__analysis__syntax-tree-linter-test-utils INTERFACE "syntax-tree-linter-test-utils.h")

add_library(verible::common::analysis::syntax-tree-linter-test-utils ALIAS verible__common__analysis__syntax-tree-linter-test-utils)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__analysis__syntax-tree-linter-test-utils INTERFACE verible_common_opts)

target_link_libraries(verible__common__analysis__syntax-tree-linter-test-utils INTERFACE
    verible::common::analysis::lint-rule-status
    verible::common::analysis::linter-test-utils
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::syntax-tree-linter
    verible::common::text::text-structure
    verible::common::util::logging
    absl::die_if_null
)

# BUILD: testonly = 1（本阶段不编译测试，库照常生成以便被引用）

# ---------------------------------------------------------------------------
# verible::common::analysis::syntax-tree-lint-rule   (纯头文件库)   <- BUILD: cc_library(name = "syntax-tree-lint-rule")
# ---------------------------------------------------------------------------
add_library(verible__common__analysis__syntax-tree-lint-rule INTERFACE)
target_sources(verible__common__analysis__syntax-tree-lint-rule INTERFACE "syntax-tree-lint-rule.h")

add_library(verible::common::analysis::syntax-tree-lint-rule ALIAS verible__common__analysis__syntax-tree-lint-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__analysis__syntax-tree-lint-rule INTERFACE verible_common_opts)

target_link_libraries(verible__common__analysis__syntax-tree-lint-rule INTERFACE
    verible::common::analysis::lint-rule
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
)

# ---------------------------------------------------------------------------
# verible::common::analysis::syntax-tree-search   (静态库)   <- BUILD: cc_library(name = "syntax-tree-search")
# ---------------------------------------------------------------------------
add_library(verible__common__analysis__syntax-tree-search STATIC "syntax-tree-search.cc")
target_sources(verible__common__analysis__syntax-tree-search PRIVATE "syntax-tree-search.h")

add_library(verible::common::analysis::syntax-tree-search ALIAS verible__common__analysis__syntax-tree-search)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__analysis__syntax-tree-search PRIVATE verible_common_opts)

target_link_libraries(verible__common__analysis__syntax-tree-search PUBLIC
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::tree-context-visitor
)

# ---------------------------------------------------------------------------
# verible::common::analysis::text-structure-linter   (静态库)   <- BUILD: cc_library(name = "text-structure-linter")
# ---------------------------------------------------------------------------
add_library(verible__common__analysis__text-structure-linter STATIC "text-structure-linter.cc")
target_sources(verible__common__analysis__text-structure-linter PRIVATE "text-structure-linter.h")

add_library(verible::common::analysis::text-structure-linter ALIAS verible__common__analysis__text-structure-linter)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__analysis__text-structure-linter PRIVATE verible_common_opts)

target_link_libraries(verible__common__analysis__text-structure-linter PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::text-structure-lint-rule
    verible::common::text::text-structure
    verible::common::util::logging
    absl::die_if_null
)

# ---------------------------------------------------------------------------
# verible::common::analysis::text-structure-linter-test-utils   (纯头文件库)   <- BUILD: cc_library(name = "text-structure-linter-test-utils")
# ---------------------------------------------------------------------------
add_library(verible__common__analysis__text-structure-linter-test-utils INTERFACE)
target_sources(verible__common__analysis__text-structure-linter-test-utils INTERFACE "text-structure-linter-test-utils.h")

add_library(verible::common::analysis::text-structure-linter-test-utils ALIAS verible__common__analysis__text-structure-linter-test-utils)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__analysis__text-structure-linter-test-utils INTERFACE verible_common_opts)

target_link_libraries(verible__common__analysis__text-structure-linter-test-utils INTERFACE
    verible::common::analysis::lint-rule-status
    verible::common::analysis::linter-test-utils
    verible::common::analysis::text-structure-lint-rule
    verible::common::analysis::text-structure-linter
    verible::common::text::text-structure
    verible::common::util::logging
)

# BUILD: testonly = 1（本阶段不编译测试，库照常生成以便被引用）

# ---------------------------------------------------------------------------
# verible::common::analysis::text-structure-lint-rule   (纯头文件库)   <- BUILD: cc_library(name = "text-structure-lint-rule")
# ---------------------------------------------------------------------------
add_library(verible__common__analysis__text-structure-lint-rule INTERFACE)
target_sources(verible__common__analysis__text-structure-lint-rule INTERFACE "text-structure-lint-rule.h")

add_library(verible::common::analysis::text-structure-lint-rule ALIAS verible__common__analysis__text-structure-lint-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__analysis__text-structure-lint-rule INTERFACE verible_common_opts)

target_link_libraries(verible__common__analysis__text-structure-lint-rule INTERFACE
    verible::common::analysis::lint-rule
    verible::common::text::text-structure
)

# ---------------------------------------------------------------------------
# verible::common::analysis::token-stream-linter   (静态库)   <- BUILD: cc_library(name = "token-stream-linter")
# ---------------------------------------------------------------------------
add_library(verible__common__analysis__token-stream-linter STATIC "token-stream-linter.cc")
target_sources(verible__common__analysis__token-stream-linter PRIVATE "token-stream-linter.h")

add_library(verible::common::analysis::token-stream-linter ALIAS verible__common__analysis__token-stream-linter)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__analysis__token-stream-linter PRIVATE verible_common_opts)

target_link_libraries(verible__common__analysis__token-stream-linter PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::token-stream-lint-rule
    verible::common::text::token-stream-view
    verible::common::util::logging
    absl::die_if_null
)

# ---------------------------------------------------------------------------
# verible::common::analysis::token-stream-linter-test-utils   (纯头文件库)   <- BUILD: cc_library(name = "token-stream-linter-test-utils")
# ---------------------------------------------------------------------------
add_library(verible__common__analysis__token-stream-linter-test-utils INTERFACE)
target_sources(verible__common__analysis__token-stream-linter-test-utils INTERFACE "token-stream-linter-test-utils.h")

add_library(verible::common::analysis::token-stream-linter-test-utils ALIAS verible__common__analysis__token-stream-linter-test-utils)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__analysis__token-stream-linter-test-utils INTERFACE verible_common_opts)

target_link_libraries(verible__common__analysis__token-stream-linter-test-utils INTERFACE
    verible::common::analysis::lint-rule-status
    verible::common::analysis::linter-test-utils
    verible::common::analysis::token-stream-lint-rule
    verible::common::analysis::token-stream-linter
    verible::common::text::text-structure
    verible::common::util::logging
)

# BUILD: testonly = 1（本阶段不编译测试，库照常生成以便被引用）

# ---------------------------------------------------------------------------
# verible::common::analysis::token-stream-lint-rule   (纯头文件库)   <- BUILD: cc_library(name = "token-stream-lint-rule")
# ---------------------------------------------------------------------------
add_library(verible__common__analysis__token-stream-lint-rule INTERFACE)
target_sources(verible__common__analysis__token-stream-lint-rule INTERFACE "token-stream-lint-rule.h")

add_library(verible::common::analysis::token-stream-lint-rule ALIAS verible__common__analysis__token-stream-lint-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__analysis__token-stream-lint-rule INTERFACE verible_common_opts)

target_link_libraries(verible__common__analysis__token-stream-lint-rule INTERFACE
    verible::common::analysis::lint-rule
    verible::common::text::token-info
)
