"""F3 判据：`qor.suggestions` 的 step/顺序在**基线 exe** 上是否本来就不稳定。

做法：同一个工程、同一个参数（`fpgaLsp/reports` + `section=qor`），每个 exe 连跑 3 次，
比较 `qor.step` 与 `qor.suggestions[*].step` 的序列。

- 基线 exe：`%TEMP%\\fpgalsp-gate\\baseline-exe\\fpgalsp-native-baseline.exe`（7EDC9A67…）
- 新版 exe：`build\\bin\\fpgalsp-native.exe`

用法：python tools/perf/reports/check_suggestions_stability.py
"""

import argparse
import json
import os
import sys
import tempfile

CPP_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, os.path.join(CPP_ROOT, "tests", "golden", "tools"))
os.environ["PYTHONPATH"] = r"Y:\fpga-lsp" + os.pathsep + os.environ.get("PYTHONPATH", "")

from goldenlib.client import LspClient, NativeLspClient, path_to_uri  # noqa: E402


def probe(exe, project_dir, log_dir):
    client = NativeLspClient(CPP_ROOT, os.path.join(log_dir, "x.log"), exe_path=exe)
    try:
        client.initialize(project_dir, timeout=300.0)
        top = os.path.join(project_dir, "report100.al")
        out = []
        for _ in range(3):
            payload = client.request("fpgaLsp/reports",
                                     {"textDocument": {"uri": path_to_uri(top)},
                                      "section": "qor"},
                                     timeout=180.0)
            qor = (payload or {}).get("qor") or {}
            out.append({
                "qorStep": qor.get("step"),
                "suggestionSteps": [s.get("step") for s in (qor.get("suggestions") or [])],
                "suggestionCount": len(qor.get("suggestions") or []),
            })
        return out
    finally:
        try:
            client.close()
        except Exception:  # noqa: BLE001
            pass


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project", default=os.path.join(
        os.environ.get("TEMP", "."), "fpgalsp-perf", "reports", "report100"))
    parser.add_argument("--baseline", default=os.path.join(
        os.environ.get("TEMP", "."), "fpgalsp-gate", "baseline-exe",
        "fpgalsp-native-baseline.exe"))
    parser.add_argument("--current", default=os.path.join(
        CPP_ROOT, "build", "bin", "fpgalsp-native.exe"))
    args = parser.parse_args()
    log_dir = os.path.join(tempfile.gettempdir(), "fpgalsp-fix", "f3")
    os.makedirs(log_dir, exist_ok=True)

    result = {}
    for label, exe in (("baseline", args.baseline), ("current", args.current)):
        if not os.path.isfile(exe):
            print("%s: exe 不存在 %s" % (label, exe))
            continue
        runs = probe(exe, args.project, log_dir)
        result[label] = runs
        stable = all(run == runs[0] for run in runs)
        print("%-9s stable=%s" % (label, stable))
        for index, run in enumerate(runs):
            print("   #%d qorStep=%-6s suggestions=%s" % (index + 1, run["qorStep"],
                                                          run["suggestionSteps"]))
    if "baseline" in result and "current" in result:
        base = result["baseline"][0]
        cur = result["current"][0]
        print("baseline vs current 首次结果相同: %s" % (base == cur))
    out = os.path.join(log_dir, "suggestions-stability.json")
    with open(out, "w", encoding="utf-8") as handle:
        json.dump(result, handle, ensure_ascii=False, indent=1)
    print("wrote %s" % out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())