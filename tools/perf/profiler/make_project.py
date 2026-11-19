#!/usr/bin/env python3
"""生成 perf-profiler 用的合成工程（200 文件 + 10 MB timing 报告）并预热 page cache。

不碰真实工程：所有产物落在 `%TEMP%\\fpgalsp-perf\\profiler\\proj<size>`。

用法:
    python tools/perf/profiler/make_project.py --files 200 --out %TEMP%\\fpgalsp-perf\\profiler
输出:
    <out>/proj200/            工程根（.al + rtl/*.v）
    <out>/proj200/proj200_Runs/phy_1/final_timing.rpt   (≈10 MB，供场景 d)
    <out>/proj200/meta.json   供 driver 读的路径与规模
"""

from __future__ import annotations

import argparse
import json
import os
import sys

AL_TEMPLATE = """<?xml version="1.0" encoding="utf-8"?>
<Project Version="2" Path="{path}">
  <TD_Version>2026.2</TD_Version>
  <Name>{name}</Name>
  <HardWare>
    <Family>EF2</Family>
    <Device>EF2L45LG144B</Device>
    <Speed>-2</Speed>
  </HardWare>
  <TOP_MODULE><MODULE>{top}</MODULE></TOP_MODULE>
  <Source_Files>
    <Verilog>
{files}    </Verilog>
  </Source_Files>
</Project>
"""

VERILOG_TEMPLATE = """// synthetic module for perf profiling (file {index})
module mod_{index:03d} (
    input  wire        clk,
    input  wire        rst_n,
    input  wire [15:0] din_{index:03d},
    output reg  [15:0] dout_{index:03d}
);
    reg [15:0] acc_{index:03d};
    reg [7:0]  cnt_{index:03d};
    wire       flag_{index:03d};

    assign flag_{index:03d} = (din_{index:03d}[3:0] == 4'hA);

    // 一段有点规模的组合逻辑，让索引真的有事可做
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            acc_{index:03d} <= 16'h0;
            cnt_{index:03d} <= 8'h0;
        end else if (flag_{index:03d}) begin
            acc_{index:03d} <= acc_{index:03d} + din_{index:03d};
            cnt_{index:03d} <= cnt_{index:03d} + 8'h1;
        end else begin
            acc_{index:03d} <= acc_{index:03d} ^ {{din_{index:03d}[7:0], din_{index:03d}[15:8]}};
        end
    end

    always @(posedge clk) begin
        dout_{index:03d} <= acc_{index:03d};
    end
endmodule
"""


def timing_block(i: int) -> str:
    """一条 TD 风格的 timing path（约 210 B，凑 10 MB 用）。"""
    return (
        f"Startpoint: u_core_{i % 977:03d}/data_r[{i % 16}] (rising edge-triggered flip-flop clocked by clk)\n"
        f"Endpoint: u_core_{(i + 7) % 977:03d}/out_r[{i % 8}] (rising edge-triggered flip-flop clocked by clk)\n"
        "Path Group: clk\n"
        f"Path Type: max\n"
        f"  Slack: {-0.001 * (i % 500):.3f}ns\n"
        f"  Data Path Delay: {1.0 + (i % 37) * 0.011:.3f}ns  (Logic {0.4 + (i % 11) * 0.01:.3f}ns "
        f"Route {0.6 + (i % 23) * 0.02:.3f}ns)\n"
        f"  Clock Path Skew: {-0.02 + (i % 5) * 0.001:.3f}ns\n"
        "  Logic Levels: 5\n\n"
    )


