"""mcp_smoke.py —— MCP server 端到端冒烟（真进程 + newline-delimited 协议）

跑什么
------
1. `fpgalsp-native.exe --mcp` 启动，`initialize` → 断言协议版本协商、能力声明、
   serverInfo、instructions（agent 会读到这段文字）。
2. `tools/list` → 默认 34 个只读工具；加 `--allow-flow --allow-write` 后 42 个；
   每个工具都要有 name/title/description/inputSchema/annotations。
3. 逐个调用**只读**工具（不碰 TD、不写文件）：
      fpga_project_list / fpga_project_info / fpga_project_files
      fpga_read_file / fpga_search_text
      fpga_constraints_status / fpga_constraint_audit
      fpga_td_info / fpga_flow_status
4. 调用不存在的工具 → `isError: true`（**不是** JSON-RPC error）。
5. 默认模式下调用 flow/write 工具 → 明确提示需要 `--allow-flow` / `--allow-write`。
6. 显式占位的工具返回 `{ok:false, error:"not implemented: ..."}`（不是静默空结果）。
7. `shutdown` → 进程干净退出，且**无残留 td_commands_prompt.exe**。

为什么放在 CTest 里
-------------------
wire 上的形状（isError 与否、structuredContent、工具数量）是单元测试抓不到的；
而且"占位必须显式失败"这条只能从协议层观察。

单独运行：
    python tests/e2e/mcp_smoke.py --server build/bin/fpgalsp-native.exe
    ctest -L e2e --output-on-failure
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(os.path.dirname(HERE))
CORPUS = os.path.join(REPO_ROOT, "tests", "gate-corpus", "corpus_alpha")

FALLBACK_SERVER = os.path.join(REPO_ROOT, "build", "bin", "fpgalsp-native.exe")
EXPECTED_READONLY_TOOLS = 34
EXPECTED_ALL_TOOLS = 42


class McpProcess:
    """最小的 MCP 客户端：一行一条 JSON-RPC。"""

    def __init__(self, exe: str, extra_args: list[str]):
        # 默认日志会写 <cwd>/.fpgalsp/logs/mcp.log；用例显式给临时路径，
        # 避免往仓库/语料目录写文件（Python 的默认行为同样如此）。
        if "--log" not in extra_args:
            extra_args = list(extra_args) + [
                "--log", os.path.join(tempfile.gettempdir(), "fpgalsp-e2e-mcp.log")]
        command = [exe, "--mcp"] + extra_args
        self.process = subprocess.Popen(
            command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=REPO_ROOT,
        )
        self._next_id = 0
        self.log: list[str] = []
        self.last_raw: bytes = b""

    def send(self, method: str, params: dict | None = None, notify: bool = False) -> dict | None:
        message = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            message["params"] = params
        if not notify:
            self._next_id += 1
            message["id"] = self._next_id
        payload = json.dumps(message, ensure_ascii=False) + "\n"
        self.process.stdin.write(payload.encode("utf-8"))
        self.process.stdin.flush()
        if notify:
            return None
        return self.read_response(self._next_id)

    def read_response(self, expected_id: int, timeout: float = 30.0) -> dict:
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = self.process.stdout.readline()
            if not line:
                raise AssertionError("MCP 进程过早关闭（等 id=%d）" % expected_id)
            self.last_raw = line
            text = line.decode("utf-8", "replace").strip()
            if not text:
                continue
            self.log.append(text)
            message = json.loads(text)
            if message.get("id") == expected_id:
                return message
        raise AssertionError("等 id=%d 的响应超时" % expected_id)

    def stop(self) -> int:
        try:
            self.send("shutdown")
        except (BrokenPipeError, OSError, AssertionError):
            pass
        try:
            return self.process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            self.process.kill()
            return -1


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def tool_map(tools: list[dict]) -> dict[str, dict]:
    return {tool.get("name", ""): tool for tool in tools}


def call_tool(client: McpProcess, name: str, arguments: dict | None = None) -> dict:
    response = client.send("tools/call", {"name": name, "arguments": arguments or {}})
    check("error" not in response,
          "tools/call %s 变成了 JSON-RPC error（应为 isError 结果）：%s" % (name, response))
    check("result" in response, "tools/call %s 没有 result：%s" % (name, response))
    return response["result"]


def structured(result: dict) -> dict:
    """取结果里的结构化对象。

    P-B1 起服务端有两种档位，客户端必须都能读：
      * 默认档（未声明 capabilities.experimental.structuredContent）：
        只有 content[0].text，内容是**紧凑 JSON**（不再重复发 structuredContent）；
      * 协商档：structuredContent + 一行摘要 text。
    以前两档都发全量，实测占响应 58–64%（doc/design/protocol-optimization.md §B1）。
    """
    if "structuredContent" in result:
        payload = result["structuredContent"]
        check(isinstance(payload, dict), "structuredContent 必须是对象：%s" % type(payload))
        return payload
    items = result.get("content") or []
    check(bool(items) and items[0].get("type") == "text",
          "结果既无 structuredContent 也无 text：%s" % list(result))
    try:
        payload = json.loads(items[0].get("text", ""))
    except ValueError as exc:
        check(False, "content[0].text 不是 JSON：%s / %r" % (exc, items[0].get("text", "")[:200]))
        return {}
    check(isinstance(payload, dict), "content[0].text 解析后必须是对象：%s" % type(payload))
    return payload


def run_suite(exe: str) -> list[str]:
    """返回已执行的检查项名称（用于报告）。"""
    checks: list[str] = []

    def note(name: str) -> None:
        checks.append(name)

    client = McpProcess(exe, ["--allow-flow", "--allow-write"])
    try:
        # ---- 1. initialize ----
        response = client.send(
            "initialize",
            {
                "protocolVersion": "2025-03-26",
                "capabilities": {},
                "clientInfo": {"name": "mcp-smoke", "version": "1"},
            },
        )
        result = response["result"]
        check(result["protocolVersion"] == "2025-03-26",
              "协议版本未回显：%r" % result.get("protocolVersion"))
        check(result["capabilities"]["tools"]["listChanged"] is False, "capabilities 不对")
        check(result["serverInfo"]["name"] == "fpga-lsp-mcp", "serverInfo.name 不对")
        check("1-based" in result["instructions"], "instructions 没有说明 1-based")
        # 坑 1：MCP 是 newline-delimited 协议。若 stdout 是文本模式，
        # Windows CRT 会把每个消息的 "\n" 变成 "\r\n"；严格客户端会解析失败。
        # 这里直接看原始字节：一条消息必须以单个 "\n" 结束，且不含 "\r"。
        raw = client.last_raw
        check(raw.endswith(b"\n"), "响应不是以 \\n 结束：%r" % raw[-4:])
        check(b"\r" not in raw, "响应里出现了 \\r（stdout 未切换到二进制模式）：%r" % raw[-8:])
        check(raw.count(b"\n") == 1, "一条消息里出现了多个换行：%r" % raw[:120])
        note("stdout 二进制模式（无 \\r\\n 污染，一行一条消息）")

        note("initialize 版本协商/能力/serverInfo/instructions")

        client.send("notifications/initialized", notify=True)
        note("notifications/initialized 不应答")

        # ---- 2. tools/list ----
        tools = client.send("tools/list", {})["result"]["tools"]
        check(len(tools) == EXPECTED_ALL_TOOLS,
              "工具数应为 %d，实际 %d" % (EXPECTED_ALL_TOOLS, len(tools)))
        for tool in tools:
            for key in ("name", "title", "description", "inputSchema", "annotations"):
                check(key in tool, "工具 %s 缺少 %s" % (tool.get("name"), key))
            check(tool["inputSchema"]["type"] == "object", "inputSchema 不是 object")
            check(tool["inputSchema"]["additionalProperties"] is False,
                  "additionalProperties 不是 false：%s" % tool["name"])
            check("handler" not in tool, "内部字段 handler 泄漏到 wire：%s" % tool["name"])
        note("tools/list 42 个工具，schema 形状齐全，内部字段已剥离")

        # ---- 3. 只读工具 ----
        result = call_tool(client, "fpga_project_list", {"root": CORPUS})
        data = structured(result)
        check(data["ok"] is True, "project_list 失败：%s" % data)
        check(len(data["projects"]) >= 1, "project_list 没发现工程")
        project_al = data["projects"][0]["al"]
        note("fpga_project_list 发现 %d 个工程" % len(data["projects"]))

        data = structured(call_tool(client, "fpga_project_info", {"project": project_al}))
        check(data["ok"] is True, "project_info 失败：%s" % data)
        check(data["family"] != "", "project_info family 为空")
        check(data["device"] != "", "project_info device 为空")
        check(data["top"] != "", "project_info top 为空")
        check(data["files"]["hdl"] > 0, "project_info hdl 文件数为 0")
        note("fpga_project_info family=%s device=%s top=%s"
             % (data["family"], data["device"], data["top"]))

        data = structured(call_tool(client, "fpga_project_files",
                                    {"project": project_al, "kind": "hdl"}))
        check(data["ok"] and len(data["files"]) > 0, "project_files 没有 HDL 文件")
        check(all(item["kind"] in ("verilog", "vhdl") for item in data["files"]),
              "project_files kind 过滤失效")
        note("fpga_project_files kind=hdl 返回 %d 个文件" % len(data["files"]))

        # 注意：top.v 前 12 行是注释，module 头在 1-based 第 15 行
        data = structured(call_tool(client, "fpga_read_file",
                                    {"project": project_al, "file": "src/top.v",
                                     "offset": 15, "limit": 3}))
        check(data["ok"] is True, "read_file 失败：%s" % data)
        check("module top" in data["text"], "read_file 没读到 module top：%r" % data["text"][:120])
        check(data["lineStart"] == 15, "read_file lineStart 应为 15，实际 %s" % data["lineStart"])
        note("fpga_read_file 从第 15 行读到 module 头")

        data = structured(call_tool(client, "fpga_search_text",
                                    {"project": project_al, "query": "pwmG",
                                     "limit": 20}))
        check(data["ok"] is True, "search_text 失败：%s" % data)
        check(data["count"] > 0, "search_text 没找到 pwmG")
        check(data["matches"][0]["line"] >= 1, "search_text 行号不是 1-based")
        note("fpga_search_text 命中 %d 处" % data["count"])

        data = structured(call_tool(client, "fpga_constraints_status",
                                    {"project": project_al}))
        check(data["ok"] is True, "constraints_status 失败：%s" % data)
        # B6/D-2：Python 的 constraints_status 没有 portsSource 键，C++ 已对齐移除；
        # 这里只断言端口数与既有字段。
        check(data["ports"] > 0, "constraints_status 没扫到顶层端口")
        check("missing" in data and "missingCount" in data, "constraints_status 字段缺失")
        check(len(data["sdc"]) >= 1, "constraints_status 没有 sdc 文件")
        note("fpga_constraints_status ports=%d constrained=%d missing=%d"
             % (data["ports"], data["constrainedPorts"], data["missingCount"]))

        data = structured(call_tool(client, "fpga_constraint_audit",
                                    {"project": project_al, "include_device": False}))
        check(data["ok"] is True, "constraint_audit 失败：%s" % data)
        check(data["totalBits"] >= data["constrainedBits"], "constraint_audit 计数矛盾")
        check(data["deviceLoaded"] is False, "include_device=false 时不该加载器件")
        note("fpga_constraint_audit totalBits=%d constrainedBits=%d"
             % (data["totalBits"], data["constrainedBits"]))

        data = structured(call_tool(client, "fpga_td_info"))
        check(data["ok"] is True and "td" in data, "td_info 形状不对")
        note("fpga_td_info ok=%s" % data["td"].get("ok"))

        data = structured(call_tool(client, "fpga_flow_status", {"project": project_al}))
        check(data["ok"] is True and "flow" in data, "flow_status 形状不对")
        check(data["flow"]["running"] is False, "flow 不该在跑")
        note("fpga_flow_status 返回 idle 状态")

        # ---- 3b. 符号类（阶段二 A 批：11 个工具走 hdl 索引 + server::features）----
        data = structured(call_tool(client, "fpga_document_symbols",
                                    {"file": "src/top.v"}))
        check(data["ok"] is True, "document_symbols 失败：%s" % data)
        check(len(data["symbols"]) > 0, "document_symbols 没返回符号")
        check(data["symbols"][0]["kind"] == "module", "第一个符号应为 module")
        check(data["symbols"][0]["range"]["start"]["line"] >= 1, "MCP 位置必须 1-based")
        note("fpga_document_symbols 返回 %d 个顶层符号" % len(data["symbols"]))

        data = structured(call_tool(client, "fpga_project_structure",
                                    {"project": project_al}))
        check(data["ok"] is True, "project_structure 失败：%s" % data)
        check(data["root"]["module"] == "top", "层次树根应为 top 模块")
        check(len(data["root"]["children"]) > 0, "top 应例化子模块")
        note("fpga_project_structure root=top children=%d" % len(data["root"]["children"]))

        data = structured(call_tool(client, "fpga_module_info", {"module": "pwm_gen"}))
        check(data["ok"] is True and len(data["ports"]) > 0, "module_info 没返回端口")
        check(data["line"] >= 1, "module_info 行号必须 1-based")
        note("fpga_module_info pwm_gen ports=%d" % len(data["ports"]))

        data = structured(call_tool(client, "fpga_workspace_symbols",
                                    {"query": "pwm", "limit": 20}))
        check(data["ok"] is True and len(data["symbols"]) > 0, "workspace_symbols 没结果")
        note("fpga_workspace_symbols 命中 %d 个" % len(data["symbols"]))

        data = structured(call_tool(client, "fpga_call_hierarchy",
                                    {"module": "pwm_gen", "direction": "incoming"}))
        check(data["ok"] is True, "call_hierarchy 失败：%s" % data)
        check(len(data["incoming"]) > 0, "pwm_gen 应至少有一个 caller")
        note("fpga_call_hierarchy pwm_gen incoming=%d" % len(data["incoming"]))

        data = structured(call_tool(client, "fpga_duplicate_modules", {}))
        check(data["ok"] is True and isinstance(data["duplicates"], list),
              "duplicate_modules 形状不对")
        note("fpga_duplicate_modules %d 组重名" % len(data["duplicates"]))

        # ---- 3c. 补全 / 实例化模板（C 批）----
        data = structured(call_tool(client, "fpga_completion",
                                    {"file": "src/top.v", "line": 70, "column": 1, "limit": 10}))
        check(data["ok"] is True and isinstance(data["items"], list),
              "completion 形状不对：%s" % str(data)[:200])
        note("fpga_completion 返回 %d 项" % len(data["items"]))

        data = structured(call_tool(client, "fpga_instantiation", {"module": "pwm_gen"}))
        check(data["ok"] is True and "u_pwm_gen" in data["text"],
              "instantiation 没生成模板：%s" % str(data)[:200])
        note("fpga_instantiation 生成 %d 行模板" % data["text"].count("\n"))

        # ---- 4. Phase B 起不再有占位工具：抽查 fpga_format 真实可用 ----
        data = structured(call_tool(client, "fpga_format",
                                    {"text": "module m;\ninput a;\nendmodule\n"}))
        check(data.get("ok") is True, "fpga_format 应返回真实结果：%s" % str(data)[:200])
        check(isinstance(data.get("text"), str) and "module" in data["text"],
              "fpga_format.text 应是格式化后的源码")
        note("fpga_format 返回 %d 字节（changed=%s）"
             % (len(data["text"]), data.get("changed")))

        # ---- 5. 未知工具 ----
        result = call_tool(client, "fpga_does_not_exist")
        check(result["isError"] is True, "未知工具必须 isError=true")
        check(result["structuredContent"]["ok"] is False, "未知工具 structuredContent.ok 应为 false")
        check("Unknown tool" in result["structuredContent"]["error"], "错误信息应含 Unknown tool")
        check("error" not in result, "未知工具不该变成 JSON-RPC error")
        note("未知工具 → isError 结果（不是 -32601）")

        # ---- 6. shutdown ----
        response = client.send("shutdown")
        check("result" in response and response["result"] is None, "shutdown 应返回 null")
        note("shutdown 返回 null")
    finally:
        exit_code = client.stop()
    check(exit_code == 0, "进程退出码应为 0，实际 %s" % exit_code)

    # ---- 7. 默认可见性 + 门控提示 ----
    restricted = McpProcess(exe, ["--root", CORPUS])
    try:
        restricted.send("initialize", {"protocolVersion": "2024-11-05", "capabilities": {}})
        tools = restricted.send("tools/list", {})["result"]["tools"]
        check(len(tools) == EXPECTED_READONLY_TOOLS,
              "默认模式工具数应为 %d，实际 %d" % (EXPECTED_READONLY_TOOLS, len(tools)))
        names = tool_map(tools)
        for gated in ("fpga_flow_run", "fpga_project_rename", "fpga_analysis_run"):
            check(gated not in names, "%s 默认不该可见" % gated)
        note("默认可见性 %d 个工具（隐藏 flow/write）" % len(tools))

        result = call_tool(restricted, "fpga_flow_run", {})
        check(result["isError"] is True, "被禁用的工具应返回 isError")
        message = result["structuredContent"]["error"]
        check("--allow-flow" in message, "错误信息应提示 --allow-flow：%s" % message)
        note("禁用工具提示 --allow-flow")

        # 默认模式下 read_file 依然可用（只读工具）
        data = structured(call_tool(restricted, "fpga_read_file",
                                    {"file": "src/top.v", "limit": 2}))
        check(data["ok"] is True, "默认模式 read_file 应可用：%s" % data)
        note("默认模式只读工具仍可用")
    finally:
        restricted.stop()

    return checks


def assert_no_orphan_td() -> None:
    """shutdown 后不应残留 td_commands_prompt.exe（PYTHON-BUGS P-5）。"""
    try:
        output = subprocess.run(
            ["tasklist", "/FI", "IMAGENAME eq td_commands_prompt.exe"],
            capture_output=True, text=True, timeout=20,
        ).stdout
    except (OSError, subprocess.TimeoutExpired):
        print("  （跳过 td 孤儿检查：tasklist 不可用）")
        return
    if "td_commands_prompt.exe" in output:
        raise AssertionError("shutdown 后仍有 td_commands_prompt.exe 残留:\n%s" % output)


def main() -> int:
    parser = argparse.ArgumentParser(description="MCP server 端到端冒烟")
    parser.add_argument("--server", default=FALLBACK_SERVER, help="fpgalsp-native.exe 路径")
    args = parser.parse_args()

    exe = args.server
    if not os.path.isfile(exe):
        print("找不到 %s；请先构建或用 --server 指定。" % exe)
        return 2
    if not os.path.isdir(CORPUS):
        print("找不到语料 %s" % CORPUS)
        return 2

    print("server: %s" % exe)
    print("corpus: %s" % CORPUS)
    try:
        checks = run_suite(exe)
    except AssertionError as error:
        print("FAIL: %s" % error)
        return 1

    for index, item in enumerate(checks, 1):
        print("  %2d. %s" % (index, item))

    try:
        assert_no_orphan_td()
        print("  td_commands_prompt.exe 无残留")
    except AssertionError as error:
        print("FAIL: %s" % error)
        return 1

    print("")
    print("MCP smoke 通过：%d 项检查" % (len(checks) + 1))
    return 0


if __name__ == "__main__":
    sys.exit(main())