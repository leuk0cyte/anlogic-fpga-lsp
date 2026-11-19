"""Collect golden snapshots from the fpga-lsp Python reference implementation.

The snapshots are the contract that the C++ rewrite is diffed against.  Two runs over the
same corpus must produce byte-identical files; see ``normalize.py`` for the rules that make
that true and ``README.md`` for the acceptance convention.

Run from anywhere::

    python tests/golden/tools/collect.py             # collect every category
    python tests/golden/tools/collect.py --only hdl  # one category
    python tests/golden/tools/collect.py --clean      # wipe snapshots first

Exit code is 0 when the collector itself stayed healthy.  Individual cases that could not be
collected (missing project, TD unavailable, timeout, JSON-RPC error) are recorded in
``_meta.json`` as ``skipped`` with a reason, and the run still succeeds: a flaky environment
must not break the pipeline that consumes these snapshots.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import sys
import time
import traceback

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
GOLDEN_DIR = os.path.dirname(TOOLS_DIR)
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

from goldenlib.client import (  # noqa: E402
    LspClient, McpClient, NativeLspClient, NativeMcpClient, TransportError,
    default_native_exe, path_to_uri,
)
from goldenlib.normalize import (  # noqa: E402
    Normalizer, build_snapshot, dump_json, write_snapshot, _digest, _measure,
)

DEFAULT_CORPUS = os.path.join(GOLDEN_DIR, "corpus", "cases.json")
DEFAULT_SNAPSHOTS = os.path.join(GOLDEN_DIR, "snapshots")
DEFAULT_SANDBOX = os.path.join(GOLDEN_DIR, "_sandbox")

HDL_EXTENSIONS = (".v", ".sv", ".vh", ".svh")
CATEGORIES = ("project", "hdl", "semantics", "hierarchy", "diagnostics", "mcp")

# Caps that keep a snapshot collection run bounded.  These are part of the corpus definition:
# changing them changes the golden set, so they are recorded in _meta.json.
MAX_PROJECT_HDL_FILES = 10
MAX_POSITIONS_PER_KIND = 3
MAX_REFERENCES_CASES = 6
MAX_SEMANTIC_FILES = 8
MAX_MCP_POSITIONS = 4

# A tool result is inlined verbatim when it is this small; otherwise only a digest is kept (the
# tool name and arguments are always preserved so a diff names the offending tool).
MCP_INLINE_MAX_LINES = 40
MCP_INLINE_MAX_BYTES = 4000

COMMONWORDS = re.compile(r"\b(module|endmodule|input|output|inout|wire|reg|assign|always|begin|end)$")


# --------------------------------------------------------------------------- helpers
def log(message):
    sys.stderr.write(message + "\n")
    sys.stderr.flush()


def load_corpus(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def display_path(path, roots):
    """Corpus-root relative spelling used as a stable case identifier."""
    normalizer = Normalizer(roots)
    token = normalizer.relative_token(path)
    return token if token else path


def describe_missing(path):
    """Skip reason for an absent corpus root, already normalised and slash-stable."""
    return "project directory not found: %s" % path.replace("\\", "/")


def slugify(text):
    slug = re.sub(r"[^A-Za-z0-9_.-]+", "-", text).strip("-")
    return slug or "case"


def detect_language(path):
    lower = path.lower()
    if lower.endswith((".v", ".sv", ".vh", ".svh")):
        return "verilog"
    if lower.endswith(".vhd") or lower.endswith(".vhdl"):
        return "vhdl"
    if lower.endswith(".adc"):
        return "adc"
    if lower.endswith(".sdc"):
        return "sdc"
    if lower.endswith(".al"):
        return "xml"
    return "plaintext"


def read_text(path):
    with open(path, encoding="utf-8", errors="replace") as handle:
        return handle.read()


def strip_comments(text):
    """Remove // and /* */ comments so declaration harvesting ignores commented-out code."""
    text = re.sub(r"/\*.*?\*/", lambda m: " " * len(m.group(0)), text, flags=re.S)
    return re.sub(r"//[^\n]*", lambda m: " " * len(m.group(0)), text)


def line_col_to_offset(text, line, character):
    lines = text.split("\n")
    offset = 0
    for index in range(min(line, len(lines))):
        offset += len(lines[index]) + 1
    return offset + character


def offset_to_line_col(text, offset):
    offset = max(0, min(offset, len(text)))
    line = text.count("\n", 0, offset)
    tail = text.rfind("\n", 0, offset)
    return line, (offset if tail < 0 else offset - tail - 1)


