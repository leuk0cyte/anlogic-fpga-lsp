#!/usr/bin/env python3
"""watcher_probe.py —— 验证"没有 watched-files 通知时，每个请求都要付一次目录 stamp 校验"。

假设（C 级，src/server/index_service.cpp:141 + lsp_server.cpp:424）：
  IndexService 默认 watcher_active=false，`Get()` 每次做 ValidateStampsLocked()
  （一次目录枚举带回整目录 size/mtime）；只有**客户端真的发过一条**
  `workspace/didChangeWatchedFiles` 之后才会 SetWatcherActive(true)。

做法：同一个会话里，先测一组 hover/documentSymbol（无 watcher），
再发一条空的 didChangeWatchedFiles（列表为空，但足以置位），再测同一组。
两组的比值就是"目录校验"的净成本。

用法：python tools/perf/latency/watcher_probe.py --sizes 200,1000 --samples 30
"""

from __future__ import annotations

import argparse
import json
import os
import sys

CPP = r"Y:/fpga-lsp-cpp"
sys.path.insert(0, os.path.join(CPP, "tools", "audit"))
import bench_servers as bs  # noqa: E402

OUT_DIR = os.path.join(os.environ["TEMP"], "fpgalsp-perf", "latency")
METHODS = ["textDocument/hover", "textDocument/documentSymbol", "textDocument/completion"]


def measure(client, spec, samples):
    out = {}
    for method in METHODS:
        params = bs.phase2_params(spec, method, spec["anchor"])
        for _ in range(3):
            client.request(method, params, timeout=1800.0)
        values = []
        for _ in range(samples):
            t0 = bs.time.perf_counter()
            client.request(method, params, timeout=1800.0)
            values.append((bs.time.perf_counter() - t0) * 1000.0)
        out[method] = bs.summarize(values, keep_samples=False)
    return out


def run(size, samples):
    spec = bs.ensure_synth_project(size)
    spec["anchor"] = bs.phase2_anchor(spec)
    client = bs.make_lsp_client("native", "watcher-%03d.log" % size)
    result = {"files": size, "samples": samples}
    try:
        load = bs.SystemLoad()
        client.initialize(spec["project_dir"], timeout=1800.0)
        client.request("textDocument/documentSymbol",
                       bs.phase2_params(spec, "textDocument/documentSymbol", spec["anchor"]),
                       timeout=1800.0)
        result["without_watcher"] = measure(client, spec, samples)
        # 一条空的 watched-files 通知：只为把 watcher_active 置 true
        client.notify("workspace/didChangeWatchedFiles", {"changes": []})
        bs.time.sleep(0.2)
        result["with_watcher"] = measure(client, spec, samples)
        load.stop()
        result["cpu_busy"] = load.busy_fraction()
    finally:
        try:
            client.close()
        except Exception:
            pass
    for method in METHODS:
        a = result["without_watcher"][method]["p50"]
        b = result["with_watcher"][method]["p50"]
        result.setdefault("ratio", {})[method] = round(a / max(0.001, b), 2)
    return result


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--sizes", default="200,1000")
    parser.add_argument("--samples", type=int, default=30)
    parser.add_argument("--out", default=os.path.join(OUT_DIR, "watcher.json"))
    args = parser.parse_args(argv)
    os.makedirs(OUT_DIR, exist_ok=True)
    os.makedirs(bs.LOG_DIR, exist_ok=True)
    report = {"meta": {"exe": bs.native_exe_info(),
                       "captured_at": bs.time.strftime("%Y-%m-%dT%H:%M:%S%z")},
              "sizes": []}
    for size in [int(x) for x in args.sizes.split(",") if x.strip()]:
        sys.stderr.write("[watcher] files=%d\n" % size)
        report["sizes"].append(run(size, args.samples))
        row = report["sizes"][-1]
        print("csv,watcher,%d,%s" % (size, json.dumps(
            {"ratio": row.get("ratio"), "cpu": row.get("cpu_busy")}, ensure_ascii=False)))
    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(report, handle, ensure_ascii=False, indent=1)
    sys.stderr.write("[watcher] wrote %s\n" % args.out)
    print(json.dumps(report, ensure_ascii=False, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())