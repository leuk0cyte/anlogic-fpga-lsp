#!/usr/bin/env python3
# =============================================================================
#  tests/compare_cst.py —— 阶段 0 门禁：token 序列一致性对比
# =============================================================================
#  对比两条完全独立的实现路径：
#    A) 官方基准:  C:/tools/verible\verible-verilog-syntax.exe
#                   --printrawtokens <file>
#    B) 本仓库:    build/bin/cst_dump.exe --rawtokens <file>
#
#  判定口径：
#    * 比较的是 **token 三元组 (kind, offset_range, text)**，逐条、按顺序。
#    * 不做任何"宽容匹配"：offset 和文本都要一模一样。
#    * 唯一允许的归一化是 **行尾差异（CRLF vs LF）** —— 因为两边都把
#      "\\r\\n" 转义成常量字符串 "\\r\\n" 后写进行文本，若不归一，
#      Windows python 以 text 模式读 stdin 会把 \r\n 变成 \n，造成假不一致。
#      归一化本身也会被统计并报告（见 --show-normalized）。
#    * 非 token 行（错误消息、"All lexed tokens:" 标题、空行）在 token 对比中
#      被丢弃，但错误消息单独对比（--strict-errors），因为错误恢复行为也是
#      解析行为的一部分。
#
#  退出码：
#    0 = 所有文件 token 序列 100% 一致（门禁通过）
#    1 = 存在不一致（门禁不通过）
#    2 = 环境错误（二进制/corpus 缺失）
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

# 官方和 cst_dump 都输出这种行：
#   (#"module" @0-6: "module")
#   (#SymbolIdentifier @7-10: "top")
#   (#"end of file" @37-37: "")
TOKEN_LINE_RE = re.compile(r"^\(#(?P<kind>.*?) @(?P<start>\d+)-(?P<end>\d+): (?P<text>.*)\)$")


@dataclass
class Token:
    kind: str
    start: int
    end: int
    text: str

    def key(self) -> tuple[str, int, int, str]:
        return (self.kind, self.start, self.end, self.text)

    def __str__(self) -> str:
        return f"(#{self.kind} @{self.start}-{self.end}: {self.text})"


@dataclass
class RunResult:
    returncode: int
    tokens: list[Token]
    error_lines: list[str]
    non_token_lines: list[str]
    stdout_bytes: int


@dataclass
class FileResult:
    path: str
    official: RunResult | None = None
    cst: RunResult | None = None
    error: str | None = None
    first_diff: str | None = None
    normalized_lines: int = 0


def decode_output(raw: bytes) -> list[str]:
    """按 UTF-8 解码（Verible 输出含 CEscape 后的 ASCII，原样 UTF-8 亦可）。"""
    text = raw.decode("utf-8", errors="surrogateescape")
    return text.split("\n")


def strip_cr(lines: list[str]) -> tuple[list[str], int]:
    """去掉行尾的所有 CR，返回 (新行列表, 被修正的行数)。

    为什么是"所有"而不是一个：Python 子进程在 Windows 上以文本模式转发
    stdout 时可能产生 \\r\\r\\n；测试夹具（如对比用的包装脚本）也可能叠加
    换行符。token 行正文本身绝不以裸 \\r 结尾（Verible 把 \\r 转义成字面
    字符序列 "\\r"，见 CEscape），因此剥掉行尾 CR 不会改变任何 token 内容。
    """
    out: list[str] = []
    fixed = 0
    for line in lines:
        stripped = line.rstrip("\r")
        if stripped != line:
            fixed += 1
        out.append(stripped)
    return out, fixed


def normalize_error_line(line: str) -> str:
    """把错误行归一化成可比较的形式。

    原始行形如：
        Y:\\path\\to\\file.v:1:8-13: lexical error at token "654foo"
      （Windows 上报告的是绝对路径，且列范围可能带 -N 后缀）

    归一化：
      * 去掉文件名前缀 -> 只保留 ":line:col[-endcol]: message"
      * 列号范围统一保留（它本身是有效信息）

    为什么不整体删掉路径就好：不同调用方式（相对路径 vs 绝对路径）会让两边
    前缀不同，但这不是解析行为差异。去掉前缀后仍然保留行/列/消息，因此
    真正的行为差异（行号错、列号错、消息文本不同）一律能被抓到。
    """
    # 匹配  <path>:<line>:<col>[-<endcol>]: <rest>
    m = re.match(r"^.*?:(\d+):(\d+(?:-\d+)?): (.*)$", line)
    if m:
        return f"{m.group(1)}:{m.group(2)}: {m.group(3)}"
    return line


