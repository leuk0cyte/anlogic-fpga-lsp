# =============================================================================
#  cmake/Dependencies.cmake — 第三方依赖接入
# =============================================================================
#  所有依赖均 vendored 在 <root>/thirdparty/ 下，一律用 add_subdirectory 接入，
#  不做 find_package，保证离线可构建、版本可复现。
#
#  ⚠ 顺序敏感：abseil 必须先于 re2 配置。
#     re2/CMakeLists.txt 中有
#         if(NOT TARGET absl::base)
#           find_package(absl REQUIRED)
#         endif()
#     vendored abseil 先 add_subdirectory 后 absl::base 已存在，因此不会走系统查找。
#
#  ⚠ 明确不引入 zlib / protobuf（原 Verible/Bazel 构建里的依赖全部剥离）。
#
#  统一别名 target（后续任务直接 link 这些名字）：
#     absl::*                     (absl 自带)
#     re2::re2                    (re2 自带)
#     nlohmann_json::nlohmann_json(json 自带)
#     GTest::gtest / GTest::gtest_main / GTest::gmock
#     spdlog::spdlog / spdlog::spdlog_header_only
#     simdutf::simdutf
#     ctre::ctre                  (ctre 自带，header-only)
#     fpga_lsp::ctre              (项目统一别名，等同 ctre::ctre)
#     fpga_lsp::verible           (由 cmake/Verible.cmake 提供)
# =============================================================================

include_guard(GLOBAL)

set(FPGA_LSP_THIRDPARTY_DIR "${CMAKE_SOURCE_DIR}/thirdparty" CACHE PATH "vendored 第三方源码根目录")

if(NOT EXISTS "${FPGA_LSP_THIRDPARTY_DIR}")
    message(FATAL_ERROR "找不到第三方依赖目录：${FPGA_LSP_THIRDPARTY_DIR}")
endif()

# -----------------------------------------------------------------------------
# C 编译器必须是 MSVC
# -----------------------------------------------------------------------------
# 顶层 CMakeLists.txt 是 project(... LANGUAGES C CXX)，照理 CMAKE_C_COMPILER 会被
# 解析成 cl。但 googletest 的 CMakeLists.txt 里有 project(... LANGUAGES C CXX)，
# 如果顶层没开 C，它会自行探测 C 编译器——本机上会探到 scoop 的 mingw g++ 并失败：
#     Check for working C compiler: .../mingw/current/bin/g++.exe - broken
#     error: #error "The CMAKE_C_COMPILER is set to a C++ compiler"
# 这里加断言，换机器时立刻给出可行动的错误，而不是半天后才报链接不一致。
if(NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    message(FATAL_ERROR
        "C 编译器必须是 MSVC（当前：'${CMAKE_C_COMPILER_ID}' / '${CMAKE_C_COMPILER}'）。\n"
        "googletest 等第三方会自行探测 C 编译器，若探到 mingw/其它工具链会导致\n"
        "CRT 与运行库不一致。请设置 -DCMAKE_C_COMPILER=cl（或使用 windows-msvc preset）。")
endif()

# -----------------------------------------------------------------------------
# 第三方统一加 /W0：vendored 代码不参与本项目警告治理
# （第三方自己写的 /W3 等在其 target 上追加，位于 /W0 之后，所以仍然生效）
# -----------------------------------------------------------------------------
add_compile_options(/W0)

# 顶层开启了全局 /permissive-（自有代码必需）。它会同时作用到 vendored 代码上，
# 而部分第三方（老版本的 absl/re2/simdutf 等）在 /permissive- 下可能报错。
# 这里在第三方 target 上追加 /permissive 覆盖回来——只在确实出问题时才需要，
# 默认保持 /permissive- 以便尽早发现第三方与本项目的一致性。
option(FPGA_LSP_THIRDPARTY_RELAXED_CONFORMANCE
       "对 vendored 第三方代码使用 /permissive（覆盖全局 /permissive-）"
       OFF)

# 对给定的第三方 target 应用“第三方宽容”选项集。
# 在 target 上 PRIVATE 追加，优先级高于全局 add_compile_options。
function(fpga_lsp_thirdparty_options target)
    if(NOT TARGET ${target})
        return()
    endif()
    target_compile_options(${target} PRIVATE /W0)
    if(FPGA_LSP_THIRDPARTY_RELAXED_CONFORMANCE)
        target_compile_options(${target} PRIVATE /permissive)
    endif()
endfunction()

# -----------------------------------------------------------------------------
# 1) abseil-cpp —— 必须最先接入
# -----------------------------------------------------------------------------
set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ABSL_USE_GOOGLETEST_HEAD OFF CACHE BOOL "" FORCE)
set(ABSL_USE_SYSTEM_INCLUDES ON CACHE BOOL "" FORCE)
# ⚠ 必须设 ON。abseil 的 CMakeLists.txt 在自己的目录作用域内**无条件**改写
# CMAKE_MSVC_RUNTIME_LIBRARY：
#     if(ABSL_MSVC_STATIC_RUNTIME)
#       set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
#     else()
#       set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
#     endif()
# 默认 OFF 会把本工程的静态 CRT（/MT、/MTd）顶成 /MD、/MDd，
# 编译静态库时看不出来，一链接可执行文件就报
#     LNK2038: RuntimeLibrary 的值 MDd_DynamicDebug 不匹配 MTd_StaticDebug
set(ABSL_MSVC_STATIC_RUNTIME ON CACHE BOOL
    "abseil 使用静态 CRT，与顶层 CMAKE_MSVC_RUNTIME_LIBRARY 契约一致" FORCE)
