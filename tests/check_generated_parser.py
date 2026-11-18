#!/usr/bin/env python3
# =============================================================================
#  tests/check_generated_parser.py —— flex/bison 生成物回归测试（双保险）
# =============================================================================
#  这个脚本与 flexbison-gen 的 CMake/CTest 断言**相互独立**，故意重复校验同一组
#  数字（硬编码在这里），构成"双保险"：
#
#    * 任一侧的 bison/flex 版本漂移，都会被独立抓到；
#    * 如果将来有意升级 bison，这些数字需要人工确认后更新 —— 这个"摩擦"是
#      刻意保留的，因为生成物变化必须被显式审查，不能悄悄漂过去。
#
#  为什么需要这个脚本（背景）：
#    G6a（std::move 补丁）的正确性由**编译器**保证：YYSTYPE = UniquePtr，
#    unique_ptr 的拷贝赋值已被 delete，漏打补丁必然编译失败（见下方 C2280 说明）。
#    所以 G6a 不需要运行时断言。
#
#    但 G6b（RecordSyntaxError 补丁）不同：漏打照样能编译，只是错误恢复能力
#    **静默丢失**。因此 G6b 必须靠断言防护，且必须防止 "0 == 0" 假通过
#    （两边都没匹配到时，朴素的"数量相等"断言会误判为通过）。
#
#  用法：
#    python tests/check_generated_parser.py --generated build/.../verilog-final.tab.cc
#    python tests/check_generated_parser.py --generated ... --raw-cc .../verilog.tab.cc
#      加 --raw-cc 时会额外校验"补丁前"的锚点计数，更强。
# =============================================================================

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# -----------------------------------------------------------------------------
# 期望计数（bison 3.8.2 + 当前 verilog.y 实测值）
# -----------------------------------------------------------------------------
# 这些数字是我用 win_bison 3.8.2 真实生成 verilog.tab.cc 后统计得到的。
# 升级 bison 后若这些数字变化，**不要直接改这里**，先确认：
#   1. 语法含义是否仍然等价（人工 diff 新旧 verilog.tab.cc 的相关片段）
#   2. 官方 release 二进制的错误行为是否一致（跑门禁对比）
# 确认无误后才能更新本表。
EXPECTED_ANCHORS = {
    "= yylval;": 2,
    "= yyval;": 1,
    "(= yyval_default)": 1,
    "yyval = yyvsp[1-yylen];": 1,
    "++yynerrs;": 2,
}

# 补丁后 verilog-final.tab.cc 中应当出现的形态
EXPECTED_PATCHED = {
    "std::move(yylval)": 2,
    "std::move(yyval)": 1,
    "std::move(yyval_default)": 1,
    "param->RecordSyntaxError(yylval);": 2,
}


