# TB330FU architecture

## Platform

The target is the Lenovo Tab M11 TB330FU.
The tablet is an aarch64 system using an Android-derived 6.6.82 kernel and the MediaTek BSP.
Stock artifacts identify the platform through MediaTek MT6768-family names; boot metadata also contains an MT8786 SKU label.
This document uses the established MT6768-family BSP description without treating either label as a complete SoC identity.

The deployed native root is ext4 on `/dev/mmcblk0p52`.
The root filesystem contains Ubuntu userspace and systemd.
The kernel and selected vendor components remain Android-derived because they provide hardware support that has no complete native replacement.

## Boot sequence

The current high-level path is:

```text
bootloader
    |
Android-derived kernel + custom vendor_boot
    |
stage1
    |
internal Ubuntu root
    |
switch_root
    |
systemd PID 1
```

The native root is reached by the project stage1 and switch-root tooling.
The current path does not use a conventional Ubuntu initramfs.
The custom vendor ramdisk supplies the early module and storage substrate required to locate the native root.
Stage1 is a small BusyBox PID1 path that loads the required early modules, locates the root filesystem, and performs `switch_root`.

## Runtime layers

```text
Bootloader
    |
Android-derived kernel + MediaTek BSP
    |
TB330FU vendor compatibility substrate
    |
native Linux interfaces and services
    |
systemd + Ubuntu
    |
KWin/Plasma + applications
```

Ubuntu and systemd own normal userspace, service management, desktop and session policy, DRM/KMS userspace, udev, BlueZ, KDE Plasma/KWin, and ordinary Linux policy where practical.
Android-derived services and libraries are intended to remain below the native Linux policy layer.
The current vendor graph still contains several generic policy actions that have not yet been migrated to native owners.

The compatibility substrate currently includes the Android property area and property service, the deployed connectivity graph, selected vendor services, stock kernel modules, camera and sensor bridges, Bluetooth support, GNSS support, and vendor power/audio components.
A component's Android origin does not make Android the operating system in charge of the desktop.
Audio above the kernel/BSP is native ALSA, UCM, PipeWire, and WirePlumber; Android AudioFlinger and an Android Audio HAL are not in that path.
See [AUDIO.md](AUDIO.md).

## Device node lifecycle

The stock kernel is built without `devtmpfs`.
`tb330fu-devd` therefore owns device-node lifecycle below udev: it performs coldboot reconciliation, subscribes to kernel uevents, and creates or removes device nodes using policy generated from stock ueventd configuration.
`tb330fu-devd.service` is active before the connectivity graph and other consumers.

`systemd-udevd` remains the normal Linux policy consumer.
It applies udev rules and hardware database entries, including the Lenovo stylus policy.
The two layers have separate roles: devd makes the nodes exist; udev applies Linux device policy.

## Display and GPU

The stock MediaTek DRM/KMS driver owns the physical display.
The live system has `mediatek_drm` on one DRM card and Panfrost with `gpu_sched` on the render stack; `/dev/dri` exposes `card0`, `card1`, and `renderD128`.
Panfrost provides Mali-G52 rendering.
KWin uses native Wayland/DRM userspace with Plasma.

SurfaceFlinger is not the desktop display stack.
The Samsung MT6768 DRM donor is archived experiment material and is not part of the current display ownership path.

## Input

Touchscreen and stylus input use the normal Linux input and udev path.
The stylus is exposed as `lenovo-stylus` and is covered by the deployed stylus udev/hwdb policy.

The physical `hall_irq` input is translated by `tb330fu-folio-lid.service` into a virtual uinput device named `TB330FU Folio Lid` carrying standard `SW_LID` state.
This gives desktop policy a Linux lid-switch interface without moving folio policy into the vendor layer.
Physical close/open transition behavior is operator-dependent and is not established by service liveness alone.

## Power

systemd and PowerDevil own user-facing power policy.
Stock vendor power modules and services remain below that layer for platform support.
Suspend is functional.
Display DPMS and physical backlight control are distinct on this platform and must not be treated as one interface.

`tb330fu-zram.service` is the sole native owner of `/dev/zram0` setup and swap activation.
It runs after `tb330fu-devd.service`, which owns block-node lifecycle on this no-devtmpfs kernel.
See [ZRAM.md](ZRAM.md).

## Connectivity

`tb330fu-connectivity-graph.service` runs `/usr/local/sbin/connectivity-fsm` against the deployed connectivity graph for the Android lifecycle events.
The graph contains Android/vendor bring-up, property sequencing, USB/network setup, and generic Linux-style policy.
Its name is historical and is not an architectural boundary.

