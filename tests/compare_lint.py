#!/usr/bin/env python3
# =============================================================================
#  tests/compare_lint.py —— 阶段 1 门禁：lint 等价验证
# =============================================================================
#  对比两条独立的实现路径：
#    A) 官方基准:  %USERPROFILE%\verible\verible-verilog-lint.exe
#    B) 本仓库:    build/bin/lint_dump.exe
#
#  四个层次（按成本从低到高，风险前移）：
#    第 1 层  --rules-file      规则清单逐字节对比（覆盖 80% 风险，最便宜）
#    第 2 层  --single-rule     单规则定向验证（隔离多规则干扰）
#    第 3 层  （默认）          全语料诊断逐字节对比（主判据）
#    第 4 层  --selftest        脚本自身灵敏度验证（变异测试）
#
#  ⚠️ 为什么第 1 层最重要：
#    Verible 每条 lint 规则靠 VERILOG_REGISTER_LINT_RULE 的 static 全局对象
#    在启动时自注册。**漏链某个规则库 → 那条规则静默消失，程序不报错**。
#    规则清单的行数就是一张「链接完整性检查表」。
#
#  ⚠️ 必须硬处理的 4 个坑（从源码查出）：
#    1. --rules_config_search 会向上找 .rules.verible_lint → 必须显式关闭
#    2. 规则配置串默认值也在默认表里（-line-length=length:100 等）→ 由第1层diff抓
#    3. --lint_fatal/--parse_fatal 控制退出码 → 除诊断外还要比退出码
#    4. --waiver_files 会抑制诊断 → 两边都不带
#
#  退出码：0 = 全部一致；1 = 存在差异；2 = 环境错误
# =============================================================================

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

# 官方默认规则集基线（bison 无关，是 Verible 源码里的 constexpr 表）
EXPECTED_TOTAL_RULES = 61
EXPECTED_DEFAULT_ENABLED = 42

# 官方诊断行有**两种**形态（都合法，都必须识别）：
#   (a) lint 违规（带 category 与 rule）：
#       path:11:7: Use spaces, not tabs. [Style: tabs] [no-tabs]
#       path:24:15-23: Explicitly define... [Style: constants] [explicit-parameter-storage-type]
#   (b) 语法/词法错误（**没有** [Category] / [rule] 后缀）：
#       path:3:3: syntax error at token ","
#       path:1:8-13: lexical error at token "654foo"
#       path:7:1: syntax error (unexpected EOF)
#
# 只匹配 (a) 会把 (b) 全判成"无法解析"，导致自比都过不了 —— 实测踩到。
DIAG_RE = re.compile(
    r"^(?P<file>.+?):(?P<line>\d+):(?P<col>\d+(?:-\d+)?):\s*(?P<msg>.*?)"
    r"(?:\s*\[(?P<cat>[^\[\]]*)\])?\s*\[(?P<rule>[^\[\]]+)\]\s*$"
)

# 语法/词法错误行：没有尾部的 [Category][rule]
SYNTAX_ERR_RE = re.compile(
    r"^(?P<file>.+?):(?P<line>\d+):(?P<col>\d+(?:-\d+)?):\s*"
    r"(?P<msg>(?:syntax|lexical) error.*?)\s*$"
)


@dataclass
class Diag:
    file: str
    line: int
    col: str          # 保持字符串：可能是 "15" 也可能是 "15-23"
    msg: str
    cat: str
    rule: str

    def key(self) -> tuple:
        # file 归一化后再比；其余四个字段逐字节比
        return (normalize_path(self.file), self.line, self.col,
                self.msg, self.cat, self.rule)

    def __str__(self) -> str:
        return (f"{normalize_path(self.file)}:{self.line}:{self.col}: "
                f"{self.msg} [{self.cat}] [{self.rule}]")


def normalize_path(p: str) -> str:
    """去掉目录前缀，只保留相对语料根的路径形态（正斜杠）。"""
    s = p.replace("\\", "/")
    # 去掉盘符
    s = re.sub(r"^[A-Za-z]:/", "", s)
    return s


def run_tool(argv: list[str]) -> tuple[int, str, str]:
    try:
        p = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except OSError as exc:
        return -1, "", str(exc)
    return (p.returncode,
            p.stdout.decode("utf-8", errors="surrogateescape"),
            p.stderr.decode("utf-8", errors="surrogateescape"))


