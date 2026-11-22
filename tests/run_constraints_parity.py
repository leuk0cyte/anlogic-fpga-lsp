#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""CTest wrapper：导出 C++ 约束解析结果，再跑 compare_constraints.py 与 Python 参考对比。

为什么需要这层包装：`unit_adc/unit_sdc` 只有在设置了 `FPGA_LSP_ADC_DUMP` /
`FPGA_LSP_SDC_DUMP` 时才写导出 JSON（`AdcParity.DumpCorpusJsonWhenRequested` /
`SdcParity.DumpCorpusJsonWhenRequested`），而 `compare_constraints.py` 需要两份 JSON
都存在才做逐字段比较。CTest 不能"先跑测试再跑脚本"，所以用这个脚本串起来：

    1. 建临时目录，设置两个 DUMP 环境变量；
    2. 跑 unit_adc / unit_sdc 的 *Parity.* 用例（导出 JSON）；
    3. 用同一解释器跑 compare_constraints.py --adc ... --sdc ...；
    4. 清理临时目录，把退出码透传出去。

用法：
    python tests/run_constraints_parity.py --bin-dir build/bin
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent


def run(cmd: list[str], env: dict[str, str], label: str) -> int:
    print(f"[parity] {label}: {' '.join(cmd)}")
    completed = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if completed.stdout.strip():
        print(completed.stdout.rstrip())
    if completed.returncode != 0:
        if completed.stderr.strip():
            print(completed.stderr.rstrip(), file=sys.stderr)
        print(f"[parity] {label} 失败（exit={completed.returncode}）", file=sys.stderr)
    return completed.returncode


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin-dir", default=str(REPO / "build" / "bin"),
                        help="包含 unit_adc.exe / unit_sdc.exe 的目录")
    args = parser.parse_args()

    bin_dir = Path(args.bin_dir)
    unit_adc = bin_dir / "unit_adc.exe"
    unit_sdc = bin_dir / "unit_sdc.exe"
    for exe in (unit_adc, unit_sdc):
        if not exe.is_file():
            print(f"[parity] 找不到 {exe}", file=sys.stderr)
            return 1

    work = Path(tempfile.mkdtemp(prefix="fpgalsp_constraints_parity_"))
    try:
        adc_dump = work / "adc_cpp.json"
        sdc_dump = work / "sdc_cpp.json"
        env = dict(os.environ)
        env["FPGA_LSP_ADC_DUMP"] = str(adc_dump)
        env["FPGA_LSP_SDC_DUMP"] = str(sdc_dump)

        for exe, filt, label in ((unit_adc, "AdcParity.*", "dump adc"),
                                 (unit_sdc, "SdcParity.*", "dump sdc")):
            code = run([str(exe), f"--gtest_filter={filt}"], env, label)
            if code != 0:
                return code

        for dump, label in ((adc_dump, "adc"), (sdc_dump, "sdc")):
            if not dump.is_file() or dump.stat().st_size == 0:
                print(f"[parity] {label} 导出文件缺失或为空: {dump}", file=sys.stderr)
                return 1

        return run([sys.executable, str(HERE / "compare_constraints.py"),
                    "--adc", str(adc_dump), "--sdc", str(sdc_dump)], env, "compare")
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
