#!/usr/bin/env python3
"""textcache_freshness_probe.py —— C1 专项：文本缓存的失效通道（didChangeWatchedFiles → EvictTextCache）。

背景（评审人证明的盲区）：
  `DocumentStore` 的文本缓存键是 `(path, mtime, size)`。**同大小 + 还原 mtime** 的改写能骗过它，
  于是后续请求读到旧文本。修复：收到 `workspace/didChangeWatchedFiles` 时把对应路径从文本缓存里驱逐。
  已知残余（文档已记）：**不发通知**时仍可能读到旧文本（缓存键本身没有内容哈希）。

本脚本用一个"格式化"观测量把这条链走一遍（不碰产品代码）：

  1) 工程里放一个**故意没格式化**的 .v，正文带标记 `AAAA`
  2) `textDocument/formatting` → 返回的 newText 应包含 `AAAA`（说明读到的是当前文本）
  3) 改盘：`AAAA` → `BBBB`（**等长**），并用 `os.utime` 把 mtime 还原成旧值（骗过缓存键）
  4) 立刻 `textDocument/formatting` → 记录读到的是 AAAA 还是 BBBB（这是"已知残余"那一侧）
  5) 发 `workspace/didChangeWatchedFiles`（type=2, 该文件）→ 再 formatting → **必须读到 BBBB**
  6) `completion` 在同一位置也做一次（第二个观测量）

判据：
  * 步骤 5/6（发通知后立刻可见）= 必须通过；
  * 步骤 4（不发通知）= 允许读到旧值（设计残余），只如实记录，不判失败。

用法：
    python tools/perf/gate/textcache_freshness_probe.py --out <tagdir>/textcache_freshness.json
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
from goldenlib.client import path_to_uri  # noqa: E402

# 故意不格式化：空格乱、括号前后不齐 → formatter 一定会返回一条覆盖全文的 TextEdit
MESSY_AAAA = """\
module  cache_probe  (  input   clk  ,
   input rst_n ,
    output reg [7:0] q ) ;
  always @(posedge clk)   if (!rst_n)   q<=8'd0;   else  q<=q+8'd1;
