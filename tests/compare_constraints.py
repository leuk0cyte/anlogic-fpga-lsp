"""compare_constraints.py —— C++ 约束解析 vs Python 参考实现的逐字段对比

用途
----
阶段 5-α（ADC 引脚约束 / SDC 时序约束）的等价性验收。C++ 侧先把解析结果导出成
JSON（见 tests/unit/adc_test.cpp :: AdcParity.DumpCorpusJsonWhenRequested 与
tests/unit/sdc_test.cpp :: SdcParity.DumpCorpusJsonWhenRequested），本脚本再用
Python 参考实现（%PYREF%\\fpgalsp\\adc.py / sdc.py）对**同一份文本**重算一遍，
逐字段 diff。

对比内容（共 93 个结构）
------------------------
    adc.corpus.entries / coverage / diagnostics        真实语料 pin.adc（47 条约束）
    adc.synthetic[<name>].entries/coverage/diagnostics 18 个边界输入（空文件/缺分号/
                                                       重复 LOCATION/未知 IO 标准/
                                                       越界 bit/未知命令/畸形花括号…）
    sdc.corpus.commands / diagnostics                 真实语料 top.sdc
    sdc.synthetic[<name>].commands/diagnostics        17 个边界输入（缺 -period/nan/
                                                       未闭合 [ { /未知选项/续行/注释…）

用法
----
    # 1) 在私有构建目录里导出 C++ 侧 JSON
    $env:FPGA_LSP_ADC_DUMP = "$env:TEMP\\adc_cpp.json"
    $env:FPGA_LSP_SDC_DUMP = "$env:TEMP\\sdc_cpp.json"
    build-cons\\bin\\unit_adc.exe --gtest_filter=AdcParity.*
    build-cons\\bin\\unit_sdc.exe --gtest_filter=SdcParity.*

    # 2) 对比
    python tests/compare_constraints.py
    python tests/compare_constraints.py --adc <json> --sdc <json>   # 显式指定

退出码：0 = 完全一致；1 = 有差异（逐条打印第一处不同的字段路径）。

注意
----
* 本脚本只读，不修改任何工程目录；语料用 tests/gate-corpus 的副本。
* 两侧的端口夹具必须完全一致，脚本里的 TOP_PORTS/SDC_PORTS 与
  tests/unit/adc_test.cpp、sdc_test.cpp 里的 MakePort/TopPorts/Ports 一一对应。
"""

from __future__ import annotations

import argparse
import json
import os
import sys

PYTHON_ROOT = os.environ.get("FPGA_LSP_PYTHON_ROOT", "")


if not PYTHON_ROOT:
    print("FAIL 缺少参考实现目录：请设置环境变量 FPGA_LSP_PYTHON_ROOT",
          "（Python 参考实现仓库的根目录）")
    sys.exit(2)

if os.path.isdir(PYTHON_ROOT) and PYTHON_ROOT not in sys.path:
    sys.path.insert(0, PYTHON_ROOT)

try:
    from fpgalsp import adc, sdc  # type: ignore
except ImportError as error:  # pragma: no cover - 只在参考实现缺失时触发
    print("无法导入 Python 参考实现（%s）：%s" % (PYTHON_ROOT, error))
    sys.exit(2)

# 语料需自备（release 树不附带工程语料）：由 --corpus 或 FPGA_LSP_TEST_PROJECT 指定，
# 指向某个工程的 constrains/ 目录。
CORPUS = None
ADC_CORPUS = None
SDC_CORPUS = None

MAX_REPORTED_GAPS = 12


def make_port(name: str, width: int = 1, line: int = 0) -> dict:
    """与 C++ 侧 MakePort() 一一对应（含 character/file 的固定值）。"""
    return {
        "name": name,
        "direction": "input",
        "msb": (width - 1) if width > 1 else None,
        "lsb": 0 if width > 1 else None,
        "width": width,
        "line": line,
        "character": 9,
        "file": "top.v",
    }


# 与 tests/unit/adc_test.cpp :: TopPorts() 一致（CorpusAlpha/src/top.v 的顶层端口）
TOP_PORTS = [
    make_port("clk_input", 1, 15), make_port("w", 3, 16), make_port("GATE_LED", 1, 18),
    make_port("GATE_ERR", 1, 19), make_port("ERR_LED", 1, 20), make_port("ad_cs", 1, 23),
    make_port("ad_sck", 1, 24), make_port("ad_dout", 1, 25), make_port("ad_din", 1, 26),
    make_port("pwmG", 6, 29), make_port("IGBT_in", 6, 30), make_port("ERR_in", 6, 31),
    make_port("PowerDown33", 1, 34), make_port("PowerDown150", 1, 35),
    make_port("OH_CHK", 1, 37), make_port("DCOV_CHK", 1, 38), make_port("SUPSTS", 1, 40),
    make_port("ERRCLR", 1, 41), make_port("GR", 1, 42), make_port("sin", 1, 45),
    make_port("sout", 1, 46), make_port("fan_i", 2, 48), make_port("fan_o", 2, 49),
    make_port("relay", 4, 51), make_port("test_led", 1, 52),
]

# 与 tests/unit/sdc_test.cpp :: Ports() 一致
SDC_PORTS = [
    make_port("clk_input"), make_port("w", 3), make_port("relay", 4), make_port("pwmG", 6),
]

# 与 sdc_test.cpp :: kKnownSubset 一致
KNOWN_COMMANDS = {"create_clock", "derive_pll_clocks"}


def canonical(value) -> str:
    return json.dumps(value, sort_keys=True, ensure_ascii=False)


