# Copyright (C) 2026 half144 and PS5 React contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional attribution term: see LICENSE-ATTRIBUTION.
"""Sends pandorad-spike.elf and its request to the console's payload loader, then prints what it answers.

    python3 tools/spike/daemon/send.py <console-ip> <url> [--dest DIR] [--flags "keepawake probe-memory"]
    python3 tools/spike/daemon/send.py <console-ip> stop        # asks the running copy to quit

The payload keeps running after this exits (Ctrl-C, or --seconds); its log is <dest>/log.txt on the
console and, with log-udp=<mac-ip>:<port> in the flags, listen.py on the Mac. Standard library only.
"""
import argparse
import socket
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("console", help="the console's IP address")
    parser.add_argument("url", help='what to download; "-" to only serve and log, "stop" to stop a running copy')
    parser.add_argument("--dest", default="", help="destination folder on the console (default /data/pandora-spike)")
    parser.add_argument("--flags", default="", help='"keepawake", "probe-memory", "insecure", "name=<file>", "log-udp=<ip>:<port>"')
    parser.add_argument("--elf", type=Path, default=ROOT / ".build/spike/daemon/pandorad-spike.elf")
    parser.add_argument("--port", type=int, default=9021)
    parser.add_argument("--seconds", type=float, default=0, help="stop reading after this long (0: until the socket closes or Ctrl-C)")
    args = parser.parse_args()
    if "\n" in args.url + args.dest + args.flags:
        raise SystemExit("Request fields cannot contain line breaks")
    elf = args.elf.read_bytes()
    request = f"DSP1\n{args.url}\n{args.dest}\n{args.flags}\n".encode()
    with socket.create_connection((args.console, args.port), timeout=10) as connection:
        connection.sendall(elf + request)
        print(f"sent {args.elf.name} ({len(elf)} bytes) and the request to {args.console}:{args.port}", flush=True)
        connection.settimeout(1)
        deadline = time.monotonic() + args.seconds if args.seconds else None
        try:
            while deadline is None or time.monotonic() < deadline:
                try:
                    data = connection.recv(65536)
                except socket.timeout:
                    continue
                if not data:
                    print("(the payload closed the connection)")
                    break
                sys.stdout.write(data.decode(errors="replace"))
                sys.stdout.flush()
        except KeyboardInterrupt:
            print("\n(closed our end; the payload keeps running)")


if __name__ == "__main__":
    main()
