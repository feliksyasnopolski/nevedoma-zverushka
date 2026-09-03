# Init corpus provenance

The corpus is stock-derived Android init configuration for the Lenovo Tab M11
TB330FU. The originating Android build fingerprint is:

`Lenovo/TB330FU_EEA/TB330FU:15/AP3A.240905.015.A2/S2002859_260104_ROW:user/release-keys`

The source was captured read-only from the stock Android oracle on 2026-08-26
using `adb exec-out su -mm -c cat <exact-path>`.
Partition files came from `/system/etc/init`, `/system_ext/etc/init`,
`/vendor/etc/init`, and `/product/etc/init`. The root-level
`/init.environ.rc` was captured separately. `/odm/etc/init` was absent.

`immutable/` preserves the captured stock text. `mutable/` is the current
project-edited corpus used by the full-corpus compiler. Its differences are
source edits, omissions, and the project addition
`product/etc/init/zz-tb330fu-boot-complete.rc`; they are not generated output.
Each layer has a deterministic SHA-256 manifest.

No ueventd files, property snapshots, firmware, keys, calibration, NVRAM,
attestation or provisioning data are included. No restrictive redistribution
notice was observed for these declarative text inputs. This records provenance
and observed notices; it is not a license grant or a claim of Lenovo
authorization.
