#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CXX=${CXX:-aarch64-linux-gnu-g++}
OUT=${OUT:-$ROOT/out}
JPEGINC=${JPEGINC:-/usr/include}
# Point this at the target AArch64 libjpeg-turbo shared object when the
# cross sysroot does not provide a libjpeg.so linker-name symlink.
JPEGLIB=${JPEGLIB:-/usr/lib/aarch64-linux-gnu/libjpeg.so}
mkdir -p "$OUT"
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$JPEGINC" \
    "$ROOT/tb330fu-camera-processor.cpp" "$JPEGLIB" -o "$OUT/tb330fu-camera-processor"
file "$OUT/tb330fu-camera-processor"
readelf -l "$OUT/tb330fu-camera-processor" | grep 'Requesting program interpreter'
sha256sum "$OUT/tb330fu-camera-processor"
