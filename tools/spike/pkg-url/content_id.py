#!/usr/bin/env python3
# Copyright (C) 2026 half144 and PS5 React contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional attribution term: see LICENSE-ATTRIBUTION.
"""Prints a PKG's content ID, as pkg_installer.c reads it, and the URL to hand the console.

A PS4 PKG starts with \\x7FCNT and names its content ID at 0x40. A PS5 PKG starts with \\x7FFIH and keeps
such a header at the offset its 64-bit little-endian field at 0x58 gives.

  content_id.py t1.pkg --base-url http://192.168.1.10:8080/
"""
import argparse
from pathlib import Path
import struct
import sys
from urllib.parse import quote


def content_id(path):
    """(content ID, "PS4" or "PS5"); raises ValueError for anything else."""
    with open(path, "rb") as file:
        header = file.read(0x40 + 36)
        kind = "PS4"
        if header[:4] == b"\x7fFIH" and len(header) == 0x40 + 36:
            kind = "PS5"
            file.seek(struct.unpack_from("<Q", header, 0x58)[0])
            header = file.read(0x40 + 36)
    if len(header) != 0x40 + 36 or header[:4] != b"\x7fCNT":
        raise ValueError("the PS5 package is incomplete" if kind == "PS5" else "not a PKG")
    return header[0x40:0x40 + 36].decode("ascii"), kind


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("pkg", type=Path, nargs="+")
    parser.add_argument("--base-url", help="serve.py's URL, to print each package's URL and its length")
    args = parser.parse_args(argv)
    code = 0
    for pkg in args.pkg:
        try:
            cid, kind = content_id(pkg)
        except (OSError, ValueError) as error:
            print(f"{pkg}: {error}", file=sys.stderr)
            code = 1
            continue
        line = f"{pkg}\t{kind}\t{cid}\t{pkg.stat().st_size} B"
        if args.base_url:
            url = args.base_url.rstrip("/") + "/" + quote(pkg.name)
            size = len(url.encode())
            line += f"\t{url}\t{size} B" + ("" if size <= 127 else "  TOO LONG (max 127): rename the file")
        print(line)
    return code


if __name__ == "__main__":
    sys.exit(main())
