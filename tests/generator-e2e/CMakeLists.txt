# =============================================================================
#  tests/generator-e2e/CMakeLists.txt — 生成器独立端到端验证工程
# =============================================================================
#  这是一个**独立的** CMake 工程，只做两件事：
#    1. include(verible-generators.cmake)，用真实的 win_flex / win_bison 跑 G1–G6
#    2. 把生成物断言注册成 CTest
#
#  它**不编译任何 C++**，因此不依赖 abseil / re2 / googletest / verible 库，
#  可以在任何构建目录、任何时刻独立运行：
#
#      cmake -S tests/generator-e2e -B build/generator-e2e -G Ninja
#      cmake --build build/generator-e2e
#      ctest --test-dir build/generator-e2e --output-on-failure
#
#  这样它同时是：
#    · task-10 的验收载体
#    · 将来任何人验证"生成器是否还正常"的独立工具
#  并且与主工程用的是**同一份** verible-generators.cmake，天然一致。
# =============================================================================

cmake_minimum_required(VERSION 3.24)

project(verible-generator-e2e LANGUAGES NONE)

# 允许在没有 VS 环境的机器上跑：本工程不需要编译器
enable_testing()

# -----------------------------------------------------------------------------
# 路径
# -----------------------------------------------------------------------------
set(_here "${CMAKE_CURRENT_SOURCE_DIR}")
get_filename_component(_root "${_here}/../.." ABSOLUTE)

set(VERIBLE_SRC_DIR "${_root}/thirdparty/verible" CACHE PATH "vendored verible 源码根")
set(VERIBLE_GEN_ROOT "${CMAKE_BINARY_DIR}/gen" CACHE PATH "生成物输出根")

# -----------------------------------------------------------------------------
# 引入真实生成器编排（与主工程同一份文件）
# -----------------------------------------------------------------------------
include("${_root}/cmake/scripts/verible-generators.cmake")

# 加载计数基线（第 2 层防线）
include("${_root}/cmake/scripts/verible-gen-baseline.cmake")

# -----------------------------------------------------------------------------
# 注册生成物回归断言
#   verible-gen-check.cmake 支持 -DCHECK=<项目> -DGEN_DIR=<根>
# -----------------------------------------------------------------------------
set(_check "${_root}/cmake/scripts/verible-gen-check.cmake")

function(_add_gen_check name)
    add_test(NAME "generator.${name}"
             COMMAND "${CMAKE_COMMAND}"
                     "-DCHECK=${name}"
                     "-DGEN_DIR=${VERIBLE_GEN_ROOT}"
                     "-DWIN_FLEX=${WIN_FLEX}"
                     "-DWIN_BISON=${WIN_BISON}"
                     -P "${_check}")
    set_tests_properties("generator.${name}" PROPERTIES LABELS "generator")
endfunction()

# 交付 1/2 的断言
_add_gen_check(files)
_add_gen_check(g4)
_add_gen_check(g5)
_add_gen_check(g6a)
_add_gen_check(g6b)
_add_gen_check(g6b-pair)
_add_gen_check(versions)

# 计数基线（第 2 层）
add_test(NAME "generator.baseline"
         COMMAND "${CMAKE_COMMAND}"
                 "-DCHECK=baseline"
                 "-DGEN_DIR=${VERIBLE_GEN_ROOT}"
                 "-DBASELINE_FILE=${_root}/cmake/scripts/verible-gen-baseline.cmake"
                 -P "${_check}")
set_tests_properties("generator.baseline" PROPERTIES LABELS "generator")

# 差分断言（第 1 层，覆盖 G6a 的四类锚点）
add_test(NAME "generator.g6a-delta"
         COMMAND "${CMAKE_COMMAND}"
                 "-DCHECK=g6a-delta"
                 "-DGEN_DIR=${VERIBLE_GEN_ROOT}"
                 -P "${_check}")
set_tests_properties("generator.g6a-delta" PROPERTIES LABELS "generator")

# 结构完整性：G6b 必须"插在 yynerrs 之后"，不只是"存在于文件里"
add_test(NAME "generator.g6b-structure"
         COMMAND "${CMAKE_COMMAND}"
                 "-DCHECK=g6b-structure"
                 "-DGEN_DIR=${VERIBLE_GEN_ROOT}"
                 -P "${_check}")
set_tests_properties("generator.g6b-structure" PROPERTIES LABELS "generator")

# G1/G3 lexer 产物必须可辨识（应当含 yyclass 对应的类名）
add_test(NAME "generator.g1-g3-lexer"
         COMMAND "${CMAKE_COMMAND}"
                 "-DCHECK=lexer"
                 "-DGEN_DIR=${VERIBLE_GEN_ROOT}"
                 -P "${_check}")
set_tests_properties("generator.g1-g3-lexer" PROPERTIES LABELS "generator")

# -----------------------------------------------------------------------------
# 单测（假输入，不依赖真实 bison，快）
# -----------------------------------------------------------------------------
file(GLOB _unit_tests "${_here}/unit/*.cmake")
foreach(_f IN LISTS _unit_tests)
    get_filename_component(_name "${_f}" NAME_WE)
    add_test(NAME "generator.unit.${_name}"
             COMMAND "${CMAKE_COMMAND}"
                     "-DPATCH_SCRIPT=${_root}/cmake/scripts/verible-patch.cmake"
                     "-DWORK_DIR=${CMAKE_BINARY_DIR}/unit/${_name}"
                     -P "${_f}")
    set_tests_properties("generator.unit.${_name}" PROPERTIES LABELS "generator;unit")
endforeach()