Native finalization is provided separately by `tb330fu-connectivity-finalize.service`; its current deployed behavior is narrow.
Generic Linux policy is intended to move to native owners where the producer, consumer, ABI/VINTF contract, and runtime call path justify that change.
The graph remains the current execution path for the actions that still require it.

The native USB management path is separate from the retained graph.
`tb330fu-usb-gadget.service` creates the `tb330fu_ncm` configfs gadget, binds its NCM function to the discovered UDC, and configures `usb0` at `192.168.7.2/24`.
NetworkManager leaves `usb0` unmanaged.
See [USB.md](USB.md).

## Bluetooth

The retained MediaTek/WMT path reaches the stock Bluetooth AIDL HAL through `bt_drv_connac1x.ko` and `/dev/stpbt`.
The promoted `tb330fu-android-bt-aidl-bridge` translates that AIDL HCI endpoint to the Unix-socket-backed `tb330fu_hci_shim.ko`, which exposes Linux `hci0`.
BlueZ owns host-side Bluetooth policy.
See [BLUETOOTH.md](BLUETOOTH.md).

## GNSS

The deployed system includes the MediaTek GNSS AIDL service and `mnld`.
These provide the Android/vendor GNSS compatibility path.
A current hardware fix, NMEA consumer, and native Linux GNSS policy owner were not established by the available live checks.
GNSS functional status is therefore unverified here.

## Camera

The Android/vendor camera path remains below the native interface.
The production camera daemon is native Linux-facing bridge code and is ordered after the camera module service.
The deployed BTF-enabled `v4l2loopback` module exposes `/dev/video10` and `/dev/video11`, with stable `/dev/video-rear` and `/dev/video-front` symlinks.

Preview and video are exposed through V4L2.
The daemon has local protobuf v1 Unix `SOCK_SEQPACKET` control and events
sockets; it is not an HTTP server. The daemon remains authoritative for camera
selection, mode, controls, Camera3 observation, and capture lifecycle.
Still capture is not implemented.
Device-node and daemon liveness do not by themselves prove correct pixels, format negotiation, or product-complete camera behavior.

## Sensors

`tb330fu_iio.ko` and the sensor IIO producer normalize the Android/vendor sensor path into Linux IIO interfaces.
`iio-sensor-proxy` consumes the IIO side for desktop-facing sensor behavior.
The Android sensor service remains part of the hardware-facing compatibility path.

## Ownership

| Subsystem | Kernel/BSP owner | Compatibility layer | Native Linux owner |
|---|---|---|---|
| Display | `mediatek_drm`, panel and MediaTek display modules | vendor display support | DRM/KMS userspace, KWin |
| GPU | Mali platform/power/gpufreq support | vendor GPU dependencies | Panfrost kernel driver, Mesa, KWin |
| Device nodes | kernel uevents; no `devtmpfs` | `tb330fu-devd` | udev policy |
| Input | touchscreen, stylus, `hall_irq` drivers | folio lid uinput proxy | libinput, logind, desktop policy |
| Power policy | vendor power modules | vendor power services | systemd, PowerDevil |
| Wi-Fi | MediaTek Wi-Fi/BSP support | connectivity graph and vendor support | mixed; native finalizer is active |
| USB NCM management | MediaTek USB/`musb-hdrc` UDC and kernel NCM support | retained Android USB actions for Android modes | `tb330fu-usb-gadget.service`, configfs, `usb0` |
| Bluetooth | stock Bluetooth/WMT modules | HCI shim and Android AIDL bridge | BlueZ/`bluetoothd` |
| GNSS | MediaTek GNSS/BSP support | GNSS AIDL service and `mnld` | unknown |
| Camera | MediaTek camera/ISP drivers | Android camera HAL path and bridge | native camera daemon/V4L2 |
| Sensors | sensor drivers and `tb330fu_iio.ko` | Android sensor service and IIO producer | IIO and `iio-sensor-proxy` |

## Recovery and provenance boundary

This release repository contains source, build recipes, metadata, and extraction instructions where redistribution is legally and technically appropriate.
It intentionally excludes stock boot/vendor images, vendor partitions, proprietary firmware, NVRAM and calibration data, security/TEE material, APKs and vendor shared libraries, neural models, and reconstructed recovery corpora whose redistribution is inappropriate.
Those artifacts remain in the separate source/evidence workspace and controlled build or recovery storage.

Stock-derived artifacts must retain provenance and exact identity outside this tree.
Generated files must identify their generator and canonical source; the release tree must not become an unclassified Android harvest.

## Known limitations

The current ownership boundary for the connectivity graph, Wi-Fi, Bluetooth, and GNSS is mixed or partially unverified.
The loaded systemd dependency graph also contains inactive or `not-found` compatibility references.
These facts are recorded as follow-up architecture work.
They do not authorize changing the deployed graph or tablet state during repository initialization.
