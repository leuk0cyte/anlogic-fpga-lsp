#!/usr/bin/env python3
"""Run clang-tidy over the clang-converted compilation database, then aggregate.

Input  : doc/audit/_db/compile_commands.json  (produced by make_clang_db.py)
Output : doc/audit/2026-11-18/static/clang-tidy.txt   aggregated tables + raw logs
         doc/audit/2026-11-18/static/clang-tidy.json  machine-readable diagnostics
         doc/audit/_db/tidy-logs/*.log                per-TU raw output

Design notes
------------
* One process per translation unit, `--jobs 8`, hard 300 s per TU.  A TU that
  times out or fails to parse is recorded as such in clang-tidy.json -- it is
  never silently dropped and never faked as "clean".
* Diagnostics are parsed out of the human text clang-tidy prints:
      <path>:<line>:<col>: <severity>: <message> [<check-name>]
  This is the stable contract of the tool (the `--export-fixes` YAML is only a
  hint layer and changes shape between releases).
* `--header-filter` is restricted to src/, so findings inside thirdparty/ and the
  vendored Verible generated headers stay quiet; they are counted separately if
  they appear anyway.
* Re-aggregate without re-running: `--aggregate-only` (reads tidy-logs/).

Run it through tools/audit/with_vcvars.cmd so the MSVC STL headers resolve.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import multiprocessing as mp
import os
import re
import subprocess
import sys
import time

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DB = os.path.join(PROJECT_ROOT, "doc", "audit", "_db", "compile_commands.json")
# Overridable so a fresh audit run does not overwrite a previous one:
#   set FPGA_LSP_AUDIT_DIR=doc\audit\2026-11-19\static
AUDIT_DIR = os.environ.get("FPGA_LSP_AUDIT_DIR") or os.path.join(
    PROJECT_ROOT, "doc", "audit", "2026-11-18", "static"
)
LOGS_DIR = os.path.join(PROJECT_ROOT, "doc", "audit", "_db", "tidy-logs")
OUT_TXT = os.path.join(AUDIT_DIR, "clang-tidy.txt")
OUT_JSON = os.path.join(AUDIT_DIR, "clang-tidy.json")

CHECKS = (
    "bugprone-*,"
    "-bugprone-easily-swappable-parameters,"
    "performance-*,"
    "modernize-*,"
    "-modernize-use-trailing-return-type,"
    "readability-*,"
    "-readability-identifier-length,"
    "-readability-magic-numbers,"
    "cppcoreguidelines-*,"
    "-cppcoreguidelines-avoid-magic-numbers,"
    "-cppcoreguidelines-pro-bounds-pointer-arithmetic,"
    "cert-*,"
    "concurrency-*,"
    "clang-analyzer-*"
)

HEADER_FILTER = r".*[/\\]src[/\\].*"

SEVERITY_ORDER = {"error": 0, "warning": 1, "remark": 2, "note": 9}

DIAG_RE = re.compile(r"^(?P<file>.+?):(?P<line>\d+):(?P<col>\d+): (?P<sev>warning|error|remark|note): (?P<msg>.*?)(?: \[(?P<check>[-a-zA-Z0-9._,]+)\])?$")
COMPILE_ERR_RE = re.compile(r"^(?P<file>.+?):(?P<line>\d+):(?P<col>\d+): error: (?P<msg>.*)$")


def clang_tidy_exe() -> str:
    for cand in (
        os.environ.get("CLANG_TIDY"),
        r"C:/Users/example\scoop\apps\llvm\current\bin\clang-tidy.exe",
        "clang-tidy",
    ):
        if not cand:
            continue
        if os.path.isabs(cand) and os.path.isfile(cand):
            return cand
        if not os.path.isabs(cand):
            return cand
    return "clang-tidy"


def tool_version(exe: str) -> str:
    try:
        p = subprocess.run([exe, "--version"], capture_output=True, text=True, errors="replace", timeout=120)
        lines = [ln.strip() for ln in p.stdout.splitlines() if ln.strip()]
        # `clang-tidy --version` prints "LLVM (http://llvm.org/):" on line 1 and
        # "  LLVM version 23.1.5" on line 2 -- recording line 1 alone loses the
        # only part that matters for reproducibility.
        for ln in lines:
            if "version" in ln.lower():
                return ln.replace("LLVM version", "LLVM").strip()
        return lines[0] if lines else "unknown"
    except Exception as exc:  # noqa: BLE001
        return f"unavailable ({exc})"


def log_name(tu: str) -> str:
    rel = os.path.relpath(tu, PROJECT_ROOT).replace("\\", "/")
    digest = hashlib.sha1(rel.encode()).hexdigest()[:8]
    base = os.path.basename(tu)
    return f"{base}.{digest}.log"


def run_one(job: tuple[str, str, int, str]) -> dict:
    # ⚠ Windows 的 multiprocessing 是 spawn：子进程重新 import 模块，父进程对 CHECKS 的
    #    修改不会带过去（--checks 曾因此被静默忽略）。所以 check 集必须随 job 一起传。
    tu, exe, timeout, checks = job
    rel = os.path.relpath(tu, PROJECT_ROOT).replace("\\", "/")
    logfile = os.path.join(LOGS_DIR, log_name(tu))
    args = [
        exe,
        "-p=" + os.path.dirname(DB),
        "--checks=" + checks,
        "--header-filter=" + HEADER_FILTER,
        "--use-color=0",
        "--extra-arg=-fno-color-diagnostics",
        tu,
    ]
    started = time.time()
    status = "ok"
    rc = -1
    out = ""
    try:
        proc = subprocess.run(
            args,
            cwd=os.path.dirname(DB),
            capture_output=True,
            text=True,
            errors="replace",
            timeout=timeout,
        )
        out = (proc.stdout or "") + (proc.stderr or "")
        rc = proc.returncode
        if re.search(r"^\d+ errors? generated", out, re.M) or re.search(r"error: (?:unable to|use of undeclared|"
                                                                       r"unknown type name|expected|"
                                                                       r"no template named)", out):
            status = "parse-error"
        if "Error while processing" in out:
            status = "parse-error"
        if rc != 0 and status == "ok":
            status = "error"
    except subprocess.TimeoutExpired as exc:
        out = ((exc.stdout or "") if isinstance(exc.stdout, str) else "") + "\n[timeout after %ss]\n" % timeout
        status = "timeout"
        rc = -9
    except Exception as exc:  # noqa: BLE001
        out = f"[launcher error] {exc}\n"
        status = "error"
        rc = -1

    with open(logfile, "w", encoding="utf-8") as fh:
        fh.write(out)
    return {
        "file": rel,
        "status": status,
        "rc": rc,
        "seconds": round(time.time() - started, 1),
        "log": os.path.relpath(logfile, PROJECT_ROOT).replace("\\", "/"),
    }


def parse_log(text: str) -> tuple[list[dict], list[str]]:
    diags: list[dict] = []
    errs: list[str] = []
    for line in text.splitlines():
        m = DIAG_RE.match(line)
        if m and m.group("check"):
            diags.append(
                {
                    "file": m.group("file").replace("\\", "/").replace("Y:/fpga-lsp-cpp/", ""),
                    "line": int(m.group("line")),
                    "col": int(m.group("col")),
                    "severity": m.group("sev"),
                    "check": m.group("check"),
                    "message": m.group("msg").strip(),
                }
            )
            continue
        me = COMPILE_ERR_RE.match(line)
        if me:
            errs.append(f"{me.group('file')}:{me.group('line')}:{me.group('col')}: {me.group('msg')}")
    return diags, errs


def in_src(path: str) -> bool:
    p = path.replace("\\", "/").lower()
    return p.startswith("src/") or "/src/" in p


def aggregate(tu_rows: list[dict], meta: dict) -> tuple[dict, str]:
    all_diags: list[dict] = []
    tu_errors: list[dict] = []
    for row in tu_rows:
        logfile = os.path.join(PROJECT_ROOT, row["log"])
        try:
            with open(logfile, encoding="utf-8", errors="replace") as fh:
                text = fh.read()
        except OSError:
            text = ""
        diags, errs = parse_log(text)
        for d in diags:
            d["tu"] = row["file"]
        all_diags.extend(diags)
        if errs or row["status"] in ("parse-error", "timeout", "error"):
            tu_errors.append({"tu": row["file"], "status": row["status"], "rc": row["rc"], "errors": errs[:8]})

    all_diags.sort(key=lambda d: (SEVERITY_ORDER.get(d["severity"], 5), d["file"], d["line"], d["check"]))
    # Findings located in the MSVC STL / Windows SDK are not ours to fix.  They
    # are kept (auditable) but excluded from every count so the src/ signal stays
    # readable -- `--header-filter` does not suppress the optin analyzer checks
    # that report at the header location.
    ext_diags = [d for d in all_diags if not in_src(d["file"])]
    all_diags = [d for d in all_diags if in_src(d["file"])]
    by_check: dict[str, int] = {}
    by_file: dict[str, int] = {}
    by_sev: dict[str, int] = {}
    for d in all_diags:
        by_check[d["check"]] = by_check.get(d["check"], 0) + 1
        by_file[d["file"]] = by_file.get(d["file"], 0) + 1
        by_sev[d["severity"]] = by_sev.get(d["severity"], 0) + 1

    nonzero = [r for r in tu_rows if r["status"] != "ok"]
    data = {
        "meta": dict(meta, total_tus=len(tu_rows), ok_tus=len(tu_rows) - len(nonzero),
                     diagnostics=len(all_diags)),
        "tu_summary": tu_rows,
        "tu_problems": tu_errors,
        "severity_counts": by_sev,
        "counts_by_check": dict(sorted(by_check.items(), key=lambda kv: -kv[1])),
        "counts_by_file": dict(sorted(by_file.items(), key=lambda kv: -kv[1])),
        "diagnostics": all_diags,
        "external_diagnostics": ext_diags,
    }

    out: list[str] = []
    w = out.append
    w("=" * 100)
    w("fpga-lsp-cpp static analysis -- clang-tidy")
    w("=" * 100)
    w(f"generated      : {meta['generated']}")
    w(f"tool           : {meta['tool_version']}")
    w(f"compilation db : doc/audit/_db/compile_commands.json ({meta['total_tus']} TUs, src/ only, cl->clang rewritten)")
    w(f"checks         : {CHECKS}")
    w(f"header-filter  : {HEADER_FILTER}")
    w(f"jobs           : {meta['jobs']}, per-TU timeout: {meta['timeout']}s")
    w(f"TUs analysed   : {len(tu_rows)}   clean/ok: {len(tu_rows) - len(nonzero)}   "
      f"problem: {len(nonzero)}")
    w(f"diagnostics    : {len(all_diags)} in src/  " + ", ".join(f"{k}={v}" for k, v in sorted(by_sev.items()))
      + (f"   (+{len(ext_diags)} suppressed outside src/, see section 0)" if ext_diags else ""))
    w("")
    if ext_diags:
        w("-" * 100)
        w(f"0. EXTERNAL FINDINGS (system headers, not fpga-lsp code) -- {len(ext_diags)}, excluded from counts")
        w("-" * 100)
        ext_by_file: dict[str, int] = {}
        for d in ext_diags:
            ext_by_file[d["file"]] = ext_by_file.get(d["file"], 0) + 1
        for f, n in sorted(ext_by_file.items(), key=lambda kv: -kv[1]):
            w(f"  {n:5d}  {f}")
        w("")
    w("-" * 100)
    w("1. SEVERITY SUMMARY")
    w("-" * 100)
    for sev in ("error", "warning", "remark", "note"):
        if sev in by_sev:
            w(f"  {sev:<10} {by_sev[sev]}")
    w("")
    w("-" * 100)
    w(f"2. FINDINGS BY CHECK  ({len(by_check)} distinct checks)")
    w("-" * 100)
    for check, n in sorted(by_check.items(), key=lambda kv: -kv[1]):
        w(f"  {n:5d}  {check}")
    w("")
    w("-" * 100)
    w("3. FINDINGS BY FILE  (top 30)")
    w("-" * 100)
    for i, (f, n) in enumerate(sorted(by_file.items(), key=lambda kv: -kv[1])[:30]):
        bar = "#" * min(60, n)
        w(f"  {n:5d}  {f:<58} {bar}")
    w("")
    groups = (
        ("4. clang-analyzer-* (path-sensitive: null deref / leak / uninit / dead store)", "clang-analyzer-"),
        ("5. concurrency-* (data races, misuse of locks/threads)", "concurrency-"),
        ("6. cert-* (CERT C++ secure-coding rules)", "cert-"),
        ("7. bugprone-* (likely bugs)", "bugprone-"),
        ("8. cppcoreguidelines-* (Core Guidelines)", "cppcoreguidelines-"),
        ("9. performance-* (avoidable copies, pessimizations)", "performance-"),
        ("10. readability-* (maintainability / correctness-adjacent)", "readability-"),
        ("11. modernize-* (representative hits for MODERNIZATION.md)", "modernize-"),
    )
    for title, prefix in groups:
        hits = [d for d in all_diags if d["check"].startswith(prefix)]
        w("-" * 100)
        w(f"{title}   [{len(hits)} hits]")
        w("-" * 100)
        if not hits:
            w("  (none)")
            w("")
            continue
        per_check: dict[str, int] = {}
        for d in hits:
            per_check[d["check"]] = per_check.get(d["check"], 0) + 1
        w("  by check: " + ", ".join(f"{k}={v}" for k, v in sorted(per_check.items(), key=lambda kv: -kv[1])))
        w("")
        shown = 0
        for d in hits:
            if shown >= 200:
                w(f"  ... {len(hits) - shown} more (see clang-tidy.json)")
                break
            w(f"  {d['file']}:{d['line']}:{d['col']}  [{d['check']}]")
            w(f"      {d['message']}")
            shown += 1
        w("")
    w("-" * 100)
    w("12. TUs WITH PROBLEMS (timeout / parse-error / tool error) -- NOT analysed, not clean")
    w("-" * 100)
    if not tu_errors:
        w("  (none)")
    for row in tu_errors:
        w(f"  {row['status']:<12} rc={row['rc']:<4} {row['tu']}")
        for e in row["errors"]:
            w(f"      {e}")
    w("")
    w("-" * 100)
    w("13. RAW clang-tidy OUTPUT, per TU")
    w("-" * 100)
    for row in sorted(tu_rows, key=lambda r: r["file"]):
        w("")
        w(f"### TU: {row['file']}   status={row['status']} rc={row['rc']} {row['seconds']}s")
        w(f"### log: {row['log']}")
        try:
            with open(os.path.join(PROJECT_ROOT, row["log"]), encoding="utf-8", errors="replace") as fh:
                for line in fh:
                    w("    " + line.rstrip("\n"))
        except OSError as exc:
            w(f"    [log unreadable: {exc}]")
    return data, "\n".join(out) + "\n"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--db", default=DB)
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--clang-tidy", default=None)
    ap.add_argument("--only", default=None, help="substring filter on TU path (debugging)")
    ap.add_argument(
        "--checks",
        default=None,
        help="override the check set (comma separated; default = the audit CHECKS)",
    )
    ap.add_argument("--aggregate-only", action="store_true")
    args = ap.parse_args(argv)
    if args.checks:
        # 维度审计（性能/安全/现代化）各跑各的子集，避免每次都全量扫。
        global CHECKS
        CHECKS = args.checks

    os.makedirs(LOGS_DIR, exist_ok=True)
    os.makedirs(AUDIT_DIR, exist_ok=True)

    with open(args.db, encoding="utf-8") as fh:
        db = json.load(fh)
    tus = [e["file"] for e in db]
    if args.only:
        tus = [t for t in tus if args.only.lower() in t.lower()]

    exe = args.clang_tidy or clang_tidy_exe()
    meta = {
        "generated": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "tool_version": tool_version(exe),
        "checks": CHECKS,
        "header_filter": HEADER_FILTER,
        "db": os.path.relpath(args.db, PROJECT_ROOT).replace("\\", "/"),
        "total_tus": len(tus),
        "jobs": args.jobs,
        "timeout": args.timeout,
    }

    rows: list[dict] = []
    if args.aggregate_only:
        with open(OUT_JSON, encoding="utf-8") as fh:
            prev = json.load(fh)
        rows = prev["tu_summary"]
        meta["generated"] = "re-aggregated " + time.strftime("%Y-%m-%dT%H:%M:%S%z")
    else:
        print(f"[tidy] {len(tus)} TUs, jobs={args.jobs}, timeout={args.timeout}s, tool={meta['tool_version']}")
        jobs = [(t, exe, args.timeout, CHECKS) for t in tus]
        with mp.Pool(processes=max(1, min(args.jobs, len(jobs) or 1))) as pool:
            for i, row in enumerate(pool.imap_unordered(run_one, jobs), 1):
                rows.append(row)
                print(f"  [{i}/{len(jobs)}] {row['status']:<11} {row['seconds']:6.1f}s  {row['file']}", flush=True)
        rows.sort(key=lambda r: r["file"])

    data, text = aggregate(rows, meta)
    with open(OUT_JSON, "w", encoding="utf-8") as fh:
        json.dump(data, fh, indent=1)
        fh.write("\n")
    with open(OUT_TXT, "w", encoding="utf-8") as fh:
        fh.write(text)
    print(f"[tidy] wrote {OUT_TXT}")
    print(f"[tidy] wrote {OUT_JSON}  ({len(data['diagnostics'])} diagnostics, "
          f"{len(data['tu_problems'])} problematic TUs)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())