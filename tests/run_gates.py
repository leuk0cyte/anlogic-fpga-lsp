#!/usr/bin/env python3
# =============================================================================
#  tests/run_gates.py —— 门禁双跑：阶段 0（gate）+ 阶段 1（lintgate）
# =============================================================================
#  一次命令跑完两套门禁，输出合并报告。用途：
#    * G6a / 生成器修复后，**同时**确认：
#        - 阶段 0 的 102/102 token 一致性结论是否仍然成立
#        - 阶段 1 lint 规则清单（61 行 / 42 启用）是否通过
#    * 把两套结论放在一起看，避免"修好 A 弄坏 B"。
#
#  为什么必须一起跑（而不是只看 lintgate）：
#    生成器产物是共用的。G4 的修复让 verilog-token-enum.h 从 547 行变成 485 行，
#    **内容确实变了**。所以"gate 仍然 102/102"证明的不是"gate 还能跑"，
#    而是"生成器修正没有破坏解析行为"——这是两件不同的事。
#
#  用法：
#    python tests/run_gates.py                     # 跑两套，控制台摘要
#    python tests/run_gates.py --json out.json     # 额外写机器可读结果
#    python tests/run_gates.py --only gate         # 只跑一套
#
#  退出码：0 = 全部通过；1 = 有失败；2 = 环境错误
# =============================================================================

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEFAULT_CST_DUMP = REPO / "build" / "bin" / "cst_dump.exe"
DEFAULT_LINT_DUMP = REPO / "build" / "bin" / "lint_dump.exe"
DEFAULT_OFFICIAL_SYNTAX = Path(r"%USERPROFILE%\verible\verible-verilog-syntax.exe")
DEFAULT_OFFICIAL_LINT = Path(r"%USERPROFILE%\verible\verible-verilog-lint.exe")
CORPUS = REPO / "tests" / "gate-corpus"


def run(cmd: list[str]) -> tuple[int, str]:
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode("utf-8", errors="surrogateescape")


def tail(text: str, n: int = 6) -> str:
    lines = [ln for ln in text.strip().split("\n") if ln.strip()]
    return "\n".join(lines[-n:])


def run_gate(cst_dump: Path, official: Path) -> dict:
    """阶段 0：token 一致性（102 文件）。"""
    print("=" * 78)
    print("阶段 0 门禁 (gate)：token 序列一致性")
    print("=" * 78)

    if not cst_dump.is_file():
        print(f"  ⚠️ 跳过：cst_dump 不存在 {cst_dump}")
        return {"name": "gate", "status": "skipped",
                "reason": f"cst_dump not found: {cst_dump}"}
    if not official.is_file():
        print(f"  ⚠️ 跳过：官方基准不存在 {official}")
        return {"name": "gate", "status": "skipped",
                "reason": f"official not found: {official}"}

    rc, out = run([
        sys.executable, str(REPO / "tests" / "compare_cst.py"),
        "--cst-dump", str(cst_dump),
        "--official", str(official),
        "--corpus", str(CORPUS),
        "--pattern", ".v,.sv",
        "--report", str(REPO / "build" / "gate-report.json"),
    ])
    print(tail(out, 10))

    # 从汇总行解析数字
    files = consistent = 0
    rate = None
    for ln in out.split("\n"):
        if "共 " in ln and "一致率" in ln:
            import re
            m = re.search(r"共 (\d+) 个文件，一致 (\d+) 个.*?一致率 ([\d.]+)%", ln)
            if m:
                files, consistent, rate = (int(m.group(1)), int(m.group(2)),
                                           float(m.group(3)))
    return {
        "name": "gate",
        "status": "pass" if rc == 0 else "fail",
        "returncode": rc,
        "files": files,
        "consistent": consistent,
        "rate": rate,
    }


