# NevedomaZverushka

NevedomaZverushka is a native Linux device-enablement stack for the Lenovo Tab M11 / TB330FU.

It boots Ubuntu/Kubuntu directly on the tablet's stock-derived Android 15 Linux 6.6 kernel and MediaTek BSP, with systemd, KDE Plasma, KWin/Wayland, DRM/KMS, Mesa/Panfrost, and ordinary Linux userspace.

Where MediaTek hardware still depends on Android/vendor machinery, that machinery is retained below the Linux policy layer and translated into normal Linux interfaces.

There is no Android framework, Android UI, SurfaceFlinger desktop, chroot, or container in the runtime.

![Tux in Android wedding dress](docs/images/tux-with-boobs.png)

## Name

The name comes from Pushkin's *The Tale of Tsar Saltan*:

> Родила царица в ночь
>
> Не то сына, не то дочь,
>
> Не мышонка, не лягушку,
>
> А неведому зверушку.

Roughly, or literally: “The tsarina gave birth that night — neither quite a son nor a daughter, neither a little mouse nor a frog, but some unknown beastie.”

The tablet is no longer meaningfully an Android tablet, but it is not a conventional mainline-Linux device either.

It is a strange hybrid: native Linux on top of retained MediaTek/Android machinery.

That is why “NevedomaZverushka” fits.

## What works today

The tablet boots from internal storage into a normal KDE Plasma desktop with accelerated Panfrost graphics, audio, Wi-Fi, Bluetooth, sensors, touchscreen/stylus input, suspend/wake, USB networking, and a native camera stack.

Android-specific hardware interfaces are terminated below the desktop and republished through ordinary Linux interfaces such as DRM, V4L2, IIO, HCI, ALSA, input, and standard userspace services.

## What this is

The target is an AArch64 tablet using a stock-derived Android 15 Linux 6.6 kernel and the MediaTek MT6768-family BSP.

The native root contains Ubuntu userspace, systemd as PID 1, KDE Plasma, KWin/Wayland, native DRM/KMS userspace, Mesa/Panfrost, and normal Linux services and interfaces.

The kernel and selected vendor components remain Android-derived because they remain the practical hardware interface on this device.

Those components are kept below the native Linux policy layer and are exposed through adapters where necessary.

This is not Linux inside Android, a chroot, a container, an Android desktop shell, or a generic mainline-Linux port.

## Architecture

```text
Linux applications / KDE Plasma
            |
     normal Linux APIs
            |
 DRM  V4L2  IIO  HCI  ALSA  input  properties
            |
 native adapters / compatibility services
            |
 retained MediaTek / Android vendor machinery
            |
          hardware
```

Linux owns the desktop, session and power policy, compositor, device policy, and ordinary userspace services.

Vendor machinery remains where it is the practical hardware-facing implementation.

Compatibility layers terminate Android-specific interfaces and expose Linux contracts above them.

Not every subsystem uses every layer in this diagram.

Examples of the boundary include:

- Camera3 → native capture daemon → V4L2 preview/video, JPEG/DNG still capture, MP4 recording, and an app-facing HTTP/WebSocket API.
- Android sensors → IIO.
- Bluetooth HCI AIDL → Linux HCI/BlueZ.
- Android properties → the Rediska property ABI.
- Vendor init corpus → native graph executor.

## Current status

| Area | Status |
|---|---|
| Boot | Working: custom `vendor_boot` stage1 reaches the native Ubuntu root and systemd. |
| Internal-root Linux | Working: internal storage is preferred, with an SD fallback path. |
| KDE/Wayland | Working: KDE Plasma and KWin run as the native desktop session. |
| DRM/KMS | Working: the MediaTek DRM driver owns panel scanout. |
| Panfrost | Working: Mesa/Panfrost provides Mali-G52 rendering. |
| Touchscreen | Working through the normal Linux input path. |
| Stylus | Working with TB330FU udev and hwdb policy. |
| Folio/lid | Working with known limitation: the Hall-to-`SW_LID` proxy cannot recover a pre-existing closed state until a transition. |
| Sensors | Working for accelerometer, gyroscope, light, and step-detector paths through IIO; interfaces publish latest values rather than buffered streams. |
| Audio | Working through native ALSA/UCM with PipeWire and WirePlumber desktop integration. |
| Bluetooth | Working through the retained MediaTek AIDL path, HCI bridge, Linux `hci0`, and BlueZ; startup is manual. |
| Wi-Fi | Mixed ownership: MediaTek/vendor bring-up remains retained, while native Linux owns the exposed networking policy. |
| Cameras | Working: rear-camera preview through V4L2, JPEG/DNG still capture, MP4 recording, an app-facing HTTP/WebSocket control API, and a native Flutter camera application. The front camera is intentionally left to ordinary V4L2 conferencing/video-call applications. |
| USB NCM | Working as the native configfs NCM gadget on `usb0`; Android USB modes are not part of this service. |
| zram | Working through the native systemd-owned `/dev/zram0` setup. |
| Suspend/wake | Working with display preflight and folio/power-button wake; double-tap-to-wake is not implemented. |
| Charger mode | Native early-boot charger renderer implemented; power-key, unplug, and transition-to-normal-boot handling are not implemented. |
| Android property compatibility | Working as the retained property ABI used by vendor-facing components. |
| Vendor runtime compatibility | Working for the selected vendor init corpus; the native executor implements the Android-init semantics required by retained vendor services. |
| GNSS | Not integrated; the vendor GNSS path is present but native functional behavior is unverified. |

