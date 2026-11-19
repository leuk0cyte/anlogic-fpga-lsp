#!/usr/bin/env python3
"""reports_probe.py —— 窄口径报告测量（给 fast tag 用，约 2 分钟）。

为什么单独写：`-Only phase2` 要 12–15 分钟，而报告路径（F3 的主战场）的关键指标
只有三项：首解析 / 重复请求 p50 / payload 字节。这里复用 bench_servers 的
`ensure_report_project` + `run_phase2_reports`，只跑这一件事，产物键名与
`verify.py` 从 phase2 里取的完全一致，所以对比逻辑不用改。

用法：
    python tools/perf/gate/reports_probe.py --report-mb 10 --repeats 5 \
        --out doc/audit/2026-11-19/fix/results/<tag>/reports.json
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


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--report-mb", type=float, default=10.0)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--files", type=int, default=200,
                        help="承载报告的合成工程规模（默认 200）")
    parser.add_argument("--out", required=True)
    args = parser.parse_args(argv)

    os.makedirs(bs.LOG_DIR, exist_ok=True)
    load = bs.SystemLoad()
    spec = bs.ensure_report_project(args.files, args.report_mb)
    spec["anchor"] = bs.phase2_anchor(spec)
    result = bs.run_phase2_reports("native", spec, args.repeats)
    load.stop()
    result["cpu_busy"] = load.busy_fraction()
    result["captured_at"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
    result["native_exe"] = bs.native_exe_info()

    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(result, handle, ensure_ascii=False, indent=1)
    repeat = (result.get("repeat_ms") or {})
    print("csv,reports,first,%s,%s" % (result.get("first_parse_ms"),
                                       result.get("payload_chars")))
    print("csv,reports,repeat,%s,%s,%s,%s" % (repeat.get("n"), repeat.get("p50"),
                                             repeat.get("p95"), repeat.get("max")))
    return 0


if __name__ == "__main__":
    sys.exit(main())