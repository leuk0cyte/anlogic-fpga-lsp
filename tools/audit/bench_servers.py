#!/usr/bin/env python3
"""Reproducible startup / indexing / query benchmark: fpga-lsp (Python) vs fpga-lsp-cpp (native).

Everything here is read-only with respect to the products under test: no file inside
``Y:/fpga-lsp`` or ``Y:/fpga-lsp-cpp`` is written.  The only writes are

* the servers' own ``--log`` files, redirected into ``%TEMP%\\fpgalsp-doc\audit\\logs``;
* the JSON report given by ``--out``;
* the synthetic project trees used by the scaling test, under ``%TEMP%``.

Design notes (why the numbers are comparable):

* **Alternating sides.**  Rep *r* runs the two servers in opposite order, so a machine-load
  drift affects both sides symmetrically.  Every rep is a *fresh process* (fresh index).
* **Fixed cwd / project.**  Both servers start with cwd ``Y:/fpga-lsp`` (exactly what
  ``tests/golden/tools/goldenlib/client.py`` does) and are pointed at the same project.
* **No ``didOpen``.**  Opening a document makes the servers publish diagnostics, which can
  queue a TD check and spawn ``td_commands_prompt.exe`` — that would contaminate a latency
  measurement.  So every request is answered from disk, identically for both sides.
  (A real editor session does open documents; the file-read part of that is measured
  separately by the closing "open + diagnostics" probe.)
* **TD is excluded.**  ``fpgaLsp.tdCheckOnSave=false`` and no ``fpgalsp.check`` command is
  ever sent; residual ``td_commands_prompt.exe`` processes are counted before/after.
* **Python startup includes imports.**  ``python -m fpgalsp`` pays interpreter + import cost;
  ``startup_breakdown`` quantifies it separately so the comparison is not misread.

Usage
-----
    python tools/audit/bench_servers.py \
        --iterations 30 --repeats 3 --project %WORKSPACE%/CorpusAlpha \
        --out doc/audit/2026-11-18/performance/bench.json
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wintypes
import hashlib
import json
import os
import platform
import queue
import statistics
import subprocess
import sys
import threading
import time
import traceback

# ---------------------------------------------------------------------------
#  constants / paths
# ---------------------------------------------------------------------------

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_CPP = os.path.dirname(os.path.dirname(HERE))          # Y:\fpga-lsp-cpp
REPO_PY = os.environ.get("FPGALSP_PY_REPO", r"Y:/fpga-lsp")
GOLDEN_TOOLS = os.path.join(REPO_CPP, "tests", "golden", "tools")
TMP_ROOT = os.path.join(os.environ.get("TEMP", r"C:\Windows\Temp"), "fpgalsp-audit")
LOG_DIR = os.path.join(TMP_ROOT, "logs")
PERF_DIR = os.path.join(TMP_ROOT, "perf")

sys.path.insert(0, GOLDEN_TOOLS)

from goldenlib.client import (  # noqa: E402
    LspClient,
    McpClient,
    NativeLspClient,
    NativeMcpClient,
    TransportError,
    default_native_exe,
    path_to_uri,
)

SIDES = ("python", "native")

# LSP methods measured with warmup + N iterations.  ``params`` is filled per project.
LSP_METHODS = [
    "textDocument/documentSymbol",
    "textDocument/hover",
    "textDocument/definition",
    "textDocument/references",
    "textDocument/completion",
    "textDocument/inlayHint",
    "workspace/symbol",          # not required by the task list, but cheap and informative
    "fpgaLsp/moduleTree",
    "fpgaLsp/reports",
]

# Probe-only methods: measured once per session; if the native side answers with an empty
# payload it is recorded as "unavailable" and excluded from the comparison table.
CAPABILITY_PROBES = [
    "textDocument/codeAction",
    "textDocument/foldingRange",
    "textDocument/semanticTokens/full",
]

MCP_TOOLS = [
    ("fpga_project_info", lambda al: {"project": al}),
    ("fpga_project_structure", lambda al: {"project": al, "include_ports": False}),
    ("fpga_project_reports", lambda al: {"project": al}),
    ("fpga_search_text", lambda al: {"project": al, "query": "clk_200M", "limit": 100}),
]

# ---------------------------------------------------------------------------
#  Windows process memory (ctypes, no third-party dependency)
# ---------------------------------------------------------------------------

PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
PROCESS_VM_READ = 0x0010


class PROCESS_MEMORY_COUNTERS_EX(ctypes.Structure):
    _fields_ = [
        ("cb", wintypes.DWORD),
        ("PageFaultCount", wintypes.DWORD),
        ("PeakWorkingSetSize", ctypes.c_size_t),
        ("WorkingSetSize", ctypes.c_size_t),
        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
        ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
        ("PagefileUsage", ctypes.c_size_t),
        ("PeakPagefileUsage", ctypes.c_size_t),
        ("PrivateUsage", ctypes.c_size_t),
    ]


_kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
_psapi = ctypes.WinDLL("psapi", use_last_error=True)
_kernel32.OpenProcess.restype = wintypes.HANDLE
_kernel32.CloseHandle.restype = wintypes.BOOL


def read_memory_counters(pid: int):
    """Return ``(working_set, peak_working_set)`` in bytes, or ``None`` if the process is gone."""
    handle = _kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, False, pid)
    if not handle:
        return None
    try:
        counters = PROCESS_MEMORY_COUNTERS_EX()
        counters.cb = ctypes.sizeof(PROCESS_MEMORY_COUNTERS_EX)
        if not _psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
            return None
        return int(counters.WorkingSetSize), int(counters.PeakWorkingSetSize)
    finally:
        _kernel32.CloseHandle(handle)


class MemorySampler:
    """Poll one pid in a background thread and keep the high-water mark."""

    def __init__(self, pid: int, interval: float = 0.02):
        self.pid = pid
        self.interval = interval
        self.peak_working_set = 0
        self.peak_counters = 0
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._loop, daemon=True)

    def __enter__(self):
        self._thread.start()
        return self

    def __exit__(self, *exc):
        self._stop.set()
        self._thread.join(timeout=2.0)
        self._sample()          # final read before the pid disappears
        return False

    def _sample(self):
        got = read_memory_counters(self.pid)
        if got is None:
            return
        working_set, peak = got
        self.peak_working_set = max(self.peak_working_set, working_set)
        self.peak_counters = max(self.peak_counters, peak)

    def _loop(self):
        while not self._stop.is_set():
            self._sample()
            self._stop.wait(self.interval)

    def as_dict(self):
        return {
            "peak_working_set_mb": round(self.peak_working_set / (1024 * 1024), 2),
            "peak_counters_working_set_mb": round(self.peak_counters / (1024 * 1024), 2),
        }


def process_alive(pid: int) -> bool:
    handle = _kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        return False
    try:
        code = wintypes.DWORD()
        if not _kernel32.GetExitCodeProcess(handle, ctypes.byref(code)):
            return True
        return code.value == 259  # STILL_ACTIVE
    finally:
        _kernel32.CloseHandle(handle)


def list_processes(image_name: str):
    """PIDs of ``image_name`` (e.g. ``td_commands_prompt.exe``) via tasklist."""
    try:
        out = subprocess.run(
            ["tasklist", "/FO", "CSV", "/NH", "/FI", "IMAGENAME eq " + image_name],
            capture_output=True, text=True, timeout=60,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        ).stdout
    except Exception:
        return None
    pids = set()
    for line in out.splitlines():
        parts = [p.strip('" ') for p in line.split('","')]
        if len(parts) >= 2 and parts[0].strip('"').lower() == image_name.lower():
            try:
                pids.add(int(parts[1]))
            except ValueError:
                pass
    return pids


class SystemLoad:
    """CPU busy fraction between two samples (GetSystemTimes)."""

    def __init__(self):
        self.start = self._read()
        self.end = None

    @staticmethod
    def _read():
        idle = wintypes.FILETIME()
        kernel = wintypes.FILETIME()
        user = wintypes.FILETIME()
        if not _kernel32.GetSystemTimes(ctypes.byref(idle), ctypes.byref(kernel), ctypes.byref(user)):
            return None

        def as_int(ft):
            return (ft.dwHighDateTime << 32) | ft.dwLowDateTime

        return as_int(idle), as_int(kernel), as_int(user)

    def stop(self):
        self.end = self._read()

    def busy_fraction(self):
        if not self.start or not self.end:
            return None
        di = self.end[0] - self.start[0]
        dk = self.end[1] - self.start[1]
        du = self.end[2] - self.start[2]
        total = dk + du
        if total <= 0:
            return None
        return round(max(0.0, min(1.0, 1.0 - di / total)), 3)


# ---------------------------------------------------------------------------
#  statistics helpers
# ---------------------------------------------------------------------------


def percentile(sorted_values, q):
    """Linear-interpolated percentile on an already sorted list."""
    if not sorted_values:
        return None
    if len(sorted_values) == 1:
        return sorted_values[0]
    pos = (len(sorted_values) - 1) * q
    lo = int(pos)
    hi = min(lo + 1, len(sorted_values) - 1)
    frac = pos - lo
    return sorted_values[lo] * (1.0 - frac) + sorted_values[hi] * frac


def summarize(samples_ms, keep_samples=True):
    if not samples_ms:
        return {"n": 0}
    ordered = sorted(samples_ms)
    out = {
        "n": len(ordered),
        "min": round(ordered[0], 3),
        "p50": round(percentile(ordered, 0.50), 3),
        "p95": round(percentile(ordered, 0.95), 3),
        "max": round(ordered[-1], 3),
        "mean": round(statistics.fmean(ordered), 3),
        "stdev": round(statistics.pstdev(ordered), 3) if len(ordered) > 1 else 0.0,
    }
    if keep_samples:
        out["samples_ms"] = [round(v, 3) for v in samples_ms]
    return out


def ratio(py_value, native_value):
    if py_value is None or native_value in (None, 0):
        return None
    return round(py_value / native_value, 2)


# ---------------------------------------------------------------------------
#  project helpers
# ---------------------------------------------------------------------------


def read_text(path):
    with open(path, encoding="utf-8", errors="replace") as handle:
        return handle.read()


def find_position(lines, needle, occur=0, offset=0):
    """(line, character) of the ``occur``-th occurrence of ``needle``, +offset columns."""
    seen = 0
    for index, line in enumerate(lines):
        start = 0
        while True:
            col = line.find(needle, start)
            if col < 0:
                break
            if seen == occur:
                return index, col + offset
            seen += 1
            start = col + 1
    raise SystemExit("bench: token %r not found (occurrence %d)" % (needle, occur))


def find_module_decl(lines, name):
    """(line, column-of-name) of the line whose stripped text starts with ``module <name>``.

    Searching for the bare identifier would hit the generated file's header comment.
    """
    needle = "module " + name
    for index, line in enumerate(lines):
        stripped = line.lstrip()
        if not stripped.startswith(needle):
            continue
        after = stripped[len(needle):]
        if after and (after[0].isalnum() or after[0] == "_"):
            continue          # synth_m00010 不是 synth_m0001
        return index, len(line) - len(stripped) + len("module ")
    raise SystemExit("bench: module declaration for %r not found" % name)


def lsp_params(project, method, anchor, capability_probe=False):
    """Build the request params for one method.  ``anchor`` holds the pre-computed positions."""
    uri = anchor["uri"]
    if method == "textDocument/documentSymbol":
        return {"textDocument": {"uri": uri}}
    if method in ("textDocument/hover", "textDocument/definition"):
        return {"textDocument": {"uri": uri}, "position": anchor["hover_pos"]}
    if method == "textDocument/references":
        return {
            "textDocument": {"uri": uri},
            "position": anchor["ref_pos"],
            "context": {"includeDeclaration": True},
        }
    if method == "textDocument/completion":
        return {"textDocument": {"uri": uri}, "position": anchor["completion_pos"]}
    if method == "textDocument/inlayHint":
        return {"textDocument": {"uri": uri}, "range": anchor["whole_range"]}
    if method == "workspace/symbol":
        return {"query": "pwm"}
    if method == "textDocument/codeAction":
        return {
            "textDocument": {"uri": uri},
            "range": {"start": anchor["ref_pos"], "end": anchor["ref_pos"]},
            "context": {"diagnostics": []},
        }
    if method == "textDocument/foldingRange":
        return {"textDocument": {"uri": uri}}
    if method == "textDocument/semanticTokens/full":
        return {"textDocument": {"uri": uri}}
    if method in ("fpgaLsp/moduleTree", "fpgaLsp/reports"):
        return {"uri": uri}
    raise ValueError("no params builder for " + method)


def build_anchor(top_path, text_lines):
    """Stable, human-meaningful request positions inside ``src/top.v``."""
    line_hover, col_hover = find_position(text_lines, "clk_200M")
    # ``trans trans(`` — module name of the first instantiation in the file.
    ref_line, ref_col = find_position(text_lines, "trans trans(")
    comp_line, comp_col = find_position(text_lines, "pwm_gen #(")
    return {
        "uri": path_to_uri(top_path),
        "hover_pos": {"line": line_hover, "character": col_hover + 2},
        "ref_pos": {"line": ref_line, "character": ref_col + 1},
        "completion_pos": {"line": comp_line, "character": comp_col + 7},
        "whole_range": {"start": {"line": 0, "character": 0},
                        "end": {"line": max(0, len(text_lines) - 1), "character": 0}},
    }


# ---------------------------------------------------------------------------
#  LSP session
# ---------------------------------------------------------------------------


def make_lsp_client(side, log_name):
    log_path = os.path.join(LOG_DIR, log_name)
    if side == "python":
        return LspClient(REPO_PY, log_path)
    return NativeLspClient(REPO_PY, log_path)


def make_mcp_client(side, root_path, log_name):
    log_path = os.path.join(LOG_DIR, log_name)
    if side == "python":
        return McpClient(REPO_PY, root_path, log_path)
    return NativeMcpClient(REPO_PY, root_path, log_path)


def run_lsp_session(side, rep, project, anchor, iterations, warmups):
    """One fresh process: cold initialize, cold index, then warmup + timed loop."""
    session = {"side": side, "rep": rep, "errors": {}}
    t_spawn = time.perf_counter()
    client = make_lsp_client(side, "lsp-%s-r%d.log" % (side, rep))
    spawn_ms = (time.perf_counter() - t_spawn) * 1000.0
    sampler = MemorySampler(client.proc.pid)
    try:
        with sampler:
            t_init = time.perf_counter()
            client.initialize(project, timeout=600.0)
            init_ms = (time.perf_counter() - t_init) * 1000.0

            # 1) cold index: the *first* index-touching request in this process has to build the
            #    whole HDL index.  This must run before the capability probes: some of those
            #    probes (codeAction / semanticTokens on the Python side) go through the same
            #    index cache and would otherwise hide the build cost on one side only.
            t0 = time.perf_counter()
            symbols = client.request("textDocument/documentSymbol",
                                     lsp_params(project, "textDocument/documentSymbol", anchor),
                                     timeout=900.0)
            session["cold_index_ms"] = round((time.perf_counter() - t0) * 1000.0, 3)
            session["symbol_count"] = len(symbols or [])

            # 2) capability probes (recorded as available / unavailable; not in the timed table)
            capabilities = {}
            for method in CAPABILITY_PROBES:
                try:
                    t0 = time.perf_counter()
                    result = client.request(method, lsp_params(project, method, anchor),
                                            timeout=300.0)
                    elapsed = (time.perf_counter() - t0) * 1000.0
                    payload = json.dumps(result, ensure_ascii=False)
                    nonempty = bool(result)
                    if method == "textDocument/semanticTokens/full" and isinstance(result, dict):
                        nonempty = bool(result.get("data"))
                    capabilities[method] = {
                        "available": nonempty,
                        "payload_chars": len(payload),
                        "first_call_ms": round(elapsed, 3),
                    }
                except Exception as exc:  # pragma: no cover - defensive
                    capabilities[method] = {"available": False, "error": str(exc)}
            session["capabilities"] = capabilities

            # 3) warmups for every measured method
            for method in LSP_METHODS:
                params = lsp_params(project, method, anchor)
                for _ in range(warmups):
                    client.request(method, params, timeout=300.0)

            # 4) timed loop: method-major, ``iterations`` samples each
            timings = {}
            payload_sizes = {}
            for method in LSP_METHODS:
                params = lsp_params(project, method, anchor)
                samples = []
                for _ in range(iterations):
                    t0 = time.perf_counter()
                    result = client.request(method, params, timeout=300.0)
                    samples.append((time.perf_counter() - t0) * 1000.0)
                timings[method] = samples
                payload_sizes[method] = len(json.dumps(result, ensure_ascii=False))
            session["timings"] = timings
            session["payload_chars"] = payload_sizes
            session["spawn_ms"] = round(spawn_ms, 3)
            session["initialize_ms"] = round(init_ms, 3)
            session["pid"] = client.proc.pid
    except Exception:
        session["fatal"] = traceback.format_exc()
    finally:
        try:
            client.close()
        except Exception:
            pass
    session["memory"] = sampler.as_dict()
    session["alive_after_close"] = process_alive(client.proc.pid)
    session["pid"] = client.proc.pid
    return session


def run_cold_start(side, samples_n, project):
    """spawn -> initialize round-trip, ``samples_n`` fresh processes."""
    samples = []
    spawn_only = []
    pids = []
    for _ in range(samples_n):
        t_spawn = time.perf_counter()
        client = make_lsp_client(side, "coldstart-%s.log" % side)
        spawn_only.append((time.perf_counter() - t_spawn) * 1000.0)
        try:
            t0 = time.perf_counter()
            client.initialize(project, timeout=600.0)
            samples.append((time.perf_counter() - t0) * 1000.0)
        finally:
            pids.append(client.proc.pid)
            try:
                client.close()
            except Exception:
                pass
    return {
        "spawn_ms": summarize(spawn_only),
        "initialize_ms": summarize(samples),
        "total_ms": summarize([a + b for a, b in zip(spawn_only, samples)]),
        "alive_after_close": [pid for pid in pids if process_alive(pid)],
    }


# ---------------------------------------------------------------------------
#  MCP session
# ---------------------------------------------------------------------------


def run_mcp_session(side, rep, project, al_path, iterations, warmups):
    session = {"side": side, "rep": rep, "errors": {}}
    t_spawn = time.perf_counter()
    client = make_mcp_client(side, project, "mcp-%s-r%d.log" % (side, rep))
    handshake_ms = (time.perf_counter() - t_spawn) * 1000.0
    sampler = MemorySampler(client.proc.pid)
    try:
        with sampler:
            timings = {}
            payload_sizes = {}
            for tool, build in MCP_TOOLS:
                args = build(al_path)
                for _ in range(warmups):
                    client.call_tool(tool, args, timeout=600.0)
                samples = []
                for _ in range(iterations):
                    t0 = time.perf_counter()
                    result, _is_error = client.call_tool(tool, args, timeout=600.0)
                    samples.append((time.perf_counter() - t0) * 1000.0)
                timings[tool] = samples
                payload_sizes[tool] = len(json.dumps(result, ensure_ascii=False))
            session["timings"] = timings
            session["payload_chars"] = payload_sizes
            session["handshake_ms"] = round(handshake_ms, 3)
    except Exception:
        session["fatal"] = traceback.format_exc()
    finally:
        try:
            client.close()
        except Exception:
            pass
    session["memory"] = sampler.as_dict()
    session["alive_after_close"] = process_alive(client.proc.pid)
    session["pid"] = client.proc.pid
    return session


# ---------------------------------------------------------------------------
#  scaling test (synthetic project, %TEMP% only)
# ---------------------------------------------------------------------------

SYNTH_FILE_TEMPLATE = """\
// synthetic benchmark module {name} (generated by tools/audit/bench_servers.py)
module {name} (
  input             clk,
  input             rst_n,
  input      [7:0]  din,
  output reg [7:0]  dout,
  output reg        valid
);
  reg [7:0] acc;
{instances}
  always @(posedge clk) begin
    if (!rst_n) begin
      acc   <= 8'd0;
      dout  <= 8'd0;
      valid <= 1'b0;
    end else begin
      acc   <= acc + din;
      dout  <= acc ^ din;
      valid <= 1'b1;
    end
  end
