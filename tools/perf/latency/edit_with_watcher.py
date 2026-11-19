#!/usr/bin/env python3
"""edit_with_watcher.py —— 追问：把 watcher 置位后，"按键→completion"还剩多少？

watcher_probe.py 证明"无 watched-files 通知时每请求多付一次目录 stamp 校验"
（@1000 hover 5.26 → 0.81 ms）。本脚本把同一现象放到**按键链路上**量一次：
同一次会话里 20 次 didChange→completion，先在无 watcher 状态下跑，再发一条
空 didChangeWatchedFiles 置位后重跑。差值 = 交互关键路径上可省掉的部分。

用法：python tools/perf/latency/edit_with_watcher.py --sizes 200,1000 --samples 20
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time

CPP = r"Y:/fpga-lsp-cpp"
sys.path.insert(0, os.path.join(CPP, "tools", "audit"))
import bench_servers as bs  # noqa: E402

OUT_DIR = os.path.join(os.environ["TEMP"], "fpgalsp-perf", "latency")


def edits(client, spec, samples, version_base):
    anchor = spec["anchor"]
    path = spec["ref_file"]
    uri = anchor["uri"]
    text = bs.read_text(path)
    comp = bs.phase2_params(spec, "textDocument/completion", anchor)
    rows = []
    for index in range(1, samples + 1):
        text = text.replace("endmodule", "  reg [7:0] w%d;\nendmodule" % index, 1)
        t0 = time.perf_counter()
        client.notify("textDocument/didChange", {
            "textDocument": {"uri": uri, "version": version_base + index},
            "contentChanges": [{"text": text}]})
        ms, _ = client.collect(client.request_async("textDocument/completion", comp))
        rows.append({"edit": index, "completion_ms": round(ms, 3),
                     "wall_ms": round((time.perf_counter() - t0) * 1000.0, 3)})
    return rows


def run(size, samples):
    spec = bs.ensure_synth_project(size)
    spec["anchor"] = bs.phase2_anchor(spec)
    client = bs.ConcurrentNativeLspClient(bs.REPO_PY,
                                          os.path.join(bs.LOG_DIR, "editwt-%03d.log" % size))
    out = {"files": size, "samples": samples}
    try:
        load = bs.SystemLoad()
        client.initialize(spec["project_dir"], timeout=1800.0)
        client.request("textDocument/documentSymbol",
                       bs.phase2_params(spec, "textDocument/documentSymbol", spec["anchor"]),
                       timeout=1800.0)
        client.open_document(spec["ref_file"], "verilog", bs.read_text(spec["ref_file"]),
                             version=1)
        for _ in range(3):
            client.request("textDocument/documentSymbol",
                           {"textDocument": {"uri": spec["anchor"]["uri"]}}, timeout=900.0)
        out["no_watcher_rows"] = edits(client, spec, samples, 1000)
        client.notify("workspace/didChangeWatchedFiles", {"changes": []})
        time.sleep(0.2)
        out["with_watcher_rows"] = edits(client, spec, samples, 2000)
        load.stop()
        out["cpu_busy"] = load.busy_fraction()
    finally:
        try:
            client.close()
        except Exception:
            pass
    for key in ("no_watcher", "with_watcher"):
        values = [row["completion_ms"] for row in out[key + "_rows"]]
        out[key] = bs.summarize(values, keep_samples=False)
    out["ratio"] = round(out["no_watcher"]["p50"] / max(0.01, out["with_watcher"]["p50"]), 2)
    return out


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--sizes", default="200,1000")
    parser.add_argument("--samples", type=int, default=20)
    parser.add_argument("--out", default=os.path.join(OUT_DIR, "edit_watcher.json"))
    args = parser.parse_args(argv)
    os.makedirs(OUT_DIR, exist_ok=True)
    os.makedirs(bs.LOG_DIR, exist_ok=True)
    report = {"meta": {"exe": bs.native_exe_info(),
                       "captured_at": time.strftime("%Y-%m-%dT%H:%M:%S%z")}, "sizes": []}
    for size in [int(x) for x in args.sizes.split(",") if x.strip()]:
        sys.stderr.write("[edit-watcher] files=%d\n" % size)
        row = run(size, args.samples)
        report["sizes"].append(row)
        print("csv,edit_watcher,%d,%s" % (size, json.dumps(
            {"no_watcher_p50": row["no_watcher"]["p50"],
             "no_watcher_p95": row["no_watcher"]["p95"],
             "with_watcher_p50": row["with_watcher"]["p50"],
             "with_watcher_p95": row["with_watcher"]["p95"],
             "ratio": row["ratio"], "cpu": row.get("cpu_busy")}, ensure_ascii=False)))
    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(report, handle, ensure_ascii=False, indent=1)
    sys.stderr.write("[edit-watcher] wrote %s\n" % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())