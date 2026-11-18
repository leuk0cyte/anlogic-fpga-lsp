# =============================================================================
#  GENERATED FILE - DO NOT EDIT
# =============================================================================
#  由 tools/gen_verible_cmake.py 从同目录的 Bazel BUILD 自动转译而来。
#  手工修改会在下次运行生成器时被覆盖；要改行为请改生成器本身。
#
#  源包: //verible/common/strings
#
#  目录里也没有 BUILD 生成物时，本文件只声明库，不在这里编测试
#  （阶段 0 不编 Verible 自带的 225 个 cc_test）。
# =============================================================================
# ---------------------------------------------------------------------------
# 本包有 15 个 cc_test —— 按 CLAUDE 约定不编译 Verible 自带测试；
# 需要时用 `--with-tests` 重新生成。
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# verible::common::strings::compare   (纯头文件库)   <- BUILD: cc_library(name = "compare")
# ---------------------------------------------------------------------------
add_library(verible__common__strings__compare INTERFACE)
target_sources(verible__common__strings__compare INTERFACE "compare.h")

add_library(verible::common::strings::compare ALIAS verible__common__strings__compare)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__strings__compare INTERFACE verible_common_opts)

# ---------------------------------------------------------------------------
# verible::common::strings::comment-utils   (静态库)   <- BUILD: cc_library(name = "comment-utils")
# ---------------------------------------------------------------------------
add_library(verible__common__strings__comment-utils STATIC "comment-utils.cc")
target_sources(verible__common__strings__comment-utils PRIVATE "comment-utils.h")

add_library(verible::common::strings::comment-utils ALIAS verible__common__strings__comment-utils)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__strings__comment-utils PRIVATE verible_common_opts)

target_link_libraries(verible__common__strings__comment-utils PUBLIC
    verible::common::util::logging
    verible::common::util::range
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::common::strings::diff   (静态库)   <- BUILD: cc_library(name = "diff")
# ---------------------------------------------------------------------------
add_library(verible__common__strings__diff STATIC "diff.cc")
target_sources(verible__common__strings__diff PRIVATE "diff.h")

add_library(verible::common::strings::diff ALIAS verible__common__strings__diff)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__strings__diff PRIVATE verible_common_opts)

target_link_libraries(verible__common__strings__diff PUBLIC
    verible::common::strings::position
    verible::common::strings::split
    verible::external_libs::editscript
    verible::common::util::iterator-range
)

# ---------------------------------------------------------------------------
# verible::common::strings::display-utils   (静态库)   <- BUILD: cc_library(name = "display-utils")
# ---------------------------------------------------------------------------
add_library(verible__common__strings__display-utils STATIC "display-utils.cc")
target_sources(verible__common__strings__display-utils PRIVATE "display-utils.h")

add_library(verible::common::strings::display-utils ALIAS verible__common__strings__display-utils)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__strings__display-utils PRIVATE verible_common_opts)

target_link_libraries(verible__common__strings__display-utils PUBLIC
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::common::strings::obfuscator   (静态库)   <- BUILD: cc_library(name = "obfuscator")
# ---------------------------------------------------------------------------
add_library(verible__common__strings__obfuscator STATIC "obfuscator.cc")
target_sources(verible__common__strings__obfuscator PRIVATE "obfuscator.h")

add_library(verible::common::strings::obfuscator ALIAS verible__common__strings__obfuscator)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__strings__obfuscator PRIVATE verible_common_opts)

target_link_libraries(verible__common__strings__obfuscator PUBLIC
    verible::common::strings::compare
    verible::common::util::bijective-map
    verible::common::util::logging
    absl::status
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::common::strings::mem-block   (纯头文件库)   <- BUILD: cc_library(name = "mem-block")
# ---------------------------------------------------------------------------
add_library(verible__common__strings__mem-block INTERFACE)
target_sources(verible__common__strings__mem-block INTERFACE "mem-block.h")

add_library(verible::common::strings::mem-block ALIAS verible__common__strings__mem-block)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__strings__mem-block INTERFACE verible_common_opts)

# ---------------------------------------------------------------------------
# verible::common::strings::naming-utils   (静态库)   <- BUILD: cc_library(name = "naming-utils")
# ---------------------------------------------------------------------------
add_library(verible__common__strings__naming-utils STATIC "naming-utils.cc")
target_sources(verible__common__strings__naming-utils PRIVATE "naming-utils.h")

add_library(verible::common::strings::naming-utils ALIAS verible__common__strings__naming-utils)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__strings__naming-utils PRIVATE verible_common_opts)

target_link_libraries(verible__common__strings__naming-utils PUBLIC
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::common::strings::patch   (静态库)   <- BUILD: cc_library(name = "patch")
# ---------------------------------------------------------------------------
add_library(verible__common__strings__patch STATIC "patch.cc")
target_sources(verible__common__strings__patch PRIVATE "patch.h")

add_library(verible::common::strings::patch ALIAS verible__common__strings__patch)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__strings__patch PRIVATE verible_common_opts)

