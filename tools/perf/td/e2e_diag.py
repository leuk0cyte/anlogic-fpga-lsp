#!/usr/bin/env python
"""perf-td：LSP 端到端 —— 冷启动 / didChange / didSave / codeAction(lint) / TD 检查。

回答 task-32 的 ①②③④：
  * initialize 冷启动里有没有 TD（预期：没有；C 级证据在源码里，这里给 M 级对照）
  * didChange -> publishDiagnostics 的端到端延迟（是否有 lint/TD 混在按键路径里）
  * didSave（enableTd=false / true）的延迟，以及真实 TD 检查的墙钟
  * codeAction 请求成本（**唯一**调用内嵌 Verible lint 的入口）13 KB vs 100 KB
  * 5 连发保存时：TD 进程启动几次、并发几个、是否取消在跑的一轮

安全：只对 %TEMP%\\fpgalsp-perf\\td 下的合成工程发请求；不碰真实工程。
      TD 进程由服务端以 <synthetic>/.fpgalsp/run 为 cwd 启动，日志落在临时目录。

用法：
    python tools\\perf\\td\\make_project.py
    python tools\\perf\\td\\e2e_diag.py --exe build\\bin\\fpgalsp-native.exe
"""

from __future__ import annotations

import argparse
import ctypes
import datetime
import json
import os
import queue
import re
import statistics
import sys
import threading
import time
from ctypes import wintypes

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, os.path.join(REPO, "tests", "golden", "tools"))

from goldenlib.client import NativeLspClient, path_to_uri  # noqa: E402

# -----------------------------------------------------------------------------
# Windows 小工具：进程枚举 / 进程 CPU
# -----------------------------------------------------------------------------
TH32CS_SNAPPROCESS = 0x00000002
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("cntUsage", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)),
        ("th32ModuleID", wintypes.DWORD),
        ("cntThreads", wintypes.DWORD),
        ("th32ParentProcessID", wintypes.DWORD),
        ("pcPriClassBase", ctypes.c_long),
        ("dwFlags", wintypes.DWORD),
        ("szExeFile", wintypes.WCHAR * 260),
    ]


_kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
_kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
_kernel32.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
_kernel32.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
_kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
_kernel32.OpenProcess.restype = wintypes.HANDLE


def td_pids() -> list[tuple[int, int]]:
    """当前 td_commands_prompt.exe 的 [(pid, parent_pid)] 列表（Toolhelp 快照，~0.1 ms）。"""
    snapshot = _kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snapshot == wintypes.HANDLE(-1).value:
        return []
    entry = PROCESSENTRY32W()
    entry.dwSize = ctypes.sizeof(PROCESSENTRY32W)
    found: list[tuple[int, int]] = []
    try:
        if not _kernel32.Process32FirstW(snapshot, ctypes.byref(entry)):
            return []
        while True:
            if entry.szExeFile.lower() == "td_commands_prompt.exe":
                found.append((int(entry.th32ProcessID), int(entry.th32ParentProcessID)))
            if not _kernel32.Process32NextW(snapshot, ctypes.byref(entry)):
                break
    finally:
        _kernel32.CloseHandle(snapshot)
    return found


def td_tree_pids(root_pid: int) -> list[int]:
    """只算**我们这台 server** 派生出来的 TD 进程（沿 parent 链回溯到 root_pid）。

    机器上可能有别人的 TD 进程（同一台机器多个 agent 共用 build/bin），
    不做归属过滤会把别人的进程算进"我们起了几个"，这是"把责任甩给外部"的经典翻车点。
    """
    pairs = td_pids()
    by_pid = {pid: parent for pid, parent in pairs}
    ours: list[int] = []
    for pid, parent in pairs:
        cursor = parent
        depth = 0
        while cursor and depth < 8:
            if cursor == root_pid:
                ours.append(pid)
                break
            cursor = by_pid.get(cursor, 0)
            depth += 1
    return ours


