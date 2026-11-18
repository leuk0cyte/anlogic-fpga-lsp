"""Regenerate the committed snapshots from a collection run directory.

Normal (team) usage is ``collect.py``; this helper exists so a snapshot set can be rebuilt from
an existing run without spending another ~13 minutes on TD sessions, and so the MCP category can
be re-reduced with the per-tool compaction policy.

    python tests/golden/tools/rebuild_snapshots.py <run_dir> [--out DIR]
"""

from __future__ import annotations

import argparse
import json
import os
import sys

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
GOLDEN_DIR = os.path.dirname(TOOLS_DIR)
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

import collect as C                                                    # noqa: E402
from goldenlib.normalize import (                                      # noqa: E402
    Normalizer, build_snapshot, dump_json, _measure, _digest,
)

CATEGORY_FILES = ("project/project.json", "hdl/hdl.json", "semantics/semantics.json",
                  "hierarchy/hierarchy.json", "diagnostics/diagnostics.json")

# Small tool results are kept verbatim; bulky ones are reduced to a digest so one category file
# stays inside the snapshot budget while every tool remains individually locatable in a diff.
MCP_INLINE_MAX_LINES = 40
MCP_INLINE_MAX_BYTES = 4000


def compact_tool(payload):
    """Delegate to the single implementation in collect.py (same policy on both paths)."""
    return C.compact_tool_result(payload)


def rebuild_mcp(raw, normalizer):
    """Recover case payloads from a possibly-truncated mcp snapshot.

    If the file still holds its cases, just re-reduce them.  If a run's mcp snapshot had to be
    collapsed (too large even after reduction), the payloads are gone and must be recollected
    with ``--recollect-mcp``.
    """
    cases = raw.get("cases")
    if not isinstance(cases, list) or not cases:
        return []
    recovered = []
    for case in cases:
        if isinstance(case, dict) and isinstance(case.get("result"), dict):
            recovered.append(case)
        else:
            raise SystemExit(
                "this run directory no longer carries MCP payloads (it was truncated at capture "
                "time); re-run collect.py for the mcp category instead")
    return recovered


def main(argv=None):
    parser = argparse.ArgumentParser(description="Rebuild snapshots from a collection run")
    parser.add_argument("run", help="directory produced by collect.py --out")
    parser.add_argument("--out", default=os.path.join(GOLDEN_DIR, "snapshots"))
    parser.add_argument("--corpus", default=os.path.join(GOLDEN_DIR, "corpus", "cases.json"))
    parser.add_argument("--sandbox", default=os.path.join(GOLDEN_DIR, "_sandbox"))
    args = parser.parse_args(argv)

    spec = C.load_corpus(args.corpus)
    corpus = C.Corpus(spec, os.path.join(args.sandbox, "synthetic"))
    corpus.resolve()
    normalizer = Normalizer(C.build_roots(corpus, spec["referenceRepository"]))
    skipped = normalizer.normalize([{"id": pid, "reason": reason}
                                    for pid, reason in corpus.skipped_projects.items()])

    def write(rel, text):
        path = os.path.join(args.out, *rel.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
        print("%-34s %7d bytes %5d lines" % (rel, len(text.encode("utf-8")), text.count("\n")))

    for rel in CATEGORY_FILES:
        source = os.path.join(args.run, *rel.split("/"))
        if not os.path.isfile(source):
            print("missing in run, skipped: %s" % rel)
            continue
        raw = json.load(open(source, encoding="utf-8"))
        cases = [normalizer.normalize(case) for case in (raw.get("cases") or [])]
        write(rel, build_snapshot(cases, extra={"category": raw.get("category"),
                                                "skipped": skipped}))

    # mcp: re-reduce each tool result individually.
    mcp_source = os.path.join(args.run, "mcp", "mcp.json")
    if os.path.isfile(mcp_source):
        raw = json.load(open(mcp_source, encoding="utf-8"))
        rebuilt = rebuild_mcp(raw, normalizer)
        cases = [normalizer.normalize(case) for case in rebuilt]
        write("mcp/mcp.json", build_snapshot(cases, extra={"category": "mcp",
                                                           "skipped": skipped}))
    else:
        print("missing in run, skipped: mcp/mcp.json (use --recollect-mcp)")

    # _meta.json and corpus.manifest.json are plain documents, not case lists.
    meta_source = os.path.join(args.run, "_meta.json")
    if os.path.isfile(meta_source):
        write("_meta.json", dump_json(normalizer.normalize(
            json.load(open(meta_source, encoding="utf-8")))))
    write("corpus.manifest.json", dump_json(C.corpus_manifest(corpus, normalizer)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())