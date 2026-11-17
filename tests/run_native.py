#!/usr/bin/env python3
"""Run logic tests on Linux; no ESP32 or serial connection is required."""

from pathlib import Path
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    protocol = root / "components/heater_protocol"
    with tempfile.TemporaryDirectory(prefix="heater-native-") as temporary:
        for test in ("test_protocol", "test_control"):
            binary = Path(temporary) / test
            command = [
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
                "-I", str(root / "tests/stubs"),
            ]
            for component in ("heater_protocol", "heater_control", "heater_board", "ads1220"):
                command += ["-I", str(root / "components" / component / "include")]
            command += [
                str(root / "tests" / f"{test}.c"),
                str(protocol / "heater_protocol.c"), "-lm", "-o", str(binary),
            ]
            subprocess.run(command, check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