KEYWORDS = {
    "module", "endmodule", "input", "output", "inout", "wire", "reg", "assign", "always",
    "always_ff", "always_comb", "begin", "end", "if", "else", "case", "endcase", "for",
    "while", "initial", "parameter", "localparam", "integer", "genvar", "generate",
    "endgenerate", "function", "endfunction", "task", "endtask", "posedge", "negedge",
    "default", "signed", "unsigned", "logic", "bit", "byte", "int", "return", "else if",
    "timescale", "include", "define", "ifdef", "endif", "else", "endif", "repeat", "forever",
}


class DocPositions:
    """Deterministic harvest of symbol positions from an HDL document.

    Positions are chosen without asking the server anything, so the same corpus always yields
    the same probes.  Each kind is sampled at most ``limit`` times, in source order.
    """

    def __init__(self, text):
        self.text = strip_comments(text.replace("\r\n", "\n").replace("\r", "\n"))
        self.lines = self.text.split("\n")

    def _positions(self, pattern, limit, require_module_context=True):
        found = []
        module_start = 0
        for index, row in enumerate(self.lines):
            if re.match(r"\s*module\b", row):
                module_start = index
            match = pattern.search(row)
            if not match:
                continue
            if require_module_context and index < module_start:
                continue
            word = match.group("name")
            if word in KEYWORDS:
                continue
            found.append({"line": index, "character": match.start("name") + 1,
                          "token": word})
            if len(found) >= limit:
                break
        return found

    def module_name(self, limit):
        return self._positions(
            re.compile(r"^\s*module\s+(?P<name>[A-Za-z_][A-Za-z0-9_$]*)"), limit,
            require_module_context=False)

    def port_name(self, limit):
        return self._positions(
            re.compile(r"^\s*(?:input|output|inout)\b[^;]*?\b(?P<name>[A-Za-z_][A-Za-z0-9_$]*)\s*(?:[,;)]|$)"),
            limit)

    def instance_name(self, limit):
        # <module> [#(...)] <instance> (
        return self._positions(
            re.compile(r"^\s*(?P<name>[A-Za-z_][A-Za-z0-9_$]*)\s*(?:#\s*\([^;]*?\)\s*)?"
                       r"[A-Za-z_][A-Za-z0-9_$]*\s*\("),
            limit)

    def signal_name(self, limit):
        return self._positions(
            re.compile(r"^\s*(?:wire|reg|logic)\b[^;]*?\b(?P<name>[A-Za-z_][A-Za-z0-9_$]*)\s*(?:[,;=\[]|$)"),
            limit)


class Recorder:
    """Accumulates per-category case results and their skip reasons."""

    def __init__(self, category):
        self.category = category
        self.cases = []
        self.skipped = []

    def add(self, case_id, payload, note=None):
        entry = {"id": case_id, "result": payload}
        if note:
            entry["note"] = note
        self.cases.append(entry)

    def skip(self, case_id, reason):
        self.skipped.append({"id": case_id, "reason": reason})
        log("    SKIP %-46s %s" % (case_id, reason))

    def error(self, case_id, reason):
        self.skipped.append({"id": case_id, "reason": reason, "error": True})
        log("    ERROR %-45s %s" % (case_id, reason))


# --------------------------------------------------------------------------- corpus prep
# Directories never copied into the sandbox work copy: agent/edit state and build output that no
# read-only collection needs, and VCS metadata that would only slow the copy down.
SANDBOX_EXCLUDE_DIRS = frozenset({
    ".fpgalsp", ".git", ".svn", "__pycache__", "node_modules",
})
# Files never copied: anything regenerated by TD or by an editor.
SANDBOX_EXCLUDE_SUFFIXES = (".log", ".logw", ".bak", ".tmp", ".swp")


def copy_project_to_sandbox(source_root, target_root):
    """Copy a project tree into a writable sandbox location.

    The reference implementation writes TD caches and run logs into ``<project>/.fpgalsp/``
    (``cache_dir = os.path.join(project.dir, ".fpgalsp", "cache")`` in server.py and
    mcp_bridge.py, and the TD session cwd is ``<project>/.fpgalsp/run``).  That path is derived
    from ``project.dir``, so the only way to collect TD-backed results without touching the
    user's real project is to run against a copy: ``project.dir`` then points at the sandbox.

    ``.al`` files reference their sources relatively, so the copy keeps resolving after the
    whole tree moves -- verified by re-parsing the copied ``.al`` and checking every recorded
    source path still exists (see :func:`verify_copied_project`).

    Returns ``(copied_file_count, skipped_entries)``.
    """
    copied = 0
    skipped = []
    os.makedirs(target_root, exist_ok=True)
    for dirpath, dirnames, filenames in os.walk(source_root):
        rel_dir = os.path.relpath(dirpath, source_root)
        dirnames[:] = sorted(name for name in dirnames
                             if name.lower() not in SANDBOX_EXCLUDE_DIRS)
        for name in dirnames:
            os.makedirs(os.path.join(target_root, rel_dir, name)
                        if rel_dir != "." else os.path.join(target_root, name),
                        exist_ok=True)
        for name in sorted(filenames):
            if name.lower().endswith(SANDBOX_EXCLUDE_SUFFIXES):
                skipped.append(os.path.join(rel_dir, name))
                continue
            src = os.path.join(dirpath, name)
            dst = (os.path.join(target_root, name) if rel_dir == "."
                   else os.path.join(target_root, rel_dir, name))
            try:
                shutil.copy2(src, dst)
                copied += 1
            except OSError as exc:
                skipped.append("%s (%s)" % (os.path.join(rel_dir, name), exc))
    return copied, skipped


