#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""HDL 索引与 Python 参考实现（`fpgalsp/hdl.py`）的**逐字段对比**。

由来：task-17 的验收标准要求"与 Python 逐字段对比一致，或有差异且根因明确"。
本脚本不参与 ctest（依赖 `Y:\\fpga-lsp` 这个 Python 工程存在），是**手工验证工具**：

    python tests/e2e/compare_hdl_python.py --unit build/bin/unit_hdl_index.exe \\
                                           --corpus tests/gate-corpus/corpus_alpha \\
                                           --python-root Y:/fpga-lsp

对比字段：
    modules[]   : name / line / character / end_line / ansi_ports / parameters[]
    ports[]     : name / direction / msb / lsb / width / line / character
    instances[] : module / name / line / character

输出：
    * 一致项计数
    * 差异明细（按类别分组，最多打印 N 条）
    * 已知/可解释差异单独归类（见 EXPECTED_NOTES）
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

# 已知差异的说明（key = 差异类别）
EXPECTED_NOTES = {
    "instance-missing-in-cpp": "C++ 没识别出的例化（通常是 ifdef 分支或非标准写法）",
    "instance-extra-in-cpp": "C++ 多识别出的例化（Python 漏掉的）",
    "module-missing-in-cpp": "C++ 没识别出的模块（多半是 `ifdef 非激活分支：Python 两个分支都算）",
}


def run_cpp_dump(unit: Path, corpus: Path, out_json: Path) -> dict:
    env = dict(os.environ)
    env["FPGALSP_HDL_DUMP"] = str(out_json)
    env["FPGALSP_HDL_DUMP_DIR"] = str(corpus)
    completed = subprocess.run(
        [str(unit), "--gtest_filter=HdlDump.*"],
        capture_output=True,
        env=env,
        timeout=300,
    )
    if not out_json.is_file():
        print(completed.stdout.decode("utf-8", "replace"))
        print(completed.stderr.decode("utf-8", "replace"))
        raise SystemExit("C++ 侧没有产出 dump 文件")
    return json.loads(out_json.read_text(encoding="utf-8"))


def run_python_dump(python_root: Path, corpus: Path) -> dict:
    """用 Python 参考实现解析同一批文件，输出同样的形状。"""
    script = r"""
import json, sys
from pathlib import Path
sys.path.insert(0, sys.argv[1])
from fpgalsp import hdl

corpus = Path(sys.argv[2])
out = {"files": []}
for path in sorted(corpus.rglob("*")):
    if not path.is_file() or path.suffix.lower() not in (".v", ".sv", ".vh", ".svh"):
        continue
    text = path.read_text(encoding="utf-8", errors="replace")
    pf = hdl.parse(text, str(path))
    out["files"].append({
        "path": str(path).replace("\\", "/"),
        "modules": [{
            "name": m["name"], "line": m["line"], "character": m["character"],
            "end_line": m["end_line"], "ansi_ports": bool(m["ansi_ports"]),
            "ports": [{
                "name": p["name"], "direction": p.get("direction") or "",
                "msb": p.get("msb"), "lsb": p.get("lsb"), "width": p.get("width"),
                "line": p["line"], "character": p["character"],
            } for p in m.get("ports") or []],
            "parameters": list(m.get("parameters") or []),
        } for m in pf.get("modules") or []],
        "instances": [{
            "module": i["module"], "name": i.get("name") or "",
            "line": i["line"], "character": i["character"],
        } for i in pf.get("instances") or []],
    })
print(json.dumps(out))
"""
    completed = subprocess.run(
        [sys.executable, "-c", script, str(python_root), str(corpus)],
        capture_output=True,
        timeout=300,
    )
    if completed.returncode != 0:
        print(completed.stderr.decode("utf-8", "replace")[:4000])
        raise SystemExit("Python 侧解析失败")
    return json.loads(completed.stdout.decode("utf-8", "replace"))


def norm(path: str) -> str:
    return path.replace("\\", "/").lower()


