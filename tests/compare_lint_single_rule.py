#!/usr/bin/env python3
# =============================================================================
#  tests/compare_lint_single_rule.py —— 第 2 层：单规则定向验证
# =============================================================================
#  目的：隔离多规则干扰。一条 no-tabs 的差异不会混在 20 条其它诊断里，
#        定位到具体是哪条规则行为不对。
#
#  做法：对每条规则单独跑
#        --ruleset=none --rules=+<rule>
#  然后逐文件比对诊断。因为规则集被缩到单条，任何差异都直接归因到该规则。
#
#  为什么需要这一层（第 3 层已经 100% 了）：
#    第 3 层全语料 100% 一致，只说明"在默认规则集下诊断相同"。
#    如果两条规则互相抵消（A 多报一条、B 少报一条），第 3 层看不出来。
#    单规则验证能排除这种"抵消式互补错误"。
#
#  用法：
#    python tests/compare_lint_single_rule.py --lint-dump ... --official ...
#    python tests/compare_lint_single_rule.py ... --rules-from-corpus   # 只测语料里出现的规则
#
#  退出码：0 = 全部规则一致；1 = 有差异；2 = 环境错误
# =============================================================================

from __future__ import annotations

import argparse
import importlib.util
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CORPUS = REPO / "tests" / "gate-corpus"


def load_compare_module():
    """复用 compare_lint.py 的诊断解析与归一化逻辑，避免两处维护。"""
    spec = importlib.util.spec_from_file_location(
        "compare_lint", REPO / "tests" / "compare_lint.py")
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def get_all_rules(official: str, cl) -> list[str]:
    """从官方 --print_rules_file 拿全部 61 条规则名。"""
    rc, out, _ = cl.run_tool([official, "--print_rules_file"])
    if rc != 0:
        return []
    names = []
    for line in cl.parse_rule_lines(out):
        names.append(cl.rule_basename(line))
    return names


def build_rule_file_map(official: str, cl,
                        files: list[Path]) -> dict[str, set[Path]]:
    """一次全语料扫描，建立 规则名 -> 产生过该规则诊断的文件集合。

    这是性能关键：单规则验证如果对 61 条规则 × 102 个文件全跑，
    就是 6000+ 次进程启动（实测 > 10 分钟）。
    但绝大多数规则在绝大多数文件上没有诊断 —— 那些组合两边都是"空"，
    比对价值极低。

    因此只对「官方在该文件上产生过该规则诊断」的组合做单规则比对，
    这样既保留"隔离多规则干扰"的目的，又把调用数降到百次量级。
    """
    mapping: dict[str, set[Path]] = {}
    for f in files:
        rc, text = cl.run_lint_tool([official, str(f)])
        diags, _ = cl.parse_diags(text)
        for d in diags:
            if d.rule and not d.rule.startswith("<"):
                mapping.setdefault(d.rule, set()).add(f)
    return mapping