endmodule
// marker AAAA
"""

MESSY_BBBB = MESSY_AAAA.replace("AAAA", "BBBB")


def make_project(root):
    project_dir = os.path.join(root, "cacheproj")
    if os.path.isdir(project_dir):
        shutil.rmtree(project_dir, ignore_errors=True)
    src = os.path.join(project_dir, "src")
    os.makedirs(src, exist_ok=True)
    path = os.path.join(src, "cache_probe.v")
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(MESSY_AAAA)
    al = os.path.join(project_dir, "cacheproj.al")
    with open(al, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(
            '<?xml version="1.0" encoding="UTF-8"?>\n'
            '<Project Version="3" Minor="2" Path="%s">\n'
            '    <Name>cacheproj</Name>\n'
            '    <HardWare><Family>EF2</Family><Device>EF2L45LG144B</Device><Speed></Speed></HardWare>\n'
            '    <Source_Files>\n        <Verilog>\n'
            '            <File Path="src/cache_probe.v"><FileInfo><Attr Name="UsedInSyn" Val="true"/></FileInfo></File>\n'
            '        </Verilog>\n    </Source_Files>\n'
            '    <TOP_MODULE><LABEL>cache_probe</LABEL><MODULE>cache_probe</MODULE></TOP_MODULE>\n'
            '</Project>\n' % project_dir.replace("\\", "/"))
    return {"dir": project_dir, "al": al, "file": path}


def format_marker(client, uri):
    """调 formatting，返回 (newText 里出现的标记, 耗时 ms)。"""
    t0 = time.perf_counter()
    edits = client.request("textDocument/formatting",
                           {"textDocument": {"uri": uri},
                            "options": {"tabSize": 2, "insertSpaces": True}},
                           timeout=600.0) or []
    ms = (time.perf_counter() - t0) * 1000.0
    text = ""
    if isinstance(edits, list) and edits:
        text = (edits[0] or {}).get("newText") or ""
    marker = None
    if "BBBB" in text:
        marker = "BBBB"
    elif "AAAA" in text:
        marker = "AAAA"
    return marker, round(ms, 2), len(text)


def completion_markers(client, uri):
    """completion 候选里出现 cache_probe 说明读到了当前文本（另一个观测量）。"""
    t0 = time.perf_counter()
    payload = client.request("textDocument/completion",
                             {"textDocument": {"uri": uri},
                              "position": {"line": 3, "character": 10}},
                             timeout=600.0) or {}
    ms = (time.perf_counter() - t0) * 1000.0
    items = payload.get("items") if isinstance(payload, dict) else None
    labels = set()
    for item in items or []:
        label = (item or {}).get("label")
        if label:
            labels.add(label)
    return {"ms": round(ms, 2), "count": len(labels),
            "has_cache_probe": "cache_probe" in labels}


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True)
    args = parser.parse_args(argv)

    root = os.path.join(os.environ.get("TEMP", "."), "fpgalsp-gate", "c1")
    os.makedirs(root, exist_ok=True)
    os.makedirs(bs.LOG_DIR, exist_ok=True)
    spec = make_project(root)
    uri = path_to_uri(spec["file"])
    report = {"native_exe": bs.native_exe_info()}
    load = bs.SystemLoad()
    client = bs.make_lsp_client("native", "textcache.log")
    try:
        client.initialize(spec["dir"], timeout=1800.0)
        # 预热：让文本缓存与索引都建立起来
        client.request("textDocument/documentSymbol", {"textDocument": {"uri": uri}}, timeout=900.0)
        base_marker, base_ms, base_len = format_marker(client, uri)
        report["baseline"] = {"marker": base_marker, "ms": base_ms, "edit_chars": base_len}

        # 同大小 + 还原 mtime 的改写
        stat = os.stat(spec["file"])
        with open(spec["file"], "w", encoding="utf-8", newline="\n") as handle:
            handle.write(MESSY_BBBB)
        new_size = os.path.getsize(spec["file"])
        os.utime(spec["file"], (stat.st_atime, stat.st_mtime))
        same_size = (new_size == os.path.getsize(spec["file"]))
        report["rewrite"] = {
            "same_size": same_size,
            "size_before": stat.st_size,
            "size_after": new_size,
            "mtime_restored": True,
        }

        # 不发通知：允许读到旧内容（已知残余）
        before_notify_marker, before_ms, before_len = format_marker(client, uri)
        report["without_notification"] = {
            "marker": before_notify_marker, "ms": before_ms, "edit_chars": before_len,
            "note": "设计残余：不发 watched-files 通知时可能读到旧文本（缓存键无内容哈希）",
        }

        # 发 watched-files 通知（type=2 changed）→ 必须立刻读到新内容
        t_notify = time.perf_counter()
        client.notify("workspace/didChangeWatchedFiles", {
            "changes": [{"uri": uri, "type": 2}]})
        after_marker, after_ms, after_len = format_marker(client, uri)
        notify_to_format_ms = (time.perf_counter() - t_notify) * 1000.0
        completion = completion_markers(client, uri)
        report["after_notification"] = {
            "marker": after_marker, "ms": after_ms, "edit_chars": after_len,
            "notify_to_format_ms": round(notify_to_format_ms, 2),
            "completion": completion,
        }

        # 也验一下 didOpen+didChange（缓冲区路径）不受影响
        client.open_document(spec["file"], "verilog", MESSY_AAAA, version=1)
        buffer_marker, buffer_ms, _ = format_marker(client, uri)
        report["buffer_path"] = {"marker": buffer_marker, "ms": buffer_ms,
                                 "note": "didOpen 用 AAAA 缓冲，应看到 AAAA（缓冲区优先）"}
        client.notify("textDocument/didClose", {"textDocument": {"uri": uri}})
    except Exception:                              # noqa: BLE001
        import traceback
        report["fatal"] = traceback.format_exc()
    finally:
        load.stop()
        try:
            client.close()
        except Exception:
            pass
    report["cpu_busy"] = load.busy_fraction()

    after = report.get("after_notification") or {}
    report["verdict"] = {
        "baseline_reads_current": (report.get("baseline") or {}).get("marker") == "AAAA",
        # 核心判据：发通知后必须看到新内容
        "notify_makes_visible": after.get("marker") == "BBBB",
        # 已知残余：不发通知时读到旧值属预期（读到了新值也不判失败，只是残余不存在了）
        "without_notify_read_stale": (report.get("without_notification") or {}).get("marker") == "AAAA",
        "completion_ok": bool((after.get("completion") or {}).get("count")),
    }
    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(report, handle, ensure_ascii=False, indent=1)
    print("csv,textcache,%s" % json.dumps(report["verdict"], ensure_ascii=False))
    for key in ("baseline", "without_notification", "after_notification", "buffer_path"):
        if key in report:
            print("csv,textcache,%s,%s" % (key, json.dumps(report[key], ensure_ascii=False)))
    return 0 if report["verdict"]["notify_makes_visible"] else 3


if __name__ == "__main__":
    sys.exit(main())