def parse_tokens(lines: list[str]) -> tuple[list[Token], list[str], list[str]]:
    """从输出行里提取 token 行、错误行、其它行。"""
    tokens: list[Token] = []
    errors: list[str] = []
    others: list[str] = []
    for line in lines:
        if not line.strip():
            continue
        m = TOKEN_LINE_RE.match(line)
        if m:
            tokens.append(
                Token(
                    kind=m.group("kind"),
                    start=int(m.group("start")),
                    end=int(m.group("end")),
                    text=m.group("text"),
                )
            )
        elif line.startswith("(") and "@" in line:
            # 形似 token 但不匹配（格式意外变化）——当成错误，别静默吞掉
            errors.append("UNPARSED-TOKEN-LINE: " + line)
        elif line.strip().startswith("All lexed tokens") or line.strip().startswith(
            "Lexed and filtered tokens"
        ):
            others.append(line)
        elif re.match(r"^.*:\d+:\d+(-\d+)?:", line):
            # Y:\path\file.v:2:14: syntax error at token ";"  ->
            # 归一化掉路径前缀，保留 行:列: 消息，见 normalize_error_line()
            errors.append(normalize_error_line(line))
        else:
            others.append(line)
    return tokens, errors, others


def run_tool(cmd: list[str], cwd: str | None = None) -> tuple[int, bytes]:
    proc = subprocess.run(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        cwd=cwd,
    )
    return proc.returncode, proc.stdout


def split_cmd(spec: str) -> list[str]:
    """把 "exe --flag" 这种命令行拆成 argv（仅用于 cst_dump 侧，便于自测）。

    用 shlex 以 posix=False 拆，保留 Windows 路径里的反斜杠。
    """
    import shlex

    parts = shlex.split(spec, posix=False)
    # shlex 在非 posix 模式下可能保留引号，去掉成对引号
    return [p.strip('"') for p in parts] if parts else [spec]


def collect_files(corpus: Path, pattern: str, excludes: list[str]) -> list[Path]:
    files: list[Path] = []
    for root, _dirs, names in os.walk(corpus):
        for name in sorted(names):
            if not name.endswith(pattern):
                continue
            full = Path(root) / name
            rel = full.relative_to(corpus).as_posix()
            if any(re.search(ex, rel) for ex in excludes):
                continue
            files.append(full)
    return sorted(files, key=lambda p: p.as_posix())


def normalize_kind(kind: str) -> str:
    return kind


