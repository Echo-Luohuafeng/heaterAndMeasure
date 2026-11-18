#!/usr/bin/env python3
# 主机工具与自动验证入口，不连接硬件进行隐式控制。
"""Run network client tests and the embedded dashboard interaction test."""
from pathlib import Path
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
subprocess.run(
    [sys.executable, str(root / "tests/test_http_client.py")], check=True)
subprocess.run([sys.executable,
                str(root / "tests/test_control_metrics.py")],
               check=True)
node = shutil.which("node")
if not node:
    candidates = sorted(
        Path("/vscode/vscode-server/bin/linux-x64").glob("*/node"))
    if candidates:
        node = str(candidates[-1])
if not node:
    sys.exit("Node.js is required for the dashboard test")
subprocess.run([node, str(root / "tests/test_dashboard.js")], check=True)
