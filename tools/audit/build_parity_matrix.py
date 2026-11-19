"""Build the frozen parity matrix for the 2026-11-18 fpga-lsp-cpp audit.

The rows below are the cross-verified findings from A2-A5.  The script emits both
``parity-matrix.json`` (machine readable) and ``parity-matrix.md`` (review table) so
the report and the Phase B task list cannot drift apart.

Usage:
    python tools/audit/build_parity_matrix.py --out audit/2026-11-18
"""

from __future__ import annotations

import argparse
import json
import os

# surface, endpoint, python, cpp, cls, sev, fix, effort, evidence
ROWS = [
    # ---------------- standard LSP methods ----------------
    ("LSP-01", "lsp", "textDocument/semanticTokens/full", "2094 tokens / 10470 ints", '{"data":[]}',
     "PORT", "S0", "B2", "S", "lsp_server.cpp:472; parity-lsp/raw.json matrix[7]"),
    ("LSP-02", "lsp", "textDocument/foldingRange", "24 ranges", "[]",
     "PORT", "S0", "B2", "S", "lsp_server.cpp:267; matrix[8]"),
    ("LSP-03", "lsp", "textDocument/codeAction", "2 actions (instantiate + Verible autofix)", "[]",
     "PORT", "S0", "B3", "M", "lsp_server.cpp:248; matrix[9]"),
    ("LSP-04", "lsp", "textDocument/prepareRename", "range + placeholder", "null",
     "PORT", "S0", "B3", "S", "lsp_server.cpp:254; matrix[10]"),
    ("LSP-05", "lsp", "textDocument/rename", "4 edits", "null",
     "PORT", "S0", "B3", "M", "lsp_server.cpp:256; matrix[11]"),
    ("LSP-06", "lsp", "textDocument/prepareCallHierarchy", "1 item", "[]",
     "PORT", "S0", "B4", "S", "lsp_server.cpp:258; matrix[12]"),
    ("LSP-07", "lsp", "callHierarchy/incomingCalls", "1 call", "[]",
     "PORT", "S0", "B4", "S", "lsp_server.cpp:260; matrix[13]"),
    ("LSP-08", "lsp", "callHierarchy/outgoingCalls", "12 calls", "[]",
     "PORT", "S0", "B4", "M", "lsp_server.cpp:262; matrix[14]"),
    ("LSP-09", "lsp", "textDocument/rangeFormatting", "1 edit", "[]",
     "PORT", "S0", "B4", "S", "lsp_server.cpp:252; matrix[16]"),
    ("LSP-10", "lsp", "fpgaLsp/reports", "structured reports + strategies/routed_db",
     'error:"reports module not implemented"', "WIRING", "S0", "B1", "S",
     "commands.cpp:260-288; src/reports/* exists; MCP project_reports works"),
    ("LSP-11", "lsp", "documentSymbol/hover/completion on .adc/.sdc", "47 / 2 symbols, hover non-null",
     "0 / 0 symbols, hover null", "MISSING", "S0", "B10", "L",
     "Lead cross-check 2026-11-18; parity-lsp/raw.json boundaries[adc:*,sdc:*]"),
    ("LSP-12", "lsp", "textDocument/completion (project symbols)", "814 items", "21 items",
     "PORT", "S1", "B10", "M", "matrix[4]; Lead cross-check 814 vs 21"),
    ("LSP-13", "lsp", "textDocument/inlayHint", "11 hints", "19 hints (Python is a strict subset)",
     "INTENTIONAL", "S2", "B9", "-", "matrix[6]; additive instance-type hints"),
    ("LSP-14", "lsp", "HDL index across `ifdef branches", "both branches", "active branch only",
     "INTENTIONAL", "S1", "B9", "-", "COMMANDS.md:80-84 documents the divergence"),
    ("LSP-15", "lsp", "fpgaLsp/flowStatus", "artifacts + flow.ok", "missing both",
     "WIRING", "S1", "B1", "S", "matrix[19]; commands.cpp:294-308"),
    ("LSP-16", "lsp", "fpgaLsp/moduleTree includePrimitives=false", "34 nodes", "36 nodes (2 primitive blackboxes)",
     "BUG", "S1", "B10", "S", "matrix[17]; commands.cpp:224-227"),
    ("LSP-17", "lsp", "hover text / workspace symbol containerName", "richer markdown + containerName",
     "terse / missing containerName", "COSMETIC", "S2", "B9", "S", "matrix[1,3]"),
    ("LSP-18", "lsp", "textDocument/formatting bytes", "LF + external Verible 0.0-4296",
     "CRLF + embedded Verible c6b29d62", "DIVERGENCE", "S2", "B9", "S",
     "parity-lsp PROBE.md 3.6; needs a product decision, not a bug fix"),
    # ---------------- capability declarations ----------------
    ("CAP-01", "capability", "completionProvider.triggerCharacters", '{"{","="," ","[","\\t","|"}',
     '{".","("}', "WIRING", "S1", "B10", "S", "initialize capability diff; matrix[4]"),
    ("CAP-02", "capability", "textDocumentSync.save.includeText", "true", "false",
     "WIRING", "S1", "B1", "S", "initialize capability diff"),
    ("CAP-03", "capability", "workspace.workspaceFolders", "supported + changeNotifications", "absent",
     "WIRING", "S1", "B1", "S", "initialize capability diff"),
    # ---------------- executeCommand ----------------
    ("CMD-01", "command", "fpgalsp.runAnalysis", "drc/cdc/device payload", "not implemented",
     "MISSING", "S0", "B5", "M", "commands.cpp:544; parity-lsp raw.json commands[5]"),
    ("CMD-02", "command", "fpgalsp.instantiate", "module/instanceName/portCount/text", "not implemented",
     "WIRING", "S0", "B1", "S", "commands.cpp:549; bridge.cpp InstantiationTool works"),
    ("CMD-03", "command", "fpgalsp.copyInstantiation", "module/text/portCount", "not implemented",
     "WIRING", "S0", "B1", "S", "commands.cpp:550; bridge.cpp has the logic"),
    ("CMD-04", "command", "fpgalsp.symbolPath", "name/path/module", "not implemented",
     "WIRING", "S0", "B1", "S", "commands.cpp:551; bridge.cpp SymbolPathTool works"),
    ("CMD-05", "command", "fpgalsp.duplicateModules", "duplicates[]", "not implemented",
     "WIRING", "S0", "B1", "S", "commands.cpp:552; bridge.cpp DuplicateModulesTool works"),
    ("CMD-06", "command", "fpgalsp.searchHierarchy", "module + instance matches", "not implemented",
     "WIRING", "S0", "B1", "S", "commands.cpp:553; bridge.cpp SearchHierarchyTool works"),
    ("CMD-07", "command", "fpgalsp.debugHighlight", "2146 tokens + counts", "not implemented",
     "MISSING", "S0", "B2", "S", "commands.cpp:554; depends on semantic tokens"),
    ("CMD-08", "command", "fpgalsp.addSourceFile", "writes .al (added:true)", "not implemented",
     "WIRING", "S0", "B1", "S", "commands.cpp:546; bridge.cpp ProjectAddSourceTool works"),
    ("CMD-09", "command", "fpgalsp.setProjectName", "writes .al <Name>", "not implemented",
     "WIRING", "S0", "B1", "S", "commands.cpp:547; bridge.cpp ProjectRenameTool works"),
    ("CMD-10", "command", "fpgalsp.updateProject", "runs check_project_version + open_project -update",
     "not implemented", "MISSING", "S0", "B5", "M", "commands.cpp:548; needs TD command sequence"),
    # ---------------- MCP tools ----------------
    ("MCP-01", "mcp", "fpga_format", "ok/text or ok/edits", "not implemented (isError=false)",
     "WIRING", "S0", "B6", "S", "bridge.cpp:1429; Verible formatter already wired for LSP"),
    ("MCP-02", "mcp", "fpga_code_actions", "ok/actions[{title,kind,isPreferred,edits}]", "not implemented",
     "PORT", "S0", "B6", "M", "bridge.cpp:1425; needs FeatureCodeActions"),
    ("MCP-03", "mcp", "fpga_rename_edits", "ok/changes{file:[{range,newText}]}", "not implemented",
     "PORT", "S0", "B6", "M", "bridge.cpp:1425; needs FeatureRename"),
    ("MCP-04", "mcp", "fpga_analysis_run", "drc/cdc/device/files", "not implemented",
     "MISSING", "S0", "B5", "M", "duplicate backend of CMD-01; src/analysis absent"),
    ("MCP-05", "mcp", "fpga_project_update", "ok/al/output (real TD run)", "not implemented",
     "MISSING", "S0", "B5", "M", "duplicate backend of CMD-10"),
    ("MCP-D1", "mcp", "pins[].pin case", "P81", "p81", "BUG", "S1", "B6", "XS",
     "parity-mcp/tools-diff.md; 17 occurrences, breaks ADC matching"),
    ("MCP-D2", "mcp", "extra/missing result keys", "portsSource/unavailable/device/al/fileCount absent",
     "extra or missing keys", "COSMETIC", "S2", "B6", "S", "parity-mcp/tools-diff.md"),
    ("MCP-D3", "mcp", "TD scratch directory", "<project>/.fpgalsp/run", "caller cwd (inherited)",
     "BUG", "S1", "B7", "S", "bridge.cpp:463-470; td_session.cpp:231-233; root td_*.logw repro"),
    # ---------------- CLI ----------------
    ("CLI-01", "cli", "--stdio --project", "honored", "silently ignored", "WIRING", "S1", "B9", "S",
     "main.cpp only reads --project in MCP branch"),
    ("CLI-02", "cli", "default log path", "<cwd>/.fpgalsp/logs/server.log", "no log unless --log",
     "WIRING", "S2", "B9", "S", "compare with __main__.py"),
    ("CLI-03", "cli", "--verible", "accepted", "exit 2 (unknown argument)", "WIRING", "S2", "B9", "S",
     "parity-mcp PROBE.md 6"),
    ("CLI-04", "cli", "--help", "exit 0", "exit 2", "COSMETIC", "S2", "B9", "XS", "main.cpp PrintUsage"),
    # ---------------- performance ----------------
    ("PERF-01", "perf", "per-request .al reparse + project signature", "cached", "O(N) per request (0.143 ms/file)",
     "BUG", "S1", "B8", "M", "performance/BENCH.md; lsp_server.cpp:978-980,488-519"),
    ("PERF-02", "perf", "MCP fpga_project_structure", "3.73 ms", "4.30 ms (0.87x)",
     "BUG", "S2", "B8", "S", "bridge.cpp:854-938 double materialization"),
    ("PERF-03", "perf", "HDL index memory", "16 MB @200 files", "126x source size; deep copy in AddFile",
     "BUG", "S2", "B8", "S", "hdl_index.cpp:1796,1805"),
    # ---------------- security ----------------
    ("SEC-01", "security", "TD Tcl injection via .al", "same defect (td.py:626)", "unquoted TOP/Device/Name into Tcl",
     "BUG", "High", "B7", "M", "static/SECURITY.md S-01; td_diagnostics.cpp:225 etc."),
    ("SEC-02", "security", "CreateProcessW quoting", "Popen([exe]) safe", "unquoted cmdline, lpApplicationName=null",
     "BUG", "Medium-High", "B7", "XS", "SECURITY.md S-02; td_session.cpp:235; C++ regression"),
    ("SEC-03", "security", "MCP project path containment", "same", ".al outside --root accepted",
     "BUG", "Medium", "B7", "S", "SECURITY.md S-03; bridge.cpp:323-348"),
    ("SEC-04", "security", "junction/symlink containment", "same", "lexical IsWithin only",
     "BUG", "Medium", "B7", "S", "SECURITY.md S-04; probe read C:\\Windows hosts"),
    ("SEC-05", "security", "XML nesting depth", "same", "100k nesting -> 0xC00000FD",
     "BUG", "Medium", "B7", "S", "SECURITY.md S-05; xml_lite.cpp:361"),
    ("SEC-06", "security", "dangling string_view", "n/a", "al_project.cpp:635-638 UB",
     "BUG", "Medium", "B7", "XS", "SECURITY.md S-06; bugprone-dangling-handle"),
    ("SEC-07", "security", "read_file size guard", "same", "whole file read before truncation",
     "BUG", "Low", "B7", "S", "SECURITY.md S-07"),
    ("SEC-08", "security", "MCP getline bound", "n/a", "unbounded line read", "BUG", "Low", "B7", "XS",
     "SECURITY.md S-08"),
    ("SEC-09", "security", "LSP mode has no allow gate", "design", "design", "INTENTIONAL", "Low", "B9", "-",
     "SECURITY.md S-09; document in README"),
    # ---------------- engineering / inherited ----------------
    ("ENG-01", "engineering", "golden snapshots", "stale (2026-11-18 16:15)", "re-collected this audit",
     "ENGINEERING", "S3", "B9", "S", "audit/2026-11-18/golden/*"),
    ("ENG-02", "engineering", "docs drift (COMMANDS/README)", "contract", "stale placeholder tables",
     "ENGINEERING", "S3", "B9", "S", "src/server/COMMANDS.md; README delivery table"),
    ("ENG-03", "engineering", "warning/tooling gates", "/WX not default, no clang-tidy/format/CI",
     "same", "ENGINEERING", "S3", "B9", "M", "static/MODERNIZATION.md"),
    ("BUG-P24", "bug", "Python MCP document_symbols child file drive", "C: not Y: when cwd differs",
     "correct", "BUG-INHERITED", "S2", "B9", "-", "parity-mcp PROBE.md 4"),
]


