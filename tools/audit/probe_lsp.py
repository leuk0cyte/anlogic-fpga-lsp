#!/usr/bin/env python3
"""Wire-level LSP parity probe: Python reference (`python -m fpgalsp --stdio`)
versus the C++23 rewrite (`fpgalsp-native.exe --stdio`).

The probe answers four questions and writes every byte it saw to ``--out``:

1. *What does ``initialize`` declare?*  (capability matrix)
2. *For each standard/custom method, what actually comes back on the wire?*
   Raw result, normalized result (volatile fields masked), emptiness, field
   count and first item are recorded per side, per request.
3. *Do the 24 ``workspace/executeCommand`` commands behave the same?*
   Safe commands are executed for real; write commands run on a throw-away
   copy of the project and the copy is diffed before/after so it is provable
   that nothing escaped to the real project.  Hardware/flow commands are not
   executed (``not probed: side effects``).
4. *Do the boundary cases hold?*  Empty document, Chinese + non-BMP (emoji)
   UTF-16 columns, both sides of an `ifdef, and non-HDL files (.adc / .sdc).

Reproduction::

    python tools/audit/probe_lsp.py \
        --python-log   %TEMP%\\probe\\python.log \
        --native-exe   build/bin/fpgalsp-native.exe \
        --project      %WORKSPACE%/CorpusAlpha \
        --out          doc/audit/2026-11-18/parity-lsp/raw.json

The probe is strictly read-only with respect to ``--project``: every server is
rooted at a copy under ``--workdir`` (default ``%TEMP%\\fpgalsp-parity-lsp``).
"""

from __future__ import annotations

import argparse
import difflib
import hashlib
import json
import os
import re
import shutil
import sys
import time
import traceback

# --------------------------------------------------------------------------- #
# Normalization
# --------------------------------------------------------------------------- #
# Keys whose *values* are expected to differ between two runs (or two servers)
# and that therefore must not take part in the parity comparison.
MASK_KEYS = {
    "elapsed", "elapsedMs", "elapsed_ms", "duration", "durationMs",
    "mtime", "pid", "processId", "td_session", "runId", "timestamp",
    "startedAt", "endedAt", "at", "time", "sizeBytes", "rss",
}
# Values that look like a wall-clock timestamp anywhere in the payload.
TS_RE = re.compile(r"^\d{4}-\d{2}-\d{2}[T ]\d{2}:\d{2}:\d{2}")


def normalize(value, path_tokens):
    """Mask volatile fields and fold absolute project paths to a token."""
    if isinstance(value, dict):
        out = {}
        for key, item in value.items():
            if key in MASK_KEYS:
                out[key] = "<masked>"
            else:
                out[key] = normalize(item, path_tokens)
        return out
    if isinstance(value, list):
        return [normalize(item, path_tokens) for item in value]
    if isinstance(value, str):
        text = value
        for token, real in path_tokens:
            if not real:
                continue
            text = text.replace(real.replace("\\", "/"), token)
            text = text.replace(real, token)
            text = text.replace(real.replace("/", "\\"), token)
        if TS_RE.match(text):
            return "<timestamp>"
        return text
    return value


def canon(value):
    return json.dumps(value, sort_keys=True, ensure_ascii=False)


def digest(value):
    return hashlib.sha256(canon(value).encode("utf-8")).hexdigest()


def shrink(value, depth=0):
    """Storage-size view of a payload; keeps shape and head/tail of arrays."""
    if depth > 6:
        return "<depth>"
    if isinstance(value, dict):
        out = {}
        for index, (key, item) in enumerate(value.items()):
            if index >= 40:
                out["__more_keys__"] = len(value) - 40
                break
            out[key] = shrink(item, depth + 1)
        return out
    if isinstance(value, list):
        if len(value) > 8:
            return {
                "__len__": len(value),
                "__head__": [shrink(item, depth + 1) for item in value[:6]],
                "__tail__": [shrink(item, depth + 1) for item in value[-2:]],
            }
        return [shrink(item, depth + 1) for item in value]
    if isinstance(value, str) and len(value) > 400:
        return value[:400] + "...<+%d chars>" % (len(value) - 400)
    return value


def describe(result):
    """Emptiness / field count / first item, in a form PROBE.md can tabulate."""
    info = {"kind": type(result).__name__, "empty": True, "count": 0,
            "fields": 0, "first": None}
    if result is None:
        info["kind"] = "null"
        return info
    if isinstance(result, list):
        info["kind"] = "array"
        info["count"] = len(result)
        info["fields"] = len(result)
        info["empty"] = not result
        if result:
            info["first"] = shrink(result[0])
        return info
    if isinstance(result, dict):
        keys = sorted(result)
        info["kind"] = "object"
        info["fields"] = len(keys)
        info["count"] = len(keys)
        # LSP "nothing to report" shapes
        info["empty"] = not keys or (keys == ["data"] and result.get("data") in ([], None))
        if "data" in result and isinstance(result["data"], list):
            info["data_len"] = len(result["data"])
        if keys:
            first_key = keys[0]
            info["first"] = {"key": first_key, "value": shrink(result[first_key])}
        return info
    if isinstance(result, str):
        info["kind"] = "string"
        info["empty"] = not result
        info["count"] = 1
        info["fields"] = 1
        info["first"] = result[:200]
        return info
    info["kind"] = "scalar"
    info["empty"] = result is None
    info["count"] = 1
    info["fields"] = 1
    info["first"] = result
    return info