def verify_copied_project(copied_root, reference_root):
    """Re-parse the copied ``.al`` and confirm every referenced source resolves inside it.

    Returns ``(ok, detail)``.  ``ok`` is False when the reference implementation would look for
    a file outside the copy, which is exactly the case that must not reach a real project.
    """
    al_files = [name for name in os.listdir(copied_root) if name.lower().endswith(".al")]
    if not al_files:
        return False, "no .al in the copy"
    try:
        if reference_root not in sys.path:
            sys.path.insert(0, reference_root)
        from fpgalsp.project import parse_al
    except Exception as exc:                              # noqa: BLE001
        return False, "cannot import reference parser: %s" % exc
    root_norm = os.path.normcase(os.path.abspath(copied_root))
    recorded = []
    try:
        project = parse_al(os.path.join(copied_root, sorted(al_files)[0]))
        for getter in ("hdl_files", "adc_files", "sdc_files"):
            method = getattr(project, getter, None)
            if method is None:
                continue
            for entry in method():
                recorded.append(getattr(entry, "path", str(entry)))
    except Exception as exc:                              # noqa: BLE001
        return False, "parse_al failed: %s" % exc

    outside, missing = [], []
    for path in recorded:
        absolute = os.path.normcase(os.path.abspath(path))
        if not absolute.startswith(root_norm):
            outside.append(path)
        elif not os.path.isfile(absolute):
            missing.append(path)
    if outside:
        return False, "%d recorded path(s) point outside the copy, e.g. %s" % (
            len(outside), outside[0])
    if missing:
        return False, "%d recorded path(s) missing from the copy, e.g. %s" % (
            len(missing), missing[0])
    return True, "%d recorded source paths all resolve inside the copy" % len(recorded)


def _reference_sys_path(path):
    """Walk up from ``path`` to whichever ancestor holds the ``fpgalsp`` package."""
    current = os.path.abspath(path)
    for _ in range(8):
        if os.path.isdir(os.path.join(current, "fpgalsp")):
            return current
        parent = os.path.dirname(current)
        if parent == current:
            break
        current = parent
    return ""


class Corpus:
    """Resolved view of ``cases.json`` plus the derived per-project HDL file list."""

    def __init__(self, spec, sandbox_dir):
        self.spec = spec
        self.sandbox_dir = sandbox_dir
        self.projects = {}          # id -> {"root", "al", "files": [...], "skipped": reason}
        self.skipped_projects = {}
        self.synthetic = []         # list of absolute HDL paths inside the sandbox

    def resolve(self):
        for entry in self.spec.get("projects", []):
            project_id = entry["id"]
            root = entry["root"]
            info = {"root": root, "al": None, "files": [], "skipped": None,
                    "expectedDevice": entry.get("expectedDevice")}
            if not os.path.isdir(root):
                info["skipped"] = "project directory not found: %s" % root
                self.projects[project_id] = info
                self.skipped_projects[project_id] = info["skipped"]
                continue
            al_path = None
            if entry.get("al"):
                candidate = os.path.join(root, entry["al"])
                if os.path.isfile(candidate):
                    al_path = candidate
            if al_path is None:
                for name in sorted(os.listdir(root)):
                    if name.lower().endswith(".al"):
                        al_path = os.path.join(root, name)
                        break
            if al_path is None:
                info["skipped"] = "no .al project file under %s" % root
                self.projects[project_id] = info
                self.skipped_projects[project_id] = info["skipped"]
                continue
            info["al"] = al_path
            info["files"] = self._hdl_files(root)
            self.projects[project_id] = info
        self._prepare_synthetic()

    @staticmethod
    def _hdl_files(root):
        found = []
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = sorted(d for d in dirnames
                                 if d not in (".fpgalsp", ".git", "__pycache__"))
            for name in sorted(filenames):
                if name.lower().endswith(HDL_EXTENSIONS):
                    found.append(os.path.join(dirpath, name))
        return found

    def _prepare_synthetic(self):
        """Materialise ``syntheticCases`` into the sandbox (never into the reference repo)."""
        os.makedirs(self.sandbox_dir, exist_ok=True)
        for case in self.spec.get("syntheticCases", []):
            rel = case.get("file") or (case["id"] + ".sv")
            target = os.path.join(self.sandbox_dir, rel)
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with open(target, "w", encoding="utf-8", newline="\n") as handle:
                handle.write(case["text"])
            self.synthetic.append(target)

    def usable(self):
        return [pid for pid, info in self.projects.items() if not info["skipped"]]

    def hdl_files(self, project_id, limit=None):
        files = self.projects[project_id]["files"]
        return files if limit is None else files[:limit]


