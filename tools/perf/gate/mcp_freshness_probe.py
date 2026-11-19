#!/usr/bin/env python3
"""mcp_freshness_probe.py —— D5 专项：MCP 长连接下"改文件后立刻看到新内容"。

背景：D5 之前，MCP 的 HDL 索引只在 `.al` 变化时重建，长连接里改 `.v` **永远看不到新内容**。
D5 之后应改为走 IndexService（250 ms TTL 内可见）。

做法（同一条连接，全程不重连）：
  1) 起一个 native MCP 会话（stdio），root 指向 %TEMP% 的合成工程副本
  2) 调 `fpga_project_structure` / `fpga_hover` 拿基线快照（模块名、portCount、modulesSeen）
  3) 改盘：把一个 `.v` 的模块端口数 +1、并重命名一个子模块（只动磁盘，不发任何通知）
  4) **同一连接**立刻再调一次，最多轮询到 `--wait-ms`（默认 600 ms），记录"多久可见"
  5) 给出改前/改后关键字段的 diff

判据：
  * `visible_after_ms` 在 TTL 窗口（250 ms）附近或更小 → 通过
  * 永远看不到（超时仍旧值）→ 失败，说明索引没跟着磁盘走

用法：
    python tools/perf/gate/mcp_freshness_probe.py --out <tagdir>/mcp_freshness.json
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
from goldenlib.client import NativeMcpClient  # noqa: E402

MODULE_BEFORE = """\
module d5_child (
  input  clk,
  input  rst_n,
  output reg [7:0] q
);
  always @(posedge clk) if (!rst_n) q <= 8'd0; else q <= q + 8'd1;
endmodule
"""

# 改后：端口 +1（portCount 3 → 4），模块名也换掉（appears_as 变化）
MODULE_AFTER = """\
module d5_child_renamed (
  input  clk,
  input  rst_n,
  input  en,
  output reg [7:0] q
);
  always @(posedge clk) if (!rst_n) q <= 8'd0; else if (en) q <= q + 8'd1;
endmodule
"""

TOP = """\
module d5_top (
  input  clk,
  input  rst_n,
  output [7:0] q
);
  d5_child u_child (.clk(clk), .rst_n(rst_n), .q(q));
endmodule
"""


TOP_AFTER = """\
module d5_top (
  input  clk,
  input  rst_n,
  input  en,
  output [7:0] q
);
  d5_child_renamed u_child (.clk(clk), .rst_n(rst_n), .en(en), .q(q));
