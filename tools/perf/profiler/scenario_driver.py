#!/usr/bin/env python3
"""perf-profiler 的场景驱动（LSP 真实协议），可选择同时用采样器采服务端。

四个场景（任务书 §2）：
    a) 冷启动 + 首次索引（200 文件）× N 次（每次一个新进程）
    b) warm 混合请求流：completion / documentSymbol / hover / definition / moduleTree 各 50 次 × 若干轮
    c) didChange 连打：20 次编辑（每次后跟 completion）× 若干轮
    d) 报告构建：10 MB timing 报告，"强制刷新（冷解析）" + "二次（缓存命中）" 交替若干轮

用法：
    python tools/perf/profiler/scenario_driver.py --exe build-prof/bin/fpgalsp-native.exe \
        --root %TEMP%\\fpgalsp-perf\\profiler\\proj200 --scenario b --with-sampler \
        --out %TEMP%\\fpgalsp-perf\\profiler\\runs
    （--with-sampler 会拉起 sampler.exe 采该 server 的 pid；不加则只测延迟，用于量化采样开销）

输出：<out>/<scenario>_<with|without>_sampler.json（延迟 p50/p95 + 系统负载 + 采样目录）
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import queue
import statistics
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_SAMPLER = os.path.join(HERE, "sampler.exe")


# -----------------------------------------------------------------------------
# 系统负载（GetSystemTimes 差分）
# -----------------------------------------------------------------------------
class FILETIME(ctypes.Structure):
    _fields_ = [("dwLowDateTime", ctypes.c_uint32), ("dwHighDateTime", ctypes.c_uint32)]


def _ft(v: FILETIME) -> int:
    return (v.dwHighDateTime << 32) | v.dwLowDateTime


class LoadMeter:
    def __init__(self) -> None:
        idle, kernel, user = FILETIME(), FILETIME(), FILETIME()
        ctypes.windll.kernel32.GetSystemTimes(ctypes.byref(idle), ctypes.byref(kernel), ctypes.byref(user))
        self.start = (_ft(idle), _ft(kernel), _ft(user))

    def stop(self) -> dict:
        idle, kernel, user = FILETIME(), FILETIME(), FILETIME()
        ctypes.windll.kernel32.GetSystemTimes(ctypes.byref(idle), ctypes.byref(kernel), ctypes.byref(user))
        end = (_ft(idle), _ft(kernel), _ft(user))
        di, dk, du = (end[i] - self.start[i] for i in range(3))
        total = dk + du
        if total <= 0:
            return {"busy_pct": 0.0, "kernel_pct": 0.0, "user_pct": 0.0, "idle_pct": 0.0}
        return {
            "busy_pct": round(100.0 * (total - di) / total, 1),
            "kernel_pct": round(100.0 * dk / total, 1),
            "user_pct": round(100.0 * du / total, 1),
            "idle_pct": round(100.0 * di / total, 1),
        }


# -----------------------------------------------------------------------------
# LSP 客户端（读线程 + 队列，避免阻塞读卡死）
# -----------------------------------------------------------------------------
class LspServer:
    def __init__(self, exe: str, cwd: str) -> None:
        self.proc = subprocess.Popen(
            [exe, "--stdio"], cwd=cwd,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, bufsize=0,
        )
        self.frames: queue.Queue = queue.Queue()
        self.next_id = 0
        self.notifications = 0
        self._stop = False
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

    # ---- 读线程 ----
    def _read_loop(self) -> None:
        buffer = b""
        while not self._stop:
            try:
                chunk = os.read(self.proc.stdout.fileno(), 1 << 16)
            except OSError:
                break
            if not chunk:
                break
            buffer += chunk
            while True:
                head_end = buffer.find(b"\r\n\r\n")
                if head_end < 0:
                    break
                head = buffer[:head_end]
                length = 0
                for line in head.split(b"\r\n"):
                    if line.lower().startswith(b"content-length:"):
                        try:
                            length = int(line.split(b":", 1)[1].strip())
                        except ValueError:
                            length = 0
                total = head_end + 4 + length
                if len(buffer) < total:
                    break
                body = buffer[head_end + 4: total]
                buffer = buffer[total:]
                arrived = time.perf_counter()
                try:
                    frame = json.loads(body.decode("utf-8", "replace"))
                except json.JSONDecodeError:
                    continue
                # ⚠ 把“响应体收全的时刻”带出去：大响应（reports 场景几十 MB）的
                #   json.loads 在客户端要几百 ms，不能算进服务端延迟。
                self.frames.put((frame, arrived, len(body)))
        self.frames.put(None)  # EOF 哨兵

    # ---- 协议 ----
    def send(self, payload: dict) -> None:
        body = json.dumps(payload).encode()
        self.proc.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.proc.stdin.flush()

    def notify(self, method: str, params: dict) -> None:
        self.send({"jsonrpc": "2.0", "method": method, "params": params})

    def request(self, method: str, params: dict, timeout: float = 120.0):
        self.next_id += 1
        rid = self.next_id
        started = time.perf_counter()
        self.send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
        deadline = started + timeout
        while True:
            remaining = deadline - time.perf_counter()
            if remaining <= 0:
                raise TimeoutError(f"{method} (id={rid}) timed out after {timeout}s")
            try:
                frame = self.frames.get(timeout=remaining)
            except queue.Empty:
                raise TimeoutError(f"{method} (id={rid}) timed out after {timeout}s")
            if frame is None:
                raise RuntimeError(f"server closed while waiting for {method}")
            payload, arrived, size = frame
            if payload.get("id") == rid:
                self.last_bytes = size
                return payload, (arrived - started) * 1000.0
            self.notifications += 1

    def initialize(self, root: str) -> None:
        self.request("initialize", {"processId": os.getpid(), "rootUri": file_uri(root),
                                    "capabilities": {}})
        self.notify("initialized", {})

    def close(self) -> None:
        try:
            self.request("shutdown", {}, timeout=15)
            self.notify("exit", {})
        except Exception:
            pass
        self._stop = True
        try:
            self.proc.stdin.close()
        except Exception:
            pass
        try:
            self.proc.wait(timeout=10)
        except Exception:
            self.proc.kill()


def file_uri(path: str) -> str:
    p = path.replace("\\", "/")
    return ("file://" + p) if p.startswith("/") else ("file:///" + p)


# -----------------------------------------------------------------------------
# 采样器会话
# -----------------------------------------------------------------------------
class SamplerSession:
    def __init__(self, sampler: str, pid: int, exe: str, out_dir: str, interval_ms: int) -> None:
        os.makedirs(out_dir, exist_ok=True)
        self.out_dir = out_dir
        self.proc = subprocess.Popen(
            [sampler, "--pid", str(pid), "--exe", exe, "--out", out_dir,
             "--interval-ms", str(interval_ms)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0,
        )
        # 等采样器把 PDB 符号加载完（它会打印一行 ready）。否则首次 StackWalk 的
        # ~1 s 符号加载会落在被测场景的窗口里，把样本数打没（实测冷索引只剩 32 sweep）。
        ready = False
        deadline = time.perf_counter() + 30.0
        while time.perf_counter() < deadline:
            line = self.proc.stdout.readline()
            if not line:
                break
            if b"ready" in line:
                ready = True
                break
        if not ready:
            raise RuntimeError("sampler did not report ready")

    def stop(self) -> str:
        try:
            self.proc.stdin.write(b"stop\n")
            self.proc.stdin.flush()
            self.proc.stdin.close()
        except Exception:
            pass
        try:
            out, err = self.proc.communicate(timeout=60)
        except Exception:
            self.proc.kill()
            return ""
        text = (out or b"").decode("utf-8", "replace") + (err or b"").decode("utf-8", "replace")
        return text.strip()


# -----------------------------------------------------------------------------
# 场景（workload）
# -----------------------------------------------------------------------------
def open_top(server: LspServer, top_file: str, text: str) -> str:
    uri = file_uri(top_file)
    server.notify("textDocument/didOpen", {"textDocument": {"uri": uri, "languageId": "verilog",
                                                            "version": 1, "text": text}})
    return uri


def workload_cold_index(server: LspServer, root: str, top_file: str, text: str,
                        clones: list | None = None) -> list[float]:
    """冷启动后的首次索引。

    有 clones 时：**一个 server 进程**里依次对 N 份独立工程副本做首次索引 ——
    之所以不在 N 个进程里做，是因为 sampler 每个进程首次 StackWalk 要加载
    fpgalsp-native.pdb（实测 ~1 s），会把 50 ms 的索引完全淹没。
    """
    server.initialize(root)
    lat: list[float] = []
    if clones:
        for clone in clones:
            croot = clone["root"]
            ctop = clone["top_file"]
            ctext = open(ctop, encoding="utf-8", errors="replace").read()
            uri = file_uri(ctop)
            server.notify("textDocument/didOpen", {"textDocument": {
                "uri": uri, "languageId": "verilog", "version": 1, "text": ctext}})
            _, ms = server.request("textDocument/documentSymbol", {"textDocument": {"uri": uri}})
            lat.append(ms)
        return lat
    uri = open_top(server, top_file, text)
    _, ms = server.request("textDocument/documentSymbol", {"textDocument": {"uri": uri}})
    return [ms]


def workload_warm(server: LspServer, root: str, top_file: str, text: str, min_ms: int):
    server.initialize(root)
    uri = open_top(server, top_file, text)
    server.request("textDocument/documentSymbol", {"textDocument": {"uri": uri}})  # 预热索引
    pos = {"line": 10, "character": 8}
    calls = {
        "completion": lambda: server.request("textDocument/completion", {"textDocument": {"uri": uri}, "position": pos}),
        "documentSymbol": lambda: server.request("textDocument/documentSymbol", {"textDocument": {"uri": uri}}),
        "hover": lambda: server.request("textDocument/hover", {"textDocument": {"uri": uri}, "position": pos}),
        "definition": lambda: server.request("textDocument/definition", {"textDocument": {"uri": uri}, "position": pos}),
        "moduleTree": lambda: server.request("fpgaLsp/moduleTree", {"uri": uri}),
    }
    lat: dict[str, list[float]] = {k: [] for k in calls}
    started = time.perf_counter()
    rounds = 0
    while (time.perf_counter() - started) * 1000.0 < min_ms or rounds < 2:
        rounds += 1
        for name, fn in calls.items():
            for _ in range(50):
                _, ms = fn()
                lat[name].append(ms)
    return lat


def workload_didchange(server: LspServer, root: str, top_file: str, text: str, min_ms: int):
    server.initialize(root)
    uri = open_top(server, top_file, text)
    server.request("textDocument/documentSymbol", {"textDocument": {"uri": uri}})  # 预热
    lines = text.split("\n")
    version = 1
    edits: list[float] = []
    completions: list[float] = []
    started = time.perf_counter()
    bursts = 0
    while (time.perf_counter() - started) * 1000.0 < min_ms or bursts < 2:
        bursts += 1
        for i in range(20):  # 连打 20 次编辑
            version += 1
            lines.insert(1, f"// burst {bursts} edit {i}")
            new_text = "\n".join(lines)
            t0 = time.perf_counter()
            server.notify("textDocument/didChange", {
                "textDocument": {"uri": uri, "version": version},
                "contentChanges": [{"text": new_text}],
            })
            # 紧跟一个 completion：把主线程从"处理 didChange 队列"里逼出来，可观测串行等待
            _, ms = server.request("textDocument/completion",
                                   {"textDocument": {"uri": uri},
                                    "position": {"line": 12, "character": 8}})
            completions.append(ms)
            edits.append((time.perf_counter() - t0) * 1000.0 - ms)
    return {"didChange": edits, "completion_after_edit": completions}


def workload_reports(server: LspServer, root: str, top_file: str, text: str, min_ms: int):
    server.initialize(root)
    uri = open_top(server, top_file, text)
    server.request("textDocument/documentSymbol", {"textDocument": {"uri": uri}})
    cold: list[float] = []
    cached: list[float] = []
    errors: list[str] = []
    started = time.perf_counter()
    rounds = 0
    while (time.perf_counter() - started) * 1000.0 < min_ms or rounds < 2:
        rounds += 1
        frame, ms = server.request("fpgaLsp/reports", {"uri": uri, "section": "timing", "refresh": True})
        cold.append(ms)
        if isinstance(frame.get("result"), dict) and frame["result"].get("error"):
            errors.append(str(frame["result"]["error"])[:160])
        frame2, ms2 = server.request("fpgaLsp/reports", {"uri": uri, "section": "timing"})
        cached.append(ms2)
        if isinstance(frame2.get("result"), dict) and frame2["result"].get("error"):
            errors.append(str(frame2["result"]["error"])[:160])
    return {"reports_cold": cold, "reports_cached": cached, "errors": errors[:3]}


# -----------------------------------------------------------------------------
def stats(values: list[float]) -> dict:
    if not values:
        return {"n": 0}
    ordered = sorted(values)
    def pct(p: float) -> float:
        idx = min(len(ordered) - 1, max(0, round(p * (len(ordered) - 1))))
        return ordered[idx]
    return {"n": len(ordered), "p50_ms": round(statistics.median(ordered), 3),
            "p95_ms": round(pct(0.95), 3), "max_ms": round(ordered[-1], 3),
            "mean_ms": round(statistics.fmean(ordered), 3)}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--root", required=True)
    ap.add_argument("--scenario", required=True, choices=list("abcd"))
    ap.add_argument("--out", default=os.path.join(HERE, "out"))
    ap.add_argument("--with-sampler", action="store_true")
    ap.add_argument("--sampler", default=DEFAULT_SAMPLER)
    ap.add_argument("--interval-ms", type=int, default=2)
    ap.add_argument("--min-ms", type=int, default=6000, help="b/c/d 至少跑这么久（凑样本）")
    ap.add_argument("--cold-repeats", type=int, default=10, help="场景 a 的冷启动次数")
    args = ap.parse_args()

    meta_path = os.path.join(args.root, "meta.json")
    meta = json.load(open(meta_path, encoding="utf-8")) if os.path.exists(meta_path) else {}
    top = meta.get("top", "mod_000")
    top_file = os.path.join(args.root, "rtl", f"{top}.v")
    text = open(top_file, encoding="utf-8", errors="replace").read()

    os.makedirs(args.out, exist_ok=True)
    tag = f"{args.scenario}_{'with' if args.with_sampler else 'without'}_sampler"
    load = LoadMeter()
    sampler_logs: list[str] = []

    by_method: dict[str, list[float]] = {}
    all_lat: list[float] = []
    extra: dict = {}

    if args.scenario == "a":
        # 冷索引：优先用 make_project.py --clones 生成的 N 份独立工程副本，
        # 在**同一个 server 进程**里各做一次"首次索引"（见 workload_cold_index 注释）。
        clones = meta.get("clones") or []
        server = LspServer(args.exe, args.root)
        session = None
        try:
            if args.with_sampler:
                session = SamplerSession(args.sampler, server.proc.pid, args.exe,
                                         os.path.join(args.out, f"sample_{tag}"), args.interval_ms)
            lat = workload_cold_index(server, args.root, top_file, text, clones or None)
            all_lat += lat
            by_method.setdefault("cold_index", []).extend(lat)
            extra["clones"] = len(clones)
        finally:
            if session is not None:
                sampler_logs.append(session.stop())
            server.close()
    else:
        server = LspServer(args.exe, args.root)
        session = None
        try:
            if args.with_sampler:
                session = SamplerSession(args.sampler, server.proc.pid, args.exe,
                                         os.path.join(args.out, f"sample_{tag}"), args.interval_ms)
            if args.scenario == "b":
                by_method = workload_warm(server, args.root, top_file, text, args.min_ms)
            elif args.scenario == "c":
                by_method = workload_didchange(server, args.root, top_file, text, args.min_ms)
            else:
                by_method = workload_reports(server, args.root, top_file, text, args.min_ms)
                extra["errors"] = by_method.pop("errors", [])
            all_lat = [v for vs in by_method.values() for v in vs]
        finally:
            if session is not None:
                sampler_logs.append(session.stop())
            server.close()

    load_stats = load.stop()
    result = {
        "scenario": args.scenario,
        "with_sampler": args.with_sampler,
        "stats": stats(all_lat),
        "by_method": {k: stats(v) for k, v in by_method.items()},
        "extra": extra,
        "system_load": load_stats,
        "sampler_logs": [s for s in sampler_logs if s][:4],
        "sample_dir": os.path.join(args.out, f"sample_{tag}") if args.with_sampler else None,
    }
    path = os.path.join(args.out, f"{tag}.json")
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(result, fh, indent=1)

    s = result["stats"]
    print(f"csv,scenario={args.scenario},with_sampler={int(args.with_sampler)},n={s.get('n')},"
          f"p50={s.get('p50_ms')},p95={s.get('p95_ms')},max={s.get('max_ms')},"
          f"busy_pct={load_stats['busy_pct']},kernel_pct={load_stats['kernel_pct']}")
    for name, st in result["by_method"].items():
        print(f"csv,method={name},n={st.get('n')},p50={st.get('p50_ms')},p95={st.get('p95_ms')}")
    if extra.get("errors"):
        print(f"[driver] report errors: {extra['errors']}")
    print(f"[driver] wrote {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())