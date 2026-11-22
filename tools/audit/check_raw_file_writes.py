#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""门禁：`src/` 里不允许再直接写文件（统一走 platform::write_service）。

背景（2026-11-22 用户反馈）：写缓存 / 写日志 / 改工程文件的代码散落在 flow / probe /
analysis / server / mcp / td，每处自己拼路径、自己决定是否原子写。以后要改"写哪、怎么写、
写坏了怎么办"就会漏改（工程根反复长出 `.fpgalsp` 就是这么来的）。

规则：
  * 扫 `src/**/*.{cpp,h}`，先剥掉注释（注释里可以随便提）；
  * 代码里出现 `write_file_utf8(` 或 `std::ofstream` 一律判违规；
  * 白名单只有三个文件（列表本身即理由）：统一写服务实现、编码实现、日志实现。

修法：调用 `platform::writes::Write(project_dir, path, bytes, {.caller="..."})` 或
`AppendLine(...)`，详见 doc/design/write-service.md。

用法：`python tools/audit/check_raw_file_writes.py [--root <repo>]`
退出码：0 = 干净；1 = 有违规（打印 file:line 与原文）。
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

# 允许直接写文件的文件 -> 理由。
ALLOWLIST = {
    "src/platform/write_service.cpp": "统一写服务的实现（原子写/审计都在这一层）",
    "src/platform/encoding.h": "write_file_utf8 的**声明**（不是调用）",
    "src/platform/encoding.cpp": "UTF-8 写文件的最底层实现",
    "src/platform/log.cpp": "日志 sink 的实现（日志本身是统一入口）",
}

PATTERNS = (
    re.compile(r"write_file_utf8\s*\("),
    re.compile(r"std::ofstream"),
)


def strip_comments(text: str) -> str:
    """把 C/C++ 注释替换成空格（保留换行，行号不变），粗略处理字符串字面量。"""
    out: list[str] = []
    i = 0
    n = len(text)
    in_line = False
    in_block = False
    string: str | None = None
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if in_line:
            if ch == "\n":
                in_line = False
                out.append(ch)
            else:
                out.append(" ")
            i += 1
            continue
        if in_block:
            if ch == "*" and nxt == "/":
                in_block = False
                out.append("  ")
                i += 2
            else:
                out.append("\n" if ch == "\n" else " ")
                i += 1
            continue
        if string is not None:
            out.append(ch)
            if ch == "\\":
                if i + 1 < n:
                    out.append(text[i + 1])
                    i += 2
                    continue
            elif ch == string:
                string = None
            i += 1
            continue
        if ch == "/" and nxt == "/":
            in_line = True
            out.append("  ")
            i += 2
            continue
        if ch == "/" and nxt == "*":
            in_block = True
            out.append("  ")
            i += 2
            continue
        if ch in ('"', "'"):
            string = ch
            out.append(ch)
            i += 1
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def scan(root: pathlib.Path) -> list[tuple[str, int, str]]:
    violations: list[tuple[str, int, str]] = []
    for path in sorted((root / "src").rglob("*")):
        if path.suffix not in (".cpp", ".h"):
            continue
        rel = path.relative_to(root).as_posix()
        if rel in ALLOWLIST:
            continue
        code = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        for number, line in enumerate(code.splitlines(), start=1):
            for pattern in PATTERNS:
                if pattern.search(line):
                    violations.append((rel, number, line.strip()))
                    break
    return violations


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=str(pathlib.Path(__file__).resolve().parents[2]))
    args = parser.parse_args()
    root = pathlib.Path(args.root).resolve()

    violations = scan(root)
    if not violations:
        print("统一写入门禁通过：src/ 的文件写入都经过 platform::write_service")
        return 0

    print("统一写入门禁失败：以下代码在绕过 platform::write_service 直接写文件")
    print("修法：platform::writes::Write(project_dir, path, bytes, {.caller=\"...\"})，")
    print("      见 doc/design/write-service.md")
    for rel, line, text in violations:
        print(f"  {rel}:{line}: {text}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
