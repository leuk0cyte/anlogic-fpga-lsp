# =============================================================================
#  fpga-lsp-cpp — Anlogic TangDynasty FPGA language server (C++23 rewrite)
# =============================================================================
#  顶层构建脚本。只支持 MSVC + Ninja，静态 CRT，C++23。
#  依赖（全部 vendored 在 thirdparty/）由 cmake/Dependencies.cmake 接入，
#  Verible（SystemVerilog 解析器/linter/formatter）由 cmake/Verible.cmake 接入。
# =============================================================================

cmake_minimum_required(VERSION 3.24)

# -----------------------------------------------------------------------------
# 编译器前置检查（必须在 project() 之前）
# -----------------------------------------------------------------------------
# 本项目同时启用 C 与 C++（googletest 等第三方会自行探测 C 编译器）。
# 若用户绕过 preset 直接 `cmake -S . -B build`，CMake 会自行在 PATH 上找 C 编译器，
# 而本机 PATH 里有 scoop 的 mingw g++ —— 它会被当成 C 编译器，
# 失败信息是 CMake 内部的：
#     CMakeTestCCompiler.cmake:65: error: #error "The CMAKE_C_COMPILER is set to a C++ compiler"
# 对用户毫无指导意义。所以在 project() 之前就把话说清楚。
if(NOT DEFINED CMAKE_C_COMPILER AND NOT DEFINED CMAKE_CXX_COMPILER)
    message(FATAL_ERROR
        "\n"
        "  未指定编译器。本项目仅支持 MSVC，且必须显式指定 C 与 C++ 编译器。\n"
        "  直接 `cmake -S . -B build` 会去 PATH 上找 C 编译器，本机可能探到\n"
        "  mingw g++ 并被 CMake 拒绝（报错形如\n"
        "    'The CMAKE_C_COMPILER is set to a C++ compiler'）。\n"
        "\n"
        "  推荐做法（自动带上 VS 环境与 cl.exe）：\n"
        "      cmake --preset windows-msvc-release\n"
        "      cmake --build --preset windows-msvc-release\n"
        "\n"
        "  或手动指定：\n"
        "      cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl\n"
        "\n"
        "  注意：cl.exe 需要在 vcvars64.bat 提供的环境里才可用：\n"
        "      \"C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat\"\n")
endif()

project(
    fpga-lsp-cpp
    VERSION 0.28.0
    DESCRIPTION "Anlogic TangDynasty FPGA language server (C++23 rewrite of Y:\\fpga-lsp)"
    LANGUAGES C CXX
)

# -----------------------------------------------------------------------------
# 工具链约束（project() 之后，此时编译器 ID 已确定）
# -----------------------------------------------------------------------------
if(NOT MSVC)
    message(FATAL_ERROR
        "\n"
        "  fpga-lsp-cpp 仅支持 MSVC。\n"
        "  当前 C  编译器：'${CMAKE_C_COMPILER_ID}' (${CMAKE_C_COMPILER})\n"
        "  当前 C++ 编译器：'${CMAKE_CXX_COMPILER_ID}' (${CMAKE_CXX_COMPILER})\n"
        "\n"
        "  请使用 preset：cmake --preset windows-msvc-release\n")
endif()

