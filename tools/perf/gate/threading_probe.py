#!/usr/bin/env python3
"""threading_probe.py —— R9a（交互请求进 worker 池）专项：线程模型改动最容易破的三件事。

R9a 动的是"谁在哪个线程上跑"，所以只看延迟不够，必须同时验证：

  A) **并发快请求不被重请求拖住**：1 个 reports 在飞 + 10 个并发 hover，
     报 hover 的 p50/p95/max 与"空闲时 10 个并发 hover"的对照（比值）。
  B) **顺序正确性**：`didChange` 之后**立刻**发 documentSymbol，必须看到新内容。
     这是并发化最容易破的一条（请求被扔进池后可能先于通知执行）。20 次为 1 轮，报违例次数。
  C) **无死锁/无卡死**：连续 200 个混合请求（hover/documentSymbol/completion/
     moduleTree/workspaceSymbol 轮转，其中每 20 个夹一个并发小批），
     整体必须在看门狗时间内完成；一旦超时**立即停止并抛出**（Lead 要求直接停手报告）。

用法：
    python tools/perf/gate/threading_probe.py --samples 20 --out <tagdir>/threading.json
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

MIXED_METHODS = [
    "textDocument/hover",
    "textDocument/documentSymbol",
    "textDocument/completion",
    "fpgaLsp/moduleTree",
    "workspace/symbol",
]


class HangDetected(RuntimeError):
    pass


def with_watchdog(fn, seconds, label):
    """在子线程里跑 fn，超过 seconds 秒没回来就当卡死。"""
    box = {}

    def runner():
        try:
            box["value"] = fn()
        except Exception as exc:          # noqa: BLE001 - 原样带回
            box["error"] = exc

    thread = threading.Thread(target=runner, daemon=True)
    thread.start()
    thread.join(timeout=seconds)
    if thread.is_alive():
        raise HangDetected("%s 超过 %.0fs 未完成（疑似死锁/卡死，已停止）" % (label, seconds))
    if "error" in box:
        raise box["error"]
    return box.get("value")


def case_concurrency(client, spec, anchor, fast=10, rounds=3):
    hover = bs.phase2_params(spec, "textDocument/hover", anchor)
    reports = bs.phase2_params(spec, "fpgaLsp/reports", anchor)
    idle_all, busy_all, heavy_all = [], [], []
    for _ in range(rounds):
        idle = [client.collect(client.request_async("textDocument/hover", hover))[0]
                for _ in range(fast)]
        heavy = client.request_async("fpgaLsp/reports", reports)
        busy = [client.collect(client.request_async("textDocument/hover", hover))[0]
                for _ in range(fast)]
        heavy_ms, _payload = client.collect(heavy, timeout=900.0)
        idle_all.extend(idle)
        busy_all.extend(busy)
        heavy_all.append(heavy_ms)
    result = {
        "idle_hover": bs.summarize(idle_all, keep_samples=True),
        "busy_hover": bs.summarize(busy_all, keep_samples=True),
        "heavy_ms": bs.summarize(heavy_all, keep_samples=False),
    }
    if idle_all and busy_all:
        result["busy_over_idle_p50"] = round(result["busy_hover"]["p50"] /
                                             max(0.01, result["idle_hover"]["p50"]), 2)
    return result


def case_ordering(client, spec, anchor, samples=20):
    """didChange → 立刻 documentSymbol：必须看到刚插入的符号。"""
    path = spec["ref_file"]
    uri = anchor["uri"]
    client.open_document(path, "verilog", bs.read_text(path), version=1)
    text = bs.read_text(path)
    violations = []
    latencies = []
    for index in range(1, samples + 1):
        marker = "ordering_probe_%d" % index
        text = text.replace("endmodule",
                            "  reg [7:0] %s;\nendmodule" % marker, 1)
        client.notify("textDocument/didChange", {
            "textDocument": {"uri": uri, "version": index + 1},
            "contentChanges": [{"text": text}]})
        t0 = time.perf_counter()
        payload = client.request("textDocument/documentSymbol",
                                 {"textDocument": {"uri": uri}}, timeout=900.0)
        latencies.append((time.perf_counter() - t0) * 1000.0)
        blob = json.dumps(payload, ensure_ascii=False)
        if marker not in blob:
            violations.append({"edit": index, "missing": marker})
    return {
        "samples": samples,
        "violations": violations,
        "violation_count": len(violations),
        "latency_ms": bs.summarize(latencies, keep_samples=False),
    }


def case_mixed200(client, spec, anchor, total=200, watchdog_s=180.0):
    """200 个混合请求（每 20 个夹一个并发小批），整体看门狗 180 s。"""
    params = {method: bs.phase2_params(spec, method, anchor) for method in MIXED_METHODS}
    started = time.perf_counter()
    done = {"n": 0, "errors": 0}

    def body():
        for index in range(total):
            method = MIXED_METHODS[index % len(MIXED_METHODS)]
            try:
                client.request(method, params[method], timeout=120.0)
                done["n"] += 1
            except Exception:
                done["errors"] += 1
            if index % 20 == 19:            # 每 20 个夹一个并发小批（5 个在飞）
                batch = [client.request_async("textDocument/hover",
                                              params["textDocument/hover"])
                         for _ in range(5)]
                for item in batch:
                    client.collect(item, timeout=120.0)
        return True

    with_watchdog(body, watchdog_s, "200 混合请求")
    elapsed = time.perf_counter() - started
    return {
        "requests": total,
        "completed": done["n"],
        "errors": done["errors"],
        "elapsed_ms": round(elapsed * 1000.0, 1),
        "per_request_ms": round(elapsed * 1000.0 / max(1, done["n"]), 2),
        "watchdog_s": watchdog_s,
    }


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--files", type=int, default=200)
    parser.add_argument("--samples", type=int, default=20)
    parser.add_argument("--fast", type=int, default=10)
    parser.add_argument("--mixed", type=int, default=200)
    parser.add_argument("--watchdog", type=float, default=180.0)
    parser.add_argument("--out", required=True)
    args = parser.parse_args(argv)

    os.makedirs(bs.LOG_DIR, exist_ok=True)
    spec = bs.ensure_synth_project(args.files)
    spec["anchor"] = bs.phase2_anchor(spec)
    anchor = spec["anchor"]
    report = {"files": args.files, "native_exe": bs.native_exe_info()}
    client = bs.ConcurrentNativeLspClient(bs.REPO_PY, os.path.join(bs.LOG_DIR, "threading.log"))
    load = bs.SystemLoad()
    try:
        client.initialize(spec["project_dir"], timeout=1800.0)
        client.request("textDocument/documentSymbol",
                       {"textDocument": {"uri": anchor["uri"]}}, timeout=1800.0)
        for _ in range(3):
            client.request("textDocument/hover",
                           bs.phase2_params(spec, "textDocument/hover", anchor), timeout=900.0)

        print("csv,threading,concurrency,start", flush=True)
        report["concurrency"] = case_concurrency(client, spec, anchor, args.fast)
        print("csv,threading,concurrency,%s" % json.dumps(
            report["concurrency"], ensure_ascii=False), flush=True)

        print("csv,threading,ordering,start", flush=True)
        report["ordering"] = case_ordering(client, spec, anchor, args.samples)
        print("csv,threading,ordering,%s" % json.dumps(
            report["ordering"], ensure_ascii=False), flush=True)

        print("csv,threading,mixed,start", flush=True)
        report["mixed"] = case_mixed200(client, spec, anchor, args.mixed, args.watchdog)
        print("csv,threading,mixed,%s" % json.dumps(
            report["mixed"], ensure_ascii=False), flush=True)
    except HangDetected as exc:
        report["hang"] = str(exc)
        print("csv,threading,HANG,%s" % exc, flush=True)
    except Exception as exc:  # noqa: BLE001
        report["fatal"] = "%s: %s" % (type(exc).__name__, exc)
    finally:
        load.stop()
        try:
            client.close()
        except Exception:
            pass
    report["cpu_busy"] = load.busy_fraction()
    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(report, handle, ensure_ascii=False, indent=1)
    return 2 if "hang" in report else 0


if __name__ == "__main__":
    sys.exit(main())