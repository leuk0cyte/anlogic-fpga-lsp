# Vendored Verible — Source & Prune Record

## Upstream source

| Field | Value |
| --- | --- |
| Project | Verible (SystemVerilog parser / linter / formatter) |
| Upstream URL | https://github.com/chipsalliance/verible |
| Commit SHA | `c6b29d6251f66f54dd4d42050bab87f01f961f2a` |
| Commit (short) | `c6b29d62` |
| `git describe` | `v0.0-4302-gc6b29d62` |
| Commit subject | Merge pull request #2655 from nigham/linter-unused-var |
| Commit date | 2026-11-16 |
| License | Apache License 2.0 (see `LICENSE` in this directory) |

Full checkout cloned from the upstream URL above, then pruned as described below.
The `.git` directory was removed after this record was written; this vendored tree is
a frozen snapshot and is **not** intended to be updated from upstream or merged into.

## Why this tree is frozen

- This repository (`fpga-lsp-cpp`) builds Verible as a **CMake** subproject for a
  C++23 / MSVC FPGA language-server backend. The upstream Bazel build is not used.
- The vendored sources are treated as a read-only reference implementation of
  SystemVerilog parsing, CST construction, linting and formatting.
- Local changes to the vendored code are expected to be minimal and mechanical
  (build-system adaptation only). Do not "sync to upstream".

## Prune policy

This project deliberately avoids two third-party dependencies:

- **zlib** — not used; the only upstream consumer was `common/util/simple-zip`.
- **protobuf** — not used; the only upstream consumer was the Kythe extractor.

Upstream Bazel wiring related to those components was removed as well.

## Deleted in this prune

### 1. Kythe tool (sole protobuf consumer)

- `verible/verilog/tools/kythe/` — entire directory (111 files): the
  `verible-verilog-kythe-extractor` and `verible-verilog-kythe-kzip-writer` tools,
  `kythe-proto-output.{h,cc}` (the only `google/protobuf` user), Kythe fact
  extractors, scope resolver, indexing facts tree, kzip creator, tests and
  `testdata/`.
- `third_party/proto/kythe/` — vendored Kythe protobuf schemas
  (`analysis.proto`, `storage.proto`, `BUILD`, `LICENSE`, `README.md`).
- `kythe-browse.sh` — top-level convenience script that built and invoked the
  Kythe extractor and the Kythe web UI; useless without the tool above.

### 2. zlib / ZIP support

- `verible/common/util/simple-zip.cc` (only `#include "zlib/include/zlib.h"`)
- `verible/common/util/simple-zip.h`
- `verible/common/util/simple-zip_test.cc`
- `third_party/portable_endian/` (`portable_endian.h`, `BUILD`) — only consumer was
  `simple-zip.cc`.

### 3. Bazel build system (superseded by this repo's CMake)

- `MODULE.bazel`, `.bazelrc`, `bazel/`, `vnames.json`, `.bant-macros`, `shell.nix`,
  `.github/`, `.devcontainer/`, `.pylintrc`, `.readthedocs.yaml`

**All `BUILD` / `*.bzl` files inside the remaining source tree are intentionally
KEPT.** They are the authoritative record of upstream target boundaries, source
lists and dependencies, and are the input for the one-time BUILD → CMake
translation. They are not executed by this project's build.

## Reference-cleanup edits applied to retained BUILD files

- `BUILD` (top level): dropped the two `//verible/verilog/tools/kythe:*` entries
  from the `install-binaries` filegroup.
- `verible/common/analysis/BUILD`: dropped the
  `//verible/verilog/tools/kythe:__pkg__` visibility entry.
- `verible/verilog/CST/BUILD`: dropped the
  `//verible/verilog/tools/kythe:__pkg__` visibility entry.
- `verible/common/util/BUILD`: dropped the `simple-zip` and `simple-zip_test`
  target definitions.

## Kept on purpose

`LICENSE`, `README.md`, `AUTHORS`, `CONTRIBUTING.md`, `doc/`, `img/` (referenced by
`README.md`), `.clang-format`, `.clang-tidy`, `.editorconfig`, `external_libs/`
(in-tree `editscript` dependency), and all source directories under `verible/`.

## Notes on remaining "kythe" mentions

After the prune, the string `kythe` survives only as **comments and prose** in
retained sources (e.g. `verible/verilog/analysis/verilog-project.{h,cc}`,
`verible/verilog/tools/project/project-tool.cc`, and `README.md`). These are not
compiled and carry no dependency.