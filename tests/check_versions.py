#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""版本号一致性门禁：CMake project VERSION / src/platform/version.h / 扩展 package.json。

为什么需要：0.21.x 之前原生 exe 自报 0.1.0（`--help`、LSP `serverInfo.version`、
MCP `serverInfo.version` 三处），而扩展是 0.21.9 —— 用户报 bug 时对不上号，审计
（2026-11-22）也把它记为"版本叙事分裂"。现在三处必须一致，任何一处漏改都会红。

用法：python tests/check_versions.py [--root <repo>]
退出码：0 = 一致；1 = 不一致（打印三处实际值）。
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys


def read_cmake_version(root: pathlib.Path) -> str:
    text = (root / "CMakeLists.txt").read_text(encoding="utf-8", errors="replace")
    match = re.search(r"project\(\s*fpga-lsp-cpp\s+VERSION\s+([0-9][0-9A-Za-z.\-]*)", text)
    return match.group(1) if match else ""


def read_header_version(root: pathlib.Path) -> str:
    path = root / "src" / "platform" / "version.h"
    if not path.is_file():
        return ""
    text = path.read_text(encoding="utf-8", errors="replace")
    match = re.search(r'kServerVersion\s*=\s*"([^"]+)"', text)
    return match.group(1) if match else ""


def read_extension_version(root: pathlib.Path) -> str:
    path = root / "vscode-extension" / "package.json"
    if not path.is_file():
        return ""
    return json.loads(path.read_text(encoding="utf-8")).get("version", "")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=str(pathlib.Path(__file__).resolve().parents[1]))
    args = parser.parse_args()
    root = pathlib.Path(args.root).resolve()

    values = {
        "CMakeLists.txt project VERSION": read_cmake_version(root),
        "src/platform/version.h kServerVersion": read_header_version(root),
        "vscode-extension/package.json version": read_extension_version(root),
    }
    missing = [name for name, value in values.items() if not value]
    if missing:
        print("版本门禁：以下位置没有解析到版本号 -> " + ", ".join(missing))
        return 1

    unique = set(values.values())
    for name, value in values.items():
        print(f"  {name}: {value}")
    if len(unique) != 1:
        print("版本门禁失败：三处版本号不一致（发布时一起改）")
        return 1

    print(f"版本门禁通过：{values['CMakeLists.txt project VERSION']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
