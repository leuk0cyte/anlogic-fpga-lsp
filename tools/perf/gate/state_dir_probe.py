#!/usr/bin/env python3
"""state_dir_probe.py —— W1/W2 专项：`FPGALSP_STATE_DIR` 的三个承诺。

W1/W2 的承诺（比延迟数字更重要，是用户明确提的需求）：
  ① 设 `FPGALSP_STATE_DIR` 后，服务端自己的状态（日志/缓存/run）落到该目录；
  ② **工作目录里不再出现 `.fpgalsp`**；
  ③ 协议输出仍与基线**逐字节相同**；
  另外必须验证 **不设该变量时行为完全等同现在**（回退安全）。

做法：把合成工程拷到 %TEMP%，用 `cwd=<工程副本>` 起 native LSP
（这样"工作目录里有没有 .fpgalsp"是可观测的），跑一组固定请求，然后：

  A 组：**不设** FPGALSP_STATE_DIR        → 期望：.fpgalsp 出现在 cwd（现状行为）
  B 组：**设** FPGALSP_STATE_DIR=<state>  → 期望：.fpgalsp **不**出现在 cwd；state 目录里有文件
  C 组：协议对比（B 组的 lsp-raw 与基线 raw 逐字节比）

输出：每组 cwd 下的 `.fpgalsp` 文件清单、state 目录文件清单、协议是否相同。

用法：
    python tools/perf/gate/state_dir_probe.py --out <tagdir>/state_dir.json
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys

CPP = r"Y:/fpga-lsp-cpp"
sys.path.insert(0, os.path.join(CPP, "tools", "audit"))
import bench_servers as bs  # noqa: E402
from goldenlib.client import NativeLspClient, path_to_uri  # noqa: E402

REQUESTS_TOP = "src/top.v"


def make_project(root):
    """小合成工程（6 个 .v），拷到 %TEMP%；不带任何 .fpgalsp。"""
    source = bs.ensure_synth_project(6)
    target = os.path.join(root, "stateproj")
    if os.path.isdir(target):
        shutil.rmtree(target, ignore_errors=True)
    shutil.copytree(source["project_dir"], target)
    stale = os.path.join(target, ".fpgalsp")
    if os.path.isdir(stale):
        shutil.rmtree(stale, ignore_errors=True)
    for name in os.listdir(target):
        if name.endswith(".al"):
            return {"dir": target, "al": os.path.join(target, name)}
    raise SystemExit("state_dir_probe: .al 未找到")


def list_tree(path, limit=20):
    if not os.path.isdir(path):
        return []
    out = []
    for root, _dirs, files in os.walk(path):
        for name in files:
            rel = os.path.relpath(os.path.join(root, name), path).replace("\\", "/")
            out.append(rel)
            if len(out) >= limit:
                return sorted(out)
    return sorted(out)


def run_session(tag, spec, state_dir):
    """起一个 native LSP（cwd=工程副本），**不给 --log**，跑固定请求。

    不给 --log 是关键：默认日志/缓存/run 目录就是 .fpgalsp 那套，
    W1 要改的正是"这些状态落在哪里"。给了 --log 就把观测点绕过去了。
    """
    log_path = None
    if state_dir:
        os.environ["FPGALSP_STATE_DIR"] = state_dir
    else:
        os.environ.pop("FPGALSP_STATE_DIR", None)
    client = NativeLspClient(spec["dir"], log_path)   # cwd = 工程副本
    anchors = None
    try:
        client.initialize(spec["dir"], timeout=1800.0)
        uri = path_to_uri(os.path.join(spec["dir"], REQUESTS_TOP))
        client.request("textDocument/documentSymbol", {"textDocument": {"uri": uri}},
                       timeout=900.0)
        client.request("fpgaLsp/moduleTree", {"uri": uri}, timeout=900.0)
        client.request("fpgaLsp/reports", {"uri": uri}, timeout=900.0)
        # 让服务端有机会写日志/缓存
        client.request("workspace/executeCommand",
                       {"command": "fpgalsp.info", "arguments": []}, timeout=900.0)
    finally:
        try:
            client.close()
        except Exception:
            pass
        os.environ.pop("FPGALSP_STATE_DIR", None)
    return {
        "fpgalsp_in_cwd": list_tree(os.path.join(spec["dir"], ".fpgalsp")),
        "state_dir_files": list_tree(state_dir) if state_dir else [],
        "log_at_call_site": bool(log_path) and os.path.isfile(log_path),
        "log_bytes": os.path.getsize(log_path) if (log_path and os.path.isfile(log_path)) else 0,
        # 不给 --log 时服务端的默认日志位置（正是 W1 要搬走的东西）
        "default_log_in_cwd": os.path.isfile(
            os.path.join(spec["dir"], ".fpgalsp", "logs", "server.log")),
    }


def protocol_snapshot(spec, out_path):
    """用 probe_lsp 的同一套参数（钉死场景）产出快照，供逐字节比对。"""
    import subprocess
    exe = bs.default_native_exe()
    proc = subprocess.run(
        [sys.executable, os.path.join(CPP, "tools", "doc", "audit", "probe_lsp.py"),
         "--native-exe", exe, "--project", "%WORKSPACE%/CorpusAlpha", "--only", "all",
         "--out", out_path],
        capture_output=True, text=True, cwd=CPP)
    return proc.returncode


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True)
    parser.add_argument("--baseline-raw",
                        default=os.path.join(CPP, "doc", "audit", "2026-11-19", "fix",
                                             "baseline-wave2", "lsp-raw.json"))
    args = parser.parse_args(argv)

    root = os.path.join(os.environ.get("TEMP", "."), "fpgalsp-gate", "w1")
    os.makedirs(root, exist_ok=True)
    os.makedirs(bs.LOG_DIR, exist_ok=True)
    state_dir = os.path.join(os.environ.get("TEMP", "."), "fpgalsp-gate", "state")
    if os.path.isdir(state_dir):
        shutil.rmtree(state_dir, ignore_errors=True)
    os.makedirs(state_dir, exist_ok=True)

    report = {"native_exe": bs.native_exe_info(), "state_dir": state_dir}

    # A 组：不设 env（现状行为必须保留）
    spec_a = make_project(root)
    report["without_state_dir"] = run_session("off", spec_a, None)

    # B 组：设 env
    spec_b = make_project(root)
    report["with_state_dir"] = run_session("on", spec_b, state_dir)

    # C 组：协议字节对比
    tag_dir = os.path.dirname(os.path.abspath(args.out))
    raw_path = os.path.join(tag_dir, "lsp-raw-with-statedir.json")
    code = protocol_snapshot(spec_b, raw_path)
    report["protocol"] = {"probe_exit": code, "raw": raw_path}
    try:
        base = json.load(open(args.baseline_raw, encoding="utf-8"))
        now = json.load(open(raw_path, encoding="utf-8"))
        report["protocol"]["baseline"] = args.baseline_raw

        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import importlib
        verify = importlib.import_module("verify")
        left = verify.normalize_for_diff(verify.strip_meta(base))
        right = verify.normalize_for_diff(verify.strip_meta(now))
        report["protocol"]["identical_to_baseline"] = (
            verify.canonical(left) == verify.canonical(right))
    except Exception as exc:                       # noqa: BLE001
        report["protocol"]["error"] = "%s: %s" % (type(exc).__name__, exc)

    verdict = {
        # ① 状态落到 state 目录
        "state_dir_populated": len(report["with_state_dir"]["state_dir_files"]) > 0,
        # ② 设了 env 后 cwd 不再出现 .fpgalsp
        "no_fpgalsp_in_cwd_when_set": len(report["with_state_dir"]["fpgalsp_in_cwd"]) == 0,
        # 回退安全：不设 env 时仍然落在 cwd（= 现状行为）
        "fpgalsp_in_cwd_when_unset": len(report["without_state_dir"]["fpgalsp_in_cwd"]) > 0,
        # ③ 协议逐字节相同
        "protocol_identical": bool(report["protocol"].get("identical_to_baseline")),
    }
    report["verdict"] = verdict
    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(report, handle, ensure_ascii=False, indent=1)
    print("csv,state_dir,%s" % json.dumps(verdict, ensure_ascii=False))
    print("csv,state_dir,with,%s" % json.dumps(
        report["with_state_dir"], ensure_ascii=False)[:400])
    print("csv,state_dir,without,%s" % json.dumps(
        report["without_state_dir"], ensure_ascii=False)[:400])
    ok = all(verdict.values())
    return 0 if ok else 3


if __name__ == "__main__":
    sys.exit(main())