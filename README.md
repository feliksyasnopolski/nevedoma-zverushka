# TB330FU Linux

Native Linux userspace for the Lenovo Tab M11 TB330FU.

The tablet runs Ubuntu/Kubuntu userspace with systemd as PID 1 on the
stock-derived Android kernel and MediaTek BSP. Native Linux owns the
desktop, device policy, graphics userspace, power policy, and ordinary
system services. Android-derived components are retained only where they
are required to operate the hardware.

The current system uses:

- native DRM/KMS with the MediaTek display driver;
- Panfrost for Mali-G52 rendering;
- KDE Plasma / KWin on Wayland;
- systemd for service and policy management;
- a small vendor substrate for hardware that still depends on Android/
  MediaTek userspace interfaces;
- `tb330fu-devd` to provide device-node lifecycle on a kernel built
  without devtmpfs.

This repository is the clean, maintainable source tree.

Bring-up experiments, raw reverse-engineering material, stock images,
vendor dumps, proprietary firmware, calibration data, APKs, neural
models, and historical working directories are intentionally kept
outside this repository.

## Documentation

Start with:

- `docs/ARCHITECTURE.md` — system architecture and ownership boundaries.
- `docs/BOOT.md` — custom vendor ramdisk, stage1, and vendor_boot build.
- `docs/BUILD.md` — build and deployment procedures.
- `docs/RECOVERY.md` — recovery-critical artifacts and procedures.
- `docs/INPUT.md` — touchscreen, stylus, and folio lid integration.
- `docs/SENSORS.md` — Android sensor HAL to Linux IIO integration.
- `docs/CHARGER.md` — native early-boot charger-mode runtime.
- `docs/AUDIO.md` — native MT6768/MT6358 ALSA, UCM, PipeWire, and WirePlumber path.
- `docs/PLYMOUTH.md` — early native graphical startup and SDDM handoff.
- `docs/INIT_CORPUS.md` — Android init corpus provenance and graph carving.
- `docs/GRAPH_COMPILER.md` — parser, compiler transformations, and graph contract.
- `docs/VENDOR_RUNTIME.md` — graph executor, retained vendor services, and readiness gate.
- `docs/BLUETOOTH.md` — retained MediaTek Bluetooth path, HCI bridge, and BlueZ interface.
- `docs/CAMERA.md` — retained Camera3 hardware exposed through standard V4L2.

Subsystem documentation lives under `docs/`.

Documentation describes the current system. Historical experiments and
investigation logs are not part of the primary documentation set.

## Project principles

Prefer standard Linux interfaces and services over board-specific code.

Keep Android/vendor compatibility below the native Linux policy layer.

Do not reproduce Android behavior when an existing Linux subsystem can
own the same function.

Preserve recovery-critical stock artifacts, kernel ABI data, module BTF,
and build provenance outside the release repository where redistribution
is not appropriate.

Keep board-specific code small, explicit, and replaceable.

## Status

The tablet boots from internal storage into a native KDE Plasma Wayland
session with working DRM/KMS, Panfrost, input, Wi-Fi, Bluetooth, audio,
sensors, camera bridge, suspend, and other board-specific services.

The project is still being consolidated from the original bring-up
workspace. This repository contains only material that has been reviewed
and promoted into the canonical implementation.
