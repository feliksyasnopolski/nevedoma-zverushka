#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
SOURCE_IMAGE=${1:?usage: build_vendor_boot.sh STOCK_VENDOR_BOOT BUSYBOX MODULE_DIR OUTPUT_IMAGE}
BUSYBOX=${2:?usage: build_vendor_boot.sh STOCK_VENDOR_BOOT BUSYBOX MODULE_DIR OUTPUT_IMAGE}
MODULE_DIR=${3:?usage: build_vendor_boot.sh STOCK_VENDOR_BOOT BUSYBOX MODULE_DIR OUTPUT_IMAGE}
OUTPUT_IMAGE=${4:?usage: build_vendor_boot.sh STOCK_VENDOR_BOOT BUSYBOX MODULE_DIR OUTPUT_IMAGE}
CC=${CC:-aarch64-linux-gnu-gcc}
WORK_PARENT=${WORK_PARENT:-"$PROJECT_DIR/.build/boot"}
mkdir -p "$WORK_PARENT"
RUN_DIR=$(mktemp -d "$WORK_PARENT/run.XXXXXX")
ROOT="$RUN_DIR/root"

case "$(uname -s)" in Linux) ;; *) echo "refusing: run on Linux" >&2; exit 1 ;; esac
command -v "$CC" >/dev/null
command -v magiskboot >/dev/null
command -v cpio >/dev/null
command -v sha256sum >/dev/null
[ -r "$BUSYBOX" ] || { echo "missing BusyBox: $BUSYBOX" >&2; exit 1; }
[ -d "$MODULE_DIR" ] || { echo "missing module directory: $MODULE_DIR" >&2; exit 1; }
mkdir -p "$ROOT/bin" "$ROOT/etc" "$ROOT/lib/modules" "$(dirname -- "$OUTPUT_IMAGE")"

"$CC" -static -O2 -s -Wall -Wextra -Werror \
    -o "$ROOT/init" "$PROJECT_DIR/src/boot/init.c"
cp "$BUSYBOX" "$ROOT/bin/busybox"
cp "$PROJECT_DIR/src/boot/stage1.sh" "$ROOT/etc/stage1.sh"
chmod 0755 "$ROOT/init" "$ROOT/etc/stage1.sh" "$ROOT/bin/busybox"

while IFS= read -r module || [ -n "$module" ]; do
    case "$module" in ''|'#'*) continue ;; esac
    [ -r "$MODULE_DIR/$module" ] || { echo "missing early module: $module" >&2; exit 1; }
    cp "$MODULE_DIR/$module" "$ROOT/lib/modules/$module"
done < "$PROJECT_DIR/manifests/modules.load.native"
cp "$PROJECT_DIR/manifests/modules.load.native" "$ROOT/lib/modules/modules.load.native"

find "$ROOT" -exec touch -h -d '@0' {} +
(cd "$ROOT" && find . -print0 | LC_ALL=C sort -z |
    cpio --null -o -H newc --owner=0:0 --reproducible > "$RUN_DIR/vendor-ramdisk.cpio")
magiskboot compress=lz4_legacy "$RUN_DIR/vendor-ramdisk.cpio" "$RUN_DIR/vendor-ramdisk.lz4"
"$SCRIPT_DIR/rebuild_vendor_boot.py" rebuild "$SOURCE_IMAGE" \
    "$RUN_DIR/vendor-ramdisk.lz4" "$OUTPUT_IMAGE"
"$SCRIPT_DIR/validate_vendor_boot.py" "$SOURCE_IMAGE" "$OUTPUT_IMAGE" \
    --extract-candidate "$RUN_DIR/validated" > "$OUTPUT_IMAGE.validation.json"
magiskboot decompress "$RUN_DIR/validated/fragment-02.lz4" "$RUN_DIR/roundtrip.cpio"
cmp "$RUN_DIR/vendor-ramdisk.cpio" "$RUN_DIR/roundtrip.cpio"
sha256sum "$SOURCE_IMAGE" "$RUN_DIR/vendor-ramdisk.cpio" \
    "$RUN_DIR/vendor-ramdisk.lz4" "$OUTPUT_IMAGE" > "$OUTPUT_IMAGE.SHA256SUMS"
printf '%s\n' "$OUTPUT_IMAGE"
