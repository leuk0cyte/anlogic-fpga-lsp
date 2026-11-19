#!/usr/bin/env python3
"""verify.py —— F0 验收分析器：把 verify.ps1 跑出来的原始结果与 baseline 对齐比较。

输入（全部来自 doc/audit/2026-11-19/fix/）：
    baseline/latency.json, baseline/phase2.json, baseline/perf_index.txt, baseline/lsp-raw.json, baseline/mcp-raw.json
    results/<tag>/{latency,phase2}.json, results/<tag>/perf_index.txt, results/<tag>/{lsp,mcp}-raw.json, meta.json

输出：
    results/<tag>.json   结构化对比（每个指标 before/after/Δ/判定）
    results/<tag>.md     markdown 表（verify.ps1 会直接打印）

判定规则（对齐 doc/audit/2026-11-19/perf/README.md §6）：
    |Δ| < 15%  → 无变化（不算优化，也不算回退）
    Δ ≤ −15%   → 通过（变快）
    Δ ≥ +15%   → 回退（变慢）
    缺 baseline → 无基线（只记录数值）
协议对比：按 canonical JSON 逐键比较，**只允许 meta 不同**；其余任何差异都标「协议变化」。
"""

from __future__ import annotations

import argparse
import json
import os
import sys

UTIL_KEYS = ("meta",)

# 协议快照里必然随环境/时间变化的字段：对比前统一归一化，否则每次都会"有差异"。
#   elapsed_ms / seconds  —— 探针自己记录的耗时
#   exe / exeSha256       —— 二进制路径与哈希（exe 本来就该变）
#   argv[0]               —— 调用方式（相对路径 vs 绝对路径）
# -NormalizeQorSuggestions：默认关闭。owner 怀疑 qor suggestions 的 step 顺序本身
# 就是非确定的（place vs route），基线证据回来之前不许默认打开（Lead 明确要求）。
NORMALIZE_QOR_SUGGESTIONS = False


def _maybe_sort_suggestions(value):
    if not NORMALIZE_QOR_SUGGESTIONS or not isinstance(value, list):
        return value
    if value and all(isinstance(item, dict) and "step" in item for item in value):
        return sorted(value, key=lambda item: (str(item.get("step")), canonical(item)))
    return value


VOLATILE_KEYS = {"elapsed_ms", "seconds", "elapsed", "exe", "exeSha256",
                 "generatedAt", "generated_at", "timestamp", "at", "mtime",
                 # digest 是探针自己对"归一化结果"算的 sha256；它把探针自己的
                 # MASK_KEYS（时延/时间戳）也算进去了，所以只要时间戳变它就变。
                 # 我这边改为直接比对归一化后的内容，故不把 digest 当差异。
                 "digest", "revision"}

# 环境性差异（不是产品行为差异）：探针每轮用不同的临时工程副本与不同的 exe 路径。
_ENV_PATH_PATTERNS = (
    (r"[A-Za-z]:[\\/][^\"']*fpgalsp-parity-lsp[\\/][^\"'\\/]*", "<probe-project>"),
    (r"[A-Za-z]:[\\/][^\"']*pm-corpus[\\/]CorpusAlpha", "<project>"),
    # 探针每次 --copy-projects 的临时目录名带 PID（pm-copy-15248 / pm-copy-43312 …），
    # 会原样出现在 fpga_project_update 的 TD 输出里 —— 属于环境差异，不是产品差异。
    (r"pm-copy-\d+", "<copy>"),
    (r"Y:[\\/]CorpusAlpha", "<project>"),
    (r"Y:[\\/]sic[\\/]CorpusAlpha", "<declared-project>"),
    (r"[A-Za-z]:[\\/][^\"']*fpgalsp-gate[\\/]exe[\\/][^\"'\\/]*\.exe", "<exe>"),
    (r"build\\bin\\fpgalsp-native\.exe", "<exe>"),
)


def _mask_string(text):
    import re
    for pattern, token in _ENV_PATH_PATTERNS:
        text = re.sub(pattern, token, text)
    return text


def normalize_for_diff(value):
    """把协议快照里的易变字段抹掉，只留下"响应内容"。"""
    if isinstance(value, dict):
        out = {}
        for key, item in value.items():
            if key in VOLATILE_KEYS:
                out[key] = "<volatile>"
                continue
            out[key] = normalize_for_diff(item)
        return out
    if isinstance(value, list):
        return _maybe_sort_suggestions([normalize_for_diff(item) for item in value])
    if isinstance(value, str):
        return _mask_string(value)
    return value


