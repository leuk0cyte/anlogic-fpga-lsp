#!/usr/bin/env python3
"""save_diag_latency.py —— W3 的"已知取舍"量化：一次保存后多久出诊断（含 300 ms 去抖）。

W3（TD single-flight + 300 ms 去抖）把 5 连发的 TD 轮数从 3 次 read_hdl 降到 1 次，
代价是**单次保存的诊断会晚约 300 ms**（等去抖窗口）。本脚本用真实 TD 量这个代价：

  * 打开 top.v，等首轮诊断稳定
  * 连续 5 次 didSave，每次都测"保存 → 该文件新的 publishDiagnostics 到达"的耗时
  * 报 p50/p95、以及第 1 次 vs 后续（后续应更短，因为被合并）

用法：
    python tools/perf/gate/save_diag_latency.py --samples 5 --out <tagdir>/save_diag.json
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import time

CPP = r"Y:/fpga-lsp-cpp"
sys.path.insert(0, os.path.join(CPP, "tools", "audit"))
import bench_servers as bs  # noqa: E402
from goldenlib.client import NativeLspClient, path_to_uri  # noqa: E402


class DiagClient(NativeLspClient):
    """记录每条通知到达时间（publishDiagnostics 是通知，基类只存不记时）。"""

    def __init__(self, *args, **kwargs):
        self.marks = []
        super().__init__(*args, **kwargs)

    def _dispatch(self, msg):
        if msg.get("method"):
            self.marks.append((msg["method"], time.perf_counter(),
                               (msg.get("params") or {}).get("uri")))
        super()._dispatch(msg)


def corpus_source():
    """工程语料需自备：本仓库不附带任何工程。

    用 FPGA_LSP_TEST_PROJECT 指向一个工程目录（或直接用 --project）。
    """
    source = os.environ.get("FPGA_LSP_TEST_PROJECT")
    if not source:
        raise SystemExit("需自备工程：请设置 FPGA_LSP_TEST_PROJECT 指向一个工程目录")
    return source


def copy_project(root):
    source = corpus_source()
    target = os.path.join(root, os.path.basename(os.path.normpath(source)))
    if os.path.isdir(target):
        shutil.rmtree(target, ignore_errors=True)
    shutil.copytree(source, target, ignore=shutil.ignore_patterns(".fpgalsp", "*_Runs"))
    al = os.path.join(target, "demo_project.al")
    for name in os.listdir(target):
        if name.endswith(".al"):
            al = os.path.join(target, name)
            break
    return {"dir": target, "al": al, "top": os.path.join(target, "src", "top.v")}


def run(tag, spec, samples):
    client = DiagClient(spec["dir"], os.path.join(bs.LOG_DIR, "save-diag-%s.log" % tag))
    result = {"tag": tag}
    latencies = []
    try:
        client.request("initialize", {
            "processId": os.getpid(),
            "rootUri": path_to_uri(spec["dir"]),
            "clientInfo": {"name": "save-diag-probe", "version": "1"},
            "capabilities": {"workspace": {"didChangeWatchedFiles": {}}},
            "initializationOptions": {"fpgaLsp": {"tdCheckOnSave": True, "enableTd": True}},
        }, timeout=1800.0)
        client.notify("initialized", {})
        uri = path_to_uri(spec["top"])
        client.open_document(spec["top"], "verilog")
        # 等首轮诊断稳定（最多 20 s）
        baseline_marks = len(client.marks)
        deadline = time.perf_counter() + 20.0
        while time.perf_counter() < deadline:
            if any(m[0] == "textDocument/publishDiagnostics" and m[2] == uri
                   for m in client.marks[baseline_marks:]):
                break
            time.sleep(0.05)
        time.sleep(1.5)

        for index in range(1, samples + 1):
            before = len(client.marks)
            t0 = time.perf_counter()
            client.notify("textDocument/didSave", {"textDocument": {"uri": uri}})
            got = None
            deadline = time.perf_counter() + 30.0
            while time.perf_counter() < deadline:
                for method, at, mark_uri in client.marks[before:]:
                    if method == "textDocument/publishDiagnostics" and mark_uri == uri:
                        got = (at - t0) * 1000.0
                        break
                if got is not None:
                    break
                time.sleep(0.02)
            latencies.append(None if got is None else round(got, 1))
            # 给 TD 轮次留出完成时间，避免下一发压在同一轮里
            time.sleep(0.4)
        result["save_to_diag_ms"] = latencies
        result["summary"] = bs.summarize([v for v in latencies if v is not None],
                                         keep_samples=False)
    except Exception:                              # noqa: BLE001
        import traceback
        result["fatal"] = traceback.format_exc()
    finally:
        try:
            client.close()
        except Exception:
            pass
    return result


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--samples", type=int, default=5)
    parser.add_argument("--out", required=True)
    args = parser.parse_args(argv)
    root = os.path.join(os.environ.get("TEMP", "."), "fpgalsp-gate", "w3save")
    os.makedirs(root, exist_ok=True)
    os.makedirs(bs.LOG_DIR, exist_ok=True)
    spec = copy_project(root)
    load = bs.SystemLoad()
    report = {"native_exe": bs.native_exe_info(), "run": run("probe", spec, args.samples)}
    load.stop()
    report["cpu_busy"] = load.busy_fraction()
    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(report, handle, ensure_ascii=False, indent=1)
    print("csv,save_diag,%s" % json.dumps(report["run"].get("summary") or {}, ensure_ascii=False))
    print("csv,save_diag,all,%s" % json.dumps(report["run"].get("save_to_diag_ms"),
                                              ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())