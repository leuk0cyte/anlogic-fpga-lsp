"""生成"大报告"合成工程（reports 管线的性能样本）。

为什么不用 `tools/audit/bench_servers.py` 里的 `make_timing_report()`：
那版的 Path 块是**简化格式**（只有 8 个字段、没有 Delay 表、没有 Path Group 段），
真实 TD 报告要重得多（Path Group/Statistics/Delay 表/legacy 兜底都对解析路径有影响）。
这里直接拿 `tests/fixtures/reports/eg4_final_timing.rpt`（真实 TD 2.10 输出，366 KB）
当模板：**保留真实头部**（Check Status / Timing Status / Clock Summary / 列头），
再把一个真实的 Path Group 块重复到目标体积。qor / area / flow.status 直接用真实 fixture。

产物（默认 `%TEMP%\\fpgalsp-perf\\reports\\report010\\`）：

    report010.al                                  # 最小但可解析的 .al（Name/Dir/HardWare）
    report010_Runs/phy_1/final_timing.rpt         # 10 MB（默认）真实格式
    report010_Runs/phy_1/report010_phy.area       # 2 MB（默认）
    report010_Runs/phy_1/route.qor                # 真实 eg4_route.qor
    report010_Runs/phy_1/route_flow.status        # 真实 eg4_route_flow.status
    report010_Runs/syn_1/gate.qor / gate.area / flow.status / gate_timing.rpt

用法：
    python tools/perf/reports/make_synth.py                      # 10 MB timing / 2 MB area
    python tools/perf/reports/make_synth.py --mb 20 --name report020
    python tools/perf/reports/make_synth.py --mb 3,6,10,20 --series   # 生成一组，便于斜率

`--series` 会把每个尺寸的工程都建出来，并打印一行 JSON，便于脚本解析。
所有输出都在 %TEMP% 下（README §4 纪律：禁止写真实工程）。
"""

from __future__ import annotations

import argparse
import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))  # tools/perf/reports/ -> 仓库根
FIXTURES = os.path.join(REPO, "tests", "fixtures", "reports")
DEFAULT_ROOT = os.path.join(os.environ.get("TEMP", "."), "fpgalsp-perf", "reports")

TIMING_TEMPLATE = os.path.join(FIXTURES, "eg4_final_timing.rpt")
AREA_TEMPLATE = os.path.join(FIXTURES, "eg4_phy.area")
GATE_AREA_TEMPLATE = os.path.join(FIXTURES, "eg4_gate.area")
QOR_ROUTE = os.path.join(FIXTURES, "eg4_route.qor")
QOR_GATE = os.path.join(FIXTURES, "eg4_gate.qor")
STATUS_ROUTE = os.path.join(FIXTURES, "eg4_route_flow.status")
STATUS_SYN = os.path.join(FIXTURES, "eg4_syn_flow.status")
GATE_TIMING = os.path.join(FIXTURES, "eg4_gate.timing")


def read_text(path: str) -> str:
    with open(path, encoding="utf-8", errors="replace", newline="") as handle:
        return handle.read()


def write_text(path: str, text: str) -> int:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    return os.path.getsize(path)


def split_template() -> tuple[str, str]:
    """把真实报告切成（头部, 单个 Path Group 块）。

    头部 = 直到第一个 Path Group 之前的水平分隔线；
    块   = 那个分隔线 -> 第二个 Path Group 之前的分隔线。
    """
    lines = read_text(TIMING_TEMPLATE).split("\n")
    group_rows = [i for i, line in enumerate(lines) if line.startswith("Path Group")]
    if not group_rows:
        raise SystemExit("模板里找不到 Path Group 段: %s" % TIMING_TEMPLATE)
    first = group_rows[0]

    def separator_before(index: int) -> int:
        for back in range(index - 1, -1, -1):
            if lines[back].startswith("----"):
                return back
        return index

    block_start = separator_before(first)
    if len(group_rows) > 1:
        block_end = separator_before(group_rows[1])
    else:
        block_end = len(lines) - 1
    header = "\n".join(lines[:block_start]) + "\n"
    block = "\n".join(lines[block_start:block_end]) + "\n"
    return header, block


def make_timing(path: str, target_mb: float) -> dict:
    header, block = split_template()
    target = int(target_mb * 1024 * 1024)
    chunks = [header]
    written = len(header)
    repeats = 0
    while written < target:
        chunks.append(block)
        written += len(block)
        repeats += 1
    size = write_text(path, "".join(chunks))
    return {"path": path, "bytes": size, "mb": round(size / (1024.0 * 1024.0), 2),
            "path_blocks": repeats, "block_bytes": len(block)}


