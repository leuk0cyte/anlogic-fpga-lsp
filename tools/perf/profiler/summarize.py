#!/usr/bin/env python3
"""把 sampler 的 CSV + driver 的 JSON 汇总成报告用的表。

用法:
    python tools/perf/profiler/summarize.py --runs %TEMP%\\fpgalsp-perf\\profiler\\runs \
        --out tools/perf/profiler/out --top 25

产出（落盘到 --out，全部 < 5 MB）：
    <out>/latency.csv          场景 × 有/无采样器 的 p50/p95/n/系统负载
    <out>/<scenario>_functions.csv   top-N（按 self 排序，含 self/inclusive 占比）
    <out>/<scenario>_threads.csv     每线程 CPU（user/kernel/样本占比/偏差）
    <out>/<scenario>_meta.csv        场景级：样本数、采样 CPU、内核态占比
并在 stdout 打印 markdown 表，便于直接贴进 CPU-PROFILE.md。
"""

from __future__ import annotations

import argparse
import csv
import glob
import io
import json
import os
import sys
from collections import defaultdict

SCENARIOS = ["a", "b", "c", "d"]
SCENARIO_NAME = {
    "a": "a) 冷启动 + 首次索引（200 文件 ×10）",
    "b": "b) warm 混合请求流",
    "c": "c) didChange 连打 20 次",
    "d": "d) 10 MB timing 报告（全量 + 缓存命中）",
}


def read_csv(path: str) -> list[dict]:
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8", errors="replace", newline="") as fh:
        rows = list(csv.DictReader(fh))
    return rows


def fnum(row: dict, key: str, default: float = 0.0) -> float:
    try:
        return float(row.get(key) or default)
    except (TypeError, ValueError):
        return default


def merge_functions(sample_dirs: list[str]) -> dict[tuple[str, str], dict]:
    merged: dict[tuple[str, str], dict] = defaultdict(
        lambda: {"self_ms": 0.0, "incl_ms": 0.0, "self_samples": 0, "incl_samples": 0}
    )
    for d in sample_dirs:
        for row in read_csv(os.path.join(d, "functions.csv")):
            key = (row.get("module", "?"), row.get("function", "?"))
            merged[key]["self_ms"] += fnum(row, "self_ms")
            merged[key]["incl_ms"] += fnum(row, "inclusive_ms")
            merged[key]["self_samples"] += int(fnum(row, "self_samples"))
            merged[key]["incl_samples"] += int(fnum(row, "inclusive_samples"))
    return merged


