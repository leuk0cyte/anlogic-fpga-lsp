"""评审用最小复现：DocumentStore 文本缓存能否被"同大小 + 同 mtime"改写骗过。

只读源码，不改产品代码；工程与文件全部在 %TEMP% 里。

做法：
  1. 把 tests/gate-corpus/corpus_alpha 拷到 %TEMP%\\fpgalsp-review\\proj；
  2. 用一个**故意未格式化**的 100 KB 级文件当靶子（保证 formatting 有可见差异）；
  3. 起服务器 -> formatting（此时文本进 (path,size,mtime) 缓存）；
  4. 用**同样字节数**的另一份内容覆盖，并 `SetLastWriteTimeUtc` 写回原 mtime；
  5. 再 formatting：如果返回的 newText 还是第一份内容的格式化结果 -> 缓存命中了陈旧文本。

用法：python tools/perf/reports/check_docstore_cache_stale.py
"""

import argparse
import json
import os
import shutil
import sys
import tempfile
import time

CPP_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, os.path.join(CPP_ROOT, "tests", "golden", "tools"))
os.environ["PYTHONPATH"] = r"Y:\fpga-lsp" + os.pathsep + os.environ.get("PYTHONPATH", "")

from goldenlib.client import NativeLspClient, path_to_uri  # noqa: E402


def make_text(tag: str, target_bytes: int) -> str:
    """生成一份"未格式化、含唯一标记"的 Verilog 文本，长度精确到 target_bytes。"""
    head = "module stale_probe (input   clk ,output   reg   q);\n"
    body = []
    size = len(head) + len("endmodule\n")
    index = 0
    while size < target_bytes - 64:
        line = "  always @(posedge clk) begin q <=  %s_%06d ; end\n" % (tag, index)
        body.append(line)
        size += len(line)
        index += 1
    tail = "endmodule\n"
    pad = target_bytes - (len(head) + sum(len(x) for x in body) + len(tail))
    if pad > 0:
        head = head.rstrip("\n") + " " * pad + "\n"
    return head + "".join(body) + tail


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", default=os.path.join(CPP_ROOT, "build", "bin",
                                                      "fpgalsp-native.exe"))
    parser.add_argument("--size", type=int, default=100_000)
    args = parser.parse_args()

    root = os.path.join(tempfile.gettempdir(), "fpgalsp-review")
    proj = os.path.join(root, "proj")
    log_dir = os.path.join(root, "logs")
    os.makedirs(log_dir, exist_ok=True)
    if os.path.isdir(proj):
        shutil.rmtree(proj, ignore_errors=True)
    shutil.copytree(os.path.join(CPP_ROOT, "tests", "gate-corpus", "corpus_alpha"), proj)

    target = os.path.join(proj, "src", "stale_probe.v")
    first = make_text("ALPHA", args.size)
    with open(target, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(first)
    import ctypes
    from ctypes import wintypes

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

    def get_filetime(path):
        h = kernel32.CreateFileW(ctypes.c_wchar_p(path), 0x80, 0x1 | 0x2, None, 3, 0x80, None)
        if h == -1:
            raise OSError(ctypes.get_last_error(), "CreateFileW failed")
        ft = wintypes.FILETIME()
        kernel32.GetFileTime(h, None, None, ctypes.byref(ft))
        kernel32.CloseHandle(h)
        return (ft.dwHighDateTime << 32) | ft.dwLowDateTime

    def set_filetime(path, value):
        h = kernel32.CreateFileW(ctypes.c_wchar_p(path), 0x100, 0x1 | 0x2, None, 3, 0x80, None)
        if h == -1:
            raise OSError(ctypes.get_last_error(), "CreateFileW failed")
        ft = wintypes.FILETIME(value & 0xFFFFFFFF, value >> 32)
        ok = kernel32.SetFileTime(h, None, None, ctypes.byref(ft))
        kernel32.CloseHandle(h)
        return bool(ok)

    stamp = os.stat(target)
    saved_ft = get_filetime(target)
    print("target=%s size=%d filetime=%d" % (target, stamp.st_size, saved_ft))

    client = NativeLspClient(CPP_ROOT, os.path.join(log_dir, "review.log"), exe_path=args.exe)
    try:
        client.initialize(proj, timeout=300.0)
        uri = path_to_uri(target)

        def fmt():
            return client.request("textDocument/formatting",
                                  {"textDocument": {"uri": uri},
                                   "options": {"tabSize": 4, "insertSpaces": True}},
                                  timeout=120.0)

        first_edit = fmt()
        first_text = first_edit[0]["newText"] if first_edit else ""
        print("1) 首次 formatting: edits=%d newText=%d B 含 ALPHA=%s"
              % (len(first_edit or []), len(first_text), "ALPHA" in first_text))

        # ---- 同大小改写 + 还原 mtime（模拟 git checkout / tar 解包 / TD 原地重写）----
        second = make_text("BETA", args.size)
        assert len(second) == len(first), (len(first), len(second))
        with open(target, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(second)
        # 用 Win32 GetFileTime/SetFileTime 精确还原 100 ns 的 mtime
        ok = set_filetime(target, saved_ft)
        after = os.stat(target)
        now_ft = get_filetime(target)
        print("2) 同大小改写后: size=%d filetime=%s set_ok=%s stamp_unchanged=%s"
              % (after.st_size, now_ft, ok, now_ft == saved_ft))
        if not ok:
            print("   !! 还原 mtime 失败，本测试无效")
            return 2

        second_edit = fmt()
        second_text = second_edit[0]["newText"] if second_edit else ""
        print("3) 二次 formatting: edits=%d newText=%d B 含 ALPHA=%s 含 BETA=%s"
              % (len(second_edit or []), len(second_text), "ALPHA" in second_text,
                 "BETA" in second_text))
        stale = ("ALPHA" in second_text) and ("BETA" not in second_text)
        print("结论: %s" % ("**陈旧的缓存文本（CONFIRMED）**" if stale
                            else "读到新内容（缓存未命中，OK）"))
        return 0 if not stale else 1
    finally:
        try:
            client.close()
        except Exception:  # noqa: BLE001
            pass


if __name__ == "__main__":
    raise SystemExit(main())