add_subdirectory("${FPGA_LSP_THIRDPARTY_DIR}/abseil-cpp"
                 "${CMAKE_BINARY_DIR}/thirdparty/abseil-cpp"
                 EXCLUDE_FROM_ALL)

# -----------------------------------------------------------------------------
# 2) re2 —— 依赖上一步的 absl::base
# -----------------------------------------------------------------------------
set(RE2_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(RE2_TEST OFF CACHE BOOL "" FORCE)
set(RE2_BENCHMARK OFF CACHE BOOL "" FORCE)
set(RE2_USE_ICU OFF CACHE BOOL "" FORCE)
set(RE2_INSTALL OFF CACHE BOOL "" FORCE)
set(USEPCRE OFF CACHE BOOL "" FORCE)
add_subdirectory("${FPGA_LSP_THIRDPARTY_DIR}/re2"
                 "${CMAKE_BINARY_DIR}/thirdparty/re2"
                 EXCLUDE_FROM_ALL)

# -----------------------------------------------------------------------------
# 3) json (nlohmann) —— header-only
# -----------------------------------------------------------------------------
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
add_subdirectory("${FPGA_LSP_THIRDPARTY_DIR}/json"
                 "${CMAKE_BINARY_DIR}/thirdparty/json"
                 EXCLUDE_FROM_ALL)

# -----------------------------------------------------------------------------
# 4) googletest
# -----------------------------------------------------------------------------
set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${FPGA_LSP_THIRDPARTY_DIR}/googletest"
                 "${CMAKE_BINARY_DIR}/thirdparty/googletest"
                 EXCLUDE_FROM_ALL)

# -----------------------------------------------------------------------------
# 5) spdlog
# -----------------------------------------------------------------------------
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE_HO OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS_HO OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL_HO OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_MASTER_PROJECT OFF CACHE BOOL "" FORCE)
add_subdirectory("${FPGA_LSP_THIRDPARTY_DIR}/spdlog"
                 "${CMAKE_BINARY_DIR}/thirdparty/spdlog"
                 EXCLUDE_FROM_ALL)

