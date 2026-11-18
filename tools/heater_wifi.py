#!/usr/bin/env python3
"""HTTP telemetry capture and OTA over USB CDC-NCM, using only the Python standard library."""

import argparse
import csv
import getpass
import json
import os
from pathlib import Path
import sys
import time
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode, urlsplit
from urllib.request import Request, build_opener, ProxyHandler

CSV_FIELDS = ["boot_id", "cursor", "sequence", "cycle_start_us", "duty1", "duty2", "duty3", "duty4",
              "R1", "R2", "R3", "R4", "valid_mask", "faults", "deadline_misses", "telemetry_drops",
              "control_stack_free_bytes", "run_id", "config_id", "mode", "algorithm",
              "target1", "target2", "target3", "target4", "requested1", "requested2", "requested3", "requested4",
              "limited_mask", "blocked_mask", "source_sequence", "computation_us"]


class Device:
    def __init__(self, url, token):
        parsed = urlsplit(url)
        if parsed.scheme not in ("http", "https") or not parsed.netloc or parsed.username or parsed.query:
            raise ValueError("--url must be an http(s) device URL")
        if len(token) != 32 or any(c not in "0123456789abcdef" for c in token):
            raise ValueError("token must be the 32-character token from the device startup log")
        self.url = url.rstrip("/")
        self.token = token
        # Device traffic stays on the LAN, including when the container uses
        # an HTTP proxy for package downloads or external websites.
        self.opener = build_opener(ProxyHandler({}))

    def request(self, path, data=None, content_type="text/plain", timeout=2):
        request = Request(self.url + path, data=data,
                          headers={"Authorization": "Bearer " + self.token, "Content-Type": content_type})
        try:
            with self.opener.open(request, timeout=timeout) as response:
                return json.load(response)
        except HTTPError as error:
            body = error.read().decode("utf-8", errors="replace")
            raise RuntimeError(f"HTTP {error.code}: {body}") from error
        except OSError as error:
            raise RuntimeError(f"Network request failed: {error}") from error

    def command(self, text):
        return self.request("/api/control", text.encode("ascii"))


def batch_rows(batch, from_cursor):
    if batch.get("schema") not in (1, 2):
        raise ValueError("unsupported telemetry schema")
    rows = []
    previous = from_cursor - 1
    for frame in batch["frames"]:
        if len(frame) != (11 if batch["schema"] == 2 else 10) or len(frame[3]) != 4 or len(frame[4]) != 4:
            raise ValueError("invalid telemetry frame")
        if frame[0] < from_cursor:
            continue
        if frame[0] <= previous:
            raise ValueError("telemetry cursors are not increasing")
        previous = frame[0]
        e = frame[10] if batch["schema"] == 2 else None
        extra = [e["run_id"], e["config_id"], e["mode"], e["algorithm"], *e["target"], *e["requested"],
                 e["limited_mask"], e["blocked_mask"], e["source_sequence"], e["computation_us"]] if e else [""] * 16
        rows.append([batch["boot_id"], *frame[:3], *frame[3], *frame[4], *frame[5:10], *extra])
    if batch["next"] < from_cursor or (rows and batch["next"] != previous + 1):
        raise ValueError("invalid next cursor")
    return rows


def capture(device, csv_path, seconds, duties=None, history=False, resistance=False):
    # Opening first guarantees that an existing CSV cannot be overwritten and
    # that failure to create the output file cannot start heating.
    with Path(csv_path).open("x", newline="", encoding="utf-8") as output:
        writer = csv.writer(output)
        writer.writerow(CSV_FIELDS)
        status = device.request("/api/status")
        if "controller" in status:
            metadata = {"initial_status": status, "experiments": device.request("/api/experiments")}
            with Path(str(csv_path) + ".metadata.json").open("x", encoding="utf-8") as meta:
                json.dump(metadata, meta, indent=2, ensure_ascii=False)
        boot = status["boot_id"]
        cursor = status["store"]["oldest" if history else "next"]
        controlling = False
        requested_run = duties is not None or resistance
        try:
            if resistance:
                device.command("RSTART")
                controlling = True
            elif duties is not None:
                device.command("START " + " ".join(map(str, duties)))
                controlling = True
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                wake = time.monotonic() + 1
                try:
                    if controlling:
                        device.command("PING")
                    status = device.request("/api/status")
                    if status["boot_id"] != boot:
                        boot, cursor, controlling = status["boot_id"], 0, False
                        print("Device rebooted; starting a new data session, heating stays off", file=sys.stderr)
                    if status["maintenance"] or not status.get("running", True):
                        controlling = False
                    # Drain a backlog quickly; ordinary capture makes one request per second.
                    for _ in range(10):
                        batch = device.request("/api/data?" + urlencode({"from": cursor, "limit": 250, "boot": boot}))
                        if batch["boot_id"] != boot:
                            raise ValueError("boot changed during data transfer")
                        if batch["lost"]:
                            print(f"Cache overwritten: {batch['lost']} missing frames", file=sys.stderr)
                        rows = batch_rows(batch, cursor)
                        writer.writerows(rows)
                        output.flush()
                        os.fsync(output.fileno())
                        # Advance only after this batch is persisted on the host.
                        cursor = batch["next"]
                        if not batch["more"] or time.monotonic() >= deadline:
                            break
                    latest = status.get("latest")
                    if latest:
                        print(f"seq={latest[1]} R={latest[4]} mask={latest[5]} faults={latest[6]}")
                except (URLError, RuntimeError) as error:
                    # Retry data capture, but never resend SET after a lost connection.
                    controlling = False
                    print(f"Connection interrupted: {error}; retrying data capture", file=sys.stderr)
                time.sleep(max(0, min(wake, deadline) - time.monotonic()))
        finally:
            if requested_run:
                try:
                    device.command("STOP")
                except (OSError, URLError, RuntimeError) as error:
                    print(f"STOP not confirmed: {error}; control lease expires on device", file=sys.stderr)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", required=True, help="e.g. http://192.168.7.1")
    parser.add_argument("--token-file", type=Path, help="otherwise read HEATER_TOKEN or prompt without echo")
    commands = parser.add_subparsers(dest="action", required=True)
    monitor = commands.add_parser("monitor")
    monitor.add_argument("--csv", required=True, type=Path)
    monitor.add_argument("--seconds", type=float, default=10)
    monitor.add_argument("--history", action="store_true", help="also fetch retained device history")
    run_mode = monitor.add_mutually_exclusive_group()
    run_mode.add_argument("--duty", type=int, nargs=4)
    run_mode.add_argument("--resistance", action="store_true", help="start configured resistance controller and renew lease")
    commands.add_parser("status")
    commands.add_parser("stop")
    for action in ("start", "set"):
        item = commands.add_parser(action)
        item.add_argument("duty", type=int, nargs=4)
    commands.add_parser("reboot")
    commands.add_parser("rstart")
    controller = commands.add_parser("controller-config")
    controller.add_argument("config", type=Path, help="controller JSON configuration")
    commands.add_parser("controllers")
    ota = commands.add_parser("ota")
    ota.add_argument("firmware", type=Path, help="application .bin, not a merged flash image")
    args = parser.parse_args(argv)
    if args.action in ("start", "set") and any(d < 0 or d > 95 for d in args.duty):
        parser.error("each duty must be 0–95")
    if args.action == "monitor" and (args.seconds <= 0 or (args.duty and any(d < 0 or d > 95 for d in args.duty))):
        parser.error("seconds must be positive; each duty must be 0–95")
    token = args.token_file.read_text().strip() if args.token_file else os.environ.get("HEATER_TOKEN")
    if not token:
        token = getpass.getpass("Device token: ").strip()
    device = Device(args.url, token)
    if args.action == "monitor":
        capture(device, args.csv, args.seconds, args.duty, args.history, args.resistance)
    elif args.action == "status":
        print(json.dumps(device.request("/api/status"), indent=2, ensure_ascii=False))
    elif args.action == "stop":
        print(device.command("STOP"))
    elif args.action in ("start", "set"):
        print(device.command(args.action.upper() + " " + " ".join(map(str, args.duty))))
    elif args.action == "rstart":
        print(device.command("RSTART"))
    elif args.action == "controller-config":
        config = json.loads(args.config.read_text())
        print(device.request("/api/controller/config", json.dumps(config).encode(), "application/json"))
    elif args.action == "controllers":
        print(json.dumps(device.request("/api/controllers"), indent=2))
    elif args.action == "reboot":
        print(device.request("/api/reboot", b""))
    elif args.action == "ota":
        data = args.firmware.read_bytes()
        if not (512 <= len(data) <= 4 * 1024 * 1024) or data[0] != 0xE9:
            raise ValueError("expected an ESP application .bin no larger than 4 MiB")
        print(device.request("/api/ota", data, "application/octet-stream", timeout=90))



if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(130)
    except (OSError, ValueError, RuntimeError, URLError) as error:
        sys.exit(str(error))
