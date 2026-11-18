#!/usr/bin/env python3
"""Run logic tests on Linux; no ESP32 or serial connection is required."""

from pathlib import Path
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    protocol = root / "components/heater_protocol"
    with tempfile.TemporaryDirectory(prefix="heater-native-") as temporary:
        controller_objects=[]
        for source in ("controllers.cpp", "heater_controller.cpp"):
            obj=Path(temporary)/(source+".o")
            subprocess.run(["c++","-std=c++17","-Wall","-Wextra","-Werror","-fsanitize=address,undefined","-g",
                            "-I",str(root/"components/heater_controller/include"),"-c",
                            str(root/"components/heater_controller"/source),"-o",str(obj)],check=True)
            controller_objects.append(str(obj))
        subprocess.run(["c++","-std=c++17","-Wall","-Wextra","-Werror","-fsanitize=address,undefined","-g",
                        "-I",str(root/"components/heater_controller/include"),str(root/"tests/test_controller.cpp"),
                        *controller_objects,"-o",str(Path(temporary)/"test_controller")],check=True)
        subprocess.run([str(Path(temporary)/"test_controller")],check=True)
        subprocess.run(["c++","-std=c++17","-Wall","-Wextra","-Werror","-fsanitize=address,undefined","-g",
                        "-I",str(root/"components/heater_controller/include"),str(root/"tests/test_controller_extension.cpp"),
                        controller_objects[1],"-o",str(Path(temporary)/"test_extension")],check=True)
        subprocess.run([str(Path(temporary)/"test_extension")],check=True)
        for test in ("test_protocol", "test_control", "test_control_limit", "test_telemetry", "test_ota"):
            binary = Path(temporary) / test
            command = [
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
                "-I", str(root / "tests/stubs"),
            ]
            for component in ("heater_protocol", "heater_control", "heater_board", "ads1220", "telemetry_store", "device_web", "heater_controller"):
                command += ["-I", str(root / "components" / component / "include")]
            command += [
                str(root / "tests" / ("test_control.c" if test == "test_control_limit" else f"{test}.c")),
                str(protocol / "heater_protocol.c"),
                str(root / "components/telemetry_store/telemetry_ring.c"),
                str(root / "components/telemetry_store/telemetry_json.c"),
                *controller_objects, "-lstdc++", "-lm", "-o", str(binary),
            ]
            if test == "test_control_limit":
                command.append("-DCONFIG_HEATER_OUTPUT_DUTY_LIMIT=2")
            if test == "test_ota":
                command.append(str(root / "components/device_web/ota_update.c"))
            subprocess.run(command, check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