Status is intentionally narrower than service liveness.

The detailed subsystem documents distinguish static, runtime, and operator-dependent evidence.

## Android and vendor machinery retained

Retaining selected Android/vendor components is a hardware-boundary decision, not a claim that Android is still the operating system.

The Android framework, SurfaceFlinger desktop path, Android UI, and Android applications are absent.

The retained layer can include Binder/AIDL vendor services, MediaTek hardware daemons, the camera HAL, the sensor HAL, the Bluetooth vendor service, Android property semantics, stock kernel modules, and a vendor-init-derived runtime graph.

These components remain only where they are required by the hardware path or are cheaper and more reliable than replacing them.

Native Linux owns the interfaces and policy above them wherever practical: KWin owns display policy, BlueZ owns host Bluetooth policy, PipeWire/WirePlumber own desktop audio policy, iio-sensor-proxy consumes standard IIO devices, and the native USB service owns the NCM gadget.

## Repository layout

| Directory | Contents |
|---|---|
| [`src/`](src/) | Native helpers and Android-facing bridge sources. |
| [`modules/`](modules/) | Project kernel-module sources and build descriptions. |
| [`systemd/`](systemd/) | Service and target unit sources. |
| [`docs/`](docs/) | Current architecture, build, subsystem, and compatibility documentation. |
| [`vendor/init-corpus/`](vendor/init-corpus/) | Stock-derived init corpus inputs selected for the native runtime graph. |
| [`tools/`](tools/) | Parsers, compilers, validators, and supporting build tools. |
| [`patches/`](patches/) | Deliberate source patches retained in the release tree. |
| [`scripts/`](scripts/) | Build, extraction, validation, and installation support scripts. |
| [`manifests/`](manifests/) | External-input and module-order manifests. |

Licensing and provenance boundaries are documented in [`docs/LICENSING.md`](docs/LICENSING.md) and [`vendor/init-corpus/PROVENANCE.md`](vendor/init-corpus/PROVENANCE.md).

Generated boot images, deployed binaries, stock images, proprietary firmware, calibration and security material, and other recovery-critical external inputs are not release-tree source files.

## Documentation

| Topic | Document |
|---|---|
| Architecture and ownership | [`ARCHITECTURE.md`](docs/ARCHITECTURE.md) |
| Build environment and provenance | [`BUILD.md`](docs/BUILD.md) |
| Boot and `vendor_boot` | [`BOOT.md`](docs/BOOT.md) |
| Vendor runtime graph | [`VENDOR_RUNTIME.md`](docs/VENDOR_RUNTIME.md) |
| Android property ABI | [`PROPERTIES.md`](docs/PROPERTIES.md) |
| Device-node lifecycle | [`DEVD.md`](docs/DEVD.md) |
| Display and power | [`DISPLAY_POWER.md`](docs/DISPLAY_POWER.md) |
| GPU and Panfrost | [`GPU.md`](docs/GPU.md) |
| Input and folio lid | [`INPUT.md`](docs/INPUT.md) |
| Sensors | [`SENSORS.md`](docs/SENSORS.md) |
| Audio | [`AUDIO.md`](docs/AUDIO.md) |
| Bluetooth | [`BLUETOOTH.md`](docs/BLUETOOTH.md) |
| Cameras | [`CAMERA.md`](docs/CAMERA.md) |
| USB NCM | [`USB.md`](docs/USB.md) |
| zram | [`ZRAM.md`](docs/ZRAM.md) |
| Charger mode | [`CHARGER.md`](docs/CHARGER.md) |

The documentation describes the current system.

Implementation reports, validation logs, historical experiments, and private stock/evidence workspaces are not prerequisites for understanding the release tree.

## Build environment

Build provenance is part of the release contract.

Start with [`docs/BUILD.md`](docs/BUILD.md) when reconstructing the environment.

The build requires exact external inputs, including Android toolchain/AOSP/NDK provenance, the target kernel's prepared output with matching BTF and symbol CRC metadata, and stock/proprietary BSP inputs such as vendor modules, firmware, and target Android libraries.

Native Linux programs use the AArch64 glibc toolchain; Android-facing bridges use the AArch64 bionic/NDK toolchain and target libraries.

This repository alone is intentionally not a complete redistributable device image. Reproducing the system requires stock-device inputs that are not distributed here.

## Project principles

Use standard Linux interfaces wherever they can own the function.

Retain vendor machinery where it is the practical hardware interface.

Remove Android policy when native Linux already has a better owner.

Keep board-specific code small, explicit, and replaceable.

The best compatibility layer is the one userspace above it does not need to know exists.

## Safety and expectations

This is a board-specific BSP and integration project, not a consumer installer.

Readers are expected to understand Linux, boot images, kernel modules, MediaTek device recovery, and partition handling.

Recovery-critical stock artifacts and proprietary inputs must be preserved with their provenance outside this repository.
