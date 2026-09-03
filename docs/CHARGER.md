# TB330FU charger mode

## Purpose

Charger mode is an early-boot operating mode selected when the bootloader
provides the charger boot metadata. It presents a native Linux battery screen
without starting systemd, Android userspace, or the desktop session.

The current implementation is native Linux code. It is not the stock Android
`/system/bin/charger` program.

## Architecture

```text
bootloader DT chosen/atag,boot
    |
stage1 reads the little-endian boot-mode field
    |
mode 0x8 -> read-only Ubuntu root and switch_root
    |
/usr/libexec/tb330fu-charger (PID 1)
    |
power_supply + DRM/KMS
```

Normal boot takes the same early root-discovery path, then mounts the Ubuntu
root read-write and starts `/sbin/init`.

## Components

| Component | Role |
|---|---|
| `src/charger/tb330fu-native-charger-renderer.cpp` | Native charger renderer source |
| `src/charger/Makefile` | AArch64 cross-build definition |
| `/usr/libexec/tb330fu-charger` | Deployed runtime executable |

No charger systemd unit is present or required. Stage1 invokes the executable
directly after `switch_root`; systemd is not involved in charger mode.

## Boot-mode detection

Stage1 reads `/sys/firmware/devicetree/base/chosen/atag,boot` with BusyBox
`hexdump`, starting at byte offset 8 and reading four bytes. The bytes are
decoded as a little-endian integer. Only numeric value `0x8` selects charger
mode. A missing, malformed, or unreadable value falls back to normal boot.

In charger mode stage1 mounts the selected Ubuntu root read-only, creates
`/dev/dri/card0` from the DRM class device with mode `0600`, and optionally
creates `/dev/pmsg0` with mode `0222`. It moves `/proc`, `/sys`, and `/dev`
into the new root and executes:

```text
/usr/libexec/tb330fu-charger
```

## Runtime

The renderer is a dynamically linked AArch64 PIE executable using glibc,
libdrm, and FreeType. It runs as the process supplied to `switch_root`, and
therefore becomes PID 1 for this charger path. It does not fork and does not
start a service manager.

At startup it discovers one `type=Battery` supply and one charger-like supply
whose type is one of `Mains`, `USB`, `USB_C`, `USB_DCP`, `USB_PD`, or
`Charger`. It reads the battery `capacity` and `status` files. It redraws once
per second when a subsequent read succeeds.

The screen is black with a battery outline, capacity percentage, charging
state, and the text `TB330FU native Linux charger renderer`. The renderer
uses two DRM dumb buffers and page flips. It selects connected DSI-1, prefers
the `1200x1920` mode, and renders the logical frame with the source rotation
used by the current implementation.

## Display and input

The renderer opens `/dev/dri/card0` directly with libdrm. It creates and maps
two 32-bit dumb buffers, modesets the selected CRTC, and presents subsequent
frames with DRM page-flip events.

It does not open evdev devices, process `KEY_POWER`, or write a backlight
sysfs control. Touch, folio, desktop input policy, and normal display policy
are outside this subsystem.

## Power and battery interfaces

The executable scans `/sys/class/power_supply`. It requires a battery supply
with readable `capacity` and `status` files. A charger-like supply is used for
identification and logging; its `online` value is not read by the renderer.

The current tablet exposes `battery` (`type=Battery`) and `charger`
(`type=USB_DCP`). The native runtime also exposes vendor charger supplies,
but the renderer does not depend on their names.

## Exit paths

The renderer handles `SIGTERM` and `SIGINT` by stopping its redraw loop and
then sleeping permanently. It does not reboot, power off, jump to normal
boot, or exec another process.

Power-key behavior in charger mode is therefore not implemented by this
process. External-power removal handling is also not implemented as a
transition: if a later power-supply read succeeds, the displayed battery
status is refreshed; if it fails, the last valid frame remains. The result of
an actual charger-mode power-key or unplug test is unverified here.

## Build

Build from the charger source directory with the Makefile:

```bash
make -C src/charger
```

The build requires an AArch64 cross-compiler, libdrm headers and library, and
FreeType headers and library. The resulting target is named
`tb330fu-native-charger-renderer`; the deployed installation path is
`/usr/libexec/tb330fu-charger`.

## Installation

The runtime target is `/usr/libexec/tb330fu-charger` on the Ubuntu root. It is
consumed by stage1 and is not installed or managed by systemd. This release
tree contains source and build instructions only; it does not contain the
deployed binary.

## Verification

Static verification can check the boot-mode parser, executable dependencies,
DRM and power-supply paths, and source/build correspondence.

Normal-runtime read-only verification can check the deployed executable and
the available power-supply interfaces:

```bash
ssh nevedoma-zverushka.local 'sha256sum /usr/libexec/tb330fu-charger; find /sys/class/power_supply -maxdepth 2 -type f -name type -print'
```

A charger-mode boot should verify the first DRM frame and the behavior of
power-key and external-power transitions. Fatal-path verification should
confirm that an exact fatal record is emitted while PID 1 remains alive.

## Failure modes

Stage1 holds PID 1 alive if the charger root cannot be mounted, the charger
executable is missing or not executable, required mountpoints are missing, or
the DRM device node cannot be created. Failure to parse `atag,boot` selects
normal boot instead.

The renderer permanently sleeps after a battery/power-supply discovery
failure, FreeType initialization failure, DRM initialization failure,
modeset failure, page-flip failure, or uncaught exception. It logs the exact
failure with the `TB330FU CHARGER FATAL:` prefix.

## Known limitations

The charger process directly owns early DRM access because systemd and the
desktop are not active. It has no input policy, power-key action, backlight
control, unplug transition, normal-boot transition, or shutdown policy.

Charger-mode behavior requires an actual charger-mode boot for runtime
confirmation.
