# Copyright (C) 2026 half144 and PS5 React contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional attribution term: see LICENSE-ATTRIBUTION.
"""Prints pandorad-spike's UDP log lines (flag log-udp=<this-mac-ip>:<port>) and appends them to a file.

    python3 tools/spike/daemon/listen.py [--port 9062] [--out spike-udp.log]

Each line is prefixed with the Mac's receive time, so gaps while the console sleeps show on both clocks.
Standard library only.
"""
import argparse
import datetime
import socket


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", type=int, default=9062)
    parser.add_argument("--out", default="spike-udp.log")
    args = parser.parse_args()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as receiver, open(args.out, "a", buffering=1) as out:
        receiver.bind(("0.0.0.0", args.port))
        print(f"listening on UDP {args.port}, appending to {args.out}", flush=True)
        try:
            while True:
                data, sender = receiver.recvfrom(65536)
                stamp = datetime.datetime.now().strftime("%H:%M:%S")
                for line in data.decode(errors="replace").splitlines():
                    text = f"[mac {stamp} {sender[0]}] {line}"
                    print(text, flush=True)
                    out.write(text + "\n")
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
