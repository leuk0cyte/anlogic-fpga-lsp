#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把语料工程**隔离**成一次性副本 —— 跑 flow / 综合 / 烧录类测试的唯一正确姿势。

## 为什么必须有这个模块

`tests/gate-corpus/corpus_alpha/demo_project.al` 的根节点是：

```xml
<Project Version="3" Minor="2" Path="C:/work/CorpusAlpha">
```

这个 `Path` **不是纯元数据**：TD 的 `open_project` 会拿它当工程根；工程目录里找不到
某个文件时会**静默回退到该原路径**（见 `tests/PYTHON-BUGS.md` **P-7**）。

后果（都是实测/差点发生的）：

1. `*_Runs` / `.fpgalsp/` 不写进你的临时副本 → 你按 `project.dir` 找产物找不到（假失败）
2. 更糟：**直接写进 `%WORKSPACE%\\CorpusAlpha`**（用户的真实工程）→ 数据损坏

所以"把目录复制一份"**不够**，必须把副本 `.al` 的 `Path` 改掉。

## 三步（缺一不可）

1. **复制**：排除 `.fpgalsp` 与 `*_Runs`
   （被取消的 run 里留着 `.stop.f`，TD 的 flow 每一步都检查它，
   复用旧目录会让 `reset_runs` 行为不确定 → **每次都要全新副本**）
2. **改写 `Path`**：把副本 `.al` 根节点的 `Path` 指向副本目录（本模块的 `_rewrite_project_path`）
3. **跑完断言原目录干净**：源语料目录不能新增 `*_Runs` / `.fpgalsp`；
   声明的真实工程目录在本次运行期间不能有任何写入
   （第 3 步由调用方断言 —— 见 `lsp_smoke.py` 末尾的"语料目录无新增项"/
   "声明的真实工程未被写入"两条 check）

## 用法

```python
from project_isolation import isolated_project

with isolated_project("tests/gate-corpus/corpus_alpha") as (tmp_dir, al_path):
    # al_path 是副本里的 .al，Path 已指向 tmp_dir
    ...  # 在这里跑 flow / 综合
# 退出 with 时整个 tmp_dir 被删除（含 .stop.f）
```

也可以直接传 `.al` 路径：`isolated_project(".../demo_project.al")`。

## 给 C++ 测试的等价实现

`tests/td/flow_integration_test.cpp` 里的 `RewriteProjectPath()` 是本模块的 C++ 版本，
逻辑必须保持一致（改 `.al` 文本里**第一个** `<Project ... Path="...">`）。
两处若有一致性变化，请同步修改并在 PR 里说明。

## 相关

* `tests/gate-corpus/README.md` —— 语料不可修改的原因（PRJ-6619 样本）
* `tests/PYTHON-BUGS.md` P-7 —— `.al` 的 `Path` 语义与 TD 的回退行为
"""

from __future__ import annotations

import os
import re
import shutil
import tempfile
from contextlib import contextmanager
from pathlib import Path
from typing import Iterator, Sequence

# 语料 .al 根节点 Path 指向的"声明工程根"（用于"未被写入"断言）。
# 默认空：工程语料需自备，其 .al 里声明的 Path 各不相同，无法预置。
# 需要该断言的调用方自行传入，或设环境变量 FPGA_LSP_DECLARED_PROJECT_ROOTS
# （多个路径用 os.pathsep 分隔）。
DECLARED_PROJECT_ROOTS: tuple[str, ...] = tuple(
    p for p in os.environ.get("FPGA_LSP_DECLARED_PROJECT_ROOTS", "").split(os.pathsep) if p
)

DEFAULT_EXCLUDES = (".fpgalsp", "*_Runs")


def _resolve_al(source: Path) -> tuple[Path, Path]:
    """source 可以是工程目录或 .al 文件；返回 (工程目录, .al 路径)。"""
    if source.is_dir():
        candidates = sorted(source.glob("*.al"))
        if not candidates:
            raise SystemExit(f"隔离失败：{source} 下没有 .al")
        if len(candidates) > 1:
            # 多工程时取第一个（与 server 的"目录里唯一 .al"约定一致）
            pass
        return source, candidates[0]
    return source.parent, source


def _rewrite_project_path(al_path: Path, new_root: Path) -> None:
    """把 .al 文本里**第一个** `<Project ... Path="...">` 的 Path 改成 new_root。

    只改第一个：根节点才是工程根，`<File Path="...">` 都是相对路径不能动。
    """
    text = al_path.read_text(encoding="utf-8", errors="replace")
    new_root_posix = new_root.as_posix()
    text, count = re.subn(
        r'(<Project[^>]*\sPath=")[^"]*(")',
        lambda match: match.group(1) + new_root_posix + match.group(2),
        text,
        count=1,
    )
    if count == 0:
        raise SystemExit("隔离失败：.al 里找不到根节点的 Path 属性")
    al_path.write_text(text, encoding="utf-8")


def _copy_tree(source_dir: Path, dest_dir: Path, excludes: Sequence[str]) -> None:
    patterns = tuple(excludes)
    for entry in sorted(source_dir.iterdir()):
        if any(entry.match(pattern) for pattern in patterns):
            continue
        target = dest_dir / entry.name
        if entry.is_dir():
            shutil.copytree(entry, target, ignore=shutil.ignore_patterns(*patterns))
        else:
            shutil.copy2(entry, target)


@contextmanager
def isolated_project(
    source_al_or_dir: str | Path,
    exclude: Sequence[str] = DEFAULT_EXCLUDES,
) -> Iterator[tuple[Path, Path]]:
    """复制工程到临时目录 + 改写 `.al` 的根 `Path` + 用完删除。

    yield `(tmp_dir, al_path)`；`al_path` 的 `Path` 已指向 `tmp_dir`。

    ⚠ 每次调用都是**全新**副本：不要复用上一次的目录（被取消的 run 里 `.stop.f`
      会让 TD 的后续行为不确定）。
    """
    source = Path(source_al_or_dir).resolve()
    source_dir, source_al = _resolve_al(source)

    tmp_dir = Path(tempfile.mkdtemp(prefix="fpgalsp-isolated-"))
    try:
        _copy_tree(source_dir, tmp_dir, exclude)
        al_path = tmp_dir / source_al.name
        _rewrite_project_path(al_path, tmp_dir)
        yield tmp_dir, al_path
    finally:
        shutil.rmtree(tmp_dir, ignore_errors=True)


def declared_projects_touched(since: float) -> list[str]:
    """返回 `DECLARED_PROJECT_ROOTS` 里在 `since`（epoch 秒）之后被写入的路径。

    用于"隔离没失效"的断言：**正常情况下应该恒为空**。
    """
    touched: list[str] = []
    for root in DECLARED_PROJECT_ROOTS:
        base = Path(root)
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            try:
                if path.stat().st_mtime > since - 1.0:
                    touched.append(str(path))
            except OSError:
                continue
    return touched


def diff_listing(path: str | Path, before: set[str]) -> tuple[set[str], set[str]]:
    """返回 (新增项, 消失项) —— 用于"语料目录无新增"断言。"""
    try:
        now = {entry.name for entry in Path(path).iterdir()}
    except OSError:
        now = set()
    return now - before, before - now


def snapshot_listing(path: str | Path) -> set[str]:
    try:
        return {entry.name for entry in Path(path).iterdir()}
    except OSError:
        return set()