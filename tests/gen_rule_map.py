#!/usr/bin/env python3
# =============================================================================
#  tests/gen_rule_map.py —— 生成「lint 规则名 -> 源码 -> 推测库 target」映射表
# =============================================================================
#  背景（阶段 1 头号风险）：
#    Verible 的每条 lint 规则靠 VERILOG_REGISTER_LINT_RULE 宏产生的 static
#    全局对象在程序启动时自注册。**如果某条规则所在的静态库没被链接进来，
#    那条规则会静默消失 —— 程序正常、不报错，只是少一条规则。**
#
#    因此当 lint_dump 的规则清单与官方对不上时，第一件事是回答：
#      「缺的这条规则，代码在哪个 .cc？应该由哪个 target 提供？」
#    本脚本就是把这条映射关系从源码里现场构建出来，避免每次人工再挖一遍。
#
#  为什么不硬编码 58 条：
#    源码增删规则时映射要自动跟上。这里从
#      - checkers/*.cc 里的 VERILOG_REGISTER_LINT_RULE(XxxRule)
#      - 同目录 BUILD 里的 cc_library(name=..., srcs=[...])
#    现场推导。
#
#  用法：
#    python tests/gen_rule_map.py                      # 打印到 stdout
#    python tests/gen_rule_map.py --json out.json      # 存成 JSON
#    python tests/gen_rule_map.py --lookup signal-name-style   # 查单条
# =============================================================================

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

# 仓库内 vendored Verible 根
REPO_ROOT = Path(__file__).resolve().parent.parent
VERIBLE_ROOT = REPO_ROOT / "thirdparty" / "verible"
VERILOG_ANALYSIS = VERIBLE_ROOT / "verible" / "verilog" / "analysis"

# 抓 VERILOG_REGISTER_LINT_RULE(XxxRule);
REGISTER_RE = re.compile(r"VERILOG_REGISTER_LINT_RULE\(\s*(\w+)\s*\)")

# 规则名的**权威来源**：GetDescriptor() 里的 `.name = "..."`。
# 不要从类名推导 —— 会错，例如 AlwaysFFNonBlockingRule 的真实规则名是
# "always-ff-non-blocking"，而驼峰拆词会得出 "always-f-f-non-blocking"。
DESCRIPTOR_NAME_RE = re.compile(
    r"GetDescriptor\(\)\s*\{.*?\.name\s*=\s*[\"']([^\"']+)[\"']", re.S
)

# 测试文件里的注册是测试夹具（test-rule1 / line-rule1 等），不是真实规则，
# 必须排除，否则会把规则数虚增。
TEST_FILE_RE = re.compile(r".*_test\.cc$")


def parse_build_targets(build_path: Path) -> list[dict]:
    """极简解析 BUILD 里的 cc_library 块，取 name / srcs。

    不做完整 Starlark 解析 —— 只需要填这个映射表，够用即可。
    """
    if not build_path.is_file():
        return []
    text = build_path.read_text(encoding="utf-8", errors="surrogateescape")
    targets: list[dict] = []

    # 逐个 cc_library( ... ) 块
    for m in re.finditer(r"cc_library\(", text):
        start = m.end()
        depth = 1
        i = start
        while i < len(text) and depth > 0:
            if text[i] == "(":
                depth += 1
            elif text[i] == ")":
                depth -= 1
            i += 1
        block = text[start:i]

        name_m = re.search(r'name\s*=\s*["\']([^"\']+)["\']', block)
        if not name_m:
            continue
        srcs: list[str] = []
        srcs_m = re.search(r"srcs\s*=\s*\[(.*?)\]", block, re.S)
        if srcs_m:
            srcs = re.findall(r'["\']([^"\']+\.(?:cc|cpp|cxx))["\']', srcs_m.group(1))
        targets.append({"name": name_m.group(1), "srcs": srcs})
    return targets


