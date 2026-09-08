# Licensing and attribution

## Project material

Project-authored source, scripts, configuration, documentation, and other
project material are licensed under GPL-3.0-or-later unless a file states
otherwise.

This is not a blanket claim that every file in the repository is GPLv3.
Third-party and vendor-origin material retains its original applicable
copyright, license, and other terms.

## Known third-party material

### Android Open Source Project / AOSP

Android/AOSP-derived interfaces, AIDL, headers, and init fragments retain
their Apache License 2.0 terms and original copyright notices where present.
The frozen graphics AIDL provenance is recorded in
[`src/camera/capture/aidl-graphics-v2-PROVENANCE.md`](../src/camera/capture/aidl-graphics-v2-PROVENANCE.md).
The repository does not claim ownership of the external AOSP build inputs
used by the build scripts.

### v4l2loopback

`modules/v4l2loopback-tb330fu/` is a project fork of v4l2loopback 0.15.4.
It remains GPL-2.0+ with the Linux syscall exception as stated by its source
headers and [`COPYING`](../modules/v4l2loopback-tb330fu/COPYING). Upstream
copyright notices are retained. The exact upstream commit was not recovered.

### JsonCpp

`src/graph-executor/json.h` and `jsoncpp.cpp` contain the JsonCpp 1.6.5
amalgamation. Its embedded notice preserves the JsonCpp public-domain/MIT
terms and Baptiste Lepilleur copyright notice.

### Hostapd-derived init fragment

`vendor/init-corpus/**/vendor/etc/init/hostapd.android.rc` retains the BSD
license notice and Jouni Malinen copyright notice present in the captured
file.

## Captured vendor material

`vendor/init-corpus/immutable/` is an exact stock-captured init input from the
retail TB330FU. `vendor/init-corpus/mutable/` is the project’s Linux-adapted
version of that input; its diff records project engineering decisions.

Captured vendor-origin files remain attributable to their original owners and
are not claimed as project-authored GPL material. Original notices are
preserved in place. The corpus provenance and the known restrictive notice
are documented in [`vendor/init-corpus/PROVENANCE.md`](../vendor/init-corpus/PROVENANCE.md).

## Generated material and external inputs

Generated protobuf/AIDL output is reproducible output from its source schema
or external interface source; it is not evidence that the underlying external
runtime or interface source is project-owned. Stock images, firmware,
proprietary libraries, calibration/security material, and local analysis
captures are external inputs and are not part of the intended public source
publication.
