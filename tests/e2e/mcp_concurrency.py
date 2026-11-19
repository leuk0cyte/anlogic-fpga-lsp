#!/usr/bin/env python3
"""P-A1 端到端：MCP loopback 必须能同时服务多个客户端。

要证明的两件事（都是旧实现的真实故障）：

1. **一个客户端"卡住"不能拖死别的客户端。**
   旧实现是 `accept -> Serve（阻塞到该客户端断开）-> 再 accept`：第二个客户端
   能连上 TCP，但永远收不到任何响应。现在每连接一个会话线程，B 必须能正常应答
   即使 A 正卡在"发了半行、没有换行符"的状态。

2. **超过 max_clients 的连接要明确被拒绝**（`-32002`），而不是静默挂起。
   用 `FPGALSP_MCP_CLIENTS=1` 复现。

单独运行：
    python tests/e2e/mcp_concurrency.py --server build/bin/fpgalsp-native.exe
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

CHECKS: list[str] = []


def check(condition: bool, message: str) -> None:
    if not condition:
        print("FAIL:", message)
        raise SystemExit(1)
    CHECKS.append(message)


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def wait_port(port: int, timeout: float = 10.0) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.3):
                return
        except OSError:
            time.sleep(0.05)
    raise SystemExit("FAIL: MCP 端口 %d 在 %.1fs 内没有就绪" % (port, timeout))


class Client:
    def __init__(self, port: int, timeout: float = 5.0) -> None:
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.buffer = b""

    def send_raw(self, payload: str) -> None:
        self.sock.sendall(payload.encode("utf-8"))

    def send(self, message: dict) -> None:
        self.send_raw(json.dumps(message, separators=(",", ":")) + "\n")

    def read_line(self, timeout: float = 5.0) -> dict:
        self.sock.settimeout(timeout)
        while b"\n" not in self.buffer:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise SystemExit("FAIL: 连接被对端关闭（还没读到完整行）")
            self.buffer += chunk
        line, self.buffer = self.buffer.split(b"\n", 1)
        return json.loads(line.decode("utf-8"))

    def request(self, ident: int, method: str, params: dict, timeout: float = 5.0) -> dict:
        self.send({"jsonrpc": "2.0", "id": ident, "method": method, "params": params})
        return self.read_line(timeout)

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


def start_server(exe: str, root: str, port: int, env_extra: dict | None = None):
    env = dict(os.environ)
    if env_extra:
        env.update(env_extra)
    log = Path(tempfile.mkdtemp(prefix="fpgalsp-mcp-conc-")) / "server.log"
    proc = subprocess.Popen(
        [exe, "--stdio", "--root", root, "--mcp-port", str(port), "--log", str(log)],
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        env=env,
        cwd=root,
    )
    wait_port(port)
    return proc, log


def stop_server(proc: subprocess.Popen) -> None:
    proc.terminate()
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()


def initialize(client: Client, ident: int) -> None:
    response = client.request(
        ident,
        "initialize",
        {
            "protocolVersion": "2025-06-18",
            "capabilities": {},
            "clientInfo": {"name": "concurrency-test", "version": "0"},
        },
    )
    check("result" in response, "initialize 有 result（客户端 #%d）" % ident)


def scenario_parallel(exe: str, root: str) -> None:
    port = free_port()
    proc, _ = start_server(exe, root, port)
    try:
        holder = Client(port)
        initialize(holder, 1)
        # 关键：发一条**没有换行符**的半行，让这个连接停在"读到半个帧"的状态。
        # 旧实现里，服务端会一直阻塞在 Serve() 的 recv 上，accept 永远不会再调用。
        holder.send_raw('{"jsonrpc":"2.0","id":99,"method":"tools/list","para')
        time.sleep(0.2)

        other = Client(port, timeout=3.0)
        initialize(other, 2)
        started = time.time()
        pong = other.request(2, "ping", {}, timeout=3.0)
        elapsed = time.time() - started
        check(pong.get("result") == {}, "第二个客户端能拿到 ping 响应")
        check(elapsed < 2.0, "第二个客户端不被卡住的连接阻塞（%.3fs < 2s）" % elapsed)

        # 补完半行，A 也应当继续被服务（同一个线程把缓冲区读完）
        holder.send_raw('ms":{}}\n')
        drained = holder.read_line(timeout=5.0)
        check("result" in drained, "半行补完后原连接仍能被服务")

        other.close()
        holder.close()
    finally:
        stop_server(proc)


def scenario_limit(exe: str, root: str) -> None:
    port = free_port()
    proc, _ = start_server(exe, root, port, {"FPGALSP_MCP_CLIENTS": "1"})
    try:
        first = Client(port)
        initialize(first, 1)
        second = Client(port, timeout=3.0)
        second.send(
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {"protocolVersion": "2025-06-18", "capabilities": {}},
            }
        )
        try:
            response = second.read_line(timeout=3.0)
        except SystemExit:
            # 先发错误再关连接是一种合法实现；只要不是"静默挂起"即可
            check(True, "超限连接被明确关闭（未静默挂起）")
        else:
            check("error" in response, "超限连接收到 JSON-RPC error")
            check(
                response["error"].get("code") == -32002,
                "超限错误码是 -32002（实际 %r）" % response["error"].get("code"),
            )
        second.close()
        first.close()
    finally:
        stop_server(proc)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True, help="fpgalsp-native.exe 路径")
    parser.add_argument(
        "--root",
        default=str(Path(__file__).resolve().parents[1] / "gate-corpus" / "corpus_alpha"),
        help="工程根（默认用只读门禁语料）",
    )
    args = parser.parse_args()
    root = str(Path(args.root).resolve())
    exe = str(Path(args.server).resolve())

    scenario_parallel(exe, root)
    scenario_limit(exe, root)

    print("PASS: %d checks" % len(CHECKS))
    for item in CHECKS:
        print("  -", item)
    return 0


if __name__ == "__main__":
    sys.exit(main())