def build_roots(corpus, repo_root):
    roots = {"repo": repo_root, "sandbox": corpus.sandbox_dir}
    for project_id, info in corpus.projects.items():
        if not info["skipped"]:
            roots["corpus:" + project_id] = info["root"]
    return roots


# --------------------------------------------------------------------------- categories
def collect_project(client, corpus, recorder, normalizer):
    for project_id, info in corpus.projects.items():
        if info["skipped"]:
            recorder.skip(project_id, info["skipped"])
            continue
        log("  project %s" % project_id)
        payload = {"info": None, "projects": None, "device": None, "errors": {}}
        for command in ("fpgalsp.info", "fpgalsp.projects", "fpgalsp.device"):
            try:
                payload[command.split(".")[1]] = client.request(
                    "workspace/executeCommand",
                    {"command": command, "arguments": []}, timeout=180)
            except TransportError as exc:
                payload["errors"][command] = str(exc)
        recorder.add(project_id, normalizer.normalize(payload))


def collect_hdl(client, corpus, recorder, normalizer, roots):
    for project_id, info in corpus.projects.items():
        if info["skipped"]:
            recorder.skip(project_id, info["skipped"])
            continue
        files = corpus.hdl_files(project_id, MAX_PROJECT_HDL_FILES)
        if not files:
            recorder.skip(project_id, "no HDL files found")
            continue
        for path in files:
            case_id = "%s::%s" % (project_id, normalizer.relative_token(path) or os.path.basename(path))
            log("  hdl %s" % case_id)
            try:
                text = client.open_document(path, detect_language(path))
                if len(info["files"]) and path == info["files"][0]:
                    client.wait_diagnostics_for(path, timeout=120)
                probes = DocPositions(text)
                entry = {
                    "documentSymbol": client.request(
                        "textDocument/documentSymbol",
                        {"textDocument": {"uri": path_to_uri(path)}}, timeout=180),
                    "workspaceSymbol": None,
                    "definition": [],
                    "references": [],
                    "hover": [],
                }
                query = probes.module_name(1)[0]["token"] if probes.module_name(1) else None
                if query:
                    entry["workspaceSymbol"] = client.request(
                        "workspace/symbol", {"query": query}, timeout=180)
                budget = {"references": MAX_REFERENCES_CASES}
                for kind, positions in (("module", probes.module_name(MAX_POSITIONS_PER_KIND)),
                                        ("port", probes.port_name(MAX_POSITIONS_PER_KIND)),
                                        ("instance", probes.instance_name(MAX_POSITIONS_PER_KIND))):
                    for position in positions:
                        params = {"textDocument": {"uri": path_to_uri(path)},
                                  "position": {"line": position["line"],
                                               "character": position["character"]}}
                        record = {"kind": kind, "line": position["line"],
                                  "character": position["character"], "token": position["token"]}
                        try:
                            record["definition"] = client.request(
                                "textDocument/definition", params, timeout=120)
                        except TransportError as exc:
                            record["definitionError"] = str(exc)
                        try:
                            record["hover"] = client.request(
                                "textDocument/hover", params, timeout=120)
                        except TransportError as exc:
                            record["hoverError"] = str(exc)
                        if budget["references"] > 0:
                            budget["references"] -= 1
                            try:
                                record["references"] = client.request(
                                    "textDocument/references",
                                    dict(params, context={"includeDeclaration": True}),
                                    timeout=120)
                            except TransportError as exc:
                                record["referencesError"] = str(exc)
                        # Each probe carries its own kind so the diff names which class of
                        # symbol regressed (module vs port vs instance).
                        entry["definition"].append(record)
                recorder.add(case_id, normalizer.normalize(entry))
            except TransportError as exc:
                recorder.error(case_id, str(exc)[:400])
            except Exception as exc:                     # noqa: BLE001 - never abort a run
                recorder.error(case_id, "%s: %s" % (type(exc).__name__, exc))
                log(traceback.format_exc())


