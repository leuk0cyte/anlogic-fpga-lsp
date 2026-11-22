#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""clang-tidy 棘轮门禁：`src/` 的静态分析**只许变好，不许变差**。

为什么是"棘轮"而不是"零告警"
------------------------------
全量 clang-tidy（`bugprone-*` + `clang-analyzer-*` + `readability-*` + …）在本仓库
是 1.1 万条诊断级别 —— 那种规模适合"出报告"，不适合"当门禁"。门禁要回答的问题只有
一个：**这次改动有没有让情况变差？**

所以：
  * 既有诊断全部记进基线 `tools/audit/clang_tidy_baseline.json`；
  * 本次诊断与基线比对，**出现基线外的新诊断 → 失败**（打印 file:line:col + check + 消息）；
  * 基线里有、本次没有 → 只剩"可以收紧基线"的提示，**不算失败**（可选 `--strict-baseline`）；
  * `--update-baseline` 重写基线。**只能人工确认后使用**：为了"让 CI 变绿"而盲目
    update，等于把棘轮拆掉 —— 这是上一轮"连续两轮缺门禁"的同一个病根。

用法
----
    # 门禁（CTest: docs_clang_tidy / ctest -L tidy 走的也是这条）
    python tools/audit/clang_tidy_gate.py

    # 全量重扫并写基线（人工确认：确认新增诊断都是"修不了/可接受"的，再写）
    python tools/audit/clang_tidy_gate.py --update-baseline

    # 调试单个模块
    python tools/audit/clang_tidy_gate.py --only src/hdl --jobs 4

⚠ 必须经 `tools/audit/with_vcvars.cmd` 运行（MSVC STL 的 include 路径由 vcvars64 提供），
  否则 STL 头全部解析失败，表现为一堆 "std::string_view not found" 的假诊断：

    cmd /c tools\\audit\\with_vcvars.cmd python tools\\audit\\clang_tidy_gate.py

设计要点（都是踩过的坑）
------------------------
* **用线程池而不是 multiprocessing**：Windows 的 `multiprocessing` 默认 spawn，
  子进程重新 import 模块；父进程对全局 CHECK 集的修改不会传过去（`run_clang_tidy.py`
  里 `--checks` 曾被静默忽略）。`subprocess.run` 等待时会释放 GIL，线程池足够。
* 诊断解析只认 clang-tidy 的**稳定文本契约**
  `<path>:<line>:<col>: <warning|error>: <msg> [<check>]`；`--export-fixes` 的 YAML
  在版本间会变形，不做门禁依赖。
* **分析失败的 TU 不算"干净"**：解析错误/超时一律让门禁失败（无法验证 ≠ 通过）。
  同理，`--update-baseline` 在存在失败 TU 时默认拒绝重写 —— 否则会把未分析 TU 的
  既有问题静默丢出基线。
* 路径一律归一化成「相对仓库根、正斜杠」，否则同一份诊断在不同 cwd/盘符下会
  被当成两条。

退出码：0 = 通过（无新增诊断）；1 = 失败（新增诊断 / TU 未成功分析 / 环境不可用）。
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

SCHEMA_VERSION = 1

# clang-tidy 的稳定文本契约：
#   Y:\repo\src\x.cpp:12:5: warning: message text [bugprone-foo]
# 注意 Windows 盘符里的冒号：file 用惰性匹配 + "冒号后紧跟数字" 锚定，不会被盘符骗到。
DIAG_RE = re.compile(
    r"^(?P<file>.+?):(?P<line>\d+):(?P<col>\d+): "
    r"(?P<sev>warning|error): (?P<msg>.*?)(?: \[(?P<check>[-a-zA-Z0-9._,]+)\])?$"
)
COMPILE_ERROR_RE = re.compile(
    r"^(?P<file>.+?):(?P<line>\d+):(?P<col>\d+): error: (?P<msg>.*)$"
)
# 只认 "N errors generated"；"N warnings generated" 是 clang 前端在**有编译告警**时的
# 正常收尾行，把它当分析失败会误判（run_clang_tidy.py 也只用 errors 版本）。
ERRORS_GENERATED_RE = re.compile(r"^\d+ errors? generated\.?$", re.M)
# "Suppressed 6118 warnings (6118 with check filters)." —— 记下来便于解释"为什么条数少"，
# 不是失败条件。
SUPPRESSED_RE = re.compile(r"^Suppressed (\d+) warnings? \((\d+) with check filters\)", re.M)

