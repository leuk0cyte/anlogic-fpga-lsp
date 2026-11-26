# =============================================================================
#  src/constraints/ —— ADC 引脚约束 + SDC 时序约束
# =============================================================================
#  对应参考实现：
#    %PYREF%\fpgalsp\adc.py  (913 行)  ->  adc.h/.cpp
#    %PYREF%\fpgalsp\sdc.py  (1068 行) ->  sdc.h/.cpp
#
#  依赖方向：constraints -> lsp（Diagnostic 唯一形状）+ re2（正则）
#  刻意**不**依赖 td / project / server：这两个解析器是纯文本的，
#  任何"顺手把工程模型拉进来"的改动都会让它们变得不可单测。
# =============================================================================

add_library(fpga_lsp_constraints STATIC
    adc.cpp
    port.cpp
    sdc.cpp
    text_util.cpp
)

add_library(fpga_lsp::constraints ALIAS fpga_lsp_constraints)

target_include_directories(fpga_lsp_constraints PUBLIC
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(fpga_lsp_constraints PUBLIC
    fpga_lsp::warnings
    # 所有诊断来源共用 lsp::Diagnostic —— 不要在这里发明第二种结构
    fpga_lsp::lsp
    # 正则统一用 re2（std::regex 在 MSVC 上慢且回溯递归易爆栈）
    re2::re2
    nlohmann_json::nlohmann_json
)

target_compile_features(fpga_lsp_constraints PUBLIC cxx_std_23)