def collect_semantics(client, corpus, recorder, normalizer):
    collected = 0
    for project_id, info in corpus.projects.items():
        if info["skipped"]:
            recorder.skip(project_id, info["skipped"])
            continue
        for path in corpus.hdl_files(project_id, MAX_PROJECT_HDL_FILES):
            if collected >= MAX_SEMANTIC_FILES:
                break
            collected += 1
            case_id = "%s::%s" % (project_id, normalizer.relative_token(path) or os.path.basename(path))
            log("  semantics %s" % case_id)
            try:
                client.open_document(path, detect_language(path))
                uri = path_to_uri(path)
                entry = {}
                for field, method, params in (
                    ("semanticTokens", "textDocument/semanticTokens/full",
                     {"textDocument": {"uri": uri}}),
                    ("foldingRange", "textDocument/foldingRange",
                     {"textDocument": {"uri": uri}}),
                    ("documentSymbol", "textDocument/documentSymbol",
                     {"textDocument": {"uri": uri}}),
                    ("inlayHint", "textDocument/inlayHint",
                     {"textDocument": {"uri": uri},
                      "range": {"start": {"line": 0, "character": 0},
                                "end": {"line": 100000, "character": 0}}}),
                ):
                    try:
                        entry[field] = client.request(method, params, timeout=180)
                    except TransportError as exc:
                        entry[field] = None
                        entry[field + "Error"] = str(exc)
                recorder.add(case_id, normalizer.normalize(entry))
            except TransportError as exc:
                recorder.error(case_id, str(exc)[:400])
            except Exception as exc:                     # noqa: BLE001
                recorder.error(case_id, "%s: %s" % (type(exc).__name__, exc))


def collect_hierarchy(client, corpus, recorder, normalizer):
    for project_id, info in corpus.projects.items():
        if info["skipped"]:
            recorder.skip(project_id, info["skipped"])
            continue
        log("  hierarchy %s" % project_id)
        try:
            result = client.request("fpgaLsp/moduleTree",
                                    {"uri": path_to_uri(info["root"])}, timeout=300)
            recorder.add(project_id, normalizer.normalize(result))
        except TransportError as exc:
            recorder.error(project_id, str(exc)[:400])


def compact_tool_result(payload):
    """Reduce bulkiest-first at the *tool* level so the category file keeps every tool.

    A single MCP result can be larger than the whole snapshot budget (``fpga_document_symbols``
    on a big module is ~47 KB).  Left to the generic shrinker, one such case collapses the whole
    category into digests and the file stops being diffable.  Here each tool keeps its name,
    arguments and a digest of its result, so a diff still points at the exact tool that
    regressed.
    """
    result = payload.get("result")
    if result is None:
        return payload
    size = _measure(result)
    if size[1] <= MCP_INLINE_MAX_LINES and size[0] <= MCP_INLINE_MAX_BYTES:
        return payload
    out = {"arguments": payload.get("arguments"),
           "resultShape": _digest(result, "tool result summarised to fit the snapshot size limit")}
    for key in ("isError", "error", "skipped"):
        if key in payload:
            out[key] = payload[key]
    if isinstance(result, dict):
        out["resultKeys"] = sorted(result.keys())
    return out


def collect_mcp(corpus_spec, corpus, recorder, normalizer, repo_root, sandbox_log_dir, allow_td,
                mcp_factory=None):
    mcp_factory = mcp_factory or (lambda root, log: McpClient(repo_root, root, log))
    tools_spec = corpus_spec.get("mcpTools") or {}
    read_only = tools_spec.get("readOnly") or []
    for project_id, info in corpus.projects.items():
        if info["skipped"]:
            recorder.skip(project_id, info["skipped"])
            continue
        log("  mcp %s" % project_id)
        client = None
        try:
            client = mcp_factory(info["root"],
                                 os.path.join(sandbox_log_dir, "mcp-%s.log" % project_id))
            listing = client.list_tools()
            available = {tool.get("name") for tool in listing.get("tools") or []}
            entry = {"tools": {}, "toolCount": len(available)}
            for name in read_only:
                if name not in available:
                    entry["tools"][name] = {"skipped": "tool not exposed by server"}
                    continue
                if name == "fpga_device_info" and not allow_td:
                    entry["tools"][name] = {"skipped": "TD unavailable"}
                    continue
                arguments = mcp_arguments(name, corpus, project_id, info)
                if arguments is None:
                    entry["tools"][name] = {"skipped": "no suitable fixed arguments for corpus"}
                    continue
                try:
                    payload = {
                        "arguments": mcp_redact_arguments(name, arguments, normalizer),
                        "result": None,
                    }
                    result, is_error = client.call_tool(name, arguments, timeout=600)
                    payload["result"] = result
                    payload["isError"] = is_error
                except TransportError as exc:
                    payload = {"arguments": mcp_redact_arguments(name, arguments, normalizer),
                               "error": str(exc)[:400]}
                entry["tools"][name] = compact_tool_result(payload)
            recorder.add(project_id, normalizer.normalize(entry))
        except TransportError as exc:
            recorder.error(project_id, str(exc)[:400])
        except Exception as exc:                         # noqa: BLE001
            recorder.error(project_id, "%s: %s" % (type(exc).__name__, exc))
            log(traceback.format_exc())
        finally:
            if client is not None:
                client.close(graceful=False)


