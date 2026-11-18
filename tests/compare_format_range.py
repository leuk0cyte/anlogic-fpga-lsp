#!/usr/bin/env python3
"""阶段 1 formatter 门禁（局部/选区格式化）：官方 `--lines N-M` vs 我方 `--range_*`。

## 为什么局部格式化必须单独验证

它与全文档格式化是**不同的代码路径**，而且 `formatter.h` 明确写了差异：

    // Used for interactive formatting, e.g. in editors and does not do
    // convergence test.

即 `FormatVerilogRange` **不做收敛性检验**，而全文档格式化默认做
（`verify_convergence = true`）。两条路径本来就可能产生不同结果。

## 官方 `--lines` 的真实语义（实测确认，容易误解）

`--lines 5-10` 对一个 27 行的文件输出 **37 行（整个文件）**，不是只输出第 5-10 行。

官方实现是把 `--lines` 解析成 `LineNumberSet`，再传给 `FormatVerilog(...)`：

    FormatVerilog(text, name, style, stream, lines_to_format, control)

所以"仅这些行参与格式化决策，输出仍是全文"。
`FormatVerilogRange`（只输出区间那一段）是**另一个 API**，编辑器做
"格式化选区"时才用；我们的门禁对齐的是官方 CLI，所以走 `lines_to_format` 这条。

## 复用而不是复制

`run_tool` / `normalize_paths` / `first_diff` / `collect_files` 全部从
`compare_format.py` import，保证两个门禁的归一化规则**永远一致**。

退出码：0 全一致 / 1 有差异 / 2 环境或用法错误
"""

from __future__ import annotations

import argparse
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# 复用全文档门禁的工具函数（单一实现，避免两套归一化规则漂移）
from compare_format import (  # noqa: E402
    DEFAULT_CORPUS,
    DEFAULT_OFFICIAL,
    collect_files,
    find_ours,
    first_diff,
    normalize_paths,
    run_tool,
    sha,
)


def count_lines(path: str) -> int:
    """数文件行数（用于挑选不越界的 range）。"""
    try:
        with open(path, "rb") as fh:
            data = fh.read()
    except OSError:
        return 0
    if not data:
        return 0
    # 末尾没有换行时，最后一行也算一行
    return data.count(b"\n") + (0 if data.endswith(b"\n") else 1)


