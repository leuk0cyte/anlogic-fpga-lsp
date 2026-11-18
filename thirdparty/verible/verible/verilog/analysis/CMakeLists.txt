# =============================================================================
#  GENERATED FILE - DO NOT EDIT
# =============================================================================
#  由 tools/gen_verible_cmake.py 从同目录的 Bazel BUILD 自动转译而来。
#  手工修改会在下次运行生成器时被覆盖；要改行为请改生成器本身。
#
#  源包: //verible/verilog/analysis
#
#  目录里也没有 BUILD 生成物时，本文件只声明库，不在这里编测试
#  （阶段 0 不编 Verible 自带的 225 个 cc_test）。
# =============================================================================
# ---------------------------------------------------------------------------
# 本包有 13 个 cc_test —— 按 CLAUDE 约定不编译 Verible 自带测试；
# 需要时用 `--with-tests` 重新生成。
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# verible::verilog::analysis::default-rules   (纯头文件库)   <- BUILD: cc_library(name = "default-rules")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__default-rules INTERFACE)
target_sources(verible__verilog__analysis__default-rules INTERFACE "default-rules.h")

add_library(verible::verilog::analysis::default-rules ALIAS verible__verilog__analysis__default-rules)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__default-rules INTERFACE verible_common_opts)

target_link_libraries(verible__verilog__analysis__default-rules INTERFACE
    verible::verilog::analysis::checkers::verilog-lint-rules
)

# ---------------------------------------------------------------------------
# verible::verilog::analysis::descriptions   (纯头文件库)   <- BUILD: cc_library(name = "descriptions")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__descriptions INTERFACE)
target_sources(verible__verilog__analysis__descriptions INTERFACE "descriptions.h")

add_library(verible::verilog::analysis::descriptions ALIAS verible__verilog__analysis__descriptions)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__descriptions INTERFACE verible_common_opts)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::extractors   (静态库)   <- BUILD: cc_library(name = "extractors")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__extractors STATIC "extractors.cc")
target_sources(verible__verilog__analysis__extractors PRIVATE "extractors.h")

add_library(verible::verilog::analysis::extractors ALIAS verible__verilog__analysis__extractors)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__extractors PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__extractors PUBLIC
    verible::verilog::analysis::verilog-analyzer
    verible::common::text::tree-utils
    verible::common::util::logging
    verible::verilog::CST::identifier
    verible::verilog::CST::module
    verible::verilog::preprocessor::verilog-preprocess
    absl::die_if_null
    absl::status
)

# ---------------------------------------------------------------------------
# verible::verilog::analysis::json-diagnostics   (静态库)   <- BUILD: cc_library(name = "json-diagnostics")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__json-diagnostics STATIC "json-diagnostics.cc")
target_sources(verible__verilog__analysis__json-diagnostics PRIVATE "json-diagnostics.h")

add_library(verible::verilog::analysis::json-diagnostics ALIAS verible__verilog__analysis__json-diagnostics)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__json-diagnostics PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__json-diagnostics PUBLIC
    verible::verilog::analysis::verilog-analyzer
    verible::common::analysis::file-analyzer
    verible::common::strings::line-column-map
    nlohmann_json::nlohmann_json
)

# ---------------------------------------------------------------------------
# verible::verilog::analysis::flow-tree   (静态库)   <- BUILD: cc_library(name = "flow-tree")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__flow-tree STATIC "flow-tree.cc")
target_sources(verible__verilog__analysis__flow-tree PRIVATE "flow-tree.h")

add_library(verible::verilog::analysis::flow-tree ALIAS verible__verilog__analysis__flow-tree)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__flow-tree PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__flow-tree PUBLIC
    verible::common::text::token-stream-view
    verible::common::util::logging
    verible::verilog::parser::verilog-token-enum
    absl::status
)

# ---------------------------------------------------------------------------
# verible::verilog::analysis::lint-rule-registry   (静态库)   <- BUILD: cc_library(name = "lint-rule-registry")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__lint-rule-registry STATIC "lint-rule-registry.cc")
target_sources(verible__verilog__analysis__lint-rule-registry PRIVATE "lint-rule-registry.h")

add_library(verible::verilog::analysis::lint-rule-registry ALIAS verible__verilog__analysis__lint-rule-registry)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__lint-rule-registry PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__lint-rule-registry PUBLIC
    verible::verilog::analysis::descriptions
    verible::common::analysis::line-lint-rule
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::text-structure-lint-rule
    verible::common::analysis::token-stream-lint-rule
    verible::common::strings::compare
    verible::common::util::container-util
    absl::node_hash_map
    absl::die_if_null
)

