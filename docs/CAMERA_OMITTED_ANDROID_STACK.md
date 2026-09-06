# Omitted Android camera/media stack

## Executive summary

The exact corpora are:

- immutable stock reference: `vendor/init-corpus/immutable/`
- mutable/current carve: `vendor/init-corpus/mutable/`

The mutable corpus retains the physical Camera3 provider path. The following
camera-critical init services are present in both corpora with the same
service definitions:

- `cameraserver` -> `/system/bin/cameraserver`
- `camerahalserver` -> `/vendor/bin/hw/camerahalserver`
- `vendor.gralloc-v2` -> `/vendor/bin/hw/android.hardware.graphics.allocator-V2-service-mediatek`
- `android-hardware-media-c2-hal` -> `/vendor/bin/hw/android.hardware.media.c2-mediatek`
- `vendor-dolby-media-c2-hal` -> `/vendor/bin/hw/vendor.dolby.media.c2-default-service-dax`
- `media`, `media32`, `media64`, `mediaextractor`, and `mediametrics`

The clearly omitted camera-adjacent definitions are `virtual_camera`,
`surfaceflinger`, the vendor HWC service, GPU service/platform startup, and
the zygote/application-startup path. Those omissions remove Android desktop,
application lifecycle, and virtual-camera ownership; they do not remove the
physical Camera3 HAL closure used by the current daemon.

The init corpus alone cannot prove that a service binary or a shared library
is present on a partition. It proves the stock/current init declaration and
its executable path. Binary/library ownership below is therefore marked as
confirmed from path/name or as inference where the corpus has no interface
declaration.

The live tablet inspection resolves the important ambiguity: `cameraserver`
and Codec2 are both selected and running. The deployed graph has the same
content hash as `generated/full-corpus/full-corpus-graph.json`, but the active
systemd unit still invokes the older-named `connectivity-fsm` executable with
the installed graph path `/usr/lib/tb330fu/connectivity/connectivity-graph.json`.
The installed graph metadata and file hash match the canonical generated graph;
the path/executor distinction is recorded because it is part of the actual
runtime authority.

## Immutable and mutable init corpora

`docs/INIT_CORPUS.md` defines `immutable/` as the verbatim, read-only copy of
captured stock init text and `mutable/` as the project-maintained corpus parsed
to generate the current runtime graph. They are not overlay layers; the
compiler parses the mutable tree as one corpus. The stock corpus contains 225
source files and the mutable compiler input contains 201, excluding manifests.
The provenance file records a read-only stock capture from `/system`,
`/system_ext`, `/vendor`, and `/product`.

The comparison used relative paths and service declarations. A file being
present in both corpora does not by itself mean its service is started; for
example, `media32` and `media64` are declared `disabled`.

## Current generated/runtime graph and live state

### Graph selection

Canonical generated graph:

```text
generated/full-corpus/full-corpus-graph.json
file SHA-256: d9ab7b941f854d2e14bc794d2c8f068b7983002a7e60fee3eb77af1460812fdb
metadata source hash: 91195b03a3641c291010c557949464b4540f65048022b57bf866fc533e94ff74
```

The tablet's active unit is:

```text
tb330fu-connectivity-graph.service
  -> /usr/local/sbin/connectivity-fsm --run
  -> /usr/lib/tb330fu/connectivity/connectivity-graph.json
```

The installed graph at that path has the same file SHA-256 and metadata as the
canonical generated graph. It is therefore the runtime graph used below. The
active executor name is historical/deployed state, not evidence of a different
graph format.

The graph contains 162 service records and 382 actions. Its generated
`class_start hal` expansion selects:

```text
hidl_memory
vendor.gralloc-v2
android-hardware-media-c2-hal
vendor-dolby-media-c2-hal
```

Its generated `class_start main` expansion selects:

```text
cameraserver
camerahalserver
media
mediaextractor
mediametrics
```

`media32` and `media64` are still present as disabled declarations and are not
selected by class start. The `media` service is selected with executable
`/system_ext/bin/mediahelper`; the live child ultimately presents as
`/system/bin/mediaserver` with Linux comm `mediaserver32`. The helper contains
the 32/64-bit mediaserver fallback strings, so the observed process is the
runtime result of the selected media helper path rather than evidence that the
disabled `media32` or `media64` init records were started directly.

### Live process and Binder evidence

Read-only SSH inspection on the tablet at 2026-09-05 found:

| Runtime role | PID / executable | State | Evidence |
|---|---|---|---|
| HIDL allocator | 25989, `/system/system_ext/bin/hw/android.hidl.allocator@1.0-service` | running | process; `android.hidl.allocator@1.0` symbols |
| Graphics allocator | 25993, `/vendor/bin/hw/android.hardware.graphics.allocator-V2-service-mediatek` | running | process; `android.hardware.graphics.allocator.IAllocator/default` registration |
| MediaTek C2 | 25995, `/vendor/bin/hw/android.hardware.media.c2-mediatek` | running | process; AIDL `IComponentStore/default` and HIDL `IComponentStore/default9` registrations |
| Dolby C2 | 26006, `/vendor/bin/hw/vendor.dolby.media.c2-default-service-dax` | running | process; Dolby C2 store registration strings and C2 service dependencies |
| Camera service | 34322, `/system/bin/cameraserver` | running | process; `android.frameworks.cameraservice.service.ICameraService/default` and `media.camera` |
| Media extractor | 34327, comm `media.extractor`, `/system/bin/mediaextractor` | running | process; `media.extractor` registration |
| Media metrics | 34328, comm `media.metrics`, `/system/bin/mediametrics` | running | process; `media.metrics` registration |
| Media player/server | 34354, `/system/bin/mediaserver`, comm `mediaserver32` | running | process; `media.player`, resource-manager, and related media registrations |
| Camera HAL server | 34367, `/vendor/bin/hw/camerahalserver` | running | process; provider and vendor-camera registrations |
| Project camera daemon | 34864, `/usr/local/lib/tb330fu/tb330fu-camera-daemon` | running | systemd unit; direct AIDL provider/allocator lookup strings |

The Binder service manager also showed:

```text
android.frameworks.cameraservice.service.ICameraService/default
android.hardware.camera.provider.ICameraProvider/internal/0
android.hardware.graphics.allocator.IAllocator/default
android.hardware.media.c2.IComponentStore/default
android.hardware.media.c2.IComponentStore/default9
media.camera
media.extractor
media.metrics
media.player
media.resource_manager
media.resource_observer
vendor.mediatek.hardware.camera.atms.IATMs/default
vendor.mediatek.hardware.camera.bgservice.IBGService/default
vendor.mediatek.hardware.camera.isphal.IISPModule/internal/1
vendor.mediatek.hardware.camera.postproc.IPostDevice/default
```

This proves service registration and process liveness at the snapshot. It does
not prove that every registered interface is active in a capture or recording
transaction.

## Camera-related init diff

### Retained physical camera closure

| Service | Binary | Immutable | Mutable | Classification and evidence |
|---|---|---:|---:|---|
| `cameraserver` | `/system/bin/cameraserver` | yes | yes | AOSP/framework camera service by name/path inference; declares `android.frameworks.cameraservice.service.ICameraService/default`; user `cameraserver`, groups `audio camera input drmrpc 1998`, `SYS_NICE`. |
| `camerahalserver` | `/vendor/bin/hw/camerahalserver` | yes | yes | MediaTek/vendor camera HAL server by vendor path/name inference; user `cameraserver`, groups `audio camera input drmrpc sdcard_rw system media graphics`, `SYS_NICE`; no init-declared Binder interface. |
| `vendor.gralloc-v2` | `/vendor/bin/hw/android.hardware.graphics.allocator-V2-service-mediatek` | yes | yes | MediaTek allocator service; user `system`, groups `graphics drmrpc`, `SYS_NICE`, class `hal animation`; restarts `surfaceflinger`, `vendor.hwcomposer`, and `camerahalserver`. |
| `cameraconfig` actions | no service binary | yes | yes | Lenovo camera data-directory setup under `/data/camera`; `cameraserver` ownership and `restorecon`. |

The only textual difference in the allocator rc is the missing final newline
in immutable versus mutable. It is not a semantic service change. The
camera-server and camera-HAL rc files are byte-for-byte equal.

The current repository documents the live logical closure as provider
`android.hardware.camera.provider.ICameraProvider/internal/0`, device
interfaces `device@1.1/internal/0` and `/1`, allocator, mapper, Binder,
properties, device nodes, firmware, and modules. The project daemon speaks
Camera3 directly and does not start or repair this closure.

### Omitted or changed camera-adjacent definitions

