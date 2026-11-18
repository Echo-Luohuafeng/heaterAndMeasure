#!/usr/bin/env python3
"""独立编译生产组件并在 Linux 上执行故障注入，不连接硬件。"""
from pathlib import Path
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    components = root / "components"
    includes = ["-I", str(root / "tests/stubs")]
    for directory in components.iterdir():
        if (directory / "include").is_dir():
            includes += ["-I", str(directory / "include")]
    includes += ["-I", str(components / "heater_runtime/private")]
    with tempfile.TemporaryDirectory(prefix="heater-native-") as temporary:
        objects = []
        for name in ("controllers.cpp", "pid_controller.cpp",
                     "heater_controller.cpp"):
            target = str(Path(temporary) / (name + ".o"))
            subprocess.run([
                "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-g", *includes, "-c",
                str(components / "heater_controller" / name), "-o", target
            ],
                           check=True)
            objects.append(target)
        for name in ("test_controller", "test_controller_extension"):
            selected = objects if name == "test_controller" else objects[-1:]
            target = str(Path(temporary) / name)
            subprocess.run([
                "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-g", *includes,
                str(root / "tests" / (name + ".cpp")), *selected, "-o", target
            ],
                           check=True)
            subprocess.run([target], check=True)
        for name in ("test_protocol", "test_control", "test_telemetry",
                     "test_ota", "test_store", "test_maintenance",
                     "test_service"):
            target = str(Path(temporary) / name)
            source = name
            files = [
                root / "tests" / (source + ".c"),
                components / "heater_domain/measurement.c",
                components / "heater_protocol/heater_protocol.c",
                components / "telemetry_store/telemetry_ring.c",
                components / "device_web/telemetry_json.c"
            ]
            if source == "test_control":
                files += list((components / "heater_runtime").glob("*.c"))
            if name == "test_ota":
                files.append(components / "device_maintenance/ota_update.c")
            if name == "test_store":
                files.append(components / "telemetry_store/telemetry_store.c")
            if name == "test_maintenance":
                files.append(components /
                             "device_maintenance/device_maintenance.c")
            if name == "test_service":
                files.append(components / "heater_service/heater_service.c")
            command = [
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
                *includes, *map(str, files), *objects, "-lstdc++", "-lm", "-o",
                target
            ]
            subprocess.run(command, check=True)
            subprocess.run([target], check=True)


if __name__ == "__main__":
    main()