def mcp_redact_arguments(name, arguments, normalizer):
    """Arguments are recorded in the snapshot, so they must be path-normalised too."""
    return normalizer.normalize(dict(arguments))


def mcp_arguments(name, corpus, project_id, info):
    """Fixed arguments for each read-only tool, derived from the corpus (never random)."""
    files = info["files"]
    first = files[0] if files else None
    if name == "fpga_project_list":
        return {"root": info["root"]}
    if name in ("fpga_project_info", "fpga_duplicate_modules"):
        return {}
    if name == "fpga_project_files":
        return {"kind": "hdl", "include_excluded": True}
    if name == "fpga_project_structure":
        return {}
    if name == "fpga_document_symbols":
        return {"file": first} if first else None
    if name == "fpga_workspace_symbols":
        return {"query": "pwm", "limit": 50}
    if name == "fpga_diagnostics":
        return {"file": first, "sources": ["hdl", "adc", "sdc"], "limit": 500} if first else None
    if name == "fpga_device_info":
        return {"include_pins": True, "pin_filter": "P8"}
    if first:
        positions = mcp_positions(first)
        if positions and name in ("fpga_definition", "fpga_references", "fpga_hover"):
            position = positions[0]
            arguments = {"file": first, "line": position["line"] + 1,
                         "column": position["character"] + 1}
            if name == "fpga_references":
                arguments["include_declaration"] = True
            return arguments
        if positions and name == "fpga_completion":
            position = positions[0]
            return {"file": first, "line": position["line"] + 1,
                    "column": position["character"] + 1, "limit": 200}
    return None


def mcp_positions(path):
    try:
        return DocPositions(read_text(path)).module_name(MAX_MCP_POSITIONS)
    except OSError:
        return []


def collect_diagnostics(client_factory, corpus, recorder, normalizer, td_available,
                        reference_root, sandbox_root, keep_work=False):
    """TD-backed diagnostics, always collected against a **sandbox copy** of each project.

    The reference implementation writes into ``<project>/.fpgalsp/`` (TD device cache from
    ``cache_dir = os.path.join(project.dir, ".fpgalsp", "cache")``, and TD session logs in
    ``<project>/.fpgalsp/run``).  ``project.dir`` comes from the ``.al`` location, so running
    against a copy keeps every write inside the sandbox and the user's project stays untouched.

    If the copy cannot be made -- or if the copied ``.al`` does not fully resolve inside the copy
    -- the project is recorded as ``skipped`` and nothing is run against the original.
    """
    if not td_available:
        for project_id in corpus.projects:
            recorder.skip(project_id, "TD unavailable")
        return
    for project_id, info in corpus.projects.items():
        if info["skipped"]:
            recorder.skip(project_id, info["skipped"])
            continue

        work_root = os.path.join(sandbox_root, "projects", project_id)
        if os.path.isdir(work_root):
            shutil.rmtree(work_root, ignore_errors=True)
        log("  diagnostics %s (copying to sandbox, then starting a real TD session)" % project_id)
        try:
            copied, excluded = copy_project_to_sandbox(info["root"], work_root)
        except OSError as exc:
            recorder.skip(project_id, "sandbox copy failed, not touching the original: %s" % exc)
            continue
        ok, detail = verify_copied_project(work_root, reference_root)
        if not ok:
            recorder.skip(project_id, "sandbox copy does not self-resolve, not touching the "
                                      "original: %s" % detail)
            shutil.rmtree(work_root, ignore_errors=True)
            continue
        log("    copied %d files (excluded .fpgalsp/.git/logs); %s" % (copied, detail))

        # Point the case's file list at the copy so diagnostics are collected on the copy too.
        work_info = dict(info)
        work_info["root"] = work_root
        work_info["files"] = Corpus._hdl_files(work_root)

        client = None
        try:
            client = client_factory(work_root)
            client.initialize(work_root, timeout=300)
            entry = {"checkAccepted": None, "deviceLoaded": False, "files": {},
                     "collectedOnCopy": True}
            try:
                entry["checkAccepted"] = client.request(
                    "workspace/executeCommand",
                    {"command": "fpgalsp.check", "arguments": []}, timeout=120)
            except TransportError as exc:
                entry["checkAccepted"] = {"error": str(exc)[:300]}
            deadline = time.time() + 600
            info_result = {}
            while time.time() < deadline:
                try:
                    info_result = client.request(
                        "workspace/executeCommand",
                        {"command": "fpgalsp.info", "arguments": []}, timeout=120) or {}
                except TransportError:
                    break
                if info_result.get("device_loaded"):
                    break
                time.sleep(2.0)
            entry["deviceLoaded"] = bool(info_result.get("device_loaded"))
            entry["info"] = info_result
            for path in work_info["files"][:MAX_PROJECT_HDL_FILES]:
                client.open_document(path, detect_language(path))
                diagnostics = client.wait_diagnostics_for(path, timeout=120)
                # Keys are tokenised against the *original* root so the snapshot stays stable.
                key = _tokenise_copied_path(path, work_root, info["root"], normalizer)
                entry["files"][key] = diagnostics
            recorder.add(project_id, normalizer.normalize(entry))
        except TransportError as exc:
            recorder.error(project_id, str(exc)[:400])
        except Exception as exc:                         # noqa: BLE001
            recorder.error(project_id, "%s: %s" % (type(exc).__name__, exc))
            log(traceback.format_exc())
        finally:
            if client is not None:
                client.close()
            if not keep_work:
                shutil.rmtree(work_root, ignore_errors=True)