def load_json(path):
    try:
        with open(path, encoding="utf-8") as handle:
            return json.load(handle)
    except Exception:
        return None


def num(value):
    return value if isinstance(value, (int, float)) else None


def delta(before, after):
    if before is None or after is None:
        return None
    if before == 0:
        return None
    return round((after - before) / before * 100.0, 1)


# 噪声底（实测：同一 exe 跑两遍，baseline vs baseline-selftest）：
#   p50 类指标 ±4% 以内；p95 类（edit→completion p95、HOL p95）能到 ±27%。
# 所以判定阈值按指标类别分档，避免把噪声当优化。
NOISE_P95_PCT = 25.0


def threshold_for(key):
    # 实测（同一 exe 连跑两个 tag，exe 哈希完全相同）：@1000 的 p95 会 7.31 ↔ 11.28 波动（+54%），
    # 所以 @1000 的 p95 必须放宽到 60%；其余 p95 用 25%，p50 用 15%。
    if "p95" in key and key.endswith("@1000"):
        return 60.0
    return NOISE_P95_PCT if "p95" in key else 15.0


def verdict(before, after, lower_is_better=True, key=""):
    d = delta(before, after)
    if d is None:
        return "无基线"
    if abs(d) < threshold_for(key):
        return "无变化"
    if lower_is_better:
        return "通过" if d <= -threshold_for(key) else "回退"
    return "通过" if d >= threshold_for(key) else "回退"


