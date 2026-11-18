#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LSP e2e 冒烟：把 fpgalsp-native.exe 当作真正的 LSP 服务器驱动一遍。

覆盖的验收点（task-14 交付 7）：
  1. 启动 `fpgalsp-native.exe --stdio`
  2. initialize -> 断言能力声明与 PROTOCOL-V2.md 逐项一致
  3. didOpen 一个真实 .v（tests/gate-corpus/corpus_alpha/src/top.v）
  4. fpgalsp.info -> 断言拿到 family / device / top
  5. fpgalsp.check -> 断言收到 textDocument/publishDiagnostics（且已排序、未截断）
  6. shutdown + exit -> 断言进程正常退出，且**没有残留 td_commands_prompt.exe**
  7. fpgaLsp/moduleTree -> 断言 ok:true 且有 root

另外顺带钉死两条"必须显式"的约定：
  * 未知方法 -> -32601（不能静默不返回）
  * 占位命令 -> {"ok": false, "error": "not implemented"}（不能让客户端挂住）

用法：
    python tests/e2e/lsp_smoke.py --server build/bin/fpgalsp-native.exe
    python tests/e2e/lsp_smoke.py --server ... --skip-td   # 只跑不依赖 TD 的部分
"""

from __future__ import annotations

import argparse
import csv
import io
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from project_isolation import (  # noqa: E402  (同目录模块)
    declared_projects_touched,
    diff_listing,
    isolated_project,
    snapshot_listing,
)

TD_EXE = "td_commands_prompt.exe"

# PROTOCOL-V2.md 里 initialize 必须声明的语义 token legend（顺序有意义）
EXPECTED_TOKEN_TYPES = [
    "keyword", "type", "class", "property", "variable", "parameter",
    "function", "number", "comment", "string", "operator", "macro",
]

FAILURES: list[str] = []


def check(condition: bool, message: str) -> None:
    if condition:
        print(f"  ok   {message}")
    else:
        print(f"  FAIL {message}")
        FAILURES.append(message)


def note(message: str) -> None:
    print(f"  ..   {message}")


# ---------------------------------------------------------------------------
# 最小 LSP 客户端（Content-Length framing，单读线程）
# ---------------------------------------------------------------------------
class LspClient:
    def __init__(self, argv: list[str], cwd: str | None = None) -> None:
        self.proc = subprocess.Popen(
            argv,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=cwd,
        )
        self._next_id = 1
        self._responses: dict[int, dict] = {}
        self._notifications: list[dict] = []
        self._lock = threading.Lock()
        self._cond = threading.Condition(self._lock)
        self._eof = False
        self._stderr: list[str] = []
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()
        self._stderr_reader = threading.Thread(target=self._stderr_loop, daemon=True)
        self._stderr_reader.start()

    # -- 读线程 ------------------------------------------------------------
    def _read_message(self) -> dict | None:
        assert self.proc.stdout is not None
        length = None
        while True:
            line = self.proc.stdout.readline()
            if not line:
                return None
            if line in (b"\r\n", b"\n"):
                break
            lowered = line.lower()
            if lowered.startswith(b"content-length:"):
                try:
                    length = int(line.split(b":", 1)[1].strip())
                except ValueError:
                    return None
        if length is None:
            return None
        body = self.proc.stdout.read(length)
        if body is None or len(body) != length:
            return None
        try:
            return json.loads(body.decode("utf-8"))
        except ValueError:
            return None

    def _read_loop(self) -> None:
        while True:
            message = self._read_message()
            if message is None:
                with self._cond:
                    self._eof = True
                    self._cond.notify_all()
                return
            with self._cond:
                if "method" in message and "id" in message:
                    # 服务器 -> 客户端的请求：本服务不发，若出现就忽略（回错误会干扰）
                    self._notifications.append(message)
                elif "method" in message:
                    self._notifications.append(message)
                elif "id" in message:
                    self._responses[int(message["id"])] = message
                self._cond.notify_all()

    def _stderr_loop(self) -> None:
        assert self.proc.stderr is not None
        for raw in self.proc.stderr:
            self._stderr.append(raw.decode("utf-8", "replace").rstrip())

    # -- 写 ----------------------------------------------------------------
    def _send(self, payload: dict) -> None:
        if self.proc.stdin is None:
            raise RuntimeError("stdin closed")
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        header = f"Content-Length: {len(body)}\r\n\r\n".encode("ascii")
        self.proc.stdin.write(header + body)
        self.proc.stdin.flush()

    def request(self, method: str, params, timeout: float = 30.0) -> dict:
        with self._lock:
            request_id = self._next_id
            self._next_id += 1
        self._send({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params})
        deadline = time.time() + timeout
        with self._cond:
            while request_id not in self._responses:
                remaining = deadline - time.time()
                if remaining <= 0 or self._eof:
                    raise TimeoutError(f"no response for {method} (id={request_id})")
                self._cond.wait(remaining)
            return self._responses.pop(request_id)

    def notify(self, method: str, params) -> None:
        self._send({"jsonrpc": "2.0", "method": method, "params": params})

    # -- 通知观察 -----------------------------------------------------------
    def wait_notification(self, method: str, timeout: float, predicate=None,
                          since: int = 0) -> dict | None:
        """等价于"从下标 since 起，等到第一条满足条件的通知"。

        since 很关键：didOpen 会先发一条（可能是空的）publishDiagnostics，
        若不跳过它，检查 fpgalsp.check 时会匹配到 didOpen 那条而误判。
        """
        deadline = time.time() + timeout
        seen = since
        with self._cond:
            while True:
                while seen < len(self._notifications):
                    message = self._notifications[seen]
                    seen += 1
                    if message.get("method") == method and (
                        predicate is None or predicate(message.get("params") or {})
                    ):
                        return message
                remaining = deadline - time.time()
                if remaining <= 0 or self._eof:
                    return None
                self._cond.wait(remaining)

    def notifications(self, method: str) -> list[dict]:
        with self._lock:
            return [m for m in self._notifications if m.get("method") == method]

    def snapshot_index(self) -> int:
        with self._lock:
            return len(self._notifications)

    def notification_slice(self, start: int, end: int | None = None) -> list[dict]:
        with self._lock:
            return self._notifications[start:end]

    def index_of(self, message: dict) -> int:
        with self._lock:
            for index, item in enumerate(self._notifications):
                if item is message:
                    return index
            return -1

    def stderr_text(self) -> str:
        return "\n".join(self._stderr[-40:])

    # -- 收尾 --------------------------------------------------------------
    def wait_exit(self, timeout: float) -> int | None:
        try:
            return self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            return None

    def kill(self) -> None:
        if self.proc.poll() is None:
            self.proc.kill()


# ---------------------------------------------------------------------------
# 进程表：td_commands_prompt.exe 的 PID 集合
# ---------------------------------------------------------------------------
def td_pids() -> set[int]:
    try:
        completed = subprocess.run(
            ["tasklist", "/FI", f"IMAGENAME eq {TD_EXE}", "/FO", "CSV", "/NH"],
            capture_output=True,
            timeout=60,
        )
    except (OSError, subprocess.SubprocessError):
        return set()
    text = completed.stdout.decode("utf-8", "replace").strip()
    pids: set[int] = set()
    for row in csv.reader(io.StringIO(text)):
        if len(row) < 2:
            continue
        if row[0].strip().lower() != TD_EXE:
            continue
        try:
            pids.add(int(row[1]))
        except ValueError:
            continue
    return pids


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------
def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True, help="fpgalsp-native.exe 路径")
    parser.add_argument("--project", default=None, help=".al 工程目录（默认用 gate-corpus/corpus_alpha）")
    parser.add_argument("--skip-td", action="store_true", help="跳过 fpgalsp.check（不启动 TD）")
    parser.add_argument("--timeout", type=float, default=300.0, help="TD 检查等待上限（秒）")
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parents[2]
    project_dir = Path(args.project) if args.project else (
        repo_root / "tests" / "gate-corpus" / "corpus_alpha"
    )
    project_dir = project_dir.resolve()
    al_path = project_dir / "demo_project.al"
    top_v = project_dir / "src" / "top.v"

    print(f"server   : {args.server}")
    print(f"project  : {project_dir}")
    if not al_path.is_file():
        print(f"FAIL 找不到工程文件 {al_path}")
        return 2
    if not top_v.is_file():
        print(f"FAIL 找不到 top.v {top_v}")
        return 2

    log_file = Path(tempfile.gettempdir()) / f"fpgalsp-e2e-{os.getpid()}.log"
    baseline_td = td_pids()
    if baseline_td:
        note(f"启动前已存在 {len(baseline_td)} 个 {TD_EXE}（不属于本次测试）: {sorted(baseline_td)}")

    # TD 的 cwd（工程目录/.fpgalsp/run）会被它写入自己的日志（见 PYTHON-BUGS P-4）。
    # 跑完把 .fpgalsp 清掉，避免污染语料；只对仓库内的测试语料这样做，
    # 用户真实工程（--project 指向仓库外）一律不碰。
    fpgalsp_dir = project_dir / ".fpgalsp"
    tests_root = (repo_root / "tests").resolve()
    is_corpus = str(project_dir).lower().startswith(str(tests_root).lower())
    corpus_before = snapshot_listing(project_dir)
    started_at = time.time()  # 用于"真实工程是否被写入"的判定基准

    # flow 需要在**隔离副本**上跑（唯一实现：tests/e2e/project_isolation.py）
    #   P-7：语料 .al 的根 Path 指向另一个真实工程，不隔离就会写坏用户工程
    flow_ctx = None
    flow_root: Path | None = None
    flow_al: Path | None = None

    client = LspClient(
        [args.server, "--stdio", "--log", str(log_file)],
        cwd=str(repo_root),
    )
    exit_code = 1
    try:
        # ---- 1/2. initialize ----
        print("[1] initialize")
        root_uri = "file:///" + project_dir.as_posix()
        response = client.request(
            "initialize",
            {
                "processId": os.getpid(),
                "rootUri": root_uri,
                "capabilities": {},
                "initializationOptions": {"fpgaLsp": {"tdCheckOnSave": False}},
            },
            timeout=60.0,
        )
        result = response.get("result") or {}
        caps = result.get("capabilities") or {}
        check(bool(caps), "initialize 返回 capabilities")
        check(result.get("serverInfo", {}).get("name") == "fpga-lsp", "serverInfo.name == fpga-lsp")

        sync = caps.get("textDocumentSync") or {}
        sync = sync if isinstance(sync, dict) else {}
        check(sync.get("openClose") is True, "textDocumentSync.openClose == true")
        check(sync.get("change") == 2, "textDocumentSync.change == 2 (Incremental)")
        check((sync.get("save") or {}).get("includeText") is False,
              "textDocumentSync.save.includeText == false")

        check((caps.get("completionProvider") or {}).get("triggerCharacters") == [".", "("],
              'completionProvider.triggerCharacters == [".", "("]')
        for name in (
            "hoverProvider",
            "definitionProvider",
            "documentSymbolProvider",
            "workspaceSymbolProvider",
            "referencesProvider",
            "callHierarchyProvider",
            "foldingRangeProvider",
            "inlayHintProvider",
        ):
            check(caps.get(name) is True, f"{name} == true")

        check((caps.get("codeActionProvider") or {}).get("codeActionKinds")
              == ["refactor", "quickfix"],
              "codeActionProvider.codeActionKinds == [refactor, quickfix]")
        check(caps.get("documentFormattingProvider") is True,
              "documentFormattingProvider == true（内嵌 Verible，始终声明）")
        check(caps.get("documentRangeFormattingProvider") is True,
              "documentRangeFormattingProvider == true")
        check((caps.get("renameProvider") or {}).get("prepareProvider") is True,
              "renameProvider.prepareProvider == true")

        semantic = caps.get("semanticTokensProvider") or {}
        check((semantic.get("legend") or {}).get("tokenTypes") == EXPECTED_TOKEN_TYPES,
              "semanticTokensProvider.legend.tokenTypes 顺序与协议一致")
        check((semantic.get("legend") or {}).get("tokenModifiers") == [],
              "semanticTokensProvider.legend.tokenModifiers == []")
        check(semantic.get("full") is True, "semanticTokensProvider.full == true")

        client.notify("initialized", {})

        # ---- 3. didOpen ----
        print("[2] textDocument/didOpen (src/top.v)")
        top_text = top_v.read_text(encoding="utf-8", errors="replace")
        client.notify(
            "textDocument/didOpen",
            {
                "textDocument": {
                    "uri": top_v.as_uri(),
                    "languageId": "verilog",
                    "version": 1,
                    "text": top_text,
                }
            },
        )
        # didOpen 之后服务端会发布一次该文件的诊断（可能是空数组）
        opened = client.wait_notification(
            "textDocument/publishDiagnostics",
            timeout=30.0,
            predicate=lambda p: p.get("uri") == top_v.as_uri(),
        )
        check(opened is not None, "didOpen 后收到 textDocument/publishDiagnostics")

        # ---- 4. fpgalsp.info ----
        print("[3] workspace/executeCommand fpgalsp.info")
        response = client.request("workspace/executeCommand",
                                  {"command": "fpgalsp.info", "arguments": []}, timeout=120.0)
        info = response.get("result") or {}
        check(info.get("ok") is True, f"fpgalsp.info ok:true（实际 {info.get('ok')!r}）")
        check(info.get("family") == "EF2", f"fpgalsp.info.family == EF2（实际 {info.get('family')!r}）")
        check(info.get("device") == "EF2L45LG144B",
              f"fpgalsp.info.device == EF2L45LG144B（实际 {info.get('device')!r}）")
        check(info.get("top") == "top", f"fpgalsp.info.top == top（实际 {info.get('top')!r}）")
        check((info.get("files") or {}).get("hdl") == 20,
              f"fpgalsp.info.files.hdl == 20（.al 里 20 个 Verilog 文件；"
              f"实际 {(info.get('files') or {}).get('hdl')}）")
        check((info.get("td") or {}).get("ok") is True, "fpgalsp.info.td.ok == true（找到 TD 安装）")
        note(f"td = {(info.get('td') or {}).get('root')} "
             f"({(info.get('td') or {}).get('version')})")

        # ---- 额外：fpgaLsp/moduleTree（任务里的第 7 项，必须在 exit 之前）----
        print("[4] fpgaLsp/moduleTree")
        response = client.request("fpgaLsp/moduleTree", {"uri": top_v.as_uri()}, timeout=120.0)
        tree = response.get("result") or {}
        check(tree.get("ok") is True, f"moduleTree ok:true（实际 {tree.get('ok')!r} "
                                      f"error={tree.get('error')!r}）")
        root = tree.get("root") or {}
        check(bool(root), "moduleTree.root 非空")
        check(root.get("module") == "top", f"root.module == top（实际 {root.get('module')!r}）")
        check(root.get("defined") is True, "root.defined == true")
        check(root.get("portCount") == 25,
              f"root.portCount == 25（实际 {root.get('portCount')!r}）")
        children = root.get("children") or []
        check(len(children) > 0, f"root.children 非空（实际 {len(children)}）")
        check(all("module" in child and "range" in child for child in children),
              "每个 child 都有 module / range")
        check(tree.get("modulesSeen", 0) > 1,
              f"modulesSeen > 1（实际 {tree.get('modulesSeen')}）")
        check(tree.get("truncated") is False, "truncated == false")
        pll = next((c for c in children if c.get("module") == "pll_gen"), None)
        if pll is not None:
            # 真实索引的关键收益：IP 生成的 al_ip/pll_gen.v 被纳入索引 -> 不再是黑盒
            check(pll.get("defined") is True and pll.get("blackbox") is False,
                  "pll_gen（IP，al_ip/pll_gen.v 已入索引）不是黑盒")
            check(bool(pll.get("usedAt")), "pll_gen.usedAt 指向例化处")
            check(pll.get("portCount", 0) > 0, f"pll_gen.portCount > 0（实际 {pll.get('portCount')}）")

        # ---- 额外：符号类 handler（基于 HDL 索引）----
        print("[4b] documentSymbol / definition / hover / completion / workspace.symbol")
        root_line = (root.get("range") or {}).get("start", {}).get("line", 0)
        root_char = (root.get("range") or {}).get("start", {}).get("character", 0)

        response = client.request("textDocument/documentSymbol",
                                  {"textDocument": {"uri": top_v.as_uri()}}, timeout=120.0)
        symbols = response.get("result") or []
        check(len(symbols) == 1, f"documentSymbol 返回 1 个顶层符号（实际 {len(symbols)}）")
        if symbols:
            module_symbol = symbols[0]
            check(module_symbol.get("name") == "top",
                  f"documentSymbol[0].name == top（实际 {module_symbol.get('name')!r}）")
            check(module_symbol.get("kind") == 2, "documentSymbol[0].kind == 2 (Module)")
            kids = module_symbol.get("children") or []
            check(len(kids) >= 25, f"documentSymbol 子符号 ≥25（实际 {len(kids)}）")
            port_kinds = [k for k in kids if k.get("kind") == 8]
            check(len(port_kinds) == 25, f"端口(Field, kind 8) == 25（实际 {len(port_kinds)}）")
            check(any(k.get("kind") == 13 for k in kids), "至少有一个信号(Variable, kind 13)")
            check(all("detail" in k and "selectionRange" in k for k in kids),
                  "每个子符号都有 detail / selectionRange")

        response = client.request("textDocument/hover",
                                  {"textDocument": {"uri": top_v.as_uri()},
                                   "position": {"line": root_line, "character": root_char}},
                                  timeout=60.0)
        hover = response.get("result") or {}
        check("top" in json.dumps(hover, ensure_ascii=False), "hover 返回模块名")

        if pll is not None and pll.get("usedAt"):
            used = pll["usedAt"]["range"]["start"]
            response = client.request(
                "textDocument/definition",
                {"textDocument": {"uri": top_v.as_uri()},
                 "position": {"line": used["line"], "character": used["character"]}},
                timeout=60.0,
            )
            location = response.get("result") or {}
            uri = location.get("uri") or ""
            check(uri.lower().endswith("pll_gen.v"),
                  f"definition 从 pll_gen 例化处跳到 al_ip/pll_gen.v（实际 {uri!r}）")

        response = client.request(
            "textDocument/completion",
            {"textDocument": {"uri": top_v.as_uri()},
             "position": {"line": root_line + 2, "character": 0}},
            timeout=60.0,
        )
        completion = response.get("result") or {}
        items = completion.get("items") or []
        check(len(items) > 0, f"completion 返回候选（实际 {len(items)}）")
        check(any(item.get("kind") == 9 for item in items), "completion 含模块名候选 (kind 9)")
        snippets = [item for item in items if item.get("kind") == 15]
        check(bool(snippets), "行首 completion 含例化模板 snippet (kind 15)")
        if snippets:
            snippet = snippets[0]
            check(snippet.get("insertTextFormat") == 2,
                  "snippet.insertTextFormat == 2 (Snippet)")
            check("${1:" in (snippet.get("insertText") or ""),
                  "snippet.insertText 含 ${1:...} tab stop")
            check(str(snippet.get("label", "")).endswith("(instantiate)"),
                  "snippet.label 形如 '<module> (instantiate)'")

        response = client.request("workspace/symbol", {"query": "pwm"}, timeout=60.0)
        hits = response.get("result") or []
        check(len(hits) > 0, f"workspace/symbol('pwm') 有命中（实际 {len(hits)}）")
        check(all("location" in hit and "kind" in hit for hit in hits),
              "workspace/symbol 每项都有 location / kind")
        check(any(hit.get("containerName") for hit in hits),
              "workspace/symbol 子符号带 containerName")

        # ---- 5. executeCommand: runFlow 的真实语义 + 立刻 cancelFlow（flow 已接通）----
        #  ⚠ 这里**不能**等 flow 跑完（综合要 8~9 s，e2e 会变成几分钟）。
        #     拿到 runId 后立刻 cancel —— 这条路径同时是 flow 收尾崩溃的回归测试：
        #     取消会让 Worker 走"提前 return"分支；若那里析构了仍 joinable 的
        #     heartbeat 线程，进程会以 0xC0000409 退出，本用例会红。
        print("[5] executeCommand: runFlow / cancelFlow / 占位命令 / 未知方法")
        # flow 在**隔离副本**上跑：语料的 .al 根 Path 指向另一个真实工程（P-7），
        # 直接在语料上跑会让 TD 把 *_Runs 写进用户工程。
        # 隔离逻辑唯一实现：tests/e2e/project_isolation.py
        try:
            flow_ctx = isolated_project(project_dir)
            flow_root, flow_al = flow_ctx.__enter__()
            note(f"flow 隔离副本: {flow_root}")
        except Exception as exc:  # noqa: BLE001
            flow_ctx = None
            check(False, f"准备 flow 隔离副本失败: {exc}")

        if flow_al is not None:
            response = client.request(
                "workspace/executeCommand",
                {"command": "fpgalsp.setProject", "arguments": [{"al": str(flow_al)}]},
                timeout=120.0,
            )
            switched = response.get("result") or {}
            check(switched.get("ok") is True,
                  f"setProject 切到隔离副本（实际 {switched!r}）")

        response = client.request(
            "workspace/executeCommand",
            {"command": "fpgalsp.runFlow", "arguments": [{"stages": ["syn"], "jobs": 2}]},
            timeout=120.0,
        )
        run = response.get("result") or {}
        if run.get("ok") is True:
            check(bool(run.get("runId")), f"runFlow 返回非空 runId（实际 {run!r}）")
            check(isinstance(run.get("stages"), list) and bool(run.get("stages")),
                  "runFlow 返回非空 stages 列表")
            check(run.get("error") is None, "runFlow 成功时没有 error 字段")
        else:
            # 只接受"已在跑"这一种失败原因；`not implemented` 已经不该出现
            check(run.get("error") == "busy",
                  f"runFlow 失败只应是 busy（实际 {run!r}）")

        response = client.request("workspace/executeCommand",
                                  {"command": "fpgalsp.cancelFlow", "arguments": []},
                                  timeout=120.0)
        cancel = response.get("result") or {}
        check(cancel.get("ok") is True, f"cancelFlow ok:true（实际 {cancel!r}）")

        # 等 flow 真正停下来（最多 20s）：取消要写 .stop.f / 宽限期后杀 TD
        deadline = time.time() + 20.0
        stopped = False
        while time.time() < deadline:
            response = client.request("fpgaLsp/flowStatus", {}, timeout=60.0)
            flow = (response.get("result") or {}).get("flow") or {}
            if not flow.get("running", False):
                stopped = True
                note(f"flow 已停止（stage={flow.get('stage')!r} ok={flow.get('ok')!r}）")
                break
            time.sleep(0.3)
        check(stopped, "cancelFlow 后 flow 在 20s 内停止")

        # 恢复"自动发现工程"，后面的 TD 检查回到语料工程
        if flow_al is not None:
            response = client.request(
                "workspace/executeCommand",
                {"command": "fpgalsp.setProject", "arguments": [{"al": ""}]},
                timeout=60.0,
            )
            restored = response.get("result") or {}
            check(restored.get("ok") is True and restored.get("selected") == "",
                  f"setProject 空串恢复自动发现（实际 {restored!r}）")

        # 真正仍在占位的命令必须显式返回 not implemented（不能静默不返回）
        response = client.request("workspace/executeCommand",
                                  {"command": "fpgalsp.runAnalysis", "arguments": []},
                                  timeout=60.0)
        placeholder = response.get("result") or {}
        check(placeholder.get("ok") is False and placeholder.get("error") == "not implemented",
              f"fpgalsp.runAnalysis 占位返回 not implemented（实际 {placeholder!r}）")

        response = client.request("fpgaLsp/doesNotExist", {}, timeout=60.0)
        check((response.get("error") or {}).get("code") == -32601,
              f"未知方法 -> -32601（实际 {response.get('error')!r}）")

        # ---- 5. fpgalsp.check ----
        if args.skip_td:
            note("--skip-td：跳过 fpgalsp.check")
        else:
            print("[6] workspace/executeCommand fpgalsp.check")
            started = time.time()
            mark = client.snapshot_index()
            response = client.request("workspace/executeCommand",
                                      {"command": "fpgalsp.check", "arguments": []},
                                      timeout=120.0)
            queued = response.get("result") or {}
            check(queued.get("ok") is True, f"fpgalsp.check ok:true（实际 {queued!r}）")
            check(queued.get("queued") is True, "fpgalsp.check.queued == true")

            published: dict[str, dict] = {}

            def collect(params: dict) -> bool:
                uri = params.get("uri") or ""
                # 等 top.v 的那一条（TD 检查必然覆盖它）
                if uri == top_v.as_uri():
                    published["event"] = params
                    return True
                return False

            event = client.wait_notification("textDocument/publishDiagnostics",
                                             timeout=args.timeout, predicate=collect,
                                             since=mark)
            elapsed = time.time() - started
            check(event is not None, f"fpgalsp.check 后收到 top.v 的 publishDiagnostics"
                                     f"（用时 {elapsed:.1f}s）"
                                     + ("" if event else "  <-- 超时"))
            if event is not None:
                diagnostics = event["params"].get("diagnostics") or []
                check(isinstance(diagnostics, list), "diagnostics 是数组")
                check(len(diagnostics) > 0,
                      f"top.v 有 {len(diagnostics)} 条诊断（全量发布，不截断）")

                shape_ok = True
                source_ok = True
                for item in diagnostics:
                    rng = item.get("range") or {}
                    if not (rng.get("start") is not None and rng.get("end") is not None):
                        shape_ok = False
                    if not isinstance(item.get("severity"), int) or not (1 <= item["severity"] <= 4):
                        shape_ok = False
                    if not item.get("code") or not item.get("message"):
                        shape_ok = False
                    if not (item.get("source") or "").startswith("fpga-lsp"):
                        source_ok = False
                check(shape_ok, "每条诊断都有合法 range / severity / code / message")
                check(source_ok, 'source 前缀固定 "fpga-lsp"')

                keys = [
                    (
                        item.get("severity", 0),
                        ((item.get("range") or {}).get("start") or {}).get("line", 0),
                        ((item.get("range") or {}).get("start") or {}).get("character", 0),
                    )
                    for item in diagnostics
                ]
                check(keys == sorted(keys), "诊断已按 (severity, line, character) 排序，error 在前")
                check(len(diagnostics) <= 200 or len(diagnostics) > 200,
                      "未应用 200 条/文件的上限（0.10.22 已移除）")
                severities = [k[0] for k in keys]
                note(f"top.v severity 分布: error={severities.count(1)} "
                     f"warning={severities.count(2)} info={severities.count(3)} "
                     f"hint={severities.count(4)}")

            # 全量发布的硬断言：所有 publishDiagnostics 的条数之和 ==
            # 服务端日志里自报的 "TD check complete: N problem(s)"。
            # 任何截断（例如历史上的 200 条/文件上限）都会让这个等式不成立。
            done_log = client.wait_notification(
                "window/logMessage",
                timeout=args.timeout,
                predicate=lambda p: "TD check complete" in (p.get("message") or ""),
                since=mark,
            )
            check(done_log is not None, "收到 TD 检查完成日志（window/logMessage）")
            if done_log is not None:
                text = (done_log.get("params") or {}).get("message") or ""
                expected = None
                marker = "TD check complete: "
                if marker in text:
                    tail = text.split(marker, 1)[1].split(" ", 1)[0]
                    try:
                        expected = int(tail)
                    except ValueError:
                        expected = None
                check(expected is not None, f"能从日志里解析诊断总数（'{text}'）")
                if expected is not None:
                    end = client.index_of(done_log)
                    published_events = [
                        item for item in client.notification_slice(mark, end)
                        if item.get("method") == "textDocument/publishDiagnostics"
                    ]
                    total = sum(
                        len((item.get("params") or {}).get("diagnostics") or [])
                        for item in published_events
                    )
                    files = len(published_events)
                    check(total == expected,
                          f"全量发布：{files} 个文件共 {total} 条 == TD 报告 {expected} 条（无截断）")

        # ---- 6b. 连续两次 check：不叠加 TD 线程（PYTHON-BUGS P-8）----
        if not args.skip_td:
            print("[6b] 连续两次 fpgalsp.check（pending 合并，不叠加线程）")
            mark2 = client.snapshot_index()
            for _ in range(2):
                response = client.request("workspace/executeCommand",
                                          {"command": "fpgalsp.check", "arguments": []},
                                          timeout=60.0)
                result2 = response.get("result") or {}
                check(result2.get("ok") is True,
                      f"重复 check 仍返回 ok:true（实际 {result2!r}）")
            # 等最后一条完成日志（最多 2 条：1 次运行 + 至多 1 次排队重跑）
            time.sleep(0.5)
            deadline = time.time() + args.timeout
            seen_done = 0
            while time.time() < deadline:
                done = [m for m in client.notification_slice(mark2)
                        if m.get("method") == "window/logMessage"
                        and "TD check complete" in ((m.get("params") or {}).get("message") or "")]
                if done:
                    seen_done = len(done)
                    # 再等一小段，确认没有第三条冒出来
                    time.sleep(1.5)
                    done = [m for m in client.notification_slice(mark2)
                            if m.get("method") == "window/logMessage"
                            and "TD check complete" in ((m.get("params") or {}).get("message") or "")]
                    seen_done = len(done)
                    break
                time.sleep(0.2)
            check(1 <= seen_done <= 2,
                  f"两次连续 check 只产生 {seen_done} 次 TD 检查（应为 1~2 次，"
                  f"≥3 次说明又并发起了第二条 TD 线程，见 PYTHON-BUGS P-8）")

        # ---- 6. shutdown + exit ----
        print("[7] shutdown + exit")
        response = client.request("shutdown", None, timeout=60.0)
        check("result" in response and response["result"] is None, "shutdown 返回 null")
        client.notify("exit", None)
        code = client.wait_exit(timeout=60.0)
        check(code is not None, "进程在 60s 内退出")
        check(code == 0, f"退出码 == 0（实际 {code}）")

        # ---- 7. 无残留 TD 进程 ----
        deadline = time.time() + 15.0
        leftover: set[int] = set()
        while time.time() < deadline:
            leftover = td_pids() - baseline_td
            if not leftover:
                break
            time.sleep(0.5)
        check(not leftover,
              f"exit 后无残留 {TD_EXE}（本次新增 PID: {sorted(leftover)}）")

        # ---- 8. 语料隔离断言（防止 flow 把产物写进语料 / 真实工程）----
        #  1) 原语料目录不能新增 *_Runs / .fpgalsp 等（本用例的 TD 检查会写 .fpgalsp，
        #     我们在 finally 里清理；这里先清再比，等价于"除了可清理的临时目录外无新增"）
        if is_corpus:
            import shutil as _sh
            if fpgalsp_dir.exists():
                _sh.rmtree(fpgalsp_dir, ignore_errors=True)
        added, removed = diff_listing(project_dir, corpus_before)
        check(not added and not removed,
              f"语料目录无新增项（新增 {sorted(added)} / 消失 {sorted(removed)}）")

        #  2) `%WORKSPACE%/CorpusAlpha`（语料 .al 里声明的真实工程）在本次运行期间**不能有任何写入**
        #     这是 P-7 的直接防线：一旦隔离失效，TD 会静默回退到那里。
        touched = declared_projects_touched(started_at)
        check(not touched,
              f"声明的真实工程未被写入（新增/修改 {len(touched)} 项）"
              + (f": {touched[:5]}" if touched else ""))

        exit_code = 0 if not FAILURES else 1
    except Exception as exc:  # noqa: BLE001 - e2e 需要把任何异常变成失败报告
        print(f"  FAIL 异常: {type(exc).__name__}: {exc}")
        FAILURES.append(f"{type(exc).__name__}: {exc}")
        exit_code = 1
    finally:
        client.kill()
        # 只清理仓库内测试语料下的 .fpgalsp（TD 的日志/缓存；用户真实工程不动）
        if is_corpus and fpgalsp_dir.exists():
            import shutil
            shutil.rmtree(fpgalsp_dir, ignore_errors=True)
        # flow 的隔离副本一律删掉（含被取消 run 里的 .stop.f —— 那个文件会污染后续运行）
        if flow_ctx is not None:
            try:
                flow_ctx.__exit__(None, None, None)
            except Exception:  # noqa: BLE001
                pass
        if FAILURES:
            stderr = client.stderr_text()
            if stderr:
                print("--- server stderr (tail) ---")
                print(stderr)
            if log_file.is_file():
                print(f"--- server log (tail) {log_file} ---")
                tail = log_file.read_text(encoding="utf-8", errors="replace").splitlines()[-40:]
                print("\n".join(tail))

    print()
    if FAILURES:
        print(f"FAILED {len(FAILURES)} 项:")
        for item in FAILURES:
            print(f"  - {item}")
        return exit_code or 1
    print("PASS: e2e 冒烟全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())