#!/usr/bin/env python3
# Copyright (C) 2026 half144 and PS5 React contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional attribution term: see LICENSE-ATTRIBUTION.
"""Serves one directory to the PS5's package installer and logs every request it makes.

Python's http.server has no Range support, and the installer reads a package with HEAD and ranged GETs
over keep-alive connections, so this answers those itself. Each request becomes one JSON line on stdout
and in the log file. --stall-after-bytes and --die-after-bytes fake a stalled or dropped server to
measure how long the installer waits (spike T8).
"""
import argparse
import email.utils
import json
import os
import signal
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import socket
import sys
import threading
import time
from urllib.parse import unquote, urlsplit

CHUNK = 64 * 1024


class Unsatisfiable(Exception):
    pass


def parse_range(header, size):
    """(first, last) inclusive for a single "bytes=" range, or None to send the whole file.

    A malformed or multi-range header is ignored, as RFC 9110 allows; a well-formed range that starts past
    the end raises Unsatisfiable (416).
    """
    if not header:
        return None
    unit, _, spec = header.strip().partition("=")
    if unit.strip().lower() != "bytes" or "," in spec:
        return None
    first, dash, last = spec.strip().partition("-")
    if not dash:
        return None
    try:
        if not first:
            length = int(last)
            if length <= 0:
                raise Unsatisfiable(header)
            return max(size - length, 0), size - 1
        first = int(first)
        last = int(last) if last else size - 1
    except ValueError:
        return None
    if first >= size:
        raise Unsatisfiable(header)
    if last < first:
        return None
    return first, min(last, size - 1)


class State:
    """What every connection shares: totals, the stall and drop triggers, the open sockets and the log."""

    def __init__(self, root, log_path, stall_after=None, stall_seconds=0.0, die_after=None, redirects=None, echo=True):
        self.root = Path(root).resolve()
        self.lock = threading.Lock()
        self.served = 0
        self.stall_after, self.stall_seconds, self.stall_until = stall_after, stall_seconds, None
        self.die_after = die_after
        self.redirects = redirects or {}
        self.sockets = set()
        self.log_file = open(log_path, "a", buffering=1) if log_path else None
        self.echo = echo

    def log(self, record):
        line = json.dumps(record, separators=(",", ":"))
        with self.lock:
            if self.echo:
                print(line, flush=True)
            if self.log_file:
                self.log_file.write(line + "\n")

    def open_connections(self):
        with self.lock:
            return len(self.sockets)

    def wait_if_stalled(self):
        """Holds every sender once the byte budget is spent, for --stall-seconds, once per run."""
        with self.lock:
            if self.stall_after is not None and self.stall_until is None and self.served >= self.stall_after:
                self.stall_until = time.monotonic() + self.stall_seconds
                started = True
            else:
                started = False
            until = self.stall_until
        if started:
            self.log({"ts": time.time(), "event": "stall", "seconds": self.stall_seconds, "served": self.served})
        if until is not None:
            remaining = until - time.monotonic()
            if remaining > 0:
                time.sleep(remaining)

    def count(self, size):
        """Adds sent bytes; returns True when this crosses --die-after-bytes and every socket was closed."""
        with self.lock:
            before = self.served
            self.served += size
            crossed = self.die_after is not None and before < self.die_after <= self.served
            sockets = list(self.sockets) if crossed else []
        if crossed:
            self.log({"ts": time.time(), "event": "die", "served": self.served, "closed": len(sockets)})
            for sock in sockets:
                try:
                    sock.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
        return crossed


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "pkg-url-spike/1"

    def setup(self):
        super().setup()
        with self.server.state.lock:
            self.server.state.sockets.add(self.connection)

    def finish(self):
        try:
            super().finish()
        finally:
            with self.server.state.lock:
                self.server.state.sockets.discard(self.connection)

    def log_message(self, format, *args):
        pass  # Every request is logged as JSON instead.

    def do_HEAD(self):
        self.answer(body=False)

    def do_GET(self):
        self.answer(body=True)

    def answer(self, body):
        state = self.server.state
        began = time.monotonic()
        url = urlsplit(self.path)
        record = {"ts": time.time(), "client": "%s:%d" % self.client_address[:2], "method": self.command,
                  "path": url.path, "query": url.query, "range": self.headers.get("Range"),
                  "headers": [[k, v] for k, v in self.headers.items()], "open": state.open_connections()}
        self._status, self._sent = 0, 0
        try:
            self.respond(url, body)
        except OSError as error:  # The console hung up, or --die-after-bytes closed the socket.
            record["aborted"] = type(error).__name__
            self.close_connection = True
        record.update(status=self._status, bytes=self._sent, duration=round(time.monotonic() - began, 4))
        state.log(record)

    def head(self, status, headers):
        self._status = status
        self.send_response(status)
        for name, value in headers:
            self.send_header(name, value)
        self.end_headers()

    def respond(self, url, body):
        state = self.server.state
        target = state.redirects.get(url.path)
        if target:
            location = target + ("?" + url.query if url.query else "")
            return self.head(302, [("Location", location), ("Content-Length", "0")])
        # The installer asks for <content_id>.crc and must hear that there is none.
        relative = unquote(url.path).lstrip("/")
        path = (state.root / relative).resolve()
        if url.path.endswith(".crc") or not relative or state.root not in path.parents or not path.is_file():
            return self.head(404, [("Content-Length", "0")])
        stat = path.stat()
        size = stat.st_size
        common = [("Accept-Ranges", "bytes"), ("ETag", '"%x-%x"' % (stat.st_mtime_ns, size)),
                  ("Last-Modified", email.utils.formatdate(stat.st_mtime, usegmt=True)),
                  ("Content-Type", "application/octet-stream")]
        try:
            span = parse_range(self.headers.get("Range"), size)
        except Unsatisfiable:
            return self.head(416, [("Content-Range", "bytes */%d" % size), ("Content-Length", "0"), *common])
        first, last = span if span else (0, size - 1)
        length = last - first + 1 if size else 0
        state.wait_if_stalled()
        headers = [("Content-Length", str(length)), *common]
        if span:
            headers.insert(0, ("Content-Range", "bytes %d-%d/%d" % (first, last, size)))
        self.head(206 if span else 200, headers)
        if not body:
            return
        with open(path, "rb") as source:
            source.seek(first)
            while self._sent < length:
                state.wait_if_stalled()
                data = source.read(min(CHUNK, length - self._sent))
                if not data:
                    break
                self.wfile.write(data)
                self._sent += len(data)
                if state.count(len(data)):
                    raise ConnectionAbortedError("die-after-bytes")


