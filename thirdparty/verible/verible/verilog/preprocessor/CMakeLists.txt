# =============================================================================
#  GENERATED FILE - DO NOT EDIT
# =============================================================================
#  由 tools/gen_verible_cmake.py 从同目录的 Bazel BUILD 自动转译而来。
#  手工修改会在下次运行生成器时被覆盖；要改行为请改生成器本身。
#
#  源包: //verible/verilog/preprocessor
#
#  目录里也没有 BUILD 生成物时，本文件只声明库，不在这里编测试
#  （阶段 0 不编 Verible 自带的 225 个 cc_test）。
# =============================================================================
# ---------------------------------------------------------------------------
# 本包有 1 个 cc_test —— 按 CLAUDE 约定不编译 Verible 自带测试；
# 需要时用 `--with-tests` 重新生成。
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# verible::verilog::preprocessor::verilog-preprocess   (静态库)   <- BUILD: cc_library(name = "verilog-preprocess")
# ---------------------------------------------------------------------------
add_library(verible__verilog__preprocessor__verilog-preprocess STATIC "verilog-preprocess.cc")
target_sources(verible__verilog__preprocessor__verilog-preprocess PRIVATE "verilog-preprocess.h")

add_library(verible::verilog::preprocessor::verilog-preprocess ALIAS verible__verilog__preprocessor__verilog-preprocess)
# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义
target_link_libraries(verible__verilog__preprocessor__verilog-preprocess PRIVATE verible_common_opts)

target_link_libraries(verible__verilog__preprocessor__verilog-preprocess PUBLIC
    verible::common::lexer::token-generator
    verible::common::lexer::token-stream-adapter
    verible::common::text::macro-definition
    verible::common::text::text-structure
    verible::common::text::token-info
    verible::common::text::token-stream-view
    verible::common::util::container-util
    verible::common::util::logging
    verible::common::util::status-macros
    verible::verilog::analysis::verilog-filelist
    verible::verilog::parser::verilog-lexer
    verible::verilog::parser::verilog-parser
    verible::verilog::parser::verilog-token-enum
    absl::status
    absl::statusor
    absl::strings
)