def run_lint_rules(lint_dump: Path, official: Path) -> dict:
    """阶段 1 第 1 层：规则清单对比。"""
    print()
    print("=" * 78)
    print("阶段 1 门禁 (lintgate 第 1 层)：规则清单对比")
    print("=" * 78)

    if not lint_dump.is_file():
        print(f"  ⚠️ 跳过：lint_dump 不存在 {lint_dump}")
        return {"name": "lintgate-rules", "status": "skipped",
                "reason": f"lint_dump not found: {lint_dump}"}
    if not official.is_file():
        print(f"  ⚠️ 跳过：官方 lint 不存在 {official}")
        return {"name": "lintgate-rules", "status": "skipped",
                "reason": f"official lint not found: {official}"}

    rc, out = run([
        sys.executable, str(REPO / "tests" / "compare_lint.py"),
        "--lint-dump", str(lint_dump),
        "--official", str(official),
        "--rules-file",
    ])
    print(out.strip())

    # 解析官方/我方规则数
    import re
    o_n = d_n = None
    o_en = d_en = None
    for ln in out.split("\n"):
        m = re.search(r"官方\s*:\s*(\d+) 行规则，其中默认启用 (\d+) 条", ln)
        if m:
            o_n, o_en = int(m.group(1)), int(m.group(2))
        m = re.search(r"lint_dump:\s*(\d+) 行规则，其中默认启用 (\d+) 条", ln)
        if m:
            d_n, d_en = int(m.group(1)), int(m.group(2))

    return {
        "name": "lintgate-rules",
        "status": "pass" if rc == 0 else "fail",
        "returncode": rc,
        "official_rules": o_n,
        "official_enabled": o_en,
        "dump_rules": d_n,
        "dump_enabled": d_en,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cst-dump", default=str(DEFAULT_CST_DUMP))
    ap.add_argument("--lint-dump", default=str(DEFAULT_LINT_DUMP))
    ap.add_argument("--official-syntax", default=str(DEFAULT_OFFICIAL_SYNTAX))
    ap.add_argument("--official-lint", default=str(DEFAULT_OFFICIAL_LINT))
    ap.add_argument("--only", choices=["gate", "lintgate"], default=None,
                    help="只跑其中一套")
    ap.add_argument("--json", default=None, help="机器可读结果输出路径")
    args = ap.parse_args()

    t0 = time.time()
    results = []

    if args.only in (None, "gate"):
        results.append(run_gate(Path(args.cst_dump), Path(args.official_syntax)))
    if args.only in (None, "lintgate"):
        results.append(run_lint_rules(Path(args.lint_dump),
                                      Path(args.official_lint)))

    elapsed = time.time() - t0

    # ---------------- 合并摘要 ----------------
    print()
    print("=" * 78)
    print("合并摘要")
    print("=" * 78)

    all_ok = True
    for r in results:
        st = r["status"]
        if st == "skipped":
            print(f"  [SKIP] {r['name']:16s} {r.get('reason', '')}")
            all_ok = False
        elif st == "pass":
            if r["name"] == "gate":
                print(f"  [PASS] {r['name']:16s} "
                      f"{r['consistent']}/{r['files']} 文件一致 "
                      f"({r['rate']:.2f}%)")
            else:
                print(f"  [PASS] {r['name']:16s} "
                      f"官方 {r['official_rules']} 行 / 启用 {r['official_enabled']}；"
                      f"我方 {r['dump_rules']} 行 / 启用 {r['dump_enabled']}")
        else:
            all_ok = False
            print(f"  [FAIL] {r['name']:16s} 退出码 {r.get('returncode')}")

    print(f"\n总耗时 {elapsed:.1f}s")

    print()
    if all_ok:
        print("双跑结论：**全部通过**")
        print()
        print("说明：")
        print("  gate 通过 = 生成器修正（G4 产物 547→485 行）没有破坏解析行为")
        print("  lintgate 通过 = lint 规则清单（61/42）与官方一致，无规则因漏链丢失")
    else:
        print("双跑结论：**未全部通过**（含跳过项，见上）")

    if args.json:
        payload = {
            "elapsed_sec": round(elapsed, 2),
            "all_ok": all_ok,
            "results": results,
        }
        Path(args.json).parent.mkdir(parents=True, exist_ok=True)
        Path(args.json).write_text(
            json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
        print(f"\nJSON 结果: {args.json}")

    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())