def _tokenise_copied_path(path, work_root, original_root, normalizer):
    """Map a sandbox path back to the ``<corpus:id>/...`` token of the original project."""
    rel = os.path.relpath(path, work_root)
    return normalizer.relative_token(os.path.join(original_root, rel)) or \
        rel.replace("\\", "/")


def td_available(repo_root):
    try:
        sys.path.insert(0, repo_root)
        from fpgalsp import td as td_module
        td_module.find_td(None)
        return True, None
    except Exception as exc:                             # noqa: BLE001
        return False, "%s: %s" % (type(exc).__name__, exc)


# --------------------------------------------------------------------------- driver
def main(argv=None):
    parser = argparse.ArgumentParser(description="Collect golden snapshots from fpga-lsp")
    parser.add_argument("--corpus", default=DEFAULT_CORPUS, help="path to cases.json")
    parser.add_argument("--out", default=DEFAULT_SNAPSHOTS, help="snapshot output directory")
    parser.add_argument("--repo", default=None, help="reference implementation checkout")
    parser.add_argument("--sandbox", default=DEFAULT_SANDBOX, help="scratch dir for logs")
    parser.add_argument("--only", action="append", choices=CATEGORIES,
                        help="collect only the named category (repeatable)")
    parser.add_argument("--skip", action="append", choices=CATEGORIES,
                        help="skip the named category (repeatable)")
    parser.add_argument("--clean", action="store_true", help="delete snapshots first")
    parser.add_argument("--no-td", action="store_true",
                        help="never start a real TD session (diagnostics recorded as skipped)")
    parser.add_argument("--keep-sandbox-projects", action="store_true",
                        help="keep the per-project sandbox copies after the diagnostics category "
                             "(they are deleted by default)")
    parser.add_argument("--server", choices=("python", "native"), default="python",
                        help="which backend to collect from: the Python reference (default) or "
                             "the C++ native executable")
    parser.add_argument("--native-exe", default="",
                        help="path to fpgalsp-native.exe (default: <cpp repo>/build/bin/...)")
    args = parser.parse_args(argv)

    spec = load_corpus(args.corpus)
    repo_root = args.repo or spec.get("referenceRepository")
    if not repo_root or not os.path.isdir(repo_root):
        log("reference repository not found (pass --repo): %s" % repo_root)
        return 2

    native_exe = ""
    if args.server == "native":
        try:
            native_exe = args.native_exe or default_native_exe()
        except FileNotFoundError as exc:
            log(str(exc))
            return 2
        if not os.path.isfile(native_exe):
            log("native executable not found: %s" % native_exe)
            return 2

    def lsp_factory(log_path):
        if args.server == "native":
            return NativeLspClient(repo_root, log_path, native_exe)
        return LspClient(repo_root, log_path)

    def mcp_factory(root_path, log_path):
        if args.server == "native":
            return NativeMcpClient(repo_root, root_path, log_path, native_exe)
        return McpClient(repo_root, root_path, log_path)

    selected = list(args.only) if args.only else list(CATEGORIES)
    for name in args.skip or []:
        if name in selected:
            selected.remove(name)

    if args.clean and os.path.isdir(args.out):
        shutil.rmtree(args.out)
    os.makedirs(args.out, exist_ok=True)
    sandbox = args.sandbox
    if args.server == "native":
        sandbox = sandbox + "-native"
    if os.path.isdir(sandbox):
        shutil.rmtree(sandbox, ignore_errors=True)
    os.makedirs(sandbox, exist_ok=True)

    corpus = Corpus(spec, os.path.join(sandbox, "synthetic"))
    corpus.resolve()
    roots = build_roots(corpus, repo_root)
    normalizer = Normalizer(roots)

    log("corpus projects:")
    for project_id, info in corpus.projects.items():
        if info["skipped"]:
            log("  %-26s SKIPPED (%s)" % (project_id, info["skipped"]))
        else:
            log("  %-26s %d HDL files" % (project_id, len(info["files"])))

    has_td, td_reason = td_available(repo_root)
    if args.no_td:
        has_td, td_reason = False, "disabled by --no-td"
    log("TD: %s%s" % ("available" if has_td else "unavailable", "" if has_td else " (%s)" % td_reason))

    meta = {
        "schema": 1,
        "corpusPath": "<repo>/tests/golden/corpus/cases.json",
        "categories": {},
        "limits": {
            "maxProjectHdlFiles": MAX_PROJECT_HDL_FILES,
            "maxPositionsPerKind": MAX_POSITIONS_PER_KIND,
            "maxReferencesCases": MAX_REFERENCES_CASES,
            "maxSemanticFiles": MAX_SEMANTIC_FILES,
            "maxMcpPositions": MAX_MCP_POSITIONS,
        },
        "tdAvailable": has_td,
        "server": args.server,
        "nativeExe": native_exe,
    }

    for category in selected:
        started = time.time()
        log("\n== %s" % category)
        recorder = Recorder(category)
        try:
            if category == "diagnostics":
                collect_diagnostics(
                    lambda root: lsp_factory(os.path.join(
                        sandbox, "server-diagnostics.log")),
                    corpus, recorder, normalizer, has_td,
                    reference_root=repo_root,
                    sandbox_root=os.path.join(sandbox, "diagnostics"),
                    keep_work=args.keep_sandbox_projects)
            elif category == "mcp":
                # MCP manages its own process lifecycle per project; no shared LSP session.
                collect_mcp(spec, corpus, recorder, normalizer, repo_root,
                            os.path.join(sandbox, "mcp"), has_td,
                            mcp_factory=mcp_factory)
            else:
                client = lsp_factory(os.path.join(sandbox, "server-%s.log" % category))
                try:
                    client.initialize(corpus.projects and
                                      next((i["root"] for i in corpus.projects.values()
                                            if not i["skipped"]), repo_root), timeout=300)
                    if category == "project":
                        collect_project(client, corpus, recorder, normalizer)
                    elif category == "hdl":
                        collect_hdl(client, corpus, recorder, normalizer, roots)
                    elif category == "semantics":
                        collect_semantics(client, corpus, recorder, normalizer)
                    elif category == "hierarchy":
                        collect_hierarchy(client, corpus, recorder, normalizer)
                finally:
                    client.close()
        except Exception as exc:                         # noqa: BLE001
            log("category %s failed: %s" % (category, exc))
            log(traceback.format_exc())
            recorder.error(category, "%s: %s" % (type(exc).__name__, exc))

        snapshot = build_snapshot(recorder.cases, extra={
            "category": category,
            # Sort by the original id, then normalise: normalising first would sort on the
            # tokenised spelling, which differs from the id it came from and made the order
            # depend on how the paths happened to be written.
            "skipped": normalizer.normalize(
                sorted(recorder.skipped, key=lambda item: item["id"])),
        })
        out_path = os.path.join(args.out, category, "%s.json" % category)
        write_snapshot(out_path, snapshot)
        meta["categories"][category] = {
            "cases": len(recorder.cases),
            "skipped": len(recorder.skipped),
            "file": "snapshots/%s/%s.json" % (category, category),
        }
        log("   -> %d cases, %d skipped" % (len(recorder.cases), len(recorder.skipped)))

    write_snapshot(os.path.join(args.out, "_meta.json"), dump_json(meta))
    write_snapshot(os.path.join(args.out, "corpus.manifest.json"),
                   dump_json(corpus_manifest(corpus, normalizer)))
    log("\nsnapshots written to %s" % args.out)
    return 0


def corpus_manifest(corpus, normalizer):
    """The corpus as the collector actually saw it, so a snapshot set is self-describing."""
    projects = {}
    for project_id, info in sorted(corpus.projects.items()):
        projects[project_id] = {
            "root": normalizer.relative_token(info["root"]) or info["root"],
            "al": normalizer.relative_token(info["al"]) if info["al"] else None,
            "files": [normalizer.relative_token(path) or path for path in info["files"]],
            "skipped": info["skipped"],
        }
    return {"schema": 1, "projects": projects,
            "synthetic": [normalizer.relative_token(p) or p for p in corpus.synthetic]}


if __name__ == "__main__":
    raise SystemExit(main())