SRC_PREFIXES = ("src/",)
DEFAULT_HEADER_FILTER = r".*[/\\]src[/\\].*"


# -----------------------------------------------------------------------------
#  路径 / 工具定位
# -----------------------------------------------------------------------------
def resolve_root(arg: str | None) -> Path:
    if arg:
        return Path(arg).resolve()
    return Path(__file__).resolve().parents[2]


def rel_to_root(path: str, root: Path, extra_bases: tuple[Path, ...] = ()) -> str:
    """归一化诊断路径：绝对路径 → 相对仓库根的正斜杠路径；仓库外保持绝对。

    extra_bases 用来兜住"clang-tidy 打印相对路径"的情况：它的 cwd 是数据库目录，
    所以相对路径的基准可能是 db 目录 / 仓库根 / build 目录。
    """
    raw = path.strip().strip('"').replace("\\", "/")
    if not raw:
        return raw
    candidate = Path(raw)
    if not candidate.is_absolute():
        for base in (root, root / "build", *extra_bases):
            probe = base / raw.replace("/", os.sep)
            if probe.exists():
                candidate = probe
                break
        else:
            return raw.lstrip("./")
    try:
        return candidate.resolve().relative_to(root).as_posix()
    except (ValueError, OSError):
        return candidate.as_posix()


def in_src(rel: str) -> bool:
    low = rel.lower()
    return any(low.startswith(p) for p in SRC_PREFIXES)


def find_clang_tidy(explicit: str | None) -> str | None:
    for cand in (explicit, os.environ.get("CLANG_TIDY")):
        if cand:
            if os.path.isabs(cand):
                return cand if os.path.isfile(cand) else None
            found = shutil.which(cand)
            return found or cand
    found = shutil.which("clang-tidy")
    if found:
        return found
    # vcvars 里 LLVM 在 PATH 上；直接跑脚本（不经 vcvars）时退回到 scoop 安装位置。
    fallback = Path(r"%USERPROFILE%\scoop\apps\llvm\current\bin\clang-tidy.exe")
    return str(fallback) if fallback.is_file() else None


def tool_version(exe: str) -> str:
    try:
        proc = subprocess.run(
            [exe, "--version"], capture_output=True, text=True, errors="replace", timeout=120
        )
    except Exception as exc:  # noqa: BLE001 - 版本只用于记录，失败不阻断
        return f"unknown ({exc})"
    for line in (proc.stdout or "").splitlines():
        if "version" in line.lower():
            return line.replace("LLVM version", "LLVM").strip()
    return "unknown"


# -----------------------------------------------------------------------------
#  编译数据库
# -----------------------------------------------------------------------------
def ensure_database(db: Path, root: Path, allow_regenerate: bool) -> list[str] | None:
    """返回 TU 绝对路径列表；不可用时打印中文提示并返回 None。"""
    if not db.is_file():
        if allow_regenerate:
            maker = root / "tools" / "audit" / "make_clang_db.py"
            if maker.is_file():
                print(f"[clang-tidy 门禁] 编译数据库缺失，尝试生成：{maker.name}")
                proc = subprocess.run(
                    [sys.executable, str(maker)],
                    cwd=str(root),
                    capture_output=True,
                    text=True,
                    errors="replace",
                )
                if proc.returncode != 0:
                    print("  生成失败（最后 20 行）：")
                    for line in ((proc.stdout or "") + (proc.stderr or "")).splitlines()[-20:]:
                        print("    " + line)
    if not db.is_file():
        print(f"[clang-tidy 门禁] 失败：找不到编译数据库 {db}")
        print("  先生成（必须经 vcvars，cl.exe 在 PATH 上才有时）：")
        print(r"    cmd /c tools\audit\with_vcvars.cmd python tools\audit\make_clang_db.py")
        print("  它把 build/compile_commands.json（cl.exe 口径）重写成 clang 可消费的数据库。")
        return None
    try:
        with db.open(encoding="utf-8") as fh:
            entries = json.load(fh)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"[clang-tidy 门禁] 失败：编译数据库不可读（{exc}）")
        return None
    tus: list[str] = []
    for entry in entries:
        # 每个条目必有 file；命令行二选一（arguments 数组或 command 字符串）。
        if "file" not in entry or not ("arguments" in entry or "command" in entry):
            print(f"[clang-tidy 门禁] 失败：编译数据库条目缺字段（{db}）: {entry}")
            return None
        tus.append(entry["file"])
    if not tus:
        print(f"[clang-tidy 门禁] 失败：编译数据库为空（{db}）")
        return None
    return tus


