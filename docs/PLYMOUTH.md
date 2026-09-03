# TB330FU Plymouth startup

## Purpose

LK framebuffer contents survive into native Linux startup.
The `tb330fu_lk_fb_clear` module removes the stale contents once.
Plymouth then presents the native DRM boot splash until SDDM starts.

## Architecture

```text
systemd
    |
tb330fu-devd
    |
tb330fu_lk_fb_clear
    |
Plymouth on DRM
    |
SDDM
    |
KWin/Plasma
```

The fb-clear module owns only the one-time framebuffer cleanup.
It does not own DRM/KMS, scanout, backlight, Plymouth policy, SDDM, or KWin.

Plymouth owns the graphical boot splash after systemd starts and before SDDM takes over.
It does not own stage1, `vendor_boot`, charger mode, the desktop session, or runtime display policy.

The earlier boot handoff is described in [BOOT.md](BOOT.md).
Display-driver and rendering ownership are described in [GPU.md](GPU.md).

## Components

| Path | Role | Classification |
|---|---|---|
| `modules/tb330fu_lk_fb_clear/tb330fu_lk_fb_clear.c` | One-shot LK framebuffer clear | source |
| `modules/tb330fu_lk_fb_clear/Makefile` | External kernel-module build entry point | build source |
| `systemd/tb330fu-lk-fb-clear.service` | Loads the clear module once | systemd unit |
| `systemd/plymouth-start.service.d/20-tb330fu.conf` | Adds TB330FU startup ordering and invocation | systemd drop-in |
| `systemd/sddm.service.d/20-tb330fu-plymouth.conf` | Terminates Plymouth before SDDM's graphical login | systemd drop-in |

Plymouth, SDDM, KWin, the MediaTek DRM driver, and the Plymouth theme are distribution components.

## Framebuffer cleanup

`tb330fu_lk_fb_clear` maps physical range `0x7bce0000`–`0x7c58a000` with `memremap(MEMREMAP_WB)`, clears `0x008ca000` bytes, publishes the write with `wmb()`, and unmaps the range before module initialization returns.
It has no persistent mapping or control surface.

`tb330fu-lk-fb-clear.service` runs after `local-fs.target` and before Plymouth and the display manager.
It skips insertion when the module is already loaded.
A mapping failure fails the service; the module is not silently treated as optional.

## Plymouth startup

The distribution `plymouth-start.service` is enabled by `graphical.target`.
The TB330FU drop-in orders it after `tb330fu-devd.service` and `tb330fu-lk-fb-clear.service`, and before `display-manager.service`.

The supported daemon invocation is:

```text
/usr/sbin/plymouthd --mode=boot --pid-file=/run/plymouth/pid --graphical-boot --ignore-serial-consoles
```

`--graphical-boot` selects the graphical boot path.
`--ignore-serial-consoles` prevents serial-console detection from selecting a non-graphical startup path.
The service also runs `plymouth show-splash` after daemon startup.

Plymouth uses the existing native MediaTek DRM/KMS display path.
Plymouth does not own or replace the MediaTek DRM driver.
The current tablet exposes that path as `/dev/dri/card0`; DRM card numbering is runtime-assigned.

## SDDM handoff

```text
Plymouth owns boot splash
    |
SDDM starts
    |
plymouth quit
    |
SDDM/KWin take display ownership
```

The TB330FU SDDM drop-in adds:

```text
ExecStartPre=-/usr/bin/plymouth quit
```

The leading `-` makes an unavailable or already-terminated Plymouth exit non-fatal to SDDM startup.
The fb-clear module does not terminate Plymouth.

`plymouth-quit.service` and `plymouth-quit-wait.service` are masked with `/dev/null`.
The SDDM handoff is therefore explicit and remains attached to the display-manager startup path.

## Systemd integration

Install the clear unit and enable it for `graphical.target`.
Install the two drop-ins under their exact unit directories.
Keep both Plymouth quit masks:

```text
/etc/systemd/system/plymouth-quit.service -> /dev/null
/etc/systemd/system/plymouth-quit-wait.service -> /dev/null
```

The current deployed Plymouth and SDDM units contain these TB330FU changes directly in their distro unit files.
The release tree expresses the same changes as minimal drop-ins so distro unit contents are not copied here.

## Build

Only the fb-clear module is built by this project.
Plymouth and SDDM are distribution packages.

Build against the matching Android-derived kernel metadata:

```bash
make -C /path/to/kernel M=/path/to/tb330fu_lk_fb_clear ARCH=arm64 \
  LLVM=/path/to/pinned/android-clang LLVM_IAS=1 \
  PAHOLE=/path/to/zstd-capable/pahole modules
```

The kernel configuration must retain `CONFIG_DEBUG_INFO_BTF_MODULES`.
The kernel tree, reconstructed stock kernel BTF, matching `Module.symvers`/CRC provenance, and pinned Android clang are external build inputs.
Do not use a host-built or BTF-less `.ko`.
Built modules are not tracked here.

## Installation

Install the module at:

```text
/usr/lib/tb330fu/native-modules/tb330fu_lk_fb_clear.ko
```

Install `tb330fu-lk-fb-clear.service` under `/etc/systemd/system/`.
Install the Plymouth and SDDM drop-ins under:

```text
/etc/systemd/system/plymouth-start.service.d/20-tb330fu.conf
/etc/systemd/system/sddm.service.d/20-tb330fu-plymouth.conf
```

Create the two `/dev/null` mask symlinks shown above, then run `systemctl daemon-reload` and enable the clear unit.

## Verification

Verification can be performed without restarting Plymouth or SDDM:

```bash
lsmod | grep tb330fu_lk_fb_clear
systemctl is-active tb330fu-lk-fb-clear.service plymouth-start.service display-manager.service
systemctl cat tb330fu-lk-fb-clear.service plymouth-start.service display-manager.service
systemctl is-enabled plymouth-quit.service plymouth-quit-wait.service
ps -eo pid,ppid,user,args | grep '[p]lymouthd\|[s]ddm'
for n in /sys/class/drm/card*/device/driver; do printf '%s -> ' "$n"; readlink "$n"; done
```

Expected state is a loaded clear module, an active or successfully exited clear/Plymouth startup path, the SDDM handoff, masked quit units, and a MediaTek DRM device.
A command-line check does not prove the physical splash or handoff pixels.

## Failure modes

* The clear service fails when the external module is missing, incompatible, or cannot map the confirmed range.
* Stale LK contents remain visible if the clear service is not ordered before Plymouth.
* Plymouth can fail to start if DRM device nodes are not available after `tb330fu-devd`.
* A Plymouth invocation without graphical mode or serial-console suppression can select the wrong startup path.
* SDDM can fail to acquire display ownership if it starts without terminating Plymouth.

## Known limitations

The physical range and behavior are TB330FU/LK-specific.
The module is not a general framebuffer or DRM cleanup facility.
The deployed Plymouth theme is the stock distribution `breeze` theme; no TB330FU-specific theme or assets are included.
Physical panel behavior is not established by SSH or systemd state alone.