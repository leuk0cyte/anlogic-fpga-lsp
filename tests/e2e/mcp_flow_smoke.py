"""mcp_flow_smoke.py —— flow 类工具的端到端验证（**只在临时副本上跑**）

为什么单独一个脚本
------------------
`fpga_flow_run` 会真的驱动 TD 跑综合，并且在工程目录下写 `*_Runs/` 与 `.fpgalsp/`。
`tests/gate-corpus/corpus_alpha` 是**只读语料**（多个 agent 共用），一旦被写脏，
`td.flow` 之类依赖"干净 runs 目录"的用例就会假失败——本轮真实发生过一次
（`.stop.f` 残留导致 area 断言失败，由 Lead 手工清理）。

所以这里：
  1. `tempfile.mkdtemp()` + `shutil.copytree(ignore=('*_Runs', '.fpgalsp'))` 造副本
  2. 所有 flow 调用都指向副本
  3. 结束时**断言原语料没有新增** `*_Runs` / `.fpgalsp`
  4. 无论成败都删掉临时目录

跑什么
------
    initialize → fpga_flow_status（idle）→ fpga_flow_run(stages=["syn"], jobs=2)
    → 断言拿到 runId → fpga_flow_cancel（立刻取消，不等综合跑完）
    → fpga_flow_wait（取消后应很快返回 running=false）
    → shutdown

用法：
    python tests/e2e/mcp_flow_smoke.py --server build/bin/fpgalsp-native.exe
    ctest -R e2e.mcp_flow --output-on-failure
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(os.path.dirname(HERE))
CORPUS = os.path.join(REPO_ROOT, "tests", "gate-corpus", "corpus_alpha")
FALLBACK_SERVER = os.path.join(REPO_ROOT, "build", "bin", "fpgalsp-native.exe")


class Client:
    def __init__(self, exe: str, root: str, allow_flow: bool = True):
        # --log 必须显式给：默认日志会写 <root>/.fpgalsp/logs/mcp.log，
        # 而本用例会断言语料目录保持干净（Python 也有同样的默认行为）。
        args = [exe, "--mcp", "--root", root,
                "--log", os.path.join(tempfile.gettempdir(), "fpgalsp-e2e-mcp-flow.log")]
        if allow_flow:
            args.append("--allow-flow")
        self.process = subprocess.Popen(
            args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            cwd=REPO_ROOT,
        )
        self._id = 0

    def call(self, method: str, params: dict | None = None, timeout: float = 120.0) -> dict:
        self._id += 1
        request = {"jsonrpc": "2.0", "id": self._id, "method": method}
        if params is not None:
            request["params"] = params
        self.process.stdin.write((json.dumps(request) + "\n").encode("utf-8"))
        self.process.stdin.flush()
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = self.process.stdout.readline()
            if not line:
                raise AssertionError("MCP 进程提前退出（等 %s）" % method)
            message = json.loads(line.decode("utf-8", "replace"))
            if message.get("id") == self._id:
                if "error" in message:
                    raise AssertionError("%s 返回协议错误：%s" % (method, message["error"]))
                return message
        raise AssertionError("%s 超时" % method)

    def tool(self, name: str, arguments: dict | None = None, timeout: float = 180.0) -> dict:
        response = self.call("tools/call", {"name": name, "arguments": arguments or {}}, timeout)
        return response["result"]

    def stop(self) -> int:
        try:
            self.call("shutdown", timeout=20)
        except (AssertionError, BrokenPipeError, OSError):
            pass
        try:
            return self.process.wait(timeout=60)
        except subprocess.TimeoutExpired:
            self.process.kill()
            return -1


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def structured(result: dict) -> dict:
    check("structuredContent" in result, "缺少 structuredContent：%s" % list(result))
    return result["structuredContent"]


def corpus_is_clean() -> tuple[bool, str]:
    """原语料必须没有 *_Runs / .fpgalsp（flow 只许写临时副本）。"""
    runs = [name for name in os.listdir(CORPUS) if name.endswith("_Runs")]
    if runs:
        return False, "语料里出现了 runs 目录：%s" % runs
    if os.path.exists(os.path.join(CORPUS, ".fpgalsp")):
        return False, "语料里出现了 .fpgalsp/"
    return True, ""


def main() -> int:
    parser = argparse.ArgumentParser(description="MCP flow 工具端到端冒烟")
    parser.add_argument("--server", default=FALLBACK_SERVER)
    args = parser.parse_args()

    exe = args.server
    if not os.path.isfile(exe):
        print("找不到 %s；请先构建或用 --server 指定。" % exe)
        return 2
    if not os.path.isdir(CORPUS):
        print("找不到语料 %s" % CORPUS)
        return 2

    # 先清掉其它用例（e2e.lsp_smoke / td.*）可能留在共享语料里的派生目录。
    # 这些目录是 TD/服务端运行时产物，不是语料内容；清理后本用例的
    # "结束时不新增" 断言才是稳定的（ctest -j 下顺序不确定，不能依赖别人先清）。
    for generated in (".fpgalsp",):
        path = os.path.join(CORPUS, generated)
        if os.path.isdir(path):
            shutil.rmtree(path, ignore_errors=True)
    for name in os.listdir(CORPUS):
        if name.endswith("_Runs"):
            shutil.rmtree(os.path.join(CORPUS, name), ignore_errors=True)

    clean_before, reason = corpus_is_clean()
    if not clean_before:
        print("FAIL: 开始前语料就不干净：%s" % reason)
        return 1

    workdir = tempfile.mkdtemp(prefix="fpgalsp-mcp-flow-")
    project_copy = os.path.join(workdir, "corpus_alpha")
    checks: list[str] = []
    exit_code = 0
    try:
        shutil.copytree(
            CORPUS, project_copy,
            ignore=shutil.ignore_patterns("*_Runs", ".fpgalsp"),
        )
        print("临时副本: %s" % project_copy)

        client = Client(exe, project_copy)
        try:
            client.call("initialize", {"protocolVersion": "2024-11-05", "capabilities": {}})

            data = structured(client.tool("fpga_flow_status", {}))
            check(data["ok"] is True, "flow_status 失败：%s" % data)
            check(data["flow"]["running"] is False, "初始状态不该在跑")
            checks.append("fpga_flow_status 初始 idle")

            data = structured(client.tool("fpga_flow_run", {"stages": ["syn"], "jobs": 2}))
            check(data.get("ok") is True, "flow_run 失败：%s" % data)
            run_id = data.get("runId", "")
            check(run_id.startswith("flow_"), "runId 形状不对：%r" % run_id)
            checks.append("fpga_flow_run 启动 syn（runId=%s）" % run_id)

            data = structured(client.tool("fpga_flow_cancel", {}))
            check(data.get("ok") is True, "flow_cancel 失败：%s" % data)
            checks.append("fpga_flow_cancel 已取消")

            data = structured(client.tool("fpga_flow_wait", {"timeout": 120, "interval": 1},
                                          timeout=180))
            check(data.get("running") is False, "取消后 flow 应停止：%s" % data)
            check(data.get("timedOut") is False, "取消后不该超时")
            checks.append("fpga_flow_wait 在取消后返回 running=false")

            data = structured(client.tool("fpga_flow_log", {"tail": 20}))
            check(data.get("ok") is True, "取消后应能读到 run 目录：%s" % data)
            check(data.get("count", 0) >= 1, "应该有日志文件")
            checks.append("fpga_flow_log 读到 %d 个文件" % data["count"])

            data = structured(client.tool("fpga_flow_issues", {"tail": 200}))
            check(data.get("ok") is True, "flow_issues 失败：%s" % data)
            check("issues" in data and "counts" in data, "flow_issues 形状不对")
            checks.append("fpga_flow_issues 返回 %d 条" % len(data["issues"]))
        finally:
            exit_code = client.stop()

        clean_after, reason = corpus_is_clean()
        check(clean_after, "原语料被写脏了：%s" % reason)
        checks.append("原语料无新增 *_Runs / .fpgalsp")
        check(exit_code == 0, "进程退出码应为 0，实际 %s" % exit_code)
        checks.append("shutdown 后退出码 0")
    except AssertionError as error:
        print("FAIL: %s" % error)
        return 1
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    for index, item in enumerate(checks, 1):
        print("  %2d. %s" % (index, item))
    print("")
    print("MCP flow smoke 通过：%d 项检查" % len(checks))
    return 0


if __name__ == "__main__":
    sys.exit(main())