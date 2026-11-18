# =============================================================================
#  GENERATED FILE - DO NOT EDIT
# =============================================================================
#  由 tools/gen_verible_cmake.py 从同目录的 Bazel BUILD 自动转译而来。
#  手工修改会在下次运行生成器时被覆盖；要改行为请改生成器本身。
#
#  源包: //verible/common/lexer
#
#  目录里也没有 BUILD 生成物时，本文件只声明库，不在这里编测试
#  （阶段 0 不编 Verible 自带的 225 个 cc_test）。
# =============================================================================
# ---------------------------------------------------------------------------
# 本包有 2 个 cc_test —— 按 CLAUDE 约定不编译 Verible 自带测试；
# 需要时用 `--with-tests` 重新生成。
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# verible::common::lexer::token-generator   (纯头文件库)   <- BUILD: cc_library(name = "token-generator")
# ---------------------------------------------------------------------------
add_library(verible__common__lexer__token-generator INTERFACE)
target_sources(verible__common__lexer__token-generator INTERFACE "token-generator.h")

add_library(verible::common::lexer::token-generator ALIAS verible__common__lexer__token-generator)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__lexer__token-generator INTERFACE verible_common_opts)

target_link_libraries(verible__common__lexer__token-generator INTERFACE
    verible::common::text::token-info
)

# ---------------------------------------------------------------------------
# verible::common::lexer::token-stream-adapter   (静态库)   <- BUILD: cc_library(name = "token-stream-adapter")
# ---------------------------------------------------------------------------
add_library(verible__common__lexer__token-stream-adapter STATIC "token-stream-adapter.cc")
target_sources(verible__common__lexer__token-stream-adapter PRIVATE "token-stream-adapter.h")

add_library(verible::common::lexer::token-stream-adapter ALIAS verible__common__lexer__token-stream-adapter)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__lexer__token-stream-adapter PRIVATE verible_common_opts)

target_link_libraries(verible__common__lexer__token-stream-adapter PUBLIC
    verible::common::lexer::lexer
    verible::common::lexer::token-generator
    verible::common::text::token-info
    verible::common::text::token-stream-view
    absl::status
)

# ---------------------------------------------------------------------------
# verible::common::lexer::flex-lexer-adapter   (纯头文件库)   <- BUILD: cc_library(name = "flex-lexer-adapter")
# ---------------------------------------------------------------------------
add_library(verible__common__lexer__flex-lexer-adapter INTERFACE)
target_sources(verible__common__lexer__flex-lexer-adapter INTERFACE "flex-lexer-adapter.h")

add_library(verible::common::lexer::flex-lexer-adapter ALIAS verible__common__lexer__flex-lexer-adapter)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__lexer__flex-lexer-adapter INTERFACE verible_common_opts)

target_link_libraries(verible__common__lexer__flex-lexer-adapter INTERFACE
    verible::common::lexer::lexer
    verible::common::text::token-info
    verible::common::util::logging
    absl::check
)

# ---------------------------------------------------------------------------
# verible::common::lexer::lexer   (纯头文件库)   <- BUILD: cc_library(name = "lexer")
# ---------------------------------------------------------------------------
add_library(verible__common__lexer__lexer INTERFACE)
target_sources(verible__common__lexer__lexer INTERFACE "lexer.h")

add_library(verible::common::lexer::lexer ALIAS verible__common__lexer__lexer)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__lexer__lexer INTERFACE verible_common_opts)

target_link_libraries(verible__common__lexer__lexer INTERFACE
    verible::common::text::token-info
)

# ---------------------------------------------------------------------------
# verible::common::lexer::lexer-test-util   (静态库)   <- BUILD: cc_library(name = "lexer-test-util")
# ---------------------------------------------------------------------------
add_library(verible__common__lexer__lexer-test-util STATIC "lexer-test-util.cc")
target_sources(verible__common__lexer__lexer-test-util PRIVATE "lexer-test-util.h")

add_library(verible::common::lexer::lexer-test-util ALIAS verible__common__lexer__lexer-test-util)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__lexer__lexer-test-util PRIVATE verible_common_opts)

target_link_libraries(verible__common__lexer__lexer-test-util PUBLIC
    verible::common::text::constants
    verible::common::text::token-info
    verible::common::text::token-info-test-util
    verible::common::util::logging
    GTest::gtest
)

# BUILD: testonly = 1（本阶段不编译测试，库照常生成以便被引用）