# -----------------------------------------------------------------------------
#  单个 TU
# -----------------------------------------------------------------------------
def run_one_tu(exe: str, db_dir: Path, config: Path, tu: str, timeout: int) -> dict:
    """跑一个 TU 并解析输出。返回 {tu, status, rc, seconds, diags, gen_errors, suppressed}。"""
    started = time.time()
    cmd = [
        exe,
        f"-p={db_dir}",
        f"--config-file={config}",
        "--use-color=0",
        "--extra-arg=-fno-color-diagnostics",
        tu,
    ]
    status = "ok"
    out = ""
    rc = -1
    try:
        proc = subprocess.run(
            cmd,
            cwd=str(db_dir),
            capture_output=True,
            text=True,
            errors="replace",
            timeout=timeout,
        )
        out = (proc.stdout or "") + (proc.stderr or "")
        rc = proc.returncode
    except subprocess.TimeoutExpired as exc:
        partial = exc.stdout or ""
        if isinstance(partial, bytes):
            partial = partial.decode("utf-8", "replace")
        out = partial + f"\n[超时：单 TU 超过 {timeout}s，已终止]\n"
        status = "timeout"
        rc = -9
    except Exception as exc:  # noqa: BLE001 - 启动失败要报出来，不能静默
        out = f"[启动失败] {exc}\n"
        status = "error"
    seconds = time.time() - started

    diags, gen_errors = parse_output(out)
    if status == "ok":
        if "Error while processing" in out or ERRORS_GENERATED_RE.search(out):
            status = "parse-error"
        elif rc != 0:
            status = "error"
    suppressed = 0
    match = SUPPRESSED_RE.search(out)
    if match:
        suppressed = int(match.group(1))
    return {
        "tu": tu,
        "status": status,
        "rc": rc,
        "seconds": seconds,
        "diags": diags,
        "gen_errors": gen_errors,
        "suppressed": suppressed,
        "raw_tail": "" if status == "ok" else "\n".join(out.splitlines()[-12:]),
    }


def parse_output(text: str) -> tuple[list[dict], list[str]]:
    """解析 clang-tidy 文本输出。

    返回 (诊断, 编译错误)。诊断只保留带 [check] 的 warning/error；不带 check 的
    `error:` 是 TU 编译失败，单独返回（由调用方决定如何失败）。
    """
    diags: list[dict] = []
    gen_errors: list[str] = []
    for line in text.splitlines():
        match = DIAG_RE.match(line)
        if match and match.group("check"):
            diags.append(
                {
                    "file": match.group("file"),
                    "line": int(match.group("line")),
                    "col": int(match.group("col")),
                    "sev": match.group("sev"),
                    "check": match.group("check"),
                    "msg": match.group("msg").strip(),
                }
            )
            continue
        err = COMPILE_ERROR_RE.match(line)
        if err:
            gen_errors.append(
                f"{err.group('file')}:{err.group('line')}:{err.group('col')}: "
                f"{err.group('msg').strip()}"
            )
    return diags, gen_errors