endmodule
"""


def synth_content(index, total):
    """Deterministic Verilog for module ``index`` of ``total``.

    Module 1 is instantiated by every module from index 10 upwards, so
    ``textDocument/references`` on it has a reference count that grows linearly with ``total``.
    """
    if index == 0:
        name = "synth_top"
        children = ["synth_m%04d" % i for i in range(1, min(6, total))]
        instances = "\n".join(
            "  %s u_inst_%d (.clk(clk), .rst_n(rst_n), .din(din), .dout(), .valid());"
            % (child, n) for n, child in enumerate(children))
        return SYNTH_FILE_TEMPLATE.format(name=name, instances=instances)
    name = "synth_m%04d" % index
    instances = []
    if index >= 10:
        instances.append("  synth_m0001 u_ref (.clk(clk), .rst_n(rst_n), .din(din), "
                         ".dout(), .valid());")
    if index + 1 < total:
        instances.append("  synth_m%04d u_next (.clk(clk), .rst_n(rst_n), .din(din), "
                         ".dout(), .valid());" % (index + 1))
    return SYNTH_FILE_TEMPLATE.format(name=name, instances="\n".join(instances))


SYNTH_AL_HEAD = """\
<?xml version="1.0" encoding="UTF-8"?>
<Project Version="3" Minor="2" Path="{path}">
    <Project_Created_Time></Project_Created_Time>
    <TD_Version>6.2.3.210850</TD_Version>
    <Name>{name}</Name>
    <HardWare>
        <Family>EF2</Family>
        <Device>EF2L45LG144B</Device>
        <Speed></Speed>
    </HardWare>
    <Source_Files>
        <Verilog>
