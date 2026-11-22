#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""门禁：`src/` 里**不允许**再出现把状态写进工程目录的 `.fpgalsp` 路径。

背景（2026-11-22）：用户反复在工程根看到 `.fpgalsp`。根因不是"没有 state_dir()"，
而是**个别调用点硬编码** `<project>/.fpgalsp/...`（最后一个是
`src/td/td_service.cpp` 的 `project.dir + "/.fpgalsp/run"`：每次 LSP 保存触发 TD 检查
就重建一次）。只靠"记住别写"必然会再犯，所以这里做成机械门禁：

  * 扫 `src/**/*.{cpp,h}`，**先剥掉注释**（注释里可以随便提 `.fpgalsp`）；
  * 剩下的代码里只要出现 `.fpgalsp`（允许 `.fpgalsp-tmp` / `.fpgalsp-bak` 这种临时后缀，
    以及"扫描时跳过该目录"的白名单文件），就判失败；
  * 白名单必须写理由（见 ALLOWLIST）。

用法：`python tools/audit/check_project_state_paths.py [--root <repo>]`
退出码：0 = 干净；1 = 有违规（打印 file:line 与原文）。
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

# 允许出现的文件 -> 理由。
# 只允许「识别并跳过/忽略这个目录」或「临时文件后缀」这两类；写入路径一律不允许。
ALLOWLIST = {
    "src/flow/strategy.cpp": "复制工程做策略对比时把 .fpgalsp 排除在副本之外",
    "src/mcp/bridge.cpp": "全库扫描时跳过 .fpgalsp；另有 .fpgalsp-tmp 原子写临时后缀",
    "src/project/al_project.cpp": "扫描工程文件时跳过 .fpgalsp 目录",
    "src/platform/path.cpp": "状态目录的唯一实现处（正常应无字面量；保留以免误删兼容逻辑时报错）",
    "src/platform/write_service.cpp": "统一写服务：错误信息里会提到 .fpgalsp 策略（不是写工程目录）",
}

ALLOWED_SUFFIX = re.compile(r"\.fpgalsp-(tmp|bak)")
TARGET = re.compile(r"\.fpgalsp")


def strip_comments(text: str) -> str:
    """把 C/C++ 注释替换成空格（保留换行，行号不变），粗略处理字符串/字符字面量。"""
    out: list[str] = []
    i = 0
    n = len(text)
    in_line_comment = False
    in_block_comment = False
    in_string: str | None = None
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if in_line_comment:
            if ch == "\n":
                in_line_comment = False
                out.append(ch)
            else:
                out.append(" ")
            i += 1
            continue
        if in_block_comment:
            if ch == "*" and nxt == "/":
                in_block_comment = False
                out.append("  ")
                i += 2
            else:
                out.append("\n" if ch == "\n" else " ")
                i += 1
            continue
        if in_string is not None:
            out.append(ch)
            if ch == "\\":
                if i + 1 < n:
                    out.append(text[i + 1])
                    i += 2
                    continue
            elif ch == in_string:
                in_string = None
            i += 1
            continue
        if ch == "/" and nxt == "/":
            in_line_comment = True
            out.append("  ")
            i += 2
            continue
        if ch == "/" and nxt == "*":
            in_block_comment = True
            out.append("  ")
            i += 2
            continue
        if ch in ('"', "'"):
            in_string = ch
            out.append(ch)
            i += 1
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def scan(root: pathlib.Path) -> list[tuple[str, int, str]]:
    violations: list[tuple[str, int, str]] = []
    src = root / "src"
    for path in sorted(src.rglob("*")):
        if path.suffix not in (".cpp", ".h"):
            continue
        rel = path.relative_to(root).as_posix()
        code = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        allowed = rel in ALLOWLIST
        for number, line in enumerate(code.splitlines(), start=1):
            for match in TARGET.finditer(line):
                tail = line[match.start(): match.start() + 16]
                if ALLOWED_SUFFIX.match(tail):
                    continue
                if allowed:
                    continue
                violations.append((rel, number, line.strip()))
    return violations


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=str(pathlib.Path(__file__).resolve().parents[2]))
    args = parser.parse_args()
    root = pathlib.Path(args.root).resolve()

    violations = scan(root)
    if not violations:
        print("工程状态目录门禁通过：src/ 里没有写工程目录 `.fpgalsp` 的代码")
        return 0

    print("工程状态目录门禁失败：以下代码会把状态写进工程目录的 `.fpgalsp`")
    print("修法：一律用 platform::state_dir(<project.dir>) 再拼子目录（见 src/platform/path.h）")
    for rel, line, text in violations:
        print(f"  {rel}:{line}: {text}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
