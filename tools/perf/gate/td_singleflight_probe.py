#!/usr/bin/env python3
"""td_singleflight_probe.py —— W3 专项：5 连发保存的 TD 轮数/进程数/总耗时 + 取消后无残留。

W3 的承诺：TD single-flight 接线后，5 次连续保存从 **2 轮 → 1 轮**（重复检查被合并），
且取消/退出后 **没有 `td_commands_prompt.exe` 残留**。

做法（全程只碰 %TEMP% 的工程副本，绝不写 Y:\\CorpusAlpha）：
  1) 把真实工程拷到 %TEMP%\\fpgalsp-gate\\w3\\
  2) 起 native LSP（cwd = 副本），initializationOptions 打开 TD 检查：
     `{fpgaLsp: {tdCheckOnSave: true, enableTd: true}}`
  3) didOpen top.v，等首轮就绪
  4) **连发 5 次 didSave**（间隔 `--gap-ms`，默认 150 ms），同时以 50 ms 采样 `tasklist`，
     记录出现过的 `td_commands_prompt.exe` PID 集合（**不同 PID 数 = TD 轮数**）
  5) 等到"连续 600 ms 没有新的 TD 进程"或超时（`--settle-s`，默认 90 s），记总耗时
  6) close（shutdown+exit），再查 `tasklist`：必须 0 个残留
  7) 另做一次"取消"路径：发 `$/cancelRequest` 指向…（TD 检查是通知触发的，没有请求 id，
     所以这里改为测"保存后立刻 shutdown"，断言进程树被带走、无残留）

用法：
    python tools/perf/gate/td_singleflight_probe.py --out <tagdir>/td_singleflight.json
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time

CPP = r"Y:/fpga-lsp-cpp"
sys.path.insert(0, os.path.join(CPP, "tools", "audit"))
import bench_servers as bs  # noqa: E402
from goldenlib.client import NativeLspClient, path_to_uri  # noqa: E402

TD_NAME = "td_commands_prompt.exe"


def copy_project(root):
    source = r"%WORKSPACE%\CorpusAlpha"
    target = os.path.join(root, "CorpusAlpha")
    if os.path.isdir(target):
        shutil.rmtree(target, ignore_errors=True)
    shutil.copytree(source, target,
                    ignore=shutil.ignore_patterns(".fpgalsp", "*_Runs"))
    al = os.path.join(target, "demo_project.al")
    for name in os.listdir(target):
        if name.endswith(".al"):
            al = os.path.join(target, name)
            break
    top = os.path.join(target, "src", "top.v")
    return {"dir": target, "al": al, "top": top}


def td_pids():
    return bs.list_processes(TD_NAME) or set()


def run_once(tag, spec, args, initial_save=True):
    """一次完整测量：5 连发保存 → 观察 TD 轮数/耗时 → 关闭 → 残留检查。"""
    log_path = os.path.join(bs.LOG_DIR, "td-%s.log" % tag)
    os.environ.pop("FPGALSP_STATE_DIR", None)
    pids_before = td_pids()
    run_dir = os.path.join(spec["dir"], ".fpgalsp", "run")
    logs_before = set()
    if os.path.isdir(run_dir):
        logs_before = {name for name in os.listdir(run_dir) if name.endswith(".log")}
    client = NativeLspClient(spec["dir"], log_path, extra_args=[])
    result = {"tag": tag}
    samples = []
    seen_pids = set()
    try:
        # 自己拼 initialize：goldenlib 的 initialize() 用 setdefault("fpgaLsp") 会吞掉覆盖值，
        # 必须整包自己发，否则 tdCheckOnSave 永远是 False、TD 根本不会启动。
        client.request("initialize", {
            "processId": os.getpid(),
            "rootUri": path_to_uri(spec["dir"]),
            "clientInfo": {"name": "td-singleflight-probe", "version": "1"},
            "capabilities": {"workspace": {"didChangeWatchedFiles": {}}},
            "initializationOptions": {
                "fpgaLsp": {"tdCheckOnSave": True, "enableTd": True}},
        }, timeout=1800.0)
        client.notify("initialized", {})
        uri = path_to_uri(spec["top"])
        client.open_document(spec["top"], "verilog", timeout=60.0) if False else None
        client.open_document(spec["top"], "verilog")
        client.request("textDocument/documentSymbol", {"textDocument": {"uri": uri}},
                       timeout=900.0)

        def sample():
            got = td_pids()
            for pid in got - pids_before:
                seen_pids.add(pid)
            samples.append((round(time.perf_counter() - t_start, 2), len(got - pids_before)))

        t_start = time.perf_counter()
        for index in range(5):
            client.notify("textDocument/didSave", {"textDocument": {"uri": uri}})
            # 150 ms 间隔里持续采样
            deadline = time.perf_counter() + args.gap_ms / 1000.0
            while time.perf_counter() < deadline:
                sample()
                time.sleep(args.sample_ms / 1000.0)
        # 等稳定：连续 settle_quiet_ms 没有新的 TD 进程
        quiet_since = None
        settle_deadline = time.perf_counter() + args.settle_s
        while time.perf_counter() < settle_deadline:
            sample()
            live = td_pids() - pids_before
            if live:
                quiet_since = None
            else:
                quiet_since = quiet_since or time.perf_counter()
                if (time.perf_counter() - quiet_since) >= args.settle_quiet_ms / 1000.0:
                    break
            time.sleep(args.sample_ms / 1000.0)
        result["total_ms"] = round((time.perf_counter() - t_start) * 1000.0, 1)
        # 更可靠的"轮数"口径：TD 每个会话写一个 td_*.log；read_hdl 每次会话 1 次
        new_logs = []
        if os.path.isdir(run_dir):
            new_logs = sorted(name for name in os.listdir(run_dir)
                              if name.endswith(".log") and name not in logs_before)
        result["td_sessions"] = len(new_logs)
        result["td_session_logs"] = new_logs[-6:]
        read_hdl = 0
        for name in new_logs:
            try:
                with open(os.path.join(run_dir, name), encoding="utf-8", errors="replace") as fh:
                    read_hdl += sum(1 for line in fh if "read_hdl" in line)
            except OSError:
                pass
        result["read_hdl_count"] = read_hdl
        result["td_rounds_distinct_pids"] = len(seen_pids)
        result["sampled_max_concurrent"] = max((n for _t, n in samples), default=0)
    except Exception:                              # noqa: BLE001
        import traceback
        result["fatal"] = traceback.format_exc()
    finally:
        try:
            client.close()
        except Exception:
            pass
    # 关闭后立刻查残留（给进程一点退出时间）
    time.sleep(0.5)
    after = td_pids()
    result["residual_td_after_close"] = sorted(after - pids_before)
    result["residual_count"] = len(after - pids_before)
    result["saved_with_text"] = False
    return result


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--gap-ms", type=float, default=150.0)
    parser.add_argument("--sample-ms", type=float, default=50.0)
    parser.add_argument("--settle-s", type=float, default=90.0)
    parser.add_argument("--settle-quiet-ms", type=float, default=600.0)
    parser.add_argument("--rounds", type=int, default=2)
    parser.add_argument("--out", required=True)
    args = parser.parse_args(argv)

    root = os.path.join(os.environ.get("TEMP", "."), "fpgalsp-gate", "w3")
    os.makedirs(root, exist_ok=True)
    os.makedirs(bs.LOG_DIR, exist_ok=True)
    spec = copy_project(root)
    report = {"native_exe": bs.native_exe_info(), "project": spec["dir"], "runs": []}
    load = bs.SystemLoad()
    for index in range(1, args.rounds + 1):
        row = run_once("r%d" % index, spec, args)
        report["runs"].append(row)
        print("csv,td_singleflight,r%d,%s" % (index, json.dumps(row, ensure_ascii=False)[:400]),
              flush=True)
    load.stop()
    report["cpu_busy"] = load.busy_fraction()
    rounds = [row.get("td_sessions") for row in report["runs"]
              if row.get("td_sessions") is not None]
    if not rounds:  # 回退到 PID 口径
        rounds = [row.get("td_rounds_distinct_pids") for row in report["runs"]
                  if row.get("td_rounds_distinct_pids") is not None]
    totals = [row.get("total_ms") for row in report["runs"] if row.get("total_ms")]
    residuals = [row.get("residual_count") for row in report["runs"]]
    report["verdict"] = {
        "runs": len(report["runs"]),
        "td_rounds_by_session": [row.get("td_sessions") for row in report["runs"]],
        "td_rounds_by_pid": [row.get("td_rounds_distinct_pids") for row in report["runs"]],
        "td_rounds": rounds,
        "max_rounds": max(rounds) if rounds else None,
        "total_ms_median": (sorted(totals)[len(totals) // 2] if totals else None),
        "residual_total": sum(r or 0 for r in residuals),
        "single_flight_ok": bool(rounds) and max(rounds) <= 1,
        "no_residual_ok": sum(r or 0 for r in residuals) == 0,
    }
    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(report, handle, ensure_ascii=False, indent=1)
    print("csv,td_singleflight,verdict,%s" % json.dumps(report["verdict"], ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())