class Checker:
    def __init__(self) -> None:
        self.failures: list[str] = []
        self.passes: list[str] = []

    def check(self, ok: bool, label: str, detail: str = "") -> None:
        if ok:
            self.passes.append(label)
            print(f"  [PASS] {label}")
        else:
            self.failures.append(f"{label}: {detail}")
            print(f"  [FAIL] {label}")
            if detail:
                print(f"         {detail}")

    def check_count(self, text: str, pattern: str, expected: int,
                    label: str, *, allow_ge: bool = False) -> None:
        # 用正则字面匹配（转义），计数所有出现
        n = len(re.findall(re.escape(pattern), text))
        if allow_ge:
            self.check(n >= expected, label,
                       f"expected >= {expected}, got {n}  (pattern: {pattern!r})")
        else:
            self.check(n == expected, label,
                       f"expected == {expected}, got {n}  (pattern: {pattern!r})")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--generated", required=True,
                    help="补丁后的 verilog-final.tab.cc 路径")
    ap.add_argument("--raw-cc", default=None,
                    help="补丁前的 verilog.tab.cc（可选，用于校验锚点计数）")
    ap.add_argument("--token-enum", default=None,
                    help="G4 产物 verilog-token-enum.h（可选）")
    ap.add_argument("--parse-interface", default=None,
                    help="G5 产物 verilog-parse-interface.h（可选）")
    ap.add_argument("--yy-cc", default=None,
                    help="flex 产物 verilog.yy.cc（可选，校验存在且非空）")
    args = ap.parse_args()

    c = Checker()

    gen_path = Path(args.generated)
    if not gen_path.is_file():
        print(f"ERROR: 找不到 {gen_path}", file=sys.stderr)
        return 2
    gen = gen_path.read_text(encoding="utf-8", errors="surrogateescape")

    # ---- 1. 补丁后文件的存在性检查 ----
    print("\n[1] 补丁后文件 (verilog-final.tab.cc)")
    c.check(len(gen) > 100_000, "文件大小合理 (>100KB)",
            f"got {len(gen)} bytes")

    # ---- 2. G6a：std::move 补丁是否全部到位 ----
    print("\n[2] G6a std_move_parser_symbols")
    for pat, n in EXPECTED_PATCHED.items():
        if "Record" in pat:
            continue
        c.check_count(gen, pat, n, f"出现 {pat}")

    # 关键：不能有未处理的拷贝赋值残留
    # 注意 "= yylval;" 若前面紧跟 std::move( 则不算残留
    residual_yy = len(re.findall(r"(?<!move\()= yylval;", gen))
    residual_yyval = len(re.findall(r"(?<!move\()= yyval;", gen))
    c.check(residual_yy == 0, "无残留 '= yylval;'", f"发现 {residual_yy} 处")
    c.check(residual_yyval == 0, "无残留 '= yyval;'", f"发现 {residual_yyval} 处")
    residual_default = len(re.findall(r"\(= yyval_default\)", gen))
    c.check(residual_default == 0, "无残留 '(= yyval_default)'",
            f"发现 {residual_default} 处")

    # yyval = yyvsp[1-yylen]; 应被注释掉
    c.check(re.search(r"//\s*yyval = yyvsp\[1-yylen\];", gen) is not None,
            "yyval = yyvsp[1-yylen]; 已被注释")

    # ---- 3. G6b：RecordSyntaxError 是否插入且数量正确 ----
    print("\n[3] G6b record_recovered_syntax_errors")
    n_nerrs = len(re.findall(re.escape("++yynerrs;"), gen))
    n_record = len(re.findall(re.escape("param->RecordSyntaxError(yylval);"), gen))

    # 关键防护 1：不能是 0（否则 0 == 0 假通过）
    c.check(n_nerrs > 0, "++yynerrs; 数量 > 0",
            f"got {n_nerrs}（为 0 说明 bison 输出格式已变化，G6b 无法生效）")
    c.check(n_record > 0, "RecordSyntaxError 调用数 > 0",
            f"got {n_record}")
    # 关键防护 2：两者必须相等
    c.check(n_nerrs == n_record, "yynerrs 与 RecordSyntaxError 数量一致",
            f"++yynerrs;={n_nerrs} vs RecordSyntaxError={n_record}")
    # 与硬编码期望值比对（bison 3.8.2）
    c.check(n_nerrs == EXPECTED_ANCHORS["++yynerrs;"],
            f"++yynerrs; == {EXPECTED_ANCHORS['++yynerrs;']}（bison 3.8.2 基线）",
            f"got {n_nerrs}；若确认是 bison 升级所致，需人工审查后更新 EXPECTED_ANCHORS")
    c.check(n_record == EXPECTED_ANCHORS["++yynerrs;"],
            f"RecordSyntaxError == {EXPECTED_ANCHORS['++yynerrs;']}",
            f"got {n_record}")

    # 每条 RecordSyntaxError 必须紧跟 ++yynerrs; 之后（同一恢复点）
    ok_order = re.search(
        r"\+\+yynerrs;\s*\n(.*\n){0,5}.*param->RecordSyntaxError\(yylval\);", gen)
    c.check(ok_order is not None, "RecordSyntaxError 紧跟 ++yynerrs; 之后",
            "未找到匹配的相邻模式")

    # ---- 4. 锚点计数校验（如果在补丁前文件上） ----
    if args.raw_cc:
        raw_path = Path(args.raw_cc)
        if not raw_path.is_file():
            print(f"ERROR: 找不到 {raw_path}", file=sys.stderr)
            return 2
        raw = raw_path.read_text(encoding="utf-8", errors="surrogateescape")
        print("\n[4] 补丁前锚点计数 (verilog.tab.cc)")
        for pat, n in EXPECTED_ANCHORS.items():
            c.check_count(raw, pat, n, f"锚点 {pat!r}")

    # ---- 5. 其它生成物 ----
    if any([args.token_enum, args.parse_interface, args.yy_cc]):
        print("\n[5] 其它生成物")
    if args.token_enum:
        p = Path(args.token_enum)
        ok = p.is_file()
        txt = p.read_text(encoding="utf-8", errors="surrogateescape") if ok else ""
        c.check(ok and "VERILOG_TOKENTYPE" in txt, "G4: 含 VERILOG_TOKENTYPE")
        c.check(ok and "VERIBLE_VERILOG_TOKEN_ENUM_H_" in txt,
                "G4: 有独立 include guard")
        c.check(ok and "enum verilog_tokentype" in txt,
                "G4: 含枚举定义")
    if args.parse_interface:
        p = Path(args.parse_interface)
        ok = p.is_file()
        txt = p.read_text(encoding="utf-8", errors="surrogateescape") if ok else ""
        c.check(ok, "G5: 文件存在")
        c.check("enum verilog_tokentype" not in txt,
                "G5: 已剥离枚举定义段")
    if args.yy_cc:
        p = Path(args.yy_cc)
        ok = p.is_file()
        size = p.stat().st_size if ok else 0
        c.check(ok and size > 10_000, "flex: verilog.yy.cc 存在且非空",
                f"{size} bytes")

    # ---- 汇总 ----
    print("\n" + "=" * 70)
    print(f"通过 {len(c.passes)} 项，失败 {len(c.failures)} 项")
    if c.failures:
        print("\n失败明细:")
        for f in c.failures:
            print(f"  - {f}")
        print("\n结论: **失败** —— 生成物与预期不符，先用官方基准跑门禁对比确认影响")
        return 1
    print("结论: **通过** —— 生成物结构与 bison 3.8.2 基线一致")
    return 0


if __name__ == "__main__":
    sys.exit(main())