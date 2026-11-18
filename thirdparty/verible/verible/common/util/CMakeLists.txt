# =============================================================================
#  GENERATED FILE - DO NOT EDIT
# =============================================================================
#  由 tools/gen_verible_cmake.py 从同目录的 Bazel BUILD 自动转译而来。
#  手工修改会在下次运行生成器时被覆盖；要改行为请改生成器本身。
#
#  源包: //verible/common/util
#
#  目录里也没有 BUILD 生成物时，本文件只声明库，不在这里编测试
#  （阶段 0 不编 Verible 自带的 225 个 cc_test）。
# =============================================================================
# ---------------------------------------------------------------------------
# BUILD 里的 genrule（Bazel 的 sed/grep 在 Windows 上不可用，改用 cmake -P）
# ---------------------------------------------------------------------------
# genrule(version-header) -> generated-verible-build-version.h
#   Bazel 靠 workspace status 生成版本头；这里写最小实现，
#   版本号取自 thirdparty/verible/VENDOR-INFO.md 的 commit。
#   目标：verible__common__util__build-version（纯头文件）

# ---------------------------------------------------------------------------
# 本包有 28 个 cc_test —— 按 CLAUDE 约定不编译 Verible 自带测试；
# 需要时用 `--with-tests` 重新生成。
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# verible::common::util::algorithm   (纯头文件库)   <- BUILD: cc_library(name = "algorithm")
# ---------------------------------------------------------------------------
add_library(verible__common__util__algorithm INTERFACE)
target_sources(verible__common__util__algorithm INTERFACE "algorithm.h")

add_library(verible::common::util::algorithm ALIAS verible__common__util__algorithm)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__util__algorithm INTERFACE verible_common_opts)

# ---------------------------------------------------------------------------
# verible::common::util::auto-pop-stack   (纯头文件库)   <- BUILD: cc_library(name = "auto-pop-stack")
# ---------------------------------------------------------------------------
add_library(verible__common__util__auto-pop-stack INTERFACE)
target_sources(verible__common__util__auto-pop-stack INTERFACE "auto-pop-stack.h")

add_library(verible::common::util::auto-pop-stack ALIAS verible__common__util__auto-pop-stack)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__util__auto-pop-stack INTERFACE verible_common_opts)

target_link_libraries(verible__common__util__auto-pop-stack INTERFACE
    verible::common::util::logging
)

# ---------------------------------------------------------------------------
# verible::common::util::forward   (纯头文件库)   <- BUILD: cc_library(name = "forward")
# ---------------------------------------------------------------------------
add_library(verible__common__util__forward INTERFACE)
target_sources(verible__common__util__forward INTERFACE "forward.h")

add_library(verible::common::util::forward ALIAS verible__common__util__forward)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__util__forward INTERFACE verible_common_opts)

# ---------------------------------------------------------------------------
# verible::common::util::range   (纯头文件库)   <- BUILD: cc_library(name = "range")
# ---------------------------------------------------------------------------
add_library(verible__common__util__range INTERFACE)
target_sources(verible__common__util__range INTERFACE "range.h")

add_library(verible::common::util::range ALIAS verible__common__util__range)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__util__range INTERFACE verible_common_opts)

target_link_libraries(verible__common__util__range INTERFACE
    verible::common::util::logging
)

# ---------------------------------------------------------------------------
# verible::common::util::status-macros   (纯头文件库)   <- BUILD: cc_library(name = "status-macros")
# ---------------------------------------------------------------------------
add_library(verible__common__util__status-macros INTERFACE)
target_sources(verible__common__util__status-macros INTERFACE "status-macros.h")

add_library(verible::common::util::status-macros ALIAS verible__common__util__status-macros)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__util__status-macros INTERFACE verible_common_opts)

target_link_libraries(verible__common__util__status-macros INTERFACE
    absl::core_headers
)

# ---------------------------------------------------------------------------
# verible::common::util::subcommand   (静态库)   <- BUILD: cc_library(name = "subcommand")
# ---------------------------------------------------------------------------
add_library(verible__common__util__subcommand STATIC "subcommand.cc")
target_sources(verible__common__util__subcommand PRIVATE "subcommand.h")

add_library(verible::common::util::subcommand ALIAS verible__common__util__subcommand)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__util__subcommand PRIVATE verible_common_opts)

