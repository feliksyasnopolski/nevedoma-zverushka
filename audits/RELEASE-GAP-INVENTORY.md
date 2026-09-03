# TB330FU release gap inventory

Date: 2026-09-03

This is a read-only comparison of `/Users/felix/tb330fu` with this release
repository. No tablet, host deployment, source tree, image, module, or old
artifact was changed. No commit was created.

## 1. Current release coverage

| Area | Canonical source | Primary documentation | Ownership |
|---|---|---|---|
| Boot, vendor_boot, stage1 | `src/boot/`, `docs/BOOT.md` | `docs/BOOT.md` | stage1/vendor_boot; systemd after switch-root |
| devd/device lifecycle | `src/devd/`, `systemd/tb330fu-devd.service` | `docs/DEVD.md` | `tb330fu-devd` creates nodes; udev applies policy |
| input/folio/stylus | `src/input/`, `udev/` | `docs/INPUT.md` | folio proxy plus Linux input/udev/logind |
| sensors/IIO | `modules/tb330fu_iio/`, `src/sensors/` | `docs/SENSORS.md` | vendor sensor service; native IIO consumer |
| properties/Rediska | `src/properties/`, `systemd/rediska*` | `docs/PROPERTIES.md` | Rediska/property area; graph consumes properties |
| vendor init corpus | `vendor/init-corpus/` | `docs/INIT_CORPUS.md` | stock-derived immutable corpus and reviewed mutable corpus |
| graph compiler | `tools/graph-compiler/` | `docs/GRAPH_COMPILER.md` | compiler owns generated graph production |
| graph executor/runtime | `src/graph-executor/`, `systemd/tb330fu-connectivity-graph.service` | `docs/VENDOR_RUNTIME.md` | executor owns retained vendor actions; native Linux owns policy |
| GPU/Panfrost | `src/gpu/`, `systemd/tb330fu-panfrost.service` | `docs/GPU.md` | kernel/BSP plus Panfrost; KWin owns desktop rendering |
| display/power/suspend | systemd drop-ins; `docs/DISPLAY_POWER.md` | `docs/DISPLAY_POWER.md` | stock DRM/vendor power; systemd/PowerDevil policy |
| Plymouth/fb clear | `systemd/tb330fu-lk-fb-clear.service` | `docs/PLYMOUTH.md` | early framebuffer clear then Plymouth/SDDM |
| charger | `src/charger/` | `docs/CHARGER.md` | vendor_boot charger branch and native renderer |
| Bluetooth | `src/bluetooth/`, `systemd/tb330fu-bt-*` | `docs/BLUETOOTH.md` | stock WMT/AIDL path; BlueZ owns host policy |
| camera | `src/camera/`, `systemd/tb330fu-camera-*` | `docs/CAMERA.md` | vendor Camera3 plus native V4L2 bridge |

These areas have clear release counterparts. Generated graph JSON is not a
source file and is intentionally excluded; it is produced from the corpus.

## 2. Missing current implementation

### Native audio integration — category A, P0

* Old source: `native-boot/audio-recon/`.
* Current evidence: `native-boot/audio/runs/audio-productization-20260828/REPORT.md`
  records the deployed four-unit chain and the `mt6768-mt6358` ALSA card;
  the retained inventory records the units as active/exited and audio as
  working. The implementation includes `tb330fu-audio-platform.service`,
  `tb330fu-audio-modules.service`, `tb330fu-audio-route.service`,
  `tb330fu-audio-jack.service`, module loaders, mixer routing, UCM files, and
  PipeWire/WirePlumber configuration.
* Release coverage: no `src/audio/`, audio units, UCM configuration, or
  `docs/AUDIO.md` exists. `docs/ARCHITECTURE.md` mentions audio only as a
  compatibility component.
* Action: promote the reviewed source/configuration and add a focused audio
  document, or explicitly mark audio as an external deployment dependency.
  Do not copy it automatically in this inventory.

### Native zram activation source — promoted

* Deployed location: `/usr/local/libexec/tb330fu-zram`; the retained inventory
  records `tb330fu-zram.service` active/exited and zstd 2 GiB zram swap.
* The deployed helper and unit were recovered read-only and are now represented
  by `src/zram/tb330fu-zram`, `systemd/tb330fu-zram.service`, and
  `docs/ZRAM.md`.
* The mutable Android-init corpus no longer contains zram setup, tuning,
  writeback ownership, or swap activation. The generated graph is compiled
  from that mutable corpus and contains no zram records.
* The native policy preserves the deployed 2G request and priority 100 while
  selecting `lz4`; it does not use writeback or a backing device. Live tablet
  swap was not changed by this task.

### Late USB NCM gadget — category A, P0

* Old source: `native-boot/ubuntu-rootfs/tb330fu-usb-gadget` and
  `native-boot/ubuntu-rootfs/tb330fu-usb-gadget.service`.
* Current evidence: the retained deployment inventory records
  `tb330fu-usb-gadget.service` as active/exited and the late management gadget
  as part of the boot path.
