# Build environment

This document defines the external environment for building the TB330FU
release sources. It is a provenance contract. Subsystem documents and the
repository build files remain the authority for individual commands.

The build host is a Linux x86_64 machine. The currently validated host is
`entertainment.local`, Ubuntu 26.04.1 LTS. The host is not the target runtime:
native Linux artifacts target AArch64 glibc, while retained Android/vendor
bridges target AArch64 bionic and are linked with target Android libraries.

## Dependency status

The following terms distinguish the release contract from the present
development layout:

| Term | Meaning |
|---|---|
| Required | Needed to reproduce the corresponding artifact class. |
| Validated with | Version observed in the existing build environment or build metadata. A newer compatible version is not automatically equivalent. |
| External | Must be supplied by the builder; it is not redistributed here. |
| Current path | An evidence path only. It is not a repository requirement. |

The old workspace `/Users/felix/tb330fu` and its remote counterpart are
evidence sources. They are not canonical dependency paths. Use logical roots
such as `KERNEL_DIR`, `NDK`, `AOSP_ROOT`, `ANDROID_BINDER_LIBS`, and
`BUSYBOX` when reconstructing the environment.

## Host tools and provenance

The versions below were read from `entertainment.local`; no build was run for
this document.

| Tool | Validated version | Provenance and requirement |
|---|---|---|
| Python | 3.10.12 | Ubuntu host `/usr/bin/python3`; required by graph compiler, boot scripts, and module-order generation. |
| GNU Make | 4.4.1 | Ubuntu package; required by Makefiles and Kbuild. |
| Ninja | 1.10.1 | Ubuntu package; no repository build file currently requires Ninja directly. |
| Host clang | Ubuntu clang 14.0.0 | Host package; not a substitute for the Android clang used for kernel modules or bionic programs. |
| Host GCC | 11.4.0 | Ubuntu package; used as the source of the AArch64 cross compiler package. |
| `aarch64-linux-gnu-gcc/g++` | 11.4.0 | Ubuntu cross-compiler package; validated for native AArch64 glibc programs and the boot `init`. |
| `aarch64-linux-android35` clang++ | NDK 28 toolchain | Android NDK LLVM driver; target name is explicit in Android bridge scripts. |
| Android clang | 18.0.0, based on `r510928` | Downloaded Android LLVM toolchain. Compiler banner identifies build `11368308` and LLVM commit `477610d4d0d988e69dbc3fae4fe86bff3f07f2b5`. Required for target kernel modules. Current path example: `/home/felix/tb330fu/toolchains/clang-r510928/bin`. |
| AIDL | Build-tools 35.0.0 | Android SDK build-tools binary. It generates NDK/VINTF AIDL sources for camera, sensors, and Bluetooth. |
| `pahole`/dwarves | v1.31 custom build | Required for module BTF. The zstd-capable build is used with the matching dwarves and libdw/libelf libraries; current build examples are under `/home/felix/tb330fu/dwarves/build-zstd` and `/home/felix/tb330fu/tools-zstd`. |
| `bpftool` | Not recovered on the host PATH | Required for base-aware split-BTF inspection. The source/workflow is in the kernel tree; the executable provenance and exact validated binary are not yet recovered. |
| `magiskboot` | Project-local binary; exact embedded version not recovered | Required by `scripts/build_vendor_boot.sh` for legacy LZ4 compression. Current path example: `/usr/local/bin/magiskboot`. |
| GNU cpio | 2.13 | Ubuntu package; creates the reproducible `newc` vendor ramdisk. |
| BusyBox | 1.30.1 | Ubuntu package was observed on the host, but the target input is a separately supplied statically linked AArch64 binary. The exact target BusyBox build provenance is not recovered. |
| zstd | Ubuntu host package | Used by the zstd-capable dwarves workflow. Exact package version was not needed by the repository build files and is not recorded as a contract. |
| LZ4 | `magiskboot` legacy LZ4 support | A separate `lz4` executable is not currently installed on the host and is not required by the vendor_boot script. |

