#!/bin/sh
set -eu
SELF=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SELF/../.." && pwd)
if [ "${LOCAL_BUILD:-0}" != 1 ]; then
    HOST=${BUILD_HOST:-entertainment.local}
    REMOTE=${REMOTE_BUILD_ROOT:-/home/felix/tb330fu-release-sensors-iio-build}
    REMOTE_BINDER_NDK=${REMOTE_BINDER_NDK:-/home/felix/tb330fu-lenovo-factory-probe/lib/libbinder_ndk.so}
    ssh "$HOST" "mkdir -p '$REMOTE/src/sensors'"
    scp "$SELF/tb330fu-sensor-iio-producer.cpp" "$SELF/tb330fu_sensor_feed.h" "$SELF/build.sh" "$HOST:$REMOTE/src/sensors/"
    ssh "$HOST" "chmod +x '$REMOTE/src/sensors/build.sh'; LOCAL_BUILD=1 BINDER_NDK='$REMOTE_BINDER_NDK' '$REMOTE/src/sensors/build.sh'"
    mkdir -p "$ROOT/build/sensors/producer"
    scp "$HOST:$REMOTE/build/sensors/producer/tb330fu-sensor-iio-producer" "$ROOT/build/sensors/producer/"
    sha256sum "$ROOT/build/sensors/producer/tb330fu-sensor-iio-producer"
    exit 0
fi
AIDL_ROOT=${AIDL_ROOT:-/home/felix/android-aosp-headers/interfaces/sensors/aidl}
COMMON_ROOT=${COMMON_ROOT:-/home/felix/android-aosp-headers/interfaces/common/aidl}
FMQ_ROOT=${FMQ_ROOT:-/home/felix/android-aosp-headers/interfaces/common/fmq/aidl}
AIDL=${AIDL:-/home/felix/android-tools/android-sdk/build-tools/35.0.0/aidl}
NDK=${NDK:-/home/felix/android-tools/android-sdk/ndk/28.0.13004108}
BINDER_NDK=${BINDER_NDK:-/home/felix/tcl11-android-root/system-lib64/libbinder_ndk.so}
OUT=${OUT:-$ROOT/build/sensors/producer}; GEN=$OUT/generated; mkdir -p "$OUT"
rm -rf "$GEN"; mkdir -p "$GEN/src" "$GEN/include"
set -- $(find "$AIDL_ROOT/android" "$COMMON_ROOT/android" "$FMQ_ROOT/android" -type f -name '*.aidl' | sort)
"$AIDL" --lang=ndk --structured --stability=vintf --version 2 -I "$AIDL_ROOT" -I "$COMMON_ROOT" -I "$FMQ_ROOT" -o "$GEN/src" --header_out "$GEN/include" "$@"
SRC=$(find "$GEN/src" -type f -name '*.cpp' | sort)
INCS="-I$GEN/include -I$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include -I$ROOT/src/sensors"
INCS="$INCS -I/home/felix/android-aosp-headers/libfmq/include -I/home/felix/android-aosp-headers/libfmq/base -I/home/felix/android-aosp-headers/libbase/include -I/home/felix/android-aosp-headers/core/libcutils/include -I/home/felix/android-aosp-headers/core/libutils/include -I/home/felix/android-aosp-headers/logging/liblog/include -I/home/felix/android-aosp-headers/interfaces/common/support/include -I/home/felix/android-aosp-headers/native/libs/binder/ndk/include_cpp -I/home/felix/android-aosp-headers/native/libs/binder/ndk/include_platform"
"$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++" --target=aarch64-linux-android35 -std=c++20 -O2 -fPIE -pie -static-libstdc++ -D__ANDROID__ -D__ANDROID_VENDOR__ -DBINDER_STABILITY_SUPPORT -DPAGE_SIZE=4096 $INCS "$SELF/tb330fu-sensor-iio-producer.cpp" $SRC /home/felix/android-aosp-headers/libfmq/EventFlag.cpp /home/felix/android-aosp-headers/libfmq/FmqInternal.cpp "$BINDER_NDK" -L/home/felix/tcl11-android-root/system-lib64 -landroid -llog -lcutils -lutils -lbase -o "$OUT/tb330fu-sensor-iio-producer"
file "$OUT/tb330fu-sensor-iio-producer"
