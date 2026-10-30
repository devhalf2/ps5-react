# Copyright (C) 2026 half144 and PS5 React contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional attribution term: see LICENSE-ATTRIBUTION.
"""verify.py's home-screen background check on synthetic pic0.dds/pic1.dds; no console."""
from pathlib import Path
import struct
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from verify import check_backgrounds  # noqa: E402


def dds(width=3840, height=2160, mips=1, dxgi=98, dimension=3, count=1, dx10=b"DX10"):
    """The header snes9xPS5 and PS5_Ghostship ship: 124-byte DDS header, DX10 extension, BC7 blocks."""
    header = bytearray(148)
    header[:4] = b"DDS "
    struct.pack_into("<7I", header, 4, 124, 0xA1007, height, width, width * height, 0, mips)
    struct.pack_into("<2I", header, 76, 32, 4)
    header[84:88] = dx10
    struct.pack_into("<5I", header, 128, dxgi, dimension, 0, count, 0)
    return bytes(header) + bytes(width * height)


def rejects(sce_sys):
    try:
        check_backgrounds(sce_sys)
    except AssertionError:
        return True
    return False


def main():
    with tempfile.TemporaryDirectory(prefix="ps5-react-sce-art-") as tmp:
        sce_sys = Path(tmp)
        check_backgrounds(sce_sys)  # An app without DDS backgrounds still builds.
        (sce_sys / "pic0.dds").write_bytes(dds())
        assert rejects(sce_sys), "pic0.dds without pic1.dds"
        (sce_sys / "pic1.dds").write_bytes(dds())
        check_backgrounds(sce_sys)
        for name, data in {"1920x1080": dds(1920, 1080), "mipmapped": dds(mips=12),
                           "BC1": dds(dxgi=71), "cube": dds(count=6), "legacy header": dds(dx10=b"DXT5"),
                           "truncated": dds()[:-16], "PNG": b"\x89PNG\r\n\x1a\n" + bytes(64)}.items():
            (sce_sys / "pic1.dds").write_bytes(data)
            assert rejects(sce_sys), name
        (sce_sys / "pic0.dds").unlink()
        (sce_sys / "pic1.dds").write_bytes(dds())
        assert rejects(sce_sys), "pic1.dds without pic0.dds"
    print("PASS: home-screen DDS backgrounds")


if __name__ == "__main__":
    main()
