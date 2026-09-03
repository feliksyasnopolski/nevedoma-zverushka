#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
NDK=${NDK:?Set NDK to the Android NDK root}
AIDL=${AIDL:?Set AIDL to the Android build-tools aidl executable}
AOSP_BINDER_INC=${AOSP_BINDER_INC:?Set AOSP_BINDER_INC to AOSP native/libs/binder/ndk/include_cpp}
AOSP_BINDER_PLATFORM_INC=${AOSP_BINDER_PLATFORM_INC:?Set AOSP_BINDER_PLATFORM_INC to AOSP native/libs/binder/ndk/include_platform}
BINDER_NDK=${BINDER_NDK:?Set BINDER_NDK to the target libbinder_ndk.so}
TARGET=${TARGET:-aarch64-linux-android35}
OUT=${OUT:-$ROOT/build}
PREBUILT=${NDK}/toolchains/llvm/prebuilt/linux-x86_64

rm -rf "$OUT"
mkdir -p "$OUT/src" "$OUT/include"
"$AIDL" --lang=ndk --structured --stability=vintf -I "$ROOT" -o "$OUT/src" --header_out "$OUT/include" \
  "$ROOT/android/hardware/bluetooth/IBluetoothHci.aidl" \
  "$ROOT/android/hardware/bluetooth/IBluetoothHciCallbacks.aidl" \
  "$ROOT/android/hardware/bluetooth/Status.aidl"

SRC=$(find "$OUT/src" -type f -name '*.cpp')
"$PREBUILT/bin/clang++" --target="$TARGET" -std=c++20 -O2 -fPIE -pie \
  -static-libstdc++ -D__ANDROID__ -DBINDER_STABILITY_SUPPORT -I "$OUT/include" \
  -I "$AOSP_BINDER_INC" -I "$AOSP_BINDER_PLATFORM_INC" \
  "$ROOT/tb330fu_android_bt_aidl_bridge.cpp" $SRC "$BINDER_NDK" \
  -o "$OUT/tb330fu-android-bt-aidl-bridge"

file "$OUT/tb330fu-android-bt-aidl-bridge"
readelf -d "$OUT/tb330fu-android-bt-aidl-bridge" | grep NEEDED
sha256sum "$OUT/tb330fu-android-bt-aidl-bridge"