endmodule
"""


def make_project(root):
    project_dir = os.path.join(root, "d5proj")
    if os.path.isdir(project_dir):
        shutil.rmtree(project_dir, ignore_errors=True)
    src = os.path.join(project_dir, "src")
    os.makedirs(src, exist_ok=True)
    child = os.path.join(src, "d5_child.v")
    top = os.path.join(src, "d5_top.v")
    with open(child, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(MODULE_BEFORE)
    with open(top, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(TOP)
    al = os.path.join(project_dir, "d5proj.al")
    with open(al, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(
            '<?xml version="1.0" encoding="UTF-8"?>\n'
            '<Project Version="3" Minor="2" Path="%s">\n'
            '    <Name>d5proj</Name>\n'
            '    <HardWare><Family>EF2</Family><Device>EF2L45LG144B</Device><Speed></Speed></HardWare>\n'
            '    <Source_Files>\n        <Verilog>\n'
            '            <File Path="src/d5_top.v"><FileInfo><Attr Name="UsedInSyn" Val="true"/></FileInfo></File>\n'
            '            <File Path="src/d5_child.v"><FileInfo><Attr Name="UsedInSyn" Val="true"/></FileInfo></File>\n'
            '        </Verilog>\n    </Source_Files>\n'
            '    <TOP_MODULE><LABEL>d5_top</LABEL><MODULE>d5_top</MODULE></TOP_MODULE>\n'
            '</Project>\n' % project_dir.replace("\\", "/"))
    return {"dir": project_dir, "al": al, "child": child, "top": top}


def structure_summary(client):
    """把 fpga_project_structure 压成可比较的关键字段。"""
    try:
        payload, _is_error = client.call_tool("fpga_project_structure",
                                              {"include_ports": True}, timeout=600.0)
    except Exception as exc:                       # noqa: BLE001
        return {"error": "%s: %s" % (type(exc).__name__, exc)}
    if not isinstance(payload, dict):
        return {"error": "non-dict payload"}

    def walk(node, out):
        if not isinstance(node, dict):
            return
        name = node.get("module") or ""
        out.setdefault("modules", []).append(name)
        out["ports"][name] = node.get("portCount")
        for child in node.get("children") or []:
            walk(child, out)

    summary = {"modules": [], "ports": {}, "modulesSeen": payload.get("modulesSeen")}
    walk(payload.get("root") or {}, summary)
    summary["modules"] = sorted(set(summary["modules"]))
    summary["renamed_present"] = "d5_child_renamed" in summary["modules"]
    summary["child_portCount"] = summary["ports"].get("d5_child_renamed",
                                                      summary["ports"].get("d5_child"))
    return summary


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--wait-ms", type=int, default=600)
    parser.add_argument("--out", required=True)
    args = parser.parse_args(argv)

    root = os.path.join(os.environ.get("TEMP", "."), "fpgalsp-gate", "d5")
    os.makedirs(root, exist_ok=True)
    os.makedirs(bs.LOG_DIR, exist_ok=True)
    spec = make_project(root)
    report = {"native_exe": bs.native_exe_info(), "wait_ms": args.wait_ms, "rounds": []}
    load = bs.SystemLoad()
    for round_no in range(1, args.rounds + 1):
        # 每轮重置文件内容，保证起点一致
        with open(spec["child"], "w", encoding="utf-8", newline="\n") as handle:
            handle.write(MODULE_BEFORE)
        with open(spec["top"], "w", encoding="utf-8", newline="\n") as handle:
            handle.write(TOP)
        client = NativeMcpClient(bs.REPO_PY, spec["dir"],
                                 os.path.join(bs.LOG_DIR, "d5-r%d.log" % round_no))
        try:
            before = structure_summary(client)
            # 只改磁盘，不发任何通知（模拟编辑器外部保存 / 另一个工具写盘）
            t_write = time.perf_counter()
            with open(spec["child"], "w", encoding="utf-8", newline="\n") as handle:
                handle.write(MODULE_AFTER)
            # 关键：例化也要同步改名，否则"定义改名但顶层还引用旧名"会变成合法的 blackbox，
            # 结构里仍出现旧名 —— 那是正确行为，不是陈旧索引。
            with open(spec["top"], "w", encoding="utf-8", newline="\n") as handle:
                handle.write(TOP_AFTER)
            first = structure_summary(client)
            first_ms = (time.perf_counter() - t_write) * 1000.0
            visible_after = None
            polls = 0
            after = first
            deadline = time.perf_counter() + args.wait_ms / 1000.0
            if after.get("renamed_present") and after.get("child_portCount") == 4:
                visible_after = first_ms
            else:
                while time.perf_counter() < deadline:
                    time.sleep(0.05)
                    polls += 1
                    after = structure_summary(client)
                    if after.get("renamed_present") and after.get("child_portCount") == 4:
                        visible_after = (time.perf_counter() - t_write) * 1000.0
                        break
            diff = {
                "modules_before": before.get("modules"),
                "modules_after": after.get("modules"),
                "modulesSeen_before": before.get("modulesSeen"),
                "modulesSeen_after": after.get("modulesSeen"),
                "child_portCount_before": before.get("child_portCount"),
                "child_portCount_after": after.get("child_portCount"),
            }
            report["rounds"].append({
                "round": round_no,
                "first_call_ms": round(first_ms, 1),
                "first_call_visible": bool(first.get("renamed_present")),
                "visible_after_ms": None if visible_after is None else round(visible_after, 1),
                "polls": polls,
                "diff": diff,
                "pass": (visible_after is not None and visible_after <= args.wait_ms
                         and after.get("child_portCount") == 4),
            })
            print("csv,mcp_freshness,r%d,%s" % (round_no, json.dumps(
                report["rounds"][-1], ensure_ascii=False)), flush=True)
        except Exception:                          # noqa: BLE001
            import traceback
            report["rounds"].append({"round": round_no, "fatal": traceback.format_exc()})
        finally:
            try:
                client.close()
            except Exception:
                pass
    load.stop()
    report["cpu_busy"] = load.busy_fraction()
    ok = [row for row in report["rounds"] if row.get("pass")]
    report["verdict"] = {
        "rounds": len(report["rounds"]),
        "pass": len(ok),
        "median_visible_ms": (sorted(row["visible_after_ms"] for row in ok)[len(ok) // 2]
                              if ok else None),
    }
    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(report, handle, ensure_ascii=False, indent=1)
    print("csv,mcp_freshness,verdict,%s" % json.dumps(report["verdict"], ensure_ascii=False))
    return 0 if report["verdict"]["pass"] == report["verdict"]["rounds"] else 3


if __name__ == "__main__":
    sys.exit(main())