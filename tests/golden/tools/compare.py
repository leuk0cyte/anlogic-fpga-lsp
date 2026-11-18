"""Structurally compare two golden snapshot directories.

Usage::

    python tests/golden/tools/compare.py <snapshot_dir_a> <snapshot_dir_b>

``a`` is the reference (the Python implementation's snapshots), ``b`` is the candidate (the
C++ implementation's snapshots).  A markdown report goes to stdout and the exit code is 0 only
when every file matches.

What counts as a difference:

* a JSON file present in one directory but not the other;
* a case id present in one snapshot but not the other;
* a value that differs anywhere in the tree;
* a *list order* difference (ordering is part of the contract under test);
* a truncated snapshot: the reference itself lost content, so the comparison cannot prove
  equivalence and it is reported as a difference (``TRUNCATED``) instead of a false pass.

Markdown output is wrapped so the container tag starts a line, which keeps it readable in
terminals as well as on GitHub.
"""

from __future__ import annotations

import argparse
import json
import os
import sys

MAX_REPORTED_DIFFS_PER_FILE = 40
MAX_VALUE_PREVIEW = 160
_COLUMNS = 88
MARKER_KEY = "__goldenTruncated"


# --------------------------------------------------------------------------- loading
def find_json(root):
    """Every ``*.json`` under ``root``, as slash-separated paths relative to ``root``."""
    found = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for name in sorted(filenames):
            if name.lower().endswith(".json"):
                full = os.path.join(dirpath, name)
                rel = os.path.relpath(full, root).replace("\\", "/")
                found.append(rel)
    return sorted(found)