def collect_rules() -> dict[str, dict]:
    """扫描所有 .cc，建立 规则名 -> {source, class, target} 映射。"""
    rules: dict[str, dict] = {}

    if not VERILOG_ANALYSIS.is_dir():
        print(f"ERROR: 找不到 {VERILOG_ANALYSIS}", file=sys.stderr)
        return rules

    # 目录 -> BUILD targets 缓存
    build_cache: dict[Path, list[dict]] = {}

    for cc in sorted(VERILOG_ANALYSIS.rglob("*.cc")):
        # 跳过测试夹具（里面的 VERILOG_REGISTER_LINT_RULE 是假的测试规则）
        if TEST_FILE_RE.match(cc.name):
            continue

        text = cc.read_text(encoding="utf-8", errors="surrogateescape")
        regs = REGISTER_RE.findall(text)
        if not regs:
            continue

        # 同目录 BUILD
        d = cc.parent
        if d not in build_cache:
            build_cache[d] = parse_build_targets(d / "BUILD")

        # 该 .cc 属于哪个 target（按 srcs 匹配文件名）
        owning = None
        for t in build_cache[d]:
            if cc.name in t["srcs"]:
                owning = t["name"]
                break

        for cls in regs:
            # 规则名从 GetDescriptor() 的 .name 取（权威来源，见 DESCRIPTOR_NAME_RE）
            name = None
            cm = re.search(
                rf"const\s+LintRuleDescriptor\s*&\s*{re.escape(cls)}::GetDescriptor\(\)",
                text,
            )
            if cm:
                dm = DESCRIPTOR_NAME_RE.search(text[cm.start():cm.start() + 4000])
                if dm:
                    name = dm.group(1)
            if not name:
                # 兜底：文件里只有一个注册且只有一个 .name 时直接用它
                all_names = re.findall(r"\.name\s*=\s*[\"']([^\"']+)[\"']", text)
                if len(regs) == 1 and len(all_names) == 1:
                    name = all_names[0]
            if not name:
                # 最后兜底：类名去 Rule 后缀转 kebab（可能不准，会被标 warn）
                stem = re.sub(r"Rule$", "", cls)
                name = re.sub(r"(?<!^)(?=[A-Z])", "-", stem).lower()

            rel_src = cc.relative_to(VERIBLE_ROOT).as_posix()
            rules[name] = {
                "rule_name": name,
                "class": cls,
                "source": rel_src,
                "target": owning or "<unknown>",
                "build_file": (d / "BUILD").relative_to(VERIBLE_ROOT).as_posix()
                if (d / "BUILD").is_file()
                else None,
            }

    return rules


def cmake_target_name(bazel_target: str) -> str:
    """Bazel target 名 -> 本工程 CMake target 名。

    生成器的命名规则是 verible__<pkg 路径用 __ 连接>__<name>。
    这里拿到的是裸 name，无法单独还原完整路径，所以给出前缀提示，
    真正精确的 target 由 cmake --build ... --target 或 dumpbin 验证。
    """
    return bazel_target


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--json", default=None, help="输出 JSON 路径")
    ap.add_argument("--lookup", default=None, help="查单条规则")
    args = ap.parse_args()

    rules = collect_rules()
    if not rules:
        print("ERROR: 未解析到任何规则", file=sys.stderr)
        return 2

    if args.lookup:
        r = rules.get(args.lookup)
        if not r:
            # 模糊匹配
            cand = [k for k in rules if args.lookup.lower() in k.lower()]
            print(f"未找到规则 {args.lookup!r}", file=sys.stderr)
            if cand:
                print(f"可能的候选: {', '.join(sorted(cand)[:10])}", file=sys.stderr)
            return 1
        print(f"规则名   : {r['rule_name']}")
        print(f"类名     : {r['class']}")
        print(f"源码     : {r['source']}")
        print(f"BUILD    : {r['build_file']}")
        print(f"推测库   : {r['target']}")
        print()
        print("验证方法（确认该库是否真的被链接）：")
        print(f"  dumpbin /symbols build/lib/*.lib | findstr /i {r['class']}")
        return 0

    if args.json:
        Path(args.json).write_text(
            json.dumps(rules, ensure_ascii=False, indent=2), encoding="utf-8"
        )
        print(f"已写出 {args.json}（{len(rules)} 条规则）", file=sys.stderr)
    else:
        print(json.dumps(rules, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())