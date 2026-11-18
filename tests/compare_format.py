#!/usr/bin/env python3
"""阶段 1 formatter 门禁：官方 verible-verilog-format vs 我方 format_dump 逐字节对比。

方法论（与阶段 0 的 token 门禁、阶段 1 的 lint 门禁同一套）：

  1. **复用官方同一条路径**：format_dump 不重写输出格式，逐项调用
     `GetContentAsString` / `InitializeFromFlags` / `FormatVerilog`，
     与官方 verilog-format.cc 一致（详见 src/tools/format_dump/main.cpp 头注释）。
     因此"输出一致"是构造上保证的，一旦有差异就是移植本身的问题。

  2. **归一化只做路径前缀**：格式化的输出里可能含文件名（诊断消息），
     把语料根路径折叠成 <corpus>，其余**一个字节都不动**。
     不做换行归一化、不做空白折叠——那会掩盖真实差异。

  3. **对比两个通道**：stdout（格式化结果）+ 退出码。
     官方在出错时把诊断写 **stdout**（control.stream = &std::cout），
     原样输出原文（failsafe），所以出错场景下 stdout 也是可比的。

  4. **变异测试证明脚本灵敏**：见 --selftest。
     只报"全一致"的脚本是没有价值的——必须证明它抓得住差异。

退出码：0 全一致 / 1 有差异 / 2 环境/用法错误
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys

# ---------------------------------------------------------------------------
# 默认路径（可用命令行覆盖）
# ---------------------------------------------------------------------------
SOURCE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_CORPUS = os.path.join(SOURCE_DIR, "tests", "gate-corpus")
DEFAULT_OFFICIAL = r"%USERPROFILE%\verible\verible-verilog-format.exe"

# format_dump.exe 的位置：优先私有构建，再退到共享 build。
_CANDIDATE_OURS = [
    os.path.join(SOURCE_DIR, "build-fmt", "bin", "format_dump.exe"),
    os.path.join(SOURCE_DIR, "build", "bin", "format_dump.exe"),
]


def find_ours(explicit: str | None) -> str | None:
    if explicit:
        return explicit if os.path.isfile(explicit) else None
    env = os.environ.get("FPGALSP_FORMAT_DUMP_EXE")
    if env and os.path.isfile(env):
        return env
    for path in _CANDIDATE_OURS:
        if os.path.isfile(path):
            return path
    return None


def collect_files(corpus_root: str) -> list[str]:
    """语料里所有 .v / .sv（.vhd 不参与：Verible 不支持 VHDL）。"""
    out: list[str] = []
    for dirpath, _dirnames, filenames in os.walk(corpus_root):
        for name in filenames:
            if os.path.splitext(name)[1].lower() in (".v", ".sv"):
                out.append(os.path.join(dirpath, name))
    out.sort()
    return out


def run_tool(argv: list[str]) -> tuple[int, bytes]:
    """跑一个工具，返回 (退出码, stdout 字节)。

    ⚠ 只比 stdout。官方把诊断也写 stdout（control.stream = &std::cout），
      所以 stdout 已经覆盖了"格式化结果 + 诊断"两个来源。

    Windows 上必须显式设 stdin/stdout 为二进制，否则 CRLF 会被转换。
    """
    try:
        proc = subprocess.run(
            argv,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=300,
            check=False,
        )
        return proc.returncode, proc.stdout
    except subprocess.TimeoutExpired:
        return -1, b"<TIMEOUT>"
    except OSError as exc:  # 工具不存在/不可执行
        return -2, f"<OSERROR {exc}>".encode()


def normalize_paths(data: bytes, corpus_root: str) -> bytes:
    """把语料根路径折叠成 <corpus>。

    输出里的文件名出现在诊断消息中（错误场景）。除了这一步，
    **不对内容做任何改动**——换行、空白、顺序全部逐字节比较。
    """
    variants = {
        corpus_root.replace("\\", "/"),
        corpus_root,
    }
    out = data
    for variant in variants:
        if not variant:
            continue
        out = out.replace(variant.encode("utf-8"), b"<corpus>")
        out = out.replace(variant.replace("/", "\\").encode("utf-8"), b"<corpus>")
    return out


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def first_diff(a: bytes, b: bytes) -> str:
    limit = min(len(a), len(b))
    for i in range(limit):
        if a[i] != b[i]:
            return (
                f"首个差异 @ offset {i}: "
                f"official=0x{a[i]:02X} ours=0x{b[i]:02X} "
                f"(official 上下文={a[max(0, i - 20):i + 20]!r})"
            )
    if len(a) != len(b):
        return f"前缀一致，长度不同: official={len(a)}B ours={len(b)}B"
    return "无差异"


def run_gate(ours: str, official: str, corpus: str, verbose: bool) -> int:
    files = collect_files(corpus)
    if not files:
        print(f"[ERROR] 语料为空: {corpus}")
        return 2

    print(f"官方端点 : {official}")
    print(f"我方端点 : {ours}")
    print(f"语料     : {corpus}")
    print(f"文件数   : {len(files)}")
    print()

    passed = 0
    failures: list[tuple[str, str]] = []
    total_bytes = 0

    for path in files:
        rel = os.path.relpath(path, corpus).replace("\\", "/")
        rc_o, out_o = run_tool([official, path])
        rc_m, out_m = run_tool([ours, path])

        out_o = normalize_paths(out_o, corpus)
        out_m = normalize_paths(out_m, corpus)
        total_bytes += len(out_o)

        same_bytes = sha(out_o) == sha(out_m)
        same_rc = rc_o == rc_m

        if same_bytes and same_rc:
            passed += 1
            if verbose:
                print(f"  [PASS] {rel}  ({len(out_o)}B, rc={rc_o})")
            continue

        reasons = []
        if not same_bytes:
            reasons.append(first_diff(out_o, out_m))
        if not same_rc:
            reasons.append(f"退出码不同: official={rc_o} ours={rc_m}")
        failures.append((rel, " ; ".join(reasons)))
        print(f"  [FAIL] {rel}")
        for reason in reasons:
            print(f"         {reason}")

    print()
    total = len(files)
    rate = 100.0 * passed / total if total else 0.0
    print("=" * 74)
    print(f"共 {total} 个文件，一致 {passed} 个，不一致 {len(failures)} 个，"
          f"一致率 {rate:.2f}%")
    print(f"对比输出总字节: {total_bytes}")
    print("=" * 74)

    if failures:
        print("\n不一致明细：")
        for rel, reason in failures:
            print(f"  - {rel}: {reason}")
        return 1
    return 0


def run_selftest(ours: str, official: str, corpus: str) -> int:
    """变异测试：证明脚本抓得住差异。

    三个用例（与阶段 0/1 的方法论一致）：
      A 自比（官方 vs 官方）        → 必须 100%，证明采集与归一化是确定性的
      B 换端点（官方 vs 我方）      → 这是真正的门禁
      C 单边变异（改 flag）         → 必须发现差异，证明脚本灵敏
    """
    files = collect_files(corpus)
    if not files:
        print("[ERROR] 语料为空")
        return 2

    # 变异测试必须挑一个**能成功格式化**的样本。
    # 踩过的坑：一开始用 files[0]（恰好是 error-cases 里的语法错误文件），
    # 官方在 failsafe 模式下原样输出原文，缩进参数根本不起作用 →
    # 单边变异测不出差异 → 误判"脚本不灵敏"。
    # 正确做法：先用官方跑一遍，取第一个 stdout 与原文不同的文件。
    sample = None
    for candidate in files:
        try:
            with open(candidate, "rb") as fh:
                original = fh.read()
        except OSError:
            continue
        _rc, out = run_tool([official, candidate])
        if out and out != original:
            sample = candidate
            break
    if sample is None:
        print("[ERROR] 语料里找不到任何『会被格式化』的文件，无法做变异测试")
        return 2
    print(f"selftest 样本: {os.path.relpath(sample, corpus)}")

    # A 自比
    rc_a1, out_a1 = run_tool([official, sample])
    rc_a2, out_a2 = run_tool([official, sample])
    a_same = sha(out_a1) == sha(out_a2) and rc_a1 == rc_a2
    print(f"  [A] 官方 vs 官方           : "
          f"{'PASS（确定性）' if a_same else 'FAIL（采集不确定！）'}")

    # B 换端点
    rc_b, out_b = run_tool([ours, sample])
    b_same = sha(out_a1) == sha(out_b) and rc_a1 == rc_b
    print(f"  [B] 官方 vs 我方           : "
          f"{'PASS（一致）' if b_same else 'FAIL（有差异）'}")

    # C 单边变异：只给**我方**换缩进宽度，官方保持默认。
    #    正确做法是只改一侧——两边同时改会"一起变"，一致率仍 100%，
    #    那是假绿灯（阶段 1 lint 门禁踩过这个坑）。
    mutated_pass = True
    try:
        rc_c, out_c = run_tool([ours, "--indentation_spaces=7", sample])
    except Exception:  # noqa: BLE001
        rc_c, out_c = -1, b""
    c_same = sha(out_a1) == sha(out_c)
    if c_same:
        mutated_pass = False
    print(f"  [C] 我方缩进=7 vs 官方默认 : "
          f"{'PASS（发现差异，脚本灵敏）' if mutated_pass else 'FAIL（假绿灯！脚本作废）'}")

    ok = a_same and mutated_pass
    print()
    print("selftest 结论:", "PASS" if ok else "FAIL")
    if not mutated_pass:
        print("  ⚠ 单边变异没被发现 → 对比脚本不灵敏，门禁结论不可信。")
    return 0 if ok else 1


def main() -> int:
    parser = argparse.ArgumentParser(description="formatter 门禁：逐字节对比")
    parser.add_argument("--corpus", default=DEFAULT_CORPUS)
    parser.add_argument("--official", default=DEFAULT_OFFICIAL)
    parser.add_argument("--ours", default=None,
                        help="format_dump.exe 路径（默认自动探测）")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--selftest", action="store_true",
                        help="跑变异测试，证明脚本灵敏")
    args = parser.parse_args()

    if not os.path.isdir(args.corpus):
        print(f"[ERROR] 语料目录不存在: {args.corpus}")
        return 2
    if not os.path.isfile(args.official):
        print(f"[ERROR] 官方二进制不存在: {args.official}")
        return 2
    ours = find_ours(args.ours)
    if not ours:
        print("[ERROR] 找不到 format_dump.exe。"
              "先构建 `cmake --build <build> --target format_dump`，"
              "或用 --ours 指定路径。")
        print("        探测过：", _CANDIDATE_OURS)
        return 2

    if args.selftest:
        return run_selftest(ours, args.official, args.corpus)
    return run_gate(ours, args.official, args.corpus, args.verbose)


if __name__ == "__main__":
    sys.exit(main())