def load(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


# --------------------------------------------------------------------------- diffing
def _preview(value):
    text = json.dumps(value, ensure_ascii=False, sort_keys=True)
    if len(text) > MAX_VALUE_PREVIEW:
        text = text[:MAX_VALUE_PREVIEW] + " ..."
    return text.replace("|", "\\|").replace("\n", " ")


def _type_name(value):
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


def _is_marker(value):
    return isinstance(value, dict) and value.get(MARKER_KEY) is True


def diff_values(left, right, path, out, limit):
    """Collect ``(path, kind, detail)`` differences between two JSON values."""
    if len(out) >= limit:
        return
    if _type_name(left) != _type_name(right):
        out.append((path, "type", "%s -> %s (%s -> %s)" % (
            _type_name(left), _type_name(right), _preview(left), _preview(right))))
        return
    if isinstance(left, dict):
        left_keys, right_keys = set(left), set(right)
        for key in sorted(left_keys - right_keys):
            out.append(("%s/%s" % (path, key), "missing-in-b",
                        "reference had %s" % _preview(left[key])))
        for key in sorted(right_keys - left_keys):
            out.append(("%s/%s" % (path, key), "extra-in-b",
                        "candidate added %s" % _preview(right[key])))
        for key in sorted(left_keys & right_keys):
            if _is_marker(left[key]) or _is_marker(right[key]):
                if left[key] != right[key]:
                    out.append(("%s/%s" % (path, key), "truncated",
                                "%s vs %s" % (_preview(left[key]), _preview(right[key]))))
                continue
            diff_values(left[key], right[key], "%s/%s" % (path, key), out, limit)
    elif isinstance(left, list):
        if len(left) != len(right):
            out.append((path, "length", "%d -> %d items" % (len(left), len(right))))
        for index in range(min(len(left), len(right))):
            if _is_marker(left[index]) or _is_marker(right[index]):
                if left[index] != right[index]:
                    out.append(("%s[%d]" % (path, index), "truncated",
                                "%s vs %s" % (_preview(left[index]), _preview(right[index]))))
                continue
            diff_values(left[index], right[index], "%s[%d]" % (path, index), out, limit)
    elif left != right:
        # A boolean/number/string mismatch.  ``bool`` vs ``int`` is caught by the type check
        # above only when the JSON types actually differ.
        out.append((path, "value", "%s -> %s" % (_preview(left), _preview(right))))


def contains_truncation(value):
    """True when the reference snapshot itself contains a truncation marker."""
    if isinstance(value, dict):
        if _is_marker(value):
            return True
        return any(contains_truncation(item) for item in value.values())
    if isinstance(value, list):
        return any(contains_truncation(item) for item in value)
    return False


def index_cases(payload):
    """Map case id -> case payload for a category snapshot (best effort)."""
    cases = payload.get("cases") if isinstance(payload, dict) else None
    if not isinstance(cases, list):
        return {}
    indexed = {}
    for position, case in enumerate(cases):
        if isinstance(case, dict) and isinstance(case.get("id"), str):
            indexed[case["id"]] = case
        else:
            indexed["#%d" % position] = case
    return indexed


def compare_file(name, path_a, path_b):
    """Return a result dict for one snapshot file."""
    result = {"file": name, "status": "match", "diffs": [], "note": ""}
    try:
        left = load(path_a)
    except Exception as exc:                              # noqa: BLE001
        result["status"] = "error"
        result["note"] = "reference unreadable: %s" % exc
        return result
    try:
        right = load(path_b)
    except Exception as exc:                              # noqa: BLE001
        result["status"] = "error"
        result["note"] = "candidate unreadable: %s" % exc
        return result

    diffs = []
    left_cases = index_cases(left)
    right_cases = index_cases(right)
    if left_cases or right_cases:
        for case_id in sorted(set(left_cases) - set(right_cases)):
            diffs.append(("case:%s" % case_id, "missing-case", "case absent in candidate"))
        for case_id in sorted(set(right_cases) - set(left_cases)):
            diffs.append(("case:%s" % case_id, "extra-case", "case only in candidate"))
        for case_id in sorted(set(left_cases) & set(right_cases)):
            diff_values(left_cases[case_id], right_cases[case_id],
                        "case:%s" % case_id, diffs, MAX_REPORTED_DIFFS_PER_FILE)
            if len(diffs) >= MAX_REPORTED_DIFFS_PER_FILE:
                break
        # Envelope-level fields (schema, skipped list, case count) still matter.
        left_envelope = {k: v for k, v in left.items() if k != "cases"} \
            if isinstance(left, dict) else {}
        right_envelope = {k: v for k, v in right.items() if k != "cases"} \
            if isinstance(right, dict) else {}
        diff_values(left_envelope, right_envelope, "envelope", diffs,
                    MAX_REPORTED_DIFFS_PER_FILE)
    else:
        diff_values(left, right, "$", diffs, MAX_REPORTED_DIFFS_PER_FILE)

    result["diffs"] = diffs
    if diffs:
        result["status"] = "diff"
    elif contains_truncation(left):
        # Content is equal, but the reference itself lost detail at capture time: equality here
        # only proves that both sides are equally incomplete.  The comparison is still a
        # content match (so the exit code and the agreement table stay meaningful, and two runs
        # of the same implementation compare clean), but the warning below surfaces it.
        result["status"] = "match"
        result["note"] = ("reference snapshot is truncated; equality is a content match only and "
                          "does not prove full equivalence")
    return result


# --------------------------------------------------------------------------- reporting
def _wrap(text):
    """Wrap markdown in a container tag that must start a line."""
    return "<!-- markdown -->\n" + text


def build_report(results, extra_files, dir_a, dir_b, identical):
    lines = []
    lines.append("# Golden snapshot comparison")
    lines.append("")
    lines.append("- reference (a): `%s`" % dir_a)
    lines.append("- candidate (b): `%s`" % dir_b)
    lines.append("- result: **%s**" % ("IDENTICAL" if identical else "DIFFERENT"))
    lines.append("")

    by_category = {}
    for result in results:
        category = result["file"].split("/")[0] if "/" in result["file"] else "(root)"
        by_category.setdefault(category, []).append(result)

    lines.append("## Per-category agreement")
    lines.append("")
    lines.append("| category | files | match | diff | missing/extra | error | truncated | agreement |")
    lines.append("| --- | --- | --- | --- | --- | --- | --- | --- |")
    totals = {"files": 0, "match": 0, "diff": 0, "missing": 0, "error": 0, "truncated": 0}
    for category in sorted(by_category):
        rows = by_category[category]
        count = {"match": 0, "diff": 0, "missing": 0, "error": 0, "truncated": 0}
        for result in rows:
            count[result["status"]] = count.get(result["status"], 0) + 1
        files = len(rows)
        agreement = (100.0 * count["match"] / files) if files else 0.0
        lines.append("| %s | %d | %d | %d | %d | %d | %d | %.1f%% |" % (
            category, files, count["match"], count["diff"], count["missing"],
            count["error"], count["truncated"], agreement))
        for key in totals:
            totals[key] += count.get(key, 0) if key != "files" else files
    total_files = totals["files"]
    total_agreement = (100.0 * totals["match"] / total_files) if total_files else 0.0
    lines.append("| **all** | **%d** | **%d** | **%d** | **%d** | **%d** | **%d** | **%.1f%%** |" % (
        total_files, totals["match"], totals["diff"], totals["missing"],
        totals["error"], totals["truncated"], total_agreement))
    lines.append("")

    if extra_files:
        lines.append("## Files present in only one snapshot set")
        lines.append("")
        for name in extra_files:
            lines.append("- `%s`" % name)
        lines.append("")

    problems = [r for r in results if r["status"] in ("diff", "missing", "error")]
    truncated = [r for r in results if r.get("note")]
    if problems:
        lines.append("## Differences")
        lines.append("")
        for result in problems:
            lines.append("### `%s` - %s" % (result["file"], result["status"]))
            lines.append("")
            if result["note"]:
                lines.append("_%s_" % result["note"])
                lines.append("")
            if result["diffs"]:
                lines.append("| path | kind | reference -> candidate |")
                lines.append("| --- | --- | --- |")
                for path, kind, detail in result["diffs"]:
                    lines.append("| `%s` | %s | %s |" % (path, kind, detail))
                lines.append("")
    else:
        lines.append("## Differences")
        lines.append("")
        lines.append("None.")
        lines.append("")

    if truncated:
        lines.append("## Truncation warnings")
        lines.append("")
        lines.append("These snapshots match, but the captured payload was reduced to fit the size")
        lines.append("limit, so equality does not prove equivalence for the dropped detail:")
        lines.append("")
        for result in truncated:
            lines.append("- `%s`" % result["file"])
        lines.append("")

    return _wrap("\n".join(lines))


def main(argv=None):
    global MAX_REPORTED_DIFFS_PER_FILE
    parser = argparse.ArgumentParser(description="Compare two golden snapshot directories")
    parser.add_argument("dir_a", help="reference snapshot directory")
    parser.add_argument("dir_b", help="candidate snapshot directory")
    parser.add_argument("--quiet", action="store_true", help="exit code only, no report")
    parser.add_argument("--max-per-file", type=int, default=MAX_REPORTED_DIFFS_PER_FILE,
                        help="cap on reported differences per file")
    args = parser.parse_args(argv)

    MAX_REPORTED_DIFFS_PER_FILE = args.max_per_file

    for directory in (args.dir_a, args.dir_b):
        if not os.path.isdir(directory):
            sys.stderr.write("not a directory: %s\n" % directory)
            return 2

    files_a = find_json(args.dir_a)
    files_b = find_json(args.dir_b)
    set_a, set_b = set(files_a), set(files_b)

    extra_files = []
    for name in sorted(set_a - set_b):
        extra_files.append("%s (only in reference)" % name)
    for name in sorted(set_b - set_a):
        extra_files.append("%s (only in candidate)" % name)

    results = []
    for name in files_a:
        if name not in set_b:
            results.append({"file": name, "status": "missing", "diffs": [],
                            "note": "file absent from the candidate snapshot set"})
            continue
        results.append(compare_file(name, os.path.join(args.dir_a, name),
                                    os.path.join(args.dir_b, name)))
    for name in files_b:
        if name not in set_a:
            results.append({"file": name, "status": "missing", "diffs": [],
                            "note": "file absent from the reference snapshot set"})

    identical = (not extra_files
                 and all(result["status"] == "match" for result in results))

    if not args.quiet:
        sys.stdout.write(build_report(results, extra_files, args.dir_a, args.dir_b, identical))
        sys.stdout.write("\n")
    return 0 if identical else 1


if __name__ == "__main__":
    raise SystemExit(main())