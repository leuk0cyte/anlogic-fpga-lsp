#!/usr/bin/env python3
"""verify_invalidation.py —— F4 缓存失效实测：改一个 .v 后 search_text 必须立刻返回新内容。

做法：
  1. 用唯一标记查一次（应为 0 匹配，且该标记此前不存在）；
  2. 往 `src/m0001.v` **追加一行**标记注释（size 与 mtime 都变）；
  3. 同一参数再查一次 —— 必须看到 ≥1 匹配，且指向 m0001.v；
  4. 再跑一次同一参数（缓存命中路径）—— 内容必须与第 3 步一致；
  5. 把文件恢复原样，确认标记消失（避免污染合成工程）。

同时验证"只改 size 不变的内容"（把 m0002.v 里一个字符换掉，size 不变、mtime 变）
也能立刻生效。

用法：python tools/perf/mcp/verify_invalidation.py
退出码：0 = 全部符合预期。
"""

from __future__ import annotations

import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, os.path.join(REPO, "tests", "golden", "tools"))
os.environ.setdefault("PYTHONPATH", r"Y:\fpga-lsp")

from goldenlib.client import NativeMcpClient  # noqa: E402

ROOT = os.path.join(os.environ.get("TEMP", r"C:\Windows\Temp"), "fpgalsp-perf", "mcp", "proj200")


def matches(client, query: str) -> tuple[int, int, list[str]]:
    result, is_error = client.call_tool("fpga_search_text", {"query": query, "limit": 50}, timeout=120)
    if is_error or not isinstance(result, dict):
        raise RuntimeError("search_text failed: %r" % (result,))
    files = sorted({item.get("relative", "") for item in result.get("matches", [])})
    return result.get("count", 0), result.get("files", 0), files


def main() -> int:
    if not os.path.isdir(ROOT):
        print("missing synthetic project: %s" % ROOT)
        return 2
    target1 = os.path.join(ROOT, "src", "m0001.v")
    target2 = os.path.join(ROOT, "src", "m0002.v")
    marker = "INVALIDATION_MARKER_7F3A"
    body1 = open(target1, "rb").read()
    body2 = open(target2, "rb").read()
    cwd = os.path.join(os.environ.get("TEMP", "."), "fpgalsp-perf", "cwd")
    os.makedirs(cwd, exist_ok=True)
    client = NativeMcpClient(cwd, ROOT, os.path.join(os.environ.get("TEMP", "."),
                                                     "fpgalsp-perf", "mcp", "invalidate.log"))
    failures = []
    try:
        count0, files0, _ = matches(client, marker)
        print("1) marker before edit: count=%d files=%d" % (count0, files0))
        if count0 != 0:
            failures.append("marker should be absent before edit")

        with open(target1, "ab") as handle:
            handle.write(("\n// %s\n" % marker).encode("utf-8"))
        count1, files1, hits1 = matches(client, marker)
        print("2) marker after append: count=%d files=%d hits=%s" % (count1, files1, hits1))
        if count1 < 1 or "m0001.v" not in " ".join(hits1):
            failures.append("append not visible to search_text (size+mtime change)")
        count1b, _, hits1b = matches(client, marker)
        print("3) marker cached read: count=%d hits=%s" % (count1b, hits1b))
        if hits1b != hits1:
            failures.append("cached read differs from fresh read")

        # size 不变、内容变化的场景：把 acc 换成 acx（同样 3 字节）
        text2 = body2.decode("utf-8", "replace")
        changed = text2.replace("acc  <= acc + din;", "acx  <= acc + din;", 1)
        if changed == text2:
            print("4) skip same-size case (pattern not found)")
        else:
            with open(target2, "wb") as handle:
                handle.write(changed.encode("utf-8"))
            new_count, _, new_hits = matches(client, "acx  <= acc + din")
            print("4) same-size edit visible: count=%d hits=%s" % (new_count, new_hits))
            if new_count < 1 or "m0002.v" not in " ".join(new_hits):
                failures.append("same-size content change not visible (mtime must invalidate)")

        # 恢复
        with open(target1, "wb") as handle:
            handle.write(body1)
        with open(target2, "wb") as handle:
            handle.write(body2)
        count_after, _, _ = matches(client, marker)
        print("5) marker after restore: count=%d" % count_after)
        if count_after != 0:
            failures.append("marker still visible after restore")
    finally:
        client.close()

    if failures:
        print("\nFAIL: " + "; ".join(failures))
        return 1
    print("\nPASS: cache invalidation is immediate (append + same-size edit + restore)")
    return 0


if __name__ == "__main__":
    sys.exit(main())