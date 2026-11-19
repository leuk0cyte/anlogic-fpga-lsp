#!/usr/bin/env python3
"""probe_mcp.py —— fpga-lsp-cpp 审计 A3：MCP / CLI / 领域语义 对照探针

与 tests/golden/tools/ 共用采集层（goldenlib.client + goldenlib.normalize）：
Python 侧走 `python -m fpgalsp.mcp`，C++ 侧走 `fpgalsp-native.exe --mcp`，
两侧都按 newline-delimited JSON-RPC 说话，因此拿到的是**真实二进制**的行为。

六个小节，可单独跑：
    schema       tools/list 在 默认 / --allow-write --allow-flow 两种可见性下的逐字段对比
    tools        cases.json 的 13 个只读工具 × 3 个工程（CorpusAlpha + 2 个 %WORKSPACE% 工程）
    placeholders 5 个占位工具 + instantiation 对照（派生型只比载荷不落盘）
    write        写 .al 的工具在 %TEMP% 副本上跑，diff 两侧的 .al（真实工程零写入）
    cli          python -m fpgalsp / -m fpgalsp.mcp 与 native --stdio / --mcp 的参数差异
    domain       reports / ADC / SDC / TD 的 MCP 层只读对比

设计约束：
  * 只读工具直接用真实工程（与 golden 采集同一约定）；任何写操作一律在 %TEMP% 副本上。
  * 结果先过 Normalizer（路径 token 化 + 易变键丢弃 + 时钟串掩码），再做结构 diff，
    所以 raw.json 里留下的是"语义差异"而不是"两台机器的路径差异"。
  * 每个工具的判定只有五种：identical / field_diff / cpp_placeholder / error_mismatch /
    *_error。判定之外仍保留 diff 明细，便于人工判断"谁对"。

用法：
    python tools/audit/probe_mcp.py                     # 全跑（含 CLI 与 domain）
    python tools/audit/probe_mcp.py --sections schema,tools
    python tools/audit/probe_mcp.py --out <path>/raw.json --projects CorpusAlpha
退出码：0 = 无差异（占位工具与已知差异除外）；1 = 有非预期差异；2 = 环境问题。
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
CPP_ROOT = os.path.dirname(os.path.dirname(HERE))          # <repo>/tools/audit -> <repo>
PY_ROOT = r"Y:\fpga-lsp"
GOLDEN_TOOLS = os.path.join(CPP_ROOT, "tests", "golden", "tools")

if GOLDEN_TOOLS not in sys.path:
    sys.path.insert(0, GOLDEN_TOOLS)
if os.path.isdir(PY_ROOT) and PY_ROOT not in sys.path:
    sys.path.insert(0, PY_ROOT)

from goldenlib.client import McpClient, NativeMcpClient, TransportError  # noqa: E402
from goldenlib.normalize import (                                       # noqa: E402
    Normalizer, GOLDEN_VOLATILE_KEYS, _CLOCK_PATTERNS, _PATHISH_KEYS, _TOKEN_RE)

# 两侧子进程都用同一个 cwd（C++ 仓库根），所以 ``python -m fpgalsp.mcp`` 必须靠
# PYTHONPATH 才找得到参考实现。golden 采集层把 cwd 设为参考仓库根，这里改成显式
# 环境变量：cwd 相同，两侧相对路径解析才不会成为 diff 的噪声源。
_python_path = os.environ.get("PYTHONPATH", "")
if PY_ROOT not in _python_path.split(os.pathsep):
    os.environ["PYTHONPATH"] = (PY_ROOT + os.pathsep + _python_path) if _python_path else PY_ROOT

# 中立的 cwd：两侧子进程与 Normalizer 共用。
#   * Normalizer.relative_token() 会对**裸相对名**做 os.path.abspath()（按进程 cwd），
#     所以如果 cwd 是 <repo>，`project: "demo_project"` 会被改写成 `<cpp>/demo_project`，
#     把"一个名字"伪装成"一条路径"；cwd 是空目录时两侧同样变成 `<cwd>/demo_project`，
#     既是确定的，也不会掩盖"一侧给相对路径、另一侧给绝对路径"这种真差异。
#   * 两侧 server 的 cwd 相同，任何 cwd 相关输出（默认日志/相对解析）都不再是噪声源。
WORK_CWD = os.path.join(tempfile.gettempdir(), "pm-cwd")
os.makedirs(WORK_CWD, exist_ok=True)

# --------------------------------------------------------------------------- 语料
# 与 tests/golden/corpus/cases.json 的 projects 保持一致（含 skip 规则外的全部现存工程）。
PROJECTS = [
    {"id": "CorpusAlpha", "root": r"%WORKSPACE%\CorpusAlpha", "al": "demo_project.al"},
    {"id": "CorpusAlpha_self_pwm_gen", "root": r"%WORKSPACE%\CorpusAlpha_self_pwm_gen",
     "al": "demo_project.al"},
    {"id": "CorpusBeta", "root": r"Y:\sic\unit_test\CorpusBeta", "al": "CorpusBeta.al"},
]

# cases.json mcpTools.readOnly —— 13 个只读工具，顺序即契约顺序。
READ_ONLY_TOOLS = [
    "fpga_project_list",
    "fpga_project_info",
    "fpga_project_files",
    "fpga_project_structure",
    "fpga_document_symbols",
    "fpga_workspace_symbols",
    "fpga_definition",
    "fpga_references",
    "fpga_hover",
    "fpga_completion",
    "fpga_duplicate_modules",
    "fpga_diagnostics",
    "fpga_device_info",
]

# REPORT 工具在 domain 小节单独跑（不在 cases.json 的 13 个里）。
DOMAIN_TOOLS = [
    "fpga_project_reports",
    "fpga_constraint_audit",
    "fpga_constraints_status",
    "fpga_td_info",
]

TOOL_TIMEOUTS = {
    "fpga_diagnostics": 600.0,
    "fpga_device_info": 300.0,
    "fpga_project_structure": 300.0,
    "fpga_project_reports": 300.0,
    "fpga_constraint_audit": 300.0,
    "fpga_analysis_run": 300.0,
    "fpga_project_update": 300.0,
}
DEFAULT_TIMEOUT = 180.0

# 有副作用的调用（默认跳过）
#   * fpga_device_info：Python 把器件引脚表缓存写到 <project>/.fpgalsp/cache/（P-4），
#     在真实工程上跑就会留下文件。
#   * fpga_analysis_run：Python 会起 TD 会话，并把 .fpgalsp/run/td_*.log 写进工程目录。
# 两者都"名义只读、实际写盘"。默认不跑；审计要复现时必须显式 --allow-side-effects，
# 并在报告里披露副作用（见 PROBE.md「副作用清单」）。
ALLOW_SIDE_EFFECTS = False
# B6：TD 类工具（device_info / analysis_run / project_update）现在有真实实现，
# 会对真实工程写 `.fpgalsp/`。`--copy-projects` 把语料复制到 %TEMP% 后再跑，
# 两侧服务器都用**同一份副本**（路径一致，diff 里不会出现副本名噪声）。
COPY_PROJECTS = False
COPY_ROOT = os.path.join(tempfile.gettempdir(), "pm-corpus")


def prepare_project(spec):
    """Return the root actually used for tool calls (副本 or 原工程)."""
    if not COPY_PROJECTS:
        return spec["root"]
    if spec["id"] in _PREPARED:
        return _PREPARED[spec["id"]]
    if not project_files_exist(spec):
        return spec["root"]
    target = os.path.join(COPY_ROOT, spec["id"])
    if os.path.isdir(target):
        shutil.rmtree(target, ignore_errors=True)
    shutil.copytree(spec["root"], target,
                    ignore=shutil.ignore_patterns("*_Runs", ".fpgalsp", "__pycache__"))
    _PREPARED[spec["id"]] = target
    log("  copied %s -> %s" % (spec["id"], target))
    return target


_PREPARED = {}
SIDE_EFFECT_TOOLS = {
    "fpga_device_info": "Python writes <project>/.fpgalsp/cache/device_*.json (PYTHON-BUGS P-4)",
    "fpga_analysis_run": "Python starts a TD session and writes <project>/.fpgalsp/run/td_*.log",
}


def side_effect_skip(name):
    return {"classification": "skipped_side_effect",
            "reason": "%s; pass --allow-side-effects to run it" % SIDE_EFFECT_TOOLS[name]}

PLACEHOLDER_TOOLS = [
    "fpga_format",
    "fpga_code_actions",
    "fpga_rename_edits",
    "fpga_analysis_run",
    "fpga_project_update",
]

MAX_DIFFS_PER_TOOL = 80
MAX_PAYLOAD_BYTES = 200_000
MAX_DIFF_VALUE_CHARS = 600


# --------------------------------------------------------------------------- 基础设施
_START = time.time()


def log(message):
    print("[%6.1fs] %s" % (time.time() - _START, message), file=sys.stderr, flush=True)


def sha256_file(path):
    try:
        with open(path, "rb") as handle:
            return hashlib.sha256(handle.read()).hexdigest().upper()
    except OSError:
        return ""


def canonical(value):
    return json.dumps(value, sort_keys=True, ensure_ascii=False, default=str)


def digest(value):
    return hashlib.sha256(canonical(value).encode("utf-8")).hexdigest()[:16]


def clip(value, limit=MAX_DIFF_VALUE_CHARS):
    text = canonical(value)
    if len(text) <= limit:
        return value
    return {"__clipped": text[:limit] + "...", "__chars": len(text)}


def payload_for_record(value):
    """Keep small payloads verbatim; large ones become a head + digest (raw.json stays usable)."""
    if value is None:
        return None
    text = canonical(value)
    if len(text) <= MAX_PAYLOAD_BYTES:
        return value
    return {"__digest": digest(value), "__chars": len(text), "__head": text[:4000]}


def type_name(value):
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "bool"
    if isinstance(value, (int, float)):
        return "number"
    if isinstance(value, str):
        return "string"
    if isinstance(value, list):
        return "array"
    if isinstance(value, dict):
        return "object"
    return type(value).__name__


def numbers_equal(left, right):
    if isinstance(left, bool) or isinstance(right, bool):
        return False
    try:
        return float(left) == float(right)
    except (TypeError, ValueError):
        return False


def diff_json(py_value, cpp_value, path="", out=None):
    """Structural diff. Left = Python oracle, right = C++ port."""
    if out is None:
        out = []
    if len(out) >= MAX_DIFFS_PER_TOOL:
        return out
    if isinstance(py_value, dict) and isinstance(cpp_value, dict):
        py_keys, cpp_keys = set(py_value), set(cpp_value)
        for key in sorted(cpp_keys - py_keys):
            out.append({"path": "%s.%s" % (path, key) if path else key,
                        "kind": "cpp_only_key", "cpp": clip(cpp_value[key])})
        for key in sorted(py_keys - cpp_keys):
            out.append({"path": "%s.%s" % (path, key) if path else key,
                        "kind": "py_only_key", "py": clip(py_value[key])})
        for key in sorted(py_keys & cpp_keys):
            if len(out) >= MAX_DIFFS_PER_TOOL:
                break
            diff_json(py_value[key], cpp_value[key],
                      "%s.%s" % (path, key) if path else key, out)
        return out
    if isinstance(py_value, list) and isinstance(cpp_value, list):
        if len(py_value) != len(cpp_value):
            out.append({"path": path or "$", "kind": "array_length",
                        "py": len(py_value), "cpp": len(cpp_value)})
        for index in range(min(len(py_value), len(cpp_value))):
            if len(out) >= MAX_DIFFS_PER_TOOL:
                break
            diff_json(py_value[index], cpp_value[index], "%s[%d]" % (path, index), out)
        return out
    if isinstance(py_value, (int, float)) and isinstance(cpp_value, (int, float)) \
            and numbers_equal(py_value, cpp_value):
        return out
    if type_name(py_value) != type_name(cpp_value):
        out.append({"path": path or "$", "kind": "type",
                    "py": type_name(py_value), "cpp": type_name(cpp_value),
                    "pyValue": clip(py_value), "cppValue": clip(cpp_value)})
        return out
    if py_value != cpp_value:
        out.append({"path": path or "$", "kind": "value",
                    "py": clip(py_value), "cpp": clip(cpp_value)})
    return out


def make_normalizer(roots_extra=None):
    roots = {"repo": PY_ROOT, "cpp": CPP_ROOT, "cwd": WORK_CWD,
             "tmp": tempfile.gettempdir()}
    for spec in PROJECTS:
        roots["corpus:" + spec["id"]] = prepare_project(spec)
    if roots_extra:
        roots.update(roots_extra)
    return AuditNormalizer(roots)


class AuditNormalizer(Normalizer):
    """goldenlib Normalizer + 一条审计专用修正。

    ``Normalizer._normalize_string()`` 对**每个**字符串先跑 ``relative_token()``，
    而后者会 ``os.path.abspath()``（按进程 cwd）。于是 ``kind: "field"``、
    ``project: "demo_project"``、``instance: "pll_gen"`` 这类**裸名字**会被改写成
    ``<cpp>/field`` ——把"名字"伪装成"路径"。两侧 server 的 cwd 相同时它不产生
    假 diff，但会让 raw.json 无法阅读，并且掩盖"一侧给名字、另一侧给路径"的情况。
    这里只对**看起来像路径**的字符串走父类逻辑，其余只做时钟掩码 + token 归一。
    """

    def _normalize_string(self, text, key):
        if not isinstance(text, str):
            return text
        pathish = (key in _PATHISH_KEYS or "\\" in text
                   or re.search(r"[A-Za-z]:[/\\]", text) or text.startswith("/"))
        if pathish:
            return super()._normalize_string(text, key)
        for pattern, replacement in _CLOCK_PATTERNS:
            text = pattern.sub(replacement, text)
        return _TOKEN_RE.sub(lambda match: "<%s>" % match.group(1).lower(), text)


def fold_copy_names(value):
    """把 `pm-copy-<pid>/{py,cpp}` 折成 `<side>`。

    TD 的 `output` 里出现的是 **8.3 短路径**（`%USERPROFILE%/...`），
    与 Normalizer 注册的长路径同前缀不匹配，所以这里按"副本目录名"做一次正则折叠；
    两侧各自跑在不同副本目录，只有折掉名字才能看出"其余字段逐字一致"。
    """
    text = canonical(value)
    text = re.sub(r"(pm-copy-\d+[\\/])(py|cpp)\b", r"\1<side>", text)
    try:
        return json.loads(text)
    except ValueError:
        return value


def call_tool(client, name, arguments, timeout=None):
    started = time.time()
    try:
        result, is_error = client.call_tool(name, arguments or {},
                                           timeout=timeout or TOOL_TIMEOUTS.get(name,
                                                                                DEFAULT_TIMEOUT))
        return {"ok": True, "isError": bool(is_error), "result": result,
                "seconds": round(time.time() - started, 2)}
    except TransportError as exc:
        return {"ok": False, "error": str(exc)[:800],
                "seconds": round(time.time() - started, 2)}
    except Exception as exc:                                   # noqa: BLE001
        return {"ok": False, "error": "%s: %s" % (type(exc).__name__, exc),
                "seconds": round(time.time() - started, 2)}


def is_placeholder_payload(result):
    return (isinstance(result, dict) and result.get("ok") is False
            and str(result.get("error", "")).startswith("not implemented"))


def classify_pair(py_call, cpp_call, diffs):
    if not py_call.get("ok") and not cpp_call.get("ok"):
        return "both_error"
    if py_call.get("ok") and not cpp_call.get("ok"):
        return "cpp_transport_error"
    if cpp_call.get("ok") and not py_call.get("ok"):
        return "py_transport_error"
    if is_placeholder_payload(cpp_call.get("result")):
        return "cpp_placeholder"
    if bool(py_call.get("isError")) != bool(cpp_call.get("isError")):
        return "error_mismatch"
    return "field_diff" if diffs else "identical"


def compare_payloads(py_call, cpp_call, normalizer, label):
    """Normalize both payloads, diff them, and return a self-contained record."""
    py_norm = normalizer.normalize(py_call.get("result")) if py_call.get("ok") else None
    cpp_norm = normalizer.normalize(cpp_call.get("result")) if cpp_call.get("ok") else None
    diffs = []
    raw_diffs = []
    if py_call.get("ok") and cpp_call.get("ok"):
        diffs = diff_json(py_norm, cpp_norm)
        # 归一化会抹掉 C:/ 盘符、时间戳之类的原始证据；差异条目额外保留原始值，
        # 便于报告里写"Python 给的是 C:/CorpusAlpha/… 而不是 Y:/…"这种可核对的事实。
        raw_diffs = diff_json(py_call.get("result"), cpp_call.get("result"))[:8]
    record = {
        "tool": label,
        "classification": classify_pair(py_call, cpp_call, diffs),
        "diffCount": len(diffs),
        "diffs": diffs,
        "rawDiffs": raw_diffs,
        "python": {"ok": py_call.get("ok"), "isError": py_call.get("isError"),
                   "seconds": py_call.get("seconds"), "error": py_call.get("error"),
                   "digest": digest(py_norm) if py_norm is not None else None,
                   "payload": payload_for_record(py_norm)},
        "cpp": {"ok": cpp_call.get("ok"), "isError": cpp_call.get("isError"),
                "seconds": cpp_call.get("seconds"), "error": cpp_call.get("error"),
                "digest": digest(cpp_norm) if cpp_norm is not None else None,
                "payload": payload_for_record(cpp_norm)},
    }
    if py_call.get("ok") and cpp_call.get("ok"):
        record["shape"] = {"python": shape_of(py_norm), "cpp": shape_of(cpp_norm)}
    return record


def shape_of(value, depth=0):
    """Coarse shape signature: keys/types only, values dropped (for 'one side placeholder')."""
    if depth > 3:
        return type_name(value)
    if isinstance(value, dict):
        return {key: shape_of(value[key], depth + 1) for key in sorted(value)}
    if isinstance(value, list):
        return [shape_of(value[0], depth + 1)] if value else []
    return type_name(value)


def first_hdl_file(py_client, spec):
    """The reference implementation enumerates the .al, so it is the oracle for probe args."""
    call = call_tool(py_client, "fpga_project_files",
                     {"kind": "hdl", "include_excluded": True}, timeout=120)
    if call.get("ok") and isinstance(call.get("result"), dict):
        files = call["result"].get("files") or []
        for entry in files:
            if isinstance(entry, dict) and entry.get("file"):
                return entry["file"]
    return None


_MODULE_RE = re.compile(r"^\s*module\s+(?P<name>[A-Za-z_][A-Za-z0-9_$]*)")


def module_position(path):
    """Replicates collect.py::DocPositions.module_name + mcp_arguments column convention."""
    try:
        with open(path, encoding="utf-8", errors="replace") as handle:
            lines = handle.read().replace("\r\n", "\n").replace("\r", "\n").split("\n")
    except OSError:
        return None
    for index, row in enumerate(lines):
        match = _MODULE_RE.match(row)
        if match:
            return {"line": index + 1, "column": match.start("name") + 2,
                    "name": match.group("name"), "file": path}
    return None


def project_files_exist(spec):
    return os.path.isdir(spec["root"]) and os.path.isfile(
        os.path.join(spec["root"], spec["al"] or ""))


# --------------------------------------------------------------------------- schema
def section_schema(exe, log_dir, out):
    log("schema: tools/list 两种可见性")
    section = {"exe": exe, "exeSha256": sha256_file(exe)}
    import fpgalsp.mcp as python_mcp  # noqa: E402  (import here so --sections cli works alone)

    internal = ("handler", "requires")

    def expected(allow_flow, allow_write):
        tools_out = []
        for tool in python_mcp.TOOLS:
            requires = tool.get("requires")
            if requires == "flow" and not allow_flow:
                continue
            if requires == "write" and not allow_write:
                continue
            tools_out.append({key: value for key, value in tool.items() if key not in internal})
        return tools_out

    for label, extra in (("default", []),
                         ("allow_flow_write", ["--allow-flow", "--allow-write"])):
        py_log = os.path.join(log_dir, "schema-py-%s.log" % label)
        cpp_log = os.path.join(log_dir, "schema-cpp-%s.log" % label)
        py_client = McpClient(WORK_CWD, r"%WORKSPACE%\CorpusAlpha", py_log, extra_args=extra)
        cpp_client = NativeMcpClient(WORK_CWD, r"%WORKSPACE%\CorpusAlpha", cpp_log,
                                     exe_path=exe, extra_args=extra)
        try:
            py_tools = (py_client.list_tools() or {}).get("tools") or []
            cpp_tools = (cpp_client.list_tools() or {}).get("tools") or []
        finally:
            py_client.close()
            cpp_client.close()
        expected_tools = expected("--allow-flow" in extra, "--allow-write" in extra)
        py_by_name = {tool.get("name"): tool for tool in expected_tools}
        cpp_by_name = {tool.get("name"): tool for tool in cpp_tools}
        per_tool = {}
        for name in sorted(set(py_by_name) | set(cpp_by_name)):
            py_tool, cpp_tool = py_by_name.get(name), cpp_by_name.get(name)
            if py_tool is None:
                per_tool[name] = {"status": "cpp_only"}
            elif cpp_tool is None:
                per_tool[name] = {"status": "py_only"}
            else:
                diffs = diff_json(py_tool, cpp_tool)
                per_tool[name] = {"status": "identical" if not diffs else "diff",
                                  "diffs": diffs}
        section[label] = {
            "pythonToolCount": len(expected_tools),
            "cppToolCount": len(cpp_tools),
            "pythonListedCount": len(py_tools),
            "countsMatch": len(expected_tools) == len(cpp_tools),
            "orderMatches": [tool.get("name") for tool in py_tools]
                            == [tool.get("name") for tool in cpp_tools],
            "namesMatch": sorted(py_by_name) == sorted(cpp_by_name),
            "differentTools": sorted(name for name, item in per_tool.items()
                                     if item["status"] != "identical"),
            "perTool": per_tool,
        }
        log("  %s: cpp=%d py=%d diffTools=%d"
            % (label, len(cpp_tools), len(expected_tools),
               len(section[label]["differentTools"])))
    return section


# --------------------------------------------------------------------------- tools
def arguments_for(name, spec, first_file, position):
    if name == "fpga_project_list":
        return {"root": spec["root"]}
    if name in ("fpga_project_info", "fpga_duplicate_modules"):
        return {}
    if name == "fpga_project_files":
        return {"kind": "hdl", "include_excluded": True}
    if name == "fpga_project_structure":
        return {}
    if name == "fpga_document_symbols":
        return {"file": first_file} if first_file else None
    if name == "fpga_workspace_symbols":
        return {"query": "pwm", "limit": 50}
    if name == "fpga_diagnostics":
        return {"file": first_file, "sources": ["hdl", "adc", "sdc"], "limit": 500} \
            if first_file else None
    if name == "fpga_device_info":
        return {"include_pins": True, "pin_filter": "P8"}
    if position and name in ("fpga_definition", "fpga_references", "fpga_hover"):
        args = {"file": position["file"], "line": position["line"],
                "column": position["column"]}
        if name == "fpga_references":
            args["include_declaration"] = True
        return args
    if position and name == "fpga_completion":
        return {"file": position["file"], "line": position["line"],
                "column": position["column"], "limit": 200}
    return None


def section_tools(exe, log_dir, out, only_projects=None):
    log("tools: 13 个只读工具 × 工程")
    normalizer = make_normalizer()
    section = {"tools": READ_ONLY_TOOLS, "projects": {}}
    for spec in PROJECTS:
        if only_projects and spec["id"] not in only_projects:
            continue
        if not project_files_exist(spec):
            section["projects"][spec["id"]] = {"skipped": "project missing"}
            continue
        log("  project %s" % spec["id"])
        spec = dict(spec, root=prepare_project(spec))
        py_log = os.path.join(log_dir, "tools-py-%s.log" % spec["id"])
        cpp_log = os.path.join(log_dir, "tools-cpp-%s.log" % spec["id"])
        # 两侧都开 --allow-flow --allow-write，让占位/派生工具也可调用（只读参数不会写）。
        py_client = McpClient(WORK_CWD, spec["root"], py_log,
                              extra_args=["--allow-flow", "--allow-write"])
        cpp_client = NativeMcpClient(WORK_CWD, spec["root"], cpp_log, exe_path=exe,
                                     extra_args=["--allow-flow", "--allow-write"])
        entry = {"root": spec["root"], "al": spec["al"], "tools": {}}
        try:
            first = first_hdl_file(py_client, spec)
            position = module_position(first) if first else None
            entry["firstFile"] = first
            entry["position"] = position
            for name in READ_ONLY_TOOLS:
                args = arguments_for(name, spec, first, position)
                if args is None:
                    entry["tools"][name] = {"classification": "no_arguments",
                                            "tool": name}
                    continue
                if name in SIDE_EFFECT_TOOLS and not ALLOW_SIDE_EFFECTS:
                    entry["tools"][name] = side_effect_skip(name)
                    log("    %-26s skipped_side_effect" % name)
                    continue
                py_call = call_tool(py_client, name, args)
                cpp_call = call_tool(cpp_client, name, args)
                record = compare_payloads(py_call, cpp_call, normalizer, name)
                record["arguments"] = normalizer.normalize(args)
                entry["tools"][name] = record
                log("    %-26s %-18s diffs=%d py=%.1fs cpp=%.1fs"
                    % (name, record["classification"], record["diffCount"],
                       record["python"]["seconds"] or 0,
                       record["cpp"]["seconds"] or 0))
        except Exception as exc:                             # noqa: BLE001
            entry["error"] = "%s: %s" % (type(exc).__name__, exc)
            entry["traceback"] = traceback.format_exc()[-2000:]
            log("  project %s FAILED: %s" % (spec["id"], exc))
        finally:
            py_client.close()
            cpp_client.close()
        section["projects"][spec["id"]] = entry
    return section


# --------------------------------------------------------------------------- placeholders
def section_placeholders(exe, log_dir, out, copy_root):
    log("placeholders: 5 个占位工具 + instantiation")
    normalizer = make_normalizer()
    spec = dict(PROJECTS[0], root=prepare_project(PROJECTS[0]))
    section = {"project": spec["id"], "tools": {}}
    py_log = os.path.join(log_dir, "ph-py.log")
    cpp_log = os.path.join(log_dir, "ph-cpp.log")
    py_client = McpClient(WORK_CWD, spec["root"], py_log,
                          extra_args=["--allow-flow", "--allow-write"])
    cpp_client = NativeMcpClient(WORK_CWD, spec["root"], cpp_log, exe_path=exe,
                                 extra_args=["--allow-flow", "--allow-write"])
    try:
        first = first_hdl_file(py_client, spec)
        position = module_position(first) if first else None
        info = call_tool(py_client, "fpga_project_info", {})
        top = ""
        if info.get("ok") and isinstance(info.get("result"), dict):
            top = info["result"].get("top") or ""
        module = position["name"] if position else (top or "")
        sample_text = "module m(input a, output b);\nassign b=a;\nendmodule\n"
        cases = {
            "fpga_format": [
                ("text", {"text": sample_text}),
                ("file", {"file": first, "tab_size": 4, "insert_spaces": True}
                 if first else None),
            ],
            "fpga_code_actions": [
                ("range", {"file": position["file"], "start_line": position["line"],
                           "start_column": position["column"],
                           "end_line": position["line"], "end_column": position["column"],
                           "include_edits": True} if position else None),
            ],
            "fpga_rename_edits": [
                ("rename", {"file": position["file"], "line": position["line"],
                            "column": position["column"],
                            "new_name": "audit_renamed"} if position else None),
            ],
            "fpga_analysis_run": [
                ("project", {}),
            ],
            "fpga_project_update": [
                ("copy_only", {"project": None}),   # 真实调用在 write 小节用副本
            ],
            # 派生型对照项：C++ 已实现，用来证明"占位清单"没有多算。
            "fpga_instantiation": [
                ("module", {"module": module, "instance_name": "u_audit"}) if module else None,
            ],
        }
        for name, variants in cases.items():
            records = {}
            for label, args in variants:
                if args is None:
                    records[label] = {"classification": "no_arguments", "tool": name}
                    continue
                if label == "copy_only":
                    records[label] = {
                        "classification": "deferred",
                        "reason": "writes the .al (check_project_version + open_project -update); "
                                  "executed only on a %TEMP% copy, see section 'write'",
                    }
                    continue
                clean = {key: value for key, value in args.items() if value is not None}
                if name in SIDE_EFFECT_TOOLS and not ALLOW_SIDE_EFFECTS:
                    records[label] = side_effect_skip(name)
                    log("    %-24s %-16s skipped_side_effect" % (name, label))
                    continue
                py_call = call_tool(py_client, name, clean)
                cpp_call = call_tool(cpp_client, name, clean)
                record = compare_payloads(py_call, cpp_call, normalizer, name)
                record["arguments"] = normalizer.normalize(clean)
                records[label] = record
                log("    %-24s %-16s %-18s diffs=%d"
                    % (name, label, record["classification"], record["diffCount"]))
            section["tools"][name] = records if len(records) > 1 else records[
                next(iter(records))]
    finally:
        py_client.close()
        cpp_client.close()
    section["placeholderVerdict"] = summarize_placeholders(section)
    return section


def summarize_placeholders(section):
    """Per-tool one-liner: what Python returns, what C++ returns, blocking dependency."""
    out = {}
    for name, record in section.get("tools", {}).items():
        if isinstance(record, dict) and "classification" in record:
            variants = {"": record}
        else:
            variants = record
        for label, item in variants.items():
            if not isinstance(item, dict) or "classification" not in item:
                continue
            key = name if not label else "%s/%s" % (name, label)
            out[key] = {
                "classification": item["classification"],
                "pythonShape": _shape_summary(item.get("shape", {}).get("python")),
                "cppShape": _shape_summary(item.get("shape", {}).get("cpp")),
                "pythonOk": (item.get("python") or {}).get("ok"),
                "cppOk": (item.get("cpp") or {}).get("ok"),
                "cppError": (item.get("cpp") or {}).get("error")
                             or ((item.get("cpp") or {}).get("payload") or {}).get("error")
                             if isinstance((item.get("cpp") or {}).get("payload"), dict) else None,
            }
    return out


def _shape_summary(shape):
    if isinstance(shape, dict):
        return sorted(shape)
    return shape


# --------------------------------------------------------------------------- write (.al)
def _copy_project(src, dst):
    shutil.copytree(src, dst,
                    ignore=shutil.ignore_patterns("*_Runs", ".fpgalsp", "__pycache__"))


def _al_path(root, spec):
    return os.path.join(root, spec["al"])


def section_write(exe, log_dir, out, copy_root):
    log("write: .al 写工具在 %%TEMP%% 副本上对照（真实工程零写入）")
    spec = PROJECTS[0]
    normalizer = make_normalizer({"tmp": copy_root})
    original_al = _al_path(spec["root"], spec)
    original_hash = sha256_file(original_al)
    section = {"project": spec["id"], "originalAlSha256": original_hash, "tools": {}}

    py_dir = os.path.join(copy_root, "py")
    cpp_dir = os.path.join(copy_root, "cpp")
    if os.path.isdir(py_dir):
        shutil.rmtree(py_dir, ignore_errors=True)
    if os.path.isdir(cpp_dir):
        shutil.rmtree(cpp_dir, ignore_errors=True)
    _copy_project(spec["root"], py_dir)
    _copy_project(spec["root"], cpp_dir)

    py_client = McpClient(WORK_CWD, py_dir, os.path.join(log_dir, "write-py.log"),
                          extra_args=["--allow-flow", "--allow-write"])
    cpp_client = NativeMcpClient(WORK_CWD, cpp_dir, os.path.join(log_dir, "write-cpp.log"),
                                 exe_path=exe,
                                 extra_args=["--allow-flow", "--allow-write"])
    try:
        # 1) project_rename：改名
        py_rename = call_tool(py_client, "fpga_project_rename", {"name": "audit_merged_name"})
        cpp_rename = call_tool(cpp_client, "fpga_project_rename", {"name": "audit_merged_name"})
        py_al = open(_al_path(py_dir, spec), encoding="utf-8", errors="replace").read()
        cpp_al = open(_al_path(cpp_dir, spec), encoding="utf-8", errors="replace").read()
        py_norm, cpp_norm = normalizer.normalize(py_al), normalizer.normalize(cpp_al)
        section["tools"]["fpga_project_rename"] = {
            "classification": ("identical" if py_norm == cpp_norm else "field_diff"),
            "python": {"ok": py_rename.get("ok"), "isError": py_rename.get("isError"),
                       "payload": payload_for_record(
                           normalizer.normalize(py_rename.get("result")))},
            "cpp": {"ok": cpp_rename.get("ok"), "isError": cpp_rename.get("isError"),
                    "payload": payload_for_record(
                        normalizer.normalize(cpp_rename.get("result")))},
            "alDiff": diff_json(py_al.split("\n"), cpp_al.split("\n"), "al")[:40],
            "alPythonHead": py_al.split("\n")[:4],
            "alCppHead": cpp_al.split("\n")[:4],
        }
        log("    fpga_project_rename -> %s"
            % section["tools"]["fpga_project_rename"]["classification"])

        # 2) project_add_source：加一个副本里新建的源文件
        added_rel = "src/audit_probe_added.v"
        added_body = ("module audit_probe_added(input wire a, output wire b);\n"
                      "  assign b = a;\nendmodule\n")
        for root in (py_dir, cpp_dir):
            target = os.path.join(root, added_rel.replace("/", os.sep))
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with open(target, "w", encoding="utf-8", newline="\n") as handle:
                handle.write(added_body)
        py_args = {"file": os.path.join(py_dir, added_rel.replace("/", os.sep))}
        cpp_args = {"file": os.path.join(cpp_dir, added_rel.replace("/", os.sep))}
        py_add = call_tool(py_client, "fpga_project_add_source", py_args)
        cpp_add = call_tool(cpp_client, "fpga_project_add_source", cpp_args)
        py_al = open(_al_path(py_dir, spec), encoding="utf-8", errors="replace").read()
        cpp_al = open(_al_path(cpp_dir, spec), encoding="utf-8", errors="replace").read()
        # 副本目录名不同（py/cpp），比较前把各自的副本根名字令牌化
        flipped_py = normalizer.normalize(py_al).replace("/py/", "/<copy>/")
        flipped_cpp = normalizer.normalize(cpp_al).replace("/cpp/", "/<copy>/")
        section["tools"]["fpga_project_add_source"] = {
            "classification": ("identical" if flipped_py == flipped_cpp else "field_diff"),
            "python": {"ok": py_add.get("ok"), "isError": py_add.get("isError"),
                       "payload": payload_for_record(
                           normalizer.normalize(py_add.get("result")))},
            "cpp": {"ok": cpp_add.get("ok"), "isError": cpp_add.get("isError"),
                    "payload": payload_for_record(
                        normalizer.normalize(cpp_add.get("result")))},
            "alDiff": diff_json(flipped_py.split("\n"), flipped_cpp.split("\n"), "al")[:40],
            "sourceContained": "audit_probe_added.v" in flipped_cpp,
        }
        log("    fpga_project_add_source -> %s"
            % section["tools"]["fpga_project_add_source"]["classification"])

        # 3) project_update：TD 工程版本同步（会真的调 TD；失败也如实记录）
        py_up = call_tool(py_client, "fpga_project_update", {}, timeout=240)
        cpp_up = call_tool(cpp_client, "fpga_project_update", {}, timeout=120)
        # 两侧各跑在自己的副本目录（py/ 与 cpp/），路径必然不同；比较前把各自的
        # 副本根都折成同一个 <copy> token，否则"一致"会被副本名掩盖成假差异。
        py_view = AuditNormalizer({"repo": PY_ROOT, "cpp": CPP_ROOT, "tmp": copy_root,
                                   "copy": py_dir})
        cpp_view = AuditNormalizer({"repo": PY_ROOT, "cpp": CPP_ROOT, "tmp": copy_root,
                                    "copy": cpp_dir})
        py_norm = py_view.normalize(py_up.get("result")) if py_up.get("ok") else None
        cpp_norm = cpp_view.normalize(cpp_up.get("result")) if cpp_up.get("ok") else None
        py_norm = fold_copy_names(py_norm)
        cpp_norm = fold_copy_names(cpp_norm)
        diffs = diff_json(py_norm, cpp_norm) if (py_norm is not None and cpp_norm is not None) else []
        record = {
            "tool": "fpga_project_update",
            "classification": classify_pair(py_up, cpp_up, diffs),
            "diffCount": len(diffs),
            "diffs": diffs,
            "rawDiffs": [],
            "python": {"ok": py_up.get("ok"), "isError": py_up.get("isError"),
                       "seconds": py_up.get("seconds"), "error": py_up.get("error"),
                       "payload": payload_for_record(py_norm)},
            "cpp": {"ok": cpp_up.get("ok"), "isError": cpp_up.get("isError"),
                    "seconds": cpp_up.get("seconds"), "error": cpp_up.get("error"),
                    "payload": payload_for_record(cpp_norm)},
        }
        section["tools"]["fpga_project_update"] = record
        log("    fpga_project_update -> %s"
            % section["tools"]["fpga_project_update"]["classification"])
    finally:
        py_client.close()
        cpp_client.close()
        section["originalAlUnchanged"] = sha256_file(original_al) == original_hash
    return section


# --------------------------------------------------------------------------- CLI
def _run(argv, cwd, stdin_text="", timeout=120, env_extra=None):
    env = dict(os.environ, PYTHONIOENCODING="utf-8", PYTHONUNBUFFERED="1")
    if env_extra:
        env.update(env_extra)
    started = time.time()
    try:
        proc = subprocess.run(argv, cwd=cwd, input=stdin_text.encode("utf-8"),
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              timeout=timeout, env=env,
                              creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        return {"argv": argv, "exitCode": proc.returncode,
                "stdout": proc.stdout.decode("utf-8", "replace")[:4000],
                "stderr": proc.stderr.decode("utf-8", "replace")[:4000],
                "seconds": round(time.time() - started, 2)}
    except subprocess.TimeoutExpired:
        return {"argv": argv, "exitCode": None, "timeout": True,
                "seconds": round(time.time() - started, 2)}
    except OSError as exc:
        return {"argv": argv, "exitCode": None, "osError": str(exc)}


def section_cli(exe, log_dir, out):
    log("cli: 参数 / 默认日志 / --project 对照")
    work = os.path.join(log_dir, "cli-work")
    os.makedirs(work, exist_ok=True)
    py_env = {"PYTHONPATH": PY_ROOT}
    section = {"exe": exe, "exeSha256": sha256_file(exe), "probes": {}, "notes": []}

    def probe(key, argv, cwd, stdin_text="", env_extra=None, timeout=120):
        result = _run(argv, cwd, stdin_text=stdin_text, timeout=timeout, env_extra=env_extra)
        section["probes"][key] = result
        log("    %-34s exit=%s" % (key, result.get("exitCode")))
        return result

    # -- 帮助文本（参数集合是契约的一部分）
    probe("help_py_lsp", [sys.executable, "-m", "fpgalsp", "--help"], CPP_ROOT, env_extra=py_env)
    probe("help_py_mcp", [sys.executable, "-m", "fpgalsp.mcp", "--help"], CPP_ROOT,
          env_extra=py_env)
    probe("help_cpp_stdio", [exe, "--stdio", "--help"], CPP_ROOT)
    probe("help_cpp_mcp", [exe, "--mcp", "--help"], CPP_ROOT)

    # -- 未知参数（argparse 是 2；c++ 也是 2，但语义不同：一个提示 usage，一个直接拒启）
    probe("unknown_arg_py", [sys.executable, "-m", "fpgalsp", "--bogus"], CPP_ROOT,
          env_extra=py_env)
    probe("unknown_arg_cpp", [exe, "--stdio", "--bogus"], CPP_ROOT)

    # -- --project 支持面
    probe("project_py_lsp", [sys.executable, "-m", "fpgalsp", "--stdio", "--project",
                             r"%WORKSPACE%\CorpusAlpha\demo_project.al"], CPP_ROOT,
          stdin_text="", env_extra=py_env, timeout=90)
    probe("project_cpp_lsp", [exe, "--stdio", "--project",
                              r"%WORKSPACE%\CorpusAlpha\demo_project.al"], CPP_ROOT, timeout=90)
    probe("project_py_mcp", [sys.executable, "-m", "fpgalsp.mcp", "--root", work,
                             "--project", r"%WORKSPACE%\CorpusAlpha\demo_project.al"], CPP_ROOT,
          stdin_text='{"jsonrpc":"2.0","id":1,"method":"shutdown"}\n',
          env_extra=py_env, timeout=90)
    probe("project_cpp_mcp", [exe, "--mcp", "--root", work,
                              "--project", r"%WORKSPACE%\CorpusAlpha\demo_project.al"],
          CPP_ROOT, stdin_text='{"jsonrpc":"2.0","id":1,"method":"shutdown"}\n', timeout=90)

    # -- --verible（Python 有，C++ 无）
    probe("verible_py_mcp", [sys.executable, "-m", "fpgalsp.mcp", "--root", work,
                             "--verible", r"Y:\verible"], CPP_ROOT,
          stdin_text='{"jsonrpc":"2.0","id":1,"method":"shutdown"}\n',
          env_extra=py_env, timeout=90)
    probe("verible_cpp_mcp", [exe, "--mcp", "--root", work, "--verible", r"Y:\verible"],
          CPP_ROOT, stdin_text='{"jsonrpc":"2.0","id":1,"method":"shutdown"}\n', timeout=90)

    # -- 默认日志路径：Python LSP 写 <cwd>/.fpgalsp/logs/server.log；MCP 写 <root>/.fpgalsp/logs/mcp.log
    for key, argv, cwd, env_extra in (
        ("log_py_lsp", [sys.executable, "-m", "fpgalsp", "--stdio"],
         os.path.join(work, "py-lsp"), py_env),
        ("log_cpp_lsp", [exe, "--stdio"], os.path.join(work, "cpp-lsp"), None),
        ("log_py_mcp", [sys.executable, "-m", "fpgalsp.mcp"],
         os.path.join(work, "py-mcp"), py_env),
        ("log_cpp_mcp", [exe, "--mcp"], os.path.join(work, "cpp-mcp"), None),
    ):
        os.makedirs(cwd, exist_ok=True)
        stdin_text = ('{"jsonrpc":"2.0","id":1,"method":"shutdown"}\n'
                      if key.endswith("mcp") else "")
        result = probe(key, argv, cwd, stdin_text=stdin_text, env_extra=env_extra, timeout=90)
        created = []
        for base, _dirs, files in os.walk(cwd):
            for name in files:
                created.append(os.path.relpath(os.path.join(base, name), cwd).replace("\\", "/"))
        result["filesCreated"] = sorted(created)
        result["logFileExpected"] = (
            ".fpgalsp/logs/server.log" if key.endswith("lsp") else ".fpgalsp/logs/mcp.log")
        result["logFilePresent"] = result["logFileExpected"] in created
    return section


# --------------------------------------------------------------------------- domain
def section_domain(exe, log_dir, out):
    log("domain: reports / ADC / SDC / TD（MCP 层只读）")
    normalizer = make_normalizer()
    spec = dict(PROJECTS[0], root=prepare_project(PROJECTS[0]))
    section = {"project": spec["id"], "tools": {}}
    py_client = McpClient(WORK_CWD, spec["root"], os.path.join(log_dir, "domain-py.log"),
                          extra_args=["--allow-flow", "--allow-write"])
    cpp_client = NativeMcpClient(WORK_CWD, spec["root"],
                                 os.path.join(log_dir, "domain-cpp.log"), exe_path=exe,
                                 extra_args=["--allow-flow", "--allow-write"])
    try:
        variants = {
            "fpga_project_reports": [
                ("summary", {}),
                ("all_stage", {"stage": "all", "section": "all", "limit": 200}),
            ],
            "fpga_constraint_audit": [
                ("with_device", {"include_sdc": True, "include_device": True}),
                ("no_device", {"include_sdc": True, "include_device": False}),
            ],
            "fpga_constraints_status": [("default", {})],
            "fpga_td_info": [("default", {})],
        }
        for name, items in variants.items():
            records = {}
            for label, args in items:
                py_call = call_tool(py_client, name, args)
                cpp_call = call_tool(cpp_client, name, args)
                record = compare_payloads(py_call, cpp_call, normalizer, name)
                record["arguments"] = normalizer.normalize(args)
                records[label] = record
                log("    %-26s %-12s %-14s diffs=%d"
                    % (name, label, record["classification"], record["diffCount"]))
            section["tools"][name] = records if len(records) > 1 else records[
                next(iter(records))]

        # 真实 .adc/.sdc 的只读诊断：把 sources 收窄到约束层，对比两侧诊断条目
        for source in ("adc", "sdc"):
            py_call = call_tool(py_client, "fpga_diagnostics",
                                {"sources": [source], "limit": 500})
            cpp_call = call_tool(cpp_client, "fpga_diagnostics",
                                 {"sources": [source], "limit": 500})
            record = compare_payloads(py_call, cpp_call, normalizer,
                                      "fpga_diagnostics[%s]" % source)
            record["arguments"] = {"sources": [source], "limit": 500}
            section["tools"]["fpga_diagnostics[%s]" % source] = record
            log("    %-26s %-12s %-14s diffs=%d"
                % ("fpga_diagnostics", source, record["classification"],
                   record["diffCount"]))
        section["constraintFiles"] = {
            "adc": _find_ext(spec["root"], (".adc",)),
            "sdc": _find_ext(spec["root"], (".sdc",)),
        }
    finally:
        py_client.close()
        cpp_client.close()
    return section


def _find_ext(root, extensions):
    found = []
    for base, dirs, files in os.walk(root):
        dirs[:] = [name for name in dirs if name not in ("_Runs", ".fpgalsp")]
        for name in files:
            if name.lower().endswith(extensions):
                found.append(os.path.join(base, name))
    return sorted(found)[:10]


# --------------------------------------------------------------------------- 汇总
def summarize(raw):
    counts = {}
    details = []
    for pid, entry in (raw.get("tools", {}).get("projects") or {}).items():
        for tool, record in (entry.get("tools") or {}).items():
            kind = record.get("classification", "?")
            counts[kind] = counts.get(kind, 0) + 1
            if kind not in ("identical", "cpp_placeholder"):
                details.append({"project": pid, "tool": tool, "classification": kind,
                                "diffCount": record.get("diffCount")})
    ph_counts = {}
    for name, record in (raw.get("placeholders", {}).get("tools") or {}).items():
        variants = record if isinstance(record, dict) and "classification" not in record \
            else {"": record}
        for label, item in variants.items():
            if not isinstance(item, dict) or "classification" not in item:
                continue
            key = item["classification"]
            ph_counts[key] = ph_counts.get(key, 0) + 1
    domain_counts = {}
    for name, record in (raw.get("domain", {}).get("tools") or {}).items():
        variants = record if isinstance(record, dict) and "classification" not in record \
            else {"": record}
        for _label, item in variants.items():
            if not isinstance(item, dict) or "classification" not in item:
                continue
            domain_counts[item["classification"]] = domain_counts.get(
                item["classification"], 0) + 1
    write_counts = {}
    for name, record in (raw.get("write", {}).get("tools") or {}).items():
        if isinstance(record, dict) and "classification" in record:
            write_counts[record["classification"]] = write_counts.get(
                record["classification"], 0) + 1
    return {
        "readOnlyToolCalls": counts,
        "readOnlyNonIdentical": details,
        "placeholderCalls": ph_counts,
        "domainCalls": domain_counts,
        "writeCalls": write_counts,
        "schemaDefaultDifferent": (raw.get("schema", {}).get("default") or {}).get(
            "differentTools"),
        "schemaAllDifferent": (raw.get("schema", {}).get("allow_flow_write") or {}).get(
            "differentTools"),
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description="fpga-lsp-cpp 审计 A3：MCP/CLI 对照探针")
    parser.add_argument("--exe", default=os.path.join(CPP_ROOT, "build", "bin",
                                                      "fpgalsp-native.exe"))
    parser.add_argument("--out", default=os.path.join(CPP_ROOT, "doc", "audit", "2026-11-18",
                                                      "parity-mcp", "raw.json"))
    parser.add_argument("--log-dir", default="")
    parser.add_argument("--sections", default="schema,tools,placeholders,write,cli,domain")
    parser.add_argument("--projects", default="", help="逗号分隔，限定 tools 小节的工程")
    parser.add_argument("--allow-side-effects", action="store_true",
                        help="允许在真实工程上跑会写盘的调用（fpga_device_info / fpga_analysis_run）")
    parser.add_argument("--copy-projects", action="store_true",
                        help="把语料复制到 %%TEMP%% 后再跑（TD 类工具必须用这个）")
    parser.add_argument("--copy-root", default=os.path.join(tempfile.gettempdir(),
                                                            "pm-corpus"))
    args = parser.parse_args(argv)

    global ALLOW_SIDE_EFFECTS, COPY_PROJECTS, COPY_ROOT
    ALLOW_SIDE_EFFECTS = bool(args.allow_side_effects)
    COPY_PROJECTS = bool(args.copy_projects)
    COPY_ROOT = args.copy_root

    sections = [item.strip() for item in args.sections.split(",") if item.strip()]
    if not os.path.isfile(args.exe):
        print("找不到 %s" % args.exe, file=sys.stderr)
        return 2
    if not os.path.isdir(PY_ROOT):
        print("找不到 Python 参考实现 %s" % PY_ROOT, file=sys.stderr)
        return 2

    out_dir = os.path.dirname(os.path.abspath(args.out))
    os.makedirs(out_dir, exist_ok=True)
    log_dir = args.log_dir or os.path.join(tempfile.gettempdir(), "pm-logs")
    os.makedirs(log_dir, exist_ok=True)
    copy_root = os.path.join(tempfile.gettempdir(), "pm-copy-%d" % os.getpid())
    os.makedirs(copy_root, exist_ok=True)

    only_projects = [item.strip() for item in args.projects.split(",") if item.strip()] or None

    raw = {
        "meta": {
            "startedAt": time.strftime("%Y-%m-%dT%H:%M:%S"),
            "exe": args.exe,
            "exeSha256": sha256_file(args.exe),
            "pythonRoot": PY_ROOT,
            "pythonVersion": "%d.%d.%d" % sys.version_info[:3],
            "cppRoot": CPP_ROOT,
            "sections": sections,
            "logDir": log_dir,
            "copyRoot": copy_root,
            "allowSideEffects": ALLOW_SIDE_EFFECTS,
            "copyProjects": COPY_PROJECTS,
            "corpusCopyRoot": COPY_ROOT if COPY_PROJECTS else "",
        },
        "schema": {},
        "tools": {},
        "placeholders": {},
        "write": {},
        "cli": {},
        "domain": {},
    }

    try:
        if "schema" in sections:
            raw["schema"] = section_schema(args.exe, log_dir, raw)
        if "tools" in sections:
            raw["tools"] = section_tools(args.exe, log_dir, raw, only_projects)
        if "placeholders" in sections:
            raw["placeholders"] = section_placeholders(args.exe, log_dir, raw, copy_root)
        if "write" in sections:
            raw["write"] = section_write(args.exe, log_dir, raw, copy_root)
        if "cli" in sections:
            raw["cli"] = section_cli(args.exe, log_dir, raw)
        if "domain" in sections:
            raw["domain"] = section_domain(args.exe, log_dir, raw)
    finally:
        raw["meta"]["finishedAt"] = time.strftime("%Y-%m-%dT%H:%M:%S")
        raw["summary"] = summarize(raw)
        with open(args.out, "w", encoding="utf-8", newline="\n") as handle:
            json.dump(raw, handle, indent=2, ensure_ascii=False, sort_keys=True)
            handle.write("\n")
        log("wrote %s" % args.out)

    print(json.dumps(raw.get("summary"), indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