def build(out_dir):
    rows = []
    for rid, surface, endpoint, py, cpp, cls, sev, fix, effort, evidence in ROWS:
        rows.append({
            "id": rid, "surface": surface, "endpoint": endpoint,
            "python": py, "cpp": cpp, "classification": cls, "severity": sev,
            "phase_b_task": fix, "effort": effort, "evidence": evidence,
        })
    summary = {"total": len(rows)}
    for key, field in (("s0", "severity"), ("s1", "severity"), ("s2", "severity"),
                       ("high", "severity")):
        pass
    by_sev, by_cls, by_surface = {}, {}, {}
    for row in rows:
        by_sev[row["severity"]] = by_sev.get(row["severity"], 0) + 1
        by_cls[row["classification"]] = by_cls.get(row["classification"], 0) + 1
        by_surface[row["surface"]] = by_surface.get(row["surface"], 0) + 1
    summary.update({"by_severity": by_sev, "by_classification": by_cls, "by_surface": by_surface})
    data = {
        "schema": 1,
        "generated": "2026-11-18",
        "reference": {"repo": "Y:\\fpga-lsp", "head": "b4891bb"},
        "candidate": {"repo": "Y:\\fpga-lsp-cpp", "head": "ccdacb9",
                      "exe_sha256": "C21670DF30681AF7B766AC37BAE16B6BBE1B6B3C188BCB8EB85470F61A1B5013"},
        "summary": summary,
        "endpoints": rows,
    }
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "parity-matrix.json"), "w", encoding="utf-8") as fh:
        json.dump(data, fh, ensure_ascii=False, indent=2)
        fh.write("\n")

    order = {"S0": 0, "S1": 1, "High": 1, "Medium-High": 2, "Medium": 2, "Medium": 3,
             "S2": 3, "Low": 4, "S3": 5}
    rows_sorted = sorted(rows, key=lambda r: (order.get(r["severity"], 9), r["id"]))
    lines = [
        "# 功能缺口矩阵（冻结于 2026-11-18 审计）",
        "",
        "参考实现 `Y:\\fpga-lsp` @ `b4891bb`；候选 `Y:\\fpga-lsp-cpp` @ `ccdacb9`，"
        "`fpgalsp-native.exe` SHA256 `C21670DF…5013`。",
        "",
        "分类：`MISSING` 后端完全没有 / `WIRING` 已实现但入口未接 / `PORT` 需移植逻辑 / "
        "`INTENTIONAL` 有意差异 / `BUG` 缺陷 / `BUG-INHERITED` 继承的 Python 缺陷 / "
        "`COSMETIC` 表现层 / `ENGINEERING` 工程化。",
        "",
        "| ID | 面 | 端点 | Python（契约） | C++ | 分类 | 级别 | 修复 | 工作量 | 证据 |",
        "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    for r in rows_sorted:
        lines.append("| {id} | {surface} | `{endpoint}` | {python} | {cpp} | {classification} | "
                     "**{severity}** | {phase_b_task} | {effort} | {evidence} |".format(**r))
    lines += ["", "## 汇总", "", "| 维度 | 数量 |", "| --- | --- |"]
    for key in ("S0", "S1", "S2", "S3", "High", "Medium-High", "Medium", "Low",
                "MISSING", "WIRING", "PORT", "BUG", "INTENTIONAL", "COSMETIC",
                "BUG-INHERITED", "ENGINEERING"):
        if key in by_sev or key in by_cls:
            count = by_sev.get(key, 0) + by_cls.get(key, 0)
            lines.append("| %s | %d |" % (key, count))
    lines += ["", "总计 **%d** 项。" % len(rows), ""]
    with open(os.path.join(out_dir, "parity-matrix.md"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))
    print("wrote %d rows -> %s/parity-matrix.{json,md}" % (len(rows), out_dir))
    print(json.dumps(summary, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default="audit/2026-11-18")
    args = parser.parse_args()
    build(args.out)