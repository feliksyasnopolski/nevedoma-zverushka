# TB330FU GPU

## Purpose

The TB330FU retains the MediaTek/Mali platform substrate required to power
and expose the GPU. Panfrost owns native Linux rendering. The handoff service
ensures the Panfrost render node exists before the graphical session starts.

## Architecture

```text
MediaTek/Mali platform support
    |
Panfrost kernel driver
    |
DRM render node
    |
Mesa
    |
KWin/Wayland
```

Display ownership is separate from rendering:

```text
MediaTek DRM/KMS -> display scanout
Panfrost/Mesa   -> GPU rendering
```

The release tree contains no custom GPU driver.

## Components

| Path | Role | Classification |
|---|---|---|
| `src/gpu/tb330fu-panfrost` | Panfrost handoff helper | hand-written source |
| `systemd/tb330fu-panfrost.service` | Runs the handoff once during boot | systemd unit |
| `systemd/display-manager.service.d/20-tb330fu-panfrost.conf` | Orders the display manager after the handoff | systemd drop-in |

Panfrost and `gpu_sched` are external kernel modules. Mesa and KWin are
distribution components.

## Kernel and BSP dependencies

The helper requires the platform device
`/sys/bus/platform/devices/13040000.mali`, the vendor module
`mtk_gpufreq_wrapper_legacy`, and these deployed module files:

```text
/usr/lib/tb330fu/native-modules/gpu_sched.ko
/usr/lib/tb330fu/native-modules/panfrost.ko
```

The vendor/platform layer retains the MediaTek GPU power and frequency
integration. The exact lower-layer module dependency graph is external to
this release tree.

## Panfrost handoff

`tb330fu-panfrost` is a root-owned shell helper. It fails if either module,
the Mali platform device, or `mtk_gpufreq_wrapper_legacy` is unavailable.
It loads `gpu_sched` when necessary. If another driver owns
`13040000.mali`, it sets the Panfrost driver override and unbinds that
platform device. It loads Panfrost, binds the device when needed, waits for
udev settling, and fails unless `/dev/dri/renderD128` exists.

The helper changes only the GPU platform-device binding. It does not bind or
replace the MediaTek display DRM device.

The service is `Type=oneshot` with `RemainAfterExit=yes`. It does not retry
or unload GPU modules. A failed helper leaves the service failed and prevents
the required display-manager dependency from being satisfied.

## DRM device topology

The current tablet exposes `/dev/dri/card0`, `/dev/dri/card1`, and
`/dev/dri/renderD128`. The observed roles are:

| Node | Current driver | Role |
|---|---|---|
| `/dev/dri/card0` | `mediatek-drm` | display scanout |
| `/dev/dri/card1` | `panfrost` | Panfrost DRM device |
| `/dev/dri/renderD128` | `panfrost` | render node |

DRM card numbers are runtime-assigned. Determine ownership from the driver
identity in `/sys/class/drm/<node>/device/driver` rather than card number
alone.

## Mesa and KWin integration

Mesa uses the Panfrost render node for native GPU rendering. KWin consumes
the native DRM/Wayland stack. The current EGL renderer identifies the
hardware path as `Mali-G52 r1 MC2 (Panfrost)`.

## Systemd integration

The display-manager drop-in requires and orders the graphical login stack
after `tb330fu-panfrost.service`:

```text
tb330fu-panfrost.service
    before
display-manager.service
    |
SDDM / KWin
```

The Panfrost service itself is ordered after local filesystems,
`tb330fu-devd.service`, and `tb330fu-android-boot-complete.service`. The
Android boot-complete unit is a property-readiness gate; the vendor GPU
substrate must also be present before the helper can succeed.

## Build and installation

The release tree does not build Panfrost, the kernel, or Mesa. It provides
the handoff helper and systemd integration required to use those external
components.

Install the helper as:

```text
/usr/libexec/tb330fu-panfrost
```

Install the unit as:

```text
/etc/systemd/system/tb330fu-panfrost.service
```

Install the drop-in as:

```text
/etc/systemd/system/display-manager.service.d/20-tb330fu-panfrost.conf
```

The `.ko` files are external deployed inputs and are not tracked here.

## Verification

Use driver identity and service state rather than fixed DRM card numbers:

```bash
systemctl is-active tb330fu-panfrost.service
systemctl cat display-manager.service
ls -l /dev/dri
for n in /sys/class/drm/card*/device/driver /sys/class/drm/renderD*/device/driver; do
    printf '%s -> ' "$n"
    readlink "$n"
done
```

The expected result is an active Panfrost service, a Panfrost render node,
the `Requires`/`After` drop-in, and separate `mediatek-drm` and `panfrost`
driver identities. On a running graphical session, `eglinfo -B` or an
equivalent Wayland/EGL query can verify the renderer. X11 tools are not
required.

## Failure modes

* The vendor GPU substrate or `mtk_gpufreq_wrapper_legacy` is unavailable.
* A required external module is absent or cannot load.
* Panfrost cannot bind `13040000.mali`.
* `/dev/dri/renderD128` is not created.
* The display-manager drop-in is absent or does not require Panfrost.
* Mesa selects software rendering instead of Panfrost.

## Known limitations

GPU power, clocks, regulators, power domains, and frequency policy remain
partly vendor/BSP-owned. DRM card numbering is not stable across boots. The
release tree does not package the kernel modules, firmware, Mesa, or vendor
GPU support binaries.