class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address, state):
        self.state = state
        super().__init__(address, Handler)


def lan_address():
    """The address other machines reach this one on, for the URL to hand the console."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        try:
            probe.connect(("192.0.2.1", 9))  # TEST-NET-1: nothing is sent; this only picks the route.
            return probe.getsockname()[0]
        except OSError:
            return "127.0.0.1"


def interrupt(signum, frame):
    raise KeyboardInterrupt  # A kill stops the server as Ctrl-C does, with a "stop" line in the log.


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dir", required=True, type=Path, help="Directory to serve")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--log", type=Path, help="JSON-lines log (default: serve-<time>.jsonl here)")
    parser.add_argument("--stall-after-bytes", type=int, help="Stop sending once this many bytes were served")
    parser.add_argument("--stall-seconds", type=float, default=30.0, help="How long the stall lasts (once)")
    parser.add_argument("--die-after-bytes", type=int, help="Close every connection once this many bytes were served")
    parser.add_argument("--redirect", action="append", default=[], metavar="/SHORT=/LONG",
                        help="Answer /SHORT with 302 to /LONG (or a full URL); repeatable")
    args = parser.parse_args(argv)
    if not args.dir.is_dir():
        parser.error(f"--dir {args.dir} is not a directory")
    redirects = {}
    for entry in args.redirect:
        short, sep, target = entry.partition("=")
        if not sep or not short.startswith("/") or not target:
            parser.error(f"--redirect {entry!r}: use /short=/long/path")
        redirects[short] = target
    log = args.log or Path(time.strftime("serve-%Y%m%d-%H%M%S.jsonl"))
    state = State(args.dir, log, args.stall_after_bytes, args.stall_seconds, args.die_after_bytes, redirects)
    server = Server((args.bind, args.port), state)
    host = lan_address() if args.bind == "0.0.0.0" else args.bind
    state.log({"ts": time.time(), "event": "listen", "url": f"http://{host}:{args.port}/", "dir": str(state.root),
               "log": str(log), "files": sorted(p.name for p in state.root.iterdir() if p.is_file())[:20],
               "stall_after": args.stall_after_bytes, "stall_seconds": args.stall_seconds,
               "die_after": args.die_after_bytes, "redirects": redirects, "pid": os.getpid()})
    signal.signal(signal.SIGTERM, interrupt)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        state.log({"ts": time.time(), "event": "stop", "served": state.served})
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
