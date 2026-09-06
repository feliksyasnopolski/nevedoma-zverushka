#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROTOC=${PROTOC:-/home/felix/protobuf-3.21-build/protoc-3.21.12.0}
export PROTOC
python3 "$ROOT/../protocol/regenerate_camera_proto.py" --protoc "$PROTOC" --check
python3 "$ROOT/../protocol/verify_camera_ipc_schema.py"
HDR=${HDR:?Set HDR to the AOSP camera/common header root}
NDK=${NDK:?Set NDK to the Android NDK root}
AIDL=${AIDL:?Set AIDL to the Android AIDL compiler}
PREBUILT=$NDK/toolchains/llvm/prebuilt/linux-x86_64
OUT=$ROOT/out
BINDER_NDK=${BINDER_NDK:?Set BINDER_NDK to target libbinder_ndk.so}
META_LIB=${META_LIB:?Set META_LIB to target libcamera_metadata.so}
FMQ_LIB=${FMQ_LIB:?Set FMQ_LIB to target libfmq.so}
CUTILS_LIB=${CUTILS_LIB:?Set CUTILS_LIB to target libcutils.so}
UTILS_LIB=${UTILS_LIB:?Set UTILS_LIB to target libutils.so}
BASE_LIB=${BASE_LIB:?Set BASE_LIB to target libbase.so}
LOG_LIB=${LOG_LIB:?Set LOG_LIB to target liblog.so}
FMQ_SRC_DIR=${FMQ_SRC_DIR:?Set FMQ_SRC_DIR to AOSP system/libfmq}
META_INC=${META_INC:?Set META_INC to camera metadata headers}
GRAPHICS_SNAPSHOT=${GRAPHICS_SNAPSHOT:?Set GRAPHICS_SNAPSHOT to the frozen graphics AIDL snapshot}
LIBPNG_INC=${LIBPNG_INC:?Set LIBPNG_INC to libpng headers}
LIBPNG_LIB=${LIBPNG_LIB:?Set LIBPNG_LIB to target libpng.so}
Z_LIB=${Z_LIB:?Set Z_LIB to target libz.so}
PROTOBUF_SRC_DIR=${PROTOBUF_SRC_DIR:?Set PROTOBUF_SRC_DIR to AOSP external/protobuf/src}
case "$LIBPNG_INC" in
    /usr/include|/usr/include/*|*/x86_64-linux-gnu|*/x86_64-linux-gnu/*)
        echo "LIBPNG_INC must be a target-compatible libpng include root, not host glibc headers" >&2
        exit 2
        ;;
esac
rm -rf "$OUT"; mkdir -p "$OUT/src" "$OUT/include/android/hardware/graphics/mapper"
git -C "$GRAPHICS_SNAPSHOT" show android-15.0.0_r17:graphics/mapper/stable-c/include/android/hardware/graphics/mapper/IMapper.h > "$OUT/include/android/hardware/graphics/mapper/IMapper.h"
set -- $(find "$HDR/interfaces/camera" "$HDR/interfaces/common/aidl" "$HDR/interfaces/common/fmq/aidl" "$GRAPHICS_SNAPSHOT/graphics/common/aidl" "$GRAPHICS_SNAPSHOT/graphics/allocator/aidl" -path '*/aidl/android/*' -type f -name '*.aidl' | sort)
"$AIDL" --lang=ndk --structured --stability=vintf --version 2 -I "$HDR/interfaces/camera/provider/aidl" -I "$HDR/interfaces/camera/device/aidl" -I "$HDR/interfaces/camera/common/aidl" -I "$HDR/interfaces/camera/metadata/aidl" -I "$HDR/interfaces/common/aidl" -I "$HDR/interfaces/common/fmq/aidl" -I "$GRAPHICS_SNAPSHOT/graphics/common/aidl" -I "$GRAPHICS_SNAPSHOT/graphics/allocator/aidl" -o "$OUT/src" --header_out "$OUT/include" "$@"
SRC=$(find "$OUT/src" -type f -name '*.cpp' | sort)
PB_CXXFLAGS="--target=aarch64-linux-android35 -std=c++20 -O2 -fPIC -Wno-deprecated-declarations -D__ANDROID__ -I $PROTOBUF_SRC_DIR -I $PROTOBUF_SRC_DIR/../android"
PB_SOURCES="any_lite.cc arena.cc arenastring.cc arenaz_sampler.cc extension_set.cc generated_enum_util.cc generated_message_tctable_lite.cc generated_message_util.cc implicit_weak_message.cc inlined_string_field.cc io/coded_stream.cc io/io_win32.cc io/strtod.cc io/zero_copy_stream.cc io/zero_copy_stream_impl.cc io/zero_copy_stream_impl_lite.cc map.cc message_lite.cc parse_context.cc repeated_field.cc repeated_ptr_field.cc stubs/bytestream.cc stubs/common.cc stubs/int128.cc stubs/status.cc stubs/statusor.cc stubs/stringpiece.cc stubs/stringprintf.cc stubs/structurally_valid.cc stubs/strutil.cc stubs/time.cc wire_format_lite.cc"
mkdir -p "$OUT/protobuf"
for source in $PB_SOURCES; do
    object="$OUT/protobuf/$(printf '%s' "$source" | tr / _).o"
    "$PREBUILT/bin/clang++" $PB_CXXFLAGS -c "$PROTOBUF_SRC_DIR/google/protobuf/$source" -o "$object"
done
"$PREBUILT/bin/llvm-ar" rcs "$OUT/libprotobuf-cpp-lite.a" "$OUT"/protobuf/*.o
"$PREBUILT/bin/clang++" --target=aarch64-linux-android35 -std=c++20 -O2 -Wno-deprecated-declarations -fPIE -pie -static-libstdc++ -D__ANDROID__ -DBINDER_STABILITY_SUPPORT -DPAGE_SIZE=4096 -I "$OUT/include" -I "$ROOT/../protocol" -I "$PROTOBUF_SRC_DIR" -I "$HDR/native/libs/binder/ndk/include_cpp" -I "$HDR/native/libs/binder/ndk/include_platform" -I "$HDR/core/libcutils/include" -I "$HDR/core/libutils/include" -I "$HDR/logging/liblog/include" -I "$HDR/libfmq/include" -I "$HDR/libfmq/base" -I "$HDR/libbase/include" -I "$META_INC" -idirafter "$LIBPNG_INC" "$ROOT/tb330fu-camera-daemon.cpp" "$ROOT/../protocol/tb330fu_camera.pb.cc" "$FMQ_SRC_DIR/EventFlag.cpp" "$FMQ_SRC_DIR/FmqInternal.cpp" $SRC "$OUT/libprotobuf-cpp-lite.a" "$BINDER_NDK" "$META_LIB" "$FMQ_LIB" "$CUTILS_LIB" "$UTILS_LIB" "$BASE_LIB" "$LOG_LIB" "$LIBPNG_LIB" "$Z_LIB" -ldl -o "$OUT/tb330fu-camera-daemon"
file "$OUT/tb330fu-camera-daemon"; sha256sum "$OUT/tb330fu-camera-daemon"; readelf -d "$OUT/tb330fu-camera-daemon" | grep NEEDED || true
