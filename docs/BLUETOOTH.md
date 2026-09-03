# Bluetooth

## Purpose

This subsystem exposes the retained MediaTek/Android Bluetooth hardware path
to standard Linux HCI and BlueZ userspace. The Android/vendor components stay
below the Linux policy layer.

## Architecture

```text
MediaTek WMT and Bluetooth vendor path
    |
bt_drv_connac1x.ko -> /dev/stpbt
    |
android.hardware.bluetooth-service-mediatek
    |
android.hardware.bluetooth.IBluetoothHci/default
    |
tb330fu-android-bt-aidl-bridge
    |  AF_UNIX SOCK_SEQPACKET /tmp/tb330fu-hci.sock
    |
tb330fu_hci_shim.ko -> Linux hci0 (HCI_VIRTUAL)
    |
BlueZ / bluetoothd -> Bluedevil / KDE
```

The bridge and shim preserve HCI packet types and payloads. The shim contains
no Bluetooth protocol logic.

## Components

| Path | Role | Status |
|---|---|---|
| `src/bluetooth/hci-shim/tb330fu_hci_shim.c` | Kernel HCI transport shim | source |
| `src/bluetooth/aidl-bridge/tb330fu_android_bt_aidl_bridge.cpp` | NDK AIDL to Unix-socket bridge | source |
| `src/bluetooth/aidl-bridge/android/hardware/bluetooth/*.aidl` | Android 15 Bluetooth HCI contracts | external-interface source |
| `src/bluetooth/tb330fu-bluetooth-modules` | Idempotent `rfkill.ko` then `bluetooth.ko` loader | helper source |
| `src/bluetooth/tb330fu-bluetooth-ready` | Bridge PID/socket readiness gate | helper source |
| `src/bluetooth/tb330fu-bluetooth-hci` | Shim loader and `hci0` gate | helper source |
| `systemd/tb330fu-bluetooth-modules.service` | Bluetooth core module unit | unit |
| `systemd/tb330fu-bt-bridge.service` | Resident AIDL bridge unit | unit |
| `systemd/tb330fu-bt-hci.service` | One-shot shim unit | unit |
| `systemd/tb330fu-bluetooth.target` | Manual integration target | unit |

The stock `bt_drv_connac1x.ko`, vendor HAL binary, firmware, target Binder
libraries, and matching kernel build metadata are external deployment inputs.
They are not copied into this repository.

## Vendor-side prerequisites

The retained connectivity graph loads the shared MediaTek WMT/connsys
dependencies and publishes the readiness properties used by the vendor path.
The Bluetooth-specific kernel leaf is `bt_drv_connac1x.ko`; the selected
transport is MediaTek STP through `/dev/stpbt`, not UART HCI. The stock vendor
HAL is `/vendor/bin/hw/android.hardware.bluetooth-service-mediatek` and
publishes `android.hardware.bluetooth.IBluetoothHci/default` through the
Android service-manager substrate.

The graph remains the owner of WMT setup, shared module ordering, property
publication, and vendor-service startup. This promotion does not remove or
duplicate those actions. See [VENDOR_RUNTIME.md](VENDOR_RUNTIME.md).

## HCI bridge

`tb330fu-android-bt-aidl-bridge` uses the NDK AIDL interface
`IBluetoothHci`. It starts a Binder callback thread pool, calls `initialize`,
and waits for `initializationComplete(SUCCESS)` before creating and listening
on `/tmp/tb330fu-hci.sock`. It then forwards Linux-to-vendor command, ACL,
SCO, and ISO packets and forwards vendor events/data in the reverse direction.

The kernel shim creates an `HCI_VIRTUAL` device with `hci_alloc_dev()` and
publishes it only after connecting to the listening Unix socket. It registers
as `hci0` in the normal current-device case. HCI Reset can be sent during
registration, so transport connection precedes `hci_register_dev()`.

The bridge is a resident target-bionic/AArch64 process. It runs through the
external Android runtime linker with the vendor Binder libraries and writes
focused diagnostics to the journal and
`/var/log/tb330fu-android-bt-aidl-bridge.log`. It has no reconnect loop. A
socket disconnect ends the bridge loop and calls the vendor `close()` path.
The unit does not set `User=`, so systemd starts the bridge as root with no
additional unit-declared capabilities or child processes. The vendor HAL is a
separate service with its own retained numeric identity and capability setup.

## BlueZ integration

BlueZ is unmodified. The package does not replace `bluetooth.service`, change
`bluetoothd` command-line options, or add BlueZ configuration. Once `hci0`
exists, BlueZ consumes it through the standard Linux Bluetooth interfaces.
The deployed `bluetooth.service` runs the stock
`/usr/libexec/bluetooth/bluetoothd`. Pairing and adapter storage remain BlueZ
responsibilities. No project-specific Bluedevil integration is present.

## rfkill

The Bluetooth module helper normalizes `/dev/rfkill` after loading
`rfkill.ko`, before loading `bluetooth.ko`. It requires the character node and
sets it to mode `0666`. The HCI helper repeats the mode change after `hci0`
appears. This deliberate single-user tablet policy allows Bluedevil and
`rfkill` clients to open the node. 
`systemd-rfkill.service` and `systemd-rfkill.socket` are separate stock static
units, not the TB330FU normalizer.

## Startup and dependencies

The public target is manual and is not enabled at boot:

```text
tb330fu-bluetooth.target
    |
    +-- tb330fu-bluetooth-modules.service
    |       rfkill.ko -> bluetooth.ko
    +-- tb330fu-bt-bridge.service
    |       Android boot-complete gate -> Binder initialize -> socket
    +-- tb330fu-bt-hci.service
            modules + bridge -> readiness check -> shim -> hci0
```

The three child units require the `tb330fu-android-boot-complete.service`
readiness boundary. The HCI unit additionally requires the module and bridge
units. The module loader uses `/system/lib/modules/rfkill.ko` and
`/system/lib/modules/bluetooth.ko`; the shim helper uses the externally
installed `/usr/lib/tb330fu/bluetooth/tb330fu_hci_shim.ko`. The bridge binary
is installed at `/usr/local/sbin/tb330fu-android-bt-aidl-bridge`.

`tb330fu-android-boot-complete.service` depends on the graph-produced
`sys.boot_completed=1` property. The graph's Rediska and `tb330fu-devd`
prerequisites are therefore indirect dependencies of this Bluetooth path.
See [VENDOR_RUNTIME.md](VENDOR_RUNTIME.md) and [PROPERTIES.md](PROPERTIES.md).

## Runtime and failure behavior

The bridge uses `Restart=no`. Missing Binder service, failed AIDL
initialization, missing callback success, socket bind failure, or vendor
`close()` failure is logged and causes the bridge to exit. The readiness helper
times out after 30 seconds when the bridge is not active with a listening
socket. The HCI helper fails if the shim cannot load or if `hci0` does not
appear within ten seconds. The target consequently fails without creating a
usable controller.

Stopping the target unloads only `tb330fu_hci_shim`; it does not unload the
stock `bluetooth` or `rfkill` modules. The bridge closes the AIDL HAL and
removes the Unix socket. Recovery is manual: restore the graph/vendor
prerequisites and start the target again. No automatic reconnect or restart
behavior is claimed.

## Build

Build the kernel shim against the exact target kernel build tree and matching
module metadata:

```bash
make -C src/bluetooth/hci-shim \
  KDIR=/path/to/target-kernel-build ARCH=arm64 \
  CROSS_COMPILE=aarch64-linux-gnu-
```

Build the bridge with Android 15 r17-compatible AIDL tooling, target bionic
Binder NDK headers, an AArch64 Android NDK compiler, and the target
`libbinder_ndk.so`:

```bash
NDK=/path/to/android-ndk \
AIDL=/path/to/build-tools/aidl \
AOSP_BINDER_INC=/path/to/aosp/native/libs/binder/ndk/include_cpp \
AOSP_BINDER_PLATFORM_INC=/path/to/aosp/native/libs/binder/ndk/include_platform \
BINDER_NDK=/path/to/target/libbinder_ndk.so \
src/bluetooth/aidl-bridge/build.sh
```

The build script does not deploy or start the bridge. The vendor binary,
firmware, Binder library, AIDL tool, AOSP headers, and kernel tree are
external dependencies.

## Verification

The target-side verification is read-only:

```bash
systemctl is-active tb330fu-bluetooth-modules.service tb330fu-bt-bridge.service tb330fu-bt-hci.service
systemctl status tb330fu-bluetooth.target tb330fu-bt-bridge.service tb330fu-bt-hci.service
journalctl -u tb330fu-bt-bridge.service -u tb330fu-bt-hci.service
test -d /sys/class/bluetooth/hci0
bluetoothctl show
rfkill
ss -xlH | grep /tmp/tb330fu-hci.sock
```

Expected evidence is active units, a listening bridge socket, `/sys/class/
bluetooth/hci0`, a controller reported by `bluetoothctl`, and accessible
rfkill state after the helper has run. `hciconfig` is optional and is not
required when `bluetoothctl` or `btmgmt` is available.

## Known limitations

The Bluetooth target is manual and remains disabled at boot. The working stack has been verified with the AIDL bridge, HCI shim, `hci0`, stock `bluetoothd`, and BlueZ controller enumeration. The currently deployed runtime may still expose `/dev/rfkill` as 0600 until the normalized helper is deployed and the relevant unit restarted.
