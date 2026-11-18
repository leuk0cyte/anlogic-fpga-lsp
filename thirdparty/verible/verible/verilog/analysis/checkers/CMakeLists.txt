# =============================================================================
#  GENERATED FILE - DO NOT EDIT
# =============================================================================
#  由 tools/gen_verible_cmake.py 从同目录的 Bazel BUILD 自动转译而来。
#  手工修改会在下次运行生成器时被覆盖；要改行为请改生成器本身。
#
#  源包: //verible/verilog/analysis/checkers
#
#  目录里也没有 BUILD 生成物时，本文件只声明库，不在这里编测试
#  （阶段 0 不编 Verible 自带的 225 个 cc_test）。
# =============================================================================
# ---------------------------------------------------------------------------
# 本包有 60 个 cc_test —— 按 CLAUDE 约定不编译 Verible 自带测试；
# 需要时用 `--with-tests` 重新生成。
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::verilog-lint-rules   (纯头文件库)   <- BUILD: cc_library(name = "verilog-lint-rules")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__verilog-lint-rules INTERFACE)

add_library(verible::verilog::analysis::checkers::verilog-lint-rules ALIAS verible__verilog__analysis__checkers__verilog-lint-rules)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__verilog-lint-rules INTERFACE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__verilog-lint-rules INTERFACE
    verible::verilog::analysis::checkers::always-comb-blocking-rule
    verible::verilog::analysis::checkers::always-comb-rule
    verible::verilog::analysis::checkers::always-ff-non-blocking-rule
    verible::verilog::analysis::checkers::banned-declared-name-patterns-rule
    verible::verilog::analysis::checkers::case-missing-default-rule
    verible::verilog::analysis::checkers::constraint-name-style-rule
    verible::verilog::analysis::checkers::create-object-name-match-rule
    verible::verilog::analysis::checkers::dff-name-style-rule
    verible::verilog::analysis::checkers::disable-statement-rule
    verible::verilog::analysis::checkers::endif-comment-rule
    verible::verilog::analysis::checkers::enum-name-style-rule
    verible::verilog::analysis::checkers::explicit-begin-rule
    verible::verilog::analysis::checkers::explicit-function-lifetime-rule
    verible::verilog::analysis::checkers::explicit-function-task-parameter-type-rule
    verible::verilog::analysis::checkers::explicit-parameter-storage-type-rule
    verible::verilog::analysis::checkers::explicit-task-lifetime-rule
    verible::verilog::analysis::checkers::forbid-consecutive-null-statements-rule
    verible::verilog::analysis::checkers::forbid-defparam-rule
    verible::verilog::analysis::checkers::forbid-line-continuations-rule
    verible::verilog::analysis::checkers::forbid-negative-array-dim
    verible::verilog::analysis::checkers::forbidden-anonymous-enums-rule
    verible::verilog::analysis::checkers::forbidden-anonymous-structs-unions-rule
    verible::verilog::analysis::checkers::forbidden-macro-rule
    verible::verilog::analysis::checkers::forbidden-symbol-rule
    verible::verilog::analysis::checkers::generate-label-prefix-rule
    verible::verilog::analysis::checkers::generate-label-rule
    verible::verilog::analysis::checkers::instance-shadow-rule
    verible::verilog::analysis::checkers::interface-name-style-rule
    verible::verilog::analysis::checkers::legacy-generate-region-rule
    verible::verilog::analysis::checkers::legacy-genvar-declaration-rule
    verible::verilog::analysis::checkers::line-length-rule
    verible::verilog::analysis::checkers::macro-name-style-rule
    verible::verilog::analysis::checkers::macro-string-concatenation-rule
    verible::verilog::analysis::checkers::mismatched-labels-rule
    verible::verilog::analysis::checkers::module-begin-block-rule
    verible::verilog::analysis::checkers::module-filename-rule
    verible::verilog::analysis::checkers::module-instantiation-rules
    verible::verilog::analysis::checkers::no-tabs-rule
    verible::verilog::analysis::checkers::no-trailing-spaces-rule
    verible::verilog::analysis::checkers::numeric-format-string-style-rule
    verible::verilog::analysis::checkers::one-module-per-file-rule
    verible::verilog::analysis::checkers::package-filename-rule
    verible::verilog::analysis::checkers::packed-dimensions-rule
    verible::verilog::analysis::checkers::parameter-name-style-rule
    verible::verilog::analysis::checkers::parameter-type-name-style-rule
    verible::verilog::analysis::checkers::plusarg-assignment-rule
    verible::verilog::analysis::checkers::port-name-suffix-rule
    verible::verilog::analysis::checkers::positive-meaning-parameter-name-rule
    verible::verilog::analysis::checkers::posix-eof-rule
    verible::verilog::analysis::checkers::proper-parameter-declaration-rule
    verible::verilog::analysis::checkers::signal-name-style-rule
    verible::verilog::analysis::checkers::struct-union-name-style-rule
    verible::verilog::analysis::checkers::suggest-parentheses-rule
    verible::verilog::analysis::checkers::suspicious-semicolon-rule
    verible::verilog::analysis::checkers::truncated-numeric-literal-rule
    verible::verilog::analysis::checkers::undersized-binary-literal-rule
    verible::verilog::analysis::checkers::unpacked-dimensions-rule
    verible::verilog::analysis::checkers::uvm-macro-semicolon-rule
    verible::verilog::analysis::checkers::v2001-generate-begin-rule
    verible::verilog::analysis::checkers::void-cast-rule
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::banned-declared-name-patterns-rule   (静态库)   <- BUILD: cc_library(name = "banned-declared-name-patterns-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__banned-declared-name-patterns-rule STATIC "banned-declared-name-patterns-rule.cc")
target_sources(verible__verilog__analysis__checkers__banned-declared-name-patterns-rule PRIVATE "banned-declared-name-patterns-rule.h")

add_library(verible::verilog::analysis::checkers::banned-declared-name-patterns-rule ALIAS verible__verilog__analysis__checkers__banned-declared-name-patterns-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__banned-declared-name-patterns-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__banned-declared-name-patterns-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::token-info
    verible::verilog::CST::module
    verible::verilog::CST::package
    verible::verilog::CST::verilog-matchers
    verible::verilog::CST::verilog-nonterminals
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    absl::strings
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::no-tabs-rule   (静态库)   <- BUILD: cc_library(name = "no-tabs-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__no-tabs-rule STATIC "no-tabs-rule.cc")
target_sources(verible__verilog__analysis__checkers__no-tabs-rule PRIVATE "no-tabs-rule.h")

add_library(verible::verilog::analysis::checkers::no-tabs-rule ALIAS verible__verilog__analysis__checkers__no-tabs-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__no-tabs-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__no-tabs-rule PUBLIC
    verible::common::analysis::line-lint-rule
    verible::common::analysis::lint-rule-status
    verible::common::text::token-info
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    verible::verilog::parser::verilog-token-enum
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::no-trailing-spaces-rule   (静态库)   <- BUILD: cc_library(name = "no-trailing-spaces-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__no-trailing-spaces-rule STATIC "no-trailing-spaces-rule.cc")
target_sources(verible__verilog__analysis__checkers__no-trailing-spaces-rule PRIVATE "no-trailing-spaces-rule.h")

add_library(verible::verilog::analysis::checkers::no-trailing-spaces-rule ALIAS verible__verilog__analysis__checkers__no-trailing-spaces-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__no-trailing-spaces-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__no-trailing-spaces-rule PUBLIC
    verible::common::analysis::line-lint-rule
    verible::common::analysis::lint-rule-status
    verible::common::text::token-info
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    verible::verilog::parser::verilog-token-enum
    absl::strings
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::positive-meaning-parameter-name-rule   (静态库)   <- BUILD: cc_library(name = "positive-meaning-parameter-name-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__positive-meaning-parameter-name-rule STATIC "positive-meaning-parameter-name-rule.cc")
target_sources(verible__verilog__analysis__checkers__positive-meaning-parameter-name-rule PRIVATE "positive-meaning-parameter-name-rule.h")

add_library(verible::verilog::analysis::checkers::positive-meaning-parameter-name-rule ALIAS verible__verilog__analysis__checkers__positive-meaning-parameter-name-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__positive-meaning-parameter-name-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__positive-meaning-parameter-name-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::token-info
    verible::verilog::CST::parameters
    verible::verilog::CST::verilog-matchers
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    absl::strings
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::posix-eof-rule   (静态库)   <- BUILD: cc_library(name = "posix-eof-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__posix-eof-rule STATIC "posix-eof-rule.cc")
target_sources(verible__verilog__analysis__checkers__posix-eof-rule PRIVATE "posix-eof-rule.h")

add_library(verible::verilog::analysis::checkers::posix-eof-rule ALIAS verible__verilog__analysis__checkers__posix-eof-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__posix-eof-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__posix-eof-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::text-structure-lint-rule
    verible::common::text::text-structure
    verible::common::text::token-info
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    verible::verilog::parser::verilog-token-enum
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::line-length-rule   (静态库)   <- BUILD: cc_library(name = "line-length-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__line-length-rule STATIC "line-length-rule.cc")
target_sources(verible__verilog__analysis__checkers__line-length-rule PRIVATE "line-length-rule.h")

add_library(verible::verilog::analysis::checkers::line-length-rule ALIAS verible__verilog__analysis__checkers__line-length-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__line-length-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__line-length-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::text-structure-lint-rule
    verible::common::strings::comment-utils
    verible::common::strings::utf8
    verible::common::text::config-utils
    verible::common::text::constants
    verible::common::text::text-structure
    verible::common::text::token-info
    verible::common::text::token-stream-view
    verible::common::util::iterator-range
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    verible::verilog::analysis::verilog-linter-constants
    verible::verilog::parser::verilog-token-classifications
    verible::verilog::parser::verilog-token-enum
    absl::status
    absl::strings
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::forbid-consecutive-null-statements-rule   (静态库)   <- BUILD: cc_library(name = "forbid-consecutive-null-statements-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__forbid-consecutive-null-statements-rule STATIC "forbid-consecutive-null-statements-rule.cc")
target_sources(verible__verilog__analysis__checkers__forbid-consecutive-null-statements-rule PRIVATE "forbid-consecutive-null-statements-rule.h")

add_library(verible::verilog::analysis::checkers::forbid-consecutive-null-statements-rule ALIAS verible__verilog__analysis__checkers__forbid-consecutive-null-statements-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__forbid-consecutive-null-statements-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__forbid-consecutive-null-statements-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::text::concrete-syntax-leaf
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::verilog::CST::verilog-matchers
    verible::verilog::CST::verilog-nonterminals
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::forbid-defparam-rule   (静态库)   <- BUILD: cc_library(name = "forbid-defparam-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__forbid-defparam-rule STATIC "forbid-defparam-rule.cc")
target_sources(verible__verilog__analysis__checkers__forbid-defparam-rule PRIVATE "forbid-defparam-rule.h")

add_library(verible::verilog::analysis::checkers::forbid-defparam-rule ALIAS verible__verilog__analysis__checkers__forbid-defparam-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__forbid-defparam-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__forbid-defparam-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::concrete-syntax-leaf
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::tree-utils
    verible::common::util::logging
    verible::verilog::CST::verilog-matchers
    verible::verilog::CST::verilog-nonterminals
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    verible::verilog::parser::verilog-token-enum
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::mismatched-labels-rule   (静态库)   <- BUILD: cc_library(name = "mismatched-labels-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__mismatched-labels-rule STATIC "mismatched-labels-rule.cc")
target_sources(verible__verilog__analysis__checkers__mismatched-labels-rule PRIVATE "mismatched-labels-rule.h")

add_library(verible::verilog::analysis::checkers::mismatched-labels-rule ALIAS verible__verilog__analysis__checkers__mismatched-labels-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__mismatched-labels-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__mismatched-labels-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::verilog::CST::seq-block
    verible::verilog::CST::verilog-matchers
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::module-begin-block-rule   (静态库)   <- BUILD: cc_library(name = "module-begin-block-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__module-begin-block-rule STATIC "module-begin-block-rule.cc")
target_sources(verible__verilog__analysis__checkers__module-begin-block-rule PRIVATE "module-begin-block-rule.h")

add_library(verible::verilog::analysis::checkers::module-begin-block-rule ALIAS verible__verilog__analysis__checkers__module-begin-block-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__module-begin-block-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__module-begin-block-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::verilog::CST::verilog-matchers
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::module-filename-rule   (静态库)   <- BUILD: cc_library(name = "module-filename-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__module-filename-rule STATIC "module-filename-rule.cc")
target_sources(verible__verilog__analysis__checkers__module-filename-rule PRIVATE "module-filename-rule.h")

add_library(verible::verilog::analysis::checkers::module-filename-rule ALIAS verible__verilog__analysis__checkers__module-filename-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__module-filename-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__module-filename-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-search
    verible::common::analysis::text-structure-lint-rule
    verible::common::text::concrete-syntax-leaf
    verible::common::text::config-utils
    verible::common::text::symbol
    verible::common::text::text-structure
    verible::common::text::token-info
    verible::common::util::file-util
    verible::common::util::logging
    verible::verilog::CST::module
    verible::verilog::CST::verilog-nonterminals
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    absl::status
    absl::strings
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::package-filename-rule   (静态库)   <- BUILD: cc_library(name = "package-filename-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__package-filename-rule STATIC "package-filename-rule.cc")
target_sources(verible__verilog__analysis__checkers__package-filename-rule PRIVATE "package-filename-rule.h")

add_library(verible::verilog::analysis::checkers::package-filename-rule ALIAS verible__verilog__analysis__checkers__package-filename-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__package-filename-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__package-filename-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-search
    verible::common::analysis::text-structure-lint-rule
    verible::common::text::config-utils
    verible::common::text::text-structure
    verible::common::text::token-info
    verible::common::util::file-util
    verible::verilog::CST::package
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    absl::status
    absl::strings
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::module-instantiation-rules   (静态库)   <- BUILD: cc_library(name = "module-instantiation-rules")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__module-instantiation-rules STATIC "module-instantiation-rules.cc")
target_sources(verible__verilog__analysis__checkers__module-instantiation-rules PRIVATE "module-instantiation-rules.h")

add_library(verible::verilog::analysis::checkers::module-instantiation-rules ALIAS verible__verilog__analysis__checkers__module-instantiation-rules)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__module-instantiation-rules PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__module-instantiation-rules PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::token-info
    verible::common::text::tree-utils
    verible::common::util::casts
    verible::verilog::CST::context-functions
    verible::verilog::CST::verilog-matchers
    verible::verilog::CST::verilog-nonterminals
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    absl::die_if_null
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::one-module-per-file-rule   (静态库)   <- BUILD: cc_library(name = "one-module-per-file-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__one-module-per-file-rule STATIC "one-module-per-file-rule.cc")
target_sources(verible__verilog__analysis__checkers__one-module-per-file-rule PRIVATE "one-module-per-file-rule.h")

add_library(verible::verilog::analysis::checkers::one-module-per-file-rule ALIAS verible__verilog__analysis__checkers__one-module-per-file-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__one-module-per-file-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__one-module-per-file-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-search
    verible::common::analysis::text-structure-lint-rule
    verible::common::text::text-structure
    verible::common::util::logging
    verible::verilog::CST::module
    verible::verilog::CST::verilog-nonterminals
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    absl::strings
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::endif-comment-rule   (静态库)   <- BUILD: cc_library(name = "endif-comment-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__endif-comment-rule STATIC "endif-comment-rule.cc")
target_sources(verible__verilog__analysis__checkers__endif-comment-rule PRIVATE "endif-comment-rule.h")

add_library(verible::verilog::analysis::checkers::endif-comment-rule ALIAS verible__verilog__analysis__checkers__endif-comment-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__endif-comment-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__endif-comment-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::token-stream-lint-rule
    verible::common::strings::comment-utils
    verible::common::text::token-info
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    verible::verilog::parser::verilog-token-enum
    absl::strings
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::forbidden-anonymous-enums-rule   (静态库)   <- BUILD: cc_library(name = "forbidden-anonymous-enums-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__forbidden-anonymous-enums-rule STATIC "forbidden-anonymous-enums-rule.cc")
target_sources(verible__verilog__analysis__checkers__forbidden-anonymous-enums-rule PRIVATE "forbidden-anonymous-enums-rule.h")

add_library(verible::verilog::analysis::checkers::forbidden-anonymous-enums-rule ALIAS verible__verilog__analysis__checkers__forbidden-anonymous-enums-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__forbidden-anonymous-enums-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__forbidden-anonymous-enums-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::verilog::CST::verilog-matchers
    verible::verilog::CST::verilog-nonterminals
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::enum-name-style-rule   (静态库)   <- BUILD: cc_library(name = "enum-name-style-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__enum-name-style-rule STATIC "enum-name-style-rule.cc")
target_sources(verible__verilog__analysis__checkers__enum-name-style-rule PRIVATE "enum-name-style-rule.h")

add_library(verible::verilog::analysis::checkers::enum-name-style-rule ALIAS verible__verilog__analysis__checkers__enum-name-style-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__enum-name-style-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__enum-name-style-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::config-utils
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::util::logging
    verible::verilog::CST::type
    verible::verilog::CST::verilog-matchers
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    absl::die_if_null
    absl::status
    absl::strings
    re2::re2
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::forbidden-macro-rule   (静态库)   <- BUILD: cc_library(name = "forbidden-macro-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__forbidden-macro-rule STATIC "forbidden-macro-rule.cc")
target_sources(verible__verilog__analysis__checkers__forbidden-macro-rule PRIVATE "forbidden-macro-rule.h")

add_library(verible::verilog::analysis::checkers::forbidden-macro-rule ALIAS verible__verilog__analysis__checkers__forbidden-macro-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__forbidden-macro-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__forbidden-macro-rule PUBLIC
    verible::common::analysis::citation
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::concrete-syntax-leaf
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::token-info
    verible::common::util::container-util
    verible::verilog::CST::verilog-matchers
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::forbidden-anonymous-structs-unions-rule   (静态库)   <- BUILD: cc_library(name = "forbidden-anonymous-structs-unions-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__forbidden-anonymous-structs-unions-rule STATIC "forbidden-anonymous-structs-unions-rule.cc")
target_sources(verible__verilog__analysis__checkers__forbidden-anonymous-structs-unions-rule PRIVATE "forbidden-anonymous-structs-unions-rule.h")

add_library(verible::verilog::analysis::checkers::forbidden-anonymous-structs-unions-rule ALIAS verible__verilog__analysis__checkers__forbidden-anonymous-structs-unions-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__forbidden-anonymous-structs-unions-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__forbidden-anonymous-structs-unions-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::config-utils
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::verilog::CST::verilog-matchers
    verible::verilog::CST::verilog-nonterminals
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    absl::status
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::forbidden-symbol-rule   (静态库)   <- BUILD: cc_library(name = "forbidden-symbol-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__forbidden-symbol-rule STATIC "forbidden-symbol-rule.cc")
target_sources(verible__verilog__analysis__checkers__forbidden-symbol-rule PRIVATE "forbidden-symbol-rule.h")

add_library(verible::verilog::analysis::checkers::forbidden-symbol-rule ALIAS verible__verilog__analysis__checkers__forbidden-symbol-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__forbidden-symbol-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__forbidden-symbol-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::concrete-syntax-leaf
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::token-info
    verible::common::util::container-util
    verible::verilog::CST::verilog-matchers
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::generate-label-rule   (静态库)   <- BUILD: cc_library(name = "generate-label-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__generate-label-rule STATIC "generate-label-rule.cc")
target_sources(verible__verilog__analysis__checkers__generate-label-rule PRIVATE "generate-label-rule.h")

add_library(verible::verilog::analysis::checkers::generate-label-rule ALIAS verible__verilog__analysis__checkers__generate-label-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__generate-label-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__generate-label-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::analysis::matcher::core-matchers
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::verilog::CST::verilog-matchers
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::generate-label-prefix-rule   (静态库)   <- BUILD: cc_library(name = "generate-label-prefix-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__generate-label-prefix-rule STATIC "generate-label-prefix-rule.cc")
target_sources(verible__verilog__analysis__checkers__generate-label-prefix-rule PRIVATE "generate-label-prefix-rule.h")

add_library(verible::verilog::analysis::checkers::generate-label-prefix-rule ALIAS verible__verilog__analysis__checkers__generate-label-prefix-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__generate-label-prefix-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__generate-label-prefix-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::config-utils
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::token-info
    verible::common::text::tree-utils
    verible::verilog::CST::seq-block
    verible::verilog::CST::verilog-matchers
    verible::verilog::CST::verilog-nonterminals
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    absl::status
    absl::strings
    re2::re2
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::v2001-generate-begin-rule   (静态库)   <- BUILD: cc_library(name = "v2001-generate-begin-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__v2001-generate-begin-rule STATIC "v2001-generate-begin-rule.cc")
target_sources(verible__verilog__analysis__checkers__v2001-generate-begin-rule PRIVATE "v2001-generate-begin-rule.h")

add_library(verible::verilog::analysis::checkers::v2001-generate-begin-rule ALIAS verible__verilog__analysis__checkers__v2001-generate-begin-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__v2001-generate-begin-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__v2001-generate-begin-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::tree-utils
    verible::verilog::CST::verilog-matchers
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::void-cast-rule   (静态库)   <- BUILD: cc_library(name = "void-cast-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__void-cast-rule STATIC "void-cast-rule.cc")
target_sources(verible__verilog__analysis__checkers__void-cast-rule PRIVATE "void-cast-rule.h")

add_library(verible::verilog::analysis::checkers::void-cast-rule ALIAS verible__verilog__analysis__checkers__void-cast-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__void-cast-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__void-cast-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::analysis::matcher::core-matchers
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::token-info
    verible::common::text::tree-utils
    verible::verilog::CST::verilog-matchers
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    absl::die_if_null
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::undersized-binary-literal-rule   (静态库)   <- BUILD: cc_library(name = "undersized-binary-literal-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__undersized-binary-literal-rule STATIC "undersized-binary-literal-rule.cc")
target_sources(verible__verilog__analysis__checkers__undersized-binary-literal-rule PRIVATE "undersized-binary-literal-rule.h")

add_library(verible::verilog::analysis::checkers::undersized-binary-literal-rule ALIAS verible__verilog__analysis__checkers__undersized-binary-literal-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__undersized-binary-literal-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__undersized-binary-literal-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::config-utils
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::token-info
    verible::common::text::tree-utils
    verible::common::util::logging
    verible::verilog::CST::numbers
    verible::verilog::CST::verilog-matchers
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    absl::status
    absl::strings
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::truncated-numeric-literal-rule   (静态库)   <- BUILD: cc_library(name = "truncated-numeric-literal-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__truncated-numeric-literal-rule STATIC "truncated-numeric-literal-rule.cc")
target_sources(verible__verilog__analysis__checkers__truncated-numeric-literal-rule PRIVATE "truncated-numeric-literal-rule.h")

add_library(verible::verilog::analysis::checkers::truncated-numeric-literal-rule ALIAS verible__verilog__analysis__checkers__truncated-numeric-literal-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__truncated-numeric-literal-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__truncated-numeric-literal-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::token-info
    verible::common::text::tree-utils
    verible::verilog::CST::numbers
    verible::verilog::CST::verilog-matchers
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    absl::int128
    absl::strings
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::create-object-name-match-rule   (静态库)   <- BUILD: cc_library(name = "create-object-name-match-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__create-object-name-match-rule STATIC "create-object-name-match-rule.cc")
target_sources(verible__verilog__analysis__checkers__create-object-name-match-rule PRIVATE "create-object-name-match-rule.h")

add_library(verible::verilog::analysis::checkers::create-object-name-match-rule ALIAS verible__verilog__analysis__checkers__create-object-name-match-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__create-object-name-match-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__create-object-name-match-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::token-info
    verible::common::text::tree-utils
    verible::verilog::CST::expression
    verible::verilog::CST::verilog-matchers
    verible::verilog::CST::verilog-nonterminals
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    verible::verilog::parser::verilog-token-enum
    absl::strings
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::always-comb-rule   (静态库)   <- BUILD: cc_library(name = "always-comb-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__always-comb-rule STATIC "always-comb-rule.cc")
target_sources(verible__verilog__analysis__checkers__always-comb-rule PRIVATE "always-comb-rule.h")

add_library(verible::verilog::analysis::checkers::always-comb-rule ALIAS verible__verilog__analysis__checkers__always-comb-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__always-comb-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__always-comb-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::tree-utils
    verible::verilog::CST::statement
    verible::verilog::CST::verilog-matchers
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::forbid-line-continuations-rule   (静态库)   <- BUILD: cc_library(name = "forbid-line-continuations-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__forbid-line-continuations-rule STATIC "forbid-line-continuations-rule.cc")
target_sources(verible__verilog__analysis__checkers__forbid-line-continuations-rule PRIVATE "forbid-line-continuations-rule.h")

add_library(verible::verilog::analysis::checkers::forbid-line-continuations-rule ALIAS verible__verilog__analysis__checkers__forbid-line-continuations-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__forbid-line-continuations-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__forbid-line-continuations-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::tree-utils
    verible::verilog::CST::verilog-matchers
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    verible::verilog::parser::verilog-token-enum
    absl::strings
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::suggest-parentheses-rule   (静态库)   <- BUILD: cc_library(name = "suggest-parentheses-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__suggest-parentheses-rule STATIC "suggest-parentheses-rule.cc")
target_sources(verible__verilog__analysis__checkers__suggest-parentheses-rule PRIVATE "suggest-parentheses-rule.h")

add_library(verible::verilog::analysis::checkers::suggest-parentheses-rule ALIAS verible__verilog__analysis__checkers__suggest-parentheses-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__suggest-parentheses-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__suggest-parentheses-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::token-info
    verible::common::text::tree-utils
    verible::verilog::CST::expression
    verible::verilog::CST::verilog-nonterminals
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    verible::verilog::parser::verilog-token-enum
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::always-comb-blocking-rule   (静态库)   <- BUILD: cc_library(name = "always-comb-blocking-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__always-comb-blocking-rule STATIC "always-comb-blocking-rule.cc")
target_sources(verible__verilog__analysis__checkers__always-comb-blocking-rule PRIVATE "always-comb-blocking-rule.h")

add_library(verible::verilog::analysis::checkers::always-comb-blocking-rule ALIAS verible__verilog__analysis__checkers__always-comb-blocking-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__always-comb-blocking-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__always-comb-blocking-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::syntax-tree-search
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::tree-utils
    verible::verilog::CST::verilog-matchers
    verible::verilog::CST::verilog-nonterminals
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    verible::verilog::parser::verilog-token-enum
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::always-ff-non-blocking-rule   (静态库)   <- BUILD: cc_library(name = "always-ff-non-blocking-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__always-ff-non-blocking-rule STATIC "always-ff-non-blocking-rule.cc")
target_sources(verible__verilog__analysis__checkers__always-ff-non-blocking-rule PRIVATE "always-ff-non-blocking-rule.h")

add_library(verible::verilog::analysis::checkers::always-ff-non-blocking-rule ALIAS verible__verilog__analysis__checkers__always-ff-non-blocking-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__always-ff-non-blocking-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__always-ff-non-blocking-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::syntax-tree-search
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::config-utils
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::tree-utils
    verible::common::util::logging
    verible::verilog::CST::verilog-matchers
    verible::verilog::CST::verilog-nonterminals
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    absl::status
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::explicit-begin-rule   (静态库)   <- BUILD: cc_library(name = "explicit-begin-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__explicit-begin-rule STATIC "explicit-begin-rule.cc")
target_sources(verible__verilog__analysis__checkers__explicit-begin-rule PRIVATE "explicit-begin-rule.h")

add_library(verible::verilog::analysis::checkers::explicit-begin-rule ALIAS verible__verilog__analysis__checkers__explicit-begin-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__explicit-begin-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__explicit-begin-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::token-stream-lint-rule
    verible::common::text::config-utils
    verible::common::text::token-info
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    verible::verilog::parser::verilog-token-enum
    absl::core_headers
    absl::status
    absl::strings
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::explicit-function-lifetime-rule   (静态库)   <- BUILD: cc_library(name = "explicit-function-lifetime-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__explicit-function-lifetime-rule STATIC "explicit-function-lifetime-rule.cc")
target_sources(verible__verilog__analysis__checkers__explicit-function-lifetime-rule PRIVATE "explicit-function-lifetime-rule.h")

add_library(verible::verilog::analysis::checkers::explicit-function-lifetime-rule ALIAS verible__verilog__analysis__checkers__explicit-function-lifetime-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__explicit-function-lifetime-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__explicit-function-lifetime-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::token-info
    verible::common::text::tree-utils
    verible::verilog::CST::context-functions
    verible::verilog::CST::functions
    verible::verilog::CST::identifier
    verible::verilog::CST::verilog-matchers
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    verible::verilog::parser::verilog-token-enum
    absl::die_if_null
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::explicit-task-lifetime-rule   (静态库)   <- BUILD: cc_library(name = "explicit-task-lifetime-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__explicit-task-lifetime-rule STATIC "explicit-task-lifetime-rule.cc")
target_sources(verible__verilog__analysis__checkers__explicit-task-lifetime-rule PRIVATE "explicit-task-lifetime-rule.h")

add_library(verible::verilog::analysis::checkers::explicit-task-lifetime-rule ALIAS verible__verilog__analysis__checkers__explicit-task-lifetime-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__explicit-task-lifetime-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__explicit-task-lifetime-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::token-info
    verible::common::text::tree-utils
    verible::verilog::CST::context-functions
    verible::verilog::CST::identifier
    verible::verilog::CST::tasks
    verible::verilog::CST::verilog-matchers
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    verible::verilog::parser::verilog-token-enum
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::packed-dimensions-rule   (静态库)   <- BUILD: cc_library(name = "packed-dimensions-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__packed-dimensions-rule STATIC "packed-dimensions-rule.cc")
target_sources(verible__verilog__analysis__checkers__packed-dimensions-rule PRIVATE "packed-dimensions-rule.h")

add_library(verible::verilog::analysis::checkers::packed-dimensions-rule ALIAS verible__verilog__analysis__checkers__packed-dimensions-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__packed-dimensions-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__packed-dimensions-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::token-info
    verible::common::text::tree-utils
    verible::verilog::CST::context-functions
    verible::verilog::CST::dimensions
    verible::verilog::CST::expression
    verible::verilog::CST::verilog-matchers
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    verible::verilog::parser::verilog-token-enum
    absl::die_if_null
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::unpacked-dimensions-rule   (静态库)   <- BUILD: cc_library(name = "unpacked-dimensions-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__unpacked-dimensions-rule STATIC "unpacked-dimensions-rule.cc")
target_sources(verible__verilog__analysis__checkers__unpacked-dimensions-rule PRIVATE "unpacked-dimensions-rule.h")

add_library(verible::verilog::analysis::checkers::unpacked-dimensions-rule ALIAS verible__verilog__analysis__checkers__unpacked-dimensions-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__unpacked-dimensions-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__unpacked-dimensions-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::token-info
    verible::common::text::tree-utils
    verible::verilog::CST::context-functions
    verible::verilog::CST::dimensions
    verible::verilog::CST::expression
    verible::verilog::CST::verilog-matchers
    verible::verilog::CST::verilog-nonterminals
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    verible::verilog::parser::verilog-token-enum
    absl::die_if_null
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::forbid-negative-array-dim   (静态库)   <- BUILD: cc_library(name = "forbid-negative-array-dim")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__forbid-negative-array-dim STATIC "forbid-negative-array-dim.cc")
target_sources(verible__verilog__analysis__checkers__forbid-negative-array-dim PRIVATE "forbid-negative-array-dim.h")

add_library(verible::verilog::analysis::checkers::forbid-negative-array-dim ALIAS verible__verilog__analysis__checkers__forbid-negative-array-dim)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__forbid-negative-array-dim PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__forbid-negative-array-dim PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::token-info
    verible::common::text::tree-utils
    verible::verilog::CST::expression
    verible::verilog::CST::verilog-matchers
    verible::verilog::CST::verilog-nonterminals
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    verible::verilog::parser::verilog-token-enum
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::case-missing-default-rule   (静态库)   <- BUILD: cc_library(name = "case-missing-default-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__case-missing-default-rule STATIC "case-missing-default-rule.cc")
target_sources(verible__verilog__analysis__checkers__case-missing-default-rule PRIVATE "case-missing-default-rule.h")

add_library(verible::verilog::analysis::checkers::case-missing-default-rule ALIAS verible__verilog__analysis__checkers__case-missing-default-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__case-missing-default-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__case-missing-default-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::analysis::matcher::matcher-builders
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::tree-utils
    verible::verilog::CST::verilog-matchers
    verible::verilog::CST::verilog-nonterminals
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::constraint-name-style-rule   (静态库)   <- BUILD: cc_library(name = "constraint-name-style-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__constraint-name-style-rule STATIC "constraint-name-style-rule.cc")
target_sources(verible__verilog__analysis__checkers__constraint-name-style-rule PRIVATE "constraint-name-style-rule.h")

add_library(verible::verilog::analysis::checkers::constraint-name-style-rule ALIAS verible__verilog__analysis__checkers__constraint-name-style-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__constraint-name-style-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__constraint-name-style-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::config-utils
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::token-info
    verible::verilog::CST::constraints
    verible::verilog::CST::verilog-matchers
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    absl::status
    absl::strings
    re2::re2
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::disable-statement-rule   (静态库)   <- BUILD: cc_library(name = "disable-statement-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__disable-statement-rule STATIC "disable-statement-rule.cc")
target_sources(verible__verilog__analysis__checkers__disable-statement-rule PRIVATE "disable-statement-rule.h")

add_library(verible::verilog::analysis::checkers::disable-statement-rule ALIAS verible__verilog__analysis__checkers__disable-statement-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__disable-statement-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__disable-statement-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::tree-utils
    verible::common::util::iterator-adaptors
    verible::verilog::CST::identifier
    verible::verilog::CST::verilog-matchers
    verible::verilog::CST::verilog-nonterminals
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::macro-name-style-rule   (静态库)   <- BUILD: cc_library(name = "macro-name-style-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__macro-name-style-rule STATIC "macro-name-style-rule.cc")
target_sources(verible__verilog__analysis__checkers__macro-name-style-rule PRIVATE "macro-name-style-rule.h")

add_library(verible::verilog::analysis::checkers::macro-name-style-rule ALIAS verible__verilog__analysis__checkers__macro-name-style-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__macro-name-style-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__macro-name-style-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::token-stream-lint-rule
    verible::common::text::config-utils
    verible::common::text::token-info
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    verible::verilog::parser::verilog-lexer
    verible::verilog::parser::verilog-token-classifications
    verible::verilog::parser::verilog-token-enum
    absl::status
    absl::strings
    re2::re2
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::macro-string-concatenation-rule   (静态库)   <- BUILD: cc_library(name = "macro-string-concatenation-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__macro-string-concatenation-rule STATIC "macro-string-concatenation-rule.cc")
target_sources(verible__verilog__analysis__checkers__macro-string-concatenation-rule PRIVATE "macro-string-concatenation-rule.h")

add_library(verible::verilog::analysis::checkers::macro-string-concatenation-rule ALIAS verible__verilog__analysis__checkers__macro-string-concatenation-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__macro-string-concatenation-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__macro-string-concatenation-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::token-stream-lint-rule
    verible::common::text::token-info
    verible::common::util::value-saver
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    verible::verilog::parser::verilog-lexer
    verible::verilog::parser::verilog-token-classifications
    verible::verilog::parser::verilog-token-enum
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::numeric-format-string-style-rule   (静态库)   <- BUILD: cc_library(name = "numeric-format-string-style-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__numeric-format-string-style-rule STATIC "numeric-format-string-style-rule.cc")
target_sources(verible__verilog__analysis__checkers__numeric-format-string-style-rule PRIVATE "numeric-format-string-style-rule.h")

add_library(verible::verilog::analysis::checkers::numeric-format-string-style-rule ALIAS verible__verilog__analysis__checkers__numeric-format-string-style-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__numeric-format-string-style-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__numeric-format-string-style-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::token-stream-lint-rule
    verible::common::text::token-info
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    verible::verilog::parser::verilog-lexer
    verible::verilog::parser::verilog-token-classifications
    verible::verilog::parser::verilog-token-enum
    absl::strings
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::parameter-name-style-rule   (静态库)   <- BUILD: cc_library(name = "parameter-name-style-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__parameter-name-style-rule STATIC "parameter-name-style-rule.cc")
target_sources(verible__verilog__analysis__checkers__parameter-name-style-rule PRIVATE "parameter-name-style-rule.h")

add_library(verible::verilog::analysis::checkers::parameter-name-style-rule ALIAS verible__verilog__analysis__checkers__parameter-name-style-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__parameter-name-style-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__parameter-name-style-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::config-utils
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::token-info
    verible::verilog::CST::parameters
    verible::verilog::CST::verilog-matchers
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    verible::verilog::parser::verilog-token-enum
    absl::status
    absl::strings
    re2::re2
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::parameter-type-name-style-rule   (静态库)   <- BUILD: cc_library(name = "parameter-type-name-style-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__parameter-type-name-style-rule STATIC "parameter-type-name-style-rule.cc")
target_sources(verible__verilog__analysis__checkers__parameter-type-name-style-rule PRIVATE "parameter-type-name-style-rule.h")

add_library(verible::verilog::analysis::checkers::parameter-type-name-style-rule ALIAS verible__verilog__analysis__checkers__parameter-type-name-style-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__parameter-type-name-style-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__parameter-type-name-style-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::strings::naming-utils
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::token-info
    verible::verilog::CST::parameters
    verible::verilog::CST::verilog-matchers
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    absl::strings
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::proper-parameter-declaration-rule   (静态库)   <- BUILD: cc_library(name = "proper-parameter-declaration-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__proper-parameter-declaration-rule STATIC "proper-parameter-declaration-rule.cc")
target_sources(verible__verilog__analysis__checkers__proper-parameter-declaration-rule PRIVATE "proper-parameter-declaration-rule.h")

add_library(verible::verilog::analysis::checkers::proper-parameter-declaration-rule ALIAS verible__verilog__analysis__checkers__proper-parameter-declaration-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__proper-parameter-declaration-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__proper-parameter-declaration-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::config-utils
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::token-info
    verible::verilog::CST::context-functions
    verible::verilog::CST::parameters
    verible::verilog::CST::verilog-matchers
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    verible::verilog::parser::verilog-token-enum
    absl::status
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::plusarg-assignment-rule   (静态库)   <- BUILD: cc_library(name = "plusarg-assignment-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__plusarg-assignment-rule STATIC "plusarg-assignment-rule.cc")
target_sources(verible__verilog__analysis__checkers__plusarg-assignment-rule PRIVATE "plusarg-assignment-rule.h")

add_library(verible::verilog::analysis::checkers::plusarg-assignment-rule ALIAS verible__verilog__analysis__checkers__plusarg-assignment-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__plusarg-assignment-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__plusarg-assignment-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::concrete-syntax-leaf
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::token-info
    verible::verilog::CST::verilog-matchers
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    absl::strings
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::port-name-suffix-rule   (静态库)   <- BUILD: cc_library(name = "port-name-suffix-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__port-name-suffix-rule STATIC "port-name-suffix-rule.cc")
target_sources(verible__verilog__analysis__checkers__port-name-suffix-rule PRIVATE "port-name-suffix-rule.h")

add_library(verible::verilog::analysis::checkers::port-name-suffix-rule ALIAS verible__verilog__analysis__checkers__port-name-suffix-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__port-name-suffix-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__port-name-suffix-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::token-info
    verible::common::util::container-util
    verible::verilog::CST::port
    verible::verilog::CST::verilog-matchers
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    absl::die_if_null
    absl::strings
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::explicit-parameter-storage-type-rule   (静态库)   <- BUILD: cc_library(name = "explicit-parameter-storage-type-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__explicit-parameter-storage-type-rule STATIC "explicit-parameter-storage-type-rule.cc")
target_sources(verible__verilog__analysis__checkers__explicit-parameter-storage-type-rule PRIVATE "explicit-parameter-storage-type-rule.h")

add_library(verible::verilog::analysis::checkers::explicit-parameter-storage-type-rule ALIAS verible__verilog__analysis__checkers__explicit-parameter-storage-type-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__explicit-parameter-storage-type-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__explicit-parameter-storage-type-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::config-utils
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::token-info
    verible::common::text::tree-utils
    verible::common::util::logging
    verible::verilog::CST::parameters
    verible::verilog::CST::verilog-matchers
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    verible::verilog::parser::verilog-token-enum
    absl::die_if_null
    absl::status
    absl::strings
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::explicit-function-task-parameter-type-rule   (静态库)   <- BUILD: cc_library(name = "explicit-function-task-parameter-type-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__explicit-function-task-parameter-type-rule STATIC "explicit-function-task-parameter-type-rule.cc")
target_sources(verible__verilog__analysis__checkers__explicit-function-task-parameter-type-rule PRIVATE "explicit-function-task-parameter-type-rule.h")

add_library(verible::verilog::analysis::checkers::explicit-function-task-parameter-type-rule ALIAS verible__verilog__analysis__checkers__explicit-function-task-parameter-type-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__explicit-function-task-parameter-type-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__explicit-function-task-parameter-type-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::verilog::CST::port
    verible::verilog::CST::type
    verible::verilog::CST::verilog-matchers
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    absl::die_if_null
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::signal-name-style-rule   (静态库)   <- BUILD: cc_library(name = "signal-name-style-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__signal-name-style-rule STATIC "signal-name-style-rule.cc")
target_sources(verible__verilog__analysis__checkers__signal-name-style-rule PRIVATE "signal-name-style-rule.h")

add_library(verible::verilog::analysis::checkers::signal-name-style-rule ALIAS verible__verilog__analysis__checkers__signal-name-style-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__signal-name-style-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__signal-name-style-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::concrete-syntax-leaf
    verible::common::text::config-utils
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::token-info
    verible::common::util::logging
    verible::verilog::CST::data
    verible::verilog::CST::net
    verible::verilog::CST::port
    verible::verilog::CST::verilog-matchers
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    absl::die_if_null
    absl::status
    absl::strings
    re2::re2
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::struct-union-name-style-rule   (静态库)   <- BUILD: cc_library(name = "struct-union-name-style-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__struct-union-name-style-rule STATIC "struct-union-name-style-rule.cc")
target_sources(verible__verilog__analysis__checkers__struct-union-name-style-rule PRIVATE "struct-union-name-style-rule.h")

add_library(verible::verilog::analysis::checkers::struct-union-name-style-rule ALIAS verible__verilog__analysis__checkers__struct-union-name-style-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__struct-union-name-style-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__struct-union-name-style-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::config-utils
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::util::logging
    verible::verilog::CST::type
    verible::verilog::CST::verilog-matchers
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    absl::die_if_null
    absl::status
    absl::strings
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::interface-name-style-rule   (静态库)   <- BUILD: cc_library(name = "interface-name-style-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__interface-name-style-rule STATIC "interface-name-style-rule.cc")
target_sources(verible__verilog__analysis__checkers__interface-name-style-rule PRIVATE "interface-name-style-rule.h")

add_library(verible::verilog::analysis::checkers::interface-name-style-rule ALIAS verible__verilog__analysis__checkers__interface-name-style-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__interface-name-style-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__interface-name-style-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::config-utils
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::token-info
    verible::verilog::CST::module
    verible::verilog::CST::type
    verible::verilog::CST::verilog-matchers
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    absl::status
    absl::strings
    re2::re2
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::instance-shadow-rule   (静态库)   <- BUILD: cc_library(name = "instance-shadow-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__instance-shadow-rule STATIC "instance-shadow-rule.cc")
target_sources(verible__verilog__analysis__checkers__instance-shadow-rule PRIVATE "instance-shadow-rule.h")

add_library(verible::verilog::analysis::checkers::instance-shadow-rule ALIAS verible__verilog__analysis__checkers__instance-shadow-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__instance-shadow-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__instance-shadow-rule PUBLIC
    verible::common::analysis::citation
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::syntax-tree-search
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::tree-utils
    verible::common::util::iterator-adaptors
    verible::verilog::CST::identifier
    verible::verilog::CST::verilog-matchers
    verible::verilog::CST::verilog-nonterminals
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::legacy-genvar-declaration-rule   (静态库)   <- BUILD: cc_library(name = "legacy-genvar-declaration-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__legacy-genvar-declaration-rule STATIC "legacy-genvar-declaration-rule.cc")
target_sources(verible__verilog__analysis__checkers__legacy-genvar-declaration-rule PRIVATE "legacy-genvar-declaration-rule.h")

add_library(verible::verilog::analysis::checkers::legacy-genvar-declaration-rule ALIAS verible__verilog__analysis__checkers__legacy-genvar-declaration-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__legacy-genvar-declaration-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__legacy-genvar-declaration-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::tree-utils
    verible::verilog::CST::identifier
    verible::verilog::CST::verilog-nonterminals
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::legacy-generate-region-rule   (静态库)   <- BUILD: cc_library(name = "legacy-generate-region-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__legacy-generate-region-rule STATIC "legacy-generate-region-rule.cc")
target_sources(verible__verilog__analysis__checkers__legacy-generate-region-rule PRIVATE "legacy-generate-region-rule.h")

add_library(verible::verilog::analysis::checkers::legacy-generate-region-rule ALIAS verible__verilog__analysis__checkers__legacy-generate-region-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__legacy-generate-region-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__legacy-generate-region-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher-builders
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::tree-utils
    verible::verilog::CST::verilog-nonterminals
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    verible::verilog::parser::verilog-token-enum
    absl::die_if_null
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::uvm-macro-semicolon-rule   (静态库)   <- BUILD: cc_library(name = "uvm-macro-semicolon-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__uvm-macro-semicolon-rule STATIC "uvm-macro-semicolon-rule.cc")
target_sources(verible__verilog__analysis__checkers__uvm-macro-semicolon-rule PRIVATE "uvm-macro-semicolon-rule.h")

add_library(verible::verilog::analysis::checkers::uvm-macro-semicolon-rule ALIAS verible__verilog__analysis__checkers__uvm-macro-semicolon-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__uvm-macro-semicolon-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__uvm-macro-semicolon-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::text::concrete-syntax-leaf
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::token-info
    verible::verilog::CST::context-functions
    verible::verilog::CST::macro
    verible::verilog::CST::verilog-nonterminals
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    verible::verilog::parser::verilog-token-enum
    absl::strings
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::suspicious-semicolon-rule   (静态库)   <- BUILD: cc_library(name = "suspicious-semicolon-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__suspicious-semicolon-rule STATIC "suspicious-semicolon-rule.cc")
target_sources(verible__verilog__analysis__checkers__suspicious-semicolon-rule PRIVATE "suspicious-semicolon-rule.h")

add_library(verible::verilog::analysis::checkers::suspicious-semicolon-rule ALIAS verible__verilog__analysis__checkers__suspicious-semicolon-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__suspicious-semicolon-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__suspicious-semicolon-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::concrete-syntax-tree
    verible::common::text::syntax-tree-context
    verible::common::text::tree-utils
    verible::verilog::CST::verilog-matchers
    verible::verilog::CST::verilog-nonterminals
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）

# ---------------------------------------------------------------------------
# verible::verilog::analysis::checkers::dff-name-style-rule   (静态库)   <- BUILD: cc_library(name = "dff-name-style-rule")
# ---------------------------------------------------------------------------
add_library(verible__verilog__analysis__checkers__dff-name-style-rule STATIC "dff-name-style-rule.cc")
target_sources(verible__verilog__analysis__checkers__dff-name-style-rule PRIVATE "dff-name-style-rule.h")

add_library(verible::verilog::analysis::checkers::dff-name-style-rule ALIAS verible__verilog__analysis__checkers__dff-name-style-rule)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__analysis__checkers__dff-name-style-rule PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__analysis__checkers__dff-name-style-rule PUBLIC
    verible::common::analysis::lint-rule-status
    verible::common::analysis::syntax-tree-lint-rule
    verible::common::analysis::matcher::matcher
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::analysis::matcher::core-matchers
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::config-utils
    verible::common::text::symbol
    verible::common::text::syntax-tree-context
    verible::common::text::token-info
    verible::common::text::tree-utils
    verible::verilog::CST::expression
    verible::verilog::CST::statement
    verible::verilog::CST::verilog-matchers
    verible::verilog::CST::verilog-nonterminals
    verible::verilog::analysis::descriptions
    verible::verilog::analysis::lint-rule-registry
    verible::verilog::parser::verilog-token-enum
    absl::status
    absl::strings
    re2::re2
)

# BUILD: alwayslink = 1
#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。
#   （verible_parser_libs 聚合 target 里已统一处理）
