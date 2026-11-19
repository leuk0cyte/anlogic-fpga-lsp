"""Server-vs-plugin split for the interactive requests the extension sends.

The perf question in audit/2026-11-19/perf/README.md is "where does the time go between a file
change and the tree refreshing". This script measures the *wire* half (server + JSON-RPC +
JSON parse of the response) against the production binary, so VSCODE.md can subtract the
plugin-side numbers measured by bench_events.js / bench_activate.js.

It never writes into the read-only project ``%WORKSPACE%/CorpusAlpha``: the benchmark project is a %TEMP%
copy prepared by prep_workspace.js.

    python tools/perf/vscode/bench_lsp_roundtrip.py [--iterations 21] [--rounds 3]

Output: results/lsp-roundtrip.json (+ csv lines).
"""

from __future__ import annotations

import argparse
import json
import os
import statistics
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
CPP_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
TEMP = os.environ.get("TEMP", "/tmp")
WORKDIR = os.path.join(TEMP, "fpgalsp-perf", "vscode")
PROJECT = os.path.join(WORKDIR, "project")
RESULTS = os.path.join(HERE, "results")
EXE = os.path.join(CPP_ROOT, "build", "bin", "fpgalsp-native.exe")


def percentile(sorted_values, fraction):
    if not sorted_values:
        return 0.0
    index = min(len(sorted_values) - 1, max(0, int(round(fraction * (len(sorted_values) - 1)))))
    return sorted_values[index]


def summarize(samples):
    ordered = sorted(samples)
    return {
        "n": len(ordered),
        "p50": percentile(ordered, 0.5),
        "p95": percentile(ordered, 0.95),
        "min": ordered[0] if ordered else 0.0,
        "max": ordered[-1] if ordered else 0.0,
        "mean": statistics.fmean(ordered) if ordered else 0.0,
    }


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--iterations", type=int, default=21)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--exe", default=EXE)
    args = parser.parse_args(argv)

    if not os.path.isfile(args.exe):
        raise SystemExit(f"missing exe: {args.exe}")
    if not os.path.isdir(PROJECT):
        raise SystemExit(f"missing temp project: {PROJECT} (run: node tools/perf/vscode/prep_workspace.js)")

    sys.path.insert(0, os.path.join(CPP_ROOT, "tests", "golden", "tools"))
    from goldenlib.client import NativeLspClient, path_to_uri  # noqa: E402

    log_path = os.path.join(WORKDIR, "logs", "roundtrip-native.log")
    os.makedirs(os.path.dirname(log_path), exist_ok=True)
    client = NativeLspClient(CPP_ROOT, log_path, exe_path=args.exe)
    init = client.initialize(PROJECT)
    server_info = (init or {}).get("serverInfo") or {}

    top = os.path.join(PROJECT, "src", "top.v")
    top_uri = path_to_uri(top)
    al = os.path.join(PROJECT, "demo_project.al")

    cases = [
        ("moduleTree", "fpgaLsp/moduleTree", {"uri": top_uri}),
        ("reports.summary", "fpgaLsp/reports", {"section": "summary"}),
        ("reports.full(no flow)", "fpgaLsp/reports", {}),
        ("debugHighlight", "workspace/executeCommand",
         {"command": "fpgalsp.debugHighlight", "arguments": [{"uri": top_uri}]}),
        ("info", "workspace/executeCommand", {"command": "fpgalsp.info", "arguments": []}),
        ("indexChanged? (no such request)", "fpgaLsp/indexChanged", {}),
    ]

    results = {
        "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "exe": args.exe,
        "exeBytes": os.path.getsize(args.exe),
        "project": PROJECT,
        "serverInfo": server_info,
        "iterations": args.iterations,
        "rounds": args.rounds,
        "cases": {},
        "csv": [],
    }

    for name, method, params in cases:
        if method == "fpgaLsp/indexChanged":
            results["cases"][name] = {"note": "server -> client notification; there is no request"}
            continue
        rounds = []
        samples = []
        payload_bytes = 0
        error = None
        for _round in range(args.rounds):
            for _ in range(args.iterations):
                started = time.perf_counter()
                try:
                    result = client.request(method, params, timeout=120.0)
                except Exception as exc:  # noqa: BLE001 - transport failures are data here
                    error = f"{type(exc).__name__}: {exc}"
                    break
                elapsed_ms = (time.perf_counter() - started) * 1000.0
                samples.append(elapsed_ms)
                payload_bytes = len(json.dumps(result, ensure_ascii=False))
            if error:
                break
            rounds.append(summarize(samples[-args.iterations:]))
        summary = summarize(samples) if samples else None
        results["cases"][name] = {
            "method": method,
            "summary": summary,
            "rounds": rounds,
            "payloadBytes": payload_bytes,
            "error": error,
        }
        if summary:
            results["csv"].append(
                "csv,%s,%s,%.3f,%.3f,%d" % (name, summary["n"], summary["p50"], summary["p95"], payload_bytes))

    client.close()

    os.makedirs(RESULTS, exist_ok=True)
    target = os.path.join(RESULTS, "lsp-roundtrip.json")
    with open(target, "w", encoding="utf-8") as handle:
        json.dump(results, handle, ensure_ascii=False, indent=1)

    for row in results["csv"]:
        print(row)
    print("wrote %s" % target)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())