"""

SYNTH_AL_FILE = """\
            <File Path="src/{base}">
                <FileInfo>
                    <Attr Name="UsedInSyn" Val="true"/>
                    <Attr Name="UsedInP&amp;R" Val="true"/>
                    <Attr Name="BelongTo" Val="design_1"/>
                    <Attr Name="CompileOrder" Val="{order}"/>
                </FileInfo>
            </File>
"""

SYNTH_AL_TAIL = """\
        </Verilog>
    </Source_Files>
    <TOP_MODULE>
        <LABEL>synth_top</LABEL>
        <MODULE>synth_top</MODULE>
    </TOP_MODULE>
    <Device_Settings>
    </Device_Settings>
</Project>
"""


def ensure_synth_project(size, root=PERF_DIR, dir_name=None, project_name=None):
    """Create ``root/<dir_name>`` with ``size`` .v files plus a matching .al.  Deterministic.

    ``dir_name``/``project_name`` 默认 ``scaleNNN``/``synthNNN``；Phase 2 的报告工程用
    ``reportNNN``，以便和纯符号基准工程分开（否则带上 ``_Runs`` 的 10 MB 报告会污染 warm 基准）。
    """
    dir_name = dir_name or ("scale%03d" % size)
    project_name = project_name or ("synth%03d" % size)
    project_dir = os.path.join(root, dir_name)
    src_dir = os.path.join(project_dir, "src")
    al_path = os.path.join(project_dir, project_name + ".al")
    os.makedirs(src_dir, exist_ok=True)
    files = []
    for index in range(size):
        base = "synth_top.v" if index == 0 else "synth_m%04d.v" % index
        path = os.path.join(src_dir, base)
        text = synth_content(index, size)
        files.append((path, base, text))
    # always rewrite: cheap, and guarantees byte-identical input for every run
    for path, _base, text in files:
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
    chunks = [SYNTH_AL_HEAD.format(path=project_dir.replace("\\", "/"), name=project_name)]
    for order, (_path, base, _text) in enumerate(files, start=1):
        chunks.append(SYNTH_AL_FILE.format(base=base, order=order))
    chunks.append(SYNTH_AL_TAIL)
    with open(al_path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("".join(chunks))
    total_bytes = sum(os.path.getsize(path) for path, _b, _t in files)
    # Warm the OS page cache: the synthetic tree is written immediately before it is measured,
    # and a first-touch read costs ~1 ms/file on this machine (AV filter + cold cache).  Without
    # this the side that runs first looks 3-4x slower for reasons that have nothing to do with
    # the server (measured: Python cold index 122 ms -> 26 ms on the 2nd pass at N=50,
    # 295 ms -> 100 ms at N=200; the native side was stable because Python had already warmed it).
    for path, _base, _text in files:
        with open(path, "rb") as handle:
            handle.read()
    return {
        "project_dir": project_dir,
        "al_path": al_path,
        "files": size,
        "bytes": total_bytes,
        "cache_warmed": True,
        "top_file": os.path.join(src_dir, "synth_top.v"),
        "ref_file": os.path.join(src_dir, "synth_m0001.v"),
    }


def run_scale_side(side, spec, iterations, warmups):
    top_path = spec["top_file"]
    ref_path = spec["ref_file"]
    ref_lines = read_text(ref_path).splitlines()
    # Anchor on the *module declaration*, not on the first textual occurrence (which sits in
    # the generated file's header comment and would make the request a no-op).
    ref_name_line, ref_name_col = find_module_decl(ref_lines, "synth_m0001")
    anchor = {
        "uri": path_to_uri(top_path),
        "symbol_uri": path_to_uri(ref_path),
        "ref_pos": {"line": ref_name_line, "character": ref_name_col + 1},
        "query": "synth_m",
    }
    result = {"side": side}
    client = make_lsp_client(side, "scale-%s-%d.log" % (side, spec["files"]))
    sampler = MemorySampler(client.proc.pid)
    try:
        with sampler:
            t0 = time.perf_counter()
            client.initialize(spec["project_dir"], timeout=900.0)
            result["initialize_ms"] = round((time.perf_counter() - t0) * 1000.0, 3)

            t0 = time.perf_counter()
            cold = client.request("textDocument/documentSymbol",
                                  {"textDocument": {"uri": path_to_uri(top_path)}},
                                  timeout=1800.0)
            result["cold_index_ms"] = round((time.perf_counter() - t0) * 1000.0, 3)
            result["top_symbols"] = len(cold or [])

            work = [
                ("documentSymbol(file)", {"textDocument": {"uri": path_to_uri(ref_path)}}),
                ("workspace/symbol", {"query": anchor["query"]}),
                ("references(module m0001)", {
                    "textDocument": {"uri": path_to_uri(ref_path)},
                    "position": anchor["ref_pos"],
                    "context": {"includeDeclaration": True},
                }),
            ]
            out = {}
            for label, params in work:
                method = "textDocument/documentSymbol" if label.startswith("documentSymbol") \
                    else ("textDocument/references" if label.startswith("references")
                          else "workspace/symbol")
                for _ in range(warmups):
                    client.request(method, params, timeout=1800.0)
                samples = []
                for _ in range(iterations):
                    t0 = time.perf_counter()
                    payload = client.request(method, params, timeout=1800.0)
                    samples.append((time.perf_counter() - t0) * 1000.0)
                summary = summarize(samples)
                summary["result_items"] = len(payload) if isinstance(payload, list) else None
                out[label] = summary
            result["methods"] = out
    except Exception:
        result["fatal"] = traceback.format_exc()
    finally:
        try:
            client.close()
        except Exception:
            pass
    result["memory"] = sampler.as_dict()
    result["alive_after_close"] = process_alive(client.proc.pid)
    return result


# ---------------------------------------------------------------------------
#  python startup breakdown
# ---------------------------------------------------------------------------


def measure_python_startup(samples=5):
    """Bare interpreter vs interpreter+imports: shows how much of the cold start is imports.

    The subprocesses must run with cwd = the *Python* reference checkout, otherwise
    ``import fpgalsp.server`` fails (and a failed import still costs ~270 ms here because the
    import machinery walks sys.path) — which would silently fake a big "import cost".
    """

    def timed(argv):
        values = []
        for _ in range(samples):
            t0 = time.perf_counter()
            subprocess.run(argv, cwd=REPO_PY, capture_output=True, timeout=300,
                           creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            values.append((time.perf_counter() - t0) * 1000.0)
        return summarize(values, keep_samples=False)

    bare = timed([sys.executable, "-c", "pass"])
    server = timed([sys.executable, "-c", "import fpgalsp.server"])
    top_imports = []
    ok = False
    try:
        proc = subprocess.run(
            [sys.executable, "-X", "importtime", "-c",
             "import fpgalsp.server; print('IMPORT-OK')"],
            cwd=REPO_PY, capture_output=True, text=True, timeout=300,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        ok = "IMPORT-OK" in (proc.stdout or "")
        rows = []
        for line in proc.stderr.splitlines():
            if not line.startswith("import time:"):
                continue
            body = line[len("import time:"):].strip()
            parts = body.split("|")
            if len(parts) < 2:
                continue
            try:
                cost = int(parts[0].strip())
                cumulative = int(parts[1].strip()) if parts[1].strip().isdigit() else cost
            except ValueError:
                continue
            name = parts[-1].strip()
            rows.append({"module": name, "self_us": cost, "cumulative_us": cumulative})
        rows.sort(key=lambda r: r["self_us"], reverse=True)
        top_imports = rows[:12]
    except Exception:
        top_imports = []
    return {"bare_interpreter_ms": bare, "import_fpgalsp_server_ms": server,
            "import_verified": ok, "top_self_imports": top_imports}


# ---------------------------------------------------------------------------
#  environment record
# ---------------------------------------------------------------------------


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def git_head(repo):
    try:
        out = subprocess.run(["git", "-C", repo, "rev-parse", "HEAD"],
                             capture_output=True, text=True, timeout=60,
                             creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        return out.stdout.strip() or None
    except Exception:
        return None


def environment_snapshot(project):
    # 记录**实际被使用的** exe：客户端走 goldenlib 的 default_native_exe()，
    # 它优先看 FPGALSP_NATIVE_EXE（before/after 对比就是靠这个环境变量切二进制）。
    try:
        exe = default_native_exe()
    except FileNotFoundError:
        exe = os.path.join(REPO_CPP, "build", "bin", "fpgalsp-native.exe")
    info = {
        "captured_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "os": platform.platform(),
        "cpu": platform.processor(),
        "cpu_count": os.cpu_count(),
        "python": sys.version.split()[0],
        "python_executable": sys.executable,
        "repo_python": REPO_PY,
        "repo_cpp": REPO_CPP,
        "git_head_cpp": git_head(REPO_CPP),
        "git_head_python": git_head(REPO_PY),
        "project": project,
        "native_exe": exe,
        "native_exe_selected_by": ("FPGALSP_NATIVE_EXE"
                                   if os.environ.get("FPGALSP_NATIVE_EXE") else "default"),
        "native_exe_bytes": os.path.getsize(exe) if os.path.isfile(exe) else None,
        "native_exe_sha256": sha256_of(exe) if os.path.isfile(exe) else None,
    }
    return info


# ---------------------------------------------------------------------------
#  main
# ---------------------------------------------------------------------------


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--project", default=r"%WORKSPACE%/CorpusAlpha",
                        help="real .al project the LSP clients are pointed at")
    parser.add_argument("--iterations", type=int, default=30, help="timed iterations per method")
    parser.add_argument("--repeats", type=int, default=3, help="fresh-process rounds per side")
    parser.add_argument("--warmups", type=int, default=3, help="warmup calls per method")
    parser.add_argument("--cold-start-samples", type=int, default=10)
    parser.add_argument("--out", default=None, help="bench.json path (default: stdout only)")
    parser.add_argument("--no-scale", action="store_true", help="skip the synthetic scaling test")
    parser.add_argument("--sizes", default="50,100,200", help="synthetic project sizes")
    parser.add_argument("--scale-iterations", type=int, default=15)
    parser.add_argument("--no-memory", action="store_true", help="skip MCP memory/memory probes")
    parser.add_argument("--only", default="", help="comma separated: cold,lsp,mcp,scale,env,phase2")
    # ---- Phase 2 / Gate 0 ----
    parser.add_argument("--phase2-sizes", default="21,200,1000",
                        help="Phase 2 合成工程规模（默认 21,200,1000）")
    parser.add_argument("--phase2-iterations", type=int, default=20,
                        help="Phase 2 warm 每方法迭代次数")
    parser.add_argument("--phase2-edits", type=int, default=5,
                        help="didChange 连续输入次数")
    parser.add_argument("--phase2-fast", type=int, default=10,
                        help="队头阻塞测试里的并发快请求数")
    parser.add_argument("--phase2-report-mb", type=float, default=10.0,
                        help="合成 timing 报告大小（MB）")
    parser.add_argument("--phase2-repeats", type=int, default=10,
                        help="报告重复请求次数")
    args = parser.parse_args(argv)

    only = {item.strip() for item in args.only.split(",") if item.strip()}
    def want(stage):
        return not only or stage in only

    os.makedirs(LOG_DIR, exist_ok=True)
    os.makedirs(PERF_DIR, exist_ok=True)

    project = os.path.abspath(args.project)
    top_path = os.path.join(project, "src", "top.v")
    al_path = os.path.join(project, "demo_project.al")
    if not os.path.isfile(top_path):
        raise SystemExit("bench: %s not found" % top_path)
    if not os.path.isfile(al_path):
        raise SystemExit("bench: %s not found" % al_path)

    text_lines = read_text(top_path).splitlines()
    anchor = build_anchor(top_path, text_lines)

    # hashes of the real project's sources, to prove the bench did not modify them
    before_hashes = {}
    for root, _dirs, files in os.walk(project):
        if os.sep + ".fpgalsp" in root:
            continue
        for name in files:
            if name.lower().endswith((".v", ".sv", ".al", ".sdc")):
                path = os.path.join(root, name)
                before_hashes[path] = sha256_of(path)

    exe_at_start = native_exe_info()
    load = SystemLoad()
    report = {
        "meta": {
            "tool": "tools/audit/bench_servers.py",
            "iterations": args.iterations,
            "repeats": args.repeats,
            "warmups": args.warmups,
            "cold_start_samples": args.cold_start_samples,
            "order_policy": "rep 1 python->native, rep 2 native->python, rep 3 python->native",
            "cwd_both_servers": REPO_PY,
            "did_send_didOpen": False,
            "td_excluded": True,
        },
        "environment": environment_snapshot(project),
        "native_exe_at_start": exe_at_start,
    }

    td_before = list_processes("td_commands_prompt.exe")
    report["residual"] = {"td_commands_prompt_before": sorted(td_before) if td_before is not None else None}

    # ---- cold start ---------------------------------------------------------
    if want("cold"):
        sys.stderr.write("[bench] cold start (%d samples/side)\n" % args.cold_start_samples)
        report["cold_start"] = {}
        for index in range(args.cold_start_samples):
            order = SIDES if index % 2 == 0 else tuple(reversed(SIDES))
            for side in order:
                sample = run_cold_start(side, 1, project)
                bucket = report["cold_start"].setdefault(side, {"spawn_ms": [], "initialize_ms": [],
                                                                "total_ms": [], "alive_after_close": []})
                bucket["spawn_ms"].append(sample["spawn_ms"]["samples_ms"][0])
                bucket["initialize_ms"].append(sample["initialize_ms"]["samples_ms"][0])
                bucket["total_ms"].append(sample["total_ms"]["samples_ms"][0])
                bucket["alive_after_close"].extend(sample["alive_after_close"])
        for side in list(report["cold_start"]):
            bucket = report["cold_start"][side]
            alive = bucket["alive_after_close"]
            for key in ("spawn_ms", "initialize_ms", "total_ms"):
                bucket[key] = summarize(bucket[key])
            bucket["alive_after_close"] = alive

    # ---- startup breakdown --------------------------------------------------
    if want("env"):
        sys.stderr.write("[bench] python startup breakdown\n")
        report["startup_breakdown"] = measure_python_startup()

    # ---- LSP rounds ---------------------------------------------------------
    if want("lsp"):
        lsp_rounds = {side: [] for side in SIDES}
        for rep in range(1, args.repeats + 1):
            order = SIDES if rep % 2 == 1 else tuple(reversed(SIDES))
            for side in order:
                sys.stderr.write("[bench] lsp side=%s rep=%d\n" % (side, rep))
                lsp_rounds[side].append(run_lsp_session(
                    side, rep, project, anchor, args.iterations, args.warmups))
        report["lsp_rounds"] = lsp_rounds
        report["lsp"] = aggregate_rounds(lsp_rounds, LSP_METHODS, keep_samples=True)
        report["cold_index"] = {
            side: summarize([r["cold_index_ms"] for r in lsp_rounds[side]
                             if "cold_index_ms" in r], keep_samples=True)
            for side in SIDES
        }
        report["capabilities"] = {
            side: (lsp_rounds[side][0].get("capabilities") if lsp_rounds[side] else {})
            for side in SIDES
        }

    # ---- MCP ----------------------------------------------------------------
    if want("mcp"):
        mcp_rounds = {side: [] for side in SIDES}
        rounds = 1 if args.no_memory else args.repeats
        for rep in range(1, rounds + 1):
            order = SIDES if rep % 2 == 1 else tuple(reversed(SIDES))
            for side in order:
                sys.stderr.write("[bench] mcp side=%s rep=%d\n" % (side, rep))
                mcp_rounds[side].append(run_mcp_session(
                    side, rep, project, al_path, args.iterations, args.warmups))
        report["mcp_rounds"] = mcp_rounds
        report["mcp"] = aggregate_rounds(mcp_rounds, [name for name, _ in MCP_TOOLS],
                                         keep_samples=True)
        report["mcp_handshake"] = {
            side: summarize([r["handshake_ms"] for r in mcp_rounds[side] if "handshake_ms" in r],
                            keep_samples=True)
            for side in SIDES
        }

    # ---- synthetic scaling --------------------------------------------------
    if want("scale") and not args.no_scale:
        sizes = [int(x) for x in args.sizes.split(",") if x.strip()]
        scale = {"sizes": [], "note": "OS page cache is warmed before measuring; side order "
                                      "alternates per size"}
        for size_index, size in enumerate(sizes):
            spec = ensure_synth_project(size)
            entry = {"files": spec["files"], "bytes": spec["bytes"],
                     "project_dir": spec["project_dir"],
                     "cache_warmed": spec["cache_warmed"], "sides": {}}
            order = SIDES if size_index % 2 == 0 else tuple(reversed(SIDES))
            for side in order:
                sys.stderr.write("[bench] scale files=%d side=%s\n" % (size, side))
                entry["sides"][side] = run_scale_side(side, spec, args.scale_iterations,
                                                      args.warmups)
            scale["sizes"].append(entry)
        report["scale"] = scale

    # ---- Phase 2 / Gate 0 ---------------------------------------------------
    if want("phase2"):
        run_phase2(args, report)

    # ---- residual processes -------------------------------------------------
    td_after = list_processes("td_commands_prompt.exe")
    report["residual"]["td_commands_prompt_after"] = sorted(td_after) if td_after is not None else None
    if td_before is not None and td_after is not None:
        report["residual"]["td_commands_prompt_new"] = sorted(td_after - td_before)
    leaked = []
    for key in ("lsp_rounds", "mcp_rounds"):
        for side, rounds in (report.get(key) or {}).items():
            for round_info in rounds:
                if round_info.get("alive_after_close"):
                    leaked.append({"side": side, "pid": round_info.get("pid"), "stage": key})
    report["residual"]["server_processes_alive_after_close"] = leaked

    # ---- native exe hash: server-side verification -------------------------
    after_hashes = {}
    for root, _dirs, files in os.walk(project):
        if os.sep + ".fpgalsp" in root:
            continue
        for name in files:
            if name.lower().endswith((".v", ".sv", ".al", ".sdc")):
                path = os.path.join(root, name)
                after_hashes[path] = sha256_of(path)
    changed = sorted(p for p in before_hashes if before_hashes[p] != after_hashes.get(p))
    report["integrity"] = {
        "source_files_hashed": len(before_hashes),
        "source_files_changed": changed,
    }

    load.stop()
    exe_at_end = native_exe_info()
    report["environment"]["cpu_busy_fraction_during_run"] = load.busy_fraction()
    report["environment"]["native_exe_stable_during_run"] = (
        exe_at_start.get("sha256") == exe_at_end.get("sha256"))
    report["environment"]["native_exe_at_end"] = exe_at_end

    # ---- summary ratios -----------------------------------------------------
    report["ratios"] = build_ratios(report)

    if args.out:
        out_path = args.out
        if not os.path.isabs(out_path):
            out_path = os.path.join(REPO_CPP, out_path)
        os.makedirs(os.path.dirname(out_path), exist_ok=True)
        with open(out_path, "w", encoding="utf-8") as handle:
            json.dump(report, handle, ensure_ascii=False, indent=2)
        sys.stderr.write("[bench] wrote %s\n" % out_path)
    else:
        json.dump(report, sys.stdout, ensure_ascii=False, indent=2)
    return 0


# ---------------------------------------------------------------------------
#  Phase 2 / Gate 0 —— 预算基线
# ---------------------------------------------------------------------------
#  三个新场景，全部只读、不跑 TD：
#    1) 21/200/1000 文件合成工程的 warm 延迟 + payload 字节
#    2) didChange 之后的 completion / documentSymbol（模拟连续输入 5 次）
#    3) 队头阻塞：1 个重请求（fpgaLsp/reports，10 MB 报告）在飞时，10 个快请求的 p95
#  外加：10 MB 报告的首解析 vs 重复请求、LSP/MCP 各自与并存时的进程数 + 峰值内存。

PHASE2_METHODS = [
    "textDocument/documentSymbol",
    "textDocument/hover",
    "textDocument/definition",
    "textDocument/references",
    "textDocument/completion",
    "textDocument/inlayHint",
    "workspace/symbol",
    "fpgaLsp/moduleTree",
    "fpgaLsp/reports",
]

# 预算草案（写进 BASELINE.md；Phase 2 每一步改完都用同一套断言复核）
PHASE2_BUDGETS = {
    "edit_to_completion_p95_ms_at200": 5.0,
    "edit_to_completion_p95_ms_at1000": 15.0,
    "warm_completion_p50_ms_at200": 4.0,
    "reports_cached_p50_ms": 2.0,
    "hover_p95_ms_while_heavy_inflight": 3.0,
    "single_engine_peak_mb_at200": 15.0,
}


# 重载荷阶段（10 MB 报告 / 队头阻塞 / 进程内存）只跑 native：
#   * 预算都是给 C++ 引擎的；
#   * Python 侧 reports.collect() 会对每条 timing path 做 HDL 解析，10 MB 报告要跑几分钟，
#     在这里没有参考价值（还会把整个基准拖死）。
HEAVY_SIDES = ("native",)


class _ConcurrentMixin:
    """给任意 LspClient 子类加上"多个在飞请求"能力（按 id 分流响应）。"""

    def __init__(self, *args, **kwargs):
        self._pending = {}
        self._pending_lock = threading.Lock()
        super().__init__(*args, **kwargs)

    def _dispatch(self, msg):
        mid = msg.get("id")
        if mid is not None and ("result" in msg or "error" in msg):
            with self._pending_lock:
                box = self._pending.get(mid)
            if box is not None:
                box.put(msg)
                return
            else:
                # 不是 async 发起的请求：交回基类的串行队列，request() 仍然可用
                self._responses.put(msg)
                return
        if msg.get("method"):
            self.notifications.append(msg)

    def request_async(self, method, params=None):
        with self._pending_lock:
            self._next_id += 1
            mid = self._next_id
            box = queue.Queue()
            self._pending[mid] = box
        self.send(method, params, mid)
        return mid, box, time.perf_counter()

    def collect(self, pending, timeout=900.0):
        mid, box, started = pending
        try:
            msg = box.get(timeout=timeout)
        except queue.Empty:
            raise TransportError("timeout waiting for id %s" % mid)
        with self._pending_lock:
            self._pending.pop(mid, None)
        elapsed = (time.perf_counter() - started) * 1000.0
        if "error" in msg:
            raise TransportError("JSON-RPC error: " +
                                 json.dumps(msg["error"], ensure_ascii=False)[:200])
        return elapsed, msg.get("result")


class ConcurrentLspClient(_ConcurrentMixin, LspClient):
    """``python -m fpgalsp`` + 多请求在飞。"""


class ConcurrentNativeLspClient(_ConcurrentMixin, NativeLspClient):
    """``fpgalsp-native.exe`` + 多请求在飞。"""


def make_concurrent_lsp_client(side, log_name):
    log_path = os.path.join(LOG_DIR, log_name)
    if side == "python":
        return ConcurrentLspClient(REPO_PY, log_path)
    return ConcurrentNativeLspClient(REPO_PY, log_path)


# ---- 合成报告（10 MB timing/area）-------------------------------------------

TIMING_HEADER = """\
Anlogic TangDynasty Timing Analysis Report
Device            : EF2L45LG144B
Speed Grade       : -6
STA coverage      : 100.0%
Confidence        : High
Period Check WNS  : -1.234 ns
SWNS              : -1.234 ns
STNS              : -5.678 ns
HWNS              : 0.000 ns
HTNS              : 0.000 ns
Max : SWNS -1.234ns, STNS -5.678ns, 128 Viol Endpoints, 45678 Total Endpoints
Min : SWNS -0.234ns, STNS -1.678ns, 8 Viol Endpoints, 1234 Total Endpoints
"""

AREA_HEADER = """\
Anlogic TangDynasty Area Report
Device            : EF2L45LG144B
Total LUTs        : 12345
Total Registers   : 6789
Total IO          : 144
"""


def make_timing_report(path, target_mb):
    """生成 TD 风格的 timing 报告；每个 Path 块都带解析器认得的字段。"""
    block = (
        "Path {index}: \n"
        "Slack : -0.{slack}ns\n"
        "  Begin Point : u_top/u_core/reg_{index}/CK\n"
        "  End Point   : u_top/u_core/reg_{next}/D\n"
        "  Check Type  : Setup\n"
        "  Data Path Delay : 1{delay}.{fh}ns\n"
        "  Clock Skew  : 0.05ns\n"
        "  Logic Level : {level}\n"
        "  Max Fanout  : {fanout}\n"
        "  Clock Uncertainty : 0.10ns\n"
    )
    target = int(target_mb * 1024 * 1024)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(TIMING_HEADER)
        written = len(TIMING_HEADER)
        index = 1
        while written < target:
            text = block.format(index=index, next=index + 1, slack=index % 1000,
                                delay=index % 90, fh=index % 10, level=index % 40,
                                fanout=index % 200)
            handle.write(text)
            written += len(text)
            index += 1
    return {"path": path, "bytes": os.path.getsize(path), "paths": index - 1}


def make_area_report(path, target_mb):
    target = int(target_mb * 1024 * 1024)
    row = "  u_top/u_core/u_%06d  |  LUT 12  |  REG 8  |  LUTRAM 0  |  ALU 2\n"
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(AREA_HEADER)
        written = len(AREA_HEADER)
        index = 0
        while written < target:
            text = row % index
            handle.write(text)
            written += len(text)
            index += 1
    return {"path": path, "bytes": os.path.getsize(path), "rows": index}


def ensure_report_project(size, report_mb, root=PERF_DIR):
    """合成工程 + ``<name>_Runs/phy_1`` 下的 10 MB timing / 2 MB area 报告。

    路径规则来自 `src/reports/artifacts.cpp:199`：``<project dir>/<project name>_Runs/<run>/``。
    """
    spec = ensure_synth_project(size, root, dir_name="report%03d" % size,
                                project_name="report%03d" % size)
    project_dir = spec["project_dir"]
    name = os.path.basename(project_dir)
    runs = os.path.join(project_dir, name + "_Runs")
    phy = os.path.join(runs, "phy_1")
    syn = os.path.join(runs, "syn_1")
    os.makedirs(phy, exist_ok=True)
    os.makedirs(syn, exist_ok=True)
    timing = make_timing_report(os.path.join(phy, "final_timing.rpt"), report_mb)
    area = make_area_report(os.path.join(phy, name + "_phy.area"), max(2, report_mb // 5))
    spec.update({
        "report_dir": runs,
        "timing_report": timing,
        "area_report": area,
        "report_mb": report_mb,
    })
    # 预热 OS page cache，避免把"首次读 10 MB"算成解析成本
    for path in (timing["path"], area["path"]):
        with open(path, "rb") as handle:
            handle.read()
    return spec


def phase2_anchor(spec):
    """每个合成工程的请求锚点（模块声明位置 / 补全行 / uri）。"""
    ref_path = spec["ref_file"]
    lines = read_text(ref_path).splitlines()
    decl_line, decl_col = find_module_decl(lines, "synth_m0001")
    body_line = 0
    for index, line in enumerate(lines):
        if "acc <= acc + din;" in line:
            body_line = index
            break
    return {
        "uri": path_to_uri(ref_path),
        "ref_pos": {"line": decl_line, "character": decl_col + 1},
        "hover_pos": {"line": decl_line, "character": decl_col + 1},
        "completion_pos": {"line": body_line, "character": 8},
    }


def phase2_params(spec, method, anchor):
    uri = anchor["uri"]
    if method == "textDocument/documentSymbol":
        return {"textDocument": {"uri": uri}}
    if method in ("textDocument/hover", "textDocument/definition"):
        return {"textDocument": {"uri": uri}, "position": anchor["hover_pos"]}
    if method == "textDocument/references":
        return {"textDocument": {"uri": uri}, "position": anchor["ref_pos"],
                "context": {"includeDeclaration": True}}
    if method == "textDocument/completion":
        return {"textDocument": {"uri": uri}, "position": anchor["completion_pos"]}
    if method == "textDocument/inlayHint":
        return {"textDocument": {"uri": uri},
                "range": {"start": {"line": 0, "character": 0},
                          "end": {"line": 200, "character": 0}}}
    if method == "workspace/symbol":
        return {"query": "synth_m"}
    if method in ("fpgaLsp/moduleTree", "fpgaLsp/reports"):
        return {"uri": uri}
    raise ValueError("phase2: no params for " + method)


def run_phase2_warm(side, spec, iterations):
    """冷索引一次，然后每方法 warm p50/p95 + payload 字节。"""
    anchor = spec["anchor"]
    result = {"side": side, "files": spec["files"], "bytes": spec["bytes"]}
    client = make_lsp_client(side, "p2-warm-%s-%03d.log" % (side, spec["files"]))
    sampler = MemorySampler(client.proc.pid)
    try:
        with sampler:
            t0 = time.perf_counter()
            client.initialize(spec["project_dir"], timeout=1800.0)
            result["initialize_ms"] = round((time.perf_counter() - t0) * 1000.0, 1)
            t0 = time.perf_counter()
            client.request("textDocument/documentSymbol",
                           phase2_params(spec, "textDocument/documentSymbol", anchor),
                           timeout=1800.0)
            result["cold_index_ms"] = round((time.perf_counter() - t0) * 1000.0, 1)
            methods = {}
            for method in PHASE2_METHODS:
                params = phase2_params(spec, method, anchor)
                for _ in range(3):
                    client.request(method, params, timeout=1800.0)
                samples = []
                payload = 0
                for _ in range(iterations):
                    t0 = time.perf_counter()
                    out = client.request(method, params, timeout=1800.0)
                    samples.append((time.perf_counter() - t0) * 1000.0)
                    payload = len(json.dumps(out, ensure_ascii=False))
                summary = summarize(samples, keep_samples=False)
                summary["payload_chars"] = payload
                methods[method] = summary
            result["methods"] = methods
    except Exception:
        result["fatal"] = traceback.format_exc()
    finally:
        try:
            client.close()
        except Exception:
            pass
    result["memory"] = sampler.as_dict()
    result["alive_after_close"] = process_alive(client.proc.pid)
    return result


def run_phase2_edit(side, spec, edits):
    """didOpen 后连续输入 ``edits`` 次：每次 didChange 立刻请求 completion / documentSymbol。

    这是 Phase 2 的核心预算（edit→completion）。当前实现里 didChange 会改变工程签名，
    所以下一个请求要重建整个索引 —— 基线应该就是"每次编辑一次全量重建"。
    """
    anchor = spec["anchor"]
    path = spec["ref_file"]
    uri = anchor["uri"]
    text = read_text(path)
    result = {"side": side, "files": spec["files"], "edits": edits}
    client = make_lsp_client(side, "p2-edit-%s-%03d.log" % (side, spec["files"]))
    try:
        client.initialize(spec["project_dir"], timeout=1800.0)
        client.request("textDocument/documentSymbol", {"textDocument": {"uri": uri}},
                       timeout=1800.0)
        client.open_document(path, "verilog", text, version=1)
        # 打开后先测一次"缓冲区已打开、无编辑"的基线
        warm = {}
        for method in ("textDocument/completion", "textDocument/documentSymbol"):
            params = phase2_params(spec, method, anchor)
            for _ in range(3):
                client.request(method, params, timeout=1800.0)
            samples = []
            for _ in range(10):
                t0 = time.perf_counter()
                client.request(method, params, timeout=1800.0)
                samples.append((time.perf_counter() - t0) * 1000.0)
            warm[method] = summarize(samples, keep_samples=False)
        result["warm_with_document_open"] = warm

        per_edit = []
        for index in range(1, edits + 1):
            text = text.replace("endmodule",
                                "  reg [7:0] edit_%d;\nendmodule" % index, 1)
            t0 = time.perf_counter()
            client.notify("textDocument/didChange", {
                "textDocument": {"uri": uri, "version": index + 1},
                "contentChanges": [{"text": text}],
            })
            entry = {"edit": index}
            for method, key in (("textDocument/completion", "completion_ms"),
                                ("textDocument/documentSymbol", "documentSymbol_ms")):
                t0 = time.perf_counter()
                client.request(method, phase2_params(spec, method, anchor), timeout=1800.0)
                entry[key] = round((time.perf_counter() - t0) * 1000.0, 2)
            per_edit.append(entry)
        result["per_edit"] = per_edit
        for key in ("completion_ms", "documentSymbol_ms"):
            result[key] = summarize([row[key] for row in per_edit], keep_samples=True)
    except Exception:
        result["fatal"] = traceback.format_exc()
    finally:
        try:
            client.close()
        except Exception:
            pass
    return result


def run_phase2_head_of_line(side, spec, fast_requests):
    """先发 1 个重请求（reports），紧接着发 N 个快请求（hover），量队头阻塞。"""
    anchor = spec["anchor"]
    result = {"side": side, "files": spec["files"], "fast_requests": fast_requests}
    client = make_concurrent_lsp_client(side, "p2-hol-%s-%03d.log" % (side, spec["files"]))
    try:
        client.initialize(spec["project_dir"], timeout=1800.0)
        client.request("textDocument/documentSymbol",
                       phase2_params(spec, "textDocument/documentSymbol", anchor),
                       timeout=1800.0)
        hover_params = phase2_params(spec, "textDocument/hover", anchor)
        for _ in range(3):
            client.request("textDocument/hover", hover_params, timeout=1800.0)

        # (a) 空闲时 10 个并发快请求 —— 客户端并发能力自身的对照
        idle = [client.collect(client.request_async("textDocument/hover", hover_params))
                for _ in range(fast_requests)]
        result["idle_concurrent_hover_ms"] = summarize([row[0] for row in idle],
                                                       keep_samples=True)

        # (b) 重请求在飞时的 10 个快请求
        heavy = client.request_async("fpgaLsp/reports",
                                     phase2_params(spec, "fpgaLsp/reports", anchor))
        pending = [client.request_async("textDocument/hover", hover_params)
                   for _ in range(fast_requests)]
        blocked = [client.collect(item) for item in pending]
        heavy_ms, heavy_payload = client.collect(heavy, timeout=300.0)
        result["busy_concurrent_hover_ms"] = summarize([row[0] for row in blocked],
                                                       keep_samples=True)
        result["heavy_request_ms"] = round(heavy_ms, 2)
        result["heavy_payload_chars"] = len(json.dumps(heavy_payload, ensure_ascii=False))
        result["head_of_line_ratio"] = round(
            result["busy_concurrent_hover_ms"]["p50"] /
            max(0.01, result["idle_concurrent_hover_ms"]["p50"]), 2)
    except Exception:
        result["fatal"] = traceback.format_exc()
    finally:
        try:
            client.close()
        except Exception:
            pass
    return result


def run_phase2_reports(side, spec, repeats):
    """10 MB 报告：首次解析 vs 重复请求（A3 ReportStore 的 before）。"""
    anchor = spec["anchor"]
    result = {"side": side, "files": spec["files"],
              "report_mb": spec.get("report_mb"),
              "timing_report_bytes": (spec.get("timing_report") or {}).get("bytes"),
              "area_report_bytes": (spec.get("area_report") or {}).get("bytes")}
    client = make_lsp_client(side, "p2-reports-%s.log" % side)
    sampler = MemorySampler(client.proc.pid)
    try:
        with sampler:
            client.initialize(spec["project_dir"], timeout=1800.0)
            params = phase2_params(spec, "fpgaLsp/reports", anchor)
            t0 = time.perf_counter()
            first = client.request("fpgaLsp/reports", params, timeout=300.0)
            result["first_parse_ms"] = round((time.perf_counter() - t0) * 1000.0, 2)
            result["payload_chars"] = len(json.dumps(first, ensure_ascii=False))
            samples = []
            for _ in range(repeats):
                t0 = time.perf_counter()
                client.request("fpgaLsp/reports", params, timeout=300.0)
                samples.append((time.perf_counter() - t0) * 1000.0)
            result["repeat_ms"] = summarize(samples, keep_samples=True)
    except Exception:
        result["fatal"] = traceback.format_exc()
    finally:
        try:
            client.close()
        except Exception:
            pass
    result["memory"] = sampler.as_dict()
    return result


def run_phase2_footprint(spec, iterations=10):
    """LSP / MCP / 两者并存：进程数 + 峰值 Working Set（单引擎 B1 的 before）。

    任一子系统启动失败（例如当前 build 的 MCP 模式启动即崩）只记录错误，不中断整轮基准。
    """
    anchor = spec["anchor"]
    output = {}

    def lsp_client(tag):
        client = make_lsp_client("native", "p2-foot-lsp-%s.log" % tag)
        client.initialize(spec["project_dir"], timeout=1800.0)
        client.request("textDocument/documentSymbol",
                       phase2_params(spec, "textDocument/documentSymbol", anchor),
                       timeout=1800.0)
        return client

    def mcp_client(tag):
        client = make_mcp_client("native", spec["project_dir"], "p2-foot-mcp-%s.log" % tag)
        for _ in range(3):
            client.call_tool("fpga_project_structure", {"include_ports": False}, timeout=900.0)
        return client

    # (a) 只跑 LSP
    lsp = None
    try:
        lsp = lsp_client("a")
        sampler = MemorySampler(lsp.proc.pid)
        with sampler:
            for _ in range(iterations):
                lsp.request("textDocument/hover",
                            phase2_params(spec, "textDocument/hover", anchor), timeout=900.0)
        output["lsp_only"] = {"server_processes": 1, "pids": [lsp.proc.pid],
                              **sampler.as_dict()}
    except Exception:
        output["lsp_only"] = {"error": traceback.format_exc()}
    finally:
        if lsp is not None:
            try:
                lsp.close()
            except Exception:
                pass

    # (b) 只跑 MCP
    mcp = None
    try:
        mcp = mcp_client("b")
        sampler = MemorySampler(mcp.proc.pid)
        with sampler:
            for _ in range(iterations):
                mcp.call_tool("fpga_project_structure", {"include_ports": False}, timeout=900.0)
        output["mcp_only"] = {"server_processes": 1, "pids": [mcp.proc.pid],
                              **sampler.as_dict()}
    except Exception:
        output["mcp_only"] = {"error": traceback.format_exc()}
    finally:
        if mcp is not None:
            try:
                mcp.close()
            except Exception:
                pass

    # (c) LSP + MCP 同时（今天 = 两个进程，B1 单引擎后应该只剩一个）
    lsp = None
    mcp = None
    try:
        lsp = lsp_client("c")
        mcp = mcp_client("c")
        samplers = {"lsp": MemorySampler(lsp.proc.pid), "mcp": MemorySampler(mcp.proc.pid)}
        peak_sum = 0
        for item in samplers.values():
            item.__enter__()
        try:
            deadline = time.perf_counter() + 3.0
            while time.perf_counter() < deadline:
                for _ in range(iterations):
                    lsp.request("textDocument/hover",
                                phase2_params(spec, "textDocument/hover", anchor),
                                timeout=900.0)
                    mcp.call_tool("fpga_project_structure", {"include_ports": False},
                                  timeout=900.0)
                current = 0
                for item in samplers.values():
                    working, _peak = read_memory_counters(item.pid) or (0, 0)
                    current += working
                peak_sum = max(peak_sum, current)
        finally:
            for item in samplers.values():
                item.__exit__(None, None, None)
        output["lsp_and_mcp"] = {
            "server_processes": 2,
            "pids": [lsp.proc.pid, mcp.proc.pid],
            "peak_working_set_mb": round(
                sum(item.peak_working_set for item in samplers.values()) / (1024 * 1024), 2),
            "peak_of_instantaneous_sum_mb": round(peak_sum / (1024 * 1024), 2),
            "peak_counters_working_set_mb": round(
                sum(item.peak_counters for item in samplers.values()) / (1024 * 1024), 2),
        }
    except Exception:
        output["lsp_and_mcp"] = {"error": traceback.format_exc()}
    finally:
        for client in (lsp, mcp):
            if client is not None:
                try:
                    client.close()
                except Exception:
                    pass
    output["td_processes"] = len(list_processes("td_commands_prompt.exe") or [])
    output["native_exe"] = native_exe_info()
    return output


def native_exe_info():
    """记录本阶段实际使用的 exe（便于在 MCP 崩溃的窗口里说明二进制来源）。"""
    try:
        exe = default_native_exe()
    except FileNotFoundError:
        return {"path": None}
    return {"path": exe,
            "bytes": os.path.getsize(exe),
            "sha256": sha256_of(exe),
            "selected_by": ("FPGALSP_NATIVE_EXE"
                            if os.environ.get("FPGALSP_NATIVE_EXE") else "default")}


def run_phase2(args, report):
    sizes = [int(x) for x in args.phase2_sizes.split(",") if x.strip()]
    phase2 = {"sizes": sizes, "budgets": PHASE2_BUDGETS,
              "methods": PHASE2_METHODS, "warm": [], "edit_loop": [],
              "head_of_line": [], "reports": None, "footprint": None}
    specs = {}
    for size_index, size in enumerate(sizes):
        spec = ensure_synth_project(size)
        spec["anchor"] = phase2_anchor(spec)
        specs[size] = spec
        entry = {"files": size, "bytes": spec["bytes"], "sides": {}}
        order = SIDES if size_index % 2 == 0 else tuple(reversed(SIDES))
        for side in order:
            sys.stderr.write("[phase2] warm files=%d side=%s\n" % (size, side))
            entry["sides"][side] = run_phase2_warm(side, spec, args.phase2_iterations)
        phase2["warm"].append(entry)

    # didChange 场景：小/中/大各跑一次（大工程的重建成本是主要观察对象）
    for size_index, size in enumerate(sizes):
        spec = specs[size]
        entry = {"files": size, "sides": {}}
        order = SIDES if size_index % 2 == 0 else tuple(reversed(SIDES))
        for side in order:
            sys.stderr.write("[phase2] edit-loop files=%d side=%s\n" % (size, side))
            entry["sides"][side] = run_phase2_edit(side, spec, args.phase2_edits)
        phase2["edit_loop"].append(entry)

    # 队头阻塞：用 200 文件的工程（1000 文件时报告队列几乎一样，只是更慢）
    hol_size = 200 if 200 in specs else sizes[-1]
    spec = specs[hol_size]
    for side in HEAVY_SIDES:
        sys.stderr.write("[phase2] head-of-line files=%d side=%s\n" % (hol_size, side))
        phase2["head_of_line"].append(run_phase2_head_of_line(side, spec, args.phase2_fast))

    # 10 MB 报告：200 文件的工程 + _Runs
    report_size = 200 if 200 in specs else sizes[-1]
    sys.stderr.write("[phase2] reports (%s MB) side=native\n" % args.phase2_report_mb)
    report_spec = ensure_report_project(report_size, args.phase2_report_mb)
    report_spec["anchor"] = phase2_anchor(report_spec)
    phase2["reports"] = {"files": report_size,
                         "report_mb": args.phase2_report_mb,
                         "sides": {side: run_phase2_reports(side, report_spec,
                                                            args.phase2_repeats)
                                   for side in HEAVY_SIDES}}

    # 进程/内存基线
    sys.stderr.write("[phase2] engine footprint\n")
    phase2["footprint"] = run_phase2_footprint(report_spec)
    report["phase2"] = phase2
    return phase2
    """Merge per-round samples into per-method p50/p95/max for each side."""
    merged = {}
    for side, round_list in rounds.items():
        bucket = {}
        for method in methods:
            samples = []
            for round_info in round_list:
                samples.extend((round_info.get("timings") or {}).get(method) or [])
            bucket[method] = summarize(samples, keep_samples=keep_samples)
        merged[side] = bucket
    return merged


def build_ratios(report):
    ratios = {"lsp": {}, "mcp": {}}
    for section, methods in (("lsp", LSP_METHODS), ("mcp", [n for n, _ in MCP_TOOLS])):
        block = report.get(section) or {}
        python_block = block.get("python") or {}
        native_block = block.get("native") or {}
        for method in methods:
            py = (python_block.get(method) or {}).get("p50")
            na = (native_block.get(method) or {}).get("p50")
            ratios[section][method] = {"python_p50_ms": py, "native_p50_ms": na,
                                       "speedup_python_over_native": ratio(py, na)}
    cold = report.get("cold_start") or {}
    if cold:
        py = (cold.get("python") or {}).get("total_ms", {}).get("p50")
        na = (cold.get("native") or {}).get("total_ms", {}).get("p50")
        ratios["cold_start_total_p50_ms"] = {"python": py, "native": na,
                                            "speedup_python_over_native": ratio(py, na)}
    cold_index = report.get("cold_index") or {}
    if cold_index:
        py = (cold_index.get("python") or {}).get("p50")
        na = (cold_index.get("native") or {}).get("p50")
        ratios["cold_index_p50_ms"] = {"python": py, "native": na,
                                       "speedup_python_over_native": ratio(py, na)}
    # Derived: "time until a cold client can answer a symbol request on a cold index".
    # Fair whether the server builds the index at initialize (Python) or lazily (native).
    if cold_index and cold:
        path = {}
        for side in SIDES:
            init_p50 = (cold.get(side) or {}).get("initialize_ms", {}).get("p50")
            index_p50 = (cold_index.get(side) or {}).get("p50")
            if init_p50 is not None and index_p50 is not None:
                path[side] = round(init_p50 + index_p50, 3)
        if len(path) == 2:
            path["speedup_python_over_native"] = ratio(path["python"], path["native"])
            ratios["cold_path_p50_ms"] = path
    return ratios


if __name__ == "__main__":
    sys.exit(main())