The host-side Apple tools in the old workspace are not a valid replacement for
the Linux build host. The release scripts that use `linux-x86_64` NDK
prebuilts and the boot builder's Linux guard require a Linux host.

## Compiler and ABI contract

Native Linux programs use the Ubuntu AArch64 glibc cross toolchain unless a
source file is configuration-only. This includes `tb330fu-devd`, the folio
input helper, graph executor, Rediska's native broker and control utility,
the charger renderer, and boot `src/boot/init.c`. The charger additionally
requires AArch64 `libdrm` and FreeType development headers/libraries. The
source Makefile currently names `/usr/include/freetype2`, `/usr/include/libdrm`,
and `/usr/lib/aarch64-linux-gnu`; these are host package layout assumptions,
not private project paths.

Android-facing programs are bionic AArch64 PIE executables. They use an NDK
LLVM compiler with `--target=aarch64-linux-android35`, target Android headers,
and target libraries from the retained Android runtime. They are not host
glibc programs and must not be linked against host `libbinder`, `libutils`, or
other host libraries. The runtime linker and library search paths are part of
the deployed target unit files.

The retained Android API assumption is API 35 / Android 15 interface shape for
the Android-facing targets. This does not establish one global AOSP r17
checkout. The camera build pins its graphics mapper snapshot to the AOSP tag
`android-15.0.0_r17`; the graph and property contracts also refer to Android
interface semantics. The current header bundle is mixed: its nested
`interfaces` Git checkout reports tag `android-13.0.0_r84`, commit
`8c308539d804a6efb2272e4c9e73407241966f34`, while the scripts consume Android
15-shaped paths and interfaces. The exact complete AOSP source provenance of
the current copied header bundle is unresolved. Do not treat the Android 13
tag as the canonical release target.

## Android, AIDL, Binder, and FMQ inputs

The Android build side requires the following external roots:

| Input | Required content | Consumers |
|---|---|---|
| Android NDK r28 `28.0.13004108` | LLVM prebuilt, bionic sysroot, libc++ | Sensors and Bluetooth scripts; also the camera target compiler. |
| Android SDK Build Tools 35.0.0 | `aidl` compiler | Camera, sensors, and Bluetooth AIDL generation. |
| AOSP source/header roots | Binder NDK C++ headers, bionic/property headers, `libfmq`, `libbase`, `libcutils`, `libutils`, `liblog`, camera/common AIDL and metadata headers. Component revisions must match the retained interfaces; the complete provenance is unresolved. | Camera, sensors, Bluetooth, and Rediska Android frontend. |
| Frozen graphics AIDL snapshot | `graphics/common`, `graphics/allocator`, and stable mapper C headers at `android-15.0.0_r17` | Camera mapper v2/v5-compatible generated bindings. |
| Target Android libraries | `libbinder_ndk.so`, `libcamera_metadata.so`, `libfmq.so`, `libcutils.so`, `libutils.so`, `libbase.so`, `liblog.so`, `libandroid.so`, `libpng.so`, and `libz.so` as applicable | Bionic bridge link steps. These are copied from the stock-compatible target runtime or a matching extracted root, not host libraries. |

Generated AIDL C++ is disposable output under each component's build output;
the `.aidl` contracts in the repository and the external AOSP contracts are
the sources. `IBluetoothHci` and callback contracts are local copies of the
interface source used by the Bluetooth bridge. Camera and sensor FMQ support
compiles the authoritative AOSP `EventFlag.cpp` and `FmqInternal.cpp` subset;
it does not substitute a host `libfmq.so`. `PAGE_SIZE=4096`,
`-DBINDER_STABILITY_SUPPORT`, VINTF stability, and the component-specific
interface versions are part of the validated compile contract.

Rediska has two ABI families. Its broker and `rediskactl` are glibc AArch64;
`rediska_android_property_service`, the genuine property backend/client, and
the property-info generator use the Android bionic property ABI and AOSP
property parser/serializer sources. See [PROPERTIES.md](PROPERTIES.md) and
[`src/properties/Makefile`](../src/properties/Makefile).

## Kernel and module build contract

External modules must be built against the reconstructed Android-derived
kernel source and prepared output tree for the exact target kernel release:

