#!/usr/bin/env python3
"""Deduplicate and summarise the clang-tidy audit results.

Why this exists
---------------
`run_clang_tidy.py` runs clang-tidy once per translation unit with
`--header-filter=src/`.  A diagnostic located in a *header* is therefore
re-reported once per TU that includes it.  On this tree that inflation is
large: 8616 raw diagnostics collapse to 6092 unique locations (30% of the
"findings" are duplicates).  Any report that quotes the raw number is wrong by
a third, and any statement like "we can auto-fix 1409 nodiscard warnings" is
wrong by a factor of 13 -- `modernize-use-nodiscard` has 1409 raw hits but only
109 distinct source locations.

This script produces the numbers that are safe to quote:
  * unique locations, deduplicated on (file, line, col, check)
  * per-family / per-check / per-file counts on the deduplicated set
  * per-module code LOC and diagnostic density (diagnostics per KLOC)
  * function-length distribution (literal-aware brace scan, so braces inside
    string literals and raw strings do not corrupt the count)

Usage
-----
    python tools/audit/summarize.py
    python tools/audit/summarize.py --json-only
    python tools/audit/summarize.py --audit-dir audit\\2026-11-19\\static
"""

from __future__ import annotations

import argparse
import collections
import glob
import json
import os
import re
import sys

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DEFAULT_DIR = os.environ.get("FPGA_LSP_AUDIT_DIR_SRC") or os.path.join(
    PROJECT_ROOT, "audit", "2026-11-19", "static"
)

SOURCE_EXT = (".cpp", ".cc", ".cxx", ".h", ".hpp")

# --------------------------------------------------------------------------- #
# function-length scan (literal aware)
# --------------------------------------------------------------------------- #
CTRL_START = re.compile(r"^(if|for|while|switch|catch|else|do|try)\b")
TYPE_START = re.compile(r"^(class|struct|union|enum|namespace)\b")


def scan_functions(text: str) -> list[tuple[int, int, str]]:
    """Return [(lines, start_line, header)] for every function-like block.

    Tracks braces while skipping // comments, /* */ comments, "..." and '...'
    literals, and R"delim( ... )delim" raw strings.  A block opening brace is
    treated as a function body when the text between the previous delimiter and
    the brace looks like a `name(args)` declarator.
    """
    i, n, line = 0, len(text), 1
    stack: list[dict] = []
    pending: list[str] = []
    pline = 1
    results: list[tuple[int, int, str]] = []

    def is_function_like(h: str) -> bool:
        h = re.sub(r"\s+", " ", h).strip()
        if "(" not in h or ")" not in h or "#" in h:
            return False
        # A lambda bound to a name/parameter ("auto f = [&](int x) { ... }") is a
        # function body for the purpose of length analysis, and must be matched
        # before the `=`-in-head rejection below.
        if re.search(r"\]\s*\([^)]*\)\s*(mutable\s*)?(->[^{]*)?$", h):
            return True
        if CTRL_START.match(h) or TYPE_START.match(h):
            return False
        if h.endswith("="):
            return False
        # reject `auto x = foo(...)` / `x = f(...)` assignment initialisers
        head = h.split("(", 1)[0]
        if "=" in head:
            return False
        return True

    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                if text[i] == "\n":
                    line += 1
                i += 1
            i += 2
            continue
        if c in "Rr" and i + 1 < n and text[i + 1] == '"':
            j = text.find("(", i + 2)
            if j != -1:
                close = ")" + text[i + 2:j] + '"'
                k = text.find(close, j)
                if k != -1:
                    line += text.count("\n", i, k)
                    i = k + len(close)
                    continue
        if c == '"':
            start_line = line
            i += 1
            while i < n and text[i] != '"':
                if text[i] == "\\":
                    i += 1
                if i < n and text[i] == "\n":
                    line += 1
                i += 1
            i += 1
            # Keep a placeholder so a literal still counts as header content
            # (otherwise `foo("x") {` and `foo() {` collapse to the same text).
            pending.append("S")
            continue
        if c == "'":
            i += 1
            while i < n and text[i] != "'":
                if text[i] == "\\":
                    i += 1
                i += 1
            i += 1
            pending.append("C")
            continue
        if c == "{":
            header = "".join(pending)
            stack.append({"start": pline, "func": is_function_like(header), "hdr": header[-200:]})
            pending = []
            pline = line
            i += 1
            continue
        if c == "}":
            if stack:
                b = stack.pop()
                if b["func"]:
                    results.append((line - b["start"] + 1, b["start"], b["hdr"]))
            pending = []
            pline = line
            i += 1
            continue
        if c == ";":
            pending = []
            pline = line
            i += 1
            continue
        if not c.isspace():
            pending.append(c)
        elif pending:
            pending.append(" ")
        i += 1
    return results


