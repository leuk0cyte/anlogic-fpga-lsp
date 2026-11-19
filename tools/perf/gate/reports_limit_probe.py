#!/usr/bin/env python3
"""reports_limit_probe.py —— F3 专项：`limit` 生效是唯一"预期输出变化"，单独记账。

F3 声明：**不传 `limit` 时输出逐字节不变**；传 `limit=200` 时 payload 应缩小约 25%。
协议整包对比（`-Only protocol`）用的是不传 limit 的默认参数，所以：
  * 默认参数 → 必须与基线一致（由 verify.ps1 的 protocol 段负责）
  * `limit=200` → 这里单独记"预期变化"：payload 字节、条目数、耗时

用法：
    python tools/perf/gate/reports_limit_probe.py --out <tagdir>/reports_limit.json
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


def call(client, label, params, repeats=5):
    for _ in range(2):
        client.request("fpgaLsp/reports", params, timeout=1800.0)
    samples = []
    payload = 0
    result = None
    for _ in range(repeats):
        t0 = time.perf_counter()
        result = client.request("fpgaLsp/reports", params, timeout=1800.0)
        samples.append((time.perf_counter() - t0) * 1000.0)
        payload = len(json.dumps(result, ensure_ascii=False))
    return {
        "label": label,
        "params": params,
        "payload_chars": payload,
        "ms": bs.summarize(samples, keep_samples=False),
        "items": _count_items(result),
    }


def _count_items(result):
    if not isinstance(result, dict):
        return None
    timing = result.get("timing")
    if isinstance(timing, dict):
        paths = timing.get("paths")
        if isinstance(paths, list):
            return {"timing_paths": len(paths)}
    return None


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--files", type=int, default=200)
    parser.add_argument("--report-mb", type=float, default=10.0)
    parser.add_argument("--limit", type=int, default=200)
    parser.add_argument("--out", required=True)
    args = parser.parse_args(argv)

    os.makedirs(bs.LOG_DIR, exist_ok=True)
    spec = bs.ensure_report_project(args.files, args.report_mb)
    spec["anchor"] = bs.phase2_anchor(spec)
    anchor = spec["anchor"]
    client = bs.make_lsp_client("native", "reports-limit.log")
    report = {"native_exe": bs.native_exe_info(), "limit": args.limit}
    load = bs.SystemLoad()
    try:
        client.initialize(spec["project_dir"], timeout=1800.0)
        client.request("textDocument/documentSymbol",
                       {"textDocument": {"uri": anchor["uri"]}}, timeout=1800.0)
        base = {"uri": anchor["uri"], "section": "summary"}
        summary_default = call(client, "summary(no limit)", base)
        summary_limited = call(client, "summary(limit=%d)" % args.limit,
                               dict(base, limit=args.limit))
        timing_default = call(client, "timing(no limit)",
                              {"uri": anchor["uri"], "section": "timing"})
        timing_limited = call(client, "timing(limit=%d)" % args.limit,
                              {"uri": anchor["uri"], "section": "timing",
                               "limit": args.limit}, repeats=3)
        report["cases"] = [summary_default, summary_limited, timing_default, timing_limited]
        report["payload_delta_pct"] = {
            "summary": round(100.0 * (summary_limited["payload_chars"] -
                                      summary_default["payload_chars"]) /
                             max(1, summary_default["payload_chars"]), 1),
            "timing": round(100.0 * (timing_limited["payload_chars"] -
                                     timing_default["payload_chars"]) /
                            max(1, timing_default["payload_chars"]), 1),
        }
    except Exception:
        report["fatal"] = __import__("traceback").format_exc()
    finally:
        try:
            client.close()
        except Exception:
            pass
    load.stop()
    report["cpu_busy"] = load.busy_fraction()
    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(report, handle, ensure_ascii=False, indent=1)
    for case in report.get("cases") or []:
        print("csv,reports_limit,%s,%d,%.2f,%.2f" % (
            case["label"], case["payload_chars"], case["ms"]["p50"], case["ms"]["p95"]))
    print("csv,reports_limit_delta,%s" % json.dumps(report.get("payload_delta_pct"),
                                                    ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())