# `tb330fu-devd`

## Purpose

The stock kernel is built without `devtmpfs`. `tb330fu-devd` provides
device-node lifecycle in userspace.

## Architecture

```
kernel device model
    |
kobject uevent netlink
    |
tb330fu-devd
    |
/dev node lifecycle
    |
systemd-udevd
    |
Linux device policy and consumers
```

`tb330fu-devd` creates and removes char/block nodes. `systemd-udevd`
remains the normal Linux policy consumer and processes device events, rules,
and the hardware database. The daemon does not implement persistent udev
database state, Android init, mounts, modules, or arbitrary static `/dev`
rules.

## Components

| Path | Role | Classification |
|---|---|---|
| `src/devd/tb330fu-devd.c` | daemon | hand-written source |
| `src/devd/gen_ueventd_policy.py` | policy compiler | hand-written generator source |
| `src/devd/generated_ueventd_policy.[ch]` | compiled policy data | generated, tracked output |
| `src/devd/Makefile` | build and explicit regeneration targets | hand-written build file |
| `systemd/tb330fu-devd.service` | systemd unit | release copy of deployed unit |

## Startup and dependencies

`tb330fu-devd.service` is a long-running `Type=notify` service. It requires
`/dev`, `/proc`, and `/sys` mounts, starts after `local-fs.target`, and
is ordered before `multi-user.target`. It sends `READY=1` only after char
and block coldboot enumeration completes. Consumers such as the connectivity
graph, Panfrost service, zram service, and folio-lid service require or order
after this unit in their own units.

The initramfs/stage1 path remains responsible for bootstrap and external-SD
nodes needed before `switch_root`. devd reconciles the moved `/dev` after
the native root has taken over.

## Coldboot

Kernel devices that existed before the daemon subscribed to netlink have no
pending add event for udev. devd enumerates
`/sys/dev/char/<major>:<minor>` and `/sys/dev/block/<major>:<minor>`,
reads each entry's `uevent` metadata, and creates or adopts the
corresponding node below `/dev`.

For a node newly materialized by devd, it writes `add` to that exact sysfs
`uevent` file. The kernel then multicasts the synthetic add event to devd
and `systemd-udevd`, allowing udev to process the lifecycle. Existing
correct nodes are adopted and do not receive this synthetic add solely because
they were observed during reconciliation. Enumeration is best effort per
device; an unusable entry does not stop unrelated entries.

## Live device lifecycle

The daemon subscribes to `NETLINK_KOBJECT_UEVENT` and parses kernel
`add`, `change`, and `remove` events.

- `add`: validate the event, create or adopt the node, apply device and
  sysfs policy, and create a partition alias when applicable.
- `change`: preserve node topology and reapply matching metadata. If the
  reported `dev_t` changes unexpectedly, the existing node is left in place.
- `remove`: process the kernel-supplied removal lifecycle and remove only
  paths owned by devd. devd does not synthesize a live remove event.

## Ownership

A node created by devd, including one replacing a wrong device node, is owned
by devd. A correct node already present during coldboot is adopted but is not
owned. Ownership is held in an in-memory table and is not persisted.

devd never deletes an adopted node merely because it later observes its
device's removal. It also refuses to replace unrelated non-device paths. An
alias is removed only if devd created it and it still points to the recorded
target; conflicting, replaced, and externally managed paths are preserved.

## Policy

`generated_ueventd_policy.c` contains transformed device permissions, sysfs
permissions, subsystem directories, firmware directories, and the netlink
receive-buffer setting. It is generated from the available stock-derived
`ueventd` extraction, not read at runtime. The generated data records source
paths and line numbers for traceability.

The generated policy is derived from stock `ueventd` configuration. The
corresponding plain-text vendor configuration may be included in the release
tree with provenance metadata when required for reproducible generation.
Binary firmware, vendor libraries, calibration data, and other opaque stock
payloads remain outside the repository.

## Partition aliases

For block events with a safe `PARTNAME`, devd creates
`/dev/block/by-name/<PARTNAME>` pointing to the validated
`/dev/<DEVNAME>`. Partition names are limited to ASCII letters, digits, `_`,
`-`, and `.`. Existing non-symlinks and conflicting symlinks are not
replaced. Removal is ownership- and target-checked.

## Firmware loading

For an `add` event carrying `FIRMWARE`, devd validates the name, opens the
kernel device's `loading` and `data` sysfs attributes, and searches these
generated directories in order:

```
/etc/firmware/
/odm/firmware/
/vendor/firmware/
/firmware/image/
```

It writes `1`, streams the first readable matching firmware file, then writes
`0` on success or `-1` on failure. Absolute paths, empty components,
`.`/`..` components, and control characters are rejected.

## Build

From `src/devd`:

```bash
make
```

This builds `tb330fu-devd` from the tracked generated policy. A Linux build
must use a compiler and libc exposing netlink, device-node, and `*at()` APIs.
The source retains host-side inspection conditionals for macOS, but a macOS
build is not deployment validation.

To regenerate policy, provide external inputs explicitly:

```bash
make policy SYSTEM_UEVENTD=/path/to/ueventd.rc POLICY_TSV=/path/to/ueventd-policy.tsv
```

The generator validates the supported grammar, Android AID mappings, policy
counts, and deterministic output. Review the resulting diff before tracking it.

## Installation

Canonical target paths are:

```
/usr/local/sbin/tb330fu-devd
/etc/systemd/system/tb330fu-devd.service
```

This repository does not provide a deployment framework. Installation and
service activation are operational actions outside the source build.

## Verification

Use read-only checks on the target:

```bash
systemctl status tb330fu-devd.service
systemctl show tb330fu-devd.service -p ExecMainPID -p SubState -p StatusText
ls -l /dev/dri /dev/input /dev/block/by-name
udevadm info --query=path --name=/dev/dri/card0
udevadm info --query=path --name=/dev/input/event0
journalctl -b -u tb330fu-devd.service --no-pager
```

Use stable device names or udev queries. `eventN` numbers are not stable
identifiers. Readiness must report `kernel device coldboot complete`;
service liveness alone does not prove every node or alias is correct.

## Failure modes

- If the unit is not started before a consumer, required nodes or aliases may
  be absent when that consumer starts.
- Missing `/sys/dev/char` or `/sys/dev/block` prevents the corresponding
  coldboot pass.
- Malformed or unsafe `DEVNAME`/`PARTNAME` data is rejected.
- A policy generated from mismatched stock inputs can produce incorrect
  ownership or modes; compare generated output and expected counts.
- A daemon failure can leave userspace without reconciliation or live node
  handling until systemd restarts it.

## Known limitations

devd is asynchronous userspace infrastructure, not `devtmpfs`. It requires a
coldboot reconciliation pass, can fail as a daemon, and does not provide
device nodes during the earliest kernel boot phase.