# ---------------------------------------------------------------------------
#  指标抽取
# ---------------------------------------------------------------------------
def metrics_from_latency(doc):
    """latency_driver.py 的输出 → 扁平指标字典。"""
    out = {}
    if not doc:
        return out
    chain = doc.get("key_chain") or {}
    for size, entry in chain.items():
        summary = (entry or {}).get("summary") or {}
        comp = summary.get("completion_ms") or {}
        diag = summary.get("didChange_to_diag_ms") or {}
        out[f"edit_completion_p50_ms@{size}"] = num(comp.get("p50"))
        out[f"edit_completion_p95_ms@{size}"] = num(comp.get("p95"))
        out[f"didChange_to_diag_p50_ms@{size}"] = num(diag.get("p50"))

    methods = doc.get("methods") or {}
    for size, entry in methods.items():
        for name, stats in ((entry or {}).get("methods") or {}).items():
            short = name.split("/")[-1].split("}")[0]
            out[f"{short}_p50_ms@{size}"] = num(stats.get("p50"))
            out[f"{short}_p95_ms@{size}"] = num(stats.get("p95"))

    hol = doc.get("head_of_line") or {}
    if hol:
        out["hol_idle_hover_p50_ms"] = num((hol.get("idle_ms") or {}).get("p50"))
        out["hol_busy_hover_p50_ms"] = num((hol.get("busy_ms") or {}).get("p50"))
        out["hol_busy_hover_p95_ms"] = num((hol.get("busy_ms") or {}).get("p95"))
        out["hol_ratio_p50"] = num(hol.get("head_of_line_ratio_p50"))
        rounds = hol.get("rounds") or []
        heavy = [num(row.get("heavy_ms")) for row in rounds if isinstance(row, dict)]
        heavy = [value for value in heavy if value is not None]
        if heavy:
            out["reports_full_p50_ms"] = round(sorted(heavy)[len(heavy) // 2], 1)

    cancel = doc.get("cancel") or {}
    if cancel:
        out["cancel_cpu_saved_ratio"] = num(cancel.get("cpu_saved_ratio"))
        out["cancel_cpu_full_s"] = (
            round(sum(cancel["full_cpu_s"]["samples"]) / len(cancel["full_cpu_s"]["samples"]), 3)
            if (cancel.get("full_cpu_s") or {}).get("samples") else None)
        out["cancel_cpu_after_s"] = (
            round(sum(cancel["cancel_cpu_s"]["samples"]) / len(cancel["cancel_cpu_s"]["samples"]), 3)
            if (cancel.get("cancel_cpu_s") or {}).get("samples") else None)
        out["hover_after_cancel_p50_ms"] = num((cancel.get("hover_after_cancel_ms") or {}).get("p50"))

    burst = doc.get("edit_burst") or {}
    for size, entry in burst.items():
        out[f"burst20_completion_ms@{size}"] = num((entry or {}).get("burst_summary", {}).get("p50"))
        out[f"burst20_drift@{size}"] = num((entry or {}).get("sequential_drift_ratio"))
    return out


def metrics_from_phase2(doc):
    out = {}
    if not doc:
        return out
    phase2 = doc.get("phase2") or {}
    for entry in phase2.get("edit_loop") or []:
        size = str(entry.get("files"))
        native = (entry.get("sides") or {}).get("native") or {}
        if "completion_ms" in native:
            out[f"phase2_editloop_p50_ms@{size}"] = num(native["completion_ms"].get("p50"))
            out[f"phase2_editloop_p95_ms@{size}"] = num(native["completion_ms"].get("p95"))
    for entry in phase2.get("warm") or []:
        size = str(entry.get("files"))
        native = (entry.get("sides") or {}).get("native") or {}
        methods = native.get("methods") or {}
        completion = methods.get("textDocument/completion") or {}
        out[f"phase2_warm_completion_p50_ms@{size}"] = num(completion.get("p50"))
        out[f"phase2_cold_index_ms@{size}"] = num(native.get("cold_index_ms"))
    reports = ((phase2.get("reports") or {}).get("sides") or {}).get("native") or {}
    if reports:
        out["reports_first_parse_ms"] = num(reports.get("first_parse_ms"))
        out["reports_repeat_p50_ms"] = num((reports.get("repeat_ms") or {}).get("p50"))
        out["reports_payload_chars"] = num(reports.get("payload_chars"))
        out["reports_peak_mb"] = num((reports.get("memory") or {}).get("peak_working_set_mb"))
    footprint = phase2.get("footprint") or {}
    for name in ("lsp_only", "mcp_only", "lsp_and_mcp"):
        entry = footprint.get(name) or {}
        out[f"mem_{name}_peak_mb"] = num(entry.get("peak_working_set_mb"))
        out[f"mem_{name}_procs"] = num(entry.get("server_processes"))
    return out


def metrics_from_reports(path):
    """reports_probe.py 的输出 → 与 phase2 同名的指标（便于共用对比逻辑）。"""
    doc = load_json(path)
    if not doc:
        return {}
    return {
        "reports_first_parse_ms": num(doc.get("first_parse_ms")),
        "reports_repeat_p50_ms": num((doc.get("repeat_ms") or {}).get("p50")),
        "reports_repeat_p95_ms": num((doc.get("repeat_ms") or {}).get("p95")),
        "reports_payload_chars": num(doc.get("payload_chars")),
        "reports_peak_mb": num((doc.get("memory") or {}).get("peak_working_set_mb")),
    }


def metrics_from_index(path):
    """perf_index.exe 的 `csv,<label>,<n>,<p50>,<p95>,<max>,<mean>` 行。"""
    out = {}
    if not os.path.isfile(path):
        return out
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = line.strip()
            if not line.startswith("csv,"):
                continue
            parts = line.split(",")
            if len(parts) < 6:
                continue
            label = parts[1]
            try:
                out[f"index_{label}_p50_ms"] = float(parts[3])
                out[f"index_{label}_p95_ms"] = float(parts[4])
            except ValueError:
                continue
    return out


# ---------------------------------------------------------------------------
#  协议对比
# ---------------------------------------------------------------------------
def strip_meta(value):
    """meta 里是 argv/exe/时间戳（"怎么测的"），不是"测出什么"；比较前整体剔除。"""
    if isinstance(value, dict):
        return {key: strip_meta(item) for key, item in value.items()
                if key not in UTIL_KEYS}
    if isinstance(value, list):
        return [strip_meta(item) for item in value]
    return value


def canonical(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def diff_paths(before, after, prefix="", limit=12):
    """返回不同之处的人类可读路径（只取前 limit 条）。"""
    diffs = []

    def walk(a, b, path):
        if len(diffs) >= limit:
            return
        if type(a) is not type(b) and not (isinstance(a, (int, float)) and isinstance(b, (int, float))):
            diffs.append(path or "<root>")
            return
        if isinstance(a, dict):
            for key in sorted(set(a) | set(b)):
                if key in UTIL_KEYS or key in ("meta",):
                    continue
                if key not in a or key not in b:
                    diffs.append("%s/%s (missing on one side)" % (path, key))
                    continue
                walk(a[key], b[key], "%s/%s" % (path, key))
        elif isinstance(a, list):
            if len(a) != len(b):
                diffs.append("%s (len %d vs %d)" % (path, len(a), len(b)))
                return
            for index, (left, right) in enumerate(zip(a, b)):
                walk(left, right, "%s[%d]" % (path, index))
        elif a != b:
            diffs.append("%s (%r vs %r)" % (path, a, b))

    walk(before, after, prefix)
    return diffs


def scenario_of(raw, kind):
    """抽出"这次探针到底测的是哪个场景"。场景不同 → 数字/载荷不可比。"""
    meta = (raw or {}).get("meta") or {}
    if kind == "mcp":
        return {
            "mode": "copy-projects" if meta.get("copyProjects") else "in-place",
            "sections": meta.get("sections"),
            "projects": meta.get("projects") or "",
        }
    argv = meta.get("argv") or []
    project = None
    only = None
    for index, item in enumerate(argv):
        if item == "--project" and index + 1 < len(argv):
            project = argv[index + 1]
        if item == "--only" and index + 1 < len(argv):
            only = argv[index + 1]
    return {"project": project, "only": only}


def compare_protocol(baseline_dir, tag_dir):
    out = {}
    for name in ("lsp", "mcp"):
        base_path = os.path.join(baseline_dir, name + "-raw.json")
        tag_path = os.path.join(tag_dir, name + "-raw.json")
        if not (os.path.isfile(base_path) and os.path.isfile(tag_path)):
            out[name] = {"status": "未测", "note": "基线或本轮快照缺失"}
            continue
        raw_before = load_json(base_path)
        raw_after = load_json(tag_path)
        scene_before = scenario_of(raw_before, name)
        scene_after = scenario_of(raw_after, name)
        if scene_before != scene_after:
            out[name] = {
                "status": "scenario-mismatch（不判回归/通过）",
                "scenario_baseline": scene_before,
                "scenario_tag": scene_after,
                "diffs": [],
            }
            continue
        before = normalize_for_diff(strip_meta(raw_before))
        after = normalize_for_diff(strip_meta(raw_after))
        if before is None or after is None:
            out[name] = {"status": "读取失败"}
            continue
        identical = canonical(before) == canonical(after)
        out[name] = {
            "status": "逐字节相同" if identical else "有差异",
            "diffs": [] if identical else diff_paths(before, after),
        }
    return out


# ADC / SDC 四项：Lead 要求每 tag 至少跑 3 遍，任何一遍与基线不同就报"复现"。
ADC_SDC_CASES = [("adc", "documentSymbol"), ("adc", "hover"), ("adc", "completion"),
                 ("sdc", "documentSymbol"), ("sdc", "hover")]


def _index_boundaries(doc):
    """(file, case) -> boundary 条目（Lead 关注的 ADC/SDC 四项就是按 file+case 定位）。"""
    table = {}
    for item in (doc or {}).get("boundaries") or []:
        key = (item.get("file"), item.get("case"))
        table[key] = item
    return table


def check_adc_sdc(baseline_dir, tag_dir, max_runs=3):
    """ADC/SDC 四项（3 遍）：与基线 digest 比较 + 检查原始响应里有没有 NUL。"""
    base = _index_boundaries(load_json(os.path.join(baseline_dir, "lsp-raw.json")))
    if not base:
        return {"status": "无基线"}
    runs = []
    for run in range(1, max_runs + 1):
        name = "lsp-raw.json" if run == 1 else "lsp-raw-run%d.json" % run
        path = os.path.join(tag_dir, name)
        if not os.path.isfile(path):
            continue
        table = _index_boundaries(load_json(path))
        bad = []
        nul = 0
        for key in ADC_SDC_CASES:
            item = table.get(key)
            base_item = base.get(key)
            if item is None or base_item is None:
                bad.append({"case": "%s/%s" % (key[0], key[1]), "reason": "缺条目"})
                continue
            digest_now = (item.get("cpp") or {}).get("digest")
            digest_base = (base_item.get("cpp") or {}).get("digest")
            raw = json.dumps((item.get("cpp") or {}).get("raw"), ensure_ascii=False)
            nul += raw.count("\\u0000")
            if digest_now != digest_base:
                bad.append({"case": "%s/%s" % (key[0], key[1]),
                            "reason": "cpp digest 变化",
                            "base": str(digest_base)[:12], "now": str(digest_now)[:12]})
        runs.append({"run": run, "mismatches": bad, "nul_in_raw": nul})
    reproduced = [row for row in runs if row["mismatches"] or row["nul_in_raw"]]
    if not runs:
        status = "未测"
    elif len(reproduced) == len(runs):
        status = "复现（%d/%d 遍全部不一致）" % (len(reproduced), len(runs))
    elif reproduced:
        status = "不稳定（%d/%d 遍不一致）" % (len(reproduced), len(runs))
    else:
        status = "三遍一致且与基线相同"
    return {"status": status, "runs": runs,
            "reproduced": bool(reproduced)}


# p95 改善必须 p50 同向：Lead 的要求（避免"p95 掉进噪声、p50 没动"被当成改善）
P95_TO_P50 = {
    "edit_completion_p95_ms@200": "edit_completion_p50_ms@200",
    "edit_completion_p95_ms@1000": "edit_completion_p50_ms@1000",
    "edit_completion_p95_ms@50": "edit_completion_p50_ms@50",
}


# ---------------------------------------------------------------------------
def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--tag", required=True)
    parser.add_argument("--tag-dir", required=True)
    parser.add_argument("--baseline-dir", required=True)
    parser.add_argument("--meta", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--normalize-qor-suggestions", action="store_true",
                        help="把 qor suggestions 按 step 排序后再比（默认关闭）")
    args = parser.parse_args(argv)

    global NORMALIZE_QOR_SUGGESTIONS
    NORMALIZE_QOR_SUGGESTIONS = bool(args.normalize_qor_suggestions)
    meta = load_json(args.meta) or {}
    stages = {name: (os.path.join(args.tag_dir, name)) for name in ("latency.json", "phase2.json")}

    after = {}
    after.update(metrics_from_latency(load_json(stages["latency.json"])))
    after.update(metrics_from_phase2(load_json(stages["phase2.json"])))
    after.update(metrics_from_reports(os.path.join(args.tag_dir, "reports.json")))
    after.update(metrics_from_index(os.path.join(args.tag_dir, "perf_index.txt")))

    before = {}
    before.update(metrics_from_latency(load_json(os.path.join(args.baseline_dir, "latency.json"))))
    before.update(metrics_from_phase2(load_json(os.path.join(args.baseline_dir, "phase2.json"))))
    before.update(metrics_from_reports(os.path.join(args.baseline_dir, "reports.json")))
    before.update(metrics_from_index(os.path.join(args.baseline_dir, "perf_index.txt")))

    rows = []
    for key in sorted(set(before) | set(after)):
        b = before.get(key)
        a = after.get(key)
        lower_better = True
        if "cpu_saved" in key or "procs" in key or "payload" in key:
            lower_better = "payload" in key
        if key in ("cancel_cpu_saved_ratio",):
            lower_better = False
        rows.append({"metric": key, "before": b, "after": a,
                     "delta_pct": delta(b, a),
                     "verdict": verdict(b, a, lower_better, key)})

    covers_protocol = bool(meta.get("covers_protocol", True))
    if covers_protocol and not meta.get("no_protocol"):
        protocol = compare_protocol(args.baseline_dir, args.tag_dir)
    else:
        # fast tag 没跑协议探针 → 必须显式写"未覆盖"，绝不能让人读成"无差异"
        protocol = {"lsp": {"status": "未覆盖（本 tag coverage=%s）" % meta.get("coverage")},
                    "mcp": {"status": "未覆盖（本 tag coverage=%s）" % meta.get("coverage")}}
    adc_sdc = check_adc_sdc(args.baseline_dir, args.tag_dir,
                            max_runs=max(1, int(meta.get("protocol_runs") or 1)))

    # 主结论（给 VERIFY.md 用的 3 个数）
    def pick(key):
        return after.get(key)

    headline = {
        "edit_completion_p95_ms@200": [before.get("edit_completion_p95_ms@200"),
                                       pick("edit_completion_p95_ms@200")],
        "edit_completion_p95_ms@1000": [before.get("edit_completion_p95_ms@1000"),
                                        pick("edit_completion_p95_ms@1000")],
        "hover_p50_ms@1000": [before.get("hover_p50_ms@1000"), pick("hover_p50_ms@1000")],
        "reports_repeat_p50_ms": [before.get("reports_repeat_p50_ms"),
                                  pick("reports_repeat_p50_ms")],
        "hol_busy_hover_p95_ms": [before.get("hol_busy_hover_p95_ms"),
                                  pick("hol_busy_hover_p95_ms")],
        "mem_lsp_and_mcp_peak_mb": [before.get("mem_lsp_and_mcp_peak_mb"),
                                    pick("mem_lsp_and_mcp_peak_mb")],
    }

    report = {
        "meta": meta,
        "valid": bool(meta.get("valid", True)),
        "invalid_reason": meta.get("invalid_reason"),
        "headline": {key: {"before": pair[0], "after": pair[1],
                           "delta_pct": delta(pair[0], pair[1]),
                           "verdict": verdict(pair[0], pair[1], True, key)}
                     for key, pair in headline.items()},
        "metrics": rows,
        "protocol": protocol,
        "adc_sdc": adc_sdc,
        "verdicts": {verdict_name: sum(1 for row in rows if row["verdict"] == verdict_name)
                     for verdict_name in ("通过", "回退", "无变化", "无基线")},
    }

    # p95 改善必须 p50 同向，否则降级为"无变化"
    for p95_key, p50_key in P95_TO_P50.items():
        entry = report["headline"].get(p95_key)
        if not entry or entry["verdict"] != "通过":
            continue
        p50_before = before.get(p50_key)
        p50_after = after.get(p50_key)
        p50_delta = delta(p50_before, p50_after)
        entry["p50_delta_pct"] = p50_delta
        if p50_delta is None or p50_delta > -15.0:
            entry["verdict"] = "无变化（p95 改善但 p50 未同向）"

    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(report, handle, ensure_ascii=False, indent=1)

    # ---- markdown ----
    lines = []
    exe = meta.get("exe") or {}
    lines.append("")
    lines.append("## %s  coverage=%s  (exe %s / %s B, load %s, git %s, dirty %s)" % (
        args.tag, meta.get("coverage"), (exe.get("sha256") or "?")[:12], exe.get("bytes"),
        meta.get("cpu_busy_fraction"), (meta.get("git_head") or "")[:8],
        meta.get("git_dirty_files")))
    lines.append("")
    lines.append("| 指标 | before | after | Δ | 判定 |")
    lines.append("| --- | ---: | ---: | ---: | --- |")
    for key, pair in headline.items():
        row = report["headline"][key]
        lines.append("| **%s** | %s | %s | %s%% | %s |" % (
            key, pair[0], pair[1], row["delta_pct"], row["verdict"]))
    lines.append("")
    lines.append("判定计数：通过 %d / 回退 %d / 无变化 %d / 无基线 %d" % (
        report["verdicts"]["通过"], report["verdicts"]["回退"],
        report["verdicts"]["无变化"], report["verdicts"]["无基线"]))
    if protocol:
        lines.append("")
        lines.append("协议对比：" + "；".join(
            "%s=%s" % (name, info.get("status")) for name, info in protocol.items()))
        for name, info in protocol.items():
            for diff in info.get("diffs") or []:
                lines.append("  - %s: %s" % (name, diff))
    for name, info in (protocol or {}).items():
        if str(info.get("status", "")).startswith("scenario-mismatch"):
            lines.append("")
            lines.append("⚠ 场景不一致（%s）：基线 %s vs 本轮 %s"
                         % (name, info.get("scenario_baseline"), info.get("scenario_tag")))
    if adc_sdc:
        lines.append("")
        lines.append("ADC/SDC 四项：**%s**" % adc_sdc.get("status"))
        for row in adc_sdc.get("runs") or []:
            if row.get("mismatches") or row.get("nul_in_raw"):
                lines.append("  - run%d: NUL=%d %s" % (row["run"], row["nul_in_raw"],
                                                      row["mismatches"]))
    if not report["valid"]:
        lines.append("")
        lines.append("**⚠ 本 tag 作废**：%s" % report.get("invalid_reason"))
    if meta.get("stage_errors"):
        lines.append("")
        lines.append("阶段错误：")
        for name, message in meta["stage_errors"].items():
            lines.append("  - %s: %s" % (name, str(message)[:200]))
    text = "\n".join(lines)
    with open(args.out + ".md", "w", encoding="utf-8") as handle:
        handle.write(text + "\n")
    print(text)

    # 协议有差异 → 非零退出，让 CI/Lead 立刻看到
    protocol_changed = any(info.get("status") == "有差异" for info in (protocol or {}).values())
    return 3 if protocol_changed else 0


if __name__ == "__main__":
    sys.exit(main())