# 编码与移植约定

## 基本约定

- 只支持 x64 + MSVC + 静态 CRT；`BUILD_SHARED_LIBS` 固定 OFF。
- 第三方代码统一 `/W0`（由 `cmake/Dependencies.cmake` 注入），自有代码用
  `fpga_lsp_default_warnings()`（`/W4 /permissive- /utf-8`）。
- 源码一律 UTF-8（`/utf-8` 全局开启）。
- 构建产物不进版本库，见 `.gitignore`。
- 注释写「为什么」，不写「做什么」；对非显然的分支要留下依据（实测数据、协议原文、
  Python 侧行号）。

## 移植铁律：发现 Python 缺陷，**忠实复现 + 登记**，不顺手修

移植的第一原则是**行为等价**。顺手"修好"一个 Python 缺陷，会让 C++ 版与
`tests/golden/` 基线、与用户现有体验同时脱节，且无法再判断"是我们的移植错了还是修掉了"。

所有发现的参考实现问题统一登记在 **[`tests/PYTHON-BUGS.md`](../tests/PYTHON-BUGS.md)**，
每条都注明：位置、实测后果、C++ 的处置方式（复现 / 修复）、钉死的测试锚点。

等所有模块完成等价性验收后，再作为一次独立的「契约修正」统一处理并同步 golden 基线。

> 例外：安全等级为 High 的缺陷不等验收，直接修并登记（例如 B7 的 Tcl 注入）。
> 判定标准是「不可信输入能否到达危险汇点」，不是"看起来不优雅"。

## 有意保留的差异

`doc/status.md` 的「已知差异」一节逐条登记了与 Python 参考实现不同、且**不打算改**的行为
（Verible 版本基线、`ifdef` 语义、inlayHint 数量、非 BMP 列、format fallback 等）。
新增差异必须同时改那张表，否则视为回归。