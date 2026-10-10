#!/usr/bin/env bash
# DriftCheck - renders the app on the PC with a scripted fake controller.
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   bash tools/preview.sh [frames] [out.png]
#
# The drawing code is the console's (src/), with tools/preview/fakes.cpp in place of
# the system calls. 600 frames ends with the rest test done and buttons held.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
frames=${1:-600}
out=${2:-$root/build/preview.png}
mkdir -p "$root/build"
"${HOST_CXX:-clang++}" -std=c++20 -O2 -Wall -Wextra -Wno-unused-function -Wno-unused-const-variable -DDRIFTCHECK_HOST_PREVIEW -I"$root/src" \
    "$root/src/main.cpp" "$root/src/pad.cpp" "$root/src/stick_analysis.cpp" \
    "$root/src/demo_renderer.cpp" "$root/tools/preview/fakes.cpp" -o "$root/build/preview"
DRIFTCHECK_PREVIEW_FRAMES=$frames DRIFTCHECK_PREVIEW_OUT="$root/build/preview.ppm" "$root/build/preview"
python3 -I - "$root/build/preview.ppm" "$out" <<'PY'
import struct, sys, zlib
data = open(sys.argv[1], "rb").read()
header, _, rest = data.partition(b"\n255\n")
width, height = map(int, header.split()[1:3])
rows = b"".join(b"\0" + rest[y * width * 3:(y + 1) * width * 3] for y in range(height))
def chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b"")
open(sys.argv[2], "wb").write(png)
print(sys.argv[2])
PY
