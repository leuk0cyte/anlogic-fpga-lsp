#!/usr/bin/env python3
"""P-A3 端到端：MCP loopback 端点广告 + 令牌鉴权。

验证三件事：

1. 服务端在 `FPGALSP_STATE_DIR` 下写出 `mcp-endpoint-<port>.json`，内容是真实端口、
   32 位十六进制 token、自己的 pid —— 扩展不再靠 FNV 派生端口 + 探测。
2. `FPGALSP_MCP_REQUIRE_TOKEN=1` 时，不带 token 的 initialize 被拒绝（-32001 或直接断开），
   带 token 的能正常握手。这是灰度期的"强制档"。
3. token 不出现在服务端日志里。

单独运行：
    python tests/e2e/mcp_auth.py --server build/bin/fpgalsp-native.exe
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


def read_endpoint(state_dir: Path, port: int, timeout: float = 10.0) -> dict:
    path = state_dir / ("mcp-endpoint-%d.json" % port)
    deadline = time.time() + timeout
    while time.time() < deadline:
        if path.exists():
            try:
                return json.loads(path.read_text(encoding="utf-8"))
            except ValueError:
                pass  # 半个文件：下一轮再读
        time.sleep(0.05)
    raise SystemExit("FAIL: 端点文件 %s 没有出现" % path)


def connect(port: int) -> socket.socket:
    sock = socket.create_connection(("127.0.0.1", port), timeout=5.0)
    sock.settimeout(5.0)
    return sock


def send(sock: socket.socket, message: dict) -> None:
    sock.sendall(json.dumps(message, separators=(",", ":")).encode("utf-8") + b"\n")


def read_line(sock: socket.socket) -> dict | None:
    buffer = b""
    try:
        while b"\n" not in buffer:
            chunk = sock.recv(65536)
            if not chunk:
                return None  # 对端直接关闭
            buffer += chunk
    except OSError:
        return None
    line = buffer.split(b"\n", 1)[0]
    return json.loads(line.decode("utf-8"))


def initialize_message(token: str | None) -> dict:
    params = {"protocolVersion": "2025-06-18", "capabilities": {}}
    if token is not None:
        params["_meta"] = {"fpgalspToken": token}
    return {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": params}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument(
        "--root",
        default=str(Path(__file__).resolve().parents[1] / "gate-corpus" / "corpus_alpha"),
    )
    args = parser.parse_args()
    root = str(Path(args.root).resolve())
    exe = str(Path(args.server).resolve())
    state_dir = Path(tempfile.mkdtemp(prefix="fpgalsp-mcp-auth-"))
    port = free_port()
    log = state_dir / "server.log"

    env = dict(os.environ)
    env["FPGALSP_STATE_DIR"] = str(state_dir)
    env["FPGALSP_MCP_REQUIRE_TOKEN"] = "1"
    proc = subprocess.Popen(
        [exe, "--stdio", "--root", root, "--mcp-port", str(port), "--log", str(log)],
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        env=env,
        cwd=root,
    )
    try:
        wait_port(port)
        endpoint = read_endpoint(state_dir, port)
        check(endpoint.get("port") == port, "端点文件里的端口是真实端口")
        token = endpoint.get("token") or ""
        check(len(token) == 32 and all(c in "0123456789abcdef" for c in token),
              "token 是 32 位十六进制（实际 %r）" % token)
        check(endpoint.get("pid") == proc.pid, "端点文件的 pid 是服务端进程")

        # 1) 不带 token：强制档下必须被拒绝
        anonymous = connect(port)
        send(anonymous, initialize_message(None))
        response = read_line(anonymous)
        if response is None:
            check(True, "未带 token 的连接被明确关闭（未静默挂起）")
        else:
            check("error" in response, "未带 token 收到 JSON-RPC error")
            check(response["error"].get("code") == -32001,
                  "错误码是 -32001（实际 %r）" % response["error"].get("code"))
        anonymous.close()

        # 2) 带正确 token：正常握手
        authorized = connect(port)
        send(authorized, initialize_message(token))
        ok = read_line(authorized)
        check(ok is not None and "result" in ok, "带正确 token 的 initialize 成功")
        check(ok["result"].get("protocolVersion") == "2025-06-18", "版本协商正常")
        authorized.close()

        # 3) 错误 token：拒绝
        wrong = connect(port)
        send(wrong, initialize_message("0" * 32))
        bad = read_line(wrong)
        if bad is None:
            check(True, "错误 token 的连接被关闭")
        else:
            check(bad.get("error", {}).get("code") == -32001, "错误 token 返回 -32001")
        wrong.close()

        # 3b) 旧的 attach 代理路径：客户端**不带 token**，由代理从端点文件补上
        proxy = subprocess.Popen(
            [exe, "--mcp", "--attach-port", str(port)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            env=env,
            cwd=root,
            text=True,
            encoding="utf-8",
            bufsize=1,
        )
        try:
            proxy.stdin.write(json.dumps(initialize_message(None), separators=(",", ":")) + "\n")
            proxy.stdin.flush()
            line = proxy.stdout.readline()
            check(bool(line), "attach 代理返回了响应")
            proxied = json.loads(line)
            check("result" in proxied,
                  "attach 代理把 token 注入后 initialize 成功（%r）" % (proxied.get("error"),))
        finally:
            proxy.terminate()
            try:
                proxy.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proxy.kill()

        # 4) token 不得出现在日志里
        time.sleep(0.2)
        text = log.read_text(encoding="utf-8", errors="replace") if log.exists() else ""
        check(token not in text, "token 不出现在服务端日志里")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()

    print("PASS: %d checks" % len(CHECKS))
    for item in CHECKS:
        print("  -", item)
    return 0


if __name__ == "__main__":
    sys.exit(main())