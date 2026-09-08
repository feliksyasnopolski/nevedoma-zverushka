# TB330FU DNG producer

The producer uses the official Adobe DNG SDK 1.7.1 to write an uncompressed
16-bit Bayer DNG. It is a separate root-owned Unix `SOCK_SEQPACKET` service;
the HTTP bridge captures RAW16, supplies the published layout/metadata/
characteristics paths, and serves the resulting artifact.

Install the authoritative rear-camera characteristics artifact at
`/usr/local/share/tb330fu/rear-characteristics.json`; it is static calibration
input and must live outside `/run` so it survives reboot.

The SDK is built with the existing `ssh4net/DNG-CMake` Linux integration. Keep
the official SDK source and resulting `libdng_sdk.a` outside this source tree.
Build with:

```text
DNGSDK_ROOT=/path/to/DNG-CMake/dng_sdk \
DNG_LIB=/path/to/libdng_sdk.a \
JXL_INC=/path/to/libjxl/client_projects/include \
JXL_LIB_INC=/path/to/libjxl/libjxl/lib/include \
  sh src/camera/dng/build.sh
```

The SDK build uses `DNG_WITH_JXL=OFF`, `DNG_WITH_XMP=OFF`, and
`BUILD_DNG_VALIDATE=ON`; JPEG XL and XMP are not needed for this ordinary
uncompressed RAW Bayer output. The target SDK build also provides
`dng_validate` for validation.