target_link_libraries(verible__common__util__subcommand PUBLIC
    verible::common::util::container-iterator-range
    verible::common::strings::compare
    absl::status
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::common::util::thread-pool   (静态库)   <- BUILD: cc_library(name = "thread-pool")
# ---------------------------------------------------------------------------
add_library(verible__common__util__thread-pool STATIC "thread-pool.cc")
target_sources(verible__common__util__thread-pool PRIVATE "thread-pool.h")

add_library(verible::common::util::thread-pool ALIAS verible__common__util__thread-pool)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__util__thread-pool PRIVATE verible_common_opts)

# ---------------------------------------------------------------------------
# verible::common::util::file-util   (静态库)   <- BUILD: cc_library(name = "file-util")
# ---------------------------------------------------------------------------
add_library(verible__common__util__file-util STATIC "file-util.cc")
target_sources(verible__common__util__file-util PRIVATE "file-util.h")

add_library(verible::common::util::file-util ALIAS verible__common__util__file-util)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__util__file-util PRIVATE verible_common_opts)

target_link_libraries(verible__common__util__file-util PUBLIC
    verible::common::util::logging
    verible::common::strings::mem-block
    absl::status
    absl::statusor
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::common::util::build-version   (纯头文件库)   <- BUILD: cc_library(name = "build-version")
# ---------------------------------------------------------------------------
add_library(verible__common__util__build-version INTERFACE)

# 构建期生成的文件（configure 时还不存在，由 cmake/Verible.cmake 挂接）：
set_property(TARGET verible__common__util__build-version PROPERTY verible_generated_headers "generated-verible-build-version.h")

add_library(verible::common::util::build-version ALIAS verible__common__util__build-version)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__util__build-version INTERFACE verible_common_opts)

# ---------------------------------------------------------------------------
# verible::common::util::init-command-line   (静态库)   <- BUILD: cc_library(name = "init-command-line")
# ---------------------------------------------------------------------------
add_library(verible__common__util__init-command-line STATIC "init-command-line.cc")
target_sources(verible__common__util__init-command-line PRIVATE "init-command-line.h")

add_library(verible::common::util::init-command-line ALIAS verible__common__util__init-command-line)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__util__init-command-line PRIVATE verible_common_opts)

target_link_libraries(verible__common__util__init-command-line PUBLIC
    verible::common::util::build-version
    absl::failure_signal_handler
    absl::symbolize
    absl::flags_config
    absl::flags_parse
    absl::flags_usage
    absl::log_globals
    absl::log_severity
    absl::log_initialize
    absl::strings
    absl::time
)

# ---------------------------------------------------------------------------
# verible::common::util::interval   (纯头文件库)   <- BUILD: cc_library(name = "interval")
# ---------------------------------------------------------------------------
add_library(verible__common__util__interval INTERFACE)
target_sources(verible__common__util__interval INTERFACE "interval.h")

add_library(verible::common::util::interval ALIAS verible__common__util__interval)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__util__interval INTERFACE verible_common_opts)

target_link_libraries(verible__common__util__interval INTERFACE
    verible::common::util::forward
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::common::util::interval-map   (纯头文件库)   <- BUILD: cc_library(name = "interval-map")
# ---------------------------------------------------------------------------
add_library(verible__common__util__interval-map INTERFACE)
target_sources(verible__common__util__interval-map INTERFACE "interval-map.h")

add_library(verible::common::util::interval-map ALIAS verible__common__util__interval-map)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__util__interval-map INTERFACE verible_common_opts)

target_link_libraries(verible__common__util__interval-map INTERFACE
    verible::common::util::auto-iterator
    verible::common::util::logging
)

# ---------------------------------------------------------------------------
# verible::common::util::interval-set   (纯头文件库)   <- BUILD: cc_library(name = "interval-set")
# ---------------------------------------------------------------------------
add_library(verible__common__util__interval-set INTERFACE)
target_sources(verible__common__util__interval-set INTERFACE "interval-set.h")

add_library(verible::common::util::interval-set ALIAS verible__common__util__interval-set)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__util__interval-set INTERFACE verible_common_opts)

