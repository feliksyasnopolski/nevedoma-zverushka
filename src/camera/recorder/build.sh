#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUT=${OUT:-$ROOT/out}
mkdir -p "$OUT"
python3 "$ROOT/../protocol/regenerate_camera_proto.py" --check
CXX=${CXX:-aarch64-linux-gnu-g++}
FFMPEG_CFLAGS=${FFMPEG_CFLAGS:--I/usr/include/aarch64-linux-gnu}
FFMPEG_LIBS=${FFMPEG_LIBS:--L/usr/lib/aarch64-linux-gnu -lavcodec -lavformat -lavutil}
LDFLAGS=${LDFLAGS:--Wl,--allow-shlib-undefined}
PROTOBUF_SRC_DIR=${PROTOBUF_SRC_DIR:-/home/felix/aosp15-init/external/protobuf/src}
PROTOBUF_LITE_A=$OUT/libprotobuf-cpp-lite-glibc.a
PB_SOURCES="any_lite.cc arena.cc arenastring.cc extension_set.cc generated_enum_util.cc generated_message_tctable_lite.cc generated_message_util.cc implicit_weak_message.cc inlined_string_field.cc io/coded_stream.cc io/io_win32.cc io/strtod.cc io/zero_copy_stream.cc io/zero_copy_stream_impl.cc io/zero_copy_stream_impl_lite.cc map.cc message_lite.cc parse_context.cc repeated_field.cc repeated_ptr_field.cc stubs/bytestream.cc stubs/common.cc stubs/int128.cc stubs/status.cc stubs/statusor.cc stubs/structurally_valid.cc stubs/strutil.cc stubs/stringpiece.cc stubs/stringprintf.cc stubs/time.cc wire_format_lite.cc"
mkdir -p "$OUT/protobuf"
for source in $PB_SOURCES; do
  object="$OUT/protobuf/$(printf '%s' "$source" | tr / _).o"
  "$CXX" -std=c++20 -O2 -fPIC -I"$PROTOBUF_SRC_DIR" -I"$PROTOBUF_SRC_DIR/../android" -c "$PROTOBUF_SRC_DIR/google/protobuf/$source" -o "$object"
done
"${AR:-aarch64-linux-gnu-ar}" rcs "$PROTOBUF_LITE_A" "$OUT"/protobuf/*.o
PROTOBUF="-I$PROTOBUF_SRC_DIR $PROTOBUF_LITE_A"
"$CXX" -std=c++20 -O2 -fPIE -pie -I"$ROOT/../protocol" -I"$ROOT/../video" $FFMPEG_CFLAGS \
  "$ROOT/tb330fu-video-recorder.cpp" "$ROOT/../protocol/tb330fu_camera.pb.cc" \
  $PROTOBUF $FFMPEG_LIBS $LDFLAGS -pthread -o "$OUT/tb330fu-video-recorder"
file "$OUT/tb330fu-video-recorder"
sha256sum "$OUT/tb330fu-video-recorder"
