#!/usr/bin/env python3
"""lint_didchange_probe.py —— G2 判据：服务端 lint 不能阻塞 didChange 的诊断推送。

两个观测量（都必须看，否则会漏掉"lint 虽然不阻塞但很晚"的问题）：

  (a) `didChange → 第一条 publishDiagnostics`：这条衡量**阻塞**（F1 那次事故就是它被拖到 342 ms）。
  (b) `didChange → 带 lint 诊断的 publishDiagnostics`：这条衡量 **lint 延迟**（去抖窗口的代价）。

配置矩阵：13 KB / 100 KB × lint on / lint off，每格 3 轮 × N 样本，报 p50/p95。
lint off 用 `initializationOptions.fpgaLsp.lintOnChange=false`。

用法：
    python tools/perf/gate/lint_didchange_probe.py --samples 20 --out <tagdir>/g2.json
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

CORPUS = os.path.join(CPP, "tests", "gate-corpus", "corpus_alpha")


class DiagClient(NativeLspClient):
    def __init__(self, *args, **kwargs):
        self.marks = []
        super().__init__(*args, **kwargs)

    def _dispatch(self, msg):
        if msg.get("method"):
            self.marks.append((msg["method"], time.perf_counter(),
                               (msg.get("params") or {}).get("uri"),
                               (msg.get("params") or {}).get("diagnostics") or []))
        super()._dispatch(msg)


def prepare(root, pad_kb):
    """13 KB = 原始 top.v；100 KB = 末尾追加注释行（不引入新 lint 违规）。"""
    target = os.path.join(root, "corpus%03d" % pad_kb)
    if os.path.isdir(target):
        shutil.rmtree(target, ignore_errors=True)
    shutil.copytree(CORPUS, target, ignore=shutil.ignore_patterns(".fpgalsp", "*_Runs"))
    top = os.path.join(target, "src", "top.v")
    if pad_kb > 13:
        text = open(top, encoding="utf-8").read()
        pad_line = "// padding line for the 100 KB didChange probe\n"
        need = pad_kb * 1024 - len(text.encode("utf-8"))
        if need > 0:
            text += "\n" + pad_line * (need // len(pad_line) + 1)
            open(top, "w", encoding="utf-8", newline="\n").write(text)
    al = os.path.join(target, "demo_project.al")
    return {"dir": target, "al": al, "top": top,
            "bytes": os.path.getsize(top)}


def measure(spec, lint_on, rounds, samples):
    first_all, lint_all = [], []
    notifications = 0
    for rep in range(1, rounds + 1):
        client = DiagClient(spec["dir"],
                            os.path.join(bs.LOG_DIR, "g2-%s-%d.log" % (lint_on, rep)))
        try:
            client.request("initialize", {
                "processId": os.getpid(),
                "rootUri": path_to_uri(spec["dir"]),
                "clientInfo": {"name": "lint-g2-probe", "version": "1"},
                "capabilities": {"workspace": {"didChangeWatchedFiles": {}}},
                "initializationOptions": {"fpgaLsp": {
                    "tdCheckOnSave": False, "enableTd": False,
                    "lintOnChange": bool(lint_on)}},
            }, timeout=1800.0)
            client.notify("initialized", {})
            uri = path_to_uri(spec["top"])
            client.open_document(spec["top"], "verilog")
            time.sleep(2.0)                      # 等首轮 lint 稳定
            base_text = open(spec["top"], encoding="utf-8").read()
            for index in range(1, samples + 1):
                text = base_text + "\n// edit %d\n" % index
                before = len(client.marks)
                t0 = time.perf_counter()
                client.notify("textDocument/didChange", {
                    "textDocument": {"uri": uri, "version": index + 1},
                    "contentChanges": [{"text": text}]})
                first_ms = None
                lint_ms = None
                deadline = time.perf_counter() + 5.0
                while time.perf_counter() < deadline:
                    for method, at, mark_uri, diags in client.marks[before:]:
                        if method != "textDocument/publishDiagnostics" or mark_uri != uri:
                            continue
                        delta = (at - t0) * 1000.0
                        if first_ms is None:
                            first_ms = delta
                        if any("verible" in str(d.get("source", "")).lower() for d in diags):
                            lint_ms = delta
                    if first_ms is not None and (lint_ms is not None or not lint_on):
                        break
                    time.sleep(0.01)
                notifications += len([m for m in client.marks[before:]
                                      if m[0] == "textDocument/publishDiagnostics"])
                if first_ms is not None:
                    first_all.append(first_ms)
                if lint_ms is not None:
                    lint_all.append(lint_ms)
                # 让下一发不落进同一去抖窗口
                time.sleep(0.35)
        finally:
            try:
                client.close()
            except Exception:
                pass
    return {
        "lint_on": bool(lint_on),
        "first_publish_ms": bs.summarize(first_all, keep_samples=False) if first_all else {"n": 0},
        "lint_publish_ms": bs.summarize(lint_all, keep_samples=False) if lint_all else {"n": 0},
        "publish_notifications": notifications,
    }


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--samples", type=int, default=20)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--out", required=True)
    args = parser.parse_args(argv)
    root = os.path.join(os.environ.get("TEMP", "."), "fpgalsp-gate", "g2")
    os.makedirs(root, exist_ok=True)
    os.makedirs(bs.LOG_DIR, exist_ok=True)
    report = {"native_exe": bs.native_exe_info(), "cases": []}
    load = bs.SystemLoad()
    for pad_kb in (13, 100):
        spec = prepare(root, pad_kb)
        for lint_on in (True, False):
            case = measure(spec, lint_on, args.rounds, args.samples)
            case["file_bytes"] = spec["bytes"]
            case["label"] = "%dKB/lint-%s" % (pad_kb, "on" if lint_on else "off")
            report["cases"].append(case)
            print("csv,lint_g2,%s,first=%s,lint=%s" % (
                case["label"], json.dumps(case["first_publish_ms"]),
                json.dumps(case["lint_publish_ms"])), flush=True)
    load.stop()
    report["cpu_busy"] = load.busy_fraction()
    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(report, handle, ensure_ascii=False, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())