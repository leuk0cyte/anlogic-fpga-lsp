"""F3 协议自查：MCP `fpga_project_reports` 的键集合是否完整。

用途：Lead 的验收人报"all_stage 场景缺 ok/files/runs/stage/project/error"。
本脚本对**当前 build/bin/fpgalsp-native.exe** 直接调该工具，打印键集合，
并与基线 `doc/audit/2026-11-19/fix/baseline/mcp-raw.json` 的同类记录对比。

用法：python tools/perf/reports/check_mcp_reports.py [--project DIR]
"""

import argparse
import json
import os
import sys
import tempfile

CPP_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, os.path.join(CPP_ROOT, "tests", "golden", "tools"))
os.environ["PYTHONPATH"] = r"Y:\fpga-lsp" + os.pathsep + os.environ.get("PYTHONPATH", "")

from goldenlib.client import McpClient, NativeMcpClient  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project", default=os.path.join(
        os.environ.get("TEMP", "."), "fpgalsp-perf", "reports", "report100"))
    parser.add_argument("--exe", default=os.path.join(CPP_ROOT, "build", "bin", "fpgalsp-native.exe"))
    args = parser.parse_args()
    log_dir = os.path.join(tempfile.gettempdir(), "fpgalsp-fix", "f3")
    os.makedirs(log_dir, exist_ok=True)

    expect = ["ok", "project", "stage", "runs", "files"]
    for label, factory in (
        ("python", lambda: McpClient(CPP_ROOT, args.project, os.path.join(log_dir, "py.log"))),
        ("native", lambda: NativeMcpClient(CPP_ROOT, args.project,
                                           os.path.join(log_dir, "cpp.log"), exe_path=args.exe)),
    ):
        client = factory()
        try:
            for arguments in (
                {"stage": "all", "section": "all"},
                {"stage": "all", "section": "summary"},
                {"stage": "phy", "section": "timing"},
            ):
                payload, is_error = client.call_tool("fpga_project_reports", arguments,
                                                     timeout=180.0)
                keys = sorted(payload) if isinstance(payload, dict) else type(payload).__name__
                missing = [k for k in expect if not (isinstance(payload, dict) and k in payload)]
                print("%-6s stage=%-4s section=%-8s isError=%s keys=%s missing=%s"
                      % (label, arguments["stage"], arguments["section"], is_error, keys, missing))
        finally:
            try:
                client.close()
            except Exception:  # noqa: BLE001
                pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())