target_link_libraries(verible__common__util__interval-set INTERFACE
    verible::common::util::auto-iterator
    verible::common::util::interval
    verible::common::util::iterator-range
    verible::common::util::logging
    absl::random_random
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::common::util::logging   (纯头文件库)   <- BUILD: cc_library(name = "logging")
# ---------------------------------------------------------------------------
add_library(verible__common__util__logging INTERFACE)
target_sources(verible__common__util__logging INTERFACE "logging.h")

add_library(verible::common::util::logging ALIAS verible__common__util__logging)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__util__logging INTERFACE verible_common_opts)

target_link_libraries(verible__common__util__logging INTERFACE
    absl::log
    absl::check
    absl::die_if_null
)

# ---------------------------------------------------------------------------
# verible::common::util::casts   (纯头文件库)   <- BUILD: cc_library(name = "casts")
# ---------------------------------------------------------------------------
add_library(verible__common__util__casts INTERFACE)
target_sources(verible__common__util__casts INTERFACE "casts.h")

add_library(verible::common::util::casts ALIAS verible__common__util__casts)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__util__casts INTERFACE verible_common_opts)

# ---------------------------------------------------------------------------
# verible::common::util::container-util   (纯头文件库)   <- BUILD: cc_library(name = "container-util")
# ---------------------------------------------------------------------------
add_library(verible__common__util__container-util INTERFACE)
target_sources(verible__common__util__container-util INTERFACE "container-util.h")

add_library(verible::common::util::container-util ALIAS verible__common__util__container-util)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__util__container-util INTERFACE verible_common_opts)

target_link_libraries(verible__common__util__container-util INTERFACE
    verible::common::util::logging
)

# ---------------------------------------------------------------------------
# verible::common::util::container-iterator-range   (纯头文件库)   <- BUILD: cc_library(name = "container-iterator-range")
# ---------------------------------------------------------------------------
add_library(verible__common__util__container-iterator-range INTERFACE)
target_sources(verible__common__util__container-iterator-range INTERFACE "container-iterator-range.h")

add_library(verible::common::util::container-iterator-range ALIAS verible__common__util__container-iterator-range)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__util__container-iterator-range INTERFACE verible_common_opts)

target_link_libraries(verible__common__util__container-iterator-range INTERFACE
    verible::common::util::iterator-range
    verible::common::util::range
)

# ---------------------------------------------------------------------------
# verible::common::util::type-traits   (纯头文件库)   <- BUILD: cc_library(name = "type-traits")
# ---------------------------------------------------------------------------
add_library(verible__common__util__type-traits INTERFACE)
target_sources(verible__common__util__type-traits INTERFACE "type-traits.h")

add_library(verible::common::util::type-traits ALIAS verible__common__util__type-traits)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__util__type-traits INTERFACE verible_common_opts)

# ---------------------------------------------------------------------------
# verible::common::util::auto-iterator   (纯头文件库)   <- BUILD: cc_library(name = "auto-iterator")
# ---------------------------------------------------------------------------
add_library(verible__common__util__auto-iterator INTERFACE)
target_sources(verible__common__util__auto-iterator INTERFACE "auto-iterator.h")

add_library(verible::common::util::auto-iterator ALIAS verible__common__util__auto-iterator)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__util__auto-iterator INTERFACE verible_common_opts)

# ---------------------------------------------------------------------------
# verible::common::util::iterator-adaptors   (纯头文件库)   <- BUILD: cc_library(name = "iterator-adaptors")
# ---------------------------------------------------------------------------
add_library(verible__common__util__iterator-adaptors INTERFACE)
target_sources(verible__common__util__iterator-adaptors INTERFACE "iterator-adaptors.h")

add_library(verible::common::util::iterator-adaptors ALIAS verible__common__util__iterator-adaptors)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__util__iterator-adaptors INTERFACE verible_common_opts)

target_link_libraries(verible__common__util__iterator-adaptors INTERFACE
    verible::common::util::auto-iterator
    verible::common::util::iterator-range
)

# ---------------------------------------------------------------------------
# verible::common::util::iterator-range   (纯头文件库)   <- BUILD: cc_library(name = "iterator-range")
# ---------------------------------------------------------------------------
add_library(verible__common__util__iterator-range INTERFACE)
target_sources(verible__common__util__iterator-range INTERFACE "iterator-range.h")

add_library(verible::common::util::iterator-range ALIAS verible__common__util__iterator-range)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__util__iterator-range INTERFACE verible_common_opts)

