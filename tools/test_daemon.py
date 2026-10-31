# Copyright (C) 2026 half144 and PS5 React contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional attribution term: see LICENSE-ATTRIBUTION.
"""The app's daemon on the desktop, end to end (docs/NATIVE-API.md, Daemon): the fixture app
(tools/tests/daemon-app) starts pandorad and closes; the daemon it left running then answers the
handshake, refuses requests without the token, hands the rest to its script, downloads a file served
here, keeps connections alive, and exits on request. Never accesses a console."""
import http.client
import http.server
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from cli import desktop, preview_env  # noqa: E402

APP = ROOT / "tools/tests/daemon-app"
TOKEN = "0123456789abcdef0123456789abcdef"
DATA = bytes(range(256)) * (6 * 1024 * 1024 // 256 + 7)


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


# Another daemon or test may hold 9061 on this machine; the desktop daemon honors PS5_REACT_DAEMON_PORT.
PORT = free_port()


class Files(http.server.BaseHTTPRequestHandler):
    """DATA at /file.bin, with the ranges the download engine asks for."""
    protocol_version = "HTTP/1.1"

    def do_HEAD(self): self.serve(False)

    def do_GET(self): self.serve(True)

    def serve(self, body):
        if self.path.split("?")[0] != "/file.bin":
            self.send_error(404)
            return
        start, end = 0, len(DATA) - 1
        ranged = self.headers.get("Range", "").startswith("bytes=")
        if ranged:
            first, _, last = self.headers["Range"][6:].partition("-")
            start, end = int(first), min(int(last) if last else len(DATA) - 1, len(DATA) - 1)
        self.send_response(206 if ranged else 200)
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(end - start + 1))
        if ranged: self.send_header("Content-Range", f"bytes {start}-{end}/{len(DATA)}")
        self.end_headers()
        try:
            if body: self.wfile.write(DATA[start:end + 1])
        except (BrokenPipeError, ConnectionResetError):
            pass  # The download engine closes its first probe once it has the size.

    def log_message(self, *args): pass


def request(method, path, body=None, token=TOKEN, headers=None, connection=None):
    connection = connection or http.client.HTTPConnection("127.0.0.1", PORT, timeout=15)
    sent = dict(headers or {})
    if token: sent["X-Pandora-Token"] = token
    connection.request(method, path, body=body, headers=sent)
    response = connection.getresponse()
    data = response.read()
    return response.status, dict((k.lower(), v) for k, v in response.getheaders()), data


def answers():
    try:
        return request("GET", "/hello", token=None)[0] == 200
    except OSError:
        return False


def wait_gone(seconds=10):
    deadline = time.time() + seconds
    while time.time() < deadline:
        if not answers(): return True
        time.sleep(0.1)
    return False


def wait_log(path, text, seconds=10):
    """The log once it holds `text`: the daemon's last lines come after its port closes."""
    deadline = time.time() + seconds
    while time.time() < deadline:
        log = path.read_text() if path.exists() else ""
        if text in log: return log
        time.sleep(0.1)
    return log


def spawn(build, env, request_text):
    """Starts pandorad as Daemon.start does and returns it with its first answer line."""
    process = subprocess.Popen([str(build / "pandorad")], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL, env=env, start_new_session=True)
    process.stdin.write(request_text.encode())
    process.stdin.close()
    line = process.stdout.readline().decode().strip()
    process.stdout.close()
    return process, line


def main():
    command, build = desktop(APP)
    assert (build / "pandorad").exists(), "the build makes pandorad for an app with a daemon entry"
    env = {**preview_env(APP), "PS5_REACT_DAEMON_PORT": str(PORT)}
    data_dir = Path(env["PS5_REACT_SANDBOX"]) / "download0"
    shutil.rmtree(data_dir, ignore_errors=True)
    data_dir.mkdir(parents=True)
    files = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Files)
    threading.Thread(target=files.serve_forever, daemon=True).start()
    try:
        run(command, build, env, data_dir, files.server_address[1])
    finally:
        files.shutdown()
        if answers():
            try: request("POST", "/exit")
            except OSError: pass