def build(root: str, files: int, report_mb: float) -> dict:
    os.makedirs(root, exist_ok=True)
    rtl = os.path.join(root, "rtl")
    os.makedirs(rtl, exist_ok=True)
    name = os.path.basename(root.rstrip("\\/"))

    entries = []
    for i in range(files):
        rel = f"rtl/mod_{i:03d}.v"
        with open(os.path.join(root, rel), "w", encoding="utf-8", newline="\n") as fh:
            fh.write(VERILOG_TEMPLATE.format(index=i))
        entries.append(
            f'      <File Path="{rel}"><FileInfo>'
            f'<Attr Name="UsedInSyn" Val="true"/><Attr Name="UsedInP&R" Val="true"/>'
            f'<Attr Name="CompileOrder" Val="{i + 1}"/></FileInfo></File>\n'
        )

    al_path = os.path.join(root, name + ".al")
    with open(al_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(AL_TEMPLATE.format(path=root.replace("\\", "/"), name=name,
                                    top="mod_000", files="".join(entries)))

    # 10 MB timing 报告：路径走 <proj>/<name>_Runs/phy_1/final_timing.rpt（reports_aggregate 的首选候选）
    runs = os.path.join(root, name + "_Runs", "phy_1")
    os.makedirs(runs, exist_ok=True)
    report_path = os.path.join(runs, "final_timing.rpt")
    target_bytes = int(report_mb * 1024 * 1024)
    written = 0
    with open(report_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("Timing Summary Report (synthetic, for perf profiling)\n")
        fh.write("Path Group: clk\nMax: SWNS 1.234ns, STNS 0.000ns, 12 Viol Endpoints, 345 Total Endpoints\n\n")
        i = 0
        while written < target_bytes:
            block = timing_block(i)
            fh.write(block)
            written += len(block)
            i += 1

    # 预热 page cache（README §6.4）：把工程与报告都读一遍
    for dirpath, _dirnames, filenames in os.walk(root):
        for fn in filenames:
            with open(os.path.join(dirpath, fn), "rb") as fh:
                while fh.read(1 << 20):
                    pass

    meta = {
        "root": root,
        "al": al_path,
        "name": name,
        "files": files,
        "top": "mod_000",
        "report": report_path,
        "report_bytes": os.path.getsize(report_path),
        "rtl_bytes": sum(os.path.getsize(os.path.join(rtl, f)) for f in os.listdir(rtl)),
    }
    with open(os.path.join(root, "meta.json"), "w", encoding="utf-8") as fh:
        json.dump(meta, fh, indent=1)
    return meta


def clone_project(src_root: str, out_dir: str, clones: int) -> list[dict]:
    """把已建好的工程复制成 N 份独立副本（供"冷索引"场景在**同一个 server 进程**里
    重复 N 次 —— 否则每个进程的 PDB 符号加载（~1 s）会把 50 ms 的索引完全吃掉，
    采样结果全是"卡在 NtReadFile 上等 stdin"）。"""
    import shutil

    result = []
    for i in range(clones):
        dst = os.path.join(out_dir, f"proj200_it{i}")
        shutil.rmtree(dst, ignore_errors=True)
        shutil.copytree(src_root, dst, ignore=shutil.ignore_patterns("_Runs", ".fpgalsp", "meta.json"))
        name = os.path.basename(dst)
        al = os.path.join(dst, "proj200.al")
        al_dst = os.path.join(dst, name + ".al")
        text = open(al, encoding="utf-8").read()
        text = text.replace(f"<Name>proj200</Name>", f"<Name>{name}</Name>")
        text = text.replace(f'Path="{src_root.replace(chr(92), "/")}"', f'Path="{dst.replace(chr(92), "/")}"')
        with open(al_dst, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
        if os.path.exists(al) and al != al_dst:
            os.remove(al)
        # 预热 page cache
        for dirpath, _dirnames, filenames in os.walk(dst):
            for fn in filenames:
                with open(os.path.join(dirpath, fn), "rb") as fh:
                    while fh.read(1 << 20):
                        pass
        result.append({"root": dst, "al": al_dst, "name": name, "top_file": os.path.join(dst, "rtl", "mod_000.v")})
    return result


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--files", type=int, default=200)
    ap.add_argument("--report-mb", type=float, default=10.0)
    ap.add_argument("--clones", type=int, default=0, help="额外复制 N 份工程（冷索引场景用）")
    ap.add_argument("--out", default=os.path.join(os.environ.get("TEMP", "."), "fpgalsp-perf", "profiler"))
    args = ap.parse_args()

    root = os.path.join(args.out, f"proj{args.files}")
    meta = build(root, args.files, args.report_mb)
    if args.clones > 0:
        meta["clones"] = clone_project(root, args.out, args.clones)
    with open(os.path.join(root, "meta.json"), "w", encoding="utf-8") as fh:
        json.dump(meta, fh, indent=1)
    print(json.dumps(meta, indent=1))
    print(f"[make_project] files={meta['files']} al={meta['al']} report={meta['report_bytes']/1e6:.1f} MB "
          f"clones={len(meta.get('clones', []))}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())