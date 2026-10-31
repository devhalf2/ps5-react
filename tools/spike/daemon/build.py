# Copyright (C) 2026 half144 and PS5 React contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional attribution term: see LICENSE-ATTRIBUTION.
"""Builds pandorad-spike.elf (spike 0B) with the payload SDK and the pinned PacBrew curl, OpenSSL and zlib.

    python3 tools/spike/daemon/build.py          # -> .build/spike/daemon/pandorad-spike.elf
    python3 tools/spike/daemon/build.py --host   # -> .build/spike/daemon/pandorad-spike-host (macOS smoke build)

Needs the payload SDK in .deps/sdk/ps5-payload-sdk (any `npm run build` fetches it) and LLVM 18 (PS5_LLVM).
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from common import DEPS, ROOT, run  # noqa: E402
from network_ports import ports  # noqa: E402
from ca_bundle import ca_bundle  # noqa: E402

BUILD = ROOT / ".build/spike/daemon"


def embedded_certificates():
    """Mozilla's roots go inside the ELF: the payload has no app folder to read them from."""
    BUILD.mkdir(parents=True, exist_ok=True)
    pem = ca_bundle().read_bytes()
    source = BUILD / "cacert.c"
    source.write_text("const char spike_cacert[] = {" + ",".join(str(x) for x in pem) + ",0};\n"
                      f"const unsigned long spike_cacert_length = {len(pem)};\n")
    return source


def host(output):
    here = Path(__file__).parent
    run(["clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-DSPIKE_HOST", "-I", here, "-pthread",
         "-o", output, here / "daemon_spike.c", here / "host_shim.c", embedded_certificates(), "-lcurl"],
        log=BUILD / "build-host.log")
    print(f"pandorad-spike (host): {output}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--payload-sdk", type=Path, default=DEPS / "sdk/ps5-payload-sdk")
    parser.add_argument("--llvm", type=Path, default=Path(os.environ.get("PS5_LLVM", "/opt/homebrew/opt/llvm@18/bin")))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--host", action="store_true", help="Build for this Mac with stubbed Sony calls")
    args = parser.parse_args()
    if args.host:
        return host(args.output or BUILD / "pandorad-spike-host")
    args.output = args.output or BUILD / "pandorad-spike.elf"
    sdk, llvm = args.payload_sdk.resolve(), args.llvm.resolve()
    if not (sdk / "bin/prospero-clang").exists():
        raise SystemExit(f"No payload SDK at {sdk}: run `npm run build` once, or pass --payload-sdk")
    if not (llvm / "clang").exists():
        raise SystemExit(f"No LLVM 18 at {llvm}: set PS5_LLVM or pass --llvm")
    BUILD.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, LLVM_CONFIG=str(llvm / "llvm-config"), PS5_CLANG=str(llvm / "clang"), PS5_PAYLOAD_SDK=str(sdk))

    certificates = embedded_certificates()
    network = ports()
    libraries = [network / "lib" / name for name in ("libcurl.a", "libssl.a", "libcrypto.a", "libz.a", "libzstd.a", "libpsl.a")]
    run([sdk / "bin/prospero-clang", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-I", network / "include",
         "-o", args.output, Path(__file__).with_name("daemon_spike.c"), certificates, *libraries, "-lkernel_sys"],
        env=env, log=BUILD / "build.log")
    # Every import must name a module the console has; lld already refused symbols no stub defines.
    needed = subprocess.check_output([llvm / "llvm-readelf", "--dynamic", args.output], text=True)
    modules = sorted({line.split("[", 1)[1].rstrip("]") for line in needed.splitlines() if "(NEEDED)" in line})
    print(f"pandorad-spike.elf: {args.output} ({args.output.stat().st_size // 1024} KiB), imports from {', '.join(modules)}")


if __name__ == "__main__":
    main()