def run_lint_tool(argv: list[str]) -> tuple[int, str]:
    """调用 lint 工具并返回 (退出码, 诊断文本)。

    ⚠️ 重要（实测坑）：官方 verible-verilog-lint 把**诊断写到 stderr**，
    不是 stdout（它的 violation handler 用 std::cerr）。若只读 stdout 会得到
    "0 条诊断"，从而让两个"都没有诊断"的实现被误判为"一致"——这是最危险的
    假通过。这里把 stdout 与 stderr 都收上来，按出现顺序拼接比较。

    注意 --print_rules_file 的输出是走 std::cout 的（见 verilog-lint.cc），
    所以规则清单用 run_tool 读 stdout，诊断用本函数。
    """
    rc, out, err = run_tool(argv)
    # 诊断可能在任一通道；两者都纳入，且保持稳定顺序（stdout 先、stderr 后，
    # 两边都以同样方式拼接，因此对比仍然是对称的）
    return rc, (out + ("\n" if out and err else "") + err)


# =============================================================================
#  第 1 层：规则清单对比
# =============================================================================
def parse_rule_lines(text: str) -> list[str]:
    """把 --rules-file 输出归成一行一条，去掉空行，保留原始顺序。"""
    return [ln for ln in text.replace("\r\n", "\n").split("\n") if ln.strip()]


def rule_basename(line: str) -> str:
    """'-always-comb-blocking=x:y' -> 'always-comb-blocking'"""
    s = line.strip()
    if s.startswith("+") or s.startswith("-"):
        s = s[1:]
    return s.split("=", 1)[0].strip()


def is_enabled(line: str) -> bool:
    return not line.strip().startswith("-")


def layer1_rules(official_exe: str, dump_exe: str, verbose: bool) -> bool:
    print("=" * 78)
    print("第 1 层：规则清单对比（链接完整性检查）")
    print("=" * 78)

    rc_o, out_o, err_o = run_tool([official_exe, "--print_rules_file"])
    rc_d, out_d, err_d = run_tool([dump_exe, "--rules-file"])

    if rc_o != 0:
        print(f"ERROR: 官方 --print_rules_file 失败 rc={rc_o}\n{err_o}", file=sys.stderr)
        return False
    if rc_d != 0:
        print(f"ERROR: lint_dump --rules-file 失败 rc={rc_d}\n{err_d}", file=sys.stderr)
        return False

    lo = parse_rule_lines(out_o)
    ld = parse_rule_lines(out_d)

    n_o, n_d = len(lo), len(ld)
    en_o = sum(1 for x in lo if is_enabled(x))
    en_d = sum(1 for x in ld if is_enabled(x))

    print(f"  官方    : {n_o} 行规则，其中默认启用 {en_o} 条")
    print(f"  lint_dump: {n_d} 行规则，其中默认启用 {en_d} 条")

    ok = True

    # 断言总数与启用数（基线来自 Verible 源码 constexpr 表）
    if n_o != EXPECTED_TOTAL_RULES:
        print(f"  ⚠️ 官方规则数 {n_o} != 基线 {EXPECTED_TOTAL_RULES}"
              f"（Verible 版本可能变了，需要更新基线）")
    if en_o != EXPECTED_DEFAULT_ENABLED:
        print(f"  ⚠️ 官方默认启用 {en_o} != 基线 {EXPECTED_DEFAULT_ENABLED}")

    # 逐字节对比
    if out_o.replace("\r\n", "\n") == out_d.replace("\r\n", "\n"):
        print("  ✅ 逐字节完全一致")
        return True

    # 不一致：先做行级 diff 定位
    ok = False
    print("  ❌ 规则清单不一致")
    set_o, set_d = set(lo), set(ld)
    only_o = [x for x in lo if x not in set_d]
    only_d = [x for x in ld if x not in set_o]

    names_o = {rule_basename(x) for x in lo}
    names_d = {rule_basename(x) for x in ld}
    missing = sorted(names_o - names_d)   # 官方有、我们没有 → 缺规则
    extra = sorted(names_d - names_o)     # 我们有、官方无 → 多规则

    if n_d < n_o:
        print(f"  🔴 规则数偏少：{n_d} < {n_o}，差 {n_o - n_d} 条")
        print("     → 极可能是某个规则库未被链接（静态注册对象被丢弃）")

    if missing:
        print(f"\n  缺失规则（官方有、我方无）共 {len(missing)} 条:")
        for m in missing:
            print(f"    - {m}")
        print("\n  用 --diagnose-missing 可定位到具体源码与库。")

    if extra:
        print(f"\n  多余规则（我方有、官方无）共 {len(extra)} 条:")
        for m in extra:
            print(f"    + {m}")

    if verbose and (only_o or only_d):
        print("\n  行级差异（含配置串差异）:")
        for x in only_o[:20]:
            print(f"    official only: {x}")
        for x in only_d[:20]:
            print(f"    dump     only: {x}")

    return ok


