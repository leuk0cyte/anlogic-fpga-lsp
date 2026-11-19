#!/usr/bin/env python
"""didChange 成本交叉验证（p13 的 13 KB 文件 vs p100 的同内容文件）。

为什么单独一个脚本：e2e_diag.py 里两次 didChange 测量（② 与 ⑤ baseline）差了 12×，
    需要确认是"工程规模/冷热"还是"测量方法"造成的。这里用同一套代码分别跑，
    并打印每次的 wall + server CPU 逐样本序列（便于看是否有阶梯/抖动）。
"""

from __future__ import annotations

import os
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, os.path.join(REPO, "tests", "golden", "tools"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from e2e_diag import (PublishRecorder, init_client, process_cpu_ms,  # noqa: E402
                      pct, row)
from goldenlib.client import NativeLspClient, path_to_uri  # noqa: E402


def measure(exe: str, project: str, log_path: str, iters: int, warmup: int,
            followup: bool = False) -> None:
    client = NativeLspClient(REPO, log_path, exe_path=exe)
    init_client(client, project, {"enableTd": False, "tdCheckOnSave": False})
    try:
        recorder = PublishRecorder(client)
        path = os.path.join(project, "src", "small.v")
        uri = path_to_uri(path)
        client.open_document(path, "verilog")
        recorder.wait_for(uri, time.perf_counter(), timeout=120)

        walls: list[float] = []
        cpus: list[float] = []
        version = 2
        for index in range(warmup + iters):
            recorder.clear()
            cpu0 = process_cpu_ms(client.proc.pid)
            started = time.perf_counter()
            client.notify("textDocument/didChange", {
                "textDocument": {"uri": uri, "version": version},
                "contentChanges": [{
                    "range": {"start": {"line": 1, "character": 0},
                              "end": {"line": 1, "character": 0}},
                    "text": " ",
                }],
            })
            version += 1
            event = recorder.wait_for(uri, started, timeout=30)
            wall = (event[0] - started) * 1000.0 if event else float("nan")
            cpu = max(0.0, process_cpu_ms(client.proc.pid) - cpu0)
            if followup:
                # 复现 e2e_diag ② 的做法：每次 didChange 之后再发一个 documentSymbol
                client.request("textDocument/documentSymbol",
                               {"textDocument": {"uri": uri}}, timeout=30)
            if index >= warmup:
                walls.append(wall)
                cpus.append(cpu)
            print(f"  #{index - warmup + 1:02d} wall={wall:7.3f} cpu={cpu:6.3f}")
        print(row(os.path.basename(project) + " didChange wall", walls))
        print(row(os.path.basename(project) + " didChange CPU ", cpus))
    finally:
        client.close()


def main() -> int:
    exe = os.path.join(REPO, "build", "bin", "fpgalsp-native.exe")
    root = os.path.join(os.environ.get("TEMP", "."), "fpgalsp-perf-td")
    print(f"exe={exe} root={root}")
    for project in ("p13", "p100"):
        print(f"== {project} (no follow-up) ==")
        measure(exe, os.path.join(root, project),
                os.path.join(root, "logs", f"didchange_{project}.log"), 20, 3)
    print("== p13 (with follow-up documentSymbol, 复现 e2e ② 的做法) ==")
    measure(exe, os.path.join(root, "p13"),
            os.path.join(root, "logs", "didchange_p13_followup.log"), 20, 3,
            followup=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())