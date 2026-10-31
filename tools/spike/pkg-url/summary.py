#!/usr/bin/env python3
# Copyright (C) 2026 half144 and PS5 React contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional attribution term: see LICENSE-ATTRIBUTION.
"""Condenses a serve.py log into what the spike asks: the request pattern, connections, speed and gaps.

  summary.py t1-serve.jsonl [--bucket 10] [--gap 5]
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import sys
import time


def size_of(record):
    """The length a Range header asked for, or None."""
    spec = (record.get("range") or "").partition("=")[2]
    first, _, last = spec.partition("-")
    return int(last) - int(first) + 1 if first.isdigit() and last.isdigit() else None


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("log", type=Path)
    parser.add_argument("--bucket", type=float, default=10.0, help="Seconds per throughput row")
    parser.add_argument("--gap", type=float, default=5.0, help="Report silences longer than this")
    args = parser.parse_args(argv)
    records = [json.loads(line) for line in args.log.read_text().splitlines() if line.strip()]
    events = [r for r in records if "event" in r]
    requests = [r for r in records if "method" in r]
    if not requests:
        print("no requests")
        return 1

    clock = lambda ts: time.strftime("%H:%M:%S", time.localtime(ts))  # noqa: E731
    began, ended = requests[0]["ts"], max(r["ts"] + r["duration"] for r in requests)
    total = sum(r["bytes"] for r in requests)
    print(f"{len(requests)} requests {clock(began)}-{clock(ended)}, {total / 1e6:.1f} MB, "
          f"{len({r['client'] for r in requests})} connections, at most {max(r['open'] for r in requests)} open")
    print("by method/status:", dict(Counter(f"{r['method']} {r['status']}" for r in requests)))
    print("paths:", dict(Counter(r["path"] for r in requests)))
    print("queries:", dict(Counter(r["query"].split("&r=")[0] for r in requests).most_common(5)))
    print("range sizes:", dict(Counter(size_of(r) for r in requests).most_common(8)))
    print("user agents:", dict(Counter(dict(r["headers"]).get("User-Agent") for r in requests)))
    aborted = [r for r in requests if r.get("aborted")]
    if aborted:
        print(f"aborted: {len(aborted)} (first {clock(aborted[0]['ts'])}: {aborted[0]['aborted']})")
    for event in events:
        if event["event"] != "listen":
            print("event:", clock(event["ts"]), {k: v for k, v in event.items() if k != "ts"})

    print("\nfirst 12 requests:")
    for r in requests[:12]:
        print(f"  {clock(r['ts'])} {r['client']:>21} {r['method']:4} {r['status']} {r['path']} "
              f"range={r['range']} bytes={r['bytes']} {r['duration']:.2f}s")

    # A request's bytes count at its end; at the console's 16 MiB ranges that is fine-grained enough.
    print(f"\nthroughput per {args.bucket:g} s:")
    buckets = Counter()
    for r in requests:
        buckets[int((r["ts"] + r["duration"] - began) // args.bucket)] += r["bytes"]
    for index in range(max(buckets) + 1):
        print(f"  {clock(began + index * args.bucket)} {buckets[index] / args.bucket / 1e6:8.1f} MB/s")

    starts = sorted(r["ts"] for r in requests)
    gaps = [(a, b) for a, b in zip(starts, starts[1:]) if b - a > args.gap]
    print(f"\nsilences over {args.gap:g} s between request starts: {len(gaps)}")
    for a, b in gaps:
        print(f"  {clock(a)} -> {clock(b)}  {b - a:.0f} s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