def compare(cpp: dict, py: dict, limit: int) -> int:
    cpp_files = {norm(f["path"]): f for f in cpp["files"]}
    py_files = {norm(f["path"]): f for f in py["files"]}

    diffs: list[tuple[str, str]] = []
    same = 0

    def record(category: str, message: str) -> None:
        diffs.append((category, message))

    for key in sorted(set(cpp_files) | set(py_files)):
        if key not in cpp_files:
            record("module-missing-in-cpp", f"{key}: C++ 侧整文件缺失")
            continue
        if key not in py_files:
            record("instance-extra-in-cpp", f"{key}: Python 侧整文件缺失")
            continue
        a = cpp_files[key]
        b = py_files[key]

        a_modules = {m["name"]: m for m in a["modules"]}
        b_modules = {m["name"]: m for m in b["modules"]}
        for name in sorted(set(a_modules) | set(b_modules)):
            if name not in a_modules:
                record("module-missing-in-cpp", f"{key}: module {name}")
                continue
            if name not in b_modules:
                record("module-extra-in-cpp", f"{key}: module {name}")
                continue
            ma, mb = a_modules[name], b_modules[name]
            for field in ("line", "character", "end_line", "ansi_ports", "parameters"):
                if ma[field] != mb[field]:
                    record(f"module-{field}",
                           f"{key}: {name}.{field}: cpp={ma[field]!r} py={mb[field]!r}")
                else:
                    same += 1
            a_ports = {p["name"]: p for p in ma["ports"]}
            b_ports = {p["name"]: p for p in mb["ports"]}
            for pname in sorted(set(a_ports) | set(b_ports)):
                if pname not in a_ports:
                    record("port-missing-in-cpp", f"{key}: {name}.{pname}")
                    continue
                if pname not in b_ports:
                    record("port-extra-in-cpp", f"{key}: {name}.{pname}")
                    continue
                pa, pb = a_ports[pname], b_ports[pname]
                for field in ("direction", "msb", "lsb", "width", "line", "character"):
                    if pa[field] != pb[field]:
                        record(f"port-{field}",
                               f"{key}: {name}.{pname}.{field}: cpp={pa[field]!r} py={pb[field]!r}")
                    else:
                        same += 1

        def inst_key(instance: dict) -> tuple:
            return (instance["module"], instance["name"], instance["line"],
                    instance["character"])

        a_insts = {inst_key(i) for i in a["instances"]}
        b_insts = {inst_key(i) for i in b["instances"]}
        for missing in sorted(b_insts - a_insts):
            record("instance-missing-in-cpp", f"{key}: {missing}")
        for extra in sorted(a_insts - b_insts):
            record("instance-extra-in-cpp", f"{key}: {extra}")
        same += len(a_insts & b_insts)

    print(f"一致项: {same}")
    print(f"差异项: {len(diffs)}")
    if not diffs:
        print("✅ 与 Python 逐字段完全一致")
        return 0

    grouped: dict[str, list[str]] = {}
    for category, message in diffs:
        grouped.setdefault(category, []).append(message)
    for category in sorted(grouped):
        note = EXPECTED_NOTES.get(category, "")
        print(f"\n[{category}] {len(grouped[category])} 项  {note}")
        for message in grouped[category][:limit]:
            print(f"  - {message}")
        if len(grouped[category]) > limit:
            print(f"  ... 另有 {len(grouped[category]) - limit} 项")
    return 1 if diffs else 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--unit", required=True, help="unit_hdl_index.exe 路径")
    parser.add_argument("--corpus", required=True, help="语料目录")
    parser.add_argument("--python-root", default="Y:/fpga-lsp")
    parser.add_argument("--limit", type=int, default=10)
    parser.add_argument("--only-modules", action="store_true", help="只看模块级差异")
    args = parser.parse_args()

    corpus = Path(args.corpus).resolve()
    out_json = Path(os.environ.get("TEMP", ".")) / "fpgalsp_hdl_cpp_dump.json"
    cpp = run_cpp_dump(Path(args.unit).resolve(), corpus, out_json)
    py = run_python_dump(Path(args.python_root), corpus)
    return compare(cpp, py, args.limit)


if __name__ == "__main__":
    sys.exit(main())