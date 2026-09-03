# PowerDevil DPMS/backlight patch

Status: supported for the TB330FU Plasma deployment.

The patch set, derived from PowerDevil 6.6.6, saves nonzero brightness before normal DPMS off, preserves the logical `setBrightness(0)` operation, writes `0` to `/sys/class/leds/lcd-backlight/brightness` for hardware-off, performs the existing DPMS operation, and restores the saved value exactly once on idle or screen-off lid wake. Screen-off lid policies explicitly request DPMS on when the lid opens. A brightness change while off becomes the new restore value. Wake without a saved screen-off brightness does not restore default state. Suspend callbacks and Standby/Suspend DPMS modes are unchanged.

Source version: PowerDevil `4:6.6.6-0ubuntu0.1`.
Changed files: `daemon/actions/bundled/dpms.cpp`, `dpms.h`, and `handlebuttonevents.cpp`.
Build: final incremental Ninja build completed.
Recorded installed artifact: `/usr/lib/aarch64-linux-gnu/qt6/plugins/powerdevil/action/powerdevil_dpmsaction.so`.
Recorded DPMS SHA-256: `a455613bd87d7595e8dc2e9b8fadeb59012c014e6ed20922f24094d548bb6616`.
Recorded lid-action SHA-256: `0ff9cf49df8d481ff05c9448dea018c1d37c09f273a5d10bc9bc78aa39558162`.
Recorded companion PowerProfile SHA-256: `8710f39bc7435b8432d5a39a625d47d2512d2b4ee3a1639232020b806005e8cf`.
