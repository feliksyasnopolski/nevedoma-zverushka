# TB330FU input

## Purpose

The touchscreen and stylus use the normal Linux input and udev path.
The TB330FU-specific input helper is required only for the vendor Hall sensor: the kernel exposes `hall_irq` as key pulses, while desktop policy expects a standard lid switch.
`tb330fu-folio-lid` performs that translation.

## Architecture

```text
hall_irq
    |
tb330fu-folio-lid
    |
uinput
    |
TB330FU Folio Lid
    |
SW_LID
    |
logind / PowerDevil
```

The kernel/BSP owns the touchscreen, stylus, and Hall input devices.
The folio helper is a native Linux input normalizer.
`systemd-udevd` identifies devices and applies input policy. libinput, logind, and PowerDevil consume standard Linux input interfaces.
The helper does not suspend the system, blank the display, control the backlight, or call desktop policy services.

## Components

| Path | Role | Classification |
|---|---|---|
| `src/input/tb330fu-folio-lid.c` | Hall-to-lid proxy | hand-written source |
| `src/input/Makefile` | helper build | hand-written build file |
| `systemd/tb330fu-folio-lid.service` | service integration | deployed unit copy |
| `udev/61-lenovo-m11-stylus.hwdb` | stylus axis policy | TB330FU-specific hwdb data |
| `udev/99-lenovo-stylus.rules` | stylus identification | TB330FU-specific udev rule |

## Touchscreen

The Himax touchscreen is exposed as a normal Linux input device.
It requires no TB330FU-specific release helper or rule in this subsystem.
`tb330fu-devd` creates device nodes during coldboot and `systemd-udevd` applies normal device policy; see [DEVD.md](DEVD.md).

## Stylus

The kernel exposes the stylus with the stable device name `lenovo-stylus`.
The release tree includes the deployed TB330FU-specific policy:

- `61-lenovo-m11-stylus.hwdb` sets the two absolute-axis resolution values used by the stylus.
- `99-lenovo-stylus.rules` sets `ID_INPUT=1` and `ID_INPUT_TABLET=1` for input event devices whose parent name is `lenovo-stylus`.

The rule matches the device name, not a dynamic `/dev/input/eventN` number.
During coldboot, devd may synthesize an `add` event for a newly materialized stylus node; udev then applies this rule and hwdb data.
The policy files are copied from the deployed native-root configuration preserved in the old workspace.
They are source data in this release tree, not generated files.

## Folio lid

The physical source is the evdev device whose name is exactly `hall_irq`.
Its established press semantics are:

| Source event | Virtual result |
|---|---|
| `EV_KEY` code `750`, value `1` | `EV_SW` `SW_LID=1` (closed) |
| `EV_KEY` code `751`, value `1` | `EV_SW` `SW_LID=0` (open) |
| release event | ignored |

The helper discovers the source by `EVIOCGNAME`; its event number is not stable.
It creates a uinput device named `TB330FU Folio Lid` with `EV_SW/SW_LID` capability, starts at open (`SW_LID=0`), suppresses duplicate states, and rediscovers the source after source loss.
Because the physical source reports edges rather than a current state, an already-closed folio is reported as open until its next transition.

Lid policy is outside the helper:

```text
folio helper -> reports lid state
logind/PowerDevil -> decides what lid state means
```

## Systemd integration

`tb330fu-folio-lid.service` is a long-running `Type=simple` service.
It is ordered after `local-fs.target`, `systemd-udevd.service`, and `tb330fu-devd.service`, and before `graphical.target`.
It checks that `/dev/uinput` is a character device, starts the deployed helper, and retries after failure.
It wants `tb330fu-devd.service` so input nodes are reconciled before the proxy searches for `hall_irq`.

## udev/hwdb policy

The files under `udev/` are the two deployed, TB330FU-specific stylus policy files.
They do not replace generic distribution input policy and contain no fixed event numbers.
The hwdb file must be loaded by the target's normal udev/hwdb mechanism; the rule is consumed by `systemd-udevd`.

## Build

Build on Linux with a compiler providing Linux input and uinput headers:

```bash
cd src/input
make
```

The output is `src/input/tb330fu-folio-lid`.
It is a local build artifact and is not a release payload.

## Installation

The deployed paths are:

```text
/usr/local/sbin/tb330fu-folio-lid
/etc/systemd/system/tb330fu-folio-lid.service
```

This repository does not provide a deployment framework.
Installation, udev/hwdb reload, and service activation are operational actions outside the source build.

## Verification

Discover input devices by name:

```bash
grep -H . /sys/class/input/event*/device/name
```

Verify the service and virtual-device properties:

```bash
systemctl status tb330fu-folio-lid.service
udevadm info --query=property --name="$(for p in /sys/class/input/event*/device/name; do [ "$(cat "$p")" = 'TB330FU Folio Lid' ] && basename "$(dirname "$(dirname "$p")")"; done | head -n1)"
```

The virtual device must be named `TB330FU Folio Lid` and expose `ID_INPUT_SWITCH=1`.
The stylus properties should include `ID_INPUT=1`, `ID_INPUT_TABLET=1`, `EVDEV_ABS_00=::50`, and `EVDEV_ABS_01=::50`.
`libinput list-devices` can provide an additional consumer-side check.

An operator may optionally close and open the physical folio to verify the two transitions.
Service liveness and device presence do not prove that physical transition behavior.

## Failure modes

- If `hall_irq` is absent, the helper keeps the virtual device and retries source discovery by name.
- If `/dev/input` is not ready, the service starts only after its declared dependencies and the helper continues retrying discovery.
- If `/dev/uinput` is unavailable, `ExecStartPre` prevents a misleading helper start.
- If a stylus rule is changed to match an event number, it will fail when input enumeration changes.
- If the desktop sees no `SW_LID`, check the virtual device's udev properties and the service journal.

## Known limitations

The folio mapping is board-specific because the vendor source uses key codes rather than a kernel `SW_LID` switch.
Startup cannot recover a pre-existing closed state until the next Hall transition.
Physical close/open behavior requires operator confirmation.