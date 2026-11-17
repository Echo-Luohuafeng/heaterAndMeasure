"""Exercise the real capture client against HTTP, including replay and STOP."""
import csv
import importlib.util
import json
from pathlib import Path
import tempfile
from threading import Thread
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("heater_wifi", ROOT / "tools/heater_wifi.py")
CLIENT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CLIENT)
TOKEN = "0123456789abcdef0123456789abcdef"
FRAME = [1, 7, 20000, [1, 2, 3, 4], [100.0, None, 120.0, 130.0], 13, 2, 0, 0, 2048]


class Handler(BaseHTTPRequestHandler):
    commands = []
    boot = "boot"
    restart_after_read = False

    def log_message(self, *args):
        pass

    def send_json(self, body, code=200):
        encoded = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)

    def authenticate(self):
        if self.headers.get("Authorization") == "Bearer " + TOKEN:
            return True
        self.send_json({"error": "AUTH_REQUIRED"}, 401)
        return False

    def do_GET(self):
        if not self.authenticate():
            return
        parsed = urlsplit(self.path)
        if parsed.path == "/api/status":
            self.send_json({"boot_id": self.boot, "maintenance": False, "latest": FRAME,
                            "store": {"oldest": 1, "next": 2}})
        else:
            cursor = int(parse_qs(parsed.query)["from"][0])
            self.send_json({"schema": 1, "boot_id": self.boot, "next": 2, "lost": 0,
                            "more": False, "frames": [FRAME] if cursor < 2 else []})
            if self.restart_after_read:
                Handler.boot = "new-boot"
                Handler.restart_after_read = False

    def do_POST(self):
        if not self.authenticate():
            return
        command = self.rfile.read(int(self.headers["Content-Length"])).decode()
        self.commands.append(command)
        self.send_json({"ack": command.split()[0]})


class ClientTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        cls.thread = Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.device = CLIENT.Device(f"http://127.0.0.1:{cls.server.server_port}", TOKEN)

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()

    def setUp(self):
        Handler.commands.clear()
        Handler.boot = "boot"
        Handler.restart_after_read = False

    def test_capture_csv_and_stop(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "samples.csv"
            CLIENT.capture(self.device, path, .05, [1, 2, 3, 4], history=True)
            with path.open() as stream:
                rows = list(csv.reader(stream))
            self.assertEqual(rows[0], CLIENT.CSV_FIELDS)
            self.assertEqual(len(rows), 2)
            self.assertEqual(rows[1][:4], ["boot", "1", "7", "20000"])
            self.assertEqual(rows[1][9], "")  # invalid channel is blank in CSV
            self.assertEqual(Handler.commands[0], "SET 1 2 3 4")
            self.assertIn("PING", Handler.commands)
            self.assertEqual(Handler.commands[-1], "STOP")
            Handler.commands.clear()
            with self.assertRaises(FileExistsError):
                CLIENT.capture(self.device, path, .05, [1, 2, 3, 4])
            self.assertEqual(Handler.commands, [])

    def test_read_only_capture_sends_no_command(self):
        with tempfile.TemporaryDirectory() as directory:
            CLIENT.capture(self.device, Path(directory) / "samples.csv", .05, history=True)
        self.assertEqual(Handler.commands, [])

    def test_auth_failure_and_deduplication(self):
        wrong = CLIENT.Device(self.device.url, "0" * 32)
        with self.assertRaisesRegex(RuntimeError, "AUTH_REQUIRED"):
            wrong.request("/api/status")
        batch = {"schema": 1, "boot_id": "boot", "next": 2, "frames": [FRAME]}
        self.assertEqual(CLIENT.batch_rows(batch, 2), [])
        with self.assertRaises(ValueError):
            CLIENT.batch_rows({**batch, "next": 1}, 1)

    def test_reboot_creates_new_session_without_repeating_set(self):
        Handler.restart_after_read = True
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "reboot.csv"
            CLIENT.capture(self.device, path, 1.05, [5, 0, 0, 0], history=True)
            with path.open() as stream:
                rows = list(csv.DictReader(stream))
        self.assertEqual([row["boot_id"] for row in rows], ["boot", "new-boot"])
        self.assertEqual(sum(command.startswith("SET") for command in Handler.commands), 1)
        self.assertEqual(Handler.commands[-1], "STOP")


if __name__ == "__main__":
    unittest.main()
