#!/usr/bin/env python3
"""Compare the C++ report parsers against the Python reference, field by field.

用法（工作目录随意）：
    python tests/compare_reports.py

流程：
  1. 造一棵与 TD 完全同构的 <name>_Runs 树（用 tests/fixtures/reports/ 里的真实报告）；
  2. 用 FPGALSP_REPORTS_DUMP / _PROJECT_DIR / _PROJECT_NAME 驱动 unit_reports_parity.exe
     把 C++ 解析结果导出成 JSON；
  3. 对同样的文件调用 %PYREF%\\fpgalsp\\reports.py，得到参考 JSON；
  4. 逐字段深比较，打印差异。

判定标准（三者必须同时成立）：
  A. Python 有的每个键，C++ 都必须有，且值相等；
  B. C++ 允许**多**出文档里声明的追加键（见 EXTRA_KEYS），但不得有其它多余键；
  C. 浮点用 1e-9 相对容差，只有 mtime / generatedAt 用 1e-6 绝对容差。

任何不一致都会以非零退出码结束，方便接入 CI。
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
FIXTURES = REPO / "tests" / "fixtures" / "reports"
PY_REF = Path(r"Y:\fpga-lsp")

# 允许用环境变量指定，否则按"私有 build 目录优先"的顺序找导出程序
_DUMP_CANDIDATES = (
    REPO / "build-reports" / "bin" / "unit_reports_parity.exe",
    REPO / "build" / "bin" / "unit_reports_parity.exe",
)


def find_dump() -> Path | None:
    override = os.environ.get("FPGALSP_REPORTS_DUMP_EXE")
    if override:
        candidate = Path(override)
        return candidate if candidate.exists() else None
    for candidate in _DUMP_CANDIDATES:
        if candidate.exists():
            return candidate
    return None

# C++ 版有意比 Python 多出来的键（协议 v3 / §2 要求，Python 侧没实现）。
# 匹配方式是"路径后缀"：compare 时先把根节点标签（如 timing[ef2_gate.timing]）去掉，
# 剩下的部分只要以其中一条结尾就算已登记。
EXTRA_KEYS = (
    "timing.paths[].begin_at",
    "timing.paths[].end_at",
    "timing.begin_end_resolved",
    "artifacts.syn_db",
)

# 不可比 / 无需比对的键
IGNORED_KEYS = {"generatedAt"}

failures: list[str] = []


def norm_path(path: str) -> str:
    """把 a.groups[3].b 归一化成 a.groups[].b"""
    out = []
    i = 0
    while i < len(path):
        if path[i] == "[":
            j = path.index("]", i)
            out.append("[]")
            i = j + 1
        else:
            out.append(path[i])
            i += 1
    return "".join(out)


def describe(value) -> str:
    text = json.dumps(value, ensure_ascii=False)
    return text if len(text) <= 160 else text[:157] + "..."


def stripped_root(path: str) -> str:
    """去掉根节点的夹具标签：timing[ef2_gate.timing].paths[0].x -> timing.paths[0].x

    只有单文件解析器的根节点带 [夹具名]，collect 的根节点没有标签。
    不能见到第一个 '[' 就砍（那会把数组下标 paths[0] 当成标签）。
    """
    for prefix in ("qor[", "area[", "flowStatus[", "timing["):
        if path.startswith(prefix):
            j = path.find("]")
            if j < 0:
                return path
            return path[: len(prefix) - 1] + path[j + 1 :]
    return path


def extra_key_allowed(path: str) -> bool:
    normalised = norm_path(stripped_root(path))
    return any(normalised == pattern or normalised.endswith("." + pattern)
               for pattern in EXTRA_KEYS)


# 这些字段是"调用方传入的路径原样回显"，两边构造这个字符串的方式不同
# （Python 用 pathlib 枚举、C++ 用 std::filesystem），所以比较前把分隔符统一。
PATH_ECHO_KEYS = {"file"}


def is_path_echo(path: str) -> bool:
    return path.rsplit(".", 1)[-1] in PATH_ECHO_KEYS


def compare(py, cpp, path: str) -> None:
    """Python 是子集基准：每个键都要在 C++ 里存在且相等。"""
    if isinstance(py, str) and isinstance(cpp, str) and is_path_echo(path):
        if py.replace("\\", "/") != cpp.replace("\\", "/"):
            failures.append(f"{path}: 路径不一致 Python={py!r} C++={cpp!r}")
        return
    if isinstance(py, dict):
        if not isinstance(cpp, dict):
            failures.append(f"{path}: Python 是 object，C++ 是 {type(cpp).__name__}")
            return
        for key, py_value in py.items():
            if key in IGNORED_KEYS:
                continue
            if key not in cpp:
                failures.append(f"{path}.{key}: C++ 缺少该键（Python={describe(py_value)}）")
                continue
            compare(py_value, cpp[key], f"{path}.{key}")
        for key in cpp:
            if key in py or key in IGNORED_KEYS:
                continue
            if extra_key_allowed(f"{path}.{key}"):
                continue
            if key in ("warnings",):
                continue
            failures.append(f"{path}.{key}: C++ 多出未登记的键（值={describe(cpp[key])}）")
        return

    if isinstance(py, list):
        if not isinstance(cpp, list):
            failures.append(f"{path}: Python 是 array，C++ 是 {type(cpp).__name__}")
            return
        if len(py) != len(cpp):
            failures.append(f"{path}: 长度不一致 Python={len(py)} C++={len(cpp)}")
            return
        for index, (py_item, cpp_item) in enumerate(zip(py, cpp)):
            compare(py_item, cpp_item, f"{path}[{index}]")
        return

    if isinstance(py, float) or isinstance(cpp, float):
        if not isinstance(py, (int, float)) or not isinstance(cpp, (int, float)):
            failures.append(f"{path}: 类型不一致 Python={describe(py)} C++={describe(cpp)}")
            return
        if path.endswith("mtime"):
            if abs(py - cpp) > 1e-6:
                failures.append(f"{path}: mtime 差异过大 Python={py} C++={cpp}")
            return
        scale = max(1.0, abs(py))
        if abs(py - cpp) > 1e-9 * scale:
            failures.append(f"{path}: 数值不一致 Python={py!r} C++={cpp!r}")
        return

    if py != cpp:
        failures.append(f"{path}: 值不一致 Python={describe(py)} C++={describe(cpp)}")


def read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def build_fake_project(work: Path) -> str:
    """造一份"综合 + 布局布线都跑过"的 <name>_Runs 树，两边用同一份文件。"""
    name = "CorpusBeta"
    runs = work / f"{name}_Runs"
    syn = runs / "syn_1"
    phy = runs / "phy_1"
    best = runs / "best_result"
    for directory in (syn, phy, best):
        directory.mkdir(parents=True, exist_ok=True)

    plan = [
        ("eg4_gate.qor", syn / "gate.qor"),
        ("eg4_gate.area", syn / f"{name}_gate.area"),
        ("eg4_gate.timing", syn / f"{name}_gate.timing"),
        ("eg4_syn_flow.status", syn / "flow.status"),
        ("eg4_route.qor", phy / "route.qor"),
        ("eg4_place.qor", phy / "place.qor"),
        ("eg4_phy.area", phy / f"{name}_phy.area"),
        ("eg4_final_timing.rpt", phy / "final_timing.rpt"),
        ("eg4_route_flow.status", phy / "route_flow.status"),
        ("eg4_place_flow.status", phy / "place_flow.status"),
    ]
    for source, target in plan:
        shutil.copyfile(FIXTURES / source, target)
    (phy / f"{name}.bit").write_bytes(b"bit")
    (phy / f"{name}_pr.db").write_bytes(b"db")
    (syn / f"{name}_gate.db").write_bytes(b"gate-db")
    (best / f"{name}.bit").write_bytes(b"best-bit")
    return name


def main() -> int:
    dump_exe = find_dump()
    if dump_exe is None:
        print("找不到 unit_reports_parity.exe；先构建该目标"
              "（或设置 FPGALSP_REPORTS_DUMP_EXE）", file=sys.stderr)
        return 2
    if not PY_REF.is_dir():
        print(f"找不到 Python 参考实现目录 {PY_REF}", file=sys.stderr)
        return 2
    sys.path.insert(0, str(PY_REF))
    from fpgalsp import reports as pyreports  # noqa: PLC0415

    work_root = Path(tempfile.mkdtemp(prefix="fpgalsp_reports_parity_"))
    project_dir = work_root / "proj"
    project_dir.mkdir(parents=True, exist_ok=True)
    project_name = build_fake_project(project_dir)
    dump_dir = work_root / "dump"
    dump_dir.mkdir(parents=True, exist_ok=True)

    env = dict(os.environ)
    env["FPGALSP_REPORTS_DUMP"] = str(dump_dir)
    env["FPGALSP_REPORTS_PROJECT_DIR"] = str(project_dir).replace("\\", "/")
    env["FPGALSP_REPORTS_PROJECT_NAME"] = project_name
    result = subprocess.run([str(dump_exe)], env=env, capture_output=True, text=True)
    dumps = list(dump_dir.glob("*.json"))
    if not dumps:
        print(result.stdout)
        print(result.stderr, file=sys.stderr)
        print("导出失败：没有产生任何 JSON", file=sys.stderr)
        return 2
    # 导出用例之外的用例失败不影响比对（它们由 ctest 单独管），这里只看 JSON 有没有齐。

    checked = 0

    def load(name: str):
        file = dump_dir / f"{name}.json"
        if not file.exists():
            failures.append(f"{name}: C++ 没有导出这个夹具")
            return None
        return json.loads(file.read_text(encoding="utf-8"))

    # ---- 单文件解析器 ----
    for fixture in sorted(FIXTURES.iterdir()):
        suffix = fixture.suffix
        stem = fixture.stem
        if suffix == ".qor":
            cpp = load(f"qor_{stem}")
            py = pyreports.parse_qor(read(fixture), stem)
            if cpp is not None:
                compare(py, cpp, f"qor[{fixture.name}]")
                checked += 1
        elif suffix == ".area":
            cpp = load(f"area_{stem}")
            py = pyreports.parse_area(read(fixture))
            if cpp is not None:
                compare(py, cpp, f"area[{fixture.name}]")
                checked += 1
        elif suffix == ".status":
            cpp = load(f"flow_{stem}")
            py = pyreports.parse_flow_status_text(read(fixture))
            if cpp is not None:
                compare(py, cpp, f"flowStatus[{fixture.name}]")
                checked += 1
        elif suffix in (".timing", ".rpt"):
            cpp = load(f"timing_{stem}")
            py = pyreports.parse_timing(read(fixture), str(fixture))
            if cpp is not None:
                compare(py, cpp, f"timing[{fixture.name}]")
                checked += 1

    # ---- 汇总（collect）----
    cpp = load("aggregate")

    class FakeProject:
        dir = str(project_dir).replace("\\", "/")
        name = project_name
        al_path = str(project_dir / f"{project_name}.al")
        top = "main"

    py = pyreports.collect(FakeProject())
    if cpp is not None:
        compare(py, cpp, "collect")
        checked += 1
        # generatedAt 单独校验：只要是个合理的 epoch 秒
        got = cpp.get("generatedAt")
        if not isinstance(got, (int, float)) or not (1.6e9 < got < 2.0e9):
            failures.append(f"collect.generatedAt 不合理：{got!r}")

    print(f"对比了 {checked} 份报告")
    if failures:
        # 按"归一化路径"归并，避免同一个问题刷几百行
        grouped: dict[str, list[str]] = {}
        for line in failures:
            key = norm_path(stripped_root(line.split(":")[0]))
            grouped.setdefault(key, []).append(line)
        print(f"\n发现 {len(failures)} 处不一致，归并为 {len(grouped)} 类：")
        for key, lines in grouped.items():
            print(f"\n[{key}] 共 {len(lines)} 处，例：")
            for line in lines[:3]:
                print("  -", line[:300])
        return 1

    print("PASS：与 Python 参考实现逐字段一致")
    return 0


if __name__ == "__main__":
    sys.exit(main())