# ---------------------------------------------------------------------------
# verible::verilog::analysis::verilog-analyzer   (静态库)   <- BUILD: cc_library(name = "verilog-analyzer")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__verilog-analyzer STATIC "verilog-analyzer.cc" "verilog-excerpt-parse.cc")
target_sources(verible__verilog__analysis__verilog-analyzer PRIVATE "verilog-analyzer.h" "verilog-excerpt-parse.h")

add_library(verible::verilog::analysis::verilog-analyzer ALIAS verible__verilog__analysis__verilog-analyzer)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__verilog-analyzer PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__verilog-analyzer PUBLIC
    verible::common::analysis::file-analyzer
    verible::common::lexer::token-stream-adapter
    verible::common::strings::comment-utils
    verible::common::strings::mem-block
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::text-structure
    verible::common::text::token-info
    verible::common::text::token-stream-view
    verible::common::text::visitors
    verible::common::util::container-util
    verible::common::util::logging
    verible::common::util::status-macros
    verible::verilog::parser::verilog-lexer
    verible::verilog::parser::verilog-lexical-context
    verible::verilog::parser::verilog-parser
    verible::verilog::parser::verilog-token-classifications
    verible::verilog::parser::verilog-token-enum
    verible::verilog::preprocessor::verilog-preprocess
    absl::log
    absl::die_if_null
    absl::vlog_is_on
    absl::status
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::verilog::analysis::verilog-equivalence   (静态库)   <- BUILD: cc_library(name = "verilog-equivalence")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__verilog-equivalence STATIC "verilog-equivalence.cc")
target_sources(verible__verilog__analysis__verilog-equivalence PRIVATE "verilog-equivalence.h")

add_library(verible::verilog::analysis::verilog-equivalence ALIAS verible__verilog__analysis__verilog-equivalence)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__verilog-equivalence PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__verilog-equivalence PUBLIC
    verible::common::lexer::token-stream-adapter
    verible::common::text::token-info
    verible::common::text::token-stream-view
    verible::common::util::enum-flags
    verible::common::util::logging
    verible::verilog::parser::verilog-lexer
    verible::verilog::parser::verilog-parser
    verible::verilog::parser::verilog-token-classifications
    verible::verilog::parser::verilog-token-enum
)

# ---------------------------------------------------------------------------
# verible::verilog::analysis::verilog-linter-configuration   (静态库)   <- BUILD: cc_library(name = "verilog-linter-configuration")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__verilog-linter-configuration STATIC "verilog-linter-configuration.cc")
target_sources(verible__verilog__analysis__verilog-linter-configuration PRIVATE "verilog-linter-configuration.h")

add_library(verible::verilog::analysis::verilog-linter-configuration ALIAS verible__verilog__analysis__verilog-linter-configuration)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__verilog-linter-configuration PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__verilog-linter-configuration PUBLIC
    verible::verilog::analysis::default-rules
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    verible::common::analysis::line-lint-rule
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::text-structure-lint-rule
    verible::common::analysis::token-stream-lint-rule
    verible::common::util::container-util
    verible::common::util::enum-flags
    verible::common::util::file-util
    verible::common::util::logging
    verible::common::util::status-macros
    absl::status
    absl::statusor
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::verilog::analysis::verilog-linter-constants   (纯头文件库)   <- BUILD: cc_library(name = "verilog-linter-constants")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__verilog-linter-constants INTERFACE)
target_sources(verible__verilog__analysis__verilog-linter-constants INTERFACE "verilog-linter-constants.h")

add_library(verible::verilog::analysis::verilog-linter-constants ALIAS verible__verilog__analysis__verilog-linter-constants)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__verilog-linter-constants INTERFACE verible_common_opts)

# ---------------------------------------------------------------------------
# verible::verilog::analysis::verilog-linter   (静态库)   <- BUILD: cc_library(name = "verilog-linter")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__verilog-linter STATIC "verilog-linter.cc")
target_sources(verible__verilog__analysis__verilog-linter PRIVATE "verilog-linter.h")