# -----------------------------------------------------------------------------
#  基线
# -----------------------------------------------------------------------------
def load_baseline(path: Path) -> dict:
    if not path.is_file():
        return {}
    try:
        with path.open(encoding="utf-8") as fh:
            data = json.load(fh)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"[clang-tidy 门禁] 警告：基线不可读（{exc}），按空基线处理")
        return {}
    if not isinstance(data, dict) or not isinstance(data.get("entries"), list):
        print("[clang-tidy 门禁] 警告：基线结构不认识（缺 entries），按空基线处理")
        return {}
    return data


def entry_key(entry: dict) -> tuple[str, int, str]:
    return (str(entry["file"]), int(entry["line"]), str(entry["check"]))


def write_baseline(path: Path, entries: list[dict], meta: dict) -> None:
    counts_by_check: dict[str, int] = {}
    counts_by_file: dict[str, int] = {}
    for entry in entries:
        counts_by_check[entry["check"]] = counts_by_check.get(entry["check"], 0) + 1
        counts_by_file[entry["file"]] = counts_by_file.get(entry["file"], 0) + 1
    payload = {
        "schema": SCHEMA_VERSION,
        "generated": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "tool": meta["tool"],
        "config": meta["config"],
        "config_sha1": meta["config_sha1"],
        "header_filter": meta["header_filter"],
        "tu_count": meta["tu_count"],
        "diag_count": len(entries),
        "counts_by_check": dict(sorted(counts_by_check.items(), key=lambda kv: (-kv[1], kv[0]))),
        "counts_by_file": dict(sorted(counts_by_file.items(), key=lambda kv: (-kv[1], kv[0]))),
        "entries": sorted(entries, key=lambda e: (e["file"], e["line"], e["check"])),
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    with tmp.open("w", encoding="utf-8", newline="\n") as fh:
        json.dump(payload, fh, ensure_ascii=False, indent=1)
        fh.write("\n")
    os.replace(tmp, path)


# -----------------------------------------------------------------------------
#  主流程
# -----------------------------------------------------------------------------
def sha1_of_file(path: Path) -> str:
    try:
        return hashlib.sha1(path.read_bytes()).hexdigest()
    except OSError:
        return ""


def _bootstrap_vcvars_if_needed() -> int | None:
    """CTest 直接调 python 时没有 MSVC 环境：自动经 with_vcvars.cmd 重启一次。

    为什么不让 CTest 自己写 `cmd /c ...`：Windows 上 CTest 传给 cmd 的参数会被
    额外加引号，`cmd /c "<带引号的路径>"` 的引号剥除规则会把命令吃掉
    （实测 `The syntax of the command is incorrect.`）。让脚本自己负责环境自举，
    CTest 里就只剩一条普通的 python 命令。
    """
    if os.environ.get("FPGA_LSP_TIDY_BOOTSTRAPPED") == "1":
        return None
    # 注意不能用 INCLUDE 判空：本机全局 INCLUDE 可能被 Intel oneAPI 之类设置成
    # 非空（但不含 MSVC STL），clang-tidy 仍会成片失败。vcvars64 会设置 VCToolsInstallDir。
    if os.environ.get("VCToolsInstallDir", "").strip():
        return None  # 已经在 vcvars 环境里
    wrapper = Path(__file__).resolve().parents[2] / "tools" / "audit" / "with_vcvars.cmd"
    if not wrapper.exists():
        return None
    env = dict(os.environ)
    env["FPGA_LSP_TIDY_BOOTSTRAPPED"] = "1"
    cmd = ["cmd", "/c", str(wrapper), sys.executable, str(Path(__file__).resolve()), *sys.argv[1:]]
    print("[clang-tidy 门禁] 未检测到 MSVC 环境（INCLUDE 为空），经 with_vcvars.cmd 重启 …", flush=True)
    return subprocess.call(cmd, env=env)


def main(argv: list[str] | None = None) -> int:
    boot = _bootstrap_vcvars_if_needed()
    if boot is not None:
        return boot
    default_root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(
        description="clang-tidy 棘轮门禁（基线外的新诊断 → 失败）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--root", default=None, help="仓库根（默认按脚本位置推断）")
    parser.add_argument("--db", default=None, help="编译数据库（默认 doc/audit/_db/compile_commands.json）")
    parser.add_argument("--config", default=None, help="clang-tidy 配置（默认 <root>/.clang-tidy）")
    parser.add_argument("--baseline", default=None, help="基线文件（默认 tools/audit/clang_tidy_baseline.json）")
    parser.add_argument("--clang-tidy", default=None, help="clang-tidy 可执行文件（默认 PATH 上的）")
    parser.add_argument("--jobs", type=int, default=8, help="并行 TU 数（默认 8）")
    parser.add_argument("--timeout", type=int, default=300, help="单 TU 超时秒数（默认 300）")
    parser.add_argument("--only", default=None, help="只分析路径包含该子串的 TU（调试用）")
    parser.add_argument("--update-baseline", action="store_true", help="重写基线（人工确认后使用）")
    parser.add_argument("--strict-baseline", action="store_true", help="基线里已消失的条目也算失败（要求基线精确）")
    parser.add_argument("--allow-moved", action="store_true",
                        help="容忍同一文件同一 check 同消息的行号漂移（默认不容忍，行号是键的一部分）")
    parser.add_argument("--force", action="store_true",
                        help="允许 --update-baseline 在「有 TU 未成功分析」或 --only 子集下重写（危险）")
    parser.add_argument("--no-auto-db", action="store_true", help="编译数据库缺失时不尝试自动生成")
    args = parser.parse_args(argv)

    root = Path(args.root).resolve() if args.root else default_root
    db = Path(args.db) if args.db else root / "doc" / "audit" / "_db" / "compile_commands.json"
    config = Path(args.config) if args.config else root / ".clang-tidy"
    baseline_path = Path(args.baseline) if args.baseline else root / "tools" / "audit" / "clang_tidy_baseline.json"
    jobs = max(1, args.jobs)
    started = time.time()

    print("[clang-tidy 门禁] 仓库根   :", root)
    if not config.is_file():
        print(f"[clang-tidy 门禁] 失败：找不到 clang-tidy 配置 {config}")
        return 1
    config_sha1 = sha1_of_file(config)

    exe = find_clang_tidy(args.clang_tidy)
    if not exe:
        print("[clang-tidy 门禁] 失败：找不到 clang-tidy（设 CLANG_TIDY 或把 LLVM 加进 PATH）")
        return 1
    version = tool_version(exe)
    print(f"[clang-tidy 门禁] 工具     : {version}")
    print(f"[clang-tidy 门禁] 配置     : {config.relative_to(root).as_posix() if config.is_relative_to(root) else config}")

    # MSVC STL 的 include 只能由 vcvars64 提供。裸跑时给一句明确提示，否则用户会把
    # 一堆 "std::string_view not found" 当成代码坏了。
    if not os.environ.get("INCLUDE"):
        print("[clang-tidy 门禁] 警告：环境里没有 INCLUDE（vcvars 未生效）。")
        print(r"  建议经 tools\audit\with_vcvars.cmd 运行，否则 MSVC STL 头无法解析。")

    tus = ensure_database(db, root, allow_regenerate=not args.no_auto_db)
    if tus is None:
        return 1
    if args.only:
        before = len(tus)
        tus = [tu for tu in tus if args.only.lower() in tu.replace("\\", "/").lower()]
        print(f"[clang-tidy 门禁] --only={args.only}：{before} TU → {len(tus)} TU")
        if not tus:
            print("[clang-tidy 门禁] 失败：--only 没有匹配到任何 TU")
            return 1

    print(f"[clang-tidy 门禁] 编译数据库: {db}（{len(tus)} TU）")
    print(f"[clang-tidy 门禁] 分析中 …  jobs={jobs}, 单 TU 超时={args.timeout}s")

    results: list[dict] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = {
            pool.submit(run_one_tu, exe, db.parent, config, tu, args.timeout): tu for tu in tus
        }
        done = 0
        for future in concurrent.futures.as_completed(futures):
            row = future.result()
            results.append(row)
            done += 1
            rel_tu = rel_to_root(row["tu"], root)
            print(
                f"  [{done}/{len(tus)}] {row['status']:<11} {row['seconds']:6.1f}s  {rel_tu}",
                flush=True,
            )
    results.sort(key=lambda r: rel_to_root(r["tu"], root))
    elapsed_scan = time.time() - started

    # --- 归并诊断：同一 (file,line,check) 在多个 TU 里各报一次（头文件包含）
    #     只算一条 —— 计数口径必须和基线一致，否则永远"新增"。 ---
    current: dict[tuple[str, int, str], dict] = {}
    external = 0
    suppressed_total = 0
    for row in results:
        suppressed_total += row["suppressed"]
        for diag in row["diags"]:
            rel = rel_to_root(diag["file"], root, (db.parent,))
            if not in_src(rel):
                external += 1
                continue
            key = (rel, diag["line"], diag["check"])
            if key not in current:
                current[key] = {
                    "file": rel,
                    "line": diag["line"],
                    "col": diag["col"],
                    "check": diag["check"],
                    "sev": diag["sev"],
                    "msg": diag["msg"],
                }

    failed_tus = [row for row in results if row["status"] != "ok"]
    scanned_tus = len(results) - len(failed_tus)
    current_keys = set(current)

    baseline = load_baseline(baseline_path)
    baseline_entries = baseline.get("entries", [])
    baseline_keys = {entry_key(entry) for entry in baseline_entries}
    # (file, check, msg) → 基线行号，用于"行号漂移"提示
    baseline_msg_lines: dict[tuple[str, str, str], int] = {}
    for entry in baseline_entries:
        baseline_msg_lines.setdefault(
            (entry_key(entry)[0], entry_key(entry)[2], str(entry.get("msg", ""))),
            int(entry["line"]),
        )
    baseline_missing = not baseline_path.is_file()

    new_keys = sorted(current_keys - baseline_keys)
    stale_keys = sorted(baseline_keys - current_keys)

    moved: list[tuple] = []
    if args.allow_moved and new_keys:
        remaining = []
        for key in new_keys:
            diag = current[key]
            old_line = baseline_msg_lines.get((diag["file"], diag["check"], diag["msg"]))
            if old_line is not None and old_line != diag["line"]:
                moved.append((key, old_line))
            else:
                remaining.append(key)
        new_keys = remaining

    # 有 TU 没分析成功时，"基线里消失"可能只是没扫到：不报可收紧，避免假阳性。
    # 同理 --only 子集不报 —— 没扫到的文件当然"消失"。
    stale_note = ""
    if failed_tus:
        stale_keys = []
        stale_note = "（有 TU 未成功分析，不判定可收紧项）"
    elif args.only:
        stale_keys = []
        stale_note = "（--only 子集，不判定可收紧项）"

    elapsed = time.time() - started
    print("")
    print("=" * 78)
    print("  clang-tidy 棘轮门禁 —— 摘要")
    print("=" * 78)
    print(f"  工具        : {version}")
    print(f"  配置        : {config.name}（sha1 {config_sha1[:12]}）")
    print(f"  TU          : {scanned_tus}/{len(results)} 成功分析，{len(failed_tus)} 个失败")
    print(f"  当前诊断    : {len(current)} 条（唯一 file:line:check；src/ 之外另有 {external} 条已忽略）")
    if suppressed_total:
        print(f"  被过滤      : {suppressed_total} 条（clang-tidy 自身 check 过滤，属预期）")
    print(f"  基线        : {len(baseline_keys)} 条（{baseline_path.name}）")
    if baseline_missing:
        print(f"  基线状态    : 不存在 —— 视为空基线，当前 {len(current)} 条全部算新增")
    print(f"  新增        : {len(new_keys)} 条")
    print(f"  可收紧      : {len(stale_keys)} 条{('  ' + stale_note) if stale_note else ''}")
    if moved:
        print(f"  行号漂移    : {len(moved)} 条（按 --allow-moved 容忍）")
    print(f"  耗时        : {elapsed_scan:.1f}s 扫描 / {elapsed:.1f}s 总计")

    if failed_tus:
        print("")
        print("-" * 78)
        print(f"  ✗ {len(failed_tus)} 个 TU 未能成功分析（无法验证 ≠ 通过）")
        print("-" * 78)
        for row in failed_tus[:20]:
            print(f"  {row['status']:<12} rc={row['rc']:<4} {rel_to_root(row['tu'], root)}")
            for line in row["raw_tail"].splitlines():
                print(f"      {line}")
        if len(failed_tus) > 20:
            print(f"  … 另有 {len(failed_tus) - 20} 个")

    if new_keys:
        print("")
        print("-" * 78)
        print(f"  ✗ 新增 {len(new_keys)} 条基线外诊断（棘轮只允许减少）")
        print("-" * 78)
        for key in new_keys:
            diag = current[key]
            print(f"  {diag['file']}:{diag['line']}:{diag['col']}  [{diag['check']}]")
            print(f"      {diag['msg']}")
            old_line = baseline_msg_lines.get((diag["file"], diag["check"], diag["msg"]))
            if old_line is not None and old_line != diag["line"]:
                print(f"      （注：基线与该条同 check 同消息在 L{old_line}，疑似行号漂移）")
        print("")
        print("  修法：修掉它；确属误报/暂不修 → **人工确认后** --update-baseline（写进基线）。")
        print("  禁止为了让门禁变绿而盲目 update —— 那等于把棘轮拆掉。")

    if stale_keys:
        print("")
        print("-" * 78)
        print(f"  ○ 可收紧基线：{len(stale_keys)} 条基线项本次已不存在（修好了就收紧基线）")
        print("-" * 78)
        for key in stale_keys[:30]:
            print(f"  {key[0]}:{key[1]}  [{key[2]}]")
        if len(stale_keys) > 30:
            print(f"  … 另有 {len(stale_keys) - 30} 条")
        print("  收紧方式：--update-baseline（基线变小不改变棘轮语义）")

    # ------------------------------------------------------------------
    #  --update-baseline：重写基线
    # ------------------------------------------------------------------
    if args.update_baseline:
        print("")
        if not scanned_tus:
            print("[clang-tidy 门禁] 拒绝写基线：没有任何 TU 分析成功")
            return 1
        if (failed_tus or args.only) and not args.force:
            print("[clang-tidy 门禁] 拒绝写基线：存在未成功分析的 TU 或使用了 --only。")
            print("  否则未分析 TU 的既有问题会被静默丢出基线（下次就变成‘合法’新增了）。")
            print("  确需如此，加 --force 并自行承担。")
            return 1
        entries = [current[key] for key in sorted(current_keys)]
        meta = {
            "tool": version,
            "config": config.relative_to(root).as_posix() if config.is_relative_to(root) else str(config),
            "config_sha1": config_sha1,
            "header_filter": DEFAULT_HEADER_FILTER,
            "tu_count": scanned_tus,
        }
        write_baseline(baseline_path, entries, meta)
        print(f"[clang-tidy 门禁] 已写基线：{baseline_path}（{len(entries)} 条，"
              f"{scanned_tus} TU，{elapsed_scan:.1f}s）")
        print("  提醒：基线是「已确认接受的债」，不是「忽略列表」。下次改动应当让它变小。")
        return 0

    # ------------------------------------------------------------------
    #  判定
    # ------------------------------------------------------------------
    failed = bool(new_keys or failed_tus or (args.strict_baseline and stale_keys))
    print("")
    if failed:
        reasons = []
        if new_keys:
            reasons.append(f"新增 {len(new_keys)} 条")
        if failed_tus:
            reasons.append(f"{len(failed_tus)} 个 TU 未分析成功")
        if args.strict_baseline and stale_keys:
            reasons.append(f"{len(stale_keys)} 条基线项已消失（--strict-baseline 要求精确）")
        print(f"[clang-tidy 门禁] 失败：{'，'.join(reasons)}")
        return 1
    print(f"[clang-tidy 门禁] 通过：无新增诊断（基线 {len(baseline_keys)} 条，"
          f"当前 {len(current)} 条，{len(results)} TU 全部成功分析）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
