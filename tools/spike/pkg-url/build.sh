#!/bin/sh
# Copyright (C) 2026 half144 and PS5 React contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional attribution term: see LICENSE-ATTRIBUTION.
#
# Builds pkg-installer.elf alone, with the same command tools/build_ps5.py uses, into .build/spike/.
# Needs the payload SDK in .deps (any earlier `npm run build` fetched it) and LLVM 18 in PS5_LLVM.
set -eu
root=$(cd "$(dirname "$0")/../../.." && pwd)
sdk=${PS5_PAYLOAD_SDK:-$root/.deps/sdk/ps5-payload-sdk}
llvm=${PS5_LLVM:-/opt/homebrew/opt/llvm@18/bin}
[ -x "$sdk/bin/prospero-clang" ] || { echo "No payload SDK at $sdk; run npm run build once, or set PS5_PAYLOAD_SDK" >&2; exit 1; }
[ -x "$llvm/clang" ] || { echo "No LLVM 18 clang at $llvm; set PS5_LLVM" >&2; exit 1; }
mkdir -p "$root/.build/spike"
out=$root/.build/spike/pkg-installer.elf
LLVM_CONFIG=$llvm/llvm-config PS5_CLANG=$llvm/clang PS5_PAYLOAD_SDK=$sdk \
  "$sdk/bin/prospero-clang" -std=gnu11 -O2 -Wall -Wextra -Werror -o "$out" \
  "$root/native/ps5/payloads/pkg_installer.c" -lkernel_sys -lSceUserService
shasum -a 256 "$out"