def merge_threads(sample_dirs: list[str]) -> dict:
    """单进程场景：合并同一 tid 的行；多进程场景（a）按"每次迭代最忙的线程"归并。"""
    per_tid: dict[str, dict] = {}
    busiest_total = {"user_ms": 0.0, "kernel_ms": 0.0, "samples": 0}
    other_total = {"user_ms": 0.0, "kernel_ms": 0.0, "samples": 0}
    multi = len(sample_dirs) > 1
    for d in sample_dirs:
        rows = read_csv(os.path.join(d, "threads.csv"))
        if multi:
            busy = None
            for row in rows:
                user, kernel = fnum(row, "user_ms"), fnum(row, "kernel_ms")
                if busy is None or (user + kernel) > (busy[0] + busy[1]):
                    busy = (user, kernel, int(fnum(row, "samples")))
            for row in rows:
                user, kernel, samples = fnum(row, "user_ms"), fnum(row, "kernel_ms"), int(fnum(row, "samples"))
                target = busiest_total if busy is not None and (user, kernel, samples) == busy else other_total
                target["user_ms"] += user
                target["kernel_ms"] += kernel
                target["samples"] += samples
            continue
        for row in rows:
            tid = row.get("tid", "?")
            entry = per_tid.setdefault(tid, {
                "tid": tid, "thread_name": row.get("thread_name", "?"),
                "user_ms": 0.0, "kernel_ms": 0.0, "samples": 0, "sweeps": 0,
                "wait_streak_max": 0,
            })
            entry["user_ms"] += fnum(row, "user_ms")
            entry["kernel_ms"] += fnum(row, "kernel_ms")
            entry["samples"] += int(fnum(row, "samples"))
            entry["sweeps"] += int(fnum(row, "sweeps"))
            entry["wait_streak_max"] = max(entry["wait_streak_max"], int(fnum(row, "wait_streak_max")))
    if multi:
        total = busiest_total["user_ms"] + busiest_total["kernel_ms"] + other_total["user_ms"] + other_total["kernel_ms"]
        return {"multi": True, "busiest": busiest_total, "others": other_total, "total_cpu_ms": total}
    return {"multi": False, "threads": per_tid}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", default=os.path.join(os.environ.get("TEMP", "."), "fpgalsp-perf", "profiler", "runs"))
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "out"))
    ap.add_argument("--top", type=int, default=25)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    print("# 汇总表（由 summarize.py 生成）\n")

    # ---------------- 延迟 + 采样开销 ----------------
    lat_rows: list[dict] = []
    for scenario in SCENARIOS:
        for with_sampler in (False, True):
            tag = f"{scenario}_{'with' if with_sampler else 'without'}_sampler"
            path = os.path.join(args.runs, f"{tag}.json")
            if not os.path.exists(path):
                continue
            data = json.load(open(path, encoding="utf-8"))
            st = data["stats"]
            lat_rows.append({
                "scenario": scenario,
                "with_sampler": with_sampler,
                "n": st.get("n", 0),
                "p50_ms": st.get("p50_ms", 0),
                "p95_ms": st.get("p95_ms", 0),
                "max_ms": st.get("max_ms", 0),
                "busy_pct": data.get("system_load", {}).get("busy_pct", 0),
                "kernel_pct": data.get("system_load", {}).get("kernel_pct", 0),
                "by_method": data.get("by_method", {}),
            })
    with open(os.path.join(args.out, "latency.csv"), "w", encoding="utf-8", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=["scenario", "with_sampler", "n", "p50_ms", "p95_ms",
                                                "max_ms", "busy_pct", "kernel_pct"])
        writer.writeheader()
        for row in lat_rows:
            writer.writerow({k: row[k] for k in writer.fieldnames})

    print("## 1. 延迟与采样开销（同一负载，有/无采样器）\n")
    print("| 场景 | 采样器 | n | p50 (ms) | p95 (ms) | p50 偏差 | p95 偏差 | 系统 busy% | 内核态% |")
    print("|---|---|---|---|---|---|---|---|---|")
    for scenario in SCENARIOS:
        base = next((r for r in lat_rows if r["scenario"] == scenario and not r["with_sampler"]), None)
        withsup = next((r for r in lat_rows if r["scenario"] == scenario and r["with_sampler"]), None)
        for row in (base, withsup):
            if row is None:
                continue
            p50_dev = p95_dev = "-"
            if row["with_sampler"] and base and base["p50_ms"]:
                p50_dev = f"{(row['p50_ms'] / base['p50_ms'] - 1) * 100:+.1f}%"
                p95_dev = f"{(row['p95_ms'] / base['p95_ms'] - 1) * 100:+.1f}%" if base["p95_ms"] else "-"
            print(f"| {SCENARIO_NAME[scenario]} | {'有' if row['with_sampler'] else '无'} | {row['n']} | "
                  f"{row['p50_ms']} | {row['p95_ms']} | {p50_dev} | {p95_dev} | {row['busy_pct']} | {row['kernel_pct']} |")
    print()

    # 分方法
    print("### 1b. 分请求类型（无采样器基线 / 有采样器）\n")
    print("| 场景 | 方法 | n | p50 无采样 | p50 有采样 | 偏差 | p95 无采样 | p95 有采样 |")
    print("|---|---|---|---|---|---|---|---|")
    for scenario in SCENARIOS:
        base = next((r for r in lat_rows if r["scenario"] == scenario and not r["with_sampler"]), None)
        withsup = next((r for r in lat_rows if r["scenario"] == scenario and r["with_sampler"]), None)
        if not base:
            continue
        for method, st in base["by_method"].items():
            other = (withsup or {}).get("by_method", {}).get(method, {})
            dev = "-"
            if other and st.get("p50_ms"):
                dev = f"{(other['p50_ms'] / st['p50_ms'] - 1) * 100:+.1f}%"
            print(f"| {scenario} | {method} | {st.get('n')} | {st.get('p50_ms')} | {other.get('p50_ms', '-')} | "
                  f"{dev} | {st.get('p95_ms')} | {other.get('p95_ms', '-')} |")
    print()

    # ---------------- 每场景函数/线程 ----------------
    for scenario in SCENARIOS:
        tag = f"{scenario}_with_sampler"
        base_dir = os.path.join(args.runs, f"sample_{tag}")
        sample_dirs = sorted(glob.glob(os.path.join(base_dir, "it*"))) or ([base_dir] if os.path.isdir(base_dir) else [])
        if not sample_dirs:
            continue

        metas = [read_csv(os.path.join(d, "meta.csv")) for d in sample_dirs]
        total_cpu_ms = sum(fnum(m[3], "value") if len(m) > 3 else 0.0 for m in metas)  # 粗略：见下方精确算法
        # 精确：从 threads.csv 汇总 CPU
        thread_agg = merge_threads(sample_dirs)
        if thread_agg["multi"]:
            total_cpu_ms = thread_agg["total_cpu_ms"]
        else:
            total_cpu_ms = sum(t["user_ms"] + t["kernel_ms"] for t in thread_agg["threads"].values())

        samples = 0
        sweeps = 0
        stall = 0
        for d in sample_dirs:
            for row in read_csv(os.path.join(d, "meta.csv")):
                if row.get("key") == "samples":
                    samples += int(fnum(row, "value"))
                elif row.get("key") == "sweeps":
                    sweeps += int(fnum(row, "value"))
                elif row.get("key") == "stall_events":
                    stall += int(fnum(row, "value"))

        merged = merge_functions(sample_dirs)
        ordered = sorted(merged.items(), key=lambda kv: -kv[1]["self_ms"])
        self_total = sum(v["self_ms"] for v in merged.values()) or 1.0

        print(f"## 2. 场景 {scenario}：{SCENARIO_NAME[scenario]}\n")
        kernel_ms = 0.0
        if thread_agg["multi"]:
            kernel_ms = thread_agg["busiest"]["kernel_ms"] + thread_agg["others"]["kernel_ms"]
        else:
            kernel_ms = sum(t["kernel_ms"] for t in thread_agg["threads"].values())
        print(f"样本 {samples}，sweep {sweeps}，stall {stall}，采样到的 CPU {total_cpu_ms:.0f} ms，"
              f"其中内核态 {kernel_ms:.0f} ms（{(100.0 * kernel_ms / total_cpu_ms if total_cpu_ms else 0):.1f}%）\n")

        print(f"| # | module | function | self ms | self % | incl ms | incl % | self samples |")
        print("|---|---|---|---|---|---|---|---|")
        func_rows = []
        for i, ((module, function), stat) in enumerate(ordered[: args.top], 1):
            self_pct = 100.0 * stat["self_ms"] / total_cpu_ms if total_cpu_ms else 0.0
            incl_pct = 100.0 * stat["incl_ms"] / total_cpu_ms if total_cpu_ms else 0.0
            short = function if len(function) <= 90 else function[:87] + "..."
            print(f"| {i} | {module} | `{short}` | {stat['self_ms']:.1f} | {self_pct:.1f} | "
                  f"{stat['incl_ms']:.1f} | {incl_pct:.1f} | {stat['self_samples']} |")
            func_rows.append({
                "rank": i, "module": module, "function": function,
                "self_ms": round(stat["self_ms"], 3), "self_pct": round(self_pct, 2),
                "incl_ms": round(stat["incl_ms"], 3), "incl_pct": round(incl_pct, 2),
                "self_samples": stat["self_samples"],
            })
        print()

        with open(os.path.join(args.out, f"{scenario}_functions.csv"), "w", encoding="utf-8", newline="") as fh:
            writer = csv.DictWriter(fh, fieldnames=list(func_rows[0].keys()) if func_rows else ["rank"])
            writer.writeheader()
            writer.writerows(func_rows)

        # 线程表
        thread_rows = []
        print("| tid | 线程名 | user ms | kernel ms | CPU 占比 | 样本占比 | 样本−CPU 差 | 最大等待 streak |")
        print("|---|---|---|---|---|---|---|---|")
        if thread_agg["multi"]:
            for label, entry in (("每次迭代最忙线程", thread_agg["busiest"]),
                                 ("其余线程合计", thread_agg["others"])):
                user, kernel = entry["user_ms"], entry["kernel_ms"]
                cpu = user + kernel
                cpu_share = 100.0 * cpu / total_cpu_ms if total_cpu_ms else 0.0
                sample_share = 100.0 * entry["samples"] / samples if samples else 0.0
                print(f"| - | {label} | {user:.0f} | {kernel:.0f} | {cpu_share:.1f}% | "
                      f"{sample_share:.1f}% | {sample_share - cpu_share:+.1f} | - |")
                thread_rows.append({"label": label, "user_ms": round(user, 1), "kernel_ms": round(kernel, 1),
                                    "cpu_share": round(cpu_share, 2), "sample_share": round(sample_share, 2),
                                    "delta": round(sample_share - cpu_share, 2)})
        else:
            for tid, t in sorted(thread_agg["threads"].items(),
                                 key=lambda kv: -(kv[1]["user_ms"] + kv[1]["kernel_ms"])):
                if t["user_ms"] + t["kernel_ms"] <= 0:
                    continue
                cpu = t["user_ms"] + t["kernel_ms"]
                cpu_share = 100.0 * cpu / total_cpu_ms if total_cpu_ms else 0.0
                sample_share = 100.0 * t["samples"] / samples if samples else 0.0
                print(f"| {tid} | {t['thread_name']} | {t['user_ms']:.0f} | {t['kernel_ms']:.0f} | "
                      f"{cpu_share:.1f}% | {sample_share:.1f}% | {sample_share - cpu_share:+.1f} | {t['wait_streak_max']} |")
                thread_rows.append({"tid": tid, "name": t["thread_name"], "user_ms": round(t["user_ms"], 1),
                                    "kernel_ms": round(t["kernel_ms"], 1), "cpu_share": round(cpu_share, 2),
                                    "sample_share": round(sample_share, 2),
                                    "delta": round(sample_share - cpu_share, 2),
                                    "wait_streak_max": t["wait_streak_max"]})
        print()
        with open(os.path.join(args.out, f"{scenario}_threads.csv"), "w", encoding="utf-8", newline="") as fh:
            writer = csv.DictWriter(fh, fieldnames=list(thread_rows[0].keys()) if thread_rows else ["tid"])
            writer.writeheader()
            writer.writerows(thread_rows)
        with open(os.path.join(args.out, f"{scenario}_meta.csv"), "w", encoding="utf-8", newline="") as fh:
            fh.write("key,value\n")
            fh.write(f"samples,{samples}\n")
            fh.write(f"sweeps,{sweeps}\n")
            fh.write(f"stall_events,{stall}\n")
            fh.write(f"sampled_cpu_ms,{round(total_cpu_ms, 1)}\n")
            fh.write(f"kernel_ms,{round(kernel_ms, 1)}\n")
            fh.write(f"kernel_pct,{round(100.0 * kernel_ms / total_cpu_ms, 2) if total_cpu_ms else 0}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())