def find_gaps(name: str, cpp, py) -> list[str]:
    """返回两侧 JSON 的差异路径列表（最多 MAX_REPORTED_GAPS 条）。"""
    gaps: list[str] = []

    def walk(a, b, path: str) -> None:
        if len(gaps) >= MAX_REPORTED_GAPS:
            return
        if isinstance(a, bool) != isinstance(b, bool):
            gaps.append("%s: type cpp=%s py=%s" % (path, type(a).__name__, type(b).__name__))
            return
        if isinstance(a, dict) and isinstance(b, dict):
            for key in sorted(set(a) | set(b)):
                if key not in a:
                    gaps.append("%s.%s: 只在 python 侧" % (path, key))
                elif key not in b:
                    gaps.append("%s.%s: 只在 cpp 侧" % (path, key))
                else:
                    walk(a[key], b[key], "%s.%s" % (path, key))
            return
        if isinstance(a, list) and isinstance(b, list):
            if len(a) != len(b):
                gaps.append("%s: 长度 cpp=%d py=%d" % (path, len(a), len(b)))
            for index in range(min(len(a), len(b))):
                walk(a[index], b[index], "%s[%d]" % (path, index))
            return
        if isinstance(a, (int, float)) and isinstance(b, (int, float)):
            if float(a) != float(b):
                gaps.append("%s: cpp=%r py=%r" % (path, a, b))
            return
        if a != b:
            gaps.append("%s: cpp=%r py=%r" % (path, a, b))

    walk(cpp, py, name)
    return gaps


class Comparison:
    def __init__(self) -> None:
        self.total = 0
        self.failed = 0
        self.diverged: list[str] = []

    def check(self, label: str, cpp, py) -> None:
        self.total += 1
        if canonical(cpp) == canonical(py):
            return
        self.failed += 1
        self.diverged.append(label)
        print("DIFF %s" % label)
        for gap in find_gaps(label, cpp, py):
            print("     %s" % gap)


def compare_adc(dump_path: str, result: Comparison) -> None:
    with open(dump_path, encoding="utf-8") as handle:
        dump = json.load(handle)
    with open(ADC_CORPUS, encoding="utf-8") as handle:
        text = handle.read()
    entries = adc.parse(text)
    result.check("adc.corpus.entries", dump["entries"], entries)
    result.check("adc.corpus.coverage", dump["coverage"],
                 adc.coverage(entries, TOP_PORTS, "top"))
    result.check("adc.corpus.diagnostics", dump["diagnostics"],
                 adc.diagnostics(entries, TOP_PORTS, None, text))

    for case in dump.get("synthetic", []):
        case_text = case["text"]
        case_entries = adc.parse(case_text)
        prefix = "adc.synthetic[%s]" % case["name"]
        result.check("%s.entries" % prefix, case["entries"], case_entries)
        result.check("%s.coverage" % prefix, case["coverage"],
                     adc.coverage(case_entries, TOP_PORTS))
        result.check("%s.diagnostics" % prefix, case["diagnostics"],
                     adc.diagnostics(case_entries, TOP_PORTS, None, case_text))


def compare_sdc(dump_path: str, result: Comparison) -> None:
    with open(dump_path, encoding="utf-8") as handle:
        dump = json.load(handle)
    with open(SDC_CORPUS, encoding="utf-8") as handle:
        text = handle.read()
    commands = sdc.parse(text)
    result.check("sdc.corpus.commands", dump["commands"], commands)
    result.check("sdc.corpus.diagnostics", dump["diagnostics"],
                 sdc.diagnostics(commands, SDC_PORTS, KNOWN_COMMANDS))

    for case in dump.get("synthetic", []):
        case_text = case["text"]
        case_commands = sdc.parse(case_text)
        prefix = "sdc.synthetic[%s]" % case["name"]
        result.check("%s.commands" % prefix, case["commands"], case_commands)
        result.check("%s.diagnostics" % prefix, case["diagnostics"],
                     sdc.diagnostics(case_commands, SDC_PORTS, KNOWN_COMMANDS))


def main() -> int:
    parser = argparse.ArgumentParser(description="C++ 约束解析 vs Python 参考实现")
    parser.add_argument("--adc", default=os.path.join(os.environ.get("TEMP", "."), "adc_cpp.json"),
                        help="unit_adc 导出的 JSON（FPGA_LSP_ADC_DUMP）")
    parser.add_argument("--sdc", default=os.path.join(os.environ.get("TEMP", "."), "sdc_cpp.json"),
                        help="unit_sdc 导出的 JSON（FPGA_LSP_SDC_DUMP）")
    parser.add_argument("--corpus", default=None,
                        help="工程 constrains/ 目录（**必填**：本仓库不附带工程语料；"
                             "也可用环境变量 FPGA_LSP_TEST_PROJECT）")
    args = parser.parse_args()

    global CORPUS, ADC_CORPUS, SDC_CORPUS
    requested = args.corpus or os.environ.get("FPGA_LSP_TEST_PROJECT")
    if not requested:
        print("FAIL 缺少语料目录：请传 --corpus <dir>（或设 FPGA_LSP_TEST_PROJECT）")
        return 2
    CORPUS = requested
    ADC_CORPUS = os.path.join(CORPUS, "pin.adc")
    SDC_CORPUS = os.path.join(CORPUS, "top.sdc")

    missing = [path for path in (args.adc, args.sdc) if not os.path.isfile(path)]
    if missing:
        for path in missing:
            print("找不到 C++ 导出的 JSON：%s" % path)
        print("请先按 tests/PYTHON-BUGS.md「阶段 5-α 的复现与验证」导出。")
        return 2

    result = Comparison()
    compare_adc(args.adc, result)
    compare_sdc(args.sdc, result)

    print("")
    print("compared %d structures, %d differ" % (result.total, result.failed))
    if result.failed:
        for label in result.diverged:
            print("  - %s" % label)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())