#!/usr/bin/env python3
"""Monitor the USB protocol; optionally set four duties and send STOP on exit."""

import argparse
import csv
from pathlib import Path
import sys
import time

import serial


COLUMNS = [
    "sequence", "cycle_start_us", "duty1", "duty2", "duty3", "duty4",
    "resistance1_ohm", "resistance2_ohm", "resistance3_ohm", "resistance4_ohm",
    "valid_mask", "faults", "deadline_misses", "telemetry_drops", "control_stack_free_bytes",
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-p", "--port", required=True, help="Prefer /dev/serial/by-id/...")
    parser.add_argument("--duty", type=int, nargs=4, metavar=("D1", "D2", "D3", "D4"),
                        help="Set four 0-95 percent duties once; send STOP on exit")
    parser.add_argument("--seconds", type=float, help="Stop after this recording duration")
    parser.add_argument("--csv", type=Path, help="Save DATA frames to a new CSV file")
    args = parser.parse_args()
    if args.duty and any(not 0 <= value <= 95 for value in args.duty):
        parser.error("each duty must be an integer from 0 to 95")
    if args.seconds is not None and args.seconds <= 0:
        parser.error("--seconds must be positive")

    with serial.Serial(args.port, 115200, timeout=0.1, write_timeout=0.5, exclusive=True) as connection:
        output = args.csv.open("x", newline="") if args.csv else None
        writer = csv.writer(output) if output else None
        if writer:
            writer.writerow(COLUMNS)
        try:
            if args.duty:
                connection.write(("SET " + " ".join(map(str, args.duty)) + "\n").encode("ascii"))
            started = time.monotonic()
            pending = bytearray()
            previous_sequence = None
            while args.seconds is None or time.monotonic() - started < args.seconds:
                pending.extend(connection.read(connection.in_waiting or 1))
                while b"\n" in pending:
                    raw, _, remainder = pending.partition(b"\n")
                    pending = bytearray(remainder)
                    line = raw.decode("ascii", errors="replace").rstrip("\r")
                    if line.startswith("DATA,"):
                        fields = line.split(",")[1:]
                        if len(fields) != len(COLUMNS):
                            print(f"Malformed DATA: {line}", file=sys.stderr)
                            continue
                        try:
                            sequence = int(fields[0])
                            valid_mask, faults = int(fields[10]), int(fields[11])
                        except ValueError:
                            print(f"Malformed DATA: {line}", file=sys.stderr)
                            continue
                        if previous_sequence is not None and sequence != ((previous_sequence + 1) & 0xffffffff):
                            print(f"Sequence gap: {previous_sequence} -> {sequence}", file=sys.stderr)
                        previous_sequence = sequence
                        if writer:
                            writer.writerow(fields)
                        print(line, flush=True)
                        if valid_mask != 15 or faults:
                            print(f"Frame {sequence}: valid=0x{valid_mask:x}, faults=0x{faults:x}",
                                  file=sys.stderr)
                    elif line:
                        print(line, file=sys.stderr, flush=True)
                if len(pending) > 4096:
                    pending.clear()
                    print("Discarded oversized unterminated input", file=sys.stderr)
        except KeyboardInterrupt:
            pass
        finally:
            if args.duty:
                try:
                    connection.write(b"STOP\n")
                    connection.flush()
                except serial.SerialException:
                    pass
            if output:
                output.close()


if __name__ == "__main__":
    main()