def make_area(path: str, target_mb: float, template: str = AREA_TEMPLATE) -> dict:
    text = read_text(template)
    target = int(target_mb * 1024 * 1024)
    lines = text.split("\n")
    # 保留头部（第一行是标题、随后是 Device/Total 之类），找到第一条数据行后整段重复
    row_start = None
    for index, line in enumerate(lines):
        if index > 20 and line.strip():
            row_start = index
            break
    if row_start is None:
        row_start = max(0, len(lines) // 2)
    header = "\n".join(lines[:row_start]) + "\n"
    rows = "\n".join(lines[row_start:])
    out = [header]
    written = len(header)
    repeats = 0
    while written < target:
        out.append(rows)
        written += len(rows)
        repeats += 1
    size = write_text(path, "".join(out))
    return {"path": path, "bytes": size, "mb": round(size / (1024.0 * 1024.0), 2),
            "repeats": repeats}


def make_al(project_dir: str, name: str) -> str:
    path = os.path.join(project_dir, name + ".al")
    text = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<Project Version="3" Minor="2" Path="%s">\n'
        "    <TD_Version>6.2.3.210850</TD_Version>\n"
        "    <Name>%s</Name>\n"
        "    <HardWare>\n"
        "        <Family>EF2</Family>\n"
        "        <Device>EF2L45LG144B</Device>\n"
        "        <Speed></Speed>\n"
        "    </HardWare>\n"
        "    <Source_Files>\n"
        "    </Source_Files>\n"
        "</Project>\n" % (project_dir.replace("\\", "/"), name))
    write_text(path, text)
    return path


def build_one(root: str, name: str, mb: float, area_mb: float) -> dict:
    project_dir = os.path.join(root, name)
    runs = os.path.join(project_dir, name + "_Runs")
    phy = os.path.join(runs, "phy_1")
    syn = os.path.join(runs, "syn_1")
    os.makedirs(phy, exist_ok=True)
    os.makedirs(syn, exist_ok=True)

    al_path = make_al(project_dir, name)
    timing = make_timing(os.path.join(phy, "final_timing.rpt"), mb)
    area = make_area(os.path.join(phy, name + "_phy.area"), area_mb)
    # qor / flow.status / 小报告：真实 fixture 直接拷（体积可忽略）
    for src, dst in (
        (QOR_ROUTE, os.path.join(phy, "route.qor")),
        (STATUS_ROUTE, os.path.join(phy, "route_flow.status")),
        (QOR_GATE, os.path.join(syn, "gate.qor")),
        (STATUS_SYN, os.path.join(syn, "flow.status")),
    ):
        write_text(dst, read_text(src))
    write_text(os.path.join(syn, "gate.area"), read_text(GATE_AREA_TEMPLATE))
    write_text(os.path.join(syn, "gate_timing.rpt"), read_text(GATE_TIMING))

    # 预热 page cache（README §6.4：先跑的一方不该付首次读盘的钱）
    for path in (timing["path"], area["path"]):
        with open(path, "rb") as handle:
            while handle.read(1 << 20):
                pass

    return {
        "name": name,
        "project_dir": project_dir,
        "al": al_path,
        "runs_dir": runs,
        "timing": timing,
        "area": area,
        "total_report_bytes": timing["bytes"] + area["bytes"],
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", default=DEFAULT_ROOT)
    parser.add_argument("--name", default="")
    parser.add_argument("--mb", default="10", help="timing 大小（MB）；--series 时用逗号分隔")
    parser.add_argument("--area-mb", type=float, default=2.0)
    parser.add_argument("--series", action="store_true", help="按 --mb 逗号列表生成多个工程")
    parser.add_argument("--json-out", default="")
    args = parser.parse_args(argv)

    os.makedirs(args.root, exist_ok=True)
    specs = []
    if args.series:
        for token in str(args.mb).split(","):
            mb = float(token)
            name = args.name or ("report%03d" % int(mb * 10))
            specs.append(build_one(args.root, name, mb, args.area_mb))
    else:
        mb = float(str(args.mb).split(",")[0])
        name = args.name or ("report%03d" % int(mb * 10))
        specs.append(build_one(args.root, name, mb, args.area_mb))

    for spec in specs:
        print(json.dumps(spec, ensure_ascii=False))
    if args.json_out:
        with open(args.json_out, "w", encoding="utf-8") as handle:
            json.dump(specs, handle, ensure_ascii=False, indent=1)
        print("wrote %s" % args.json_out)
    return 0


if __name__ == "__main__":
    sys.exit(main())