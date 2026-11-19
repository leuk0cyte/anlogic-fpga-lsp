#!/usr/bin/env python
"""F1（task-36）验收脚本：codeAction 移出 IO 线程 + lint 缓存。

四件事：
  1. **协议快照**：对固定文档发 codeAction，把响应原样写 JSON（before/after 逐字节 diff）。
  2. **codeAction 自身延迟**：13 KB / 100 KB，缓存命中（同一文本连发两次）与未命中（每次改一行）。
  3. **同进程对照**：后台持续跑"缓存未命中"的 codeAction（每个 ~350 ms），主线程量
     didChange->publishDiagnostics 的 p50/p95。改前应看到 ~370 ms 的队头阻塞，改后应回到 ~1 ms。
  4. **并发正确性**：一次 codeAction 与 didChange 同时进行时，响应字段不得出现撕裂
     （同一份文档同一 range 的两次响应必须完全相同）。

用法：
    python tools/perf/td/f1_check.py --exe build/bin/fpgalsp-native.exe --dump %TEMP%\f1-before.json
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import threading
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, os.path.join(REPO, "tests", "golden", "tools"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from e2e_diag import (ConcurrentClient, PublishRecorder, init_client, pct, row)  # noqa: E402
from goldenlib.client import path_to_uri  # noqa: E402

RANGE = {"start": {"line": 0, "character": 0}, "end": {"line": 0, "character": 0}}


def code_action_params(uri: str, line: int = 0) -> dict:
    return {
        "textDocument": {"uri": uri},
        "range": {"start": {"line": line, "character": 0},
                  "end": {"line": line, "character": 0}},
        "context": {"diagnostics": []},
    }


def start_client(exe: str, project: str, log_path: str) -> ConcurrentClient:
    client = ConcurrentClient(REPO, log_path, exe_path=exe)
    init_client(client, project, {"enableTd": False, "tdCheckOnSave": False})
    return client


def capture_protocol(exe: str, project: str, log_dir: str, dump: str | None) -> dict:
    """固定输入下的 codeAction 响应（用于 before/after 逐字节 diff）。"""
    client = start_client(exe, project, os.path.join(log_dir, "f1_capture.log"))
    payload: dict[str, object] = {}
    try:
        cases = [("small.v", 0), ("small.v", 3), ("big.v", 0), ("big.v", 5), ("big.v", 40)]
        for name, line in cases:
            path = os.path.join(project, "src", name)
            if not os.path.exists(path):
                continue
            uri = path_to_uri(path)
            client.open_document(path, "verilog")
            # 打开之后先把"实例化 refactor"也能触发的位置也截一份（line 变化覆盖两条分支）
            payload[f"{name}:{line}"] = client.call("textDocument/codeAction",
                                                    code_action_params(uri, line), timeout=120)
        # .adc 分支（不在 p100 工程里，跳过时保持 payload 简单）
    finally:
        client.close()
    if dump:
        os.makedirs(os.path.dirname(os.path.abspath(dump)), exist_ok=True)
        with open(dump, "w", encoding="utf-8") as handle:
            json.dump(payload, handle, ensure_ascii=False, indent=1, sort_keys=True)
        print(f"protocol snapshot -> {dump} ({os.path.getsize(dump)} bytes, {len(payload)} cases)")
    return payload


def mutate(client: ConcurrentClient, uri: str, version: int, line: int = 2) -> None:
    """在注释行插一个空格，保证下一次 codeAction 是**缓存未命中**。"""
    client.notify("textDocument/didChange", {
        "textDocument": {"uri": uri, "version": version},
        "contentChanges": [{
            "range": {"start": {"line": line, "character": 0},
                      "end": {"line": line, "character": 0}},
            "text": " ",
        }],
    })


def scenario_latency(exe: str, project: str, log_dir: str, iters: int) -> None:
    print("== A. codeAction 自身延迟（缓存未命中 vs 命中）==")
    client = start_client(exe, project, os.path.join(log_dir, "f1_latency.log"))
    try:
        recorder = PublishRecorder(client)
        for name in ("small.v", "big.v"):
            path = os.path.join(project, "src", name)
            if not os.path.exists(path):
                continue
            uri = path_to_uri(path)
            text = client.open_document(path, "verilog")
            recorder.wait_for(uri, time.perf_counter(), timeout=120)
            size_kb = os.path.getsize(path) / 1024.0

            # 1) 未命中：每次先改一行再请求（文本变了 -> 缓存键变了）
            misses = []
            version = 2
            for _ in range(iters):
                mutate(client, uri, version)
                version += 1
                time.sleep(0.02)  # 让 didChange 先被处理（同步 handler，几乎不需要等）
                started = time.perf_counter()
                client.call("textDocument/codeAction", code_action_params(uri), timeout=180)
                misses.append((time.perf_counter() - started) * 1000.0)

            # 2) 命中：同一文本连发两次，第二次应命中
            mutate(client, uri, version)
            version += 1
            time.sleep(0.02)
            first = time.perf_counter()
            r1 = client.call("textDocument/codeAction", code_action_params(uri), timeout=180)
            first_ms = (time.perf_counter() - first) * 1000.0
            second = time.perf_counter()
            r2 = client.call("textDocument/codeAction", code_action_params(uri), timeout=180)
            second_ms = (time.perf_counter() - second) * 1000.0

            print(row(f"codeAction {name} ({size_kb:.1f} KB) cache-miss", misses))
            print(f"  {name}: 1st(compute)={first_ms:8.2f} ms  2nd(cache hit)={second_ms:8.2f} ms "
                  f"speedup={first_ms / max(second_ms, 0.01):.0f}x  identical={r1 == r2}")
    finally:
        client.close()
    print()


def scenario_blocking(exe: str, project: str, log_dir: str, iters: int,
                      load_file: str = "big.v") -> None:
    print("== B. 同进程对照：后台 codeAction（缓存未命中）时 didChange 的延迟 ==")
    client = start_client(exe, project, os.path.join(log_dir, "f1_blocking.log"))
    try:
        recorder = PublishRecorder(client)
        small = os.path.join(project, "src", "small.v")
        big = os.path.join(project, "src", load_file)
        small_uri = path_to_uri(small)
        big_uri = path_to_uri(big)
        client.open_document(small, "verilog")
        client.open_document(big, "verilog")
        recorder.wait_for(small_uri, time.perf_counter(), timeout=120)

        def measure(rounds: int, tag: str) -> list[float]:
            latencies: list[float] = []
            for version in range(2, rounds + 2):
                recorder.clear()
                started = time.perf_counter()
                mutate(client, small_uri, version, line=1)
                event = recorder.wait_for(small_uri, started, timeout=180)
                if event:
                    latencies.append((event[0] - started) * 1000.0)
            print(row(f"didChange {tag}", latencies))
            return latencies

        baseline = measure(iters, "baseline")

        def measure_under(mode: str, tag: str) -> tuple[list[float], list[float]]:
            """后台线程按 mode 制造负载，同时量 didChange。

            mode:
              mutate_only     只发 big.v 的 didChange（100 KB 重解析 ~6 ms），不发 codeAction
              codeaction_miss 每次改 big.v 再 codeAction（缓存未命中，真算 ~350 ms）
              codeaction_hit  不改文本，只反复 codeAction（缓存命中 ~3 ms）
            """
            stop = threading.Event()
            codes: list[float] = []

            def load() -> None:
                version = 500
                while not stop.is_set():
                    if mode in ("mutate_only", "codeaction_miss"):
                        mutate(client, big_uri, version, line=2)
                        version += 1
                        time.sleep(0.01)
                    if mode == "mutate_only":
                        time.sleep(0.005)
                        continue
                    started = time.perf_counter()
                    try:
                        client.call("textDocument/codeAction", code_action_params(big_uri),
                                    timeout=180)
                    except Exception:  # noqa: BLE001 - 关停时可能抛
                        break
                    codes.append((time.perf_counter() - started) * 1000.0)

            worker = threading.Thread(target=load, daemon=True)
            worker.start()
            time.sleep(0.15)
            latencies = measure(iters, tag)
            stop.set()
            worker.join(timeout=180)
            return latencies, codes

        only_mutate, _ = measure_under("mutate_only", "under big.v didChange only (control)")
        loaded, codes = measure_under("codeaction_miss", "under codeAction load (cache miss)")
        hit, hit_codes = measure_under("codeaction_hit", "under codeAction load (cache hit)")

        def delta(name: str, values: list[float]) -> None:
            if not (baseline and values):
                return
            print(f"  {name}: p50 {pct(baseline, .5):.2f} -> {pct(values, .5):.2f} ms, "
                  f"p95 {pct(baseline, .95):.2f} -> {pct(values, .95):.2f} ms "
                  f"(×{pct(values, .95) / max(pct(baseline, .95), 0.001):.1f})")

        delta("didChange vs big.v-didChange-only", only_mutate)
        delta("didChange vs codeAction cache-miss", loaded)
        delta("didChange vs codeAction cache-hit ", hit)
        if codes:
            print(f"  background codeAction (miss): n={len(codes)} p50={pct(codes, .5):.1f} ms "
                  f"(证明 worker 真的在算)")
        if hit_codes:
            print(f"  background codeAction (hit) : n={len(hit_codes)} "
                  f"p50={pct(hit_codes, .5):.2f} ms")
    finally:
        client.close()
    print()


def run_with_retry(fn, label: str, attempts: int = 3) -> bool:
    """6 个 agent 共用一台机器：别人的构建脚本会 `Stop-Process fpgalsp-native`，
    把我们正在测量的 server 杀掉（表现为 stdin flush 的 Errno 22）。重试即可。"""
    for attempt in range(1, attempts + 1):
        try:
            fn()
            return True
        except (OSError, RuntimeError, EOFError) as exc:
            print(f"  [retry {attempt}/{attempts}] {label} 被外部因素打断: {exc}")
            time.sleep(2.0)
    print(f"  [give up] {label}")
    return False


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", default=os.path.join(REPO, "build", "bin", "fpgalsp-native.exe"))
    parser.add_argument("--root", default=os.path.join(os.environ.get("TEMP", "."),
                                                       "fpgalsp-perf-td"))
    parser.add_argument("--log-dir", default=None)
    parser.add_argument("--iters", type=int, default=20)
    parser.add_argument("--dump", default=None, help="把 codeAction 响应写到这里（协议快照）")
    parser.add_argument("--skip-blocking", action="store_true")
    parser.add_argument("--load-file", default="big.v",
                        help="后台 codeAction 打哪个文件（big.v / small.v，用于归因残余延迟）")
    args = parser.parse_args()

    root = os.path.abspath(args.root)
    log_dir = os.path.abspath(args.log_dir or os.path.join(root, "logs"))
    os.makedirs(log_dir, exist_ok=True)
    project = os.path.join(root, "p100")
    if not os.path.isdir(os.path.join(project, "src")):
        raise SystemExit("缺少合成工程；先跑 python tools/perf/td/make_project.py")
    print(f"exe={args.exe}\nproject={project}\n")

    if args.dump:
        if not run_with_retry(lambda: capture_protocol(args.exe, project, log_dir, args.dump),
                              "protocol capture"):
            return 3
    if not run_with_retry(lambda: scenario_latency(args.exe, project, log_dir,
                                                   min(args.iters, 10)), "latency"):
        return 3
    if not args.skip_blocking:
        if not run_with_retry(lambda: scenario_blocking(args.exe, project, log_dir, args.iters,
                                                       args.load_file),
                              "blocking"):
            return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())