| Definition | Binary | Immutable | Mutable | Result |
|---|---|---:|---:|---|
| `virtual_camera` | `/system/bin/virtual_camera` | yes | no | Removed. It is `disabled`, `oneshot`, and declares `android.hardware.camera.provider.ICameraProvider/virtual/0`; no physical dependency is established. |
| `surfaceflinger` | `/system/bin/surfaceflinger` | yes | no | Removed. This is Android display/compositor ownership, not the retained physical camera HAL. |
| `vendor.hwcomposer` | `/vendor/bin/hw/android.hardware.graphics.composer@3.3-service` | yes | no | Removed with SurfaceFlinger/HWC desktop ownership; its rc restarts SurfaceFlinger. |
| `gpu` | `/system/bin/gpuservice` | yes | no | Removed Android GPU service startup; no camera Binder interface in the rc. |
| `vendor.mali_platform-mediatek` | `/vendor/bin/hw/arm.mali.platform-service.mediatek` | yes | no | Removed GPU platform service startup; camera relevance is indirect and unproven. |
| `zygote` | `/system/bin/app_process64 ... --zygote` | yes | no effective service | `init.zygote64.rc` is empty in mutable. The application/Java framework lifecycle and its Camera2 client machinery are therefore not retained as a startup path. |
| `zygote_secondary` | `/system/bin/app_process32 ...` | yes | no | Removed from `init.zygote64_32.rc`. |

Removing zygote also removes the stock zygote restart actions for
`cameraserver` and `media` found in the immutable 64-bit zygote definition.
That is a lifecycle/policy difference, not evidence that the physical camera
services themselves were removed.

## Media/codec-related init diff

### Retained media and codec declarations

| Service | Binary | Immutable | Mutable | Ownership / role |
|---|---|---:|---:|---|
| `media` | `/system_ext/bin/mediahelper` | yes | yes | AOSP/framework media helper by path/name inference; groups `audio camera ... mediadrm`; not a Camera2 surface proof. |
| `media32` | `/system/bin/mediaserver32` | yes | yes | Legacy/framework media process declaration; `disabled`; no stock start is implied. |
| `media64` | `/system/bin/mediaserver64` | yes | yes | Legacy/framework media process declaration; `disabled`; no stock start is implied. |
| `mediaextractor` | `/system/bin/mediaextractor` | yes | yes | AOSP media extraction service by path/name inference; user `mediaex`, groups `drmrpc mediadrm`. Useful for container/media parsing, not proven camera-buffer plumbing. |
| `mediametrics` | `/system/bin/mediametrics` | yes | yes | AOSP media telemetry service by path/name inference; no buffer or codec interface in rc. |
| `android-hardware-media-c2-hal` | `/vendor/bin/hw/android.hardware.media.c2-mediatek` | yes | yes | MediaTek Codec2 vendor HAL; user `mediacodec`, groups include `camera`, `mediadrm`, `media`, `media_rw`, `sdcard_rw`, audio/network groups. Highest-value retained codec executable. |
| `vendor-dolby-media-c2-hal` | `/vendor/bin/hw/vendor.dolby.media.c2-default-service-dax` | yes | yes | MediaTek/third-party Dolby Codec2 component; user `mediacodec`, groups `camera mediadrm drmrpc`; likely codec/effect policy or integration, but exact role is unknown from rc. |
| `hidl_memory` | `/system/system_ext/bin/hw/android.hidl.allocator@1.0-service` | yes | yes | Generic AOSP HIDL memory allocator; user/group `system`; retained but not shown by init text to be the camera allocator used by the current path. |

There is no immutable-only `MediaCodec`, `MediaRecorder`, BufferQueue, or
mapper service declaration. Those are framework/library and native-window
contracts rather than individually named init services in this corpus.
The mapper is loaded as a library by the allocator/camera path; the current
implementation records `/vendor/lib64/hw/mapper.<suffix>.so` and API 5, but
the exact stock suffix is a runtime value rather than an init declaration.

### Media definitions absent from the current corpus

No camera/media service with an actual media or codec role was found to be
immutable-only. The immutable-only service-like definitions in this area are
the display/GPU and virtual-camera entries above. Therefore it would be
incorrect to report `mediaserver`, `mediaextractor`, `mediametrics`, or the
MediaTek C2 HAL as deliberately omitted: their init definitions are retained.

## Stock Camera2 -> Camera3 layers

The stock application-facing chain, as recorded by the repository's stock
video archaeology, is approximately:

```text
Camera2 app
  -> CameraDevice / CameraDeviceImpl
  -> SessionConfiguration / OutputConfiguration / Surface objects
  -> cameraserver / CameraService / Camera3Device
  -> Camera3 provider and device session
  -> camerahalserver
```

