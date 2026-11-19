// =============================================================================
//  src/constraints/data/adc_io.inc —— TD 实测 IO 标准 / PULLTYPE（ADC 补全候选）
// =============================================================================
//  ⚠ **生成文件，不要手改**。生成器：`scratch/gen_b10_data.py`（B10 / parity-symbols）。
//  数据源：`fpgalsp/data/io_standards.json`（Python 参考实现 `Y:\fpga-lsp` @ b4891bb）。
//
//  为什么 vendored 而不是运行时读 JSON：
//    语言服务器不应当依赖 Python 仓库的安装路径存在；而且这两个表只随 TD release
//    变化，编译期嵌入后行为可复现、可单测（见 tests/unit/constraint_symbols_test.cpp）。
//
//  计数口径务必与 Python 侧一致（探针按集合/计数比对）：
//    kAdcIoStandards  37 项（Python `_complete_adc` 的 iostandard 分支）
//    kAdcPulltypes    4 项（pulltype 分支）
// =============================================================================

constexpr std::string_view kAdcIoStandards[] = {
    "BLVDS25",
    "DIFF_HSTL18_I",
    "DIFF_POD12",
    "DIFF_SSTL12",
    "DIFF_SSTL135",
    "DIFF_SSTL15",
    "DIFF_SSTL18_I",
    "HSTL12_I",
    "HSTL18",
    "HSTL18_I",
    "HSTL18_II",
    "HSUL12",
    "LVCMOS10",
    "LVCMOS12",
    "LVCMOS15",
    "LVCMOS16",
    "LVCMOS18",
    "LVCMOS25",
    "LVCMOS33",
    "LVCMOS4",
    "LVCMOS8",
    "LVDS18",
    "LVDS18_E",
    "LVDS25",
    "LVDS25_E",
    "LVDS33_E",
    "LVDS71",
    "LVPECL33",
    "LVTTL33",
    "POD12",
    "SSTL12",
    "SSTL135",
    "SSTL15",
    "SSTL18",
    "SSTL18_I",
    "SSTL18_II",
    "SSTL25_I",
};

constexpr std::string_view kAdcPulltypes[] = {
    "KEEPER",
    "NONE",
    "PULLDOWN",
    "PULLUP",
};
