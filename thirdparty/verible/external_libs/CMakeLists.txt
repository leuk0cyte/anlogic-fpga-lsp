# =============================================================================
#  GENERATED FILE - DO NOT EDIT
# =============================================================================
#  由 tools/gen_verible_cmake.py 从同目录的 Bazel BUILD 自动转译而来。
#  手工修改会在下次运行生成器时被覆盖；要改行为请改生成器本身。
#
#  源包: //external_libs
#
#  目录里也没有 BUILD 生成物时，本文件只声明库，不在这里编测试
#  （阶段 0 不编 Verible 自带的 225 个 cc_test）。
# =============================================================================
# ---------------------------------------------------------------------------
# 本包有 1 个 cc_test —— 按 CLAUDE 约定不编译 Verible 自带测试；
# 需要时用 `--with-tests` 重新生成。
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# external_libs::editscript   (纯头文件库)   <- BUILD: cc_library(name = "editscript")
# ---------------------------------------------------------------------------
add_library(external_libs__editscript INTERFACE)
target_sources(external_libs__editscript INTERFACE "editscript.h")

add_library(external_libs::editscript ALIAS external_libs__editscript)
add_library(verible::external_libs::editscript ALIAS external_libs__editscript)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(external_libs__editscript INTERFACE verible_common_opts)