| Requirement | Contract |
|---|---|
| Kernel release | `6.6.82-android15-8-g076ac12bde16-ab13715361-4k`, `arm64`; verify the exact target configuration and vermagic before deployment. |
| Kernel source | The reconstructed `common` tree plus the matching MediaTek/Lenovo device-module source and headers. Current source example: `/home/felix/tb330fu/common`; the supplied module tree example is `/home/felix/tb330fu/samsung-mt6768-6.6/kernel/kernel_device_modules-6.6`. |
| Prepared output | A prepared `O=` tree with generated headers, `.config`, `Module.symvers`, and the target kernel's generated metadata. |
| Compiler | Android clang based on `r510928`, `LLVM_IAS=1`, `ARCH=arm64`. Host clang and GCC have failed target module gates. |
| Cross prefix | The Kbuild `CROSS_COMPILE` setting may remain available for auxiliary tools, but LLVM is the compiler authority for the validated module workflow. |
| Modversions | `CONFIG_MODVERSIONS=y`; preserve the target `Module.symvers` and imported symbol CRCs. |
| BTF | `CONFIG_DEBUG_INFO_BTF=y` and `CONFIG_DEBUG_INFO_BTF_MODULES=y`; every deployed project module must retain a `.BTF` section. |
| Signing | The prepared configuration has module signing enabled (`CONFIG_MODULE_SIG=y`, SHA-1, RSA key metadata). Whether a release module is signed by the target deployment pipeline is an external key/provisioning decision; no signing key is redistributed here. |
| Vermagic | Must match the running target kernel exactly. A successful compiler invocation is not sufficient. |

The release module set includes the Bluetooth HCI shim, sensor IIO shim,
camera `v4l2loopback` fork, and LK framebuffer helper. Their build entry
points are the respective `Makefile`/`build.sh` files under `src/` and
`modules/`. Built `.ko` files are disposable and are not canonical source
artifacts in this repository. Stock Lenovo/MediaTek modules remain imported
deployment inputs.

## BTF and symbol CRC contract

Module BTF is mandatory. Earlier modules built without module BTF caused
kernel panics on this device. Do not disable `CONFIG_DEBUG_INFO_BTF_MODULES`,
strip `.BTF`, or replace a split-BTF module with a standalone BTF encoding.

The validated workflow uses:

1. the prepared output tree for `6.6.82-android15-8-g076ac12bde16-ab13715361-4k`;
2. the reconstructed stock base BTF, currently represented by the external
   `stock-vmlinux.btf` artifact;
3. zstd-capable `pahole`/dwarves v1.31 and its matching libdwarves/libdw/libelf
   runtime;
4. kernel `scripts/mod/modpost`, Kbuild, and `scripts/sign-file` where signing
   is authorized; and
5. the reconstructed stock symbol-CRC data, currently represented by
   `stock-reconstructed.symvers` and the `recover-stock-crcs.py` workflow.

The current external artifact hashes are evidence identifiers, not repository
payloads: `stock-vmlinux.btf` SHA-256 is
`8551db8c9f43ddf5dc949c7b6094f5362e692803edaa4f4ed78213d98ff3acba`, and
`stock-reconstructed.symvers` SHA-256 is
`c58d55871ae24d83dc6f72348bf2fe19d8ee1d17600325e23a8a2b913d6674c4`.
Obtain them from the matching stock-kernel reconstruction; do not copy these
values onto a different kernel.

Split-BTF compatibility and symbol-CRC compatibility are separate gates.
`bpftool` must validate a module's split BTF against the matching base BTF;
standalone `pahole -F btf` output is not that check. Kbuild/modpost and
`Module.symvers`/`__versions` must separately provide the exact symbol CRC
closure. Passing one gate does not prove the other, and neither proves runtime
driver compatibility. The exact validated `bpftool` binary provenance is
currently unknown and must be resolved before a clean-room reproduction.

## Boot image inputs and tooling

`scripts/build_vendor_boot.sh` builds only the project-owned `init` and
vendor-ramdisk fragment, then replaces the platform `init_boot` fragment in a
stock Android `vendor_boot` v4 container. It requires:

| Input/tool | Role |
|---|---|
| Stock-compatible `vendor_boot` v4 image | Preserved header, other fragments, DTB, bootconfig, and container metadata. |
| Static AArch64 BusyBox | Installed as `/bin/busybox` in the ramdisk; exact target provenance is external and must be retained by the supplier. |
| Exact 154-module early closure | Supplied according to `manifests/modules.load.native`, including vermagic, CRC, and BTF-compatible modules. |
| `aarch64-linux-gnu-gcc` | Static AArch64 build of `src/boot/init.c`. |
| GNU cpio 2.13 | Reproducible `newc` ramdisk creation. |
| `magiskboot` | `lz4_legacy` compression and boot-image support; exact binary release is not recovered. |
| Python 3 | `rebuild_vendor_boot.py`, validation, and module-order tooling. |
| `sha256sum`, `cmp`, `find`, `sort`, and POSIX shell tools | Deterministic assembly and validation. |

DTB/DTBO, stock boot/vendor_boot metadata, firmware, Wi-Fi NVRAM, SELinux
policy, target Android libraries, and proprietary Lenovo/MediaTek userspace
are imported or stock-derived inputs. They are intentionally not
redistributed. Generated ramdisks, images, module order reports, and AIDL
outputs are disposable build products unless a release artifact is explicitly
archived outside the source tree. Runtime boot behavior is documented in
[BOOT.md](BOOT.md).

## Artifact classes and subsystem matrix