# ---------------------------------------------------------------------------
# verible::common::util::spacer   (静态库)   <- BUILD: cc_library(name = "spacer")
# ---------------------------------------------------------------------------
add_library(verible__common__util__spacer STATIC "spacer.cc")
target_sources(verible__common__util__spacer PRIVATE "spacer.h")

add_library(verible::common::util::spacer ALIAS verible__common__util__spacer)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__util__spacer PRIVATE verible_common_opts)

# ---------------------------------------------------------------------------
# verible::common::util::top-n   (纯头文件库)   <- BUILD: cc_library(name = "top-n")
# ---------------------------------------------------------------------------
add_library(verible__common__util__top-n INTERFACE)
target_sources(verible__common__util__top-n INTERFACE "top-n.h")

add_library(verible::common::util::top-n ALIAS verible__common__util__top-n)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__util__top-n INTERFACE verible_common_opts)

# ---------------------------------------------------------------------------
# verible::common::util::value-saver   (纯头文件库)   <- BUILD: cc_library(name = "value-saver")
# ---------------------------------------------------------------------------
add_library(verible__common__util__value-saver INTERFACE)
target_sources(verible__common__util__value-saver INTERFACE "value-saver.h")

add_library(verible::common::util::value-saver ALIAS verible__common__util__value-saver)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__util__value-saver INTERFACE verible_common_opts)

# ---------------------------------------------------------------------------
# verible::common::util::enum-flags   (纯头文件库)   <- BUILD: cc_library(name = "enum-flags")
# ---------------------------------------------------------------------------
add_library(verible__common__util__enum-flags INTERFACE)
target_sources(verible__common__util__enum-flags INTERFACE "enum-flags.h")

add_library(verible::common::util::enum-flags ALIAS verible__common__util__enum-flags)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__util__enum-flags INTERFACE verible_common_opts)

target_link_libraries(verible__common__util__enum-flags INTERFACE
    verible::common::util::bijective-map
    verible::common::strings::compare
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::common::util::bijective-map   (纯头文件库)   <- BUILD: cc_library(name = "bijective-map")
# ---------------------------------------------------------------------------
add_library(verible__common__util__bijective-map INTERFACE)
target_sources(verible__common__util__bijective-map INTERFACE "bijective-map.h")

add_library(verible::common::util::bijective-map ALIAS verible__common__util__bijective-map)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__util__bijective-map INTERFACE verible_common_opts)

target_link_libraries(verible__common__util__bijective-map INTERFACE
    verible::common::util::logging
)

# ---------------------------------------------------------------------------
# verible::common::util::vector-tree   (纯头文件库)   <- BUILD: cc_library(name = "vector-tree")
# ---------------------------------------------------------------------------
add_library(verible__common__util__vector-tree INTERFACE)
target_sources(verible__common__util__vector-tree INTERFACE "vector-tree.h")

add_library(verible::common::util::vector-tree ALIAS verible__common__util__vector-tree)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__util__vector-tree INTERFACE verible_common_opts)

target_link_libraries(verible__common__util__vector-tree INTERFACE
    verible::common::util::container-proxy
    verible::common::util::iterator-range
    verible::common::util::logging
)

# ---------------------------------------------------------------------------
# verible::common::util::tree-operations   (静态库)   <- BUILD: cc_library(name = "tree-operations")
# ---------------------------------------------------------------------------
add_library(verible__common__util__tree-operations STATIC "tree-operations.cc")
target_sources(verible__common__util__tree-operations PRIVATE "tree-operations.h")

add_library(verible::common::util::tree-operations ALIAS verible__common__util__tree-operations)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__util__tree-operations PRIVATE verible_common_opts)

target_link_libraries(verible__common__util__tree-operations PUBLIC
    verible::common::util::logging
    verible::common::util::spacer
    verible::common::util::type-traits
    absl::die_if_null
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::common::util::expandable-tree-view   (纯头文件库)   <- BUILD: cc_library(name = "expandable-tree-view")
# ---------------------------------------------------------------------------
add_library(verible__common__util__expandable-tree-view INTERFACE)
target_sources(verible__common__util__expandable-tree-view INTERFACE "expandable-tree-view.h")

add_library(verible::common::util::expandable-tree-view ALIAS verible__common__util__expandable-tree-view)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__util__expandable-tree-view INTERFACE verible_common_opts)