* Release coverage: no native USB gadget source, unit, or network companion is
  present. The vendor corpus and graph contain Android USB actions, but they
  are not a substitute for the native configfs NCM helper. The old
  `native-boot/usb-role-recon/` ACM/NCM helpers are separate diagnostic and
  role-reconfiguration experiments.
* Action: promote the exact deployed gadget source/unit and its network
  contract, or document it as an external deployment prerequisite. Keep USB
  role experiments archived. Do not migrate policy in this pass.

No other meaningful current subsystem was found without a release
counterpart. USB/network helpers in `native-boot/usb-role-recon/` are older
diagnostic/ACM/NCM experiments; the deployed late NCM gadget is the distinct
missing current implementation identified above.

## 3. External build/recovery dependencies

Retain outside Git and document centrally in future `docs/BUILD.md` and
`docs/RECOVERY.md` updates:

* exact stock boot, vendor_boot, init_boot, dtbo, recovery, DT and ramdisk
  artifacts plus hashes and provenance;
* `/home/felix/common`, the exact `clang-r510928` toolchain, AOSP Android 15
  r17 headers/libraries, BusyBox and boot-image tooling;
* stock module archives, `Module.symvers`, reconstructed stock CRC data,
  matching stock vmlinux BTF, `bpftool`/`dwarves`, and the kernel module
  manifests;
* proprietary vendor modules/firmware, Wi-Fi NVRAM and Android target
  libraries, retained read-only with redistribution restrictions;
* `native-boot/vendor-boot-build/`, stage1/root-selection recipes, rescue SD
  and internal-root procedures;
* the Samsung MT6768 DRM donor archive and its exact-CRC/BTF/runtime evidence
  as experiment provenance, not current display source.

The build contract is duplicated across working notes and host recipes. A
central build-environment/recovery contract is justified (P1), but large or
proprietary inputs must not be copied into this repository.

## 4. Planned/unfinished work

* zram ownership cleanup and stale systemd dependency edges: planned P2;
* GNSS native consumer/fix and functional proof: unverified/planned;
* Samsung MT6768 DRM donor: experimental and superseded by stock MediaTek DRM;
* alternate compositor/status-display/display experiments and PowerDevil
  deployment variants: experimental or superseded;
* camera UI/APK/AI, Gatekeeper/Keymaster, modem, watchdog, vibrator and USB
  role work: stock audit, probe, or unfinished feature unless covered by a
  current release component. No current promotion follows from existence of
  an audit or client probe.

## 5. Archaeology/superseded material

Keep unique audits, pstore, raw traces, operator evidence, reverse-engineering
databases, stock salvage, GNSS/camera/AI reconstructions, donor reports,
historical graph snapshots, and failed bring-up logs outside the release tree.
The old `analysis/`, `audits/`, `gnss-stock/`, `potemkin-sf/`, migration trees,
and most `*-recon` directories are evidence or provenance, not current source.
Near-duplicate graph snapshots have different provenance even when hashes
match; do not delete by filename or size.

## 6. Generated/transient material

Generated graph JSON, generated policy, build directories, unpacked ramdisks,
compiled binaries, module outputs, caches, screenshots, packet captures,
large logs, temporary patch outputs, and experimental run directories should
remain outside Git. This is an intentional omission, not evidence that the
source is missing. No safe deletion candidate was established by this pass.

## 7. Documentation gaps

Evidence justifies:

* a current `docs/AUDIO.md` when audio source is recovered/promoted;
* a short zram ownership/recovery section, or `docs/ZRAM.md`, only after the
  deployed helper is recovered;
* a centralized build/recovery contract covering host, AOSP/NDK, kernel,
  clang, CRC/BTF, proprietary inputs, hashes and rescue procedures.

Evidence does not justify separate new USB, networking, Binder, SELinux,
APEX, thermal, watchdog, modem, or security primary documents at this time.
Binder/filesystem/linker/SELinux compatibility is already represented by the
release vendor-runtime/property architecture and retained vendor corpus;
stock thermal, charging, RTC, watchdog and storage behavior is BSP material.

## 8. Recommended next actions

* **P0:** recover and promote or explicitly package the active native audio
  source, units, UCM and PipeWire/WirePlumber configuration.
* **P0:** recover the deployed zram activation helper and record its exact
  source/artifact hash and ownership boundary.
* **P0:** promote or explicitly package the deployed late USB NCM gadget
  source, unit and network contract.
* **P1:** consolidate the shared build and recovery contract without copying
  proprietary or generated inputs.
* **P2:** audit graph/native ownership for zram and generic USB/network policy;
  do not migrate it as part of this inventory.
* **P3:** preserve GNSS, donor DRM, camera UI/AI, PowerDevil variants and other
  probes as explicitly historical or planned work.
* **P4:** add factual README/LORE notes only after the current-vs-historical
  boundary is stable.

Validation for this pass: release `git status --short` was clean before the
report; `git diff --check` was run after creating this uncommitted report.