def process_cpu_ms(pid: int) -> float:
    handle = _kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        return 0.0
    creation, exit_time, kernel, user = (wintypes.FILETIME() for _ in range(4))
    try:
        if not ctypes.windll.kernel32.GetProcessTimes(
                handle, ctypes.byref(creation), ctypes.byref(exit_time),
                ctypes.byref(kernel), ctypes.byref(user)):
            return 0.0
    finally:
        _kernel32.CloseHandle(handle)

    def ticks(ft: wintypes.FILETIME) -> int:
        return (ft.dwHighDateTime << 32) | ft.dwLowDateTime

    return (ticks(kernel) + ticks(user)) / 10000.0


# -----------------------------------------------------------------------------
# 统计
# -----------------------------------------------------------------------------

def pct(values: list[float], q: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    pos = q * (len(ordered) - 1)
    lo = int(pos)
    hi = min(lo + 1, len(ordered) - 1)
    frac = pos - lo
    return ordered[lo] * (1 - frac) + ordered[hi] * frac


def row(label: str, values: list[float]) -> str:
    if not values:
        return f"{label:<46} n=0"
    return (f"{label:<46} n={len(values):<3} p50={pct(values, .5):8.3f}  "
            f"p95={pct(values, .95):8.3f}  max={max(values):8.3f}  ms")


def emit_csv(label: str, values: list[float]) -> None:
    if not values:
        return
    print(f"csv,{label},{len(values)},{pct(values, .5):.4f},{pct(values, 95 / 100):.4f},"
          f"{max(values):.4f}")


# -----------------------------------------------------------------------------
# LSP 客户端封装
# -----------------------------------------------------------------------------

class PublishRecorder:
    """给 publishDiagnostics 打接收时间戳（在 client 的 reader 线程里）。"""

    def __init__(self, client: NativeLspClient):
        self._lock = threading.Lock()
        self.events: list[tuple[float, str, int]] = []
        self._orig = client._dispatch

        def patched(msg):
            if msg.get("method") == "textDocument/publishDiagnostics":
                params = msg.get("params") or {}
                with self._lock:
                    self.events.append((
                        time.perf_counter(),
                        params.get("uri") or "",
                        len(params.get("diagnostics") or []),
                    ))
            return self._orig(msg)

        client._dispatch = patched

    def clear(self) -> None:
        with self._lock:
            self.events.clear()

    def snapshot(self) -> list[tuple[float, str, int]]:
        with self._lock:
            return list(self.events)

    def wait_for(self, uri: str, after: float, timeout: float = 60.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            for ts, event_uri, count in self.snapshot():
                if ts >= after and event_uri == uri:
                    return ts, count
            time.sleep(0.0005)
        return None


def init_client(client: NativeLspClient, root: str, settings: dict, timeout: float = 120.0):
    client.request("initialize", {
        "processId": os.getpid(),
        "rootUri": path_to_uri(root),
        "clientInfo": {"name": "perf-td", "version": "1"},
        "capabilities": {
            "textDocument": {"completion": {}, "hover": {}, "documentSymbol": {},
                             "semanticTokens": {}, "foldingRange": {}, "inlayHint": {},
                             "codeAction": {}},
            "workspace": {"symbol": {}},
        },
        "initializationOptions": {"fpgaLsp": settings},
    }, timeout=timeout)
    client.notify("initialized", {})


def spawn_client(exe: str, root: str, log_path: str, settings: dict) -> NativeLspClient:
    client = NativeLspClient(REPO, log_path, exe_path=exe)
    init_client(client, root, settings)
    return client


class ConcurrentClient(NativeLspClient):
    """支持多线程同时发请求的客户端。

    goldenlib 的 ``request()`` 从**共享队列**里取响应并丢弃不属于自己的 id，
    两个线程同时用会互相偷响应。这里改成"按 id 分发的 future + 写锁"，
    用来做「codeAction 正在跑时 didChange 会不会被堵住」这类并发实验。
    """

    def __init__(self, repo_root: str, log_path: str, exe_path: str):
        self._pending: dict[int, queue.Queue] = {}
        self._pending_lock = threading.Lock()
        self._write_lock = threading.Lock()
        super().__init__(repo_root, log_path, exe_path=exe_path)
        self._next_id = 0

    def _dispatch(self, msg):  # 在 reader 线程里
        msg_id = msg.get("id")
        if msg_id is not None and ("result" in msg or "error" in msg):
            with self._pending_lock:
                future = self._pending.pop(msg_id, None)
            if future is not None:
                future.put(msg)
                return
        super()._dispatch(msg)

    def call(self, method: str, params=None, timeout: float = 120.0):
        with self._pending_lock:
            self._next_id += 1
            msg_id = self._next_id
            future: queue.Queue = queue.Queue()
            self._pending[msg_id] = future
        with self._write_lock:
            self.send(method, params, msg_id)
        try:
            msg = future.get(timeout=timeout)
        except queue.Empty:
            raise RuntimeError(f"timeout waiting for {method}")
        if "error" in msg:
            raise RuntimeError(f"JSON-RPC error for {method}: {msg['error']}")
        return msg.get("result")


def background_usage(seconds: float = 0.4) -> float:
    """采一段系统 CPU 使用率（0..1），用于报告"当时系统负载"。"""
    def sample():
        idle = wintypes.FILETIME()
        kernel = wintypes.FILETIME()
        user = wintypes.FILETIME()
        ctypes.windll.kernel32.GetSystemTimes(ctypes.byref(idle), ctypes.byref(kernel),
                                             ctypes.byref(user))
        def ticks(ft):
            return (ft.dwHighDateTime << 32) | ft.dwLowDateTime
        return ticks(idle), ticks(kernel) + ticks(user)

    i0, b0 = sample()
    time.sleep(seconds)
    i1, b1 = sample()
    total = (i1 - i0) + (b1 - b0)
    if total <= 0:
        return 0.0
    return 1.0 - (i1 - i0) / total


# -----------------------------------------------------------------------------
# 场景
# -----------------------------------------------------------------------------

def scenario_cold_start(exe: str, root: str, log_dir: str, runs: int) -> None:
    latencies = []
    init_only = []
    print("== ① 冷启动（进程 spawn -> initialize 响应）==")
    for i in range(runs):
        log_path = os.path.join(log_dir, f"cold_{i}.log")
        started = time.perf_counter()
        client = NativeLspClient(REPO, log_path, exe_path=exe)
        try:
            init_client(client, root, {"enableTd": False, "tdCheckOnSave": False})
            init_only.append((time.perf_counter() - started) * 1000.0)
            # 再发一个 trivial 请求：确认"响应已经真的可服务"
            client.request("textDocument/documentSymbol", {
                "textDocument": {"uri": path_to_uri(os.path.join(root, "src", "top.v"))},
            }, timeout=60)
            latencies.append((time.perf_counter() - started) * 1000.0)
        finally:
            client.close()
    print(row("spawn+initialize (cold process)", init_only))
    print(row("spawn+initialize+documentSymbol", latencies))
    emit_csv("cold_start", init_only)
    print()


def scenario_did_change(exe: str, root: str, log_dir: str, iters: int) -> None:
    print("== ② didChange -> publishDiagnostics（按键路径，enableTd=false）==")
    client = spawn_client(exe, root, os.path.join(log_dir, "didchange.log"),
                          {"enableTd": False, "tdCheckOnSave": False})
    try:
        recorder = PublishRecorder(client)
        path = os.path.join(root, "src", "small.v")
        uri = path_to_uri(path)
        text = client.open_document(path, "verilog")
        recorder.wait_for(uri, time.perf_counter(), timeout=60)

        lines = text.split("\n")
        target_line = 1  # `  wire [7:0] w_0;` 之前，插一个空格不会改变语义
        characters = len(lines[target_line]) if target_line < len(lines) else 0
        del characters  # 只在行首插入

        latencies = []
        cpu_total = 0.0
        blocking = []
        for version in range(2, iters + 2):
            recorder.clear()
            started = time.perf_counter()
            cpu0 = process_cpu_ms(client.proc.pid)
            client.notify("textDocument/didChange", {
                "textDocument": {"uri": uri, "version": version},
                "contentChanges": [{
                    "range": {"start": {"line": target_line, "character": 0},
                              "end": {"line": target_line, "character": 0}},
                    "text": " ",
                }],
            })
            event = recorder.wait_for(uri, started, timeout=30)
            if event is None:
                print(f"  didChange#{version}: NO publishDiagnostics (timeout)")
                continue
            cpu_total += max(0.0, process_cpu_ms(client.proc.pid) - cpu0)
            latencies.append((event[0] - started) * 1000.0)
            # "阻塞感"：didChange 是同步 handler，后一个请求要排在它后面
            t_after = time.perf_counter()
            client.request("textDocument/documentSymbol",
                           {"textDocument": {"uri": uri}}, timeout=30)
            blocking.append((time.perf_counter() - t_after) * 1000.0)

        print(row("didChange -> publishDiagnostics", latencies))
        emit_csv("didchange", latencies)
        print(row("follow-up request RTT after didChange", blocking))
        if latencies:
            print(f"  server CPU per didChange (avg): "
                  f"{cpu_total / len(latencies):.3f} ms  (wall p50 {pct(latencies, .5):.3f})")

        # didSave（同一文件，整文本；enableTd=false 时不应起 TD）
        save_latencies = []
        for version in range(iters + 2, iters + 2 + min(iters, 10)):
            recorder.clear()
            started = time.perf_counter()
            client.notify("textDocument/didSave", {
                "textDocument": {"uri": uri},
                "text": text,
            })
            event = recorder.wait_for(uri, started, timeout=30)
            if event is None:
                continue
            save_latencies.append((event[0] - started) * 1000.0)
        print(row("didSave  -> publishDiagnostics", save_latencies))
        emit_csv("didsave", save_latencies)
        td = td_pids()
        print(f"  TD processes after didSave burst: {len(td)} {td}")
    finally:
        client.close()
    print()


def scenario_code_action(exe: str, targets: list[tuple[str, str]], log_dir: str, iters: int) -> None:
    print("== ③ codeAction（唯一调用内嵌 Verible lint 的入口）==")
    client = spawn_client(exe, targets[0][0], os.path.join(log_dir, "codeaction.log"),
                          {"enableTd": False, "tdCheckOnSave": False})
    try:
        recorder = PublishRecorder(client)
        for project, name in targets:
            path = os.path.join(project, "src", name)
            if not os.path.exists(path):
                print(f"  skip {path} (missing)")
                continue
            uri = path_to_uri(path)
            size = os.path.getsize(path)
            client.open_document(path, "verilog")
            recorder.wait_for(uri, time.perf_counter(), timeout=120)

            latencies = []
            cpu_latencies = []
            result = []
            for _ in range(iters):
                cpu0 = process_cpu_ms(client.proc.pid)
                started = time.perf_counter()
                result = client.request("textDocument/codeAction", {
                    "textDocument": {"uri": uri},
                    "range": {"start": {"line": 0, "character": 0},
                              "end": {"line": 0, "character": 0}},
                    "context": {"diagnostics": []},
                }, timeout=120)
                latencies.append((time.perf_counter() - started) * 1000.0)
                cpu_latencies.append(max(0.0, process_cpu_ms(client.proc.pid) - cpu0))
            actions = len(result) if isinstance(result, list) else -1
            print(row(f"codeAction {name} ({size / 1024:.1f} KB)", latencies))
            emit_csv(f"codeaction_{name}", latencies)
            print(f"  server CPU per codeAction (p50): "
                  f"{pct(cpu_latencies, .5):.3f} ms  actions={actions}")
    finally:
        client.close()
    print()


def _count_log_lines(log_path: str, needle: str) -> int:
    try:
        with open(log_path, encoding="utf-8", errors="replace") as handle:
            return sum(1 for line in handle if needle in line)
    except OSError:
        return 0


_LOG_LINE = re.compile(r"^\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3})\] \[[a-z]+\] (.*)$")
_LOG_DURATION = re.compile(r"in ([\d.]+)s")


def read_td_rounds(log_path: str) -> list[tuple[datetime.datetime, float, str]]:
    """从服务端日志里取 TD 轮次：[(完成时刻, TD 内部秒数, 原始行)]。"""
    rounds: list[tuple[datetime.datetime, float, str]] = []
    try:
        with open(log_path, encoding="utf-8", errors="replace") as handle:
            for line in handle:
                match = _LOG_LINE.match(line.strip())
                if not match:
                    continue
                text = match.group(2)
                if "TD check complete" not in text and "TD check failed" not in text:
                    continue
                stamp = datetime.datetime.strptime(match.group(1), "%Y-%m-%d %H:%M:%S.%f")
                duration = _LOG_DURATION.search(text)
                rounds.append((stamp, float(duration.group(1)) if duration else 0.0, text))
    except OSError:
        pass
    return rounds


def scenario_td_burst(exe: str, project: str, log_dir: str, saves: int, interval_ms: int) -> None:
    """5 连发保存：TD 进程启动几次？并发几个？在跑的一轮会不会被取消？"""
    print(f"== ④ TD 检查：{saves} 连发保存（间隔 {interval_ms} ms，enableTd=true）==")
    log_path = os.path.join(log_dir, "td_burst.log")
    if os.path.exists(log_path):
        os.remove(log_path)
    client = spawn_client(exe, project, log_path, {"enableTd": True, "tdCheckOnSave": True})
    try:
        recorder = PublishRecorder(client)
        path = os.path.join(project, "src", "top.v")
        uri = path_to_uri(path)
        text = client.open_document(path, "verilog")
        recorder.wait_for(uri, time.perf_counter(), timeout=60)

        samples: list[tuple[float, list[int]]] = []
        stop = threading.Event()
        server_pid = client.proc.pid

        def poll() -> None:
            while not stop.is_set():
                samples.append((time.perf_counter(), td_tree_pids(server_pid)))
                time.sleep(0.02)

        poller = threading.Thread(target=poll, daemon=True)
        poller.start()

        recorder.clear()
        save_times = []
        burst_start = time.perf_counter()
        burst_wall = datetime.datetime.now()
        for i in range(saves):
            save_times.append(time.perf_counter())
            client.notify("textDocument/didSave", {
                "textDocument": {"uri": uri},
                "text": text,
            })
            time.sleep(interval_ms / 1000.0)
        last_save = save_times[-1]

        # 等 TD 收尾：日志里出现两轮 "TD check complete" 或超时
        deadline = time.perf_counter() + 180.0
        while time.perf_counter() < deadline:
            rounds = len(read_td_rounds(log_path))
            if rounds >= 2:
                break
            if time.perf_counter() - last_save > 60.0:
                break
            time.sleep(0.05)
        # 再给最后一条发布留一点时间
        time.sleep(1.0)
        stop.set()
        poller.join(timeout=2.0)

        round_events = read_td_rounds(log_path)
        distinct_pids = sorted({pid for _, pids in samples for pid in pids})
        max_concurrent = max((len(pids) for _, pids in samples), default=0)
        events = recorder.snapshot()
        publishes_after = [e for e in events if e[0] >= burst_start]
        last_publish = publishes_after[-1] if publishes_after else None

        print(f"  saves at +{', +'.join(f'{(t - burst_start) * 1000:.0f}ms' for t in save_times)}")
        print(f"  TD rounds (log): {len(round_events)}")
        for stamp, seconds, text in round_events:
            relative = (stamp - burst_wall).total_seconds() * 1000.0
            print(f"    round done at +{relative:8.1f} ms  (TD 内部 {seconds:.2f}s)  {text[:70]}")
        print(f"  TD processes seen (our server tree, distinct PIDs): {len(distinct_pids)} "
              f"{distinct_pids}")
        print(f"  TD max concurrent: {max_concurrent}")
        print(f"  'TD 检查进行中，已排队一次重跑' lines: "
              f"{_count_log_lines(log_path, '已排队一次重跑')} "
              f"(每次 = 一次请求被排队而不是取消)")
        print(f"  publishDiagnostics events after burst: {len(publishes_after)} "
              f"(non-empty: {sum(1 for _, _, n in publishes_after if n)})")
        if last_publish:
            print(f"  settle time (last save -> last publish): "
                  f"{(last_publish[0] - last_save) * 1000:.1f} ms")
        if len(round_events) >= 2:
            first_done = (round_events[0][0] - burst_wall).total_seconds() * 1000.0
            last_done = (round_events[-1][0] - burst_wall).total_seconds() * 1000.0
            print(f"  extra round cost (round2 done - round1 done): {last_done - first_done:.1f} ms")
        for ts, _, count in publishes_after[:8]:
            print(f"    publish at +{(ts - burst_start) * 1000:8.1f} ms  diags={count}")

        # 关键判据：没有 cancel 的证据 = 没有任何一轮在"另一轮还在跑"时被替换
        print(f"  verdict: rounds={len(round_events)} spawns={len(distinct_pids)} "
              f"concurrent_max={max_concurrent} "
              f"-> {'单飞生效，但没有去抖/取消（旧策略）' if max_concurrent <= 1 else '并发>1，单飞失效'}")
    finally:
        client.close()
    print()


def scenario_concurrency(exe: str, project: str, log_dir: str, iters: int) -> None:
    """★ 关键对照：codeAction（同步、全量 lint）正在跑时，didChange 会被堵多久？

    codeAction 不在 AsyncDispatcher 的 heavy 白名单里 -> 它在 **IO 线程**上同步执行；
    而 didChange/didSave 也在同一个 IO 线程。所以一个 100 KB 的 codeAction 会把
    后面所有消息（包括按键）一起卡住。这里用同一个 server 进程做两组测量。
    """
    print("== ⑤ 对照实验：codeAction 正在跑时 didChange 的延迟 ==")
    client = ConcurrentClient(REPO, os.path.join(log_dir, "concurrency.log"), exe_path=exe)
    init_client(client, project, {"enableTd": False, "tdCheckOnSave": False})
    try:
        recorder = PublishRecorder(client)
        small = os.path.join(project, "src", "small.v")
        big = os.path.join(project, "src", "big.v")
        small_uri = path_to_uri(small)
        big_uri = path_to_uri(big)
        small_text = client.open_document(small, "verilog")
        client.open_document(big, "verilog")
        recorder.wait_for(small_uri, time.perf_counter(), timeout=120)

        def measure_did_change(rounds: int, tag: str) -> list[float]:
            latencies: list[float] = []
            for version in range(2, rounds + 2):
                recorder.clear()
                started = time.perf_counter()
                client.notify("textDocument/didChange", {
                    "textDocument": {"uri": small_uri, "version": version},
                    "contentChanges": [{
                        "range": {"start": {"line": 1, "character": 0},
                                  "end": {"line": 1, "character": 0}},
                        "text": " ",
                    }],
                })
                event = recorder.wait_for(small_uri, started, timeout=120)
                if event:
                    latencies.append((event[0] - started) * 1000.0)
            print(row(f"didChange {tag}", latencies))
            emit_csv(f"didchange_{tag}", latencies)
            return latencies

        baseline = measure_did_change(iters, "baseline (no concurrent work)")

        # 后台线程持续发 codeAction（每个 ~330 ms，全部落在 IO 线程上）
        stop = threading.Event()
        codes: list[float] = []

        def load() -> None:
            code_action_params = {
                "textDocument": {"uri": big_uri},
                "range": {"start": {"line": 0, "character": 0},
                          "end": {"line": 0, "character": 0}},
                "context": {"diagnostics": []},
            }
            while not stop.is_set():
                started = time.perf_counter()
                try:
                    client.call("textDocument/codeAction", code_action_params, timeout=120)
                except Exception:  # noqa: BLE001 - 关停时可能抛，忽略
                    break
                codes.append((time.perf_counter() - started) * 1000.0)

        worker = threading.Thread(target=load, daemon=True)
        worker.start()
        time.sleep(0.1)
        loaded = measure_did_change(iters, "under codeAction load")
        stop.set()
        worker.join(timeout=90)

        if baseline and loaded:
            print(f"  didChange p50: {pct(baseline, .5):.1f} -> {pct(loaded, .5):.1f} ms "
                  f"(+{pct(loaded, .5) - pct(baseline, .5):.1f})")
            print(f"  didChange p95: {pct(baseline, .95):.1f} -> {pct(loaded, .95):.1f} ms "
                  f"(×{pct(loaded, .95) / max(pct(baseline, .95), 0.001):.1f})")
        print(f"  concurrent codeAction calls: n={len(codes)} "
              f"p50={pct(codes, .5):.1f} ms (每个都独占 IO 线程)")
    finally:
        client.close()
    print()


def scenario_td_checks(exe: str, project: str, log_dir: str, count: int) -> None:
    """5 个并发 fpgalsp.check：TD 进程起几个？并发几个？结束后有没有残留？"""
    print(f"== ⑥ {count} 个并发 fpgalsp.check（enableTd=true，tdCheckOnSave=false）==")
    log_path = os.path.join(log_dir, "td_checks.log")
    if os.path.exists(log_path):
        os.remove(log_path)
    client = ConcurrentClient(REPO, log_path, exe_path=exe)
    init_client(client, project, {"enableTd": True, "tdCheckOnSave": False})
    try:
        path = os.path.join(project, "src", "top.v")
        client.open_document(path, "verilog")
        server_pid = client.proc.pid

        samples: list[tuple[float, list[int]]] = []
        stop = threading.Event()

        def poll() -> None:
            while not stop.is_set():
                samples.append((time.perf_counter(), td_tree_pids(server_pid)))
                time.sleep(0.02)

        poller = threading.Thread(target=poll, daemon=True)
        poller.start()

        barrier = threading.Barrier(count)
        results: list[dict] = []
        errors: list[str] = []

        def fire() -> None:
            try:
                barrier.wait(timeout=10)
                started = time.perf_counter()
                reply = client.call("workspace/executeCommand",
                                    {"command": "fpgalsp.check", "arguments": []}, timeout=120)
                results.append({"ok": isinstance(reply, dict) and reply.get("ok"),
                                "ms": (time.perf_counter() - started) * 1000.0,
                                "reply": reply if isinstance(reply, dict) else None})
            except Exception as exc:  # noqa: BLE001
                errors.append(str(exc))

        threads = [threading.Thread(target=fire, daemon=True) for _ in range(count)]
        started = time.perf_counter()
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(timeout=180)
        replies_done = time.perf_counter()

        # 等 TD 收尾（最多 120 s）
        deadline = time.perf_counter() + 120
        while time.perf_counter() < deadline:
            if len(read_td_rounds(log_path)) >= 2:
                break
            time.sleep(0.1)
        time.sleep(2.0)
        stop.set()
        poller.join(timeout=2)

        rounds = read_td_rounds(log_path)
        distinct = sorted({pid for _, pids in samples for pid in pids})
        max_concurrent = max((len(pids) for _, pids in samples), default=0)
        print(f"  executeCommand replies: n={len(results)} "
              f"({'all ok' if all(r['ok'] for r in results) else 'some failed'}) "
              f"p50={pct([r['ms'] for r in results], .5):.1f} ms "
              f"(这些是**异步受理**，不代表检查完成)")
        if errors:
            print(f"  errors: {errors[:2]}")
        print(f"  TD rounds (log): {len(rounds)}")
        for stamp, seconds, text in rounds:
            print(f"    +{(stamp - datetime.datetime.now()).total_seconds() * 1000:8.1f} ms "
                  f"(TD 内部 {seconds:.2f}s) {text[:60]}")
        print(f"  '已排队一次重跑' lines: {_count_log_lines(log_path, '已排队一次重跑')}")
        print(f"  TD processes (our tree): distinct={len(distinct)} {distinct} "
              f"max_concurrent={max_concurrent}")
        print(f"  total wall until replies={((replies_done - started) * 1000):.1f} ms")
        print(f"  residue right after settle: {len(td_tree_pids(server_pid))}")
    finally:
        client.close()
    # 关闭后再确认一次：Job Object 应该把进程树带走
    time.sleep(1.0)
    print(f"  residue 1 s after client close: {len(td_pids())} (machine-wide)")
    print()


def scenario_td_disabled(exe: str, project: str, log_dir: str, saves: int) -> None:
    print("== ④c 对照：enableTd=false 时连发保存（不应起任何 TD 进程）==")
    log_path = os.path.join(log_dir, "td_disabled.log")
    client = spawn_client(exe, project, log_path, {"enableTd": False, "tdCheckOnSave": False})
    try:
        recorder = PublishRecorder(client)
        path = os.path.join(project, "src", "top.v")
        uri = path_to_uri(path)
        text = client.open_document(path, "verilog")
        recorder.wait_for(uri, time.perf_counter(), timeout=60)
        latencies = []
        blocking = []
        for _ in range(saves):
            recorder.clear()
            started = time.perf_counter()
            client.notify("textDocument/didSave", {"textDocument": {"uri": uri}, "text": text})
            event = recorder.wait_for(uri, started, timeout=30)
            if event:
                latencies.append((event[0] - started) * 1000.0)
            t_after = time.perf_counter()
            client.request("textDocument/documentSymbol",
                           {"textDocument": {"uri": uri}}, timeout=30)
            blocking.append((time.perf_counter() - t_after) * 1000.0)
        print(row("didSave -> publishDiagnostics", latencies))
        print(row("follow-up request RTT after didSave", blocking))
        print(f"  TD processes (our tree): {len(td_tree_pids(client.proc.pid))}")
    finally:
        client.close()
    print()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", default=os.path.join(REPO, "build", "bin", "fpgalsp-native.exe"))
    parser.add_argument("--root", default=os.path.join(os.environ.get("TEMP", "."),
                                                       "fpgalsp-perf-td"))
    parser.add_argument("--log-dir", default=None)
    parser.add_argument("--cold-runs", type=int, default=5)
    parser.add_argument("--iters", type=int, default=20)
    parser.add_argument("--td-saves", type=int, default=5)
    parser.add_argument("--td-interval-ms", type=int, default=50)
    parser.add_argument("--skip-cold", action="store_true")
    parser.add_argument("--skip-td", action="store_true")
    args = parser.parse_args()

    root = os.path.abspath(args.root)
    log_dir = os.path.abspath(args.log_dir or os.path.join(root, "logs"))
    os.makedirs(log_dir, exist_ok=True)
    missing = [name for name in ("p13", "p100", "ptd")
               if not os.path.isdir(os.path.join(root, name))]
    if missing:
        raise SystemExit(f"合成工程缺失 {missing}；先跑 python tools/perf/td/make_project.py")

    print(f"exe={args.exe}")
    print(f"root={root}")
    print(f"log_dir={log_dir}")
    print(f"system_cpu_busy≈{background_usage():.1%} (采样 0.4 s)\n")

    if not args.skip_cold:
        scenario_cold_start(args.exe, os.path.join(root, "p13"), log_dir, args.cold_runs)
    scenario_did_change(args.exe, os.path.join(root, "p13"), log_dir, args.iters)
    scenario_code_action(args.exe,
                         [(os.path.join(root, "p13"), "small.v"),
                          (os.path.join(root, "p100"), "big.v")],
                         log_dir, min(args.iters, 10))
    scenario_concurrency(args.exe, os.path.join(root, "p100"), log_dir, min(args.iters, 20))
    if not args.skip_td:
        scenario_td_burst(args.exe, os.path.join(root, "ptd"), log_dir,
                          args.td_saves, args.td_interval_ms)
        scenario_td_checks(args.exe, os.path.join(root, "ptd"), log_dir, 5)
        scenario_td_disabled(args.exe, os.path.join(root, "ptd"), log_dir, args.td_saves)

    print(f"remaining TD processes at exit: {len(td_pids())}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())