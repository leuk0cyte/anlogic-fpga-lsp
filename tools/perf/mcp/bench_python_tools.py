#!/usr/bin/env python3
"""bench_python_tools.py —— 同一份 200 文件合成工程上，Python 参考实现 vs native 的 MCP 工具延迟。

为什么需要：`doc/audit/2026-11-18/performance/BENCH.md` §6 的 4.30 vs 3.73 ms 是**第一阶段**
（exe ccdacb9）在 21 文件真实工程上的数；第二阶段改了 IndexService/ReportStore/AsyncDispatcher，
必须今天重测。本脚本让两侧跑**同一个合成工程、同一组固定参数**，只比工具延迟。

- Python 侧：`python -m fpgalsp.mcp --root <proj>`
- native 侧：`build/bin/fpgalsp-native.exe --mcp --root <proj>`（生产 exe，不覆盖）

用法：
    python tools/perf/mcp/bench_python_tools.py --iters 20 --warmup 3
    python tools/perf/mcp/bench_python_tools.py --out tools/perf/mcp/out/python_vs_native.json

输出：每个工具一行 `p50/p95 + bytes`，以及 `csv,` 行；JSON 落到 --out。
"""

from __future__ import annotations

import argparse
import json
import os
import statistics
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, os.path.join(REPO, "tests", "golden", "tools"))
os.environ.setdefault("PYTHONPATH", r"Y:\fpga-lsp")
sys.path.insert(0, r"Y:\fpga-lsp")

from goldenlib.client import McpClient, NativeMcpClient  # noqa: E402

DEFAULT_ROOT = os.path.join(os.environ.get("TEMP", r"C:\Windows\Temp"), "fpgalsp-perf", "mcp", "proj200")

# 与 probe_mcp_bridge.cpp 的 tools 节同一组参数（固定、可复现）
TOOLS = [
    ("fpga_project_info", {}),
    ("fpga_project_files", {"kind": "hdl", "include_excluded": True}),
    ("fpga_project_structure", {}),
    ("fpga_workspace_symbols", {"query": "m00", "limit": 200}),
    ("fpga_search_hierarchy", {"query": "m00", "limit": 200}),
    ("fpga_references", {"file": "src/top.v", "line": 1, "column": 8, "include_declaration": True}),
    ("fpga_completion", {"file": "src/top.v", "line": 1, "column": 1, "limit": 200}),
    ("fpga_search_text", {"query": "always", "limit": 200}),
    ("fpga_read_file", {"file": "src/top.v", "offset": 1, "limit": 200}),
]


def module_position(root: str) -> tuple[int, int]:
    """top.v 里第一个 `module` 的行列（1-based），与 C++ 探针一致。"""
    path = os.path.join(root, "src", "top.v")
    try:
        lines = open(path, encoding="utf-8", errors="replace").read().split("\n")
    except OSError:
        return 1, 1
    for index, line in enumerate(lines):
        stripped = line.lstrip()
        if stripped.startswith("module "):
            return index + 1, len(line) - len(stripped) + 1
    return 1, 1


def measure(client, name: str, arguments: dict, iters: int, warmup: int) -> dict:
    for _ in range(warmup):
        client.call_tool(name, arguments, timeout=300)
    samples = []
    payload_bytes = 0
    for _ in range(iters):
        started = time.perf_counter()
        result, _is_error = client.call_tool(name, arguments, timeout=300)
        samples.append((time.perf_counter() - started) * 1000.0)
        payload_bytes = len(json.dumps(result, ensure_ascii=False).encode("utf-8"))
    samples.sort()
    p50 = statistics.median(samples)
    p95 = samples[min(len(samples) - 1, int(round(0.95 * (len(samples) - 1))))]
    return {
        "tool": name,
        "iters": iters,
        "p50_ms": round(p50, 3),
        "p95_ms": round(p95, 3),
        "max_ms": round(samples[-1], 3),
        "bytes": payload_bytes,
        "us_per_kb": round(p50 * 1000.0 / (payload_bytes / 1024.0), 2) if payload_bytes else None,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Python vs native MCP 工具延迟（同一合成工程）")
    parser.add_argument("--root", default=DEFAULT_ROOT)
    parser.add_argument("--exe", default=os.path.join(REPO, "build", "bin", "fpgalsp-native.exe"))
    parser.add_argument("--iters", type=int, default=20)
    parser.add_argument("--warmup", type=int, default=3)
    parser.add_argument("--log-dir", default=os.path.join(os.environ.get("TEMP", "."), "fpgalsp-perf", "mcp", "logs"))
    parser.add_argument("--out", default=os.path.join(REPO, "tools", "perf", "mcp", "out", "python_vs_native.json"))
    args = parser.parse_args()

    if not os.path.isdir(args.root):
        print("missing synthetic project: %s" % args.root)
        return 2
    os.makedirs(args.log_dir, exist_ok=True)
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    root = args.root
    line, column = module_position(root)

    cwd = os.path.join(os.environ.get("TEMP", "."), "fpgalsp-perf", "cwd")
    os.makedirs(cwd, exist_ok=True)

    report = {"root": root, "iters": args.iters, "warmup": args.warmup, "sides": {}}
    for side, factory in (
        ("python", lambda: McpClient(r"Y:\fpga-lsp", root, os.path.join(args.log_dir, "py.log"))),
        ("native", lambda: NativeMcpClient(cwd, root, os.path.join(args.log_dir, "native.log"),
                                           exe_path=args.exe)),
    ):
        client = factory()
        rows = []
        print("=== %s ===" % side)
        print("%-30s %8s %8s %9s" % ("tool", "p50/ms", "p95/ms", "bytes"))
        for name, arguments in TOOLS:
            args_for_call = dict(arguments)
            if "file" in args_for_call:
                args_for_call["file"] = os.path.join(root, args_for_call["file"].replace("/", os.sep))
                args_for_call.pop("line", None)
                args_for_call.pop("column", None)
                if name == "fpga_references":
                    args_for_call["line"] = line
                    args_for_call["column"] = column
            row = measure(client, name, args_for_call, args.iters, args.warmup)
            rows.append(row)
            print("%-30s %8.3f %8.3f %9d" % (row["tool"], row["p50_ms"], row["p95_ms"], row["bytes"]))
            print("csv,%s,%s,%d,%.4f,%.4f,%.4f,%d" % (side, row["tool"], args.iters,
                                                       row["p50_ms"], row["p95_ms"], row["max_ms"],
                                                       row["bytes"]))
        client.close()
        report["sides"][side] = rows

    with open(args.out, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(report, handle, ensure_ascii=False, indent=2)
        handle.write("\n")
    print("\nwrote %s" % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())