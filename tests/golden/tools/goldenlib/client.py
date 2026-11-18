"""Stdio JSON-RPC clients for the fpga-lsp reference implementation.

Two transports are needed by the golden collector:

* :class:`LspClient` speaks Content-Length framed LSP over ``python -m fpgalsp --stdio``,
  started exactly the way ``tests/e2e_lsp.py`` starts it (same argv, same cwd, same env).
* :class:`McpClient` speaks newline-delimited JSON-RPC over ``python -m fpgalsp.mcp``.

Both clients are deliberately strict about read-only behaviour: nothing here writes to the
workspace under test.  The only file either client creates is the server's own log file,
which the caller points at a throw-away location.
"""

from __future__ import annotations

import json
import os
import queue
import subprocess
import sys
import threading
import time

DOCUMENT_WAIT_TIMEOUT = 120.0


def path_to_uri(path: str) -> str:
    """Match the reference implementation's own URI spelling (``file:///Y:/x``)."""
    return "file:///" + os.path.abspath(path).replace("\\", "/")


def uri_to_path(uri: str) -> str:
    if uri.startswith("file:///"):
        return uri[len("file:///"):]
    if uri.startswith("file://"):
        return uri[len("file://"):]
    return uri


class TransportError(RuntimeError):
    """Raised when the child process dies or a request cannot be answered."""


