# Copyright (C) 2026 half144 and PS5 React contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional attribution term: see LICENSE-ATTRIBUTION.
"""Validate local title structure and binary integrity; never contacts a console."""
import json
from pathlib import Path
import re
import struct
import subprocess
import zipfile

import sys
from common import ROOT, digest


def check_backgrounds(sce_sys):
    """The home screen's backgrounds, when the app has them: both, each a 3840x2160 BC7_UNORM DX10 DDS
    with one mip, the layout homebrew that shows its art there ships."""
    backgrounds = [sce_sys / name for name in ("pic0.dds", "pic1.dds")]
    present = [path.exists() for path in backgrounds]
    assert all(present) or not any(present), "pic0.dds and pic1.dds go together"
    for path in backgrounds:
        if not path.exists(): continue
        data = path.read_bytes()
        assert len(data) >= 148 and data[:4] == b"DDS " and data[84:88] == b"DX10", path.name
        height, width = struct.unpack_from("<II", data, 12)
        assert (width, height) == (3840, 2160) and struct.unpack_from("<I", data, 28)[0] == 1, path.name
        dxgi, dimension, _, count, _ = struct.unpack_from("<5I", data, 128)
        assert (dxgi, dimension, count) == (98, 3, 1), path.name  # BC7_UNORM, one 2D texture
        assert len(data) == 148 + width * height, path.name


def main():
    TITLE = sys.argv[1] if len(sys.argv) > 1 else "PPSA99053"
    receipt_path = ROOT / "dist" / (TITLE + ".receipt.json")
    receipt = json.loads(receipt_path.read_text())
    BUILD = ROOT / ".build" / receipt["app"] / "ps5"
    app = ROOT / "dist" / TITLE
    param = json.loads((app / "sce_sys/param.json").read_text())
    assert param["titleId"] == TITLE
    assert re.fullmatch(r"[A-Z]{2}\d{4}-" + TITLE + r"_00-[A-Z0-9]{16}", param["contentId"])
    assert int(param["requiredSystemSoftwareVersion"], 16) == 0
    assert param["applicationDrmType"] == "free"
    if receipt["config"].get("filesystemAccess") == "console":
        helper = json.loads((app / "lapy-manifest.json").read_text())
        assert helper["target_title"] == TITLE
        assert helper["mode"] == "elf-helper" and helper["max_requests"] == 1
        assert helper["service"] is False
        assert helper["elf_sha256"] == digest(app / "lapy.elf")
        assert int(param["downloadDataSize"]) > 0
    from PIL import Image
    with Image.open(app / "sce_sys/icon0.png") as image:
        assert image.size == (512, 512)
        image.verify()
    check_backgrounds(app / "sce_sys")

    data = (BUILD / "eboot.elf").read_bytes()
    assert data[:7] == b"\x7fELF\x02\x01\x01"
    assert data[7:9] == b"\x09\x02", "Expected PS5 ABI"
    assert struct.unpack_from("<HH", data, 16) == (0xFE10, 62)
    offset = struct.unpack_from("<Q", data, 32)[0]
    size, count = struct.unpack_from("<HH", data, 54)
    loads = 0
    for index in range(count):
        kind, _, file_offset, address, _, file_size, mem_size, align = struct.unpack_from(
            "<IIQQQQQQ", data, offset + index * size)
        assert file_offset + file_size <= len(data)
        if kind == 1:
            loads += 1
            assert align >= 16384 and file_offset % align == address % align
            assert file_size <= mem_size
    assert loads >= 3

    for path in (app / "eboot.bin", app / "sce_module/libc.prx"):
        result = subprocess.check_output([str(BUILD / "host/ps5-native-tool"), "self",
                                          "--inspect", "--file", str(path)], text=True)
        assert "integrity: valid" in result

    receipt = json.loads(receipt_path.read_text())
    assert receipt["hardware_tested"] is False
    for name, checksum in receipt["files"].items():
        assert digest(app / name) == checksum, name
    with zipfile.ZipFile(ROOT / "dist" / (TITLE + ".zip")) as package:
        assert package.testzip() is None
        for name, checksum in receipt["files"].items():
            assert package.read(TITLE + "/" + name) == (app / name).read_bytes(), name
    print("PASS: title metadata, icon, backgrounds, PS5 ELF/segments, SELF integrity, hashes and ZIP")


if __name__ == "__main__":
    main()