# --------------------------------------------------------------------------- #
# Project fixtures
# --------------------------------------------------------------------------- #
BOUNDARY_HDL = """\
// =============================================================================
// audit boundary probe —— 中文注释 + emoji（非 BMP）列换算 + `ifdef 两侧
// Project: CorpusAlpha (copy)   Generated by tools/audit/probe_lsp.py
// =============================================================================
`ifdef AUDIT_NEVER_DEFINED
module audit_ifdef_dead (
    input wire dead_clk
);
    // 死分支：若索引器不处理 `ifdef，这里会出现 audit_ifdef_dead / dead_sig
    wire dead_sig;
    assign dead_sig = dead_clk;
endmodule
`else
module audit_ifdef_live #(
    parameter WIDTH = 8          // 参数：位宽
) (
    input  wire              clk,   // 时钟 🕐
    input  wire              rst_n, // 复位（低有效）🧊
    output reg  [WIDTH-1:0]  q      // 输出 🚀
);
    /* 🚀 中文注释 */ wire emoji_after_comment;
    assign emoji_after_comment = clk;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) q <= {WIDTH{1'b0}};
        else        q <= q + 1'b1;
    end
endmodule
`endif
"""

ADDED_HDL = """\
// audit addSourceFile probe (copied project only)
module audit_added_source (
    input  wire clk,
    output wire out
);
    assign out = clk;
endmodule
"""


def covered_copy(source, target, extra_files=None):
    """Copy a project tree, skipping server scratch dirs, then add fixtures."""
    if os.path.isdir(target):
        shutil.rmtree(target)
    shutil.copytree(
        source, target,
        ignore=shutil.ignore_patterns(".fpgalsp", "*.vcd", "__pycache__"),
    )
    for relative, text in (extra_files or {}).items():
        full = os.path.join(target, relative.replace("/", os.sep))
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
    return target


