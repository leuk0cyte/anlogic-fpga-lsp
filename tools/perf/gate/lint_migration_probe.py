#!/usr/bin/env python3
"""lint_migration_probe.py —— G1/G3 判据：服务端内嵌 lint 的"独立性"与"并存性"。

G1（最重要）：**子进程 PATH 置空**后打开一个已知有 lint 问题的 `.v`，必须仍出 lint 诊断。
  - 语料：tests/gate-corpus/corpus_alpha/src/top.v（71 行含 tab；其中注释里的 tab 必须被掩码，
    预期只发布 **代码里的 3 条 no-tabs** + 1 条 module-filename = **4 条**；若看到 6 条 = 掩码失效）。
G3：同一文件同时有 lint 与 TD 诊断时，两者**并存**、互不覆盖（按 `source` 分组，两侧都非空）。

用法：
    python tools/perf/gate/lint_migration_probe.py --scenario g1 --out .../g1.json
    python tools/perf/gate/lint_migration_probe.py --scenario g3 --out .../g3.json
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import time

CPP = r"Y:/fpga-lsp-cpp"
sys.path.insert(0, os.path.join(CPP, "tools", "audit"))
import bench_servers as bs  # noqa: E402
from goldenlib.client import NativeLspClient, path_to_uri  # noqa: E402

CORPUS = os.path.join(CPP, "tests", "gate-corpus", "corpus_alpha")


class DiagClient(NativeLspClient):
    def __init__(self, *args, **kwargs):
        self.marks = []
        super().__init__(*args, **kwargs)

    def _dispatch(self, msg):
        if msg.get("method"):
            self.marks.append((msg["method"], time.perf_counter(),
                               (msg.get("params") or {}).get("uri"),
                               (msg.get("params") or {}).get("diagnostics") or []))
        super()._dispatch(msg)

    def diagnostics_for(self, uri, since=0):
        """该 uri 的最后一条 publishDiagnostics 的 diagnostics 列表（以及到达时间）。"""
        out = None
        at = None
        for method, stamp, mark_uri, diags in self.marks[since:]:
            if method == "textDocument/publishDiagnostics" and mark_uri == uri:
                out = diags
                at = stamp
        return out, at

    def wait_diagnostics(self, uri, timeout=20.0, predicate=None, since=0,
                         settle_ms=800.0):
        """等到"没有新的 publish 再出现"（settle），再返回最后一条**非空**诊断。

        不能拿到第一条空 publish 就返回：服务端先推一条空集合清场，lint 结果随后到达。
        """
        deadline = time.perf_counter() + timeout
        best = None
        best_at = None
        last_change = time.perf_counter()
        seen = 0
        while time.perf_counter() < deadline:
            marks = [m for m in self.marks[since:]
                     if m[0] == "textDocument/publishDiagnostics" and m[2] == uri]
            if len(marks) != seen:
                seen = len(marks)
                last_change = time.perf_counter()
                diags = marks[-1][3]
                if diags:
                    best, best_at = diags, marks[-1][1]
                if predicate is not None and predicate(diags):
                    return diags, marks[-1][1]
            elif best is not None and (time.perf_counter() - last_change) * 1000.0 >= settle_ms:
                return best, best_at
            time.sleep(0.05)
        return best, best_at


def copy_corpus(root):
    target = os.path.join(root, "corpus")
    if os.path.isdir(target):
        shutil.rmtree(target, ignore_errors=True)
    shutil.copytree(CORPUS, target, ignore=shutil.ignore_patterns(".fpgalsp", "*_Runs"))
    al = os.path.join(target, "demo_project.al")
    for name in os.listdir(target):
        if name.endswith(".al"):
            al = os.path.join(target, name)
            break
    return {"dir": target, "al": al, "top": os.path.join(target, "src", "top.v")}


def summarize_diags(diags):
    by_source = {}
    rules = []
    for item in diags or []:
        source = (item.get("source") or "<none>")
        by_source[source] = by_source.get(source, 0) + 1
        rules.append({"source": source, "code": item.get("code"),
                      "message": (item.get("message") or "")[:120],
                      "line": ((item.get("range") or {}).get("start") or {}).get("line")})
    return {"total": len(diags or []), "by_source": by_source, "items": rules}


def scenario_g1(spec, args):
    """PATH 置空 + 打开 top.v → 必须出 lint 诊断（且注释里的 tab 被掩码）。"""
    report = {"scenario": "g1", "corpus_copy": spec["dir"]}
    saved_path = os.environ.get("PATH", "")
    state_dir = os.path.join(os.environ.get("TEMP", "."), "fpgalsp-gate", "g1-state")
    os.makedirs(state_dir, exist_ok=True)
    os.environ["FPGALSP_STATE_DIR"] = state_dir
    try:
        os.environ["PATH"] = ""            # 关键：子进程 PATH 置空
        client = DiagClient(spec["dir"], os.path.join(bs.LOG_DIR, "g1.log"))
    finally:
        os.environ["PATH"] = saved_path
        os.environ.pop("FPGALSP_STATE_DIR", None)
    try:
        client.initialize(spec["dir"], timeout=1800.0)
        uri = path_to_uri(spec["top"])
        client.open_document(spec["top"], "verilog")
        diags, at = client.wait_diagnostics(uri, timeout=25.0)
        summary = summarize_diags(diags)
        report.update(summary)
        report["lint_like"] = [item for item in summary["items"]
                               if item["source"] not in ("td", "TangDynasty", "<none>")]
        report["no_tabs_count"] = sum(1 for item in summary["items"]
                                      if "no-tabs" in str(item.get("code") or ""))
        report["module_filename_count"] = sum(
            1 for item in summary["items"] if "module-filename" in str(item.get("code") or ""))
        report["path_len_at_spawn"] = 0
    except Exception:                                  # noqa: BLE001
        import traceback
        report["fatal"] = traceback.format_exc()
    finally:
        try:
            client.close()
        except Exception:
            pass
    return report


def scenario_g3(spec, args):
    """lint + TD 并存：保存后两侧都非空，且互不覆盖。"""
    report = {"scenario": "g3", "corpus_copy": spec["dir"]}
    state_dir = os.path.join(os.environ.get("TEMP", "."), "fpgalsp-gate", "g3-state")
    os.makedirs(state_dir, exist_ok=True)
    os.environ["FPGALSP_STATE_DIR"] = state_dir
    client = DiagClient(spec["dir"], os.path.join(bs.LOG_DIR, "g3.log"))
    try:
        # 自己拼 initialize：打开 TD 检查（tdCheckOnSave）
        client.request("initialize", {
            "processId": os.getpid(),
            "rootUri": path_to_uri(spec["dir"]),
            "clientInfo": {"name": "lint-g3-probe", "version": "1"},
            "capabilities": {"workspace": {"didChangeWatchedFiles": {}}},
            "initializationOptions": {"fpgaLsp": {"tdCheckOnSave": True, "enableTd": True}},
        }, timeout=1800.0)
        client.notify("initialized", {})
        uri = path_to_uri(spec["top"])
        client.open_document(spec["top"], "verilog")
        # 先等 lint（快），再保存触发 TD，等两侧都有
        first, _ = client.wait_diagnostics(uri, timeout=20.0)
        report["after_open"] = summarize_diags(first)
        client.notify("textDocument/didSave", {"textDocument": {"uri": uri}})
        deadline = time.perf_counter() + 60.0
        merged = None
        while time.perf_counter() < deadline:
            diags, _at = client.diagnostics_for(uri)
            summary = summarize_diags(diags)
            sources = summary["by_source"]
            has_lint = any("verible" in str(name).lower() for name in sources)
            has_td = any("td" in str(name).lower() for name in sources)
            if has_lint and has_td:
                merged = summary
                break
            time.sleep(0.1)
        report["after_save"] = merged or summarize_diags(client.diagnostics_for(uri)[0])
        sources = (report["after_save"] or {}).get("by_source") or {}
        # 服务端的 source 实际叫 "fpga-lsp:verible" / "fpga-lsp:td"，用子串匹配
        has_lint = any("verible" in str(name).lower() and count > 0
                       for name, count in sources.items())
        has_td = any("td" in str(name).lower() and count > 0
                     for name, count in sources.items())
        report["has_lint"] = has_lint
        report["has_td"] = has_td
        report["both_sources_nonempty"] = has_lint and has_td
    except Exception:                                  # noqa: BLE001
        import traceback
        report["fatal"] = traceback.format_exc()
    finally:
        os.environ.pop("FPGALSP_STATE_DIR", None)
        try:
            client.close()
        except Exception:
            pass
    return report


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--scenario", choices=["g1", "g3"], required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args(argv)
    root = os.path.join(os.environ.get("TEMP", "."), "fpgalsp-gate", "lint")
    os.makedirs(root, exist_ok=True)
    os.makedirs(bs.LOG_DIR, exist_ok=True)
    spec = copy_corpus(root)
    report = {"native_exe": bs.native_exe_info()}
    if args.scenario == "g1":
        report["g1"] = scenario_g1(spec, args)
    else:
        report["g3"] = scenario_g3(spec, args)
    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(report, handle, ensure_ascii=False, indent=1)
    payload = report["g1"] if args.scenario == "g1" else report["g3"]
    print("csv,lint_%s,%s" % (args.scenario, json.dumps(
        {k: v for k, v in payload.items() if k != "items"}, ensure_ascii=False)[:600]))
    return 0


if __name__ == "__main__":
    sys.exit(main())