def run(command, build, env, data_dir, file_port):
    # The app starts the daemon, starts it again (it answers busy, then the handshake), and closes.
    result = subprocess.run([str(x) for x in command], cwd=build, env=env, capture_output=True, text=True, timeout=120)
    reports = {}
    for line in result.stdout.splitlines():
        if line.startswith("daemon-test "):
            name, _, value = line[len("daemon-test "):].partition(" ")
            reports[name] = json.loads(value) if value.startswith(("{", "\"", "[")) else value
    assert result.returncode == 0 and "failed" not in reports, result.stdout[-4000:] + result.stderr[-2000:]
    assert reports["app"] == {"supported": True, "isDaemon": False, "args": None}, reports
    assert "only the daemon serves" in reports["serve"], reports
    assert reports["invalid"]["ok"] is False and "token" in reports["invalid"]["error"], reports
    assert reports["start"] == {"ok": True}, reports
    assert reports["again"] == {"ok": True}, reports
    print("PASS: the app sees Daemon.supported, starts pandorad, and a second start finds it serving.")

    # The app has closed; the daemon it started runs on in its own session.
    status, headers, body = request("GET", "/hello", token=None)
    hello = json.loads(body)
    assert status == 200 and hello["protocol"] == 1 and hello["version"] == "01.002.003", body
    status, _, body = request("GET", "/echo", token=None)
    assert status == 401 and b"x-pandora-token" in body, (status, body)
    assert request("GET", "/echo", token="f" * 32)[0] == 401
    assert request("HEAD", "/hello", token=None)[0] == 401, "only GET /hello goes without the token"
    status, _, body = request("GET", "/i/0123abcd.pkg", token=None)
    assert status == 404 and b"no route GET /i/0123abcd.pkg" in body, "/i/ paths reach the script without it"
    print("PASS: the app closed and the daemon answers; 401 without the token except GET /hello and /i/.")

    status, headers, body = request("POST", "/echo?since=4&wait=1000&name=a%20b+c", body="{\"x\":1}",
                                    headers={"Content-Type": "application/json", "X-Custom": "Value"})
    echo = json.loads(body)
    assert status == 200 and headers["content-type"] == "application/json", (status, headers)
    assert echo["method"] == "POST" and echo["path"] == "/echo" and echo["body"] == "{\"x\":1}", echo
    assert echo["query"] == {"since": "4", "wait": "1000", "name": "a b c"}, echo
    assert echo["headers"]["x-custom"] == "Value" and echo["headers"]["x-pandora-token"] == TOKEN, echo
    assert echo["isDaemon"] is True and echo["supported"] is True and echo["platform"] == "desktop", echo
    assert echo["args"] == {"token": TOKEN, "stateDir": "/download0/daemon-state", "version": "01.002.003"}, echo
    assert echo["timers"] > 0, "setInterval runs between requests"
    for absent in ("image", "sound", "shader", "browser", "pad", "text", "openURL", "exit"):
        assert absent not in echo["modules"], echo["modules"]
    for present in ("fs", "network", "archives", "packages", "power", "notify", "daemon"):
        assert present in echo["modules"], echo["modules"]
    print("PASS: a request with the token reaches the script: method, path, query, lowercase headers, body, args.")

    # Keep-alive: three requests on one connection; a slow request does not hold up another client.
    connection = http.client.HTTPConnection("127.0.0.1", PORT, timeout=15)
    request("GET", "/echo", connection=connection)
    first = connection.sock
    for _ in range(2): request("GET", "/echo", connection=connection)
    assert connection.sock is first, "the connection was kept alive"
    slow = http.client.HTTPConnection("127.0.0.1", PORT, timeout=15)
    slow.request("GET", "/wait?ms=800", headers={"X-Pandora-Token": TOKEN})
    began = time.time()
    request("GET", "/echo")
    assert time.time() - began < 0.5, "a long poll held up another request"
    response = slow.getresponse()
    assert response.status == 200 and json.loads(response.read()) == {"waited": 800}
    assert request("HEAD", "/echo")[2] == b"", "HEAD has no body"
    assert request("GET", "/empty")[0] == 204
    print("PASS: keep-alive, concurrent long polls, HEAD and 204.")

    assert request("POST", "/echo", body=b"x" * (1024 * 1024 + 1))[0] == 413
    assert request("POST", "/echo", body=b"x" * (1024 * 1024))[0] == 200
    assert request("GET", "/throw")[0] == 500 and request("GET", "/bad")[0] == 500
    held = []
    for _ in range(8):
        held.append(socket.create_connection(("127.0.0.1", PORT)))
    time.sleep(0.3)
    extra = socket.create_connection(("127.0.0.1", PORT))
    extra.settimeout(5)
    assert extra.recv(64).startswith(b"HTTP/1.1 503"), "a ninth connection is refused"
    for sock in (*held, extra): sock.close()
    time.sleep(0.3)
    assert request("GET", "/echo")[0] == 200
    print("PASS: 1 MiB body limit (413), handler errors answer 500, at most 8 connections (503).")

    # Downloads in the daemon: a file served here, written under the app's data folder.
    status, _, body = request("POST", "/download", body=json.dumps({
        "url": f"http://127.0.0.1:{file_port}/file.bin", "destination": "/download0/daemon-test/file.bin"}))
    assert status == 200 and json.loads(body) == {"state": "completed", "size": len(DATA)}, body
    assert (data_dir / "daemon-test/file.bin").read_bytes() == DATA
    print(f"PASS: Downloads in the daemon wrote {len(DATA)} bytes from a local server.")

    status, _, body = request("POST", "/exit?code=0")
    assert status == 200 and body == b"{}", (status, body)
    assert wait_gone(), "Daemon.exit stopped the daemon"
    log = wait_log(data_dir / "daemon.log", "exit 0")
    assert "fixture daemon 01.002.003 serving from /download0/daemon-state" in log, log
    assert "Daemon.exit(0)" in log and "exit 0" in log, log
    assert "GET /throw failed: Error: fixture failure" in log, "a handler's error is logged"
    assert "Unhandled promise rejection" not in log, "a rejection the handler wrapper catches is not reported"
    print("PASS: Daemon.exit answers first, then exits; console and errors land in daemon.log.")

    # The start protocol directly: the log rotates, a second daemon answers busy and exits 3, a bad
    # request answers an error.
    request_text = f"PDD1\n{TOKEN}\n/download0/daemon-state\n01.002.003\n"
    daemon, line = spawn(build, env, request_text)
    assert line == "ready", line
    assert (data_dir / "daemon.prev.log").read_text() == log, "the previous run's log is kept"
    second, line = spawn(build, env, request_text)
    assert line == "busy" and second.wait(10) == 3, line
    bad, line = spawn(build, env, "HELLO\n\n\n\n")
    assert line.startswith("error bad request") and bad.wait(10) == 2, line
    os.kill(daemon.pid, 15)
    assert daemon.wait(10) == 0 and wait_gone(), "SIGTERM stops it cleanly"
    assert "stopping on signal 15" in (data_dir / "daemon.log").read_text()
    print("PASS: log rotation, busy (exit 3) when the port is taken, bad request (exit 2), SIGTERM.")


if __name__ == "__main__":
    main()