def register_in_al(al_path, entries):
    """Insert <File Path="...">Verilog</File> entries before </Verilog>."""
    with open(al_path, encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    if not entries:
        return
    blocks = []
    for relative, order in entries:
        blocks.append(
            '            <File Path="%s">\n'
            '                <FileInfo>\n'
            '                    <Attr Name="UsedInSyn" Val="true"/>\n'
            '                    <Attr Name="UsedInP&R" Val="true"/>\n'
            '                    <Attr Name="BelongTo" Val="design_1"/>\n'
            '                    <Attr Name="CompileOrder" Val="%d"/>\n'
            '                </FileInfo>\n'
            '            </File>\n' % (relative, order))
    marker = "        </Verilog>"
    if marker not in text:
        raise SystemExit("cannot find </Verilog> in %s" % al_path)
    text = text.replace(marker, "".join(blocks) + marker, 1)
    with open(al_path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


def tree_snapshot(root):
    """relpath -> sha256 for every file outside .fpgalsp (plus a scratch flag)."""
    snapshot = {}
    scratch = False
    for base, dirs, files in os.walk(root):
        if ".fpgalsp" in dirs:
            scratch = True
        for name in files:
            full = os.path.join(base, name)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            if rel.startswith(".fpgalsp/"):
                continue
            try:
                with open(full, "rb") as handle:
                    snapshot[rel] = hashlib.sha256(handle.read()).hexdigest()
            except OSError as exc:
                snapshot[rel] = "unreadable:%s" % exc
    snapshot[".fpgalsp/"] = "created" if scratch else "absent"
    return snapshot


def snapshot_diff(before, after):
    added = sorted(set(after) - set(before))
    removed = sorted(set(before) - set(after))
    changed = sorted(key for key in set(before) & set(after)
                     if before[key] != after[key])
    return {"added": added, "removed": removed, "changed": changed}


def utf16_col(line, token):
    index = line.index(token)
    return len(line[:index].encode("utf-16-le")) // 2


def _shape_key(result):
    """Structural signature: kind + number of items/fields (ignores item content)."""
    summary = describe(result)
    return (summary["kind"], summary["count"], summary["fields"])


def _text_edit_note(python_result, cpp_result):
    """For TextEdit arrays, record exact hashes and a unified diff head."""
    def edits(value):
        if isinstance(value, list):
            return [item for item in value
                    if isinstance(item, dict) and isinstance(item.get("newText"), str)]
        return []

    py_edits = edits(python_result)
    cpp_edits = edits(cpp_result)
    if not py_edits and not cpp_edits:
        return None
    note = {"python_edits": len(py_edits), "cpp_edits": len(cpp_edits)}

    def joined(edit_list):
        return "\n".join(item["newText"] for item in edit_list)

    py_text = joined(py_edits)
    cpp_text = joined(cpp_edits)
    note["sha256_python"] = hashlib.sha256(py_text.encode("utf-8")).hexdigest()
    note["sha256_cpp"] = hashlib.sha256(cpp_text.encode("utf-8")).hexdigest()
    note["byte_identical"] = py_text == cpp_text
    py_lf = py_text.replace("\r\n", "\n")
    cpp_lf = cpp_text.replace("\r\n", "\n")
    note["identical_after_eol_normalization"] = py_lf == cpp_lf
    note["python_uses_crlf"] = "\r\n" in py_text
    note["cpp_uses_crlf"] = "\r\n" in cpp_text
    if py_lf != cpp_lf:
        diff = list(difflib.unified_diff(py_lf.split("\n"), cpp_lf.split("\n"),
                                         "python", "cpp", lineterm="", n=1))
        note["diff_head"] = [line[:300] for line in diff[:30]]
    return note


# --------------------------------------------------------------------------- #
# Probe driver
# --------------------------------------------------------------------------- #
class Prober:
    def __init__(self, args, clients):
        self.args = args
        self.LspClient = clients["LspClient"]
        self.NativeLspClient = clients["NativeLspClient"]
        self.TransportError = clients["TransportError"]
        self.uri = clients["path_to_uri"]
        self.logs = os.path.join(args.workdir, "logs")
        self.pairs = []
        self.results = {"matrix": [], "commands": [], "boundaries": []}

    # -- process plumbing ---------------------------------------------------
    def pair(self, python_root, cpp_root, tag):
        os.makedirs(self.logs, exist_ok=True)
        python = self.LspClient(
            self.args.python_repo,
            os.path.join(self.logs, "%s-python.log" % tag))
        native = self.NativeLspClient(
            self.args.python_repo,
            os.path.join(self.logs, "%s-native.log" % tag),
            exe_path=self.args.native_exe)
        py_init = python.initialize(python_root, timeout=180.0)
        cpp_init = native.initialize(cpp_root, timeout=180.0)
        entry = {"tag": tag, "python": python, "native": native,
                 "python_root": python_root, "cpp_root": cpp_root,
                 "python_init": py_init, "cpp_init": cpp_init}
        self.pairs.append(entry)
        return entry

    @staticmethod
    def client_of(entry, side):
        """entry["python"] / entry["native"] under the report's python/cpp naming."""
        return entry["native"] if side == "cpp" else entry["python"]

    def close_all(self):
        for entry in self.pairs:
            for client in (entry["python"], entry["native"]):
                try:
                    client.close()
                except Exception:
                    pass

    # -- one request --------------------------------------------------------
    def ask(self, client, method, params, timeout):
        started = time.time()
        try:
            result = client.request(method, params, timeout=timeout)
            return {"ok": True, "elapsed_ms": int((time.time() - started) * 1000),
                    "result": result}
        except self.TransportError as exc:
            return {"ok": False, "elapsed_ms": int((time.time() - started) * 1000),
                    "error": str(exc), "result": None}
        except Exception as exc:  # noqa: BLE001 - transport may die in odd ways
            return {"ok": False, "elapsed_ms": int((time.time() - started) * 1000),
                    "error": "%s: %s" % (type(exc).__name__, exc), "result": None}

    def compare(self, case, python_root, cpp_root, timeout):
        """Run one params payload against both servers and record everything."""
        params_py = self._subst(case["params"], python_root, cpp_root, "python")
        params_cpp = self._subst(case["params"], python_root, cpp_root, "cpp")
        entry = self.pairs[-1]
        py = self.ask(entry["python"], case["method"], params_py, timeout)
        cpp = self.ask(entry["native"], case["method"], params_cpp, timeout)

        tokens = [("<PROJECT>", os.path.abspath(python_root)),
                  ("<PROJECT>", os.path.abspath(cpp_root))]
        py_norm = normalize(py["result"], tokens) if py["ok"] else None
        cpp_norm = normalize(cpp["result"], tokens) if cpp["ok"] else None
        py_ok_norm = {"ok": py["ok"], "error": self._norm_error(py.get("error"), tokens),
                      "result": py_norm}
        cpp_ok_norm = {"ok": cpp["ok"], "error": self._norm_error(cpp.get("error"), tokens),
                       "result": cpp_norm}

        record = {
            "id": case["id"],
            "method": case["method"],
            "note": case.get("note", ""),
            "params_python": shrink(params_py),
            "params_cpp": shrink(params_cpp),
            "python": {"ok": py["ok"], "elapsed_ms": py["elapsed_ms"],
                       "error": py.get("error"), "raw": shrink(py["result"]),
                       "normalized": shrink(py_ok_norm),
                       "summary": describe(py["result"]),
                       "digest": digest(py_ok_norm)},
            "cpp": {"ok": cpp["ok"], "elapsed_ms": cpp["elapsed_ms"],
                    "error": cpp.get("error"), "raw": shrink(cpp["result"]),
                    "normalized": shrink(cpp_ok_norm),
                    "summary": describe(cpp["result"]),
                    "digest": digest(cpp_ok_norm)},
            "same_shape": describe(py["result"]) == describe(cpp["result"]),
            "shape_equal": _shape_key(py["result"]) == _shape_key(cpp["result"]),
            "identical_after_normalization": digest(py_ok_norm) == digest(cpp_ok_norm),
        }
        text_note = _text_edit_note(py["result"], cpp["result"])
        if text_note:
            record["text_diff"] = text_note
        if record["python"]["summary"]["kind"] != record["cpp"]["summary"]["kind"]:
            record["shape_diff"] = "python=%s / cpp=%s" % (
                record["python"]["summary"]["kind"], record["cpp"]["summary"]["kind"])
        return record

    @staticmethod
    def _subst(params, python_root, cpp_root, side):
        text = json.dumps(params, ensure_ascii=False)
        text = text.replace("<PROJECT>", "<PROJECT>")  # placeholder, substituted below
        if side == "python":
            text = text.replace("__ROOT__", python_root.replace("\\", "/"))
        else:
            text = text.replace("__ROOT__", cpp_root.replace("\\", "/"))
        return json.loads(text)

    @staticmethod
    def _norm_error(message, tokens):
        if not message:
            return None
        for token, real in tokens:
            message = message.replace(real.replace("\\", "/"), token)
            message = message.replace(real, token)
        return message

    # -- phase 1/2: capabilities + method matrix ----------------------------
    def run_matrix(self):
        args = self.args
        read_root = os.path.join(args.workdir, "project-read")
        covered_copy(args.project, read_root)
        self.pair(read_root, read_root, "matrix")

        entry = self.pairs[-1]
        capabilities = {
            "python": shrink(entry["python_init"]),
            "cpp": shrink(entry["cpp_init"]),
            "normalized": {
                "python": shrink(normalize(entry["python_init"], [])),
                "cpp": shrink(normalize(entry["cpp_init"], [])),
            },
        }
        caps_py = normalize(entry["python_init"], [])
        caps_cpp = normalize(entry["cpp_init"], [])
        cap_py_keys = set((caps_py.get("capabilities") or {}).keys())
        cap_cpp_keys = set((caps_cpp.get("capabilities") or {}).keys())
        capabilities["capability_keys"] = {
            "python": sorted(cap_py_keys),
            "cpp": sorted(cap_cpp_keys),
            "only_python": sorted(cap_py_keys - cap_cpp_keys),
            "only_cpp": sorted(cap_cpp_keys - cap_py_keys),
        }
        differing = {}
        for key in sorted(cap_py_keys & cap_cpp_keys):
            py_val = canon(caps_py["capabilities"][key])
            cpp_val = canon(caps_cpp["capabilities"][key])
            if py_val != cpp_val:
                differing[key] = {"python": caps_py["capabilities"][key],
                                  "cpp": caps_cpp["capabilities"][key]}
        capabilities["capability_values_differing"] = differing
        self.results["capabilities"] = capabilities

        top = os.path.join(read_root, "src", "top.v")
        with open(top, encoding="utf-8", errors="replace") as handle:
            lines = handle.read().split("\n")

        def pos(pattern, token, start=0):
            for index in range(start, len(lines)):
                if re.search(pattern, lines[index]):
                    base = lines[index].index(token)
                    return {"line": index, "character": base + 1}
            raise SystemExit("anchor not found: %r / %r" % (pattern, token))

        top_uri = self.uri(top)
        module_pos = pos(r"^\s*module\s+top", "top")
        inst_pos = pos(r"^\s*sys_rstn\s+sys_rstn", "sys_rstn")
        inst_decl = pos(r"^\s*sys_rstn\s+sys_rstn", "sys_rstn(")
        wire_pos = pos(r"^\s*wire\s+clk_50M;", "clk_50M")
        port_line = next(i for i, line in enumerate(lines) if ".refclk" in line)
        dot_char = lines[port_line].index(".") + 1

        document = {"textDocument": {"uri": top_uri}}
        cases = [
            {"id": "documentSymbol", "method": "textDocument/documentSymbol",
             "params": dict(document), "note": "top.v"},
            {"id": "workspace/symbol", "method": "workspace/symbol",
             "params": {"query": "sys"}, "note": "query=sys"},
            {"id": "definition(instance)", "method": "textDocument/definition",
             "params": dict(document, position=inst_pos), "note": "on sys_rstn instance"},
            {"id": "hover(instance)", "method": "textDocument/hover",
             "params": dict(document, position=inst_pos)},
            {"id": "completion(port-list)", "method": "textDocument/completion",
             "params": dict(document, position={"line": port_line, "character": dot_char}),
             "note": "after '.' in .refclk(...)"},
            {"id": "references(instance)", "method": "textDocument/references",
             "params": dict(document, position=inst_pos,
                            context={"includeDeclaration": True})},
            {"id": "inlayHint", "method": "textDocument/inlayHint",
             "params": dict(document, range={"start": {"line": 0, "character": 0},
                                             "end": {"line": len(lines), "character": 0}})},
            {"id": "semanticTokens/full", "method": "textDocument/semanticTokens/full",
             "params": dict(document)},
            {"id": "foldingRange", "method": "textDocument/foldingRange",
             "params": dict(document)},
            {"id": "codeAction", "method": "textDocument/codeAction",
             "params": dict(document, range={"start": module_pos, "end": module_pos},
                            context={"diagnostics": []}),
             "note": "range at module header"},
            {"id": "prepareRename", "method": "textDocument/prepareRename",
             "params": dict(document, position=wire_pos), "note": "on clk_50M"},
            {"id": "rename", "method": "textDocument/rename",
             "params": dict(document, position=wire_pos, newName="clk_50M_probe")},
            {"id": "prepareCallHierarchy", "method": "textDocument/prepareCallHierarchy",
             "params": dict(document, position=inst_pos)},
            {"id": "callHierarchy/incomingCalls", "method": "callHierarchy/incomingCalls",
             "params": {"item": {"name": "sys_rstn", "kind": 2, "uri": top_uri,
                                 "range": {"start": inst_decl, "end": inst_decl},
                                 "selectionRange": {"start": inst_decl, "end": inst_decl},
                                 "data": {"module": "sys_rstn", "defined": True}}}},
            {"id": "callHierarchy/outgoingCalls", "method": "callHierarchy/outgoingCalls",
             "params": {"item": {"name": "top", "kind": 2, "uri": top_uri,
                                 "range": {"start": module_pos, "end": module_pos},
                                 "selectionRange": {"start": module_pos, "end": module_pos},
                                 "data": {"module": "top", "defined": True}}}},
            {"id": "formatting", "method": "textDocument/formatting",
             "params": dict(document, options={"tabSize": 4, "insertSpaces": True})},
            {"id": "rangeFormatting", "method": "textDocument/rangeFormatting",
             "params": dict(document, range={"start": {"line": 10, "character": 0},
                                             "end": {"line": 25, "character": 0}},
                            options={"tabSize": 4, "insertSpaces": True})},
            {"id": "fpgaLsp/moduleTree", "method": "fpgaLsp/moduleTree",
             "params": {"uri": top_uri}},
            {"id": "fpgaLsp/reports", "method": "fpgaLsp/reports",
             "params": {"uri": top_uri}},
            {"id": "fpgaLsp/flowStatus", "method": "fpgaLsp/flowStatus",
             "params": {"uri": top_uri}},
        ]
        self.results["anchors"] = {"module": module_pos, "instance": inst_pos,
                                   "wire": wire_pos,
                                   "port_line": port_line, "dot_char": dot_char}

        for case in cases:
            print("  matrix: %-28s" % case["id"], end="", flush=True)
            record = self.compare(case, read_root, read_root, args.timeout)
            py_sum = record["python"]["summary"]
            cpp_sum = record["cpp"]["summary"]
            print(" py=%-8s%s cpp=%-8s%s" % (
                py_sum["kind"], "(%s)" % py_sum["count"] if py_sum["count"] else "(empty)",
                cpp_sum["kind"], "(%s)" % cpp_sum["count"] if cpp_sum["count"] else "(empty)"))
            self.results["matrix"].append(record)

    # -- phase 3: workspace/executeCommand ----------------------------------
    # (command, argument builder, probed?, reason)
    def command_table(self, al_path, uri_top, sym_pos):
        project_uri = self.uri(al_path)
        return [
            ("fpgalsp.info", [], True, ""),
            ("fpgalsp.projects", [], True, ""),
            ("fpgalsp.tdInfo", [], True, ""),
            ("fpgalsp.device", [], True, "TD session, no hardware writes"),
            ("fpgalsp.reports", [], True, "Python: unknown command"),
            ("fpgalsp.runAnalysis", [], True, "placeholder both sides?"),
            ("fpgalsp.instantiate", [{"module": "top"}], True, ""),
            ("fpgalsp.copyInstantiation", [{"uri": uri_top}], True, ""),
            ("fpgalsp.symbolPath", [{"uri": uri_top, "position": sym_pos}], True, ""),
            ("fpgalsp.duplicateModules", [], True, ""),
            ("fpgalsp.searchHierarchy", [{"query": "sys_rstn"}], True, ""),
            ("fpgalsp.debugHighlight", [{"uri": uri_top}], True, ""),
            ("fpgalsp.addSourceFile", [{"file": "<PROJECT>/src/audit_added.v"}],
             True, "writes .al (copy only)"),
            ("fpgalsp.setProject", [{"al": "<PROJECT>/demo_project.al"}], True,
             "state only"),
            ("fpgalsp.setProjectName", [{"name": "demo_project_probe"}], True,
             "writes .al (copy only)"),
            ("fpgalsp.updateProject", [], True, "TD open_project -update, writes .al"),
            ("fpgalsp.check", [], True, "TD read_hdl, no hardware writes"),
            ("fpgalsp.restart", [], True, ""),
            ("fpgalsp.program", [{"confirm": True}], False, "not probed: side effects"),
            ("fpgalsp.svfgen", [], False, "not probed: side effects"),
            ("fpgalsp.cables", [], False, "not probed: side effects"),
            ("fpgalsp.runFlow", [{"stage": "syn"}], False, "not probed: side effects"),
            ("fpgalsp.compareStrategies", [], False, "not probed: side effects"),
            ("fpgalsp.cancelFlow", [], False, "not probed: side effects"),
        ]

    def run_commands(self):
        args = self.args
        roots = {"python": os.path.join(args.workdir, "project-py"),
                 "cpp": os.path.join(args.workdir, "project-cpp")}
        for side, root in roots.items():
            covered_copy(args.project, root,
                         extra_files={"src/audit_added.v": ADDED_HDL})

        entry = self.pair(roots["python"], roots["cpp"], "commands")
        # open top.v in both sessions so document-scoped commands work
        for side in ("python", "cpp"):
            root = roots[side]
            client = self.client_of(entry, side)
            client.open_document(os.path.join(root, "src", "top.v"), "verilog")

        al_py = os.path.join(roots["python"], "demo_project.al")
        al_cpp = os.path.join(roots["cpp"], "demo_project.al")
        with open(os.path.join(args.project, "src", "top.v"), encoding="utf-8",
                  errors="replace") as handle:
            lines = handle.read().split("\n")
        wire_line = next(i for i, line in enumerate(lines) if "clk_50M" in line
                         and line.strip().startswith("wire"))
        sym_pos = {"line": wire_line, "character": lines[wire_line].index("clk_50M") + 1}

        table_py = self.command_table(al_py, self.uri(os.path.join(roots["python"],
                                                                   "src", "top.v")), sym_pos)
        table_cpp = self.command_table(al_cpp, self.uri(os.path.join(roots["cpp"],
                                                                     "src", "top.v")), sym_pos)

        backups = os.path.join(args.workdir, "backup")
        os.makedirs(backups, exist_ok=True)
        for side, al in (("python", al_py), ("cpp", al_cpp)):
            shutil.copyfile(al, os.path.join(backups, "%s-before.al" % side))

        for index, (py_row, cpp_row) in enumerate(zip(table_py, table_cpp)):
            name, args_py, probed, reason = py_row
            args_cpp = cpp_row[1]
            record = {
                "index": index,
                "command": name,
                "arguments_python": shrink(args_py),
                "arguments_cpp": shrink(args_cpp),
                "probed": probed,
                "reason": reason,
            }
            if not probed:
                print("  command: %-30s SKIPPED (%s)" % (name, reason))
                record["python"] = {"skipped": True}
                record["cpp"] = {"skipped": True}
                self.results["commands"].append(record)
                continue

            # per-side .al backup + full tree diff around the single call
            record["sides"] = {}
            for side, al, al_local, call_args, client in (
                    ("python", al_py, al_py, args_py, entry["python"]),
                    ("cpp", al_cpp, al_cpp, args_cpp, entry["native"])):
                before = tree_snapshot(roots[side])
                shutil.copyfile(al, os.path.join(backups, "%02d-%s-before.al" % (index, side)))
                # "<PROJECT>/..." placeholders resolve to this side's copy
                resolved = json.loads(json.dumps(call_args).replace(
                    "<PROJECT>", roots[side].replace("\\", "/")))
                timeout = 420 if name in ("fpgalsp.check", "fpgalsp.device",
                                          "fpgalsp.updateProject") else args.timeout
                result = self.ask(client, "workspace/executeCommand",
                                  {"command": name, "arguments": resolved}, timeout)
                shutil.copyfile(al, os.path.join(backups, "%02d-%s-after.al" % (index, side)))
                after = tree_snapshot(roots[side])
                if name in ("fpgalsp.check", "fpgalsp.device", "fpgalsp.updateProject"):
                    time.sleep(2.0)  # let a queued TD worker settle before the next call
                record["sides"][side] = {
                    "ok": result["ok"], "elapsed_ms": result["elapsed_ms"],
                    "error": result.get("error"), "raw": shrink(result["result"]),
                    "payload_ok": (result["result"].get("ok")
                                   if isinstance(result["result"], dict) else None),
                    "payload_error": (result["result"].get("error")
                                      if isinstance(result["result"], dict) else None),
                    "summary": describe(result["result"]),
                    "al_sha_before": before.get("demo_project.al"),
                    "al_sha_after": after.get("demo_project.al"),
                    "al_changed": before.get("demo_project.al") != after.get("demo_project.al"),
                    "tree_diff": snapshot_diff(before, after),
                    "result": result["result"],
                }
            py_side = record["sides"]["python"]
            cpp_side = record["sides"]["cpp"]
            tokens = [("<PROJECT>", os.path.abspath(roots["python"])),
                      ("<PROJECT>", os.path.abspath(roots["cpp"]))]
            py_norm = {"ok": py_side["ok"], "result": normalize(py_side["result"], tokens),
                       "error": self._norm_error(py_side["error"], tokens)}
            cpp_norm = {"ok": cpp_side["ok"], "result": normalize(cpp_side["result"], tokens),
                        "error": self._norm_error(cpp_side["error"], tokens)}
            record["python"] = {"ok": py_side["ok"], "elapsed_ms": py_side["elapsed_ms"],
                                "error": py_side["error"], "raw": py_side["raw"],
                                "payload_ok": py_side["payload_ok"],
                                "payload_error": py_side["payload_error"],
                                "normalized": shrink(py_norm),
                                "summary": py_side["summary"],
                                "al_changed": py_side["al_changed"],
                                "tree_diff": py_side["tree_diff"],
                                "digest": digest(py_norm)}
            record["cpp"] = {"ok": cpp_side["ok"], "elapsed_ms": cpp_side["elapsed_ms"],
                             "error": cpp_side["error"], "raw": cpp_side["raw"],
                             "payload_ok": cpp_side["payload_ok"],
                             "payload_error": cpp_side["payload_error"],
                             "normalized": shrink(cpp_norm),
                             "summary": cpp_side["summary"],
                             "al_changed": cpp_side["al_changed"],
                             "tree_diff": cpp_side["tree_diff"],
                             "digest": digest(cpp_norm)}
            record["identical_after_normalization"] = (
                digest(py_norm) == digest(cpp_norm))
            # the side payloads keep full results out of the report except previews
            for side in ("python", "cpp"):
                del record["sides"][side]["result"]
            print("  command: %-30s py=%-6s cpp=%-6s al_changed py=%s cpp=%s" % (
                name,
                record["python"]["ok"], record["cpp"]["ok"],
                record["python"]["al_changed"], record["cpp"]["al_changed"]))
            self.results["commands"].append(record)

        self.results["command_backups"] = backups

    # -- phase 4: boundary cases --------------------------------------------
    def run_boundaries(self):
        args = self.args
        root = os.path.join(args.workdir, "project-boundary")
        covered_copy(args.project, root,
                     extra_files={"src/audit_boundary.v": BOUNDARY_HDL,
                                  "src/audit_empty.v": ""})
        register_in_al(os.path.join(root, "demo_project.al"),
                       [("src/audit_boundary.v", 20), ("src/audit_empty.v", 21)])
        self.pair(root, root, "boundary")
        entry = self.pairs[-1]

        def load(relative):
            path = os.path.join(root, relative.replace("/", os.sep))
            with open(path, encoding="utf-8", errors="replace") as handle:
                return path, handle.read()

        boundary_path, boundary_text = load("src/audit_boundary.v")
        boundary_lines = boundary_text.split("\n")
        empty_path, empty_text = load("src/audit_empty.v")
        adc_path, adc_text = load("constrains/pin.adc")
        sdc_path, sdc_text = load("constrains/top.sdc")

        emoji_line = next(i for i, line in enumerate(boundary_lines)
                          if "emoji_after_comment;" in line and "/*" in line)
        emoji_col = utf16_col(boundary_lines[emoji_line], "emoji_after_comment")
        live_line = next(i for i, line in enumerate(boundary_lines)
                         if "module audit_ifdef_live" in line)
        dead_line = next(i for i, line in enumerate(boundary_lines)
                         if "module audit_ifdef_dead" in line)
        q_line = next(i for i, line in enumerate(boundary_lines)
                      if re.search(r"q <= q \+ 1'b1", line))

        documents = {
            "empty": (empty_path, empty_text, "verilog"),
            "boundary": (boundary_path, boundary_text, "verilog"),
            "adc": (adc_path, adc_text, "adc"),
            "sdc": (sdc_path, sdc_text, "sdc"),
        }
        # didOpen every document up front: both servers must see the same buffers
        for side in ("python", "cpp"):
            client = self.client_of(entry, side)
            for label, (path, text, language) in documents.items():
                client.open_document(path, language, text=text)
        # let the (async) diagnostic pass settle so it cannot interleave later
        time.sleep(1.0)

        def doc(label):
            return {"textDocument": {"uri": self.uri(documents[label][0])}}

        whole = lambda label, lines: {"range": {"start": {"line": 0, "character": 0},
                                                "end": {"line": max(0, len(lines)), "character": 0}}}

        cases = []
        for label in ("empty", "boundary"):
            path, text, _language = documents[label]
            lines = text.split("\n")
            cases += [
                (label, "documentSymbol", "textDocument/documentSymbol", doc(label)),
                (label, "hover", "textDocument/hover",
                 dict(doc(label), position={"line": 0, "character": 0})),
                (label, "definition", "textDocument/definition",
                 dict(doc(label), position={"line": 0, "character": 0})),
                (label, "completion", "textDocument/completion",
                 dict(doc(label), position={"line": 0, "character": 0})),
                (label, "references", "textDocument/references",
                 dict(doc(label), position={"line": 0, "character": 0},
                      context={"includeDeclaration": True})),
                (label, "inlayHint", "textDocument/inlayHint",
                 dict(doc(label), **whole(label, lines))),
                (label, "semanticTokens/full", "textDocument/semanticTokens/full", doc(label)),
                (label, "foldingRange", "textDocument/foldingRange", doc(label)),
                (label, "codeAction", "textDocument/codeAction",
                 dict(doc(label), range={"start": {"line": 0, "character": 0},
                                         "end": {"line": 0, "character": 0}},
                      context={"diagnostics": []})),
            ]
        # UTF-16 / ifdef specific probes on the boundary file
        boundary_doc = doc("boundary")
        cases += [
            ("boundary", "hover(utf16 emoji line)", "textDocument/hover",
             dict(boundary_doc, position={"line": emoji_line, "character": emoji_col})),
            ("boundary", "definition(utf16 emoji line)", "textDocument/definition",
             dict(boundary_doc, position={"line": emoji_line, "character": emoji_col})),
            ("boundary", "hover(live branch module)", "textDocument/hover",
             dict(boundary_doc, position={"line": live_line,
                                          "character": boundary_lines[live_line].index("audit_ifdef_live") + 1})),
            ("boundary", "hover(dead branch module)", "textDocument/hover",
             dict(boundary_doc, position={"line": dead_line,
                                          "character": boundary_lines[dead_line].index("audit_ifdef_dead") + 1})),
            ("boundary", "hover(q in always)", "textDocument/hover",
             dict(boundary_doc, position={"line": q_line,
                                          "character": boundary_lines[q_line].index("q <= q") + 1})),
            ("boundary", "prepareRename(utf16)", "textDocument/prepareRename",
             dict(boundary_doc, position={"line": emoji_line, "character": emoji_col})),
            ("boundary", "formatting", "textDocument/formatting",
             dict(boundary_doc, options={"tabSize": 4, "insertSpaces": True})),
        ]
        for label in ("adc", "sdc"):
            path, text, _language = documents[label]
            lines = text.split("\n")
            cases += [
                ("%s" % label, "documentSymbol", "textDocument/documentSymbol", doc(label)),
                ("%s" % label, "hover", "textDocument/hover",
                 dict(doc(label), position={"line": 0, "character": 0})),
                ("%s" % label, "completion", "textDocument/completion",
                 dict(doc(label), position={"line": 0, "character": 0})),
                ("%s" % label, "inlayHint", "textDocument/inlayHint",
                 dict(doc(label), **whole(label, lines))),
                ("%s" % label, "semanticTokens/full", "textDocument/semanticTokens/full",
                 doc(label)),
                ("%s" % label, "foldingRange", "textDocument/foldingRange", doc(label)),
                ("%s" % label, "codeAction", "textDocument/codeAction",
                 dict(doc(label), range={"start": {"line": 0, "character": 0},
                                         "end": {"line": 8, "character": 0}},
                      context={"diagnostics": []})),
                ("%s" % label, "formatting", "textDocument/formatting",
                 dict(doc(label), options={"tabSize": 4, "insertSpaces": True})),
                ("%s" % label, "prepareRename", "textDocument/prepareRename",
                 dict(doc(label), position={"line": 0, "character": 0})),
            ]

        self.results["boundary_anchor"] = {"emoji_line": emoji_line,
                                           "emoji_character": emoji_col,
                                           "live_line": live_line,
                                           "dead_line": dead_line, "q_line": q_line}
        for file_label, name, method, params in cases:
            case = {"id": "%s:%s" % (file_label, name), "method": method,
                    "params": params, "note": ""}
            print("  boundary: %-36s" % case["id"], end="", flush=True)
            record = self.compare(case, root, root, args.timeout)
            record["file"] = file_label
            record["case"] = name
            print(" py=%s/%s cpp=%s/%s" % (
                record["python"]["summary"]["kind"],
                record["python"]["summary"]["count"],
                record["cpp"]["summary"]["kind"],
                record["cpp"]["summary"]["count"]))
            self.results["boundaries"].append(record)

    # -- orchestration ------------------------------------------------------
    def run(self):
        self.run_matrix()
        self.run_commands()
        self.run_boundaries()


def main(argv=None):
    here = os.path.dirname(os.path.abspath(__file__))
    cpp_root = os.path.dirname(os.path.dirname(here))
    default_python_repo = os.path.join(os.path.dirname(cpp_root), "fpga-lsp")
    default_workdir = os.path.join(os.environ.get("TEMP", "/tmp"), "fpgalsp-parity-lsp")

    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--python-log", default=os.path.join(default_workdir,
                                                             "logs", "python.log"),
                        help="log file for the Python reference server")
    parser.add_argument("--native-exe", default=os.path.join(cpp_root, "build", "bin",
                                                             "fpgalsp-native.exe"),
                        help="fpgalsp-native.exe")
    parser.add_argument("--native-log", default=os.path.join(default_workdir,
                                                             "logs", "native.log"),
                        help="log file for the native server")
    parser.add_argument("--project", default="%WORKSPACE%/CorpusAlpha",
                        help="project directory to copy and probe (never written to)")
    parser.add_argument("--python-repo", default=default_python_repo,
                        help="checkout of the Python reference implementation")
    parser.add_argument("--cpp-root", default=cpp_root,
                        help="checkout of the C++ rewrite (goldenlib lives here)")
    parser.add_argument("--workdir", default=default_workdir,
                        help="scratch root for the project copies")
    parser.add_argument("--out", default=None, help="write the full JSON report here")
    parser.add_argument("--timeout", type=float, default=120.0,
                        help="per-request timeout in seconds (TD commands get 420)")
    parser.add_argument("--only", default="all",
                        choices=["all", "matrix", "commands", "boundary"],
                        help="restrict the probe to one phase")
    args = parser.parse_args(argv)

    args.workdir = os.path.abspath(args.workdir)
    temp_root = os.path.abspath(os.environ.get("TEMP", "/tmp"))
    if not args.workdir.startswith(temp_root):
        raise SystemExit("refusing to use a workdir outside %%TEMP%%: %s" % args.workdir)
    os.makedirs(args.workdir, exist_ok=True)
    if not os.path.isdir(args.project):
        raise SystemExit("project directory not found: %s" % args.project)
    if not os.path.isfile(args.native_exe):
        raise SystemExit("native exe not found: %s" % args.native_exe)
    os.makedirs(os.path.dirname(os.path.abspath(args.python_log)), exist_ok=True)
    os.makedirs(os.path.dirname(os.path.abspath(args.native_log)), exist_ok=True)

    sys.path.insert(0, os.path.join(args.cpp_root, "tests", "golden", "tools"))
    from goldenlib.client import (  # noqa: E402  (path bootstrap)
        LspClient, NativeLspClient, TransportError, path_to_uri, uri_to_path)

    clients = {"LspClient": LspClient, "NativeLspClient": NativeLspClient,
               "TransportError": TransportError, "path_to_uri": path_to_uri,
               "uri_to_path": uri_to_path}

    with open(args.native_exe, "rb") as handle:
        native_sha = hashlib.sha256(handle.read()).hexdigest()

    started = time.time()
    prober = Prober(args, clients)
    error = None
    try:
        if args.only == "all":
            prober.run()
        else:
            if args.only == "matrix":
                prober.run_matrix()
            elif args.only == "commands":
                prober.run_commands()
            elif args.only == "boundary":
                prober.run_boundaries()
    except Exception:  # noqa: BLE001 - always write what we have
        error = traceback.format_exc()
        print(error, file=sys.stderr)
    finally:
        prober.close_all()

    report = {
        "meta": {
            "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
            "duration_s": round(time.time() - started, 1),
            "argv": sys.argv,
            "python_repo": os.path.abspath(args.python_repo),
            "cpp_root": os.path.abspath(args.cpp_root),
            "native_exe": os.path.abspath(args.native_exe),
            "native_sha256": native_sha,
            "project_source": os.path.abspath(args.project),
            "workdir": args.workdir,
            "python_log": args.python_log,
            "native_log": args.native_log,
            "python_version": sys.version.split()[0],
            "normalization": {
                "masked_keys": sorted(MASK_KEYS),
                "path_tokens": ["<PROJECT> for each side's project copy",
                                "<timestamp> for wall-clock looking strings"],
                "note": ("raw payloads are shrunk for storage: arrays >8 keep len "
                         "plus head/tail, strings >400 chars are cut; the digest "
                         "is computed on the full normalized payload"),
            },
            "error": error,
        },
    }
    report.update(prober.results)
    if args.out:
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        with open(args.out, "w", encoding="utf-8") as handle:
            json.dump(report, handle, ensure_ascii=False, indent=1)
        print("\nwrote %s (%d bytes)" % (args.out, os.path.getsize(args.out)))
    return 0 if error is None else 1


if __name__ == "__main__":
    raise SystemExit(main())