The init diff confirms only the `cameraserver` process declaration; it does
not enumerate its internal classes or libraries. Existing repository evidence
confirms that stock VIDEO reached the HAL with one 1920x1080
`IMPLEMENTATION_DEFINED` stream and `GPU_TEXTURE` usage, while the current
daemon instead submits one Camera3 `YCBCR_420_888` stream with
`CPU_READ_OFTEN`.

Likely useful omitted *framework layers* are therefore inside the absent
zygote/application/framework runtime and the retained `/system/bin/cameraserver`
process, not in a missing camera init service:

- Camera2 client/session and `CameraDeviceClient` policy: absent as a native
  Linux application path; internal cameraserver implementation is retained.
- `Camera3Device` stream/session translation: likely inside cameraserver and
  its framework camera libraries; exact library ownership is unknown from rc.
- `OutputConfiguration` and deferred-surface handling: likely framework
  `Surface`/native-window plumbing; no init declaration exists.
- BufferQueue consumer/producer setup and usage derivation: likely framework
  graphics/native-window plus gralloc; SurfaceFlinger itself is omitted, but
  omission of SurfaceFlinger does not prove BufferQueue libraries are absent.

The current daemon bypasses Camera2, Java lifecycle, CameraService client
policy, and Surface/OutputConfiguration management. It directly opens the
retained provider and Camera3 device session, allocates through the retained
allocator, imports/locks through mapper, and exports frames through the
project V4L2 bridge.

## Stock recording/codec layers

The stock recording observation establishes `MediaRecorder` configuration,
an input `Surface`, AVC at 1920x1080/30, and a surface-based app topology. It
does not establish a second Camera3 stream, a same-FD camera-to-venc handoff,
the concrete gralloc format, or the encoder queue format.

The likely chain is:

```text
MediaRecorder or MediaCodec API
  -> framework recorder/codec and Surface/BufferQueue producer
  -> MediaCodec/Codec2 or MediaTek codec glue
  -> gralloc/native handles and fences
  -> MediaTek venc userspace/driver
  -> /dev/video1 (mtk-vcodec-enc) or an intermediate vendor path
```

The current corpus retains the MediaTek C2 service and the generic media
helpers. The live codec census confirms `/dev/video1` advertises standard
`YU12`, `YV12`, `NV12`, `NV21` and MediaTek `NM12`, `NM21`, `YM12`, `YM21`
input families plus H.264 output, but streaming stopped at `EIO`; this is
format capability evidence, not a working stock recorder path.

## Components deliberately omitted

| Omitted role | Exact init evidence | What it removed | Confidence |
|---|---|---|---|
| Android application/Java startup | empty mutable `init.zygote64.rc`; secondary zygote removed | Camera2 app lifecycle and framework client execution | confirmed from corpus |
| Android desktop compositor | `surfaceflinger.rc` absent; `vendor.hwcomposer` absent | SurfaceFlinger/HWC display policy and desktop ownership | confirmed from corpus |
| Android GPU service startup | `gpuservice.rc` and Mali platform rc absent | Android GPU service policy/startup | confirmed from corpus |
| Virtual camera provider | `virtual_camera.hal.rc` absent | disabled/oneshot virtual provider | confirmed from corpus |
| Framework Surface/BufferQueue/MediaRecorder internals | no service declaration to compare | not an init-level omission; current path bypasses it | unknown at binary level |
| Stock recorder-to-venc topology | no init declaration identifies one | not attributable to a single omitted service | unknown |

## Policy vs useful plumbing

Policy and lifecycle material that can normally be ignored for the current
Linux bridge includes Android application startup, Camera2 permission/AppOps,
package lifecycle, camera availability/multi-client policy, zygote restart
policy, SurfaceFlinger desktop ownership, and the disabled virtual-camera
provider. The corpus does not expose each policy implementation as a separate
binary, so the assignment is architectural rather than an init-file claim.

Potentially reusable plumbing is different:

- Camera2-to-Camera3 stream and session translation, especially
  `Camera3Device` and `CameraDeviceClient`.
- `OutputConfiguration` deferred-surface and BufferQueue/native-window
  attachment.
- gralloc usage selection for `IMPLEMENTATION_DEFINED` and
  `VIDEO_ENCODER`/`GPU_TEXTURE` buffers.