# 单配置生成器未指定构建类型时默认 Release
get_property(_fpga_lsp_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _fpga_lsp_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    message(STATUS "fpga-lsp-cpp: 未指定 CMAKE_BUILD_TYPE，默认使用 Release")
endif()

# -----------------------------------------------------------------------------
# C++ 语言标准
# -----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 静态 CRT（/MT、/MTd），与第三方的默认静态链接保持一致
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# 第三方 library 一律静态
if(NOT DEFINED BUILD_SHARED_LIBS)
    set(BUILD_SHARED_LIBS OFF)
endif()

# -----------------------------------------------------------------------------
# 全局编译选项
# 注意：这里的选项会加到所有 target 上（包括 thirdparty）。第三方可以再追加
#       自己的 /wd 抑制，最后的选项优先级更高。
# -----------------------------------------------------------------------------
add_compile_options(
    /utf-8                # 源码/执行字符集统一 UTF-8（源码里有中文注释）
    /permissive-          # 关闭 MSVC 宽松模式（自有代码必需，见下）
    /Zc:__cplusplus       # __cplusplus 反映真实标准版本
    /Zc:preprocessor      # 标准预处理器（absl/re2/spdlog 均需要）
    /Zc:inline            # 去掉未引用的 COMDAT，减小体积
    /bigobj               # 大编译单元的必需项
)

# /MP 只在 Visual Studio 生成器下启用。
# Ninja 本身已经做 target 级并行，再叠加 /MP 会让每个 cl.exe 进程内部又开线程，
# 在 192 逻辑核的机器上并发度失控、内存容易被 abseil/Verible 的大 TU 打爆。
if(CMAKE_GENERATOR MATCHES "Visual Studio")
    add_compile_options(/MP)
endif()

# 架构：默认 x64
if(NOT CMAKE_GENERATOR_PLATFORM AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(WARNING "fpga-lsp-cpp 目标平台为 x64，当前指针宽度为 ${CMAKE_SIZEOF_VOID_P} 字节，请检查 vcvars 环境")
endif()

# 调试信息
if(POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
endif()
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>")

# 输出目录统一
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# 在 VS Code / clangd 里也能拿到编译数据库
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# -----------------------------------------------------------------------------
# 编译选项辅助函数（供 src/ 与后续任务使用）
# -----------------------------------------------------------------------------
include(cmake/CompilerWarnings.cmake)

# -----------------------------------------------------------------------------
# 第三方依赖（abseil -> re2 顺序敏感，见 Dependencies.cmake）
# -----------------------------------------------------------------------------
include(cmake/Dependencies.cmake)

# -----------------------------------------------------------------------------
# Verible（Bazel -> CMake 移植；阶段 0-B 填充）
# -----------------------------------------------------------------------------
include(cmake/Verible.cmake)

# -----------------------------------------------------------------------------
# 本项目源码
# -----------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/CMakeLists.txt")
    add_subdirectory(src)
endif()

# -----------------------------------------------------------------------------
# 默认构建（all）内容
# -----------------------------------------------------------------------------
# 所有 vendored 依赖都以 EXCLUDE_FROM_ALL 接入（这样它们在 configure 阶段正常
# 提供 target，但不会被无关的 all 构建拖来编译）。代价是：当本工程自身还没有
# 任何产出 target 时，CMake 生成的 `build all: phony` 会没有任何依赖，
# `cmake --build` 就变成 “ninja: no work to do”。
#
# 因此在依赖库全部接入后显式把它们挂到 all 上：
#   - FPGA_LSP_BUILD_THIRDPARTY_IN_ALL=ON（默认）：直接把第三方库纳入默认构建，
#     保证 `cmake --build` 一定编出 absl/re2/spdlog/simdutf/gtest。
#   - 设为 OFF：第三方不参与默认构建，只在本工程 target 链接到它们时才编译
#     （适合只要 server 可执行文件的日常增量构建）。
# -----------------------------------------------------------------------------
option(FPGA_LSP_BUILD_THIRDPARTY_IN_ALL
       "把 vendored 第三方静态库纳入默认构建目标（使 cmake --build 编出所有依赖库）"
       ON)

set(FPGA_LSP_THIRDPARTY_TARGETS
    absl_core_headers absl_base absl_strings absl_synchronization
    absl_flat_hash_map absl_str_format absl_log
    re2
    spdlog
    simdutf
)

if(FPGA_LSP_BUILD_THIRDPARTY_IN_ALL)
    set(_fpga_lsp_all_targets)
    foreach(_t IN LISTS FPGA_LSP_THIRDPARTY_TARGETS)
        if(TARGET ${_t})
            list(APPEND _fpga_lsp_all_targets ${_t})
        endif()
    endforeach()
    # GTest 只在开启测试时才需要
    if(BUILD_TESTING)
        foreach(_t gtest gtest_main)
            if(TARGET ${_t})
                list(APPEND _fpga_lsp_all_targets ${_t})
            endif()
        endforeach()
    endif()

    if(_fpga_lsp_all_targets)
        add_custom_target(fpga_lsp_thirdparty ALL
                          COMMENT "构建 vendored 第三方库"
                          DEPENDS ${_fpga_lsp_all_targets})
    endif()
endif()

# -----------------------------------------------------------------------------
# 测试
# -----------------------------------------------------------------------------
# 统一测试根目录为 tests/ （复数）。
# 历史上这里找的是 test/ （单数），而 gate-verify 的 tests/gate-corpus、
# verible-prune 的 tests/golden 都在 tests/ 下，容易分裂成两套。现统一为 tests/，
# 并对遗留的 test/ 保留兼容（两者都有 CMakeLists.txt 时优先 tests/）。
# -----------------------------------------------------------------------------
include(CTest)
enable_testing()
if(BUILD_TESTING)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
        add_subdirectory(tests)
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/CMakeLists.txt")
        message(DEPRECATION "test/CMakeLists.txt 已废弃，请迁移到 tests/CMakeLists.txt")
        add_subdirectory(test)
    endif()
endif()

# -----------------------------------------------------------------------------
# 配置期摘要
# -----------------------------------------------------------------------------
message(STATUS "")
message(STATUS "==================== fpga-lsp-cpp ====================")
message(STATUS "  source dir   : ${CMAKE_CURRENT_SOURCE_DIR}")
message(STATUS "  binary dir   : ${CMAKE_BINARY_DIR}")
message(STATUS "  generator    : ${CMAKE_GENERATOR}")
message(STATUS "  build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "  cxx compiler : ${CMAKE_CXX_COMPILER}")
message(STATUS "  cxx compiler : ${CMAKE_CXX_COMPILER_VERSION} (${CMAKE_CXX_COMPILER_ID})")
message(STATUS "  cxx std      : ${CMAKE_CXX_STANDARD}")
message(STATUS "  static crt   : ${CMAKE_MSVC_RUNTIME_LIBRARY}")
message(STATUS "  tests        : ${BUILD_TESTING}")
message(STATUS "=====================================================")
message(STATUS "")