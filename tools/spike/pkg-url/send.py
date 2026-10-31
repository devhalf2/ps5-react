#!/usr/bin/env python3
# Copyright (C) 2026 half144 and PS5 React contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional attribution term: see LICENSE-ATTRIBUTION.
"""Sends pkg-installer.elf and one request to the PS5's ELF loader, then prints and logs every answer.

The loader (port 9021) runs the ELF and hands it the rest of the connection, so the request lines follow
the ELF on the same socket, as the engine's payload_loader does on the console.

  send.py --ip 192.168.1.50 --elf .build/spike/pkg-installer.elf pki2 \\
      --uri http://192.168.1.10:8080/t1.pkg --content-id UP0001-CUSA04286_00-XXXXXXXXXXXXXXXX --name T1 --dump
"""
import argparse
from datetime import datetime
from pathlib import Path
import socket
import sys

URI_LIMIT = 127  # The installer answers 0x80A30003 at 128 bytes.


def requests(args):
    """The request lines, one string per request. Repeating --uri in pki2 asks the same process for
    another install after each one ends (spike T9)."""
    if args.command == "pki1":
        return ["PKI1\n%s\n%s\n" % (args.path, args.name)]
    if args.command == "raw":
        return ["".join(line + "\n" for line in args.lines)]
    ids = args.content_id or [""]
    if len(ids) not in (1, len(args.uri)):
        sys.exit("give one --content-id, or one per --uri")
    out = []
    for index, uri in enumerate(args.uri):
        if uri.startswith(("http://", "https://")) and len(uri.encode()) > URI_LIMIT:
            sys.exit(f"{uri}: {len(uri.encode())} bytes; the installer accepts at most {URI_LIMIT}")
        flags = [name for name, on in (("dump", args.dump), ("detach", args.detach)) if on]
        if index + 1 < len(args.uri):
            flags.append("more")
        content_id = ids[index] if len(ids) > 1 else ids[0]
        out.append("PKI2\n%s\n%s\n%s\n%s\n%s\n" % (uri, content_id, args.name, args.icon_url, " ".join(flags)))
    return out


class Log:
    def __init__(self, path, full_raw):
        self.file = open(path, "a", buffering=1)
        self.full_raw = full_raw
        self.previous_raw = None
        print(f"# log: {path}", flush=True)

    def write(self, direction, line):
        stamp = datetime.now().isoformat(timespec="milliseconds")
        self.file.write(f"{stamp} {direction} {line}\n")
        shown = line
        if line.startswith("raw ") and not line.startswith("raw error") and not self.full_raw:
            shown = "raw " + self.summarize(bytes.fromhex(line[4:]))
        print(f"{stamp} {direction} {shown}", flush=True)

    def summarize(self, raw):
        """Which byte spans changed since the last dump; the full hex stays in the log file."""
        previous, self.previous_raw = self.previous_raw, raw
        if previous is None or len(previous) != len(raw):
            return f"[{len(raw)} B, first dump] {raw[:32].hex()}…"
        spans, start = [], None
        for offset in range(len(raw) + 1):
            differs = offset < len(raw) and raw[offset] != previous[offset]
            if differs and start is None:
                start = offset
            elif not differs and start is not None:
                spans.append(f"0x{start:03x}-0x{offset - 1:03x}={raw[start:offset].hex()}")
                start = None
        return f"[{len(raw)} B] " + (", ".join(spans) if spans else "unchanged")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    parser.add_argument("--ip", required=True, help="The PS5's address")
    parser.add_argument("--port", type=int, default=9021)
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--log", type=Path, help="Answer log (default: send-<time>.log here)")
    parser.add_argument("--full-raw", action="store_true", help="Print raw status dumps in full, not as changes")
    commands = parser.add_subparsers(dest="command", required=True)
    pki1 = commands.add_parser("pki1", help="The app's request: a package path on the console")
    pki1.add_argument("--path", required=True)
    pki1.add_argument("--name", default="PKG spike")
    pki2 = commands.add_parser("pki2", help="A package path or an http(s) URL")
    pki2.add_argument("--uri", required=True, action="append", help="Repeat for another install in the same process")
    pki2.add_argument("--content-id", action="append", help="Required for a URL; once, or once per --uri")
    pki2.add_argument("--name", default="PKG spike")
    pki2.add_argument("--icon-url", default="")
    pki2.add_argument("--dump", action="store_true", help="Raw 0x300-byte status dumps")
    pki2.add_argument("--detach", action="store_true", help="The payload exits right after 'started' (T2)")
    raw = commands.add_parser("raw", help="Arbitrary request lines")
    raw.add_argument("lines", nargs="+")
    args = parser.parse_args(argv)

    elf = args.elf.read_bytes()
    if elf[:4] != b"\x7fELF":
        sys.exit(f"{args.elf} is not an ELF")
    lines = requests(args)
    log = Log(args.log or Path(datetime.now().strftime("send-%Y%m%d-%H%M%S.log")), args.full_raw)
    try:
        return run(args, elf, lines, log, " ".join(sys.argv[1:] if argv is None else argv))
    finally:
        log.file.close()


def run(args, elf, lines, log, argv):
    log.write("#", f"{args.ip}:{args.port} elf={args.elf} ({len(elf)} B) argv={argv}")
    try:
        connection = socket.create_connection((args.ip, args.port), timeout=5)
    except OSError as error:
        log.write("!", f"the loader on {args.ip}:{args.port} did not answer ({error}); load an ELF loader and retry")
        return 1
    connection.settimeout(None)  # An install can take as long as the download does.
    with connection:
        connection.sendall(elf)
        for request in lines:
            for line in request.splitlines():
                log.write(">", line)
            connection.sendall(request.encode())
        pending, code = b"", 1
        try:
            while True:
                data = connection.recv(65536)
                if not data:
                    break
                pending += data
                while b"\n" in pending:
                    line, pending = pending.split(b"\n", 1)
                    text = line.decode(errors="replace")
                    log.write("<", text)
                    if text == "ok":
                        code = 0
                    elif text.startswith("fail "):
                        code = 1
        except KeyboardInterrupt:
            log.write("#", "interrupted; the payload keeps running on the console")
            return 130
        if pending:
            log.write("<", pending.decode(errors="replace"))
        log.write("#", "connection closed by the payload")
    return code


if __name__ == "__main__":
    sys.exit(main())