- native-handle, DMA-BUF, acquire-fence, and release-fence ownership.
- MediaRecorder/MediaCodec input-surface creation and Codec2-to-MediaTek
  encoder configuration.

These should be treated as executable documentation. Reusing them does not
justify restoring the full Android framework.

## Current replacements and bypasses

| Stock role | Current replacement or status |
|---|---|
| Camera2 application API | Absent from the native runtime path; app-facing access is project bridge/API plus V4L2. |
| Camera2 client/session policy | Bypassed; `tb330fu-camera-daemon` opens Camera3 provider/device interfaces directly. |
| Camera3 stream setup | The main project stream requests stock-compatible `IMPLEMENTATION_DEFINED`/`GPU_TEXTURE` and consumes the returned HAL format/usage; the still stream uses `YCBCR_420_888` with `CPU_READ_OFTEN`. |
| CameraService/provider startup | Retained `cameraserver`, `camerahalserver`, allocator, mapper, Binder/property prerequisites; daemon waits for provider publication and does not start it. |
| Surface/BufferQueue recorder path | Not implemented in the current Linux bridge; no claim of recorder-surface attachment. |
| Android MediaRecorder/MediaCodec recording | Not reproduced by the Linux bridge; native recording is provided separately by the project recorder over the daemon's shared-memory ring. |
| Camera output | Native daemon -> mapper lock/copy -> NV21-to-I420 conversion -> V4L2 loopback -> `/dev/video-rear`/`/dev/video-front`. |
| First-party control API | Project bridge and daemon control plane; current daemon owns camera selection, mode, controls, Camera3 observation, and capture lifecycle. |
| Android permissions/AppOps and multi-client policy | Intentionally absent from the native Linux-facing interface. |

## Runtime implementation-library closure

This closure is derived from tablet `readelf -dW`, dynamic symbols, strings,
Binder registrations, and `/proc/<pid>/maps`. It is the implementation closure
for the selected services, not a claim that every transitive libc/VNDK support
library is individually listed.

### Camera service and HAL

`/system/bin/cameraserver` directly needs the Android camera/framework side:

```text
/system/lib64/libcamerapandora.so
/system/lib64/libcamera_client.so
/system/lib64/libcamera_metadata.so
/system/lib64/libnativewindow.so
/system/lib64/libgui.so
/system/lib64/libstagefright.so
/system/lib64/libstagefright_foundation.so
/system/lib64/libmedia.so
/system/lib64/libmedia_omx.so
/system/lib64/libmedia_codeclist.so
/system/lib64/libfmq.so
/system/lib64/libbinder.so
/system/lib64/libbinder_ndk.so
/system/lib64/android.hardware.camera.device@3.3.so
/system/lib64/android.hardware.camera.device@3.5.so
/system/lib64/android.hardware.camera.device@3.6.so
/system/lib64/android.hardware.camera.device@3.7.so
/system/lib64/android.hardware.common-V2-ndk.so
/system/lib64/android.hardware.common.fmq-V1-ndk.so
```

Its dynamic symbols explicitly reference `BnCameraService`, `CameraMetadata`,
`CameraDeviceUser`, `SessionConfiguration`, `OutputConfiguration`, vendor-tag
descriptors, `MediaPlayer`, `AppOpsManager`, `SensorPrivacyManager`, and
`ActivityManager`. This is direct proof that the retained cameraserver binary
contains the Camera2 client/session and policy boundary relevant to the task;
it is not merely a generic provider launcher.

`/vendor/bin/hw/camerahalserver` directly needs the MediaTek provider/HAL
implementation:

```text
/vendor/lib64/libmtkcam_hal_aidl_provider.so
/vendor/lib64/libmtkcam_hal_aidl_device.so
/vendor/lib64/libmtkcam_hal_aidl_common.so
/vendor/lib64/libmtkcam_hal_aidl_utils.so
/vendor/lib64/libmtkcam_device3_app.so
/vendor/lib64/libmtkcam_device3_hal.so
/vendor/lib64/libmtkcam_grallocutils.so
/vendor/lib64/libmtkcam_modulehelper.so
/vendor/lib64/libmtkcam_stdutils.so
/vendor/lib64/libmtkcam_scenariorecorder.so
/vendor/lib64/vendor.mediatek.hardware.camera.isphal_aidl@1.0-impl.so
/vendor/lib64/vendor.mediatek.hardware.camera.bgservice_aidl@1.0-impl.so
/vendor/lib64/vendor.mediatek.hardware.camera.postproc_aidl@1.0-impl.so
/vendor/lib64/vendor.mediatek.hardware.camera.atms_aidl@1.0-impl.so
/vendor/lib64/vendor.mediatek.hardware.camera.frhandler@1.0.so
/vendor/lib64/android.hardware.camera.provider@2.6-impl-mediatek.so
/vendor/lib64/android.hardware.camera.provider-V3-ndk.so
/vendor/lib64/android.hardware.camera.device-V3-ndk.so
```