class _BaseClient:
    def __init__(self, argv, cwd, log_path, child_env=None):
        env = dict(os.environ, PYTHONIOENCODING="utf-8", PYTHONUNBUFFERED="1")
        if child_env:
            env.update(child_env)
        self._log_handle = None
        self.log_path = log_path
        if log_path:
            os.makedirs(os.path.dirname(log_path), exist_ok=True)
        self.proc = subprocess.Popen(
            argv, cwd=cwd,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=env, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        self._next_id = 0
        self._responses = queue.Queue()
        self.notifications = []
        self._stderr_tail = []
        self._dead = False
        threading.Thread(target=self._reader, daemon=True).start()
        threading.Thread(target=self._stderr_reader, daemon=True).start()

    # -- subclass hooks -------------------------------------------------------
    def _reader(self):
        raise NotImplementedError

    def _write(self, payload: bytes):
        raise NotImplementedError

    # -- shared plumbing ------------------------------------------------------
    def _stderr_reader(self):
        for line in self.proc.stderr:
            self._stderr_tail.append(line.decode("utf-8", "replace").rstrip())
            if len(self._stderr_tail) > 40:
                del self._stderr_tail[0]

    def stderr_tail(self) -> str:
        return "\n".join(self._stderr_tail)

    def _dispatch(self, msg):
        if msg.get("id") is not None and ("result" in msg or "error" in msg):
            self._responses.put(msg)
        if msg.get("method"):
            self.notifications.append(msg)

    def send(self, method, params=None, msg_id=None):
        msg = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        if msg_id is not None:
            msg["id"] = msg_id
        self._write(json.dumps(msg, ensure_ascii=False).encode("utf-8"))

    def notify(self, method, params=None):
        self.send(method, params)

    def request(self, method, params=None, timeout=60.0):
        """Send a request and return its ``result``.

        Raises :class:`TransportError` for a JSON-RPC ``error`` reply, a timeout, or a dead
        child.  Callers that want to record a soft failure catch this themselves.
        """
        self._next_id += 1
        msg_id = self._next_id
        self.send(method, params, msg_id)
        deadline = time.time() + timeout
        while True:
            remaining = deadline - time.time()
            if remaining <= 0:
                raise TransportError("timeout after %.0fs waiting for %s" % (timeout, method))
            try:
                msg = self._responses.get(timeout=min(remaining, 0.25))
            except queue.Empty:
                if self._dead and self._responses.empty():
                    raise TransportError("server exited while waiting for %s%s"
                                         % (method, self._stderr_suffix()))
                continue
            if msg.get("id") == msg_id:
                if "error" in msg:
                    raise TransportError("JSON-RPC error for %s: %s"
                                         % (method, json.dumps(msg["error"], ensure_ascii=False)))
                return msg.get("result")

    def _stderr_suffix(self) -> str:
        tail = self.stderr_tail()
        return ("\n--- server stderr ---\n" + tail) if tail else ""

    def wait_notification(self, method, timeout=DOCUMENT_WAIT_TIMEOUT, predicate=None):
        """Block until a notification with ``method`` (optionally matching ``predicate``) arrives."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            for msg in list(self.notifications):
                if msg.get("method") == method and (predicate is None or predicate(msg)):
                    return msg
            time.sleep(0.05)
        return None

    def clear_notifications(self):
        self.notifications.clear()

    def close(self, graceful=True):
        if self.proc.poll() is None:
            if graceful:
                try:
                    self._next_id += 1
                    self.send("shutdown", {}, self._next_id)
                    deadline = time.time() + 8
                    while time.time() < deadline:
                        try:
                            msg = self._responses.get(timeout=0.25)
                        except queue.Empty:
                            continue
                        if msg.get("id") == self._next_id:
                            break
                    self.notify("exit")
                except Exception:
                    pass
            try:
                self.proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                try:
                    self.proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    pass
        for stream in (self.proc.stdin, self.proc.stdout, self.proc.stderr):
            try:
                if stream:
                    stream.close()
            except Exception:
                pass


class LspClient(_BaseClient):
    """``python -m fpgalsp --stdio`` driven over Content-Length framed JSON-RPC."""

    def __init__(self, repo_root, log_path, extra_args=None):
        argv = [sys.executable, "-m", "fpgalsp", "--stdio"]
        if log_path:
            argv += ["--log", log_path]
        argv += list(extra_args or [])
        # tests/e2e_lsp.py runs the server with cwd=<repo root>, so that "fpgalsp" resolves
        # from the source tree rather than whatever the caller's cwd happens to be.
        super().__init__(argv, cwd=repo_root, log_path=log_path)

    def _write(self, payload: bytes):
        if self.proc.stdin is None or self.proc.stdin.closed:
            raise TransportError("stdin closed")
        self.proc.stdin.write(b"Content-Length: %d\r\n\r\n" % len(payload) + payload)
        self.proc.stdin.flush()

    def _reader(self):
        stream = self.proc.stdout
        try:
            while True:
                headers = {}
                while True:
                    line = stream.readline()
                    if not line:
                        self._dead = True
                        return
                    line = line.strip()
                    if not line:
                        break
                    key, _, value = line.partition(b":")
                    headers[key.strip().lower()] = value.strip()
                try:
                    length = int(headers.get(b"content-length", b"0"))
                except ValueError:
                    self._dead = True
                    return
                body = stream.read(length)
                if not body:
                    self._dead = True
                    return
                try:
                    msg = json.loads(body.decode("utf-8", "replace"))
                except ValueError:
                    continue
                self._dispatch(msg)
        except Exception:
            self._dead = True

    # -- convenience ----------------------------------------------------------
    def initialize(self, root_path, timeout=180.0, extra_options=None):
        options = {"fpgaLsp": {"tdCheckOnSave": False}}
        if extra_options:
            for key, value in extra_options.items():
                options.setdefault(key, value)
        result = self.request("initialize", {
            "processId": os.getpid(),
            "rootUri": path_to_uri(root_path),
            "clientInfo": {"name": "fpga-lsp-cpp-golden-collector", "version": "1"},
            "capabilities": {
                "textDocument": {
                    "completion": {}, "hover": {}, "documentSymbol": {},
                    "semanticTokens": {}, "foldingRange": {}, "inlayHint": {},
                },
                "workspace": {"symbol": {}},
            },
            "initializationOptions": options,
        }, timeout=timeout)
        self.notify("initialized", {})
        return result

    def open_document(self, path, language_id, text=None, version=1):
        if text is None:
            with open(path, encoding="utf-8", errors="replace") as handle:
                text = handle.read()
        self.notify("textDocument/didOpen", {"textDocument": {
            "uri": path_to_uri(path), "languageId": language_id,
            "version": version, "text": text}})
        return text

    def wait_diagnostics_for(self, path, timeout=DOCUMENT_WAIT_TIMEOUT):
        """Return the last ``publishDiagnostics`` payload for ``path`` (``None`` on timeout)."""
        uri = path_to_uri(path)
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            for msg in list(self.notifications):
                if msg.get("method") != "textDocument/publishDiagnostics":
                    continue
                params = msg.get("params") or {}
                if params.get("uri") != uri:
                    continue
                last = params.get("diagnostics") or []
                if last:
                    return last
            time.sleep(0.05)
        return last


class McpClient(_BaseClient):
    """``python -m fpgalsp.mcp`` driven over newline-delimited JSON-RPC."""

    def __init__(self, repo_root, root_path, log_path, extra_args=None):
        argv = [sys.executable, "-m", "fpgalsp.mcp", "--root", root_path]
        if log_path:
            argv += ["--log", log_path]
        argv += list(extra_args or [])
        super().__init__(argv, cwd=repo_root, log_path=log_path)
        self._handshake()

    def _write(self, payload: bytes):
        if self.proc.stdin is None or self.proc.stdin.closed:
            raise TransportError("stdin closed")
        self.proc.stdin.write(payload + b"\n")
        self.proc.stdin.flush()

    def _reader(self):
        stream = self.proc.stdout
        try:
            while True:
                line = stream.readline()
                if not line:
                    self._dead = True
                    return
                line = line.strip()
                if not line:
                    continue
                try:
                    msg = json.loads(line.decode("utf-8", "replace"))
                except ValueError:
                    continue
                self._dispatch(msg)
        except Exception:
            self._dead = True

    def _handshake(self):
        self.request("initialize", {
            "protocolVersion": "2024-11-05",
            "capabilities": {},
            "clientInfo": {"name": "fpga-lsp-cpp-golden-collector", "version": "1"},
        }, timeout=180.0)
        self.notify("notifications/initialized", {})

    def list_tools(self, timeout=120.0):
        return self.request("tools/list", {}, timeout=timeout) or {}

    def call_tool(self, name, arguments=None, timeout=120.0):
        """Return ``(structured_content_or_text, is_error)`` for one ``tools/call``."""
        result = self.request("tools/call",
                              {"name": name, "arguments": arguments or {}}, timeout=timeout) or {}
        is_error = bool(result.get("isError"))
        structured = result.get("structuredContent")
        if structured is not None:
            return structured, is_error
        blocks = result.get("content") or []
        texts = [block.get("text") for block in blocks if isinstance(block, dict)
                 and block.get("type") == "text" and block.get("text") is not None]
        text = "\n".join(texts)
        try:
            return json.loads(text), is_error
        except ValueError:
            return text, is_error