# -----------------------------------------------------------------------------
# 6) simdutf
#    simdutf 默认在 MSVC 上给 simdutf target 加了 /WX（warnings-as-errors），
#    第三方头文件/编译器版本一变就可能整体构建失败。这里在 target 上覆盖为
#    /W0 /WX-，只针对本项目构建生效，不改动 thirdparty 源码。
#    同时关闭其 tests / tools / benchmarks / fuzzers / iconv。
# -----------------------------------------------------------------------------
set(SIMDUTF_TESTS OFF CACHE BOOL "" FORCE)
set(SIMDUTF_TOOL_TESTS OFF CACHE BOOL "" FORCE)
set(SIMDUTF_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(SIMDUTF_TOOLS OFF CACHE BOOL "" FORCE)
set(SIMDUTF_ICONV OFF CACHE BOOL "" FORCE)
set(SIMDUTF_FUZZERS OFF CACHE BOOL "" FORCE)
set(SIMDUTF_INTERNAL_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${FPGA_LSP_THIRDPARTY_DIR}/simdutf"
                 "${CMAKE_BINARY_DIR}/thirdparty/simdutf"
                 EXCLUDE_FROM_ALL)

if(TARGET simdutf)
    target_compile_options(simdutf PRIVATE /WX-)
endif()

# -----------------------------------------------------------------------------
# 7) compile-time-regular-expressions (ctre) —— header-only，自带 interface target
# -----------------------------------------------------------------------------
set(CTRE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CTRE_BUILD_PACKAGE OFF CACHE BOOL "" FORCE)
set(CTRE_MODULE OFF CACHE BOOL "" FORCE)
add_subdirectory("${FPGA_LSP_THIRDPARTY_DIR}/compile-time-regular-expressions"
                 "${CMAKE_BINARY_DIR}/thirdparty/ctre"
                 EXCLUDE_FROM_ALL)

# ctre 的 C++ 标准要跟本项目保持一致（ctre 自己默认只要求 C++20，且它把自己
# 的 C++ 标准需求写成了接口属性，会覆盖本项目的 C++23 设置）
if(TARGET ctre::ctre)
    set_target_properties(ctre::ctre PROPERTIES INTERFACE_COMPILE_FEATURES "")
    target_compile_features(ctre::ctre INTERFACE cxx_std_23)
endif()

# 项目统一别名：fpga_lsp::ctre
#   注意 CMake 不允许 add_library(<名字含 ::>) 的普通/INTERFACE target 直接建名，
#   带 :: 的名字只能是 ALIAS，因此这里先建一个内部 target 再做 alias。
if(NOT TARGET fpga_lsp::ctre)
    add_library(fpga_lsp_ctre INTERFACE)
    if(TARGET ctre::ctre)
        target_link_libraries(fpga_lsp_ctre INTERFACE ctre::ctre)
    else()
        target_include_directories(fpga_lsp_ctre INTERFACE
            "${FPGA_LSP_THIRDPARTY_DIR}/compile-time-regular-expressions/include")
    endif()
    add_library(fpga_lsp::ctre ALIAS fpga_lsp_ctre)
endif()

# -----------------------------------------------------------------------------
# 依赖完整性自检：configure 阶段就把缺 target 的问题暴露出来
# -----------------------------------------------------------------------------
set(_fpga_lsp_required_targets
    absl::base
    absl::strings
    absl::flat_hash_map
    absl::synchronization
    absl::str_format
    re2::re2
    nlohmann_json::nlohmann_json
    GTest::gtest
    GTest::gtest_main
    spdlog::spdlog
    simdutf::simdutf
    fpga_lsp::ctre
)

set(_fpga_lsp_missing_targets)
foreach(_t IN LISTS _fpga_lsp_required_targets)
    if(NOT TARGET ${_t})
        list(APPEND _fpga_lsp_missing_targets ${_t})
    endif()
endforeach()
if(_fpga_lsp_missing_targets)
    message(FATAL_ERROR "第三方依赖接入失败，缺少 target：${_fpga_lsp_missing_targets}")
endif()

message(STATUS "Dependencies: abseil-cpp, re2, nlohmann_json, googletest, spdlog, simdutf, ctre 全部就绪")