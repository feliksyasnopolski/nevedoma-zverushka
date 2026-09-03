# Android init corpus

## Purpose

`vendor/init-corpus/mutable/` is the declarative source parsed to generate the TB330FU vendor runtime graph.
The graph is generated output, not canonical source.

## Source corpus

The corpus contains Android init-language text from the stock TB330FU partitions `/system`, `/system_ext`, `/vendor`, and `/product`, plus the root-level `/init.environ.rc`.
`/odm/etc/init` was absent.
The stock layer contains 225 source files; the mutable compiler input contains 201 source files, excluding their manifest files.

The corpus does not include ueventd configuration because the full-corpus compiler does not parse it.
Property snapshots and service ledgers are not compiler inputs.
The compiler input boundary is the files reachable by the parser from the mutable tree's regular files and its resolved imports.

## Immutable and mutable layers

`immutable/` is a read-only reference copy of captured stock text.
It is preserved verbatim and is not the compiler input.

`mutable/` is a project-maintained copy.
Files may be edited, omitted, or added to establish the native-runtime selection.
It is not a generated overlay: the compiler parses this tree as one corpus.
The current compiler does not merge immutable and mutable trees and does not apply file-level overlay precedence.
The immutable layer is used for comparison and audit.

## Selection methodology

Selection is a conservative, partly manual graph-carving process.
A branch is retained when it supplies a retained vendor process, Binder/HIDL/AIDL endpoint, property producer or consumer, service dependency, imported init definition, required sysfs/device/module action, or hardware bring-up prerequisite.
The decision is checked against retained runtime owners and observed source/runtime evidence.
A file name alone is not sufficient.

The mutable tree records the resulting carve.
The compiler then applies only the transformations currently implemented in its source: it suppresses modules already established by the stock first-stage closure, expands `class_start` over the parsed service set, removes reviewed host-owned commands, and inserts the measured module-readiness wait.
These transformations are deterministic but are not a second tracked corpus.

VINTF and Binder interface evidence can establish why a vendor endpoint is retained; it does not make Android the owner of the desktop or generic Linux policy.
Runtime traces and service-call evidence support liveness decisions, but do not by themselves authorize unrelated branches.
The mutable carve omits absent optional and non-normal-boot imports; an absent import that is present in the carve remains a compiler diagnostic.

## Retained behavior

| Cluster | Why retained | Native owner above it | Notes |
|---|---|---|---|
| Connectivity/WMT and Wi-Fi | MediaTek drivers, firmware helpers, readiness properties, and Wi-Fi HAL processes remain hardware prerequisites. | NetworkManager and native networking policy | Mixed ownership. |
| Bluetooth | WMT/Bluetooth modules and the MediaTek Android HAL remain the hardware path. | BlueZ/`bluetoothd` | The Android bridge is compatibility code. |
| GNSS | `mnld`, GNSS helpers, driver actions, and the MediaTek GNSS HAL remain the available vendor path. | Native GNSS consumers | Ownership and full physical behavior remain partly unverified. |
| Sensors | The sensor HAL and its kernel/module preparation remain behind the native IIO producer. | IIO and `iio-sensor-proxy` | The Android service remains below the native interface. |
| Camera | Camera HAL startup/configuration remains required by the native camera bridge. | Native camera daemon/V4L2 bridge | Capture liveness and valid pixels are separate checks. |
| Binder/vendor services | Retained vendor processes require Android Binder compatibility and service credentials/options. | Native service orchestration | This is a compatibility substrate, not Android framework startup. |
| Power/audio/vendor support | Retained MediaTek/Lenovo processes and hardware actions remain where no complete native replacement exists. | systemd, PipeWire/WirePlumber, and PowerDevil as applicable | Ownership is mixed by component. |

## Excluded behavior

The mutable carve excludes Android framework UI and application startup, SurfaceFlinger/hwcomposer desktop ownership, package/framework lifecycle, Android-only diagnostics and factory/META/recovery paths, absent variant imports, and dead service definitions.
Operations replaced by native Linux owners are also excluded where the current compiler has an explicit reviewed transformation.
Unsupported or absent imports remain diagnostics rather than being silently substituted.

## Ownership drift

Some generic policy remains in the mutable corpus because it has not yet been migrated.
These actions remain until a native owner and equivalent ordering are established.
Known candidates include generic network policy, USB policy, and generic sysfs tuning.
Their presence does not make them vendor-substrate ownership.
Zram setup and activation have been removed from the mutable corpus and are owned by `tb330fu-zram.service`.
Future cleanup must establish the native owner and verify ordering before removal.

## Ordering and imports

The parser uses AOSP r17-style end-of-file import processing.
A file's local actions and services are closed before its imports are parsed.
Imports are resolved against the mutable root using the observed values `ro.hardware=mt6768`, `ro.zygote=zygote64_32`, and `ro.vendor.rc=/vendor/etc/init/hw/`.
A directory import is expanded in sorted path order; explicit imports are not globbed.
Repeated imports are retained.

File discovery and import traversal produce parser declaration order.
The compiler preserves action/service declaration order except for its documented transformations.
Duplicate service stanzas remain separate parsed declarations; `class_start` expands over matching non-disabled declarations in that order.
There is no immutable-over-mutable override rule in the current compiler.

## Provenance

See [`vendor/init-corpus/PROVENANCE.md`](../vendor/init-corpus/PROVENANCE.md).
The stock build identity, capture command, partition paths, and redistribution limits are recorded there.
The corpus contains declarative text only.

## Regeneration contract

```text
mutable corpus + compiler transformations
    -> generated/full-corpus/full-corpus-graph.json
```

The graph is disposable, deterministic build output.
It is not hand-edited, not tracked in Git, and must be regenerated from tracked source when needed.
The graph compiler and executor are separate subsystems.

## Verification

The two corpus manifests record SHA-256 for every source file.
Verification should confirm that the manifests match, that the corpus contains only text inputs and manifests, and that the mutable parser reports no unexpected binary files.
Compare semantic differences with the immutable layer before accepting a carve change.
The current compiler is fail-closed for unsupported syntax and records source path and line for diagnostics.

## Known limitations

The selection is not yet encoded as an independent semantic include/exclude manifest.
The mutable tree is therefore the authoritative carve result, while some ownership decisions remain manual and compiler-side.
The immutable-versus- mutable semantic audit currently reports unresolved differences that require human review; those records are not promoted as release documentation.