target_link_libraries(verible__common__strings__patch PUBLIC
    verible::common::strings::compare
    verible::common::strings::position
    verible::common::strings::split
    verible::common::util::algorithm
    verible::common::util::container-iterator-range
    verible::common::util::file-util
    verible::common::util::iterator-adaptors
    verible::common::util::iterator-range
    verible::common::util::logging
    verible::common::util::status-macros
    verible::common::util::user-interaction
    absl::core_headers
    absl::status
    absl::statusor
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::common::strings::position   (静态库)   <- BUILD: cc_library(name = "position")
# ---------------------------------------------------------------------------
add_library(verible__common__strings__position STATIC "position.cc")
target_sources(verible__common__strings__position PRIVATE "position.h")

add_library(verible::common::strings::position ALIAS verible__common__strings__position)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__strings__position PRIVATE verible_common_opts)

target_link_libraries(verible__common__strings__position PUBLIC
    verible::common::util::interval
    verible::common::util::interval-set
)

# ---------------------------------------------------------------------------
# verible::common::strings::random   (静态库)   <- BUILD: cc_library(name = "random")
# ---------------------------------------------------------------------------
add_library(verible__common__strings__random STATIC "random.cc")
target_sources(verible__common__strings__random PRIVATE "random.h")

add_library(verible::common::strings::random ALIAS verible__common__strings__random)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__strings__random PRIVATE verible_common_opts)

target_link_libraries(verible__common__strings__random PUBLIC
    verible::common::util::interval-set
    verible::common::util::iterator-range
    verible::common::util::logging
)

# ---------------------------------------------------------------------------
# verible::common::strings::range   (静态库)   <- BUILD: cc_library(name = "range")
# ---------------------------------------------------------------------------
add_library(verible__common__strings__range STATIC "range.cc")
target_sources(verible__common__strings__range PRIVATE "range.h")

add_library(verible::common::strings::range ALIAS verible__common__strings__range)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__strings__range PRIVATE verible_common_opts)

target_link_libraries(verible__common__strings__range PUBLIC
    verible::common::util::logging
    verible::common::util::range
)

# ---------------------------------------------------------------------------
# verible::common::strings::split   (静态库)   <- BUILD: cc_library(name = "split")
# ---------------------------------------------------------------------------
add_library(verible__common__strings__split STATIC "split.cc")
target_sources(verible__common__strings__split PRIVATE "split.h")

add_library(verible::common::strings::split ALIAS verible__common__strings__split)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__strings__split PRIVATE verible_common_opts)

target_link_libraries(verible__common__strings__split PUBLIC
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::common::strings::rebase   (静态库)   <- BUILD: cc_library(name = "rebase")
# ---------------------------------------------------------------------------
add_library(verible__common__strings__rebase STATIC "rebase.cc")
target_sources(verible__common__strings__rebase PRIVATE "rebase.h")

add_library(verible::common::strings::rebase ALIAS verible__common__strings__rebase)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__strings__rebase PRIVATE verible_common_opts)

target_link_libraries(verible__common__strings__rebase PUBLIC
    verible::common::util::logging
)

# ---------------------------------------------------------------------------
# verible::common::strings::utf8   (纯头文件库)   <- BUILD: cc_library(name = "utf8")
# ---------------------------------------------------------------------------
add_library(verible__common__strings__utf8 INTERFACE)
target_sources(verible__common__strings__utf8 INTERFACE "utf8.h")

add_library(verible::common::strings::utf8 ALIAS verible__common__strings__utf8)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__strings__utf8 INTERFACE verible_common_opts)

# ---------------------------------------------------------------------------
# verible::common::strings::line-column-map   (静态库)   <- BUILD: cc_library(name = "line-column-map")
# ---------------------------------------------------------------------------
add_library(verible__common__strings__line-column-map STATIC "line-column-map.cc")
target_sources(verible__common__strings__line-column-map PRIVATE "line-column-map.h")

add_library(verible::common::strings::line-column-map ALIAS verible__common__strings__line-column-map)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__strings__line-column-map PRIVATE verible_common_opts)

target_link_libraries(verible__common__strings__line-column-map PUBLIC
    verible::common::strings::utf8
)

# ---------------------------------------------------------------------------
# verible::common::strings::string-memory-map   (纯头文件库)   <- BUILD: cc_library(name = "string-memory-map")
# ---------------------------------------------------------------------------
add_library(verible__common__strings__string-memory-map INTERFACE)
target_sources(verible__common__strings__string-memory-map INTERFACE "string-memory-map.h")

add_library(verible::common::strings::string-memory-map ALIAS verible__common__strings__string-memory-map)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__strings__string-memory-map INTERFACE verible_common_opts)

target_link_libraries(verible__common__strings__string-memory-map INTERFACE
    verible::common::strings::range
    verible::common::util::interval-map
    verible::common::util::interval-set
    verible::common::util::logging
)
