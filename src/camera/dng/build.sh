#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CXX=${CXX:-g++}
OUT=${OUT:-$ROOT/out}
DNGSDK_ROOT=${DNGSDK_ROOT:?Set DNGSDK_ROOT to the official Adobe DNG SDK source headers}
DNG_LIB=${DNG_LIB:?Set DNG_LIB to the target-built libdng_sdk.a}
JXL_INC=${JXL_INC:-}
JXL_LIB_INC=${JXL_LIB_INC:-}
mkdir -p "$OUT"
INCLUDES="-I$DNGSDK_ROOT/source"
if [ -n "$JXL_INC" ]; then INCLUDES="$INCLUDES -I$JXL_INC"; fi
if [ -n "$JXL_LIB_INC" ]; then INCLUDES="$INCLUDES -I$JXL_LIB_INC"; fi
# The Adobe headers include dng_jxl.h even when JPEG XL support is disabled;
# the installed SDK build supplies the disabled implementation in libdng_sdk.a.
# Keep the compatibility platform definitions aligned with the SDK build.
"$CXX" -std=c++17 -O2 -Wall -Wextra \
    -DBIB_MULTI_THREAD=1 -DUNICODE -D_CONSOLE -D_CRT_SECURE_NO_DEPRECATE \
    -DqAndroid=0 -DqDNGReportErrors=1 -DqDNGUseXMP=0 -DqDNGValidateTarget=0 \
    -DqDebugPixelType=0 -DqLinux=1 -DqLogDNGUpdateMetadata=0 -DqMacOS=0 \
    -DqOptGetBitsMath=0 -DqVisualC=0 -DqWeb=0 -DqWinOS=0 -DqiPhone=0 \
    $INCLUDES "$ROOT/tb330fu-camera-dng-producer.cpp" "$DNG_LIB" \
    -lz -pthread -o "$OUT/tb330fu-camera-dng-producer"
file "$OUT/tb330fu-camera-dng-producer"
sha256sum "$OUT/tb330fu-camera-dng-producer"