| Subsystem | Class | Target ABI | Build system | Compiler/toolchain | External build inputs | Build entry point |
|---|---|---|---|---|---|---|
| Boot `init` and stage1 | Source-built plus generated image | glibc AArch64 for `init`; BusyBox target ABI | Make-like shell script | `aarch64-linux-gnu-gcc`; supplied BusyBox | stock vendor_boot, DTB/bootconfig, modules, magiskboot, cpio | [`scripts/build_vendor_boot.sh`](../scripts/build_vendor_boot.sh) |
| devd and policy | Source-built; checked-in generated policy | glibc AArch64 | GNU Make; Python only for explicit policy generation | AArch64 GCC | stock `ueventd.rc` and policy TSV only when regenerating | [`src/devd/Makefile`](../src/devd/Makefile) |
| Folio input | Source-built | glibc AArch64 | GNU Make | AArch64 GCC | Linux input/uinput headers | [`src/input/Makefile`](../src/input/Makefile) |
| Sensors producer | Source-built plus generated AIDL | bionic AArch64 | shell script, AIDL, direct compile | NDK r28 clang, target `android35` | AOSP sensor/common/FM Q AIDL, Binder NDK, target libs | [`src/sensors/build.sh`](../src/sensors/build.sh) |
| Sensor IIO module | Source-built kernel module | kernel arm64 | Kbuild | clang r510928, LLVM IAS, pahole | prepared kernel, Module.symvers, stock BTF/CRCs | [`modules/tb330fu_iio/build.sh`](../modules/tb330fu_iio/build.sh) |
| Panfrost helper | Script/config only | host shell | none | none | stock Panfrost/MediaTek kernel drivers | [`src/gpu/tb330fu-panfrost`](../src/gpu/tb330fu-panfrost) |
| Bluetooth AIDL bridge | Source-built plus generated AIDL | bionic AArch64 | shell script, AIDL, direct compile | NDK clang, target `android35` | local AIDL contracts, AOSP Binder headers, target `libbinder_ndk.so` | [`src/bluetooth/aidl-bridge/build.sh`](../src/bluetooth/aidl-bridge/build.sh) |
| Bluetooth HCI shim | Source-built kernel module | kernel arm64 | Kbuild | target kernel compiler contract | prepared kernel, stock Bluetooth modules and CRC/BTF data | [`src/bluetooth/hci-shim/Makefile`](../src/bluetooth/hci-shim/Makefile) |
| Camera daemon | Source-built plus generated AIDL/FMQ | bionic AArch64 | shell script, AIDL, direct compile | NDK clang, target `android35` | camera/common AIDL, graphics snapshot, FMQ sources, target Binder/mapper/media libs | [`src/camera/daemon/build.sh`](../src/camera/daemon/build.sh) |
| Camera `v4l2loopback` fork | Source-built kernel module | kernel arm64 | Kbuild | target kernel compiler contract | prepared kernel, Module.symvers, BTF/CRC data | [`modules/v4l2loopback-tb330fu/build.sh`](../modules/v4l2loopback-tb330fu/build.sh) |
| Charger renderer | Source-built | glibc AArch64 | GNU Make | AArch64 GCC | AArch64 libdrm and FreeType | [`src/charger/Makefile`](../src/charger/Makefile) |
| Rediska broker/control | Source-built | glibc AArch64 | GNU Make | AArch64 GCC | pthread and Linux headers | [`src/properties/Makefile`](../src/properties/Makefile) |
| Rediska Android frontend/generator | Source-built | bionic AArch64 | GNU Make | NDK-compatible clang plus AOSP parser/serializer | AOSP property sources matching the retained Android property ABI, bionic headers/libs | [`src/properties/Makefile`](../src/properties/Makefile) |
| Graph compiler | Generated graph from source corpus | host Python | Python script | none | corpus and compiler config | [`tools/graph-compiler/compiler.py`](../tools/graph-compiler/compiler.py) |
| Graph executor | Source-built | glibc AArch64 | GNU Make | AArch64 G++ | checked-in JSON headers/source | [`src/graph-executor/Makefile`](../src/graph-executor/Makefile) |
| Plymouth/fbclear | Source-built kernel module plus configuration | kernel arm64 / glibc userspace config | Kbuild and systemd | target kernel compiler for module | prepared kernel, stock BTF/CRCs | [`modules/tb330fu_lk_fb_clear/Makefile`](../modules/tb330fu_lk_fb_clear/Makefile) |
| Audio | Script/configuration only | native Linux | systemd, UCM2, WirePlumber | none | stock ASoC modules and firmware | [`docs/AUDIO.md`](AUDIO.md) |
| ZRAM | Script/configuration only | native Linux | systemd and shell | none | kernel `zram.ko` | [`src/zram/tb330fu-zram`](../src/zram/tb330fu-zram) |
| USB gadget | Script/configuration only | native Linux | systemd and shell | none | kernel configfs/MUSB UDC | [`src/usb/tb330fu-usb-gadget`](../src/usb/tb330fu-usb-gadget) |
| PowerDevil integration | Patch/configuration only | host KDE/Qt | patch application and KDE build outside repo | none in this tree | matching PowerDevil source/package | [`patches/powerdevil/`](../patches/powerdevil/) |
| Vendor runtime/init corpus | Imported plus generated | target Android/vendor runtime | Python graph generation and shell/runtime config | none for checked-in corpus | stock init files, vendor libraries, Binder/HIDL services, firmware, policy | [`docs/VENDOR_RUNTIME.md`](VENDOR_RUNTIME.md) |

The graph JSON, generated policy, generated AIDL, module-order reports, boot
ramdisk, and boot images are outputs. The repository's source corpus, scripts,
Makefiles, AIDL contracts, and configuration are the canonical repository inputs.

## Known provenance gaps and path debt

The following are deliberately not presented as canonical facts:

* the exact `magiskboot` release and exact target BusyBox build;
* the validated `bpftool` binary and package/source release;
* a complete, single-revision AOSP checkout corresponding to all current
  copied headers; and
* exact package versions for host zstd, libdrm, and FreeType.

The release scripts still contain development-path defaults. In particular,
`src/sensors/build.sh` and `modules/tb330fu_iio/build.sh` contain defaults for
`/home/felix/android-aosp-headers`, `/home/felix/android-tools`,
`/home/felix/tcl11-android-root`, `/home/felix/tb330fu`, the release scratch
directory, and the old clang/pahole locations. These are technical debt and
current-environment examples, not normative dependencies. Override them with
logical environment variables or relocate the scripts before a clean-room
build. No unrelated script was changed while documenting this contract.
