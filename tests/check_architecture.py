#!/usr/bin/env python3
"""检查组件边界与中文用途注释，让后续扩展保持当前依赖方向。"""
from pathlib import Path
import re


def main():
    root = Path(__file__).resolve().parents[1]
    components = root / "components"
    names = {p.name for p in components.iterdir() if p.is_dir()}
    graph = {}
    for name in names:
        source = (components / name / "CMakeLists.txt").read_text()
        dependencies = []
        # 只读取 register 的依赖段，不把源文件或 include 目录误认为组件。
        for match in re.finditer(r"(?:PRIV_REQUIRES|(?<!_)REQUIRES)\s+([^)]*)",
                                 source):
            dependencies += re.findall(r"[a-zA-Z0-9_]+", match[1])
        graph[name] = set(dependencies) & names
    visited, active = set(), set()

    def visit(name):
        assert name not in active, f"组件依赖循环：{name}"
        if name in visited:
            return
        active.add(name)
        for dependency in graph[name]:
            visit(dependency)
        active.remove(name)
        visited.add(name)

    for name in names:
        visit(name)
    assert graph["heater_controller"] == {"heater_domain"}
    assert graph["heater_domain"] == set()
    assert "heater_runtime" not in graph["telemetry_store"]
    assert not graph["heater_runtime"] & {
        "heater_service", "telemetry_store", "device_web", "device_usb"
    }
    worker = (components / "heater_runtime/controller_worker.c").read_text()
    assert not re.search(
        r"heater_board|runtime_internal|driver/|nvs|esp_ota|gpio_", worker)
    assert not any(p.name in {"heater_control", "network_manager", "host_link"}
                   for p in components.iterdir())
    for directory in (components, root / "main", root / "tools",
                      root / "tests"):
        for path in directory.rglob("*"):
            if path.suffix in {".c", ".h", ".cpp", ".hpp", ".js", ".py"}:
                assert re.search(r"[\u4e00-\u9fff]",
                                 path.read_text()), f"缺少中文用途注释：{path}"
    print("组件依赖、算法边界及中文用途注释检查通过")


if __name__ == "__main__":
    main()
