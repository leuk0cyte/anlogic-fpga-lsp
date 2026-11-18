# =============================================================================
#  GENERATED FILE - DO NOT EDIT
# =============================================================================
#  由 tools/gen_verible_cmake.py 从同目录的 Bazel BUILD 自动转译而来。
#  手工修改会在下次运行生成器时被覆盖；要改行为请改生成器本身。
#
#  源包: //verible/common/analysis/matcher
#
#  目录里也没有 BUILD 生成物时，本文件只声明库，不在这里编测试
#  （阶段 0 不编 Verible 自带的 225 个 cc_test）。
# =============================================================================
# ---------------------------------------------------------------------------
# 本包有 4 个 cc_test —— 按 CLAUDE 约定不编译 Verible 自带测试；
# 需要时用 `--with-tests` 重新生成。
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# verible::common::analysis::matcher::bound-symbol-manager   (静态库)   <- BUILD: cc_library(name = "bound-symbol-manager")
# ---------------------------------------------------------------------------
add_library(verible__common__analysis__matcher__bound-symbol-manager STATIC "bound-symbol-manager.cc")
target_sources(verible__common__analysis__matcher__bound-symbol-manager PRIVATE "bound-symbol-manager.h")

add_library(verible::common::analysis::matcher::bound-symbol-manager ALIAS verible__common__analysis__matcher__bound-symbol-manager)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__analysis__matcher__bound-symbol-manager PRIVATE verible_common_opts)

target_link_libraries(verible__common__analysis__matcher__bound-symbol-manager PUBLIC
    verible::common::text::symbol
    verible::common::text::tree-utils
    verible::common::util::container-util
    absl::die_if_null
)

# ---------------------------------------------------------------------------
# verible::common::analysis::matcher::matcher   (静态库)   <- BUILD: cc_library(name = "matcher")
# ---------------------------------------------------------------------------
add_library(verible__common__analysis__matcher__matcher STATIC "matcher.cc")
target_sources(verible__common__analysis__matcher__matcher PRIVATE "matcher.h")

add_library(verible::common::analysis::matcher::matcher ALIAS verible__common__analysis__matcher__matcher)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__analysis__matcher__matcher PRIVATE verible_common_opts)

target_link_libraries(verible__common__analysis__matcher__matcher PUBLIC
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::text::symbol
)

# ---------------------------------------------------------------------------
# verible::common::analysis::matcher::core-matchers   (纯头文件库)   <- BUILD: cc_library(name = "core-matchers")
# ---------------------------------------------------------------------------
add_library(verible__common__analysis__matcher__core-matchers INTERFACE)
target_sources(verible__common__analysis__matcher__core-matchers INTERFACE "core-matchers.h")

add_library(verible::common::analysis::matcher::core-matchers ALIAS verible__common__analysis__matcher__core-matchers)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__analysis__matcher__core-matchers INTERFACE verible_common_opts)

target_link_libraries(verible__common__analysis__matcher__core-matchers INTERFACE
    verible::common::analysis::matcher::inner-match-handlers
    verible::common::analysis::matcher::matcher
    verible::common::text::symbol
)

# ---------------------------------------------------------------------------
# verible::common::analysis::matcher::inner-match-handlers   (静态库)   <- BUILD: cc_library(name = "inner-match-handlers")
# ---------------------------------------------------------------------------
add_library(verible__common__analysis__matcher__inner-match-handlers STATIC "inner-match-handlers.cc")
target_sources(verible__common__analysis__matcher__inner-match-handlers PRIVATE "inner-match-handlers.h")

add_library(verible::common::analysis::matcher::inner-match-handlers ALIAS verible__common__analysis__matcher__inner-match-handlers)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__analysis__matcher__inner-match-handlers PRIVATE verible_common_opts)

target_link_libraries(verible__common__analysis__matcher__inner-match-handlers PUBLIC
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::analysis::matcher::matcher
    verible::common::text::symbol
    verible::common::util::logging
)

# ---------------------------------------------------------------------------
# verible::common::analysis::matcher::matcher-builders   (纯头文件库)   <- BUILD: cc_library(name = "matcher-builders")
# ---------------------------------------------------------------------------
add_library(verible__common__analysis__matcher__matcher-builders INTERFACE)
target_sources(verible__common__analysis__matcher__matcher-builders INTERFACE "matcher-builders.h")

add_library(verible::common::analysis::matcher::matcher-builders ALIAS verible__common__analysis__matcher__matcher-builders)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__analysis__matcher__matcher-builders INTERFACE verible_common_opts)

target_link_libraries(verible__common__analysis__matcher__matcher-builders INTERFACE
    verible::common::analysis::matcher::descent-path
    verible::common::analysis::matcher::inner-match-handlers
    verible::common::analysis::matcher::matcher
    verible::common::text::symbol
)

# ---------------------------------------------------------------------------
# verible::common::analysis::matcher::descent-path   (静态库)   <- BUILD: cc_library(name = "descent-path")
# ---------------------------------------------------------------------------
add_library(verible__common__analysis__matcher__descent-path STATIC "descent-path.cc")
target_sources(verible__common__analysis__matcher__descent-path PRIVATE "descent-path.h")

add_library(verible::common::analysis::matcher::descent-path ALIAS verible__common__analysis__matcher__descent-path)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__analysis__matcher__descent-path PRIVATE verible_common_opts)

target_link_libraries(verible__common__analysis__matcher__descent-path PUBLIC
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::util::casts
)

# ---------------------------------------------------------------------------
# verible::common::analysis::matcher::matcher-test-utils   (静态库)   <- BUILD: cc_library(name = "matcher-test-utils")
# ---------------------------------------------------------------------------
add_library(verible__common__analysis__matcher__matcher-test-utils STATIC "matcher-test-utils.cc")
target_sources(verible__common__analysis__matcher__matcher-test-utils PRIVATE "matcher-test-utils.h")

add_library(verible::common::analysis::matcher::matcher-test-utils ALIAS verible__common__analysis__matcher__matcher-test-utils)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__analysis__matcher__matcher-test-utils PRIVATE verible_common_opts)

target_link_libraries(verible__common__analysis__matcher__matcher-test-utils PUBLIC
    verible::common::analysis::matcher::bound-symbol-manager
    verible::common::analysis::matcher::matcher
    verible::common::text::concrete-syntax-leaf
    verible::common::text::concrete-syntax-tree
    verible::common::text::symbol
    verible::common::text::tree-utils
    verible::common::text::visitors
    absl::status
    GTest::gtest
)

# BUILD: testonly = 1（本阶段不编译测试，库照常生成以便被引用）