target_link_libraries(verible__common__util__expandable-tree-view INTERFACE
    verible::common::util::logging
    verible::common::util::tree-operations
    verible::common::util::vector-tree
)

# ---------------------------------------------------------------------------
# verible::common::util::vector-tree-test-util   (静态库)   <- BUILD: cc_library(name = "vector-tree-test-util")
# ---------------------------------------------------------------------------
add_library(verible__common__util__vector-tree-test-util STATIC "vector-tree-test-util.cc")
target_sources(verible__common__util__vector-tree-test-util PRIVATE "vector-tree-test-util.h")

add_library(verible::common::util::vector-tree-test-util ALIAS verible__common__util__vector-tree-test-util)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__util__vector-tree-test-util PRIVATE verible_common_opts)

target_link_libraries(verible__common__util__vector-tree-test-util PUBLIC
    verible::common::util::tree-operations
    verible::common::util::vector-tree
    GTest::gtest
)

# BUILD: testonly = 1（本阶段不编译测试，库照常生成以便被引用）

# ---------------------------------------------------------------------------
# verible::common::util::vector-tree-iterators   (纯头文件库)   <- BUILD: cc_library(name = "vector-tree-iterators")
# ---------------------------------------------------------------------------
add_library(verible__common__util__vector-tree-iterators INTERFACE)
target_sources(verible__common__util__vector-tree-iterators INTERFACE "vector-tree-iterators.h")

add_library(verible::common::util::vector-tree-iterators ALIAS verible__common__util__vector-tree-iterators)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__util__vector-tree-iterators INTERFACE verible_common_opts)

target_link_libraries(verible__common__util__vector-tree-iterators INTERFACE
    verible::common::util::iterator-range
    verible::common::util::logging
    verible::common::util::tree-operations
)

# ---------------------------------------------------------------------------
# verible::common::util::map-tree   (纯头文件库)   <- BUILD: cc_library(name = "map-tree")
# ---------------------------------------------------------------------------
add_library(verible__common__util__map-tree INTERFACE)
target_sources(verible__common__util__map-tree INTERFACE "map-tree.h")

add_library(verible::common::util::map-tree ALIAS verible__common__util__map-tree)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__util__map-tree INTERFACE verible_common_opts)

target_link_libraries(verible__common__util__map-tree INTERFACE
    verible::common::util::logging
    verible::common::util::spacer
)

# ---------------------------------------------------------------------------
# verible::common::util::with-reason   (纯头文件库)   <- BUILD: cc_library(name = "with-reason")
# ---------------------------------------------------------------------------
add_library(verible__common__util__with-reason INTERFACE)
target_sources(verible__common__util__with-reason INTERFACE "with-reason.h")

add_library(verible::common::util::with-reason ALIAS verible__common__util__with-reason)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__util__with-reason INTERFACE verible_common_opts)

# ---------------------------------------------------------------------------
# verible::common::util::user-interaction   (静态库)   <- BUILD: cc_library(name = "user-interaction")
# ---------------------------------------------------------------------------
add_library(verible__common__util__user-interaction STATIC "user-interaction.cc")
target_sources(verible__common__util__user-interaction PRIVATE "user-interaction.h")

add_library(verible::common::util::user-interaction ALIAS verible__common__util__user-interaction)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__util__user-interaction PRIVATE verible_common_opts)

# ---------------------------------------------------------------------------
# verible::common::util::container-proxy   (纯头文件库)   <- BUILD: cc_library(name = "container-proxy")
# ---------------------------------------------------------------------------
add_library(verible__common__util__container-proxy INTERFACE)
target_sources(verible__common__util__container-proxy INTERFACE "container-proxy.h")

add_library(verible::common::util::container-proxy ALIAS verible__common__util__container-proxy)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__util__container-proxy INTERFACE verible_common_opts)

# ---------------------------------------------------------------------------
# verible::common::util::sha256   (静态库)   <- BUILD: cc_library(name = "sha256")
# ---------------------------------------------------------------------------
add_library(verible__common__util__sha256 STATIC "sha256.cc")
target_sources(verible__common__util__sha256 PRIVATE "sha256.h")

add_library(verible::common::util::sha256 ALIAS verible__common__util__sha256)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__util__sha256 PRIVATE verible_common_opts)

target_link_libraries(verible__common__util__sha256 PUBLIC
    absl::strings
)
