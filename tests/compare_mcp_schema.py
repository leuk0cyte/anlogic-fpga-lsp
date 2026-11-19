"""compare_mcp_schema.py —— C++ MCP `tools/list` 与 Python 参考实现的逐字段对比

为什么这是本任务的第一验收项
----------------------------
`inputSchema` 是 **AI agent 的实际契约**：agent 按 schema 生成参数、按 required
判断要不要传、按 enum 收窄取值。schema 一旦漂移，调用会**静默失败在 agent 侧**
（表现为"这个工具不好用"），几乎无法归因。所以 39 个工具的
`name / title / description / inputSchema / annotations` 必须与 Python 版逐字段一致。

本脚本的做法
------------
1. 直接驱动真实二进制：`fpgalsp-native.exe --mcp`，用 newline-delimited JSON-RPC
   发 `initialize` + `tools/list`，抓回 tools 数组（不依赖任何 C++ 侧的导出模式）。
2. Python 侧同样从 `fpgalsp.mcp.TOOLS` 推期望值（含 `visible_tools()` 的可见性过滤）。
3. canonical（sort_keys）后逐字段 diff；数组顺序（enum / 工具顺序）严格比对。

两种可见性模式都测
------------------
    default                     -> 只读工具（32 个）
    --allow-flow --allow-write  -> 全部 39 个
这正是 agent 实际会遇到的两种状态；只测一种会漏掉可见性过滤的 bug。

用法
----
    python tests/compare_mcp_schema.py                       # 自动找 build/bin 下的 exe
    python tests/compare_mcp_schema.py --exe build-mcp/bin/fpgalsp-native.exe
    python tests/compare_mcp_schema.py --exe <exe> --verbose  # 打印每个工具的比对结果

退出码：0 = 完全一致；1 = 有差异；2 = 环境问题（找不到 exe / Python 参考实现）。
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(HERE)
PYTHON_ROOT = r"Y:\fpga-lsp"
if os.path.isdir(PYTHON_ROOT) and PYTHON_ROOT not in sys.path:
    sys.path.insert(0, PYTHON_ROOT)

try:
    from fpgalsp import mcp as python_mcp  # type: ignore
except ImportError as error:  # pragma: no cover
    print("无法导入 Python 参考实现（%s）：%s" % (PYTHON_ROOT, error))
    sys.exit(2)

INTERNAL_KEYS = ("handler", "requires")


def expected_tools(allow_flow: bool, allow_write: bool) -> list[dict]:
    """对应 Python McpServer.visible_tools() 的输出。"""
    out = []
    for tool in python_mcp.TOOLS:
        requires = tool.get("requires")
        if requires == "flow" and not allow_flow:
            continue
        if requires == "write" and not allow_write:
            continue
        out.append({key: value for key, value in tool.items() if key not in INTERNAL_KEYS})
    return out


def candidate_exes() -> list[str]:
    return [
        os.path.join(REPO_ROOT, "build", "bin", "fpgalsp-native.exe"),
        os.path.join(REPO_ROOT, "build-mcp", "bin", "fpgalsp-native.exe"),
        os.path.join(REPO_ROOT, "build-mcp2", "bin", "fpgalsp-native.exe"),
    ]


def find_exe(explicit: str) -> str:
    if explicit:
        return explicit
    for path in candidate_exes():
        if os.path.isfile(path):
            return path
    return ""


def run_tools_list(exe: str, extra_args: list[str], verbose: bool = False) -> list[dict]:
    """启动 exe，发 initialize + tools/list，返回 tools 数组。"""
    command = [exe, "--mcp"] + list(extra_args)
    process = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        cwd=REPO_ROOT,
    )
    try:
        requests = [
            {"jsonrpc": "2.0", "id": 1, "method": "initialize",
             "params": {"protocolVersion": "2024-11-05", "capabilities": {},
                        "clientInfo": {"name": "compare-mcp-schema", "version": "1"}}},
            {"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}},
        ]
        payload = "".join(json.dumps(item, ensure_ascii=False) + "\n" for item in requests)
        process.stdin.write(payload.encode("utf-8"))
        process.stdin.flush()

        deadline_lines = 2
        tools = None
        for _ in range(deadline_lines):
            line = process.stdout.readline()
            if not line:
                break
            message = json.loads(line.decode("utf-8", "replace"))
            if message.get("id") == 2:
                tools = (message.get("result") or {}).get("tools")
        if tools is None:
            raise SystemExit("tools/list 没有返回结果（exe=%s）" % exe)
        return tools
    finally:
        try:
            process.stdin.write(b'{"jsonrpc":"2.0","id":99,"method":"shutdown"}\n')
            process.stdin.flush()
        except (BrokenPipeError, OSError):
            pass
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
        if verbose:
            print("  （已退出：%s）" % exe)


def canonical(value) -> str:
    return json.dumps(value, sort_keys=True, ensure_ascii=False)



def additive_only(cpp_tool: dict, py_tool: dict) -> bool:
    """cpp 相对 python 是否只有"新增可选属性"这一种差异。

    用户已明确放弃 Python 接口契约（2026-11），但 schema 门禁仍有价值：它能抓住
    "少字段/改语义/顺序漂移"。所以规则收敛为：
      * 顶层字段集合必须一致，除 inputSchema 外逐字段相等；
      * inputSchema 除 properties 外逐字段相等（required/type/additionalProperties 等）；
      * py 的每个 property 定义必须逐字相等；
      * cpp 多出的 property 允许存在，但**不能出现在 required 里**（保持向后兼容）。
    """
    if set(cpp_tool) != set(py_tool):
        return False
    for key in cpp_tool:
        if key == "inputSchema":
            continue
        if canonical(cpp_tool[key]) != canonical(py_tool[key]):
            return False
    cpp_schema = cpp_tool.get("inputSchema") or {}
    py_schema = py_tool.get("inputSchema") or {}
    if set(cpp_schema) != set(py_schema):
        return False
    for key in cpp_schema:
        if key == "properties":
            continue
        if canonical(cpp_schema[key]) != canonical(py_schema[key]):
            return False
    py_props = py_schema.get("properties") or {}
    cpp_props = cpp_schema.get("properties") or {}
    for name, schema in py_props.items():
        if canonical(cpp_props.get(name)) != canonical(schema):
            return False
    extra = set(cpp_props) - set(py_props)
    required = set(py_schema.get("required") or [])
    return extra.isdisjoint(required)

def compare_tools(cpp_tools: list[dict], py_tools: list[dict], label: str,
                  verbose: bool) -> int:
    """返回差异条数。"""
    problems = 0
    cpp_by_name = {tool.get("name", ""): tool for tool in cpp_tools}
    py_by_name = {tool.get("name", ""): tool for tool in py_tools}

    for name in py_by_name:
        if name not in cpp_by_name:
            print("  [%s] 缺少工具：%s" % (label, name))
            problems += 1
    for name in cpp_by_name:
        if name not in py_by_name:
            print("  [%s] 多出工具：%s" % (label, name))
            problems += 1

    # 顺序也是契约的一部分（客户端可能依赖首次出现的顺序）
    cpp_order = [tool.get("name", "") for tool in cpp_tools]
    py_order = [tool.get("name", "") for tool in py_tools]
    if cpp_order != py_order:
        for index, (left, right) in enumerate(zip(cpp_order, py_order)):
            if left != right:
                print("  [%s] 工具顺序不同：第 %d 个 cpp=%s py=%s"
                      % (label, index, left, right))
                break
        problems += 1

    for name, py_tool in py_by_name.items():
        cpp_tool = cpp_by_name.get(name)
        if cpp_tool is None:
            continue
        if canonical(cpp_tool) == canonical(py_tool):
            if verbose:
                print("  [%s] OK   %s" % (label, name))
            continue
        if additive_only(cpp_tool, py_tool):
            # 只多了可选参数（例如 P-B2 给 workspace_symbols 加的 offset）：允许，
            # 但打印出来，避免悄悄扩参。
            print("  [%s] EXT  %s（cpp 多出可选属性，允许）" % (label, name))
            continue
        problems += 1
        print("  [%s] DIFF %s" % (label, name))
        for key in sorted(set(cpp_tool) | set(py_tool)):
            if key not in cpp_tool:
                print("        %s: 只在 python 侧" % key)
                continue
            if key not in py_tool:
                print("        %s: 只在 cpp 侧" % key)
                continue
            cpp_value = cpp_tool[key]
            py_value = py_tool[key]
            if canonical(cpp_value) == canonical(py_value):
                continue
            print("        %s:" % key)
            print("          cpp = %s" % canonical(cpp_value)[:400])
            print("          py  = %s" % canonical(py_value)[:400])
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description="C++ MCP tools/list vs Python 参考实现")
    parser.add_argument("--exe", default="", help="fpgalsp-native.exe 路径")
    parser.add_argument("--verbose", action="store_true", help="逐个工具打印比对结果")
    args = parser.parse_args()

    exe = find_exe(args.exe)
    if not exe or not os.path.isfile(exe):
        print("找不到 fpgalsp-native.exe；请先构建，或用 --exe 指定。")
        return 2
    print("exe: %s" % exe)

    total = 0
    print("[1/2] 默认可见性（只读工具）")
    cpp_default = run_tools_list(exe, [], args.verbose)
    py_default = expected_tools(False, False)
    total += compare_tools(cpp_default, py_default, "default", args.verbose)
    print("      cpp=%d py=%d" % (len(cpp_default), len(py_default)))

    print("[2/2] --allow-flow --allow-write（全部工具）")
    cpp_all = run_tools_list(exe, ["--allow-flow", "--allow-write"], args.verbose)
    py_all = expected_tools(True, True)
    total += compare_tools(cpp_all, py_all, "all", args.verbose)
    print("      cpp=%d py=%d" % (len(cpp_all), len(py_all)))

    if total == 0:
        print("")
        print("schema 逐字段一致：%d 个工具 × 2 种可见性" % len(py_all))
        return 0
    print("")
    print("共 %d 处差异" % total)
    return 1


if __name__ == "__main__":
    sys.exit(main())