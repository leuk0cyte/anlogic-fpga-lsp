#!/usr/bin/env python3
"""bench_transport.py —— MCP 传输层对比：stdio vs loopback TCP vs attach 代理。

问题（README §2 / task-31 §2c）：
  * 第二阶段说"MCP over loopback TCP 与 LSP 共用一个引擎"——共享的部分到底是什么？
  * 每次请求会不会重建连接 / 重复握手？loopback 比 stdio 多付多少 µs？

三条路径（都驱动**生产 exe** `build/bin/fpgalsp-native.exe`，不覆盖）：
  stdio     : `--mcp --root <proj>`，stdin/stdout 直接说 newline JSON
  tcp       : `--stdio --mcp-port <P>` 让 LSP 进程开 loopback 监听，客户端直接连 socket
  proxy     : `--mcp --attach-port <P>` 薄代理，客户端对代理说 stdio（= VS Code 走的路径）

测量的是一次 `tools/call` 的端到端往返（自造 request → 读到响应行），
并把「连接建立 + initialize + 首次调用」与「稳态第 2..N 次」分开，避免把
per-connection 的 Bridge 构造（要重新解析 .al）混进稳态数字。

用法：
    python tools/perf/mcp/bench_transport.py --iters 30 --tool fpga_project_info
    python tools/perf/mcp/bench_transport.py --out tools/perf/mcp/out/transport.json
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import statistics
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
DEFAULT_EXE = os.path.join(REPO, "build", "bin", "fpgalsp-native.exe")
DEFAULT_ROOT = os.path.join(os.environ.get("TEMP", r"C:\Windows\Temp"), "fpgalsp-perf", "mcp", "proj200")


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


class LineClient:
    """一个 newline-JSON-RPC 客户端；read/write 走注入的两个可调用对象。"""

    def __init__(self, write, readline, name="client"):
        self._write = write
        self._readline = readline
        self._next_id = 0
        self.name = name

    def request(self, method, params=None, timeout=120.0):
        self._next_id += 1
        message = {"jsonrpc": "2.0", "id": self._next_id, "method": method}
        if params is not None:
            message["params"] = params
        self._write(json.dumps(message, ensure_ascii=False) + "\n")
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = self._readline()
            if not line:
                raise RuntimeError("%s: EOF" % self.name)
            line = line.strip()
            if not line:
                continue
            try:
                payload = json.loads(line)
            except ValueError:
                continue
            if payload.get("id") == self._next_id:
                if "error" in payload:
                    raise RuntimeError("%s: %s" % (self.name, payload["error"]))
                return payload.get("result")
        raise RuntimeError("%s: timeout" % self.name)

    def notify(self, method, params=None):
        message = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            message["params"] = params
        self._write(json.dumps(message, ensure_ascii=False) + "\n")


def handshake(client):
    client.request("initialize", {"protocolVersion": "2024-11-05", "capabilities": {},
                                  "clientInfo": {"name": "perf-transport", "version": "1"}})
    client.notify("notifications/initialized", {})


# --------------------------------------------------------------------------- stdio
def run_stdio(exe, root, tool, arguments, iters):
    process = subprocess.Popen([exe, "--mcp", "--root", root], stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                               creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    client = LineClient(lambda text: (process.stdin.write(text.encode("utf-8")),
                                      process.stdin.flush()),
                        lambda: process.stdout.readline().decode("utf-8", "replace"), "stdio")
    try:
        started = time.perf_counter()
        handshake(client)
        first = client.request("tools/call", {"name": tool, "arguments": arguments})
        cold_ms = (time.perf_counter() - started) * 1000.0
        return client, process, cold_ms, len(json.dumps(first).encode("utf-8"))
    except Exception:
        process.kill()
        raise


# --------------------------------------------------------------------------- tcp
def start_tcp_host(exe, root, port):
    process = subprocess.Popen([exe, "--stdio", "--mcp-port", str(port), "--root", root],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL,
                               creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    # 等端口可用（LSP 进程开了监听就会 accept）
    deadline = time.time() + 20.0
    while time.time() < deadline:
        try:
            probe = socket.create_connection(("127.0.0.1", port), timeout=0.5)
            probe.close()
            return process
        except OSError:
            if process.poll() is not None:
                raise RuntimeError("tcp host exited: %s" % process.returncode)
            time.sleep(0.1)
    process.kill()
    raise RuntimeError("tcp host did not listen on %d" % port)


def connect_tcp(port, root, tool, arguments):
    sock = socket.create_connection(("127.0.0.1", port), timeout=10)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    # 读必须用**带缓冲**的 file：buffering=0 的 readline 会每字节一次 recv，
    # 在 700 B 响应上凭空多出 ~6 ms —— 那是测量错误，不是传输开销。
    reader = sock.makefile("rb")

    def write(text):
        sock.sendall(text.encode("utf-8"))

    def readline():
        return reader.readline().decode("utf-8", "replace")

    client = LineClient(write, readline, "tcp")
    started = time.perf_counter()
    handshake(client)
    first = client.request("tools/call", {"name": tool, "arguments": arguments})
    cold_ms = (time.perf_counter() - started) * 1000.0
    return client, sock, cold_ms, len(json.dumps(first).encode("utf-8"))


def start_proxy(exe, port):
    process = subprocess.Popen([exe, "--mcp", "--attach-port", str(port)], stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                               creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    return process


def run_proxy(process, tool, arguments):
    client = LineClient(lambda text: (process.stdin.write(text.encode("utf-8")),
                                      process.stdin.flush()),
                        lambda: process.stdout.readline().decode("utf-8", "replace"), "proxy")
    started = time.perf_counter()
    handshake(client)
    first = client.request("tools/call", {"name": tool, "arguments": arguments})
    cold_ms = (time.perf_counter() - started) * 1000.0
    return client, cold_ms, len(json.dumps(first).encode("utf-8"))


def measure_steady(client, tool, arguments, iters):
    samples = []
    for _ in range(iters):
        started = time.perf_counter()
        client.request("tools/call", {"name": tool, "arguments": arguments})
        samples.append((time.perf_counter() - started) * 1000.0)
    samples.sort()
    return {
        "p50_ms": round(statistics.median(samples), 4),
        "p95_ms": round(samples[min(len(samples) - 1, int(round(0.95 * (len(samples) - 1))))], 4),
        "min_ms": round(samples[0], 4),
        "max_ms": round(samples[-1], 4),
        "n": len(samples),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="MCP 传输层对比（stdio / loopback TCP / proxy）")
    parser.add_argument("--exe", default=DEFAULT_EXE)
    parser.add_argument("--root", default=DEFAULT_ROOT)
    parser.add_argument("--tool", default="fpga_project_info")
    parser.add_argument("--iters", type=int, default=30)
    parser.add_argument("--out", default=os.path.join(REPO, "tools", "perf", "mcp", "out", "transport.json"))
    args = parser.parse_args()

    if not os.path.isdir(args.root):
        print("missing synthetic project: %s" % args.root)
        return 2
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    arguments = {"text": "module m(input a);endmodule\n"} if args.tool == "fpga_format" else {}
    report = {"tool": args.tool, "iters": args.iters, "root": args.root, "paths": {}}

    # ---- stdio ----
    client, process, cold_ms, bytes_out = run_stdio(args.exe, args.root, args.tool, arguments, args.iters)
    try:
        steady = measure_steady(client, args.tool, arguments, args.iters)
    finally:
        try:
            client.request("shutdown", {})
        except Exception:
            pass
        process.stdin.close()
        process.wait(timeout=10)
    report["paths"]["stdio"] = {"cold_ms": round(cold_ms, 3), "bytes": bytes_out, **steady}
    print("stdio  cold=%.3f ms  p50=%.4f ms  p95=%.4f ms" % (cold_ms, steady["p50_ms"], steady["p95_ms"]))

    # ---- loopback TCP（直连 LSP 进程内的 MCP 端口）----
    port = free_port()
    host = start_tcp_host(args.exe, args.root, port)
    try:
        client, sock, cold_ms, bytes_out = connect_tcp(port, args.root, args.tool, arguments)
        steady = measure_steady(client, args.tool, arguments, args.iters)
        try:
            client.request("shutdown", {})
        except Exception:
            pass
        sock.close()
        report["paths"]["tcp"] = {"port": port, "cold_ms": round(cold_ms, 3), "bytes": bytes_out,
                                 **steady}
        print("tcp    cold=%.3f ms  p50=%.4f ms  p95=%.4f ms" % (cold_ms, steady["p50_ms"], steady["p95_ms"]))
    finally:
        host.kill()
        host.wait(timeout=10)

    # ---- attach 代理（客户端仍是 stdio，服务端走 socket）----
    port = free_port()
    host = start_tcp_host(args.exe, args.root, port)
    proxy = start_proxy(args.exe, port)
    try:
        client, cold_ms, bytes_out = run_proxy(proxy, args.tool, arguments)
        steady = measure_steady(client, args.tool, arguments, args.iters)
        report["paths"]["proxy"] = {"port": port, "cold_ms": round(cold_ms, 3), "bytes": bytes_out,
                                   **steady}
        print("proxy  cold=%.3f ms  p50=%.4f ms  p95=%.4f ms" % (cold_ms, steady["p50_ms"], steady["p95_ms"]))
    finally:
        for process in (proxy, host):
            try:
                process.kill()
                process.wait(timeout=10)
            except Exception:
                pass

    with open(args.out, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(report, handle, ensure_ascii=False, indent=2)
        handle.write("\n")
    print("\nwrote %s" % args.out)
    if all(path in report["paths"] for path in ("stdio", "tcp", "proxy")):
        delta = report["paths"]["tcp"]["p50_ms"] - report["paths"]["stdio"]["p50_ms"]
        print("loopback delta vs stdio: %+.4f ms" % delta)
    return 0


if __name__ == "__main__":
    sys.exit(main())