def compare_one(
    path: Path,
    official_argv: list[str],
    cst_argv: list[str],
    strict_errors: bool,
) -> FileResult:
    res = FileResult(path=path.as_posix())

    official_cmd = official_argv + [str(path)]
    cst_cmd = cst_argv + [str(path)]

    try:
        off_rc, off_out = run_tool(official_cmd)
    except OSError as exc:
        res.error = f"official run failed: {exc}"
        return res
    try:
        cst_rc, cst_out = run_tool(cst_cmd)
    except OSError as exc:
        res.error = f"cst_dump run failed: {exc}"
        return res

    off_lines, off_fixed = strip_cr(decode_output(off_out))
    cst_lines, cst_fixed = strip_cr(decode_output(cst_out))
    res.normalized_lines = off_fixed + cst_fixed

    off_tokens, off_errors, off_other = parse_tokens(off_lines)
    cst_tokens, cst_errors, cst_other = parse_tokens(cst_lines)

    res.official = RunResult(off_rc, off_tokens, off_errors, off_other, len(off_out))
    res.cst = RunResult(cst_rc, cst_tokens, cst_errors, cst_other, len(cst_out))

    # --- token 逐个对比 ---
    off_keys = [t.key() for t in off_tokens]
    cst_keys = [t.key() for t in cst_tokens]

    if off_keys != cst_keys:
        n = min(len(off_keys), len(cst_keys))
        idx = next((i for i in range(n) if off_keys[i] != cst_keys[i]), n)
        parts = [f"token mismatch at index {idx} "
                 f"(official={len(off_keys)} tokens, cst_dump={len(cst_keys)} tokens)"]
        lo = max(0, idx - 2)
        for i in range(lo, min(max(len(off_keys), len(cst_keys)), idx + 3)):
            o = str(off_tokens[i]) if i < len(off_tokens) else "<none>"
            c = str(cst_tokens[i]) if i < len(cst_tokens) else "<none>"
            marker = "  <<<" if o != c else ""
            parts.append(f"  [{i}] official: {o}")
            parts.append(f"  [{i}] cst_dump: {c}{marker}")
        res.first_diff = "\n".join(parts)
        return res

    # --- 错误消息对比（解析错误恢复行为） ---
    if strict_errors and off_errors != cst_errors:
        parts = [f"error-message mismatch (official={len(off_errors)}, "
                 f"cst_dump={len(cst_errors)})"]
        n = min(len(off_errors), len(cst_errors))
        idx = next((i for i in range(n) if off_errors[i] != cst_errors[i]), n)
        parts.append(f"  first diff at index {idx}:")
        parts.append(f"    official: {off_errors[idx] if idx < len(off_errors) else '<none>'}")
        parts.append(f"    cst_dump: {cst_errors[idx] if idx < len(cst_errors) else '<none>'}")
        res.first_diff = "\n".join(parts)

    return res


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cst-dump", required=True, help="cst_dump 可执行文件路径")
    ap.add_argument("--official", required=True, help="官方 verible-verilog-syntax 路径")
    ap.add_argument("--corpus", required=True, help="语料根目录")
    ap.add_argument("--report", default=None, help="JSON 报告输出路径")
    ap.add_argument("--pattern", default=".v", help="文件后缀过滤（默认 .v；.sv 另跑一次）")
    ap.add_argument("--exclude", action="append", default=[], help="正则，排除相对路径")
    ap.add_argument("--strict-errors", action="store_true", default=True,
                    help="同时对比错误消息（默认开启）")
    ap.add_argument("--no-strict-errors", dest="strict_errors", action="store_false")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--selftest", action="store_true",
                    help="用官方基准自比，验证脚本自身的解析/归一化逻辑")
    ap.add_argument("--max-show", type=int, default=20, help="最多展示多少条不一致")
    args = ap.parse_args()

    # cst_dump 侧允许写成 "exe --flag"，便于 --selftest 时用官方 exe 自比。
    # 注意：先拆 argv 再做存在性检查，否则 "python xxx.py" 会被当成单个路径。
    cst_argv = split_cmd(args.cst_dump)
    if len(cst_argv) == 1:
        # 单 token 时按"可执行文件"处理，规范化为绝对路径
        cst_argv = [str(Path(args.cst_dump).resolve())]
    official_exe = str(Path(args.official).resolve())
    corpus = Path(args.corpus).resolve()
    if args.selftest:
        # 自比：官方 exe 的 --printrawtokens 输出 vs 它自己，一致率必须是 100%。
        # 这是脚本本身的正确性验证（解析/归一化逻辑没错）。
        cst_argv = [official_exe, "--printrawtokens"]
        print("*** --selftest 模式：官方基准自比（验证脚本逻辑，非门禁）***")
    elif len(cst_argv) == 1:
        # 裸可执行文件（不带参数）默认用 --rawtokens，对齐官方 --printrawtokens。
        cst_argv = [cst_argv[0], "--rawtokens"]
    official_argv = [official_exe, "--printrawtokens"]
    cst_dump_exe = " ".join(cst_argv)

    if not Path(cst_argv[0]).is_file() and shutil.which(cst_argv[0]) is None:
        print(f"ERROR: cst_dump 不存在: {cst_argv[0]}", file=sys.stderr)
        return 2
    if not Path(official_exe).is_file():
        print(f"ERROR: 官方基准不存在: {official_exe}", file=sys.stderr)
        return 2
    if not corpus.is_dir():
        print(f"ERROR: 语料目录不存在: {corpus}", file=sys.stderr)
        return 2

    # 每个后缀都跑一遍
    patterns = [p.strip() for p in args.pattern.split(",") if p.strip()]
    files: list[Path] = []
    for pat in patterns:
        files.extend(collect_files(corpus, pat, args.exclude))
    # 去重且保持稳定顺序
    seen = set()
    uniq: list[Path] = []
    for f in files:
        key = f.as_posix()
        if key not in seen:
            seen.add(key)
            uniq.append(f)
    files = uniq

    if not files:
        print(f"ERROR: 语料目录下没有匹配 {patterns} 的文件: {corpus}", file=sys.stderr)
        return 2

    print(f"语料目录 : {corpus}")
    print(f"官方基准 : {official_exe}")
    print(f"被测实现 : {cst_dump_exe}")
    print(f"文件数   : {len(files)}  后缀: {patterns}")
    print("-" * 78)

    results: list[FileResult] = []
    ok = 0
    bad: list[FileResult] = []
    total_tokens = 0
    normalized_total = 0

    for idx, path in enumerate(files, 1):
        res = compare_one(path, official_argv, cst_argv, args.strict_errors)
        results.append(res)
        if res.official is not None:
            total_tokens += len(res.official.tokens)
        normalized_total += res.normalized_lines

        if res.error is None and res.first_diff is None:
            ok += 1
            if args.verbose:
                print(f"  [OK]   {path.relative_to(corpus).as_posix()}")
        else:
            bad.append(res)
            rel = path.relative_to(corpus).as_posix()
            status = "ERR" if res.error else "DIFF"
            print(f"  [{status}] {rel}")
            if res.error:
                print(f"         {res.error}")
            elif res.first_diff:
                for line in res.first_diff.split("\n"):
                    print(f"         {line}")

    print("-" * 78)
    total = len(files)
    rate = (ok / total * 100.0) if total else 0.0
    print(f"共 {total} 个文件，一致 {ok} 个，不一致 {len(bad)} 个，一致率 {rate:.2f}%")
    print(f"对比 token 总数（以官方为准）: {total_tokens}")
    if normalized_total:
        print(f"行尾归一化生效行数: {normalized_total}（CRLF->LF，仅影响行文本，不改 token）")

    if bad:
        print("\n不一致明细:")
        for res in bad[: args.max_show]:
            rel = Path(res.path).name
            print(f"  - {res.path}")
            if res.error:
                print(f"      error: {res.error}")
            elif res.first_diff:
                print(res.first_diff)
        if len(bad) > args.max_show:
            print(f"  ... 其余 {len(bad) - args.max_show} 个省略")

    if args.report:
        report = {
            "corpus": str(corpus),
            "official": official_exe,
            "cst_dump": cst_dump_exe,
            "patterns": patterns,
            "total_files": total,
            "consistent": ok,
            "inconsistent": len(bad),
            "consistency_rate": round(rate, 4),
            "official_token_count": total_tokens,
            "normalized_line_count": normalized_total,
            "failures": [
                {
                    "file": r.path,
                    "error": r.error,
                    "first_diff": r.first_diff,
                }
                for r in bad
            ],
            "files": [
                {
                    "file": r.path,
                    "official_tokens": len(r.official.tokens) if r.official else None,
                    "cst_tokens": len(r.cst.tokens) if r.cst else None,
                    "official_rc": r.official.returncode if r.official else None,
                    "cst_rc": r.cst.returncode if r.cst else None,
                    "official_errors": len(r.official.error_lines) if r.official else None,
                    "cst_errors": len(r.cst.error_lines) if r.cst else None,
                    "match": r.error is None and r.first_diff is None,
                }
                for r in results
            ],
        }
        out = Path(args.report)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        print(f"\nJSON 报告: {out}")

    print()
    if bad:
        print("门禁结论: **不通过** —— 存在 token 序列/解析行为差异")
        return 1
    print("门禁结论: **通过** —— 所有文件 token 序列 100% 一致")
    return 0


if __name__ == "__main__":
    sys.exit(main())