def pick_ranges(line_count: int) -> list[tuple[int, int]]:
    """给一个文件挑 3 个 1 基闭区间：开头 / 中间 / 结尾。

    选 3 个位置是为了覆盖"区间与结构边界的关系"：
      * 开头：区间起点与文件起点重合（边界条件）
      * 中间：区间两端都不贴边（最常见）
      * 结尾：区间终点与文件末行重合（边界条件）
    """
    if line_count <= 0:
        return []
    if line_count <= 3:
        return [(1, line_count)]

    picked: list[tuple[int, int]] = []

    # 开头：1..min(10, n)
    picked.append((1, min(10, line_count)))

    # 中间：随机起点但确定（用文件行数做种子，保证可复现）
    rng = random.Random(line_count)
    span = max(1, min(10, line_count // 3))
    start = rng.randint(1, max(1, line_count - span))
    end = min(line_count, start + span - 1)
    if (start, end) not in picked:
        picked.append((start, end))

    # 结尾：max(1, n-9)..n
    picked.append((max(1, line_count - 9), line_count))

    # 去重并保证 start <= end
    unique: list[tuple[int, int]] = []
    for a, b in picked:
        if a <= b and (a, b) not in unique:
            unique.append((a, b))
    return unique


def run_gate(ours: str, official: str, corpus: str, verbose: bool) -> int:
    files = collect_files(corpus)
    if not files:
        print(f"[ERROR] 语料为空: {corpus}")
        return 2

    print(f"官方端点 : {official}")
    print(f"我方端点 : {ours}")
    print(f"语料     : {corpus}")
    print(f"文件数   : {len(files)}（每个文件挑最多 3 个区间）")
    print()

    cases = 0
    passed = 0
    failures: list[tuple[str, str, str]] = []

    for path in files:
        rel = os.path.relpath(path, corpus).replace("\\", "/")
        line_count = count_lines(path)
        for start, end in pick_ranges(line_count):
            cases += 1
            tag = f"{rel} [{start}-{end}]"

            rc_o, out_o = run_tool([official, "--lines", f"{start}-{end}", path])
            rc_m, out_m = run_tool(
                [ours, f"--range_start={start}", f"--range_end={end}", path])

            out_o = normalize_paths(out_o, corpus)
            out_m = normalize_paths(out_m, corpus)

            same_bytes = sha(out_o) == sha(out_m)
            same_rc = rc_o == rc_m

            if same_bytes and same_rc:
                passed += 1
                if verbose:
                    print(f"  [PASS] {tag}  ({len(out_o)}B, rc={rc_o})")
                continue

            reasons = []
            if not same_bytes:
                reasons.append(first_diff(out_o, out_m))
            if not same_rc:
                reasons.append(f"退出码不同: official={rc_o} ours={rc_m}")
            failures.append((rel, f"[{start}-{end}]", " ; ".join(reasons)))
            print(f"  [FAIL] {tag}")
            for reason in reasons:
                print(f"         {reason}")

    print()
    rate = 100.0 * passed / cases if cases else 0.0
    print("=" * 74)
    print(f"共 {cases} 个区间用例，一致 {passed} 个，不一致 {len(failures)} 个，"
          f"一致率 {rate:.2f}%")
    print("=" * 74)

    if failures:
        print("\n不一致明细：")
        for rel, rng, reason in failures:
            print(f"  - {rel} {rng}: {reason}")
        return 1
    return 0


def find_indented_range(path: str) -> tuple[int, int] | None:
    """找一个**含缩进代码行**的 1 基闭区间，用于变异测试。

    为什么要专门找：
      第一版变异用 `--indentation_spaces=7`，样本是 `pll_gen.v` 的 1-10 行，
      而那 10 行全是注释块 —— 缩进参数对注释不起作用，输出完全一样，
      于是 [C] 报"没发现差异"。

      **这不是脚本不灵敏，是变异没作用到有效内容上。**
      两件事必须区分开，否则会把"测试设计错误"误判成"门禁失效"。

    做法：跳过注释行（`//`、`*`、`/*`、`*/`、以及纯空行），
          取第一条有前导空白的代码行作为区间起点。
    """
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            lines = fh.readlines()
    except OSError:
        return None
    for index, line in enumerate(lines, start=1):
        stripped = line.strip()
        if not stripped:
            continue
        if stripped.startswith(("//", "*", "/*", "*/", "#")):
            continue
        if not line[:1].isspace():
            continue  # 顶格代码：缩进参数改了也看不出来
        return (index, min(len(lines), index + 5))
    return None


def run_selftest(ours: str, official: str, corpus: str) -> int:
    """变异测试：证明脚本抓得住差异。

    与全文档门禁同一套三证据结构：
      A 官方 vs 官方                    → 采集/归一化确定性
      B 官方 --lines vs 我方 --range_*  → 真正的门禁
      C 只改**我方** range 边界          → 必须发现差异（灵敏度）

    ⚠ C 的变异必须只作用于一侧。两边同时改会"一起变"、一致率仍 100%，
      那是假绿灯（阶段 1 lint 门禁踩过，见 LINT-GATE-REPORT.md §3.4）。
    """
    files = collect_files(corpus)
    if not files:
        print("[ERROR] 语料为空")
        return 2

    # 挑一个样本：必须 (a) 行数够多、(b) 会被格式化、(c) **有含缩进的代码行**
    # （供变异测试用；否则缩进变异看不到效果，详见 find_indented_range 注释）
    sample = None
    bounds = None
    for candidate in files:
        if count_lines(candidate) < 12:
            continue
        ri = find_indented_range(candidate)
        if ri is None:
            continue
        try:
            with open(candidate, "rb") as fh:
                original = fh.read()
        except OSError:
            continue
        _rc, out = run_tool([official, candidate])
        if not out or out == original:
            continue
        sample, bounds = candidate, ri
        break

    if sample is None or bounds is None:
        print("[ERROR] 找不到合适样本"
              "（需行数 ≥12、会被格式化、且含缩进代码行）")
        return 2

    start, end = bounds
    print(f"selftest 样本: {os.path.relpath(sample, corpus)} 区间 {start}-{end}")

    # A 自比
    rc_a1, out_a1 = run_tool([official, "--lines", f"{start}-{end}", sample])
    rc_a2, out_a2 = run_tool([official, "--lines", f"{start}-{end}", sample])
    a_same = sha(out_a1) == sha(out_a2) and rc_a1 == rc_a2
    print(f"  [A] 官方 vs 官方              : "
          f"{'PASS（确定性）' if a_same else 'FAIL（采集不确定！）'}")

    # B 换端点
    rc_b, out_b = run_tool(
        [ours, f"--range_start={start}", f"--range_end={end}", sample])
    b_same = sha(out_a1) == sha(out_b) and rc_a1 == rc_b
    print(f"  [B] 官方 --lines vs 我方      : "
          f"{'PASS（一致）' if b_same else 'FAIL（有差异）'}")

    # C 单边变异：把**我方**的 range 扩到整个文件并加一个必然改变输出的 style 覆盖。
    #
    # 踩过的两个坑（都记下来，避免后人重犯）：
    #   坑 A：第一版用 `range_end + 3` —— 多出来的几行本来就是规范格式，输出一样。
    #   坑 B：改用 `--indentation_spaces=7`，但挑的区间是端口列表，
    #         端口列表的缩进由 `wrap_spaces` / `*_indentation` 控制，
    #         `indentation_spaces` 对它无效 → 仍然一样。
    #         同理 `--line_terminator=CRLF` 在 range 模式下也没造成差异。
    #
    # 教训：**变异必须保证作用在会受该参数影响的代码上**。
    # 最稳的做法是让 range 覆盖整个文件 —— 这样 `indentation_spaces` 一定作用于
    # 所有嵌套块体（`always` / `if` / 模块体），输出必然不同。
    #
    # 注意这仍然是**只改一侧**（官方用默认 2 空格）。
    total_lines = count_lines(sample)
    rc_c, out_c = run_tool(
        [ours, "--range_start=1", f"--range_end={total_lines}",
         "--indentation_spaces=8", sample])
    rc_base, out_base = run_tool(
        [official, "--lines", f"1-{total_lines}", sample])
    c_detected = sha(out_base) != sha(out_c)
    print(f"  [C] 我方(全文件+缩进8) vs 官方(默认2) : "
          f"{'PASS（发现差异，脚本灵敏）' if c_detected else 'FAIL（假绿灯！脚本作废）'}")

    ok = a_same and b_same and c_detected
    print()
    print("selftest 结论:", "PASS" if ok else "FAIL")
    if not c_detected:
        print("  ⚠ 单边变异没被发现 → 对比脚本不灵敏，门禁结论不可信。")
    return 0 if ok else 1


def main() -> int:
    parser = argparse.ArgumentParser(
        description="formatter 局部（选区）格式化门禁：逐字节对比")
    parser.add_argument("--corpus", default=DEFAULT_CORPUS)
    parser.add_argument("--official", default=DEFAULT_OFFICIAL)
    parser.add_argument("--ours", default=None)
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()

    if not os.path.isdir(args.corpus):
        print(f"[ERROR] 语料目录不存在: {args.corpus}")
        return 2
    if not os.path.isfile(args.official):
        print(f"[ERROR] 官方二进制不存在: {args.official}")
        return 2
    ours = find_ours(args.ours)
    if not ours:
        print("[ERROR] 找不到 format_dump.exe"
              "（先构建 --target format_dump，或用 --ours 指定）")
        return 2

    if args.selftest:
        return run_selftest(ours, args.official, args.corpus)
    return run_gate(ours, args.official, args.corpus, args.verbose)


if __name__ == "__main__":
    sys.exit(main())