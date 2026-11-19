#!/usr/bin/env python3
"""Mechanical guard for the Tcl-injection invariant (SEC-01 / SEC-04).

Why this exists
---------------
SEC-01 in `audit/2026-11-19/static/SECURITY.md` was not a typo: a previous fix
introduced `src/td/tcl_quote.h` and adopted it at 21 call sites, but one
hand-copied `import_device` loop in `commands.cpp` kept concatenating
`<HardWare><Device>` from the `.al` straight into the command string. The
existing test suite could not catch it, because for benign device names the
quoted and unquoted forms are byte-identical.

So the invariant needs to be checked mechanically:

  Every Tcl command handed to a TD/BitWizard session must be built from
  `TclQuote`, `TclQuoteGrouped`, `IsTclToken` + literal, or
  `MakeImportDeviceCommand`. Raw concatenation of a project field is a defect.

This script flags, for every `.Run(...)` / `RunCommand(...)` / `.RunTcl(...)`
call under `src/`:
  1. manual brace interpolation  -> `"open_project {" + al + "}"`
  2. untrusted project fields concatenated without a quoting helper
  3. `+ "}"` / `{" +` fragments anywhere in the argument expression

Exit code 0 = clean, 1 = violations (suitable as a CI gate).

Usage
-----
    python tools/audit/check_tcl_invariant.py
    python tools/audit/check_tcl_invariant.py --verbose
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import sys

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# Function-call prefixes whose single argument is a Tcl command line.
CALL_RE = re.compile(r"(?:\.|->)\s*(?:Run|RunCommand|SendCommand|RunTcl)\s*\(")

# A quoting helper makes the expression safe.
SAFE_HELPERS = ("TclQuote", "TclQuoteGrouped", "IsTclToken", "IsTclIdentifier",
                "TryTclQuote", "MakeImportDeviceCommand", "SpeedArgSuffix")

# Project fields that come from untrusted `.al` content (see al_project.cpp).
UNTRUSTED_FIELDS = re.compile(
    r"\b(?:project|proj|owner->project_|project_)\s*(?:\.|->)\s*"
    r"(?:device|top|family|name|speed)\b")

# Manual brace interpolation, e.g. `"open_project {" + al` or `+ "}"`.
BRACE_OPEN = re.compile(r'"[^"]*\{\s*"\s*\+')
BRACE_CLOSE = re.compile(r'\+\s*"\s*\}')


def argument_text(text: str, open_paren: int) -> tuple[str, int]:
    """Return the balanced argument text starting at `open_paren` and its line."""
    depth = 0
    i = open_paren
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"':
            i += 1
            while i < n and text[i] != '"':
                if text[i] == "\\":
                    i += 1
                i += 1
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return text[open_paren + 1:i], text.count("\n", 0, open_paren) + 1
        i += 1
    return text[open_paren + 1:], text.count("\n", 0, open_paren) + 1


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args(argv)

    violations: list[str] = []
    scanned = 0
    calls = 0

    files = sorted(glob.glob(os.path.join(PROJECT_ROOT, "src", "**", "*.cpp"), recursive=True))
    files += sorted(glob.glob(os.path.join(PROJECT_ROOT, "src", "**", "*.h"), recursive=True))

    for path in files:
        rel = os.path.relpath(path, PROJECT_ROOT).replace("\\", "/")
        try:
            text = open(path, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        scanned += 1
        for m in CALL_RE.finditer(text):
            calls += 1
            arg, line = argument_text(text, m.end() - 1)
            esc = f"{rel}:{line}"
            # ignore calls that are pure literals with no concatenation at all
            if "+" not in arg:
                continue
            safe = any(h in arg for h in SAFE_HELPERS)
            problems = []
            if BRACE_OPEN.search(arg) or BRACE_CLOSE.search(arg):
                problems.append("manual brace interpolation ({...}) around a concatenated value")
            if UNTRUSTED_FIELDS.search(arg) and not safe:
                problems.append("untrusted project field concatenated without a Tcl quoting helper")
            if problems and "":  # keep structure explicit
                pass
            for p in problems:
                violations.append(f"{esc}  {p}\n      {arg.strip()[:160]}")
            if args.verbose and not problems and "+" in arg:
                print(f"  ok  {esc}")

    print(f"[tcl-invariant] scanned {scanned} files, {calls} Tcl command call sites")
    if not violations:
        print("[tcl-invariant] PASS -- no raw concatenation of untrusted values")
        return 0
    print(f"[tcl-invariant] FAIL -- {len(violations)} violation(s)\n")
    for v in violations:
        print("  " + v)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())