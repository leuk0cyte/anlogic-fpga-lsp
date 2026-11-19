#!/usr/bin/env python
"""生成 perf-td 用的**合成工程**（只写 %TEMP%\\fpgalsp-perf\\td\\）。

为什么要合成工程：
  * 真实工程 Y:\\CorpusAlpha 是只读的，且 TD 会往 <project>/.fpgalsp/run 写日志；
  * 性能测量需要可控的文件大小（13 KB / 100 KB）与可控的 HDL 内容。

三个工程：
  p13   13 KB 的 small.v（codeAction/lint、didChange 用）
  p100  100 KB 的 big.v（同上，测 O(文件大小)）
  ptd   一个很小的 top.v（真实 TD read_hdl 端到端用，让一轮 TD 尽量短）

用法：
    python tools\\perf\\td\\make_project.py            # 建/重建到默认目录
    python tools\\perf\\td\\make_project.py --dir D:\\x  # 指定根目录
"""

from __future__ import annotations

import argparse
import os
import shutil

DEVICE = "EF2L45LG144B"
FAMILY = "EF2"


def make_verilog(target_bytes: int, seed: int = 0) -> str:
    """生成语法合法的 Verilog：每 ~1.0 KB 一个模块（含 always / assign）。"""
    out: list[str] = []
    size = 0
    index = 0
    while size < target_bytes:
        name = f"gen_{seed}_{index}"
        block = [
            f"module {name} (input wire clk, input wire [7:0] a, output reg [7:0] q);\n",
        ]
        for j in range(8):
            block.append(f"  wire [7:0] w_{j};\n")
            block.append(f"  assign w_{j} = a + 8'd{j};\n")
        block.append("  always @(posedge clk) begin\n")
        block.append("    q <= w_0;\n")
        block.append("  end\n")
        block.append(f"endmodule  // {name}\n\n")
        text = "".join(block)
        out.append(text)
        size += len(text)
        index += 1
    return "".join(out)


def make_top(inner_modules: list[str], undefined_module: str = "") -> str:
    """一个把生成模块串起来的 top（TD read_hdl 从 top 开始解析）。

    undefined_module 非空时额外例化一个不存在的模块 —— 用来让 TD 产出**非空诊断**
    （否则 "0 problem(s)" 就没有 publishDiagnostics 可测，端到端延迟无法观察）。
    """
    lines = ["module top (input wire clk, input wire [7:0] a, output wire [7:0] q);\n"]
    previous = "a"
    for i, name in enumerate(inner_modules):
        lines.append(f"  wire [7:0] n{i};\n")
        lines.append(f"  {name} u{i} (.clk(clk), .a({previous}), .q(n{i}));\n")
        previous = f"n{i}"
    if undefined_module:
        lines.append(f"  {undefined_module} u_missing (.clk(clk));\n")
    lines.append(f"  assign q = {previous};\n")
    lines.append("endmodule\n")
    return "".join(lines)


def make_al(name: str, hdl_files: list[str], top: str) -> str:
    """一个 TD 能读的最小 .al（字段与真实工程一致：Family/Device/Speed/TOP_MODULE/Source_Files）。"""
    entries = []
    for order, rel in enumerate(hdl_files, start=1):
        entries.append(
            "    <Verilog>\n"
            f"      <File Path=\"{rel}\">\n"
            "        <FileInfo>\n"
            "          <Attr Name=\"UsedInSyn\" Val=\"true\"/>\n"
            "          <Attr Name=\"UsedInP&R\" Val=\"true\"/>\n"
            "          <Attr Name=\"BelongTo\" Val=\"design_1\"/>\n"
            f"          <Attr Name=\"CompileOrder\" Val=\"{order}\"/>\n"
            "        </FileInfo>\n"
            "      </File>\n"
            "    </Verilog>\n"
        )
    return (
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<Project Version=\"3\" Minor=\"2\">\n"
        f"  <Name>{name}</Name>\n"
        "  <HardWare>\n"
        "    <Family>{FAMILY}</Family>\n"
        f"    <Device>{DEVICE}</Device>\n"
        # ⚠ Speed 必须像真实工程一样为空：写 -2 会让 import_device 带上 -speed 2，
        #    TD 的 35 个 db 候选全部拒绝（实测 "No device library supports ..."），
        #    测出来的就不是 read_hdl 而是"35 次失败 import"。
        "    <Speed></Speed>\n"
        "  </HardWare>\n"
        f"  <TOP_MODULE><MODULE>{top}</MODULE></TOP_MODULE>\n"
        "  <Source_Files>\n" + "".join(entries) + "  </Source_Files>\n"
        "</Project>\n"
    )


def write(path: str, text: str) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write(text)


def build(root: str) -> dict:
    if os.path.isdir(root):
        shutil.rmtree(root)
    os.makedirs(root, exist_ok=True)
    made = {}

    # p13：13 KB
    small = make_verilog(13 * 1024, seed=13)
    write(os.path.join(root, "p13", "src", "small.v"), small)
    write(os.path.join(root, "p13", "src", "top.v"),
          make_top(["gen_13_0", "gen_13_1"]))
    write(os.path.join(root, "p13", "p13.al"),
          make_al("p13", ["src/small.v", "src/top.v"], "top"))
    made["p13"] = os.path.join(root, "p13")

    # p100：100 KB（并且也带 13 KB 的 small.v，供"codeAction 阻塞 didChange"同进程实验）
    big = make_verilog(100 * 1024, seed=100)
    write(os.path.join(root, "p100", "src", "big.v"), big)
    write(os.path.join(root, "p100", "src", "small.v"), make_verilog(13 * 1024, seed=13))
    write(os.path.join(root, "p100", "src", "top.v"),
          make_top(["gen_100_0", "gen_100_1"]))
    write(os.path.join(root, "p100", "p100.al"),
          make_al("p100", ["src/big.v", "src/small.v", "src/top.v"], "top"))
    made["p100"] = os.path.join(root, "p100")

    # ptd：很小的工程（真实 TD 一轮尽量短）。故意引用一个不存在的模块，
    #      让 TD 产出非空诊断 -> 才能测 "didSave -> TD -> publishDiagnostics" 的延迟。
    write(os.path.join(root, "ptd", "src", "top.v"),
          make_top(["gen_td_0"], undefined_module="module_that_does_not_exist"))
    write(os.path.join(root, "ptd", "src", "gen_td_0.v"), make_verilog(1024, seed=0))
    write(os.path.join(root, "ptd", "ptd.al"),
          make_al("ptd", ["src/gen_td_0.v", "src/top.v"], "top"))
    made["ptd"] = os.path.join(root, "ptd")

    return made


def main() -> int:
    parser = argparse.ArgumentParser()
    # ⚠ 用**私有**根目录：%TEMP%\fpgalsp-perf\ 是多名 agent 共享的，实测被别人的清理
    #    脚本删过一次（我们的 p13/p100/ptd 全没了）。perf-td 自己用 fpgalsp-perf-td。
    parser.add_argument("--dir", default=os.path.join(
        os.environ.get("TEMP", "."), "fpgalsp-perf-td"))
    args = parser.parse_args()
    made = build(os.path.abspath(args.dir))
    for name, path in made.items():
        al = [f for f in os.listdir(path) if f.endswith(".al")]
        sizes = {f: os.path.getsize(os.path.join(path, "src", f))
                 for f in os.listdir(os.path.join(path, "src"))}
        print(f"{name}: {path}")
        print(f"    al={al} src={sizes}")
    print("done")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())