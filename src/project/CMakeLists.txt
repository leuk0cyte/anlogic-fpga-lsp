# =============================================================================
#  src/project/ —— Anlogic TD 工程模型（.al 容错 XML 解析）
# =============================================================================

add_library(fpga_lsp_project STATIC
    xml_lite.cpp
    al_project.cpp
)

add_library(fpga_lsp::project ALIAS fpga_lsp_project)

target_include_directories(fpga_lsp_project PUBLIC
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(fpga_lsp_project PUBLIC
    fpga_lsp::warnings
    fpga_lsp::platform
)

target_compile_features(fpga_lsp_project PUBLIC cxx_std_23)