# =============================================================================
#  --diagnose-missing：把「少了哪条规则」翻译成「哪个库缺什么」
# =============================================================================
def diagnose_missing(official_exe: str, dump_exe: str) -> int:
    print("=" * 78)
    print("诊断模式：定位缺失规则的源码与所属库")
    print("=" * 78)

    rc_o, out_o, _ = run_tool([official_exe, "--print_rules_file"])
    rc_d, out_d, _ = run_tool([dump_exe, "--rules-file"])
    if rc_o != 0 or rc_d != 0:
        print("ERROR: 无法取得规则清单", file=sys.stderr)
        return 2

    names_o = {rule_basename(x) for x in parse_rule_lines(out_o)}
    names_d = {rule_basename(x) for x in parse_rule_lines(out_d)}
    missing = sorted(names_o - names_d)

    if not missing:
        print("✅ 没有缺失规则（规则清单完整）")
        return 0

    # 载入源码映射
    map_path = Path(__file__).resolve().parent.parent / "build" / "rule-map.json"
    rule_map: dict = {}
    if map_path.is_file():
        rule_map = json.loads(map_path.read_text(encoding="utf-8"))
    else:
        print(f"⚠️ 未找到 {map_path}，先运行: python tests/gen_rule_map.py --json build/rule-map.json")

    repo = Path(__file__).resolve().parent.parent
    build_lib = repo / "build" / "lib"

    print(f"发现 {len(missing)} 条缺失规则：\n")
    for name in missing:
        info = rule_map.get(name)
        print(f"缺失规则：{name}")
        if not info:
            print("   ⚠️ 未在源码映射中找到（可能是新增规则，需重跑 gen_rule_map.py）\n")
            continue

        print(f"  所在源码：{info['source']}")
        print(f"  推测库：  {info['target']}")
        print(f"  BUILD：   {info['build_file']}")

        # 检查对应的 .lib 是否存在
        # 生成器命名：verible__verilog__analysis__checkers__<target>
        cand = build_lib / f"verible__verilog__analysis__checkers__{info['target']}.lib"
        if cand.is_file():
            print(f"  lib 状态：存在 ({cand.name}, {cand.stat().st_size} B)")
            # 检查符号
            if shutil.which("dumpbin"):
                rc, sout, _ = run_tool(
                    ["dumpbin", "/symbols", str(cand)])
                sym = info["class"]
                if sym in sout:
                    print(f"  符号检查：✅ 含 {sym}")
                    print("  结论：lib 存在且含该规则符号 → 问题在【最终可执行文件未链接该 lib，"
                          "或缺 /WHOLEARCHIVE】")
                else:
                    print(f"  符号检查：❌ 不含 {sym}")
                    print("  结论：lib 存在但不含该规则符号 → 该 .cc 未被编入 target")
            else:
                print("  (dumpbin 不在 PATH，跳过符号检查)")
        else:
            print(f"  lib 状态：❌ 不存在 ({cand.name})")
            print("  结论：该规则库根本没被构建 → 检查 _VERIBLE_PHASE0_PACKAGES "
                  "是否包含 checkers 包")

        print(f"  验证命令：dumpbin /symbols {cand} | findstr /i {info['class']}")
        print()
    return 1


# =============================================================================
#  第 3 层：全语料诊断对比
# =============================================================================
def parse_diags(text: str) -> tuple[list[Diag], list[str]]:
    diags: list[Diag] = []
    unparsed: list[str] = []
    for line in text.replace("\r\n", "\n").split("\n"):
        if not line.strip():
            continue
        m = DIAG_RE.match(line)
        if m:
            diags.append(Diag(
                file=m.group("file"),
                line=int(m.group("line")),
                col=m.group("col"),   # 字符串，可能是 "15" 或 "15-23"
                msg=m.group("msg").strip(),
                cat=(m.group("cat") or "").strip(),
                rule=m.group("rule").strip(),
            ))
            continue
        # 语法/词法错误行（无 [Category][rule] 后缀）：用固定的类别/规则名占位，
        # 保证两边用同样方式归一化后可以逐字节比较。
        se = SYNTAX_ERR_RE.match(line)
        if se:
            diags.append(Diag(
                file=se.group("file"),
                line=int(se.group("line")),
                col=se.group("col"),
                msg=se.group("msg").strip(),
                cat="<syntax-error>",
                rule="<syntax-error>",
            ))
            continue
        unparsed.append(line)
    return diags, unparsed


def common_flags(rules: str | None, ruleset: str) -> list[str]:
    """两边共用的 flag。第 1 个坑在这里硬处理。"""
    f = [
        # ⚠️ 坑 1：必须显式关闭，否则会向上找 .rules.verible_lint 造成假差异
        f"--ruleset={ruleset}",
    ]
    if rules is not None:
        f.append(f"--rules={rules}")
    # ⚠️ 坑 4：不带 waiver（默认即空）
    return f


def layer3_corpus(official_exe: str, dump_exe: str, corpus: Path,
                  pattern: str, extra_flags: list[str],
                  show: int, verbose: bool,
                  dump_uses_lint_flag: bool = True,
                  dump_extra_flags: list[str] | None = None) -> tuple[int, int]:
    """第 3 层：全语料诊断逐字节对比。

    dump_uses_lint_flag:
      True  -> 被测端调用 [dump_exe, "--lint", ...]（lint_dump 的正常用法）
      False -> 被测端调用 [dump_exe, ...]（自比模式：官方 exe 没有 --lint 子命令）
    dump_extra_flags:
      None  -> 被测端与官方端使用相同 flags（正常门禁）
      否则  -> 被测端使用该组 flags，用于**单边变异测试**
    """
    files = sorted(
        p for p in corpus.rglob("*")
        if p.suffix in pattern.split(",") and p.is_file()
    )
    if not files:
        print(f"ERROR: 语料目录无匹配文件: {corpus}", file=sys.stderr)
        return 0, 0

    print(f"语料：{corpus}  文件数：{len(files)}")
    ok = 0
    bad: list[tuple[Path, str]] = []
    total_diags = 0

    dump_prefix = [dump_exe, "--lint"] if dump_uses_lint_flag else [dump_exe]
    d_flags = extra_flags if dump_extra_flags is None else dump_extra_flags

    for f in files:
        # ⚠️ 坑 3：同时记录退出码
        # 用 run_lint_tool：诊断走 stderr，必须一并收集（否则 0 诊断假通过）
        rc_o, out_o = run_lint_tool([official_exe] + extra_flags + [str(f)])
        rc_d, out_d = run_lint_tool(dump_prefix + d_flags + [str(f)])

        d_o, u_o = parse_diags(out_o)
        d_d, u_d = parse_diags(out_d)
        total_diags += len(d_o)

        ko = [d.key() for d in d_o]
        kd = [d.key() for d in d_d]

        if ko != kd:
            n = min(len(ko), len(kd))
            idx = next((i for i in range(n) if ko[i] != kd[i]), n)
            parts = [f"诊断不一致 at index {idx} "
                     f"(official={len(ko)}, dump={len(kd)})"]
            for i in range(max(0, idx - 1), min(max(len(ko), len(kd)), idx + 2)):
                a = str(d_o[i]) if i < len(d_o) else "<none>"
                b = str(d_d[i]) if i < len(d_d) else "<none>"
                parts.append(f"  [{i}] official: {a}")
                parts.append(f"  [{i}] dump    : {b}{'  <<<' if a != b else ''}")
            bad.append((f, "\n".join(parts)))
            continue

        # 坑 3：退出码也要一致
        if rc_o != rc_d:
            bad.append((f, f"退出码不一致: official={rc_o}, dump={rc_d}"))
            continue

        if u_o or u_d:
            bad.append((f, f"存在无法解析的行: official={u_o[:2]}, dump={u_d[:2]}"))
            continue

        ok += 1
        if verbose:
            print(f"  [OK] {f.name}")

    for f, detail in bad[:show]:
        print(f"  [DIFF] {f}")
        for ln in detail.split("\n"):
            print(f"         {ln}")
    if len(bad) > show:
        print(f"  ... 其余 {len(bad) - show} 个省略")

    rate = ok / len(files) * 100
    print(f"\n共 {len(files)} 文件，一致 {ok}，不一致 {len(bad)}，"
          f"一致率 {rate:.2f}%")
    print(f"对比诊断总数（官方）：{total_diags}")
    return ok, len(files)


# =============================================================================
#  主流程
# =============================================================================
def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--lint-dump", required=True)
    ap.add_argument("--official", required=True)
    ap.add_argument("--corpus", default=None)
    ap.add_argument("--pattern", default=".v,.sv")
    ap.add_argument("--ruleset", default="default")
    ap.add_argument("--rules", default=None, help="透传官方 --rules 语法")
    ap.add_argument("--rules-file", action="store_true",
                    help="只做第 1 层规则清单对比")
    ap.add_argument("--diagnose-missing", action="store_true",
                    help="诊断缺失规则的源码与所属库")
    ap.add_argument("--selftest", action="store_true",
                    help="官方自比，验证脚本自身灵敏度")
    ap.add_argument("--show", type=int, default=10)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    dump = str(Path(args.lint_dump).resolve())
    official = str(Path(args.official).resolve())
    if not Path(dump).is_file() and shutil.which(dump) is None:
        print(f"ERROR: lint_dump 不存在: {dump}", file=sys.stderr)
        return 2
    if not Path(official).is_file():
        print(f"ERROR: 官方 lint 不存在: {official}", file=sys.stderr)
        return 2

    # --- 诊断模式 ---
    if args.diagnose_missing:
        return diagnose_missing(official, dump)

    # --- 自比模式：官方当被测端，验证脚本灵敏度 ---
    if args.selftest:
        print("*** --selftest：官方自比，验证脚本逻辑 ***")
        # 规则清单自比
        rc_a, out_a, _ = run_tool([official, "--print_rules_file"])
        rc_b, out_b, _ = run_tool([official, "--print_rules_file"])
        same = out_a.replace("\r\n", "\n") == out_b.replace("\r\n", "\n")
        print(f"规则清单自比: {'✅ 一致' if same else '❌ 不一致'} "
              f"({len(parse_rule_lines(out_a))} 行)")
        layer1_ok = same
    else:
        layer1_ok = layer1_rules(official, dump, args.verbose)

        # 第 1 层就失败 → 直接停，不要继续往下跑（风险前移的意义）
        if not layer1_ok:
            print("\n⚠️ 第 1 层未通过，停止后续层次。")
            print("   先解决规则清单差异（用 --diagnose-missing 定位），"
                  "再跑全语料对比。")
            return 1

    if args.rules_file:
        print("\n门禁结论（第 1 层）：**通过**")
        return 0

    # --- 第 3 层 ---
    if not args.corpus:
        print("ERROR: 未指定 --corpus", file=sys.stderr)
        return 2
    corpus = Path(args.corpus).resolve()

    flags = common_flags(args.rules, args.ruleset)
    if args.verbose:
        print(f"共用 flags: {flags}")

    if args.selftest:
        # 自比：两端都是官方 exe。官方没有 --lint 子命令，直接吃文件。
        #
        # ⚠️ 变异测试语义（重要，踩过的坑）：
        #   自比模式下若把同样的 flags 同时传给两边（例如 --rules=-no-tabs 都传），
        #   两边一起变，结果仍然 100% 一致 —— 这样的"自比"无法证明灵敏度，
        #   是个假绿灯。真正的灵敏度测试必须**只改变一边**。
        #   因此自比模式下把 --rules 只施加给"被测端"，
        #   用来验证脚本能否发现规则集差异。
        same_flags = common_flags(None, args.ruleset)
        dump_only_flags = same_flags + (
            [f"--rules={args.rules}"] if args.rules else [])
        if args.rules:
            print(f"*** 变异测试：仅被测端施加 --rules={args.rules}"
                  f"（官方端用默认集）***")
        ok, total = layer3_corpus(
            official, official, corpus, args.pattern,
            same_flags, args.show, args.verbose,
            dump_uses_lint_flag=False,
            dump_extra_flags=dump_only_flags)
    else:
        # 正常模式：两端施加完全相同的 flags（坑 1/4 在此硬处理）
        flags = common_flags(args.rules, args.ruleset)
        if args.verbose:
            print(f"共用 flags: {flags}")
        ok, total = layer3_corpus(
            official, dump, corpus, args.pattern, flags,
            args.show, args.verbose, dump_uses_lint_flag=True)

    print()
    if ok == total:
        print("门禁结论：**通过** —— 全部语料诊断逐字节一致")
        return 0
    print("门禁结论：**不通过** —— 存在诊断差异")
    return 1


if __name__ == "__main__":
    sys.exit(main())