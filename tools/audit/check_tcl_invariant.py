#!/usr/bin/env python3
"""Mechanical guard for the Tcl-injection invariant (SEC-01 / SEC-04).

Why this exists
---------------
SEC-01 in `doc/audit/2026-11-19/static/SECURITY.md` was not a typo: a previous fix
introduced `src/td/tcl_quote.h` and adopted it at 21 call sites, but one
hand-copied `import_device` loop in `commands.cpp` kept concatenating
`<HardWare><Device>` from the `.al` straight into the command string. The
existing test suite could not catch it, because for benign device names the
quoted and unquoted forms are byte-identical.

So the invariant needs to be checked mechanically:

  Every Tcl command handed to a TD/BitWizard session must be built from
  `TclQuote`, `TclQuoteGrouped`, `IsTclToken` + literal, or
  `MakeImportDeviceCommand`. Raw concatenation of a project field is a defect.

Rules
-----
R0 (call site, pre-existing -- do not weaken)
  For every `.Run(...)` / `RunCommand(...)` / `.RunTcl(...)` call under `src/`,
  flag the argument expression when it concatenates (`+`) and
    1. hand-interpolates braces   -> `"open_project {" + al + "}"`,
    2. concatenates an untrusted project field with no quoting helper.
R1 (data flow, added for S-02 / T-10)
  The same brace-interpolation pattern assigned to a string variable
  (`const std::string commands = "open_project {" + al + "}";`) and only later
  passed to `.Run(commands)` bypasses R0 completely. R1 flags such assignments
  (plain `=` and `+=`) when the statement looks Tcl-bound: it mentions a Tcl
  command keyword, or the variable is later used at a Tcl/file sink.
  `features.cpp`-style code generation (`"${" + ...`, JS/JSON, no Tcl keyword)
  is deliberately not flagged.
R2 (sink, added for S-01 / T-10)
  `chip_debugger/user.mdn` is a Tcl script executed by TD. Any function that
  builds a `"mark_debug ..."` string literal must reference a validation helper
  (`ValidateProbeSignalNames` / `IsValidProbeSignalName` / `TclQuote*`), because
  an unvalidated signal name containing `}` closes the `mark_debug { ... }`
  list early and turns the rest into new Tcl commands.

Exit code 0 = clean, 1 = violations (suitable as a CI gate).
`--self-test` runs the rules against embedded good/bad samples (positive and
negative controls) so the guard itself is testable without adding fixtures.

Usage
-----
    python tools/audit/check_tcl_invariant.py
    python tools/audit/check_tcl_invariant.py --verbose
    python tools/audit/check_tcl_invariant.py --self-test
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

# A quoting helper makes the expression safe (R0 keeps the pre-existing set).
SAFE_HELPERS = ("TclQuote", "TclQuoteGrouped", "IsTclToken", "IsTclIdentifier",
                "TryTclQuote", "MakeImportDeviceCommand", "SpeedArgSuffix",
                "ValidateProbeSignalNames", "IsValidProbeSignalName")

# R1 needs a helper that quotes *the concatenated value itself*.
# `MakeImportDeviceCommand(db, device, speed)` only quotes its own arguments, so
# it must NOT make `"open_project {" + al + "}"` look safe.
QUOTE_HELPERS = ("TclQuote", "TclQuoteGrouped", "TryTclQuote", "IsTclToken",
                 "IsTclIdentifier", "ValidateProbeSignalNames", "IsValidProbeSignalName")

# Project fields that come from untrusted `.al` content (see al_project.cpp).
UNTRUSTED_FIELDS = re.compile(
    r"\b(?:project|proj|owner->project_|project_)\s*(?:\.|->)\s*"
    r"(?:device|top|family|name|speed)\b")

# Manual brace interpolation, e.g. `"open_project {" + al` or `+ "}"`.
BRACE_OPEN = re.compile(r'"[^"]*\{\s*"\s*\+')
BRACE_CLOSE = re.compile(r'\+\s*"\s*\}')

# R1: assignments/compound-appends whose RHS is a candidate Tcl command build.
ASSIGN_RE = re.compile(
    r"(?m)^[ \t]*(?:const\s+)?(?:std::string|std::string_view|auto|string)\s+"
    r"([A-Za-z_]\w*)\s*=")
APPEND_RE = re.compile(r"(?m)^[ \t]*([A-Za-z_]\w*)\s*\+=")

# Keywords that make a brace-interpolating assignment "Tcl-bound". Keep this a
# substring check: the goal is a conservative detector, not a Tcl parser.
TCL_KEYWORDS = (
    "open_project", "import_db", "import_device", "insert_debugger", "-cwc",
    "read_hdl", "mark_debug", "config_chipwatcher", "write_hw_cw_data",
    "set_param", "launch_runs", "reset_runs", "wait_run", "svfgen",
    "download ", "program ",
)

# Tcl / TD-file sinks; R1 also flags when the variable is passed to one of them.
SINK_CALL = (r"(?:\.\s*(?:Run|RunCommand|SendCommand|RunTcl)\s*\("
             r"|(?:platform::writes::)?WriteTextFile\s*\("
             r"|(?:platform::)?writes::Write\s*\()")

# R2: a built `mark_debug { ... }` list is a Tcl script that TD executes.
# Only the brace-group form matters here (`mark_debug {`); plain prose that
# happens to contain the word (error messages) must not trigger the rule.
MARK_DEBUG_LITERAL = re.compile(r'"mark_debug\s*\{')
VALIDATION_HELPERS = ("ValidateProbeSignalNames", "IsValidProbeSignalName",
                      "NormaliseSignals")

IDENTIFIER = re.compile(r"[A-Za-z_]")


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


def _skip_string(text: str, i: int) -> int:
    """`i` points at an opening quote; return the index just past the literal."""
    quote = text[i]
    i += 1
    n = len(text)
    while i < n:
        if text[i] == "\\":
            i += 2
            continue
        if text[i] == quote:
            return i + 1
        i += 1
    return n


def statement_rhs(text: str, start: int) -> tuple[str, int]:
    """Return the text from `start` to the closing `;` at bracket depth 0."""
    depth = 0
    i = start
    n = len(text)
    while i < n:
        c = text[i]
        if c in "\"'":
            i = _skip_string(text, i)
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == ";" and depth <= 0:
            return text[start:i], i
        i += 1
    return text[start:], n


def strip_string_literals(expr: str) -> str:
    """Drop string/char literals so only actual code operands remain."""
    out: list[str] = []
    i = 0
    n = len(expr)
    while i < n:
        if expr[i] in "\"'":
            i = _skip_string(expr, i)
            out.append('""')
            continue
        out.append(expr[i])
        i += 1
    return "".join(out)


def var_used_in_sink(text: str, start: int, var: str) -> bool:
    tail = text[start:start + 6000]
    pattern = re.compile(SINK_CALL + r"[^;]{0,600}?\b" + re.escape(var) + r"\b")
    return bool(pattern.search(tail))


def block_spans(text: str) -> list[tuple[int, int]]:
    """Return (open, close) indices of every brace block, ignoring literals/comments."""
    spans: list[tuple[int, int]] = []
    stack: list[int] = []
    i = 0
    n = len(text)
    while i < n:
        if text.startswith("//", i):
            newline = text.find("\n", i)
            i = n if newline < 0 else newline + 1
            continue
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            i = n if end < 0 else end + 2
            continue
        c = text[i]
        if c in "\"'":
            i = _skip_string(text, i)
            continue
        if c == "{":
            stack.append(i)
        elif c == "}" and stack:
            spans.append((stack.pop(), i))
        i += 1
    return spans


_MODIFIERS = ("const", "override", "final", "noexcept")


def is_function_block(text: str, open_idx: int) -> bool:
    """True when the `{` at open_idx belongs to a function/lambda body.

    Namespace/class/struct blocks and plain control blocks at file scope are not
    functions; nested control blocks (if/for/while) also look function-like, but
    `enclosing_function` picks the *outermost* function-like block, which is the
    real function for code under a namespace.
    """
    j = open_idx - 1
    while j >= 0 and text[j] in " \t\r\n":
        j -= 1
    while j >= 0 and (text[j].isalnum() or text[j] == "_"):
        k = j
        while k >= 0 and (text[k].isalnum() or text[k] == "_"):
            k -= 1
        word = text[k + 1:j + 1]
        if word not in _MODIFIERS:
            break
        j = k
        while j >= 0 and text[j] in " \t\r\n":
            j -= 1
    return j >= 0 and text[j] == ")"


def enclosing_function(text: str, pos: int, spans: list[tuple[int, int]]) -> str | None:
    candidates = [span for span in spans
                  if span[0] < pos < span[1] and is_function_block(text, span[0])]
    if not candidates:
        return None
    open_idx, close_idx = min(candidates)
    return text[open_idx:close_idx + 1]


def _snippet(expr: str) -> str:
    return expr.strip()[:160]


def scan_text(rel: str, text: str, verbose: bool = False) -> tuple[list[str], int]:
    """Run R0/R1/R2 over one source file; returns (violations, call_sites)."""
    violations: list[str] = []
    calls = 0

    # ---- R0: hand-built Tcl command at the `.Run(...)` call site ------------
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
        for p in problems:
            violations.append(f"{esc}  {p}\n      {_snippet(arg)}")
        if verbose and not problems:
            print(f"  ok  {esc}")

    # ---- R1: brace interpolation assigned to a variable (S-02 pattern) -----
    # `const std::string commands = "open_project {" + al + "}";` then
    # `session->Run(commands)` -- the R0 scan sees no `+` at the call site.
    for regex in (ASSIGN_RE, APPEND_RE):
        for m in regex.finditer(text):
            var = m.group(1)
            rhs, stop = statement_rhs(text, m.end())
            if not (BRACE_OPEN.search(rhs) or BRACE_CLOSE.search(rhs)):
                continue
            if any(h in rhs for h in QUOTE_HELPERS):
                continue
            if not IDENTIFIER.search(strip_string_literals(rhs)):
                continue  # only literals: e.g. `out = "a {" "}";` is odd but not data flow
            tcl_bound = any(k in rhs for k in TCL_KEYWORDS) or var_used_in_sink(text, stop, var)
            if not tcl_bound:
                continue
            line = text.count("\n", 0, m.start()) + 1
            violations.append(
                f"{rel}:{line}  hand-built brace interpolation assigned to `{var}` "
                "(pre-built Tcl command bypasses the Run(...) call-site rule)\n"
                f"      {_snippet(rhs)}")
            if verbose:
                print(f"  r1  {rel}:{line} `{var}`")

    # ---- R2: mark_debug list written into chip_debugger/user.mdn ----------
    spans = block_spans(text)
    for m in MARK_DEBUG_LITERAL.finditer(text):
        line = text.count("\n", 0, m.start()) + 1
        body = enclosing_function(text, m.start(), spans)
        if body is None:
            continue  # e.g. `constexpr std::string_view kMarkDebug = "mark_debug";`
        if any(h in body for h in VALIDATION_HELPERS):
            continue
        # 每个 signal 走 TclQuote 也**不能**代替校验：TclQuote 返回的是一个 quoted
        # 单词，粘进 `mark_debug { ... }` 列表里仍然可能改变列表内容（比如空格）。
        # 规则要求的是"名字白名单校验"（NormaliseSignals 内部也是它）。
        violations.append(
            f"{rel}:{line}  `\"mark_debug{{` list built in a function without "
            "ValidateProbeSignalNames/IsValidProbeSignalName/NormaliseSignals "
            "(user.mdn is a Tcl script; unvalidated names can close the brace list)\n"
            f"      {_snippet(body)}")
        if verbose:
            print(f"  r2  {rel}:{line}")

    return violations, calls


# -----------------------------------------------------------------------------
# Self-test: positive/negative controls for the rules themselves.
# -----------------------------------------------------------------------------
SELF_TEST_CASES: list[tuple[str, str, bool]] = [
    (
        "r0-brace-at-call-site",
        'void f() {\n'
        '  session->Run("open_project {" + al + "} -single_run\\n");\n'
        '}\n',
        True,
    ),
    (
        "r0-quoted-at-call-site",
        'void f() {\n'
        '  session->Run("open_project " + td::TclQuoteGrouped(al) + " -single_run\\n");\n'
        '}\n',
        False,
    ),
    (
        "r1-s02-import-db-variable-then-run",
        'void f() {\n'
        '  const std::string commands =\n'
        '      td::MakeImportDeviceCommand(db, device, speed) + "\\n" +\n'
        '      "open_project {" + al + "} -single_run\\n" +\n'
        '      "import_db {" + gate_db + "}\\n";\n'
        '  session->Run(commands, 240.0);\n'
        '}\n',
        True,
    ),
    (
        "r1-s02-quoted-variable-then-run",
        'void f() {\n'
        '  const std::string commands =\n'
        '      td::MakeImportDeviceCommand(db, device, speed) + "\\n" +\n'
        '      "open_project " + td::TclQuoteGrouped(al) + " -single_run\\n" +\n'
        '      "import_db " + td::TclQuoteGrouped(gate_db) + "\\n";\n'
        '  session->Run(commands, 240.0);\n'
        '}\n',
        False,
    ),
    (
        "r1-insert-debugger-append",
        'void f() {\n'
        '  std::string insert_body = "insert_debugger {" + debugger_dir + "}";\n'
        '  insert_body += " -cwc {" + config_path + "}";\n'
        '  write_hook(path, insert_body);\n'
        '}\n',
        True,
    ),
    (
        "r1-js-brace-codegen-is-not-tcl",
        'void f() {\n'
        '  out += indent + "  ." + port.name + "(" + "${" + std::to_string(index) + ":" +\n'
        '         port.name + "}),\\n";\n'
        '}\n',
        False,
    ),
    (
        "r2-mark-debug-without-validation",
        'std::string RenderMdn(const std::vector<std::string>& signals) {\n'
        '  std::string command = "mark_debug { ";\n'
        '  for (const std::string& signal : signals) command += signal + " ";\n'
        '  command += "}";\n'
        '  return command;\n'
        '}\n',
        True,
    ),
    (
        "r2-mark-debug-validated",
        'std::string RenderMdn(const std::vector<std::string>& signals, std::string* error) {\n'
        '  std::string signal_error;\n'
        '  if (!ValidateProbeSignalNames(signals, &signal_error)) return {};\n'
        '  std::string command = "mark_debug { ";\n'
        '  for (const std::string& signal : signals) command += signal + " ";\n'
        '  command += "}";\n'
        '  return command;\n'
        '}\n',
        False,
    ),
    (
        "r2-constant-definition-is-not-a-build",
        'namespace {\n'
        'constexpr std::string_view kMarkDebug = "mark_debug";\n'
        '}\n',
        False,
    ),
]


def run_self_test() -> int:
    failures = 0
    for name, source, expect_violation in SELF_TEST_CASES:
        found, _ = scan_text(name + ".cpp", source)
        ok = bool(found) == expect_violation
        status = "ok" if ok else "FAIL"
        print(f"  [{status}] {name}: expected "
              f"{'violation' if expect_violation else 'clean'}, got {len(found)}")
        for item in found:
            print(f"        {item.splitlines()[0]}")
        if not ok:
            failures += 1
    if failures:
        print(f"[tcl-invariant] self-test FAIL -- {failures}/{len(SELF_TEST_CASES)} controls wrong")
        return 1
    print(f"[tcl-invariant] self-test PASS -- {len(SELF_TEST_CASES)} positive/negative controls")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--self-test", action="store_true",
                    help="run the rule positive/negative controls instead of scanning src/")
    args = ap.parse_args(argv)

    if args.self_test:
        return run_self_test()

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
        file_violations, file_calls = scan_text(rel, text, verbose=args.verbose)
        violations.extend(file_violations)
        calls += file_calls

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
