# =============================================================================
#  cmake/CompilerWarnings.cmake —— 本项目自有代码的警告策略
# =============================================================================
#  用法：
#     include(cmake/CompilerWarnings.cmake)
#     fpga_lsp_default_warnings(my_target)
#     fpga_lsp_warnings_as_errors(my_target)   # 可选，CI 用
#
#  注意：thirdparty 与 vendor 进来的 Verible 代码不要调用本函数（它们走 /W0）。
# =============================================================================

include_guard(GLOBAL)

function(fpga_lsp_default_warnings target)
    if(NOT MSVC)
        return()
    endif()
    target_compile_options(${target} PRIVATE
        /W4
        /permissive-
        /utf-8
        /Zc:__cplusplus
        /Zc:preprocessor
        /wd4324   # structure was padded due to alignment specifier
    )
endfunction()

function(fpga_lsp_warnings_as_errors target)
    if(NOT MSVC)
        return()
    endif()
    target_compile_options(${target} PRIVATE /WX)
endfunction()