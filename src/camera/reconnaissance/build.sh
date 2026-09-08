#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DAEMON_ROOT="$ROOT/../daemon"
OUT="$DAEMON_ROOT/out"
mkdir -p "$ROOT/out"

# Reuse the canonical daemon build to generate the exact AIDL bindings and
# validate the checked-in protobuf contract, but link only the disposable
# characteristics translation unit below.
"$DAEMON_ROOT/build.sh"

PREBUILT="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
set -- $(find "$OUT/src" -type f -name '*.cpp' | sort)
"$PREBUILT/bin/clang++" --target=aarch64-linux-android35 -std=c++20 -O2 \
    -Wno-deprecated-declarations -fPIE -pie -static-libstdc++ \
    -D__ANDROID__ -DBINDER_STABILITY_SUPPORT -DPAGE_SIZE=4096 \
    -I "$OUT/include" -I "$HDR/native/libs/binder/ndk/include_cpp" \
    -I "$HDR/native/libs/binder/ndk/include_platform" \
    -I "$HDR/core/libcutils/include" -I "$HDR/core/libutils/include" \
    -I "$HDR/logging/liblog/include" -I "$HDR/libfmq/include" \
    -I "$HDR/libfmq/base" -I "$HDR/libbase/include" -I "$META_INC" \
    -I "$LIBPNG_INC" -I "$PROTOBUF_SRC_DIR" \
    "$ROOT/tb330fu-camera-characteristics.cpp" \
    "$FMQ_SRC_DIR/EventFlag.cpp" "$FMQ_SRC_DIR/FmqInternal.cpp" "$@" \
    "$BINDER_NDK" "$META_LIB" "$FMQ_LIB" "$CUTILS_LIB" "$UTILS_LIB" \
    "$BASE_LIB" "$LOG_LIB" "$LIBPNG_LIB" "$Z_LIB" -ldl \
    -o "$ROOT/out/tb330fu-camera-characteristics"
file "$ROOT/out/tb330fu-camera-characteristics"
sha256sum "$ROOT/out/tb330fu-camera-characteristics"
