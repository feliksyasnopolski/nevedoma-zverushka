# PowerDevil DPMS/backlight patch

Status: pending/experimental. Do not deploy from this release tree.

The old workspace contains `powerdevil-dpms.patch`, derived from PowerDevil 6.6.6. It saves nonzero brightness before normal DPMS off, sets brightness to zero, performs the existing DPMS operation, and restores the saved value after DPMS on. A brightness change while off becomes the new restore value. Suspend callbacks and Standby/Suspend DPMS modes are unchanged.

Source version: PowerDevil `4:6.6.6-0ubuntu0.1`.
Changed files: `daemon/actions/bundled/dpms.cpp` and `dpms.h`.
Build: final incremental Ninja build completed.
Recorded installed artifact: `/usr/lib/aarch64-linux-gnu/qt6/plugins/powerdevil/action/powerdevil_dpmsaction.so`.
Recorded SHA-256: `d64b7eeed0b4192b5df756cf013c0e9876661602211ea043088f9d108f26a50f`.

The old audit did not validate brightness, wake restoration, folio transitions, or suspend/resume regression. The checked-in diff is provenance material only and must be independently verified before deployment.