The HAL binary's symbols register `ICameraProvider`, `IISPModule`,
`IPostDevice`, `IBGService`, and `IATMs`, and call
`NSCam::IGrallocHelper::singleton()`. The provider/device libraries export
`AidlCameraProvider`, `AidlCameraDevice`, `AidlCameraDeviceSession`,
`configureStreams`, `configureStreamsV2`, `processCaptureRequest`, FMQ access,
buffer-handle cache conversion, stream-buffer conversion, acquire/release
fence handling, and request/result metadata conversion. This is the strongest
implementation evidence for the physical Camera3 side.

### Allocator, mapper, and native-buffer closure

The selected allocator and live mapper use this implementation cluster:

```text
/vendor/lib64/hw/android.hardware.graphics.allocator-V2-mediatek.so
/vendor/lib64/hw/mapper.mediatek.so
/vendor/lib64/libgralloc_extra.so
/vendor/lib64/libgralloc_metadata.so
/vendor/lib64/libgralloctypes_mtk.so
/vendor/lib64/libdmabufheap.so
/vendor/lib64/libged.so
/vendor/lib64/libgpud.so
/vendor/lib64/libion.so
/vendor/lib64/libnativewindow.so
/vendor/lib64/libdrm.so
/vendor/lib64/libsync.so
/vendor/lib64/arm.graphics-V5-ndk.so
```

Allocator symbols include `allocate2`, `isSupported`,
`getIMapperLibrarySuffix`, `mali_gralloc_select_format`,
`mali_gralloc_derive_format_and_size`, `mali_gralloc_lock_ycbcr`,
`make_private_handle`, `drm_fourcc_from_handle`, and DMA-BUF heap allocation.
Mapper symbols include `AIMapper_loadIMapper`, `importBuffer`, `freeBuffer`,
`lock`, `unlock`, `getStandardMetadata`, `get_plane_layouts`, and transport
size handling. Strings explicitly reference `VIDEO_ENCODER`-relevant format
selection, usage validation, AFBC/AFRC, DRM fourcc, plane layouts, ION/DMA-BUF
heap allocation, and fence/buffer synchronization.

This closes the earlier attribution gap: the allocator/mapper is not an
abstract Android prerequisite. The selected MediaTek/Mali implementation
contains the concrete usage-to-format, native-handle, plane-layout, and
DMA-BUF mechanisms that must be traced for `IMPLEMENTATION_DEFINED`.

### Codec2 and venc closure

The selected MediaTek C2 service directly needs:

```text
/vendor/lib64/libcodec2_mtk_c2store.so
/vendor/lib64/libcodec2_mtk_venc.so
/vendor/lib64/libcodec2_mtk_vdec.so
/vendor/lib64/libcodec2.so
/vendor/lib64/libcodec2_vndk.so
/vendor/lib64/libcodec2_aidl.so
/vendor/lib64/libcodec2_hal_common.so
/vendor/lib64/libcodec2_hidl@1.0.so
/vendor/lib64/libcodec2_hidl@1.1.so
/vendor/lib64/libcodec2_hidl@1.2.so
/vendor/lib64/libdmabufheap.so
/vendor/lib64/libion.so
/vendor/lib64/libion_mtk.so
/vendor/lib64/libstagefright_foundation.so
/vendor/lib64/libsfplugin_ccodec_utils.so
```

`libcodec2_mtk_c2store.so` exports `C2MtkComponentStore`, component
enumeration/creation, `copyBuffer`, and separate camera-record,
decoder, encoder, secure-decoder, and secure-encoder heap/usage logging. The
service exports and registers the AIDL/HIDL `IComponentStore` implementations.

`libcodec2_mtk_venc.so` exports `CreateCodec2AvcFactory`, HEVC, H.263, MPEG-4,
HEIF, secure factories, and the generic factory. Its strings identify
`VCodecV4L2Enc`, `VCodecProprietaryEnc`, `VIDIOC_S_SELECTION`, input/output
format setup, queue/dequeue, gralloc buffer width/height/stride/color-format
queries, acquire/release fences, DMA-BUF-like plane FDs, and:

```text
V4L2_PIX_FMT_NV12
V4L2_PIX_FMT_NV21
V4L2_PIX_FMT_YUV420
V4L2_PIX_FMT_YUV420M
V4L2_PIX_FMT_YVU420
V4L2_PIX_FMT_MT21CS*
V4L2_PIX_FMT_P010S
```

It also references `/vendor/lib64/libvcodecdrv.so`, `/dev/video`,
`/dev/vdec-fmt`, and `/data/vendor/vcodec/{venc_input,venc_output,...}`.
This is direct implementation evidence that the C2-to-venc target is
`libcodec2_mtk_venc.so` plus `libvcodecdrv.so`, not only the C2 service binary.

The Dolby service resolves to `/vendor/lib64/libcodec2_store_dolby.so` and
exports `GetCodec2DolbyComponentStore`/Dolby component-store registration.
It is a secondary target until a runtime component query or call path shows it
participates in AVC recording.

### Media/framework surface closure

The live `cameraserver` maps the framework surface/codec closure, including:

```text
/system/lib64/libgui.so
/system/lib64/libnativewindow.so
/system/lib64/libstagefright_graphicbuffersource_aidl.so
/system/lib64/libstagefright_bufferqueue_helper.so
/system/lib64/libstagefright_surface_utils.so
/system/lib64/libstagefright_aidl_bufferpool2.so
/system/lib64/libstagefright_bufferpool@2.0.1.so
/system/lib64/libcodec2_client.so
/system/lib64/libcodec2_aidl_client.so
/system/lib64/libcodec2_hidl_client@1.2.so
/system/lib64/android.hardware.graphics.bufferqueue@1.0.so
/system/lib64/android.hardware.graphics.bufferqueue@2.0.so
```

The live C2 processes additionally map the corresponding `/vendor/lib64`
bufferqueue, bufferpool, mapper, nativewindow, and C2 libraries. Thus
BufferQueue/native-window plumbing is present in the selected runtime even
though SurfaceFlinger itself is not running. The correct conclusion is that
SurfaceFlinger desktop ownership was omitted, not that BufferQueue plumbing
was omitted.

### Selected media executables and current daemon

The selected media helper's direct dependencies are limited to libcutils/log
and the C++ runtime; its strings name `/system/bin/mediaserver32`,
`/system/bin/mediaserver64`, and the fallback `/system/bin/mediaserver`.
`mediaextractor` directly loads `libmedia.so`,
`libmediaextractorservice.so`, and `libmediautils.so`; `mediametrics` directly
loads `libmediametricsservice.so`, `libmediametricsservice-aidl-cpp.so`, and
the package-manager AIDL support. The disabled `mediaserver32`/`64` binaries
directly load `libmediaplayerservice.so` and
`libresourcemanagerservice.so`, but neither init record is selected. The
running media process therefore proves the helper's handoff and media Binder
registrations, while the exact helper-selected server artifact is not
reconstructed as a separate init start.

The current `/usr/local/lib/tb330fu/tb330fu-camera-daemon` directly needs
Binder-NDK, camera metadata/FMQ, cutils/base/log/utils, libpng, zlib, and the
dynamic-loader libraries. Its symbols and strings resolve
`ICameraProvider/internal/0`, `IAllocator/default`, `AIMapper_loadIMapper`,
`/vendor/lib64/hw/mapper.%s.so`, Camera3 `configureStreams` and
`processCaptureRequest`, `YCBCR_420_888`, DMABUF, and V4L2 output. This is the
current consumer edge into the retained provider/allocator/mapper closure;
the daemon has no direct C2 dependency.

## Ghidra target shortlist

The previous init-executable-only list is superseded by this implementation
closure list. Reverse the smallest set that answers the requested data-flow
questions:

| Priority | Target | Why this exact target matters |
|---|---|---|
| High | `/vendor/lib64/libmtkcam_hal_aidl_device.so` | `AidlCameraDeviceSession::configureStreams`, `configureStreamsV2`, `processCaptureRequest`, FMQ, buffer-handle cache, stream-buffer conversion, and fence ownership. Answers the Camera3 side of A, C, and F. |
| High | `/vendor/lib64/libmtkcam_grallocutils.so` | Camera-side GrallocHelper, stream usage/format translation, allocation metadata, native-handle and plane-layout interpretation. Answers B, C, and F. |
| High | `/vendor/lib64/hw/android.hardware.graphics.allocator-V2-mediatek.so` | `mali_gralloc_select_format`, usage validation, allocation dimensions, DRM fourcc, DMA-BUF heap and private-handle construction. Answers B and C. |
| High | `/vendor/lib64/hw/mapper.mediatek.so` | `AIMapper_loadIMapper`, import/lock/unlock, `get_plane_layouts`, metadata, transport size, and fence behavior. Answers C and F. |
| High | `/vendor/lib64/libcodec2_mtk_venc.so` | C2 encoder factories, V4L2/proprietary backend selection, concrete input fourccs, gralloc buffer queries, plane FDs, queue controls, and fence flow. Answers D, E, and F. |
| High | `/vendor/lib64/libcodec2_mtk_c2store.so` | Component discovery/selection, camera-record heap/usage policy, encoder factory loading, and C2 buffer copying. Answers D and the usage policy portion of B. |
| High | `/vendor/lib64/libvcodecdrv.so` | Referenced by the encoder implementation; likely the narrow driver glue below C2. Resolve V4L2 node, queue, control, and DMABUF operations. Answers D and E. |
| Medium | `/vendor/lib64/libmtkcam_hal_aidl_provider.so` | Provider/device creation, camera ID discovery, vendor tags, and provider-to-device ownership. Answers the provider half of A. |
| Medium | `/system/bin/cameraserver` plus `/system/lib64/libcamera_client.so`, `libgui.so`, `libnativewindow.so`, and `libstagefright_graphicbuffersource_aidl.so` | Camera2 `OutputConfiguration`/Surface policy, Camera3 client translation, BufferQueue/native-window attachment, and MediaCodec-facing surface creation. Answers A and the framework half of C/D. |
| Low | `/vendor/lib64/libcodec2_store_dolby.so` | Determine whether Dolby is on the ordinary AVC path or a separate component store. |

The exact mapper and library paths above are confirmed by the live mapped
objects and current daemon strings. This target list is therefore based on
implementation ownership and live load edges, not on init names alone.

## Unknowns

- The init corpus does not identify internal shared-library ownership; the
  tablet ELF/map pass identifies the selected implementation closure, but not
  every transitive system/VNDK support library.
- No init declaration proves that SurfaceFlinger is required for the retained
  physical camera HAL; its omission is a display-ownership decision.
- The exact stock recorder-surface attach operation remains unknown.
- The concrete native allocation behind stock `IMPLEMENTATION_DEFINED` /
  `GPU_TEXTURE` remains unknown.
- The stock venc input fourcc, plane layout, queue memory model, controls,
  profile/level, and fence contract remain unknown.
- The live encoder's `EIO` boundary is unresolved; no successful hardware
  encode has been promoted.
- The init diff cannot attribute implementation code inside a retained binary
  to AOSP, MediaTek, Lenovo, or project-native ownership beyond path/name and
  interface evidence.

## Proposed Ghidra follow-up scope

Reverse the implementation closure in this order:

1. `libmtkcam_hal_aidl_device.so` and `libmtkcam_grallocutils.so`, correlating
   Camera3 stream descriptors, usage, native handles, plane layouts, and
   acquire/release fences.
2. `android.hardware.graphics.allocator-V2-mediatek.so` and
   `mapper.mediatek.so`, resolving `IMPLEMENTATION_DEFINED` and
   `VIDEO_ENCODER`/`GPU_TEXTURE` to concrete allocation metadata and DMA-BUF
   ownership.
3. `libcodec2_mtk_c2store.so`, `libcodec2_mtk_venc.so`, and
   `libvcodecdrv.so`, identifying component selection, venc fourcc/plane
   layout, queue mode, controls, and fence transfer.
4. `cameraserver` with its mapped Camera2/native-window libraries, then the
   provider library, only where the call path is needed to connect a recorder
   surface to the Camera3 stream.
5. `libcodec2_store_dolby.so` only if component enumeration or call evidence
   places it on the ordinary AVC recording path.

Success means a bounded call/data-flow map answers A-F from the task: the
Camera2 recording surface becomes a specific Camera3 stream; usage bits select
a specific allocation; `IMPLEMENTATION_DEFINED` resolves to a concrete
format/layout; the recorder/codec reaches a named MTK component; venc's
fourcc and queue contract are identified; and DMA-BUF handles/fences have an
identified owner at each transfer. No service restoration or runtime change is
part of that follow-up.
