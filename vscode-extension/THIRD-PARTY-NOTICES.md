# Third-Party Notices

This product, **fpga-lsp-cpp**, is licensed under the
**GNU Affero General Public License v3.0 or later** (see [`LICENSE`](LICENSE)).
It statically links the third-party components listed below. Each component is
distributed under its own license; the full license texts ship with the sources
under `thirdparty/<component>/`.

## Bundled components

| Component | Version / origin | License | Copyright |
| --- | --- | --- | --- |
| Abseil C++ (`thirdparty/abseil-cpp`) | abseil-cpp | Apache-2.0 | Copyright 2017 The Abseil Authors |
| RE2 (`thirdparty/re2`) | re2 | BSD-3-Clause | Copyright (c) 2009 The RE2 Authors |
| JSON for Modern C++ (`thirdparty/json`) | nlohmann/json | MIT | Copyright (c) 2013-2026 Niels Lohmann |
| spdlog (`thirdparty/spdlog`) | spdlog | MIT | Copyright (c) 2016-present Gabi Melman and spdlog contributors |
| {fmt} (vendored inside spdlog, `SPDLOG_FMT_EXTERNAL=OFF`) | fmt | MIT | Copyright (c) 2012-present Victor Zverovich and fmt contributors |
| simdutf (`thirdparty/simdutf`) | simdutf | **MIT** (dual `Apache-2.0 OR MIT`; this distribution elects MIT) | Copyright 2021 The simdutf authors |
| CTRE (`thirdparty/compile-time-regular-expressions`) | compile-time-regular-expressions | Apache-2.0 WITH LLVM-exception | Copyright 2020 Hana Dusíková |
| GoogleTest (`thirdparty/googletest`) | googletest | BSD-3-Clause | Copyright 2008 Google Inc. |
| Verible (`thirdparty/verible`) | verible **v0.0-4302-gc6b29d62**, Apache-2.0 | Copyright 2017-2020 The Verible Authors / Google LLC |
| IETF Trust SHA-256 (`thirdparty/verible/.../common/util/sha256*`) | bundled with Verible | BSD-3-Clause | Copyright (c) 2014, IETF Trust |
| diff-match-patch (`thirdparty/verible/external_libs/editscript*`) | bundled with Verible | Apache-2.0 | Copyright 2018 The diff-match-patch Authors |
| flex/bison **generated** sources (`verible/verilog/parser/verilog.yy.cc`, `verilog-final.tab.cc`) | generated at build time from `verilog.lex` / `verilog.y` | covered by the Verible Apache-2.0 license; generated output is not subject to GPL from Bison's runtime exception | — |

Build-time tools (not distributed): `win_flex` / `win_bison` (winflexbison,
GPL-3.0 with the standard "output not covered" exception).

## Modifications to third-party code

> **This distribution contains modified Verible sources.**
> See [`thirdparty/verible/PATCHES.md`](thirdparty/verible/PATCHES.md) for the
> complete list. In summary: 34 patch markers across
> 17 upstream `.cc`/`.h` files, all of the form "replace a
> cross-`std::string_view` iterator comparison/subtraction with pointer
> arithmetic" so the code is well-defined and builds under MSVC's checked
> iterators; behaviour and output are unchanged. Verible's CMake build files
> under `thirdparty/verible/` are project-authored (upstream builds with Bazel).

## Trademarks

"Anlogic", "TangDynasty" and "TD" are trademarks of 上海安路信息科技股份有限公司
(Shanghai Anlogic Info Technology Co., Ltd.). This project is **not affiliated
with, sponsored by, or endorsed by** Anlogic. The names are used only to
describe interoperability with that vendor's FPGA toolchain. No vendor artwork or
logos are included; all icons are original.
