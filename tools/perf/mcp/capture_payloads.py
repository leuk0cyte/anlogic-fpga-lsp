#!/usr/bin/env python3
"""capture_payloads.py —— 抓 MCP 工具 payload 的逐字节快照（F4 协议不变验证）。

用同一组固定参数对 `project_list` / `search_text` 取响应，`json.dumps(sort_keys=False)`
后的文本直接入库；改动前后各跑一次，`diff` 文本即可证明"同参数 payload 逐字节一致"。

用法：
    python tools/perf/mcp/capture_payloads.py --out tools/perf/mcp/out/payload-before.json
    python tools/perf/mcp/capture_payloads.py --out tools/perf/mcp/out/payload-after.json
    python tools/perf/mcp/capture_payloads.py --diff 前.json 后.json
"""

from __future__ import annotations

import argparse
import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, os.path.join(REPO, "tests", "golden", "tools"))
os.environ.setdefault("PYTHONPATH", r"Y:\fpga-lsp")

from goldenlib.client import NativeMcpClient  # noqa: E402

ROOT = os.path.join(os.environ.get("TEMP", r"C:\Windows\Temp"), "fpgalsp-perf", "mcp", "proj200")

CASES = [
    ("project_list_all", "fpga_project_list", {}),
    ("project_list_root", "fpga_project_list", {"root": ROOT}),
    ("search_text_always", "fpga_search_text", {"query": "always", "limit": 200}),
    ("search_text_m00", "fpga_search_text", {"query": "m00", "limit": 200}),
    ("search_text_regex", "fpga_search_text", {"query": "m0+1", "regex": True, "limit": 50}),
    ("search_text_glob_context", "fpga_search_text",
     {"query": "module", "glob": "*.v", "limit": 100, "context": 2}),
    ("search_text_path", "fpga_search_text",
     {"query": "mod", "path": os.path.join(ROOT, "src", "top.v"), "limit": 20}),
    ("search_text_case", "fpga_search_text",
     {"query": "MODULE", "case_sensitive": True, "limit": 20}),
    ("search_text_truncated", "fpga_search_text", {"query": "e", "limit": 5}),
]


def capture(out_path: str) -> int:
    cwd = os.path.join(os.environ.get("TEMP", "."), "fpgalsp-perf", "cwd")
    os.makedirs(cwd, exist_ok=True)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    client = NativeMcpClient(cwd, ROOT, os.path.join(os.environ.get("TEMP", "."),
                                                     "fpgalsp-perf", "mcp", "capture.log"))
    payloads = {}
    try:
        for name, tool, args in CASES:
            result, is_error = client.call_tool(tool, args, timeout=300)
            payloads[name] = {"tool": tool, "arguments": args, "isError": is_error,
                              "bytes": json.dumps(result, ensure_ascii=False, sort_keys=False)}
    finally:
        client.close()
    with open(out_path, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(payloads, handle, ensure_ascii=False, indent=2, sort_keys=True)
        handle.write("\n")
    print("wrote %s (%d cases)" % (out_path, len(payloads)))
    return 0


def diff(before_path: str, after_path: str) -> int:
    before = json.load(open(before_path, encoding="utf-8"))
    after = json.load(open(after_path, encoding="utf-8"))
    failures = 0
    for name in sorted(set(before) | set(after)):
        left = before.get(name, {}).get("bytes")
        right = after.get(name, {}).get("bytes")
        if left == right:
            print("SAME  %-28s %d bytes" % (name, len(left or "")))
        else:
            failures += 1
            print("DIFF  %-28s before=%d after=%d" % (name, len(left or ""), len(right or "")))
            if left and right:
                for index, (a, b) in enumerate(zip(left, right)):
                    if a != b:
                        print("      first difference at char %d:" % index)
                        print("        before: %r" % left[max(0, index - 60):index + 60])
                        print("        after : %r" % right[max(0, index - 60):index + 60])
                        break
    print("\n%s" % ("ALL SAME" if failures == 0 else "%d case(s) differ" % failures))
    return 0 if failures == 0 else 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default="")
    parser.add_argument("--diff", nargs=2, metavar=("BEFORE", "AFTER"))
    args = parser.parse_args()
    if args.diff:
        return diff(args.diff[0], args.diff[1])
    if not args.out:
        parser.error("--out or --diff required")
    return capture(args.out)


if __name__ == "__main__":
    sys.exit(main())