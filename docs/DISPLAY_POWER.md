# TB330FU display and power

## Purpose

This document describes native DRM/KMS display ownership and Linux desktop policy for display power, suspend, wake, lid input, the power button, and brightness.

## Architecture

MediaTek DRM/KMS
        |
KWin / Wayland
        |
Plasma / PowerDevil
        |
DPMS, brightness, suspend, lid, power button

The Android-derived kernel and MediaTek BSP provide hardware support.
Native Linux owns the compositor, display policy, session policy, and systemd power path.
See [GPU.md](GPU.md), [INPUT.md](INPUT.md), and [SENSORS.md](SENSORS.md) for adjacent ownership boundaries.

## Display ownership

The stock `mediatek_drm` driver owns the physical DSI panel.
The current observed topology is:

| Object | Role |
|---|---|
| `/dev/dri/card0` | MediaTek display scanout (`mediatek-drm`) |
| `DSI-1` | Connected internal panel |
| `crtc-0` | Active panel CRTC |
| `/dev/dri/card1` | Panfrost DRM device |
| `/dev/dri/renderD128` | Panfrost render node |

The panel exposes `1200x1920`; the active CRTC geometry is `1200x1920` and DRM rotation is `0`.
Card numbers are runtime-assigned.
Panfrost is GPU rendering for the Mali-G52.
It does not own panel scanout or display policy.
KWin/Wayland owns DRM master in the desktop session.

Plymouth uses the same MediaTek DRM path before SDDM; SDDM terminates Plymouth before KWin starts.
SurfaceFlinger and Android hwcomposer are absent from the active desktop display path.
The vendor MediaTek PQ service may remain as a compatibility service, but it is not the compositor or DRM owner.
The Samsung DRM donor is archived and unsupported.

## Brightness and DPMS

KWin/PowerDevil owns desktop display policy.
No TB330FU brightness daemon is in this release tree.
The current target snapshot exposed no `/sys/class/backlight` entry, so the active kernel backlight path and slider behavior require a live graphical-session check.

DRM DPMS and physical backlight state are separate.
The display can be black after DPMS while the PWM/backlight remains lit.
This affects power use and closed-folio behavior.

The PowerDevil change sets logical brightness to zero, writes `0` to `/sys/class/leds/lcd-backlight/brightness` for hardware-off, then performs normal idle DPMS off and restores the previous logical value exactly once on input wake.
Brightness changes while off are retained as the new restore value, and wake without a saved idle-off value does not alter brightness.
It does not change kernel DRM, KWin, folio, or logind.

## Suspend

PowerDevil initiates the interactive policy action. logind exposes the system operation and `systemd-suspend.service` runs the suspend path.
The project preflight is an `ExecStartPre`; it selects the active Wayland session, asks KWin to turn DPMS off, verifies it, waits for `disp_crtc0_wakelock` to become inactive, and reads `/sys/power/wakeup_count`.
It never executes suspend.

PowerDevil / logind
        |
tb330fu-suspend-preflight
        |
KWin DPMS off -> MediaTek atomic display disable
        |
disp_crtc0_wakelock inactive
        |
systemd-sleep -> kernel suspend -> resume

The MediaTek path holds `disp_crtc0_wakelock` while the display CRTC remains active.
DPMS off changes KWin output state and the DRM atomic disable path releases display-side wake protection.
If DPMS off or release does not complete, the preflight exits nonzero and `systemd-sleep` is not reached.

Suspend with the preflight has been verified.
WLAN is a confirmed wake source.

## Wake sources

| Source | Status |
|---|---|
| Physical power button | Current hardware wake source |
| Pickup motion | Current hardware/vendor wake source |
| Folio open | Current path; physical transition needs operator confirmation |
| WLAN | Observed wake reason in the validated cycle |
| Double-tap | Not implemented/working |

USB can affect wake through the vendor/kernel `USB suspend lock`.
Suspend validation uses Wi-Fi/mDNS rather than USB networking.
This is verification guidance, not a USB policy change.

## Power button

`systemd/logind.conf.d/10-tb330fu-power-button.conf` sets `HandlePowerKey=ignore`, so logind does not own the final interactive action.
PowerDevil handles the KDE event and normal KDE UI/policy.
No custom power-button daemon exists.
The project PowerDevil configuration disables automatic idle suspend; press-duration behavior remains KDE policy.

## Folio and lid

`tb330fu-folio-lid.service` translates vendor `hall_irq` key pulses into virtual `SW_LID`.
Policy remains above the helper:

hall_irq -> tb330fu-folio-lid -> SW_LID -> PowerDevil -> display/power policy

The current policy goal is folio close -> display/backlight off, without requiring system suspend.
The tablet should remain reachable and charging while closed.
Open should restore display state.
Physical close/open behavior is not proven by service liveness.

## Automatic rotation

accelerometer -> IIO -> iio-sensor-proxy -> KWin/Plasma -> output transform

The sensor chain is documented in [SENSORS.md](SENSORS.md).
Current desktop integration reports all four tablet orientations with aligned display, touch, and stylus behavior.
No custom rotation helper exists.

## Project-owned files

| Path | Role |
|---|---|
| `systemd/systemd-suspend.service.d/10-tb330fu-preflight.conf` | Suspend preflight ordering |
| `scripts/tb330fu-suspend-preflight` | DPMS/wake-lock/wakeup-count gate |
| `systemd/logind.conf.d/10-tb330fu-power-button.conf` | Keep button policy with PowerDevil |
| `systemd/tb330fu-folio-lid.service` | Hall-to-`SW_LID` service |
| `systemd/tb330fu-panfrost.service` | Panfrost readiness |

The preflight does not modify the distro `systemd-suspend.service`.
User KScreen state and PowerDevil home-directory state are not promoted.

## Verification

for n in /sys/class/drm/card*/device/driver; do printf '%s -> ' "$n"; readlink -f "$n"; done cat /sys/class/drm/card0-DSI-1/status cat /sys/class/drm/card0-DSI-1/modes grep -E 'disp_crtc0_wakelock|USB suspend lock' /sys/kernel/debug/wakeup_sources systemctl is-active tb330fu-folio-lid.service systemctl --user is-active plasma-powerdevil.service

These checks do not prove physical pixels, brightness, or folio transitions.
Suspend verification must use the preflight and Wi-Fi/mDNS access.

## Known limitations

The release contains no kernel DRM change, donor DRM stack, custom brightness daemon, double-tap wake, or automated folio-transition test.
Physical backlight state is represented by the PowerDevil brightness backend; direct operator observation remains outside automated validation.