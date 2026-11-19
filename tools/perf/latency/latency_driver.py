#!/usr/bin/env python3
"""端到端 LSP 延迟归因驱动（perf-latency / task-28）。

回答一个问题：**用户按一个键到屏幕刷新，时间花在哪一段。**

设计要点（对齐 doc/audit/2026-11-19/perf/README.md 的纪律）：

* 每个结论 3 轮 × 20 样本，报 p50/p95/max，另记每轮的系统负载（GetSystemTimes 差分）。
* 不修改产品代码、不动 build/bin；合成工程全部在 %TEMP%\\fpgalsp-perf\\latency。
* 服务端分段靠"客户端时间戳差分"：
    t0 发 didChange 通知  →  t_diag 收到该文件的 publishDiagnostics  →  t_req 响应
  因为服务端是同一条管道顺序处理（didChange 是通知、先被消费），
  t_diag - t0 覆盖 docs.Change + 索引增量 + 诊断计算 + 写回管道；
  t_req - t_diag 覆盖紧随其后的快请求本身。
* 另外用 GetProcessTimes 直接读服务端进程的 user+kernel 时间，用来回答
  "一次按键到底烧掉多少 CPU"，也是 $/cancelRequest 是否真省工作的证据。

用法：
    python tools/perf/latency/latency_driver.py --sizes 200,1000 --rounds 3 --samples 20
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
import traceback

CPP = r"Y:/fpga-lsp-cpp"
AUDIT_TOOLS = os.path.join(CPP, "tools", "audit")
sys.path.insert(0, AUDIT_TOOLS)

import bench_servers as bs  # noqa: E402  （复用合成工程生成 / 参数 / 客户端 / 统计）

from goldenlib.client import NativeLspClient, path_to_uri  # noqa: E402

OUT_DIR = os.path.join(os.environ["TEMP"], "fpgalsp-perf", "latency")


# ---------------------------------------------------------------------------
#  带时间戳的客户端：记录每条通知到达的时刻（用于 didChange→诊断 的分段）
# ---------------------------------------------------------------------------
class ChainClient(bs._ConcurrentMixin, NativeLspClient):
    def __init__(self, *args, **kwargs):
        self.marks = []          # [(method, perf_counter)]
        super().__init__(*args, **kwargs)

    def _dispatch(self, msg):
        method = msg.get("method")
        if method:
            self.marks.append((method, time.perf_counter(), msg.get("params") or {}))
        super()._dispatch(msg)

    def diag_marks(self, uri, since_len=0):
        return [(t, params) for method, t, params in self.marks[since_len:]
                if method == "textDocument/publishDiagnostics"
                and (params or {}).get("uri") == uri]

    def mark_count(self):
        return len(self.marks)


def make_chain_client(log_name):
    return ChainClient(bs.REPO_PY, os.path.join(bs.LOG_DIR, log_name))


def new_load():
    return bs.SystemLoad()


def proc_times(pid):
    """服务端进程的 (user, kernel) 秒数；拿不到返回 None。"""
    handle = bs._kernel32.OpenProcess(0x1000, False, pid)  # QUERY_LIMITED_INFORMATION
    if not handle:
        return None
    try:
        class FILETIME(bs.ctypes.Structure):
            _fields_ = [("dwLowDateTime", bs.ctypes.c_ulong),
                        ("dwHighDateTime", bs.ctypes.c_ulong)]

        creation, exit_, kernel, user = FILETIME(), FILETIME(), FILETIME(), FILETIME()
        if not bs._kernel32.GetProcessTimes(handle, bs.ctypes.byref(creation),
                                            bs.ctypes.byref(exit_),
                                            bs.ctypes.byref(kernel),
                                            bs.ctypes.byref(user)):
            return None

        def secs(ft):
            return ((ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 1e7

        return secs(user), secs(kernel)
    finally:
        bs._kernel32.CloseHandle(handle)


def cpu_total(pid):
    got = proc_times(pid)
    return None if got is None else got[0] + got[1]


# ---------------------------------------------------------------------------
#  1) warm 逐方法 + 每文件斜率
# ---------------------------------------------------------------------------
def run_methods(spec, rounds, samples):
    method_rounds = {}
    result = {"files": spec["files"], "rounds": [], "methods": {}}
    for rep in range(1, rounds + 1):
        load = new_load()
        client = bs.make_lsp_client("native", "lat-methods-%03d-r%d.log" % (spec["files"], rep))
        try:
            client.initialize(spec["project_dir"], timeout=1800.0)
            t0 = time.perf_counter()
            client.request("textDocument/documentSymbol",
                           bs.phase2_params(spec, "textDocument/documentSymbol", spec["anchor"]),
                           timeout=1800.0)
            cold = (time.perf_counter() - t0) * 1000.0
            per_method = {}
            for method in bs.PHASE2_METHODS:
                params = bs.phase2_params(spec, method, spec["anchor"])
                for _ in range(3):
                    client.request(method, params, timeout=1800.0)
                values = []
                payload = 0
                for _ in range(samples):
                    t0 = time.perf_counter()
                    out = client.request(method, params, timeout=1800.0)
                    values.append((time.perf_counter() - t0) * 1000.0)
                    payload = len(json.dumps(out, ensure_ascii=False))
                per_method[method] = {"samples_ms": values, "payload_chars": payload}
                method_rounds.setdefault(method, []).extend(values)
            load.stop()
            result["rounds"].append({"rep": rep, "cold_index_ms": round(cold, 2),
                                     "cpu_busy": load.busy_fraction(),
                                     "methods": per_method})
        finally:
            try:
                client.close()
            except Exception:
                pass
    for method, values in method_rounds.items():
        summary = bs.summarize(values, keep_samples=False)
        summary["payload_chars"] = result["rounds"][0]["methods"][method]["payload_chars"]
        summary["rounds"] = rounds
        result["methods"][method] = summary
    return result


# ---------------------------------------------------------------------------
#  2) 按键链路：didChange → publishDiagnostics → 紧随其后的快请求
# ---------------------------------------------------------------------------
EDIT_LINE = "  reg [7:0] edit_%d;\n"


def build_edit_text(base, index):
    return base.replace("endmodule", (EDIT_LINE % index) + "endmodule", 1)


def run_key_chain(spec, rounds, samples):
    """一轮 = 20 次按键；每次记录 t_didChange → t_diag → t_completion。"""
    anchor = spec["anchor"]
    path = spec["ref_file"]
    uri = anchor["uri"]
    base_text = bs.read_text(path)
    result = {"files": spec["files"], "rounds": []}
    all_rows = []
    for rep in range(1, rounds + 1):
        load = new_load()
        client = make_chain_client("lat-chain-%03d-r%d.log" % (spec["files"], rep))
        try:
            client.initialize(spec["project_dir"], timeout=1800.0)
            client.request("textDocument/documentSymbol", {"textDocument": {"uri": uri}},
                           timeout=1800.0)
            client.open_document(path, "verilog", base_text, version=1)
            # 打开后先让服务端稳定（首轮诊断/索引都完成）
            for _ in range(3):
                client.request("textDocument/documentSymbol", {"textDocument": {"uri": uri}},
                               timeout=1800.0)
            rows = []
            text = base_text
            for index in range(1, samples + 1):
                text = build_edit_text(base_text if index == 1 else text, index)
                before = client.mark_count()
                t0 = time.perf_counter()
                client.notify("textDocument/didChange", {
                    "textDocument": {"uri": uri, "version": index + 1},
                    "contentChanges": [{"text": text}],
                })
                # 立刻发 completion（用户真正在等的那一个）。只发一个，避免第二个请求
                # 把测量搅浑；documentSymbol 的链路在同一张表里由 methods 段单独覆盖。
                pending_comp = client.request_async(
                    "textDocument/completion",
                    bs.phase2_params(spec, "textDocument/completion", anchor))
                comp_ms, _ = client.collect(pending_comp)
                t_after = time.perf_counter()
                diag = client.diag_marks(uri, before)
                first_diag = (diag[0][0] - t0) * 1000.0 if diag else None
                rows.append({
                    "edit": index,
                    "didChange_to_diag_ms": round(first_diag, 3) if first_diag else None,
                    "completion_ms": round(comp_ms, 3),
                    "diag_count": len(diag),
                    "wall_ms": round((t_after - t0) * 1000.0, 3),
                })
            load.stop()
            result["rounds"].append({"rep": rep, "cpu_busy": load.busy_fraction(), "rows": rows})
            all_rows.extend(rows)
        except Exception:
            result["rounds"].append({"rep": rep, "fatal": traceback.format_exc()})
        finally:
            try:
                client.close()
            except Exception:
                pass
    def stats(key):
        values = [row[key] for row in all_rows if row.get(key) is not None]
        return bs.summarize(values, keep_samples=True) if values else {"n": 0}
    result["summary"] = {
        "didChange_to_diag_ms": stats("didChange_to_diag_ms"),
        "completion_ms": stats("completion_ms"),
        "diag_count": stats("diag_count"),
    }
    return result


# ---------------------------------------------------------------------------
#  3) 队头阻塞：全量报告在飞时，10 个快请求被拖到多少
# ---------------------------------------------------------------------------
def run_head_of_line(report_spec, rounds, fast=10):
    anchor = report_spec["anchor"]
    result = {"files": report_spec["files"], "rounds": []}
    idle_all, busy_all = [], []
    for rep in range(1, rounds + 1):
        load = new_load()
        client = make_chain_client("lat-hol-r%d.log" % rep)
        try:
            client.initialize(report_spec["project_dir"], timeout=1800.0)
            client.request("textDocument/documentSymbol",
                           {"textDocument": {"uri": anchor["uri"]}}, timeout=1800.0)
            hover_params = bs.phase2_params(report_spec, "textDocument/hover", anchor)
            for _ in range(3):
                client.request("textDocument/hover", hover_params, timeout=900.0)
            idle = [client.collect(client.request_async("textDocument/hover", hover_params))[0]
                    for _ in range(fast)]
            heavy = client.request_async("fpgaLsp/reports",
                                         bs.phase2_params(report_spec, "fpgaLsp/reports", anchor))
            busy = [client.collect(client.request_async("textDocument/hover", hover_params))[0]
                    for _ in range(fast)]
            heavy_ms, heavy_payload = client.collect(heavy, timeout=600.0)
            load.stop()
            result["rounds"].append({
                "rep": rep, "cpu_busy": load.busy_fraction(),
                "idle_ms": [round(v, 3) for v in idle],
                "busy_ms": [round(v, 3) for v in busy],
                "heavy_ms": round(heavy_ms, 2),
                "heavy_payload_chars": len(json.dumps(heavy_payload, ensure_ascii=False)),
            })
            idle_all.extend(idle)
            busy_all.extend(busy)
        except Exception:
            result["rounds"].append({"rep": rep, "fatal": traceback.format_exc()})
        finally:
            try:
                client.close()
            except Exception:
                pass
    result["idle_ms"] = bs.summarize(idle_all, keep_samples=True) if idle_all else {"n": 0}
    result["busy_ms"] = bs.summarize(busy_all, keep_samples=True) if busy_all else {"n": 0}
    if idle_all and busy_all:
        result["head_of_line_ratio_p50"] = round(result["busy_ms"]["p50"] /
                                                 max(0.01, result["idle_ms"]["p50"]), 2)
    return result


# ---------------------------------------------------------------------------
#  4) $/cancelRequest 是否真省工作
# ---------------------------------------------------------------------------
def run_cancel(report_spec, rounds):
    """对比：重请求跑完 vs 跑 30 ms 后取消 —— 服务端 CPU 与后续请求延迟。"""
    anchor = report_spec["anchor"]
    reports_params = bs.phase2_params(report_spec, "fpgaLsp/reports", anchor)
    hover_params = bs.phase2_params(report_spec, "textDocument/hover", anchor)
    result = {"files": report_spec["files"], "rounds": []}
    baseline_cpu, cancel_cpu = [], []
    baseline_hover, cancel_hover = [], []
    for rep in range(1, rounds + 1):
        load = new_load()
        row = {"rep": rep}
        # --- (a) 跑完
        client = make_chain_client("lat-cancel-a-r%d.log" % rep)
        try:
            client.initialize(report_spec["project_dir"], timeout=1800.0)
            client.request("textDocument/documentSymbol",
                           {"textDocument": {"uri": anchor["uri"]}}, timeout=1800.0)
            client.request("textDocument/hover", hover_params, timeout=900.0)
            cpu0 = cpu_total(client.proc.pid)
            t0 = time.perf_counter()
            heavy = client.request_async("fpgaLsp/reports", reports_params)
            elapsed, _payload = client.collect(heavy, timeout=600.0)
            cpu1 = cpu_total(client.proc.pid)
            hover_ms, _ = client.collect(client.request_async("textDocument/hover", hover_params))
            row["full_ms"] = round(elapsed, 1)
            row["full_cpu_s"] = round((cpu1 - cpu0), 3) if cpu0 is not None else None
            row["full_then_hover_ms"] = round(hover_ms, 3)
        except Exception:
            row["full_fatal"] = traceback.format_exc()
        finally:
            try:
                client.close()
            except Exception:
                pass

        # --- (b) 发出去 30 ms 后取消
        client = make_chain_client("lat-cancel-b-r%d.log" % rep)
        try:
            client.initialize(report_spec["project_dir"], timeout=1800.0)
            client.request("textDocument/documentSymbol",
                           {"textDocument": {"uri": anchor["uri"]}}, timeout=1800.0)
            client.request("textDocument/hover", hover_params, timeout=900.0)
            cpu0 = cpu_total(client.proc.pid)
            t0 = time.perf_counter()
            heavy = client.request_async("fpgaLsp/reports", reports_params)
            heavy_id = heavy[0]
            time.sleep(0.030)
            t_cancel = time.perf_counter()
            client.notify("$/cancelRequest", {"id": heavy_id})
            # 取消之后立刻测一个快请求：如果 worker 真的被释放，它应该很快
            hover_ms, _ = client.collect(client.request_async("textDocument/hover", hover_params))
            # 再等 300 ms 看那个被取消的请求是否还会回响应
            cancelled_reply = None
            deadline = time.perf_counter() + 0.4
            while time.perf_counter() < deadline:
                try:
                    got = client.collect(heavy, timeout=0.05)
                    cancelled_reply = {"ms": round(got[0], 1), "had_result": got[1] is not None}
                    break
                except Exception:
                    continue
            cpu1 = cpu_total(client.proc.pid)
            row["cancel_ack_ms"] = round((t_cancel - t0) * 1000.0, 1)
            row["cancelled_reply"] = cancelled_reply
            row["cancel_cpu_s"] = round(cpu1 - cpu0, 3) if cpu0 is not None else None
            row["cancel_then_hover_ms"] = round(hover_ms, 3)
        except Exception:
            row["cancel_fatal"] = traceback.format_exc()
        finally:
            try:
                client.close()
            except Exception:
                pass

        load.stop()
        row["cpu_busy"] = load.busy_fraction()
        result["rounds"].append(row)
        if row.get("full_cpu_s") is not None:
            baseline_cpu.append(row["full_cpu_s"])
        if row.get("cancel_cpu_s") is not None:
            cancel_cpu.append(row["cancel_cpu_s"])
        if row.get("full_then_hover_ms") is not None:
            baseline_hover.append(row["full_then_hover_ms"])
        if row.get("cancel_then_hover_ms") is not None:
            cancel_hover.append(row["cancel_then_hover_ms"])
    result["full_cpu_s"] = {"samples": baseline_cpu}
    result["cancel_cpu_s"] = {"samples": cancel_cpu}
    if baseline_cpu and cancel_cpu:
        result["cpu_saved_ratio"] = round(1.0 - (sum(cancel_cpu) / len(cancel_cpu)) /
                                          max(1e-9, sum(baseline_cpu) / len(baseline_cpu)), 3)
    result["hover_after_full_ms"] = bs.summarize(baseline_hover) if baseline_hover else {"n": 0}
    result["hover_after_cancel_ms"] = bs.summarize(cancel_hover) if cancel_hover else {"n": 0}
    return result


# ---------------------------------------------------------------------------
#  5) 连打 20 次编辑：延迟/队列是否累积
# ---------------------------------------------------------------------------
def run_edit_burst(spec, bursts=3, edits=20):
    """两种模式：
    (a) 逐次等待（每次 didChange 后等 completion 返回）—— 看是否"越打越慢"；
    (b) 一口气灌 20 个 didChange（不等待）再发 completion —— 看队列是否堆积。
    """
    anchor = spec["anchor"]
    path = spec["ref_file"]
    uri = anchor["uri"]
    base_text = bs.read_text(path)
    result = {"files": spec["files"], "sequential": [], "burst": []}
    for rep in range(1, bursts + 1):
        load = new_load()
        client = make_chain_client("lat-burst-%03d-r%d.log" % (spec["files"], rep))
        try:
            client.initialize(spec["project_dir"], timeout=1800.0)
            client.request("textDocument/documentSymbol", {"textDocument": {"uri": uri}},
                           timeout=1800.0)
            client.open_document(path, "verilog", base_text, version=1)
            comp_params = bs.phase2_params(spec, "textDocument/completion", anchor)

            seq = []
            text = base_text
            for index in range(1, edits + 1):
                text = build_edit_text(text, 1000 + index)
                client.notify("textDocument/didChange", {
                    "textDocument": {"uri": uri, "version": 1000 + index},
                    "contentChanges": [{"text": text}]})
                ms, _ = client.collect(client.request_async("textDocument/completion",
                                                           comp_params))
                seq.append(round(ms, 3))
            result["sequential"].append({"rep": rep, "cpu_busy": load.busy_fraction(),
                                         "completion_ms": seq})

            # (b) burst：不等待，灌完再问
            text2 = base_text
            for index in range(1, edits + 1):
                text2 = build_edit_text(text2, 2000 + index)
                client.notify("textDocument/didChange", {
                    "textDocument": {"uri": uri, "version": 2000 + index},
                    "contentChanges": [{"text": text2}]})
            ms, _ = client.collect(client.request_async("textDocument/completion", comp_params),
                                   timeout=1800.0)
            result["burst"].append({"rep": rep, "cpu_busy": load.busy_fraction(),
                                    "completion_ms": round(ms, 3),
                                    "pending_after": len(client._pending)})
        except Exception:
            result.setdefault("fatal", []).append(traceback.format_exc())
        finally:
            try:
                client.close()
            except Exception:
                pass
    values = [row["completion_ms"] for row in result["burst"]]
    result["burst_summary"] = bs.summarize(values) if values else {"n": 0}
    # 逐次模式：前半 vs 后半，看有没有单调上升
    flat = [v for row in result["sequential"] for v in row["completion_ms"]]
    if flat:
        half = len(flat) // 2
        result["sequential_summary"] = bs.summarize(flat)
        result["sequential_first_half_p50"] = bs.summarize(flat[:half], keep_samples=False)["p50"]
        result["sequential_second_half_p50"] = bs.summarize(flat[half:], keep_samples=False)["p50"]
        result["sequential_drift_ratio"] = round(result["sequential_second_half_p50"] /
                                                 max(0.01, result["sequential_first_half_p50"]), 2)
    return result


# ---------------------------------------------------------------------------
def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sizes", default="200,1000")
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--samples", type=int, default=20)
    parser.add_argument("--fast", type=int, default=10)
    parser.add_argument("--report-mb", type=float, default=10.0)
    parser.add_argument("--out", default=os.path.join(OUT_DIR, "latency.json"))
    parser.add_argument("--only", default="",
                        help="methods,chain,hol,cancel,burst 的子集")
    args = parser.parse_args(argv)
    only = {item.strip() for item in args.only.split(",") if item.strip()}
    want = lambda stage: not only or stage in only  # noqa: E731

    os.makedirs(OUT_DIR, exist_ok=True)
    os.makedirs(bs.LOG_DIR, exist_ok=True)
    sizes = [int(x) for x in args.sizes.split(",") if x.strip()]

    report = {
        "meta": {
            "captured_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
            "sizes": sizes, "rounds": args.rounds, "samples": args.samples,
            "fast_requests": args.fast, "report_mb": args.report_mb,
            "native_exe": bs.native_exe_info(),
        }
    }
    load = new_load()
    specs = {}
    for size in sizes:
        spec = bs.ensure_synth_project(size)
        spec["anchor"] = bs.phase2_anchor(spec)
        specs[size] = spec

    if want("methods"):
        report["methods"] = {}
        for size in sizes:
            sys.stderr.write("[latency] methods files=%d\n" % size)
            report["methods"][size] = run_methods(specs[size], args.rounds, args.samples)

    if want("chain"):
        report["key_chain"] = {}
        for size in sizes:
            sys.stderr.write("[latency] key-chain files=%d\n" % size)
            report["key_chain"][size] = run_key_chain(specs[size], args.rounds, args.samples)

    report_size = 200 if 200 in specs else sizes[-1]
    report_spec = bs.ensure_report_project(report_size, args.report_mb)
    report_spec["anchor"] = bs.phase2_anchor(report_spec)

    if want("hol"):
        sys.stderr.write("[latency] head-of-line\n")
        report["head_of_line"] = run_head_of_line(report_spec, args.rounds, args.fast)

    if want("cancel"):
        sys.stderr.write("[latency] cancel\n")
        report["cancel"] = run_cancel(report_spec, args.rounds)

    if want("burst"):
        report["edit_burst"] = {}
        for size in sizes:
            sys.stderr.write("[latency] edit-burst files=%d\n" % size)
            report["edit_burst"][size] = run_edit_burst(specs[size], args.rounds, args.samples)

    load.stop()
    report["meta"]["cpu_busy_fraction_during_run"] = load.busy_fraction()
    report["meta"]["native_exe_at_end"] = bs.native_exe_info()

    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(report, handle, ensure_ascii=False, indent=1)
    sys.stderr.write("[latency] wrote %s\n" % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())