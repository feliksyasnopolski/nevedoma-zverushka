# TB330FU boot

## Purpose

The custom vendor ramdisk replaces the Android init path with a small BusyBox
stage1. Stage1 enters the native Ubuntu root and starts systemd.

This path is not a conventional Ubuntu initramfs.

## Architecture

```text
bootloader
    |
Android-derived kernel
    |
custom vendor ramdisk
    |
BusyBox stage1 (PID 1)
    |
early modules + root discovery
    |
+----------------------+
|                      |
normal                 charger
|                      |
RW root                RO root
|                      |
/sbin/init              tb330fu-charger
```

The kernel and vendor compatibility inputs remain Android-derived. The
vendor ramdisk supplies the project PID 1 path. The Ubuntu root supplies
`/sbin/init`, systemd, and the charger executable.

## Boot artifacts

| Artifact | Status | Role |
|---|---|---|
| `src/boot/init.c` | source | Static PID 1 wrapper that execs BusyBox stage1 |
| `src/boot/stage1.sh` | source | Early mounts, module loading, root selection, and handoff |
| `manifests/modules.load.native` | tracked manifest | Fixed 154-entry early module load order |
| `scripts/rebuild_vendor_boot.py` | source tool | Android vendor_boot v4 parser and fragment replacer |
| `scripts/validate_vendor_boot.py` | source tool | Structural and preservation validator |
| `scripts/build_vendor_boot.sh` | source tool | Public assembly and repack workflow |

Generated images, ramdisks, BusyBox, kernel modules, firmware, and stock
images are external or ignored artifacts.

## Stage1

`init` runs as PID 1 and executes `/bin/busybox sh /etc/stage1.sh`.
Stage1 mounts proc, sysfs, a tmpfs `/dev`, and devpts, then creates the static
nodes needed before the general device-node service exists. The relationship
to that later service is:

```text
stage1 -> minimum early nodes
tb330fu-devd -> general device-node lifecycle after systemd starts
```

See [DEVD.md](DEVD.md).

Stage1 reads the boot-mode metadata, loads the fixed early module closure,
waits for the MMC providers, discovers a root, moves `/proc`, `/sys`, and
`/dev` into it, and executes `switch_root`.

## Early modules

The canonical request order is tracked in
`manifests/modules.load.native`. It contains 154 entries: the stock-derived
normal-boot request list plus the hard dependency closure required by the
current path. Stage1 uses BusyBox `insmod` in that order. A module that fails
to load is fatal unless the matching module is already present in
`/sys/module`.

The `.ko` files and their stock metadata are not redistributed here. Obtain
matching files from supported Lenovo stock firmware or the controlled kernel
build provenance. The module payload must preserve the matching BTF, vermagic, and modversion requirements of the target kernel.

## Root selection

The current selection is controller-based and internal-first:

| Priority | Controller | Selected partition | Mount |
|---|---|---|---|
| 1 | `11230000.mmc` | discovered disk partition 52 | read-write in normal mode; read-only in charger mode |
| 2 | `11240000.mmc` | discovered disk partition 1 | same mode-dependent mount |

Stage1 discovers the host and disk below each controller from sysfs, creates
block nodes from their major/minor values, and then mounts the selected
partition as ext4. It does not promise fixed `mmcblk` numbering. The known
internal identity is `/dev/mmcblk0p52`, ext4, label `ubuntu-root-inte`, UUID
`84e8d023-6d9e-462c-b900-65319c593cb8`.

If internal discovery fails, stage1 probes the SD path. If an internal root is
found but its mount fails, it also probes the SD path. The current code may
wait through the SD discovery timeout even when internal storage is present
and no SD card is inserted. This is a known limitation.

## Charger-mode branch

Stage1 reads `/sys/firmware/devicetree/base/chosen/atag,boot` at offset 8 as a
little-endian u32. Only value `0x8` selects charger mode. Normal and charger
boot share module loading and root discovery. Charger mode mounts the selected
root read-only and executes `/usr/libexec/tb330fu-charger` after `switch_root`.

See [CHARGER.md](CHARGER.md) for the charger runtime.

## Normal switch_root

Normal mode mounts the selected root read-write, requires an executable
`/newroot/sbin/init`, and executes:

```text
/bin/switch_root /newroot /sbin/init
```

`/sbin/init` starts systemd on the Ubuntu root.

## vendor_boot build

The public build requires a supported Lenovo stock-derived `vendor_boot` v4
image, an external static AArch64 BusyBox, and the exact external module
directory listed in the stock-input manifest:

```bash
scripts/build_vendor_boot.sh STOCK_VENDOR_BOOT BUSYBOX MODULE_DIR OUTPUT_IMAGE
```

The builder compiles the PID 1 wrapper, creates a reproducible newc CPIO,
installs stage1, BusyBox, the module payloads, and `modules.load.native`, then
compresses the CPIO with legacy LZ4. `rebuild_vendor_boot.py` replaces exactly
the platform `init_boot` fragment and preserves the other vendor ramdisk
fragments and container metadata. The validator checks the result and the
builder performs a compressed-fragment round-trip check.

The source image is not flattened. No stock image, raw stock ramdisk,
proprietary module, firmware, or device-specific data belongs in Git.

See `manifests/stock-inputs.txt` for the complete input boundary.

## Stock inputs and provenance

Stock-derived kernel modules and vendor ramdisk fragments are opaque external
inputs. Their exact firmware version, hashes, extraction record, and license
status must be retained outside this repository. This project does not assert
redistribution rights for Lenovo or MediaTek modules.

The supported container is Android vendor_boot v4 with one platform fragment
named `init_boot`. The builder rejects an ambiguous or missing replacement
target.

## Installation and recovery

This repository documents construction only. Flashing is a manual operation
and is not performed by the build script. Preserve the known-good stock
`vendor_boot`, boot image, and other recovery-critical partitions before any
installation. A known-good vendor_boot can restore the original early boot
path; an SD rescue root provides the current stage1 fallback when its
controller and partition are available.

## Verification

Source and generated-image validation can be performed offline. Runtime verification after installation uses:

```bash
uname -a
findmnt /
cat /etc/tb330fu-internal-root
ps -p 1 -o comm=
```

Expected evidence is the Android-derived kernel, the intended ext4 root,
the project marker, and `systemd` as PID 1. These checks do not replace
physical or charger-mode confirmation.

## Failure modes

* A missing or incompatible early module stops stage1 with PID 1 held alive.
* Failure to discover both controller paths holds PID 1 alive.
* Missing or malformed charger metadata selects normal boot.
* An ext4 mount failure holds PID 1 alive; internal-root mount failure can
  enter the SD rescue probe.
* Missing or non-executable `/sbin/init` prevents normal handoff.
* A malformed vendor_boot, wrong fragment target, or failed round-trip rejects
  the generated image before installation.

## Known limitations

The SD fallback probe is currently not skipped immediately after internal-root
availability in all failure paths. Its timeout is preserved as current
behavior.