#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
: "${KERNEL_DIR:?Set KERNEL_DIR to the reconstructed target kernel tree}"
: "${ARCH:=arm64}"
: "${CROSS_COMPILE:?Set CROSS_COMPILE to the target AArch64 toolchain prefix}"

make -C "$ROOT" KERNEL_DIR="$KERNEL_DIR" ARCH="$ARCH" \
  CROSS_COMPILE="$CROSS_COMPILE" v4l2loopback.ko

if command -v readelf >/dev/null 2>&1; then
    readelf -S "$ROOT/v4l2loopback.ko" | grep -q '[.]BTF'
else
    echo "readelf is required for the module BTF gate" >&2
    exit 1
fi

echo "BTF section present: $ROOT/v4l2loopback.ko"