def compare_one_rule(rule: str, files: list[Path], official: str,
                     dump: str, cl) -> list[tuple[Path, str]]:
    """单规则下逐文件比对；返回差异列表。"""
    problems: list[tuple[Path, str]] = []
    for f in files:
        off_args = [official, "--ruleset=none", f"--rules=+{rule}", str(f)]
        dump_args = [dump, "--lint", "--ruleset=none", f"--rules=+{rule}", str(f)]

        rc_o, out_o = cl.run_lint_tool(off_args)
        rc_d, out_d = cl.run_lint_tool(dump_args)

        d_o, _ = cl.parse_diags(out_o)
        d_d, _ = cl.parse_diags(out_d)

        ko = [d.key() for d in d_o]
        kd = [d.key() for d in d_d]

        if ko != kd:
            n = min(len(ko), len(kd))
            idx = next((i for i in range(n) if ko[i] != kd[i]), n)
            msg = [f"诊断不一致 at index {idx} "
                   f"(official={len(ko)}, dump={len(kd)})"]
            for i in range(max(0, idx - 1), min(max(len(ko), len(kd)), idx + 2)):
                a = str(d_o[i]) if i < len(d_o) else "<none>"
                b = str(d_d[i]) if i < len(d_d) else "<none>"
                msg.append(f"  [{i}] official: {a}")
                msg.append(f"  [{i}] dump    : {b}{'  <<<' if a != b else ''}")
            problems.append((f, "\n".join(msg)))
        elif rc_o != rc_d:
            problems.append((f, f"退出码不一致: official={rc_o}, dump={rc_d}"))
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--lint-dump", required=True)
    ap.add_argument("--official", required=True)
    ap.add_argument("--corpus", default=str(CORPUS))
    ap.add_argument("--pattern", default=".v,.sv")
    ap.add_argument("--rules-from-corpus", action="store_true",
                    help="只测语料里实际出现过的规则（更快）")
    ap.add_argument("--show", type=int, default=5)
    args = ap.parse_args()

    dump = str(Path(args.lint_dump).resolve())
    official = str(Path(args.official).resolve())
    if not Path(dump).is_file():
        print(f"ERROR: lint_dump 不存在: {dump}", file=sys.stderr)
        return 2
    if not Path(official).is_file():
        print(f"ERROR: 官方 lint 不存在: {official}", file=sys.stderr)
        return 2

    cl = load_compare_module()

    all_rules = get_all_rules(official, cl)
    if not all_rules:
        print("ERROR: 无法从官方取得规则清单", file=sys.stderr)
        return 2

    corpus = Path(args.corpus).resolve()
    patterns = tuple(args.pattern.split(","))
    files = sorted(p for p in corpus.rglob("*")
                   if p.suffix in patterns and p.is_file())
    if not files:
        print(f"ERROR: 语料目录无匹配文件: {corpus}", file=sys.stderr)
        return 2

    rules = all_rules
    rule_files: dict[str, set[Path]] = {}

    if args.rules_from_corpus:
        # 单次扫描建立 规则 -> 文件 映射（性能关键，见 build_rule_file_map 文档）
        print("扫描语料，建立 规则→文件 映射 ...")
        rule_files = build_rule_file_map(official, cl, files)
        rules = [r for r in all_rules if r in rule_files]
        total_pairs = sum(len(v) for v in rule_files.values())
        print(f"  {len(rules)} 条规则在 {total_pairs} 个 (规则,文件) 组合上产生过诊断")

    print("=" * 78)
    print("第 2 层：单规则定向验证")
    print("=" * 78)
    print(f"语料：{corpus}  文件数：{len(files)}")
    print(f"待测规则：{len(rules)} 条"
          f"{'（语料中出现过的）' if args.rules_from_corpus else '（全部）'}")
    print()

    ok_rules = 0
    bad_rules: list[tuple[str, list]] = []

    for i, rule in enumerate(rules, 1):
        # 只对该规则产生过诊断的文件做定向验证（-from-corpus 模式）；
        # 全量模式仍扫全部文件。
        targets = sorted(rule_files.get(rule, set())) if rule_files else files
        problems = compare_one_rule(rule, targets, official, dump, cl)
        if problems:
            bad_rules.append((rule, problems))
            print(f"  [{i}/{len(rules)}] ❌ {rule}  "
                  f"({len(problems)}/{len(targets)} 个文件不一致)")
        else:
            ok_rules += 1
            print(f"  [{i}/{len(rules)}] ✅ {rule}  ({len(targets)} 个文件)")

    print()
    print("=" * 78)
    rate = ok_rules / len(rules) * 100 if rules else 0
    print(f"共 {len(rules)} 条规则，一致 {ok_rules}，不一致 {len(bad_rules)}，"
          f"一致率 {rate:.2f}%")

    if bad_rules:
        print("\n不一致明细：")
        for rule, problems in bad_rules[:args.show]:
            print(f"\n  规则 {rule}:")
            for f, detail in problems[:3]:
                print(f"    {f.name}")
                for ln in detail.split("\n"):
                    print(f"      {ln}")
        print("\n门禁结论（第 2 层）：**不通过**")
        return 1

    print("\n门禁结论（第 2 层）：**通过** —— 每条规则单独验证均一致")
    return 0


if __name__ == "__main__":
    sys.exit(main())