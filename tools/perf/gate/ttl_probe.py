#!/usr/bin/env python3
"""ttl_probe.py —— F2 专项：验证 IndexService 的 TTL 副作用是否符合设计。

F2 把"每请求一次目录 stamp 校验"换成 TTL（窗口 250 ms）。这带来两个必须钉死的语义：

  A) **窗口内不重扫（设计如此）**：改磁盘上的文件后，250 ms 内的第一次 Get()
     应当**看不到**变化；等过窗口后的第一次 Get() 必须看到。
     → 看不到是 expected；永远看不到才是 bug。

  B) **didSave 之后必须立刻可见（force 路径）**：didSave 是"用户明确落盘"，
     语义上不允许被 TTL 挡住。如果 didSave 之后第一次请求仍返回旧内容 → **真回归**。

本脚本用真实 LSP 协议验证这两条（不碰产品代码）：

  1) 打开工程 → 记住 documentSymbol 的符号名集合（baseline）
  2) 直接改磁盘上的 .v（新增一个 module 名可辨识的符号），**立刻**发 documentSymbol
     → 记录"是否看到新符号"（窗口内应 = 否）
  3) sleep 350 ms 后再发 → 应 = 是（TTL 过期后的重扫生效）
  4) 再改一次磁盘，然后 `didOpen + didChange + didSave`（模拟编辑器保存），**立刻**发
     → 记录"是否看到新符号"（必须 = 是）
  5) 重复 3 轮，报每步的延迟与判定

用法：
    python tools/perf/gate/ttl_probe.py --rounds 3 --out <tagdir>/ttl.json
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


def symbols_of(client, uri):
    result = client.request("textDocument/documentSymbol", {"textDocument": {"uri": uri}},
                            timeout=900.0) or []
    names = set()
    for item in result:
        if isinstance(item, dict):
            name = item.get("name")
            if name:
                names.add(name)
            for child in item.get("children") or []:
                if isinstance(child, dict) and child.get("name"):
                    names.add(child["name"])
    return names, result


MODULE_TEMPLATE = """\
// ttl probe injected module
module ttl_probe_%d (
  input  clk,
  input  rst_n,
  output reg [7:0] q
);
  always @(posedge clk) if (!rst_n) q <= 8'd0; else q <= q + 8'd1;
endmodule
"""


def make_project(root):
    """独立的小工程（避免污染其它 tag 的合成工程）。"""
    project_dir = os.path.join(root, "ttlproj")
    src = os.path.join(project_dir, "src")
    os.makedirs(src, exist_ok=True)
    files = []
    for index in range(6):
        name = "ttl_base_%d" % index
        path = os.path.join(src, name + ".v")
        text = ("module %s(input clk, input rst_n, output reg [7:0] q);\n"
                "  always @(posedge clk) if (!rst_n) q <= 8'd0; else q <= q + 8'd1;\n"
                "endmodule\n" % name)
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
        files.append((path, name))
    al = os.path.join(project_dir, "ttlproj.al")
    head = ('<?xml version="1.0" encoding="UTF-8"?>\n'
            '<Project Version="3" Minor="2" Path="%s">\n'
            '    <Name>ttlproj</Name>\n'
            '    <HardWare><Family>EF2</Family><Device>EF2L45LG144B</Device><Speed></Speed></HardWare>\n'
            '    <Source_Files>\n        <Verilog>\n' % project_dir.replace("\\", "/"))
    body = "".join(
        '            <File Path="src/%s.v">\n'
        '                <FileInfo>\n'
        '                    <Attr Name="UsedInSyn" Val="true"/>\n'
        '                    <Attr Name="UsedInP&amp;R" Val="true"/>\n'
        '                </FileInfo>\n'
        '            </File>\n' % name for _path, name in files)
    tail = ('        </Verilog>\n    </Source_Files>\n'
            '    <TOP_MODULE><LABEL>ttl_base_0</LABEL><MODULE>ttl_base_0</MODULE></TOP_MODULE>\n'
            '</Project>\n')
    with open(al, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(head + body + tail)
    for path, _name in files:
        with open(path, "rb") as handle:      # 预热 page cache
            handle.read()
    return {"dir": project_dir, "al": al, "files": files}


def inject(path, marker):
    with open(path, "a", encoding="utf-8", newline="\n") as handle:
        handle.write("\n" + MODULE_TEMPLATE % marker)


def run_round(client, spec, marker, round_no):
    target_path, target_name = spec["files"][0]
    uri = bs.path_to_uri(target_path)
    row = {"round": round_no}

    # 0) 基线符号集合（确保旧内容已在索引里）
    base_names, _ = symbols_of(client, uri)
    row["baseline_has_injected"] = any("ttl_probe" in name for name in base_names)

    # 1) 直接改盘 + 立刻请求（TTL 窗口内 → 设计上看不到）
    inject(target_path, marker)
    t0 = time.perf_counter()
    names_immediate, _ = symbols_of(client, uri)
    row["disk_change_immediate_ms"] = round((time.perf_counter() - t0) * 1000.0, 2)
    row["disk_change_immediate_seen"] = any("ttl_probe" in name for name in names_immediate)

    # 2) 等过 TTL 窗口再请求（应看到）
    time.sleep(0.40)
    t0 = time.perf_counter()
    names_after, _ = symbols_of(client, uri)
    row["after_ttl_window_ms"] = round((time.perf_counter() - t0) * 1000.0, 2)
    row["after_ttl_window_seen"] = any("ttl_probe" in name for name in names_after)

    # 3) didOpen + didChange + didSave 后再改盘，didSave 必须立刻可见（force 路径）
    text_path = target_path
    with open(text_path, encoding="utf-8") as handle:
        opened = handle.read()
    client.open_document(text_path, "verilog", opened, version=1)
    marker2 = marker + 100
    edited = opened + "\n" + (MODULE_TEMPLATE % marker2)
    client.notify("textDocument/didChange", {
        "textDocument": {"uri": uri, "version": 2},
        "contentChanges": [{"text": edited}]})
    t_didchange = time.perf_counter()
    names_after_change, _ = symbols_of(client, uri)
    row["didchange_to_symbol_ms"] = round((time.perf_counter() - t_didchange) * 1000.0, 2)
    row["didchange_seen"] = any("ttl_probe_%d" % marker2 in name for name in names_after_change)

    t_did_save = time.perf_counter()
    client.notify("textDocument/didSave", {"textDocument": {"uri": uri}, "text": edited})
    # didSave 之后立刻请求：force 路径必须让新内容可见（缓冲区已是最新）
    names_after_save, _ = symbols_of(client, uri)
    row["didsave_to_symbol_ms"] = round((time.perf_counter() - t_did_save) * 1000.0, 2)
    row["didsave_seen"] = any("ttl_probe_%d" % marker2 in name for name in names_after_save)

    # 4) 关键：**关闭文档**之后改盘，验证 TTL 语义（这才是"磁盘变化何时可见"的正确场景）
    #    文档还开着时缓冲区优先于磁盘，此时"看不到磁盘改动"是正确语义，不是回归。
    client.notify("textDocument/didClose", {"textDocument": {"uri": uri}})
    time.sleep(0.05)
    marker3 = marker + 200
    inject(text_path, marker3)                      # 磁盘变了，且没有缓冲区
    t_immediate = time.perf_counter()
    names_closed_immediate, _ = symbols_of(client, uri)
    row["closed_disk_change_immediate_ms"] = round(
        (time.perf_counter() - t_immediate) * 1000.0, 2)
    row["closed_disk_change_immediate_seen"] = any(
        "ttl_probe_%d" % marker3 in name for name in names_closed_immediate)
    time.sleep(0.40)                                # 过 TTL 窗口
    names_closed_after, _ = symbols_of(client, uri)
    row["closed_disk_change_after_window_seen"] = any(
        "ttl_probe_%d" % marker3 in name for name in names_closed_after)

    # 5) didSave 的 force 路径：文档重新打开 → didChange 改缓冲区 → didSave → 必须立刻可见
    client.open_document(text_path, "verilog",
                         open(text_path, encoding="utf-8").read(), version=10)
    marker4 = marker + 300
    text_now = open(text_path, encoding="utf-8").read()
    edited2 = text_now + "\n" + (MODULE_TEMPLATE % marker4)
    client.notify("textDocument/didChange", {
        "textDocument": {"uri": uri, "version": 11},
        "contentChanges": [{"text": edited2}]})
    t_save = time.perf_counter()
    client.notify("textDocument/didSave", {"textDocument": {"uri": uri}, "text": edited2})
    names_after_save2, _ = symbols_of(client, uri)
    row["didsave_force_to_symbol_ms"] = round((time.perf_counter() - t_save) * 1000.0, 2)
    row["didsave_force_seen"] = any("ttl_probe_%d" % marker4 in name
                                    for name in names_after_save2)
    return row


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--out", required=True)
    args = parser.parse_args(argv)

    root = os.path.join(os.environ.get("TEMP", "."), "fpgalsp-gate", "ttl")
    os.makedirs(root, exist_ok=True)
    spec = make_project(root)
    os.makedirs(bs.LOG_DIR, exist_ok=True)
    report = {"native_exe": bs.native_exe_info(), "rounds": []}
    load = bs.SystemLoad()
    for round_no in range(1, args.rounds + 1):
        client = bs.make_lsp_client("native", "ttl-r%d.log" % round_no)
        try:
            client.initialize(spec["dir"], timeout=1800.0)
            client.request("textDocument/documentSymbol",
                           {"textDocument": {"uri": bs.path_to_uri(spec["files"][0][0])}},
                           timeout=1800.0)
            row = run_round(client, spec, round_no * 1000, round_no)
        except Exception as exc:
            row = {"round": round_no, "fatal": "%s: %s" % (type(exc).__name__, exc)}
        finally:
            try:
                client.close()
            except Exception:
                pass
        report["rounds"].append(row)
    load.stop()
    report["cpu_busy"] = load.busy_fraction()

    ok_rounds = [row for row in report["rounds"] if "fatal" not in row]
    report["verdict"] = {
        # A) 窗口内看不到磁盘改动 = 设计（看到也不算错：TTL 可能刚好过期）
        "ttl_window_hides_disk_change_rounds": sum(
            1 for row in ok_rounds if not row.get("disk_change_immediate_seen")),
        # 过窗口必须看到
        "ttl_scan_after_window_ok": all(row.get("after_ttl_window_seen") for row in ok_rounds
                                        if "after_ttl_window_seen" in row),
        # 缓冲区路径（didChange）必须立刻可见 —— 不受 TTL 影响
        "didchange_immediate_ok": all(row.get("didchange_seen") for row in ok_rounds
                                      if "didchange_seen" in row),
        # didSave（带 text）必须立刻可见
        "didsave_visible_ok": all(row.get("didsave_seen") for row in ok_rounds
                                  if "didsave_seen" in row),
        # 文档关闭后改盘：窗口内可看不到（设计），过窗口必须看到
        "closed_doc_disk_after_window_ok": all(
            row.get("closed_disk_change_after_window_seen") for row in ok_rounds
            if "closed_disk_change_after_window_seen" in row),
        # didSave 的 force 路径（didChange 缓冲区 + didSave）必须立刻可见 —— 失灵就是真回归
        "didsave_force_visible_ok": all(row.get("didsave_force_seen") for row in ok_rounds
                                        if "didsave_force_seen" in row),
        "rounds": len(ok_rounds),
    }
    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(report, handle, ensure_ascii=False, indent=1)
    print("csv,ttl,%s" % json.dumps(report["verdict"], ensure_ascii=False))
    for row in report["rounds"]:
        print("csv,ttl_round,%s" % json.dumps(
            {k: v for k, v in row.items() if k != "round"}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())