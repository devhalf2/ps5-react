# Copyright (C) 2026 half144 and PS5 React contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional attribution term: see LICENSE-ATTRIBUTION.
"""The PKG-by-URL spike tools (tools/spike/pkg-url) against local sockets; no console."""
import contextlib
import http.client
import io
import json
from pathlib import Path
import socket
import struct
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent / "spike/pkg-url"))
import content_id  # noqa: E402
import send  # noqa: E402
import serve  # noqa: E402
import summary  # noqa: E402

DATA = bytes(range(256)) * 1024  # 256 KiB, every offset recognizable.


class RangeTest(unittest.TestCase):
    def test_ranges(self):
        size = 1000
        self.assertIsNone(serve.parse_range(None, size))
        self.assertEqual(serve.parse_range("bytes=0-65535", size), (0, 999))
        self.assertEqual(serve.parse_range("bytes=10-19", size), (10, 19))
        self.assertEqual(serve.parse_range("bytes=990-", size), (990, 999))
        self.assertEqual(serve.parse_range("bytes=-10", size), (990, 999))
        self.assertEqual(serve.parse_range("bytes=-5000", size), (0, 999))
        self.assertEqual(serve.parse_range(" Bytes = 1-2", size), (1, 2))
        for ignored in ("items=0-1", "bytes=0-1,5-6", "bytes=x-y", "bytes=5-1", "bytes=5"):
            self.assertIsNone(serve.parse_range(ignored, size), ignored)
        for unsatisfiable in ("bytes=1000-", "bytes=2000-3000", "bytes=-0"):
            with self.assertRaises(serve.Unsatisfiable, msg=unsatisfiable):
                serve.parse_range(unsatisfiable, size)


class ServerTest(unittest.TestCase):
    def start(self, **options):
        self.tmp = tempfile.TemporaryDirectory(prefix="ps5-react-pkg-url-")
        root = Path(self.tmp.name)
        (root / "files").mkdir()
        (root / "files/t1.pkg").write_bytes(DATA)
        self.log_path = root / "serve.jsonl"
        self.state = serve.State(root / "files", self.log_path, echo=False, **options)
        self.server = serve.Server(("127.0.0.1", 0), self.state)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.port = self.server.server_address[1]

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.state.log_file.close()
        self.tmp.cleanup()

    def request(self, method, path, headers=None, connection=None):
        own = connection is None
        connection = connection or http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        connection.request(method, path, headers=headers or {})
        response = connection.getresponse()
        body = response.read()
        if own:
            connection.close()
        return response, body

    def records(self):
        return [json.loads(line) for line in self.log_path.read_text().splitlines()]

    def test_installer_pattern(self):
        self.start()
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        query = "?product=0287&serverIpAddr=192.168.1.10&r=12345"
        response, body = self.request("HEAD", "/t1.pkg" + query, connection=connection)
        self.assertEqual((response.status, body), (200, b""))
        self.assertEqual(response.getheader("Content-Length"), str(len(DATA)))
        self.assertEqual(response.getheader("Accept-Ranges"), "bytes")
        self.assertTrue(response.getheader("ETag") and response.getheader("Last-Modified"))

        # The same keep-alive connection serves the ranged reads.
        response, body = self.request("GET", "/t1.pkg" + query, {"Range": "bytes=65536-131071"}, connection)
        self.assertEqual(response.status, 206)
        self.assertEqual(response.getheader("Content-Range"), "bytes 65536-131071/%d" % len(DATA))
        self.assertEqual(response.getheader("Content-Length"), "65536")
        self.assertEqual(body, DATA[65536:131072])
        response, body = self.request("GET", "/t1.pkg", {"Range": "bytes=-16"}, connection)
        self.assertEqual((response.status, body), (206, DATA[-16:]))
        response, body = self.request("GET", "/t1.pkg", connection=connection)
        self.assertEqual((response.status, body), (200, DATA))
        response, _ = self.request("GET", "/t1.pkg", {"Range": "bytes=999999999-"}, connection)
        self.assertEqual(response.status, 416)
        self.assertEqual(response.getheader("Content-Range"), "bytes */%d" % len(DATA))

        response, _ = self.request("GET", "/UP0001-CUSA04286_00-XXXXXXXXXXXXXXXX.crc" + query, connection=connection)
        self.assertEqual(response.status, 404)
        (Path(self.tmp.name) / "files/t1.crc").write_bytes(b"x")  # A .crc 404s even when the file exists.
        self.assertEqual(self.request("GET", "/t1.crc", connection=connection)[0].status, 404)
        for outside in ("/../serve.jsonl", "/%2e%2e/serve.jsonl", "/", "/missing.pkg"):
            self.assertEqual(self.request("GET", outside, connection=connection)[0].status, 404, outside)

        records = self.records()
        head = records[0]
        self.assertEqual((head["method"], head["path"], head["status"], head["bytes"]), ("HEAD", "/t1.pkg", 200, 0))
        self.assertEqual(head["query"], query[1:])
        self.assertIn("ts", head)
        self.assertIn("duration", head)
        self.assertEqual(head["open"], 1)
        self.assertTrue(any(name == "Host" for name, _ in head["headers"]))
        ranged = records[1]
        self.assertEqual((ranged["range"], ranged["status"], ranged["bytes"]), ("bytes=65536-131071", 206, 65536))
        self.assertEqual(len({record["client"] for record in records}), 1, "one keep-alive connection")
        connection.close()
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(summary.main([str(self.log_path)]), 0)
        self.assertIn("11 requests", out.getvalue())

    def test_redirect(self):
        self.start(redirects={"/s": "/t1.pkg"})
        response, _ = self.request("GET", "/s?product=0287")
        self.assertEqual((response.status, response.getheader("Location")), (302, "/t1.pkg?product=0287"))

    def test_stall(self):
        self.start(stall_after=65536, stall_seconds=1.0)
        began = time.monotonic()
        response, body = self.request("GET", "/t1.pkg", {"Range": "bytes=0-65535"})
        self.assertEqual(len(body), 65536)
        self.assertLess(time.monotonic() - began, 0.8, "the first 64 KiB go out at once")
        began = time.monotonic()
        response, body = self.request("GET", "/t1.pkg", {"Range": "bytes=65536-131071"})
        self.assertEqual(body, DATA[65536:131072])
        self.assertGreaterEqual(time.monotonic() - began, 0.9, "the budget is spent, so the next range waits")
        began = time.monotonic()
        self.request("GET", "/t1.pkg", {"Range": "bytes=0-15"})
        self.assertLess(time.monotonic() - began, 0.8, "the stall happens once")
        self.assertEqual([r["seconds"] for r in self.records() if r.get("event") == "stall"], [1.0])

    def test_die(self):
        self.start(die_after=100000)
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        connection.request("GET", "/t1.pkg")
        response = connection.getresponse()
        self.assertEqual(response.status, 200)
        with self.assertRaises(http.client.IncompleteRead):
            response.read()
        connection.close()
        for _ in range(50):  # The handler logs once it sees the closed socket.
            if any(r.get("aborted") for r in self.records()):
                break
            time.sleep(0.05)
        records = self.records()
        self.assertTrue(any(r.get("event") == "die" for r in records))
        self.assertTrue(any(r.get("aborted") for r in records))
        response, body = self.request("GET", "/t1.pkg", {"Range": "bytes=0-9"})
        self.assertEqual(body, DATA[:10], "the server keeps listening after it drops the connections")


class ContentIdTest(unittest.TestCase):
    def test_ps4_and_ps5(self):
        cid = "UP0001-CUSA04286_00-BATTLESHIP000000"
        cnt = b"\x7fCNT" + bytes(0x3C) + cid.encode() + bytes(64)
        with tempfile.TemporaryDirectory(prefix="ps5-react-pkg-id-") as tmp:
            ps4, ps5, short, other = (Path(tmp) / name for name in ("a.pkg", "b.pkg", "c.pkg", "d.pkg"))
            ps4.write_bytes(cnt)
            header = bytearray(b"\x7fFIH" + bytes(0x200))
            struct.pack_into("<Q", header, 0x58, 0x400)
            ps5.write_bytes(bytes(header) + bytes(0x400 - len(header)) + cnt)
            short.write_bytes(bytes(header))
            other.write_bytes(b"PK\x03\x04" + bytes(200))
            self.assertEqual(content_id.content_id(ps4), (cid, "PS4"))
            self.assertEqual(content_id.content_id(ps5), (cid, "PS5"))
            with self.assertRaisesRegex(ValueError, "incomplete"):
                content_id.content_id(short)
            with self.assertRaisesRegex(ValueError, "not a PKG"):
                content_id.content_id(other)


class SendTest(unittest.TestCase):
    def test_elf_then_lines(self):
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        received = []

        def loader():
            connection, _ = listener.accept()
            with connection:
                connection.settimeout(0.5)
                data = b""
                try:
                    while True:
                        chunk = connection.recv(65536)
                        if not chunk:
                            break
                        data += chunk
                except socket.timeout:
                    pass
                received.append(data)
                raw = bytes(0x300)
                changed = bytearray(raw)
                changed[0x18:0x20] = b"\x01" * 8
                connection.sendall(b"ready\nstarted UP0001-CUSA04286_00-BATTLESHIP000000\nraw " + raw.hex().encode()
                                   + b"\nraw " + bytes(changed).hex().encode() + b"\np 1 2 transferring\nok\n")

        thread = threading.Thread(target=loader, daemon=True)
        thread.start()
        with tempfile.TemporaryDirectory(prefix="ps5-react-pkg-send-") as tmp:
            elf = Path(tmp) / "pkg-installer.elf"
            elf.write_bytes(b"\x7fELF" + bytes(100))
            log = Path(tmp) / "send.log"
            uri = "http://192.168.1.10:8080/t1.pkg"
            with contextlib.redirect_stdout(io.StringIO()):
                code = send.main(["--ip", "127.0.0.1", "--port", str(listener.getsockname()[1]), "--elf", str(elf),
                                  "--log", str(log), "pki2", "--uri", uri, "--uri", uri,
                                  "--content-id", "UP0001-CUSA04286_00-BATTLESHIP000000", "--name", "T9", "--dump"])
            thread.join(5)
            listener.close()
            self.assertEqual(code, 0)
            request = ("PKI2\n%s\nUP0001-CUSA04286_00-BATTLESHIP000000\nT9\n\ndump more\n"
                       "PKI2\n%s\nUP0001-CUSA04286_00-BATTLESHIP000000\nT9\n\ndump\n") % (uri, uri)
            self.assertEqual(received[0], elf.read_bytes() + request.encode())
            text = log.read_text()
            self.assertIn("< ok", text)
            self.assertIn("< raw " + "00" * 0x300, text, "the log keeps the full dump")
            with self.assertRaisesRegex(SystemExit, "at most 127"):
                send.main(["--ip", "127.0.0.1", "--elf", str(elf), "pki2", "--uri", "http://x/" + "a" * 119,
                           "--content-id", "UP0001-CUSA04286_00-BATTLESHIP000000"])


if __name__ == "__main__":
    result = unittest.main(exit=False, verbosity=1).result
    if not result.wasSuccessful():
        sys.exit(1)
    print("PASS: PKG-by-URL spike tools")
