#!/usr/bin/env python3
"""codeaction_contention.py —— F1 专项：codeAction 在飞时的 didChange 延迟（串行点探测）。

背景：F1 的 owner 报"codeAction 在飞时 didChange p95 342.5 → 7.14 ms"，但留了一个未归因残余：
即使 lint 命中、文件只有 13 KB，并发下 didChange p95 仍有 5–9 ms，而单独跑只有 0.7–0.9 ms。
他怀疑是 dispatcher / IndexService 的锁竞争 —— 那正是 F2 改的地方。

本脚本用**统一口径**量三组，专门用来判定"残余是不是已经被 F2 解决"：

  A) idle            —— 只打 didChange→completion（无并发），作为下限
  B) codeAction 在飞 —— 一个后台线程持续发 codeAction（13 KB 文件），主线程打同样的 didChange→completion
  C) 两个 codeAction —— 2 个后台线程，进一步加压（看锁竞争是否随并发线性恶化）

并发用 `_ConcurrentMixin`（按 id 分流响应），所以"在飞"是真的在飞，不是排队。

用法：
    python tools/perf/gate/codeaction_contention.py --samples 20 --rounds 3 \
        --out audit/2026-11-19/fix/results/<tag>/codeaction_contention.json
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import threading
import time

CPP = r"Y:/fpga-lsp-cpp"
sys.path.insert(0, os.path.join(CPP, "tools", "audit"))
import bench_servers as bs  # noqa: E402


def did_change_chain(client, spec, samples, version_base):
    """20 次 didChange→completion，返回 (completion_ms 列表, wall_ms 列表)。"""
    anchor = spec["anchor"]
    path = spec["ref_file"]
    uri = anchor["uri"]
    text = bs.read_text(path)
    comp = bs.phase2_params(spec, "textDocument/completion", anchor)
    latencies = []
    walls = []
    for index in range(1, samples + 1):
        text = text.replace("endmodule", "  reg [7:0] ca%d;\nendmodule" % index, 1)
        t0 = time.perf_counter()
        client.notify("textDocument/didChange", {
            "textDocument": {"uri": uri, "version": version_base + index},
            "contentChanges": [{"text": text}]})
        ms, _ = client.collect(client.request_async("textDocument/completion", comp),
                               timeout=1800.0)
        latencies.append(ms)
        walls.append((time.perf_counter() - t0) * 1000.0)
    return latencies, walls


def run_case(client, spec, samples, version_base, background, ca_params, anchor):
    """background = 后台 codeAction 线程数（0 = idle）。"""
    stop = threading.Event()
    counters = {"sent": 0, "errors": 0}

    def hammer():
        round_no = 0
        while not stop.is_set():
            try:
                client.collect(client.request_async("textDocument/codeAction", ca_params),
                               timeout=1800.0)
                counters["sent"] += 1
            except Exception:
                counters["errors"] += 1
                return
            round_no += 1
            if round_no > 10000:
                return

    threads = [threading.Thread(target=hammer, daemon=True) for _ in range(background)]
    for thread in threads:
        thread.start()
    try:
        time.sleep(0.15)          # 让后台请求先进入在飞状态
        latencies, walls = did_change_chain(client, spec, samples, version_base)
    finally:
        stop.set()
        for thread in threads:
            thread.join(timeout=5.0)
    return {
        "background": background,
        "completion_ms": bs.summarize(latencies, keep_samples=True),
        "wall_ms": bs.summarize(walls, keep_samples=False),
        "codeactions_inflight_completed": counters["sent"],
        "codeaction_errors": counters["errors"],
    }


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--files", type=int, default=200)
    parser.add_argument("--samples", type=int, default=20)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--out", required=True)
    args = parser.parse_args(argv)

    os.makedirs(bs.LOG_DIR, exist_ok=True)
    spec = bs.ensure_synth_project(args.files)
    spec["anchor"] = bs.phase2_anchor(spec)
    anchor = spec["anchor"]

    # codeAction 的目标：同文件的模块声明处（F1 的 lint/autofix 路径）
    ca_params = {
        "textDocument": {"uri": anchor["uri"]},
        "range": {"start": anchor["ref_pos"], "end": anchor["ref_pos"]},
        "context": {"diagnostics": []},
    }

    report = {"files": args.files, "samples": args.samples, "rounds": args.rounds,
              "native_exe": bs.native_exe_info(), "rounds_detail": [],
              "cases": {}}
    load = bs.SystemLoad()
    aggregated = {0: [], 1: [], 2: []}
    for rep in range(1, args.rounds + 1):
        # 每轮一个全新会话（避免 round 之间互相污染），顺序 idle→1→2 交替方向
        order = (0, 1, 2) if rep % 2 == 1 else (2, 1, 0)
        for background in order:
            client = bs.ConcurrentNativeLspClient(
                bs.REPO_PY, os.path.join(bs.LOG_DIR, "ca-contention-%d-b%d.log" % (rep, background)))
            try:
                client.initialize(spec["project_dir"], timeout=1800.0)
                client.request("textDocument/documentSymbol", {"textDocument": {"uri": anchor["uri"]}},
                               timeout=1800.0)
                client.open_document(spec["ref_file"], "verilog", bs.read_text(spec["ref_file"]),
                                     version=1)
                for _ in range(3):  # 预热
                    client.request("textDocument/completion",
                                   bs.phase2_params(spec, "textDocument/completion", anchor),
                                   timeout=900.0)
                    client.request("textDocument/codeAction", ca_params, timeout=900.0)
                case = run_case(client, spec, args.samples, 5000 + rep * 100, background,
                                ca_params, anchor)
            finally:
                try:
                    client.close()
                except Exception:
                    pass
            case["rep"] = rep
            report["rounds_detail"].append(case)
            aggregated[background].extend(case["completion_ms"]["samples_ms"])
    load.stop()
    report["cpu_busy"] = load.busy_fraction()
    for background, values in aggregated.items():
        if values:
            report["cases"]["bg%d" % background] = bs.summarize(values, keep_samples=True)
    idle = report["cases"].get("bg0", {}).get("p95")
    for background in (1, 2):
        busy = report["cases"].get("bg%d" % background, {}).get("p95")
        if idle and busy:
            report["cases"]["bg%d" % background]["p95_over_idle"] = round(busy / idle, 2)

    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(report, handle, ensure_ascii=False, indent=1)
    for background in (0, 1, 2):
        case = report["cases"].get("bg%d" % background)
        if case:
            print("csv,codeaction_contention,bg%d,%d,%.3f,%.3f,%.3f" % (
                background, case["n"], case["p50"], case["p95"], case["max"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())