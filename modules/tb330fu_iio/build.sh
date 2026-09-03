#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
HOST=${BUILD_HOST:-entertainment.local}
REMOTE=${REMOTE_BUILD_ROOT:-/home/felix/tb330fu-release-sensors-iio-build}
KERNEL=${KERNEL_BUILD_ROOT:-/home/felix/tb330fu/build-module-btf}
CLANG=${CLANG_TOOLCHAIN:-/home/felix/tb330fu/toolchains/clang-r510928/bin/}
PAHOLE=${PAHOLE_TOOL:-/home/felix/tb330fu/tools-zstd/bin/pahole}
ssh "$HOST" "mkdir -p '$REMOTE/modules/tb330fu_iio' '$REMOTE/src/sensors'"
scp "$ROOT/modules/tb330fu_iio/tb330fu_iio.c" "$ROOT/modules/tb330fu_iio/Makefile" "$HOST:$REMOTE/modules/tb330fu_iio/"
ssh "$HOST" "mkdir -p '$REMOTE/src/sensors'"
scp "$ROOT/src/sensors/tb330fu_sensor_feed.h" "$HOST:$REMOTE/src/sensors/"
ssh "$HOST" "cd /home/felix/tb330fu && LD_LIBRARY_PATH=/home/felix/tb330fu/dwarves/build-zstd:/home/felix/tb330fu/tools-zstd/lib make -C '$KERNEL' M='$REMOTE/modules/tb330fu_iio' ARCH=arm64 LLVM='$CLANG' LLVM_IAS=1 PAHOLE='$PAHOLE' modules"
mkdir -p "$ROOT/build/sensors/module"
scp "$HOST:$REMOTE/modules/tb330fu_iio/tb330fu_iio.ko" "$ROOT/build/sensors/module/"
sha256sum "$ROOT/build/sensors/module/tb330fu_iio.ko"
ssh "$HOST" "readelf -SW '$REMOTE/modules/tb330fu_iio/tb330fu_iio.ko' | grep -E '\\.BTF([[:space:]]|$)'"
