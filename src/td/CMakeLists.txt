# =============================================================================
#  src/td/ —— TangDynasty（TD）会话层
# =============================================================================
#  对应参考实现：%PYREF%\fpgalsp\td.py
# =============================================================================

add_library(fpga_lsp_td STATIC
    td_install.cpp
    td_output.cpp
    td_session.cpp
    td_device.cpp
    td_diagnostics.cpp
    # A5 / task-25：单飞队列 + 取消 + 去抖（策略层，不依赖 lsp_server）
    td_service.cpp
)

add_library(fpga_lsp::td ALIAS fpga_lsp_td)

target_include_directories(fpga_lsp_td PUBLIC
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(fpga_lsp_td PUBLIC
    fpga_lsp::warnings
    fpga_lsp::platform
    # td_diagnostics 产出 lsp::Diagnostic（所有诊断来源共用的唯一形状）。
    # 依赖方向是 td -> lsp，单向；lsp 不依赖 td，不存在循环。
    fpga_lsp::lsp
    # 路径解析复用 .al 工程模型（resolved_via 三态 / declared_path 回落）
    fpga_lsp::project
    re2::re2
)

target_compile_features(fpga_lsp_td PUBLIC cxx_std_23)