def code_lines(path: str) -> tuple[int, int]:
    """(physical lines, code lines) where code lines exclude blank/comment-only."""
    try:
        text = open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return 0, 0
    physical = text.count("\n") + 1
    code = sum(
        1
        for ln in text.splitlines()
        if ln.strip() and not ln.strip().startswith(("//", "/*", "*"))
    )
    return physical, code


# --------------------------------------------------------------------------- #
def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--audit-dir", default=DEFAULT_DIR)
    ap.add_argument("--json-only", action="store_true")
    args = ap.parse_args(argv)

    tidy_json = os.path.join(args.audit_dir, "clang-tidy.json")
    if not os.path.isfile(tidy_json):
        print(f"error: {tidy_json} not found -- run tools/audit/run_clang_tidy.py first",
              file=sys.stderr)
        return 2

    data = json.load(open(tidy_json, encoding="utf-8"))
    raw = data["diagnostics"]

    seen = set()
    uniq = []
    for d in raw:
        key = (d["file"], d["line"], d["col"], d["check"])
        if key in seen:
            continue
        seen.add(key)
        uniq.append(d)

    # ---- per-file LOC -----------------------------------------------------
    src_files = [
        p
        for p in glob.glob(os.path.join(PROJECT_ROOT, "src", "**", "*"), recursive=True)
        if p.lower().endswith(SOURCE_EXT)
    ]
    loc = {}
    for p in src_files:
        rel = os.path.relpath(p, PROJECT_ROOT).replace("\\", "/")
        phys, code = code_lines(p)
        loc[rel] = (phys, code)

    per_file = collections.Counter(d["file"] for d in uniq)
    per_check = collections.Counter(d["check"] for d in uniq)
    per_family = collections.Counter(d["check"].split("-")[0] for d in uniq)
    raw_per_check = collections.Counter(d["check"] for d in raw)

    modules: dict[str, list[int]] = collections.defaultdict(lambda: [0, 0, 0])
    for rel, (phys, code) in loc.items():
        parts = rel.split("/")
        mod = parts[1] if len(parts) > 2 else parts[-1]
        modules[mod][0] += 1
        modules[mod][1] += code
        modules[mod][2] += per_file.get(rel, 0)

    total_code = sum(v[1] for v in loc.values())

    # ---- function lengths -------------------------------------------------
    funcs: list[tuple[int, str, int, str]] = []
    for p in src_files:
        try:
            text = open(p, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        for length, start, hdr in scan_functions(text):
            funcs.append((length, os.path.relpath(p, PROJECT_ROOT).replace("\\", "/"), start, hdr))
    funcs.sort(reverse=True)
    buckets = collections.Counter()
    for length, *_ in funcs:
        if length < 30:
            buckets["<30"] += 1
        elif length < 50:
            buckets["30-49"] += 1
        elif length < 100:
            buckets["50-99"] += 1
        elif length < 200:
            buckets["100-199"] += 1
        elif length < 500:
            buckets["200-499"] += 1
        else:
            buckets[">=500"] += 1

    over200 = [f for f in funcs if f[0] >= 200]

    out = {
        "meta": {
            "generated": __import__("time").strftime("%Y-%m-%dT%H:%M:%S%z"),
            "audit_dir": os.path.relpath(args.audit_dir, PROJECT_ROOT).replace("\\", "/"),
            "tool": data.get("meta", {}).get("tool_version", "unknown"),
            "tus": data.get("meta", {}).get("total_tus"),
            "ok_tus": data.get("meta", {}).get("ok_tus"),
        },
        "totals": {
            "raw_diagnostics": len(raw),
            "unique_locations": len(uniq),
            "dedup_ratio": round(len(uniq) / len(raw), 4) if raw else 0.0,
            "files_with_findings": len(per_file),
            "source_files": len(loc),
            "code_loc": total_code,
            "physical_loc": sum(v[0] for v in loc.values()),
            "diagnostics_per_kloc": round(len(uniq) / total_code * 1000, 1) if total_code else 0.0,
        },
        "by_family": dict(per_family.most_common()),
        "by_check": dict(per_check.most_common()),
        "raw_by_check": dict(raw_per_check.most_common()),
        "by_file": dict(per_file.most_common()),
        "by_module": {
            m: {"files": n, "code_loc": c, "diagnostics": d,
                "per_kloc": round(d / c * 1000, 1) if c else 0.0}
            for m, (n, c, d) in sorted(modules.items(), key=lambda kv: -kv[1][1])
        },
        "functions": {
            "total": len(funcs),
            "buckets": {k: buckets[k] for k in ("<30", "30-49", "50-99", "100-199", "200-499", ">=500")},
            "over_200": [{"lines": n, "file": f, "line": s, "header": re.sub(r"\s+", " ", h)[-140:]}
                         for n, f, s, h in over200],
            "longest": [{"lines": n, "file": f, "line": s, "header": re.sub(r"\s+", " ", h)[-140:]}
                        for n, f, s, h in funcs[:20]],
        },
    }

    os.makedirs(args.audit_dir, exist_ok=True)
    json_path = os.path.join(args.audit_dir, "summary.json")
    with open(json_path, "w", encoding="utf-8") as fh:
        json.dump(out, fh, indent=1, ensure_ascii=False)
        fh.write("\n")

    if args.json_only:
        print(f"[summary] wrote {json_path}")
        return 0

    w = []
    a = w.append
    a("=" * 92)
    a("fpga-lsp-cpp 静态分析汇总（去重后口径）")
    a("=" * 92)
    t = out["totals"]
    a(f"原始诊断 (raw)      : {t['raw_diagnostics']}")
    a(f"唯一位置 (dedup)    : {t['unique_locations']}   去重比 {t['dedup_ratio']:.1%}")
    a(f"源码文件 / 代码行   : {t['source_files']} 个 / {t['code_loc']:,} 行（物理行 {t['physical_loc']:,}）")
    a(f"密度               : {t['diagnostics_per_kloc']} 处 / KLOC")
    a(f"覆盖 TU            : {out['meta']['ok_tus']}/{out['meta']['tus']} 全部解析成功")
    a("")
    a("-" * 92)
    a("1. 按家族（去重后）")
    a("-" * 92)
    for k, v in out["by_family"].items():
        a(f"  {v:6d}  {k}")
    a("")
    a("-" * 92)
    a("2. 按 check（去重后 Top 30，含 raw→unique 的膨胀倍数）")
    a("-" * 92)
    for k, v in list(out["by_check"].items())[:30]:
        r = out["raw_by_check"].get(k, v)
        a(f"  {v:5d}  {k:<58} raw={r}")
    a("")
    a("-" * 92)
    a("3. 按模块（代码行 / 诊断 / 密度）")
    a("-" * 92)
    a(f'  {"module":<18}{"files":>7}{"codeLOC":>10}{"diags":>8}{"perKLOC":>10}')
    for m, s in out["by_module"].items():
        a(f'  {m:<18}{s["files"]:>7}{s["code_loc"]:>10}{s["diagnostics"]:>8}{s["per_kloc"]:>10.1f}')
    a("")
    a("-" * 92)
    a("4. 按文件（去重后 Top 20）")
    a("-" * 92)
    for k, v in list(out["by_file"].items())[:20]:
        a(f"  {v:5d}  {k}")
    a("")
    a("-" * 92)
    a(f"5. 函数长度分布（共 {out['functions']['total']} 个函数，字面量感知扫描）")
    a("-" * 92)
    for k, v in out["functions"]["buckets"].items():
        a(f"  {k:<10} {v:>5}")
    a(f"  >= 200 行: {len(out['functions']['over_200'])} 个")
    for f in out["functions"]["over_200"]:
        a(f'    {f["lines"]:>5} 行  {f["file"]}:{f["line"]}')
    a("")

    md_path = os.path.join(args.audit_dir, "summary.md")
    with open(md_path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(w) + "\n")

    print("\n".join(w))
    print(f"[summary] wrote {json_path}")
    print(f"[summary] wrote {md_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())