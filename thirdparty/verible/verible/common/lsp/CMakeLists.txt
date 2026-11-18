# =============================================================================
#  GENERATED FILE - DO NOT EDIT
# =============================================================================
#  由 tools/gen_verible_cmake.py 从同目录的 Bazel BUILD 自动转译而来。
#  手工修改会在下次运行生成器时被覆盖；要改行为请改生成器本身。
#
#  源包: //verible/common/lsp
#
#  目录里也没有 BUILD 生成物时，本文件只声明库，不在这里编测试
#  （阶段 0 不编 Verible 自带的 225 个 cc_test）。
# =============================================================================
# ---------------------------------------------------------------------------
# BUILD 里的 genrule（Bazel 的 sed/grep 在 Windows 上不可用，改用 cmake -P）
# ---------------------------------------------------------------------------
# genrule(lsp-protocol-gen) -> lsp-protocol.h
#   TODO(阶段 0-D)：该生成规则属于 flex/bison 链路，
#   由 cmake/scripts/ 实现，本脚本不重复生成。

# genrule(jcxxgen-testfile-gen) -> jcxxgen-testfile.h
#   TODO(阶段 0-D)：该生成规则属于 flex/bison 链路，
#   由 cmake/scripts/ 实现，本脚本不重复生成。

# ---------------------------------------------------------------------------
# 本包有 6 个 cc_test —— 按 CLAUDE 约定不编译 Verible 自带测试；
# 需要时用 `--with-tests` 重新生成。
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# verible::common::lsp::message-stream-splitter   (静态库)   <- BUILD: cc_library(name = "message-stream-splitter")
# ---------------------------------------------------------------------------
add_library(verible__common__lsp__message-stream-splitter STATIC "message-stream-splitter.cc")
target_sources(verible__common__lsp__message-stream-splitter PRIVATE "message-stream-splitter.h")

add_library(verible::common::lsp::message-stream-splitter ALIAS verible__common__lsp__message-stream-splitter)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__lsp__message-stream-splitter PRIVATE verible_common_opts)

target_link_libraries(verible__common__lsp__message-stream-splitter PUBLIC
    verible::common::util::status-macros
    absl::status
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::common::lsp::json-rpc-dispatcher   (静态库)   <- BUILD: cc_library(name = "json-rpc-dispatcher")
# ---------------------------------------------------------------------------
add_library(verible__common__lsp__json-rpc-dispatcher STATIC "json-rpc-dispatcher.cc")
target_sources(verible__common__lsp__json-rpc-dispatcher PRIVATE "json-rpc-dispatcher.h")

add_library(verible::common::lsp::json-rpc-dispatcher ALIAS verible__common__lsp__json-rpc-dispatcher)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__lsp__json-rpc-dispatcher PRIVATE verible_common_opts)

target_link_libraries(verible__common__lsp__json-rpc-dispatcher PUBLIC
    verible::common::util::logging
    nlohmann_json::nlohmann_json
)

# ---------------------------------------------------------------------------
# verible::common::lsp::lsp-protocol   (纯头文件库)   <- BUILD: cc_library(name = "lsp-protocol")
# ---------------------------------------------------------------------------
add_library(verible__common__lsp__lsp-protocol INTERFACE)

# 构建期生成的文件（configure 时还不存在，由 cmake/Verible.cmake 挂接）：
set_property(TARGET verible__common__lsp__lsp-protocol PROPERTY verible_generated_headers "lsp-protocol.h")

add_library(verible::common::lsp::lsp-protocol ALIAS verible__common__lsp__lsp-protocol)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__lsp__lsp-protocol INTERFACE verible_common_opts)

target_link_libraries(verible__common__lsp__lsp-protocol INTERFACE
    nlohmann_json::nlohmann_json
)

# ---------------------------------------------------------------------------
# verible::common::lsp::lsp-protocol-enums   (纯头文件库)   <- BUILD: cc_library(name = "lsp-protocol-enums")
# ---------------------------------------------------------------------------
add_library(verible__common__lsp__lsp-protocol-enums INTERFACE)
target_sources(verible__common__lsp__lsp-protocol-enums INTERFACE "lsp-protocol-enums.h")

add_library(verible::common::lsp::lsp-protocol-enums ALIAS verible__common__lsp__lsp-protocol-enums)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__lsp__lsp-protocol-enums INTERFACE verible_common_opts)

# ---------------------------------------------------------------------------
# verible::common::lsp::lsp-protocol-operators   (纯头文件库)   <- BUILD: cc_library(name = "lsp-protocol-operators")
# ---------------------------------------------------------------------------
add_library(verible__common__lsp__lsp-protocol-operators INTERFACE)
target_sources(verible__common__lsp__lsp-protocol-operators INTERFACE "lsp-protocol-operators.h")

add_library(verible::common::lsp::lsp-protocol-operators ALIAS verible__common__lsp__lsp-protocol-operators)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__lsp__lsp-protocol-operators INTERFACE verible_common_opts)

target_link_libraries(verible__common__lsp__lsp-protocol-operators INTERFACE
    verible::common::lsp::lsp-protocol
)

# ---------------------------------------------------------------------------
# verible::common::lsp::lsp-text-buffer   (静态库)   <- BUILD: cc_library(name = "lsp-text-buffer")
# ---------------------------------------------------------------------------
add_library(verible__common__lsp__lsp-text-buffer STATIC "lsp-text-buffer.cc")
target_sources(verible__common__lsp__lsp-text-buffer PRIVATE "lsp-text-buffer.h")

add_library(verible::common::lsp::lsp-text-buffer ALIAS verible__common__lsp__lsp-text-buffer)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__lsp__lsp-text-buffer PRIVATE verible_common_opts)

target_link_libraries(verible__common__lsp__lsp-text-buffer PUBLIC
    verible::common::lsp::json-rpc-dispatcher
    verible::common::lsp::lsp-protocol
    verible::common::strings::utf8
    absl::flat_hash_map
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::common::lsp::lsp-file-utils   (静态库)   <- BUILD: cc_library(name = "lsp-file-utils")
# ---------------------------------------------------------------------------
add_library(verible__common__lsp__lsp-file-utils STATIC "lsp-file-utils.cc")
target_sources(verible__common__lsp__lsp-file-utils PRIVATE "lsp-file-utils.h")

add_library(verible::common::lsp::lsp-file-utils ALIAS verible__common__lsp__lsp-file-utils)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__lsp__lsp-file-utils PRIVATE verible_common_opts)

target_link_libraries(verible__common__lsp__lsp-file-utils PUBLIC
    absl::flags_config
    absl::strings
)

# ---------------------------------------------------------------------------
# verible::common::lsp::jcxxgen-testfile   (纯头文件库)   <- BUILD: cc_library(name = "jcxxgen-testfile")
# ---------------------------------------------------------------------------
add_library(verible__common__lsp__jcxxgen-testfile INTERFACE)

# 构建期生成的文件（configure 时还不存在，由 cmake/Verible.cmake 挂接）：
set_property(TARGET verible__common__lsp__jcxxgen-testfile PROPERTY verible_generated_headers "jcxxgen-testfile.h")

add_library(verible::common::lsp::jcxxgen-testfile ALIAS verible__common__lsp__jcxxgen-testfile)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__lsp__jcxxgen-testfile INTERFACE verible_common_opts)

target_link_libraries(verible__common__lsp__jcxxgen-testfile INTERFACE
    nlohmann_json::nlohmann_json
)

# ---------------------------------------------------------------------------
# verible::common::lsp::jcxxgen   (可执行)   <- BUILD: cc_binary(name = "jcxxgen")
# ---------------------------------------------------------------------------
add_executable(verible__common__lsp__jcxxgen "jcxxgen.cc")

# cc_binary：可执行文件，无 add_library ALIAS；引用时用 $<TARGET_FILE:verible__common__lsp__jcxxgen>
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__common__lsp__jcxxgen PRIVATE verible_common_opts)

target_link_libraries(verible__common__lsp__jcxxgen PUBLIC
    verible::common::util::init-command-line
    absl::flat_hash_map
    absl::flags
    absl::strings
    re2::re2
)