add_library(verible::verilog::analysis::verilog-linter ALIAS verible__verilog__analysis__verilog-linter)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__verilog-linter PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__verilog-linter PUBLIC
    verible::verilog::analysis::default-rules
    verible::verilog::analysis::lint-rule-registry
    verible::verilog::analysis::verilog-analyzer
    verible::verilog::analysis::verilog-linter-configuration
    verible::verilog::analysis::verilog-linter-constants
    verible::common::analysis::citation
    verible::common::analysis::line-linter
    verible::common::analysis::lint-rule-status
    verible::common::analysis::lint-waiver
    verible::common::analysis::syntax-tree-linter
    verible::common::analysis::text-structure-linter
    verible::common::analysis::token-stream-linter
    verible::common::analysis::violation-handler
    verible::common::strings::line-column-map
    verible::common::text::concrete-syntax-tree
    verible::common::text::text-structure
    verible::common::text::token-info
    verible::common::util::file-util
    verible::common::util::logging
    verible::common::util::status-macros
    verible::verilog::parser::verilog-token-classifications
    verible::verilog::parser::verilog-token-enum
    absl::flags
    absl::log
    absl::die_if_null
    absl::vlog_is_on
    absl::status
    absl::statusor
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::verilog::analysis::verilog-filelist   (静态库)   <- BUILD: cc_library(name = "verilog-filelist")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__verilog-filelist STATIC "verilog-filelist.cc")
target_sources(verible__verilog__analysis__verilog-filelist PRIVATE "verilog-filelist.h")

add_library(verible::verilog::analysis::verilog-filelist ALIAS verible__verilog__analysis__verilog-filelist)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__verilog-filelist PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__verilog-filelist PUBLIC
    verible::common::util::file-util
    verible::common::util::iterator-range
    absl::status
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::verilog::analysis::verilog-project   (静态库)   <- BUILD: cc_library(name = "verilog-project")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__verilog-project STATIC "verilog-project.cc")
target_sources(verible__verilog__analysis__verilog-project PRIVATE "verilog-project.h")

add_library(verible::verilog::analysis::verilog-project ALIAS verible__verilog__analysis__verilog-project)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__verilog-project PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__verilog-project PUBLIC
    verible::verilog::analysis::verilog-analyzer
    verible::common::strings::mem-block
    verible::common::strings::string-memory-map
    verible::common::text::text-structure
    verible::common::util::file-util
    verible::common::util::logging
    absl::status
    absl::statusor
    absl::strings
    absl::time
)

# ---------------------------------------------------------------------------
# verible::verilog::analysis::symbol-table   (静态库)   <- BUILD: cc_library(name = "symbol-table")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__symbol-table STATIC "symbol-table.cc")
target_sources(verible__verilog__analysis__symbol-table PRIVATE "symbol-table.h")

add_library(verible::verilog::analysis::symbol-table ALIAS verible__verilog__analysis__symbol-table)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__symbol-table PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__symbol-table PUBLIC
    verible::verilog::analysis::verilog-project
    verible::common::strings::compare
    verible::common::strings::display-utils
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::token-info
    verible::common::text::tree-compare
    verible::common::text::tree-context-visitor
    verible::common::text::tree-utils
    verible::common::text::visitors
    verible::common::util::casts
    verible::common::util::enum-flags
    verible::common::util::logging
    verible::common::util::map-tree
    verible::common::util::spacer
    verible::common::util::tree-operations
    verible::common::util::value-saver
    verible::common::util::vector-tree
    verible::verilog::CST::class
    verible::verilog::CST::declaration
    verible::verilog::CST::functions
    verible::verilog::CST::macro
    verible::verilog::CST::module
    verible::verilog::CST::net
    verible::verilog::CST::package
    verible::verilog::CST::parameters
    verible::verilog::CST::port
    verible::verilog::CST::seq-block
    verible::verilog::CST::statement
    verible::verilog::CST::tasks
    verible::verilog::CST::type
    verible::verilog::CST::verilog-nonterminals
    verible::verilog::parser::verilog-parser
    verible::verilog::parser::verilog-token-enum
    absl::die_if_null
    absl::status
    absl::statusor
    absl::strings
    absl::time
)

# ---------------------------------------------------------------------------
# verible::verilog::analysis::dependencies   (静态库)   <- BUILD: cc_library(name = "dependencies")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__dependencies STATIC "dependencies.cc")
target_sources(verible__verilog__analysis__dependencies PRIVATE "dependencies.h")

add_library(verible::verilog::analysis::dependencies ALIAS verible__verilog__analysis__dependencies)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__dependencies PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__dependencies PUBLIC
    verible::verilog::analysis::symbol-table
    verible::verilog::analysis::verilog-project
    verible::common::strings::compare
    verible::common::strings::display-utils
    verible::common::util::logging
)
