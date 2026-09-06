# Stock Lenovo video path archaeology

## Executive summary

The strongest evidence shows that the Lenovo app uses Android Camera2/framework APIs, not a direct Camera3 client. In stock VIDEO mode, the native HAL boundary received one 1920x1080 `OUTPUT` stream using Android format 34 (`IMPLEMENTATION_DEFINED`), usage `GPU_TEXTURE` (256), `UNKNOWN` dataspace, rotation 0, standard dynamic-range profile 1, and normal operation mode. PHOTO received three outputs instead.

The app configured `MediaRecorder` for 1920x1080 at 30 fps with AVC, obtained its input `Surface`, and started recording after the VIDEO preview session was already active. The captured Java trace did not observe a second encoder stream, session recreation, output update, or output finalization. Whether the recorder surface was attached dynamically was not captured. Therefore a direct Camera3-to-venc DMA-BUF handoff is not established.

The intimate native-format relationship remains most likely distributed across Camera2/framework surface plumbing, gralloc/BufferQueue, the camera HAL, and MediaCodec/MediaRecorder's MediaTek codec path. The historical material does not contain a stock venc queue negotiation, concrete gralloc allocation description, or same-FD trace.

## Evidence sources inspected

Historical evidence was read from the explicitly authorized old tree:

- `native-boot/android-init-island/audits/stock-camera-configurestreams-frida-20260831/video-stream-configuration.json`
- `.../photo-vs-video-diff.json` and `video-session-metadata.json`
- `.../audits/stock-camera-session-frida-20260831/STOCK-VS-NATIVE-SESSION.md`
- `.../video-recording-session.json`, `session-diffs.json`, and `camera-session-full.log`
- `.../audits/stock-camera-frida-20260831/camera2-state-diffs.json`
- `.../audits/camera-stock-video-policy-20260831/CAMERA-STOCK-VIDEO-POLICY.md` and its request/result evidence
- `migration-recon/capture/hardware/video-media.txt`
- Decompiled `ZuiCamera.apk` sources under `.../audits/zuicamera-ai-recon-20260901/work/jadx/sources/`

The current implementation was checked only in the canonical release tree, especially `docs/CAMERA.md`, `src/camera/daemon/tb330fu-camera-daemon.cpp`, and `src/camera/capture/tb330fu-camera-capture.cpp`.

## Stock app / Camera2 layering

**Confirmed:** the stock app uses Camera2. Decompiled code calls `CameraDevice.createCaptureSession`, `SessionConfiguration`, `OutputConfiguration`, and `CaptureRequest`; the Java trace observed `CameraDeviceImpl.createCaptureSession` and Camera2 session callbacks. The app therefore supplies framework-level surfaces and requests; framework/camera-service glue translates these into Camera3/HAL parcels.

**Confirmed:** the app has a `SurfaceManager` with `PREVIEW`, `VIDEO`, YUV, JPEG, and RAW surface roles. `BaseVideoModule` creates a `MediaRecorderWrapper`, obtains a video surface, and passes it to `MediaRecorder.setInputSurface`. The app also has a `MEDIA_CODEC` mode and a separate motion-video GL/MediaCodec implementation, but those are not proof of the ordinary stock VIDEO path used in the captured run.

**Unknown:** the exact framework operation that attaches the recorder surface. `OutputConfiguration.addSurface/removeSurface` was not hooked, and no `updateOutputConfiguration` or `finalizeOutputConfigurations` event was observed.

## Camera3 stream topology

Native HAL `configureStreams` capture for completed VIDEO action:

| Field | Observed value |
|---|---|
| stream count | 1 |
| stream ID | 0 |
| direction | `OUTPUT` |
| size | 1920x1080 |
| format | 34 / `IMPLEMENTATION_DEFINED` |
| usage | 256 / `GPU_TEXTURE` |
| dataspace | 0 / `UNKNOWN` |
| rotation | `ROTATION_0` |
| operation mode | 0 / `NORMAL_MODE` |
| dynamic range | 1 / `STANDARD` |
| stream use case | 0 / `DEFAULT` |
| input configuration | null at Java session boundary |

The native session parameters were five entries: MediaTek `zsl.mode` BYTE 1, MediaTek `proprietaryRequest` INT32 1, Lenovo `com.lenovo.camera.mode.value` INT32 2, standard `videoStabilizationMode` BYTE 0, and MediaTek `initrequest` INT32 1.

PHOTO was independently observed with three output streams: a 1600x1200 `IMPLEMENTATION_DEFINED`/GPU-texture preview, plus 3264x2448 YUV format 35 and JPEG/BLOB format 33 outputs. Thus stock VIDEO is not the PHOTO three-output topology.

**Confirmed:** stock VIDEO used `IMPLEMENTATION_DEFINED`, not `YCBCR_420_888`, YV12, or an explicitly named YUV format at the HAL boundary.

**Unknown:** whether recording ultimately caused a second stream to be attached after the captured configuration. The Java trace saw one preview `SurfaceTexture` output at session creation and no later configuration event. This supports “one observed configured stream,” not an unconditional claim that no dynamic attachment ever occurred.

## Buffer formats and gralloc usage

**Confirmed:** the Camera3 stream's requested/transport-level format was Android 34 and its observed stream usage was `GPU_TEXTURE`. Android format 34 does not identify the concrete allocated memory layout.

**Unknown:** the resolved native pixel format, allocation usage bits beyond the stream value, stride, modifier, plane layout, native-handle metadata, and whether buffers were linear, tiled, compressed, or vendor-specific. No captured allocator/mapper record resolves these fields for the stock VIDEO allocation.

The old property harvest records generic gralloc and EGL format capabilities, including recordable NV12/NV21/YUV420 entries, but those are capability/configuration evidence, not proof of the stock VIDEO allocation selected for this session.

## MediaTek venc configuration

**Confirmed only:** the stock device has a MediaTek V4L2 encoder at `/dev/video1` (`mtk-vcodec-enc`), with the associated VENC platform/IOMMU components present. The decompiled ordinary recording path configures Android `MediaRecorder` with video source 2, container/output format 2, AVC encoder 2, 1920x1080, and 30 fps. The runtime trace observed the same calls, followed by `prepare`, `getSurface`, `start`, `stop`, and `release`.

**Not found in captured evidence:** venc OUTPUT queue format, CAPTURE encoded format, selected input pixel format, plane count, stride, sizeimage, DMABUF queue mode, codec profile/level, bitrate, controls, or frame interval at the V4L2 encoder boundary. Consequently the exact stock venc input/output formats are unknown. The observation that an FFmpeg probe can negotiate an undesirable proprietary format is not sufficient to identify the stock format.

## Buffer handoff evidence

The recorder receives a `Surface`, not a captured V4L2 buffer FD. The Java trace does not expose the producer queue's native format or FDs, and no stock trace correlates a Camera3 buffer handle FD with a venc queue FD.

Classification of direct Camera3-to-venc DMA-BUF handoff: **unsupported by captured evidence**. It is technically plausible because `IMPLEMENTATION_DEFINED` surface buffers and MediaCodec commonly use native gralloc/BufferQueue allocations, but that is an inference, not a stock-specific result. CPU conversion, GPU composition, or an intermediate codec component cannot be excluded.

## VIDEO metadata and vendor tags

Material VIDEO differences from PHOTO that may affect behavior:

| Metadata | Type | Observed VIDEO value | Interpretation |
|---|---|---:|---|
| `android.control.aeTargetFpsRange` | standard | `[30,30]` | fixed video cadence policy; PHOTO was `[5,30]` |
| `android.control.afMode` | standard | `3` | video AF policy; PHOTO was `4` |
| `android.control.captureIntent` | standard | `3` / VIDEO_RECORD | standard request intent |
| `android.control.enableZsl` | standard | request traces varied: false in the native session capture; true in the stock request oracle | must not be normalized without matching the exact trace window |
| `android.control.videoStabilizationMode` | standard | `0` | stabilization off in the captured session parameters |
| `com.lenovo.camera.mode.value` | Lenovo vendor | `2` | VIDEO mode selector in session parameters and direct Builder.set observation |
| `com.mediatek.control.capture.zsl.mode` | MediaTek vendor | BYTE `1` | retained vendor session policy; semantic effect not independently proven |
| `com.mediatek.configure.setting.proprietaryRequest` | MediaTek vendor | INT32 `1` | retained vendor configure policy; semantic effect not independently proven |
| `com.mediatek.configure.setting.initrequest` | MediaTek vendor | INT32 `1` | retained vendor configure policy; semantic effect not independently proven |

The stock VIDEO request oracle also showed approximately 29.993 ms exposure, ISO 4000, and 33.35 ms frame duration. The recorded VIDEO-to-recording transition changed only small color-correction/focus-result values in the representative snapshots; no distinct final recording request was submitted at the marker.

## Exposure-control finding

The exact previously identified knob is `com.lenovo.camera.mode.value`, tag `0x90010000`, type `TYPE_INT32`, value `2` for VIDEO. It was observed in the stock native `configureStreams` session-parameter blob and in a direct stock Camera2 `Builder.set` observation during PHOTO-to-VIDEO transition.

The stock request oracle produced approximately ISO 4000 and 29.993 ms exposure with this VIDEO policy. A native A/B that submitted the five-entry session policy, including mode 2, was accepted and produced frames, but remained dark at 230 ISO and 30.006 ms exposure. Thus mode 2 is part of the stock policy, but mode 2 alone is not a sufficient reproduction of stock exposure.

The evidence cannot distinguish whether the app itself set the final consumed vendor value or whether framework/HAL code inserted or transformed it. The direct `Builder.set` observation proves app-visible setting activity, not final HAL ownership.

## Decompiled app findings

The ordinary `BaseVideoModule` path uses Android `MediaRecorder`, sets the video profile/size/rate/encoder, obtains a recorder input surface, and supplies that surface through the app's `SurfaceManager`. The app also renders through `SurfaceTexture` and has GL watermark/rendering support. `MotionVideoRecorder` separately creates an AVC `MediaCodec` input surface and renders into it, but this is a motion-video feature and should not be conflated with the ordinary recording trace.

This code supports a surface-based Camera2 topology. It does not reveal the concrete Camera3 native format, gralloc layout, venc V4L2 format, or whether the ordinary recorder surface was attached as a second HAL stream.

## Where the camera-venc relationship likely lives

**Strong evidence:** app-level recording policy and surface creation live in the stock app/framework-facing layer; Camera3 stream translation and vendor session policy cross the Camera2/cameraservice/HAL boundary; concrete allocation selection belongs to gralloc/allocator plus the camera HAL; codec configuration belongs to MediaRecorder/MediaCodec and the MediaTek codec userspace/driver stack.

**Architectural conclusion:** the relationship is a combination of these layers, with the native format/buffer contract most likely in framework BufferQueue/gralloc, camera HAL, and MediaTek codec components rather than in the Flutter-facing app. The captured material cannot assign the direct handoff to one component more precisely.

## Comparison with the current Linux path

The current canonical V4L2 preview path requests stock-compatible
`IMPLEMENTATION_DEFINED` with `GPU_TEXTURE` for its main stream, consumes the
returned HAL format/usage, imports and locks mapper buffers, observes a
contiguous Y-plus-interleaved-VU layout, converts NV21-style data to planar
I420/YU12 in userspace, and submits it through V4L2 loopback. The separate
still stream is CPU-readable. A native recorder consumes the daemon's shared
ring and is not a reproduction of the stock Android recorder-surface path.

Evidence-supported opportunities are:

- **First-party recording:** a separate Android/native recorder path could preserve stock Camera2/MediaRecorder surface semantics, but the native buffer/venc contract must first be measured.
- **Cheaper preview:** a surface/native path may avoid the current mapper lock and NV21-to-I420 conversion, but no captured stock allocation layout proves it is usable for the Linux V4L2 contract.
- **Shared native-buffer path:** direct or semi-direct venc use is plausible, but requires matching format, usage, modifiers, fences, and FD ownership; none are established here.

The generic V4L2 path should remain independent even if a first-party recording path is later added.

## Candidate recording architectures

| Candidate | Evidence level | Copies/conversions | Complexity | Prototype decision |
|---|---|---|---|---|
| A. Current YUV -> I420 V4L2 -> mtk-venc | Current path confirmed; stock equivalence unknown | mapper read, NV21-to-I420 conversion, loopback copy, then venc path | lowest reuse, but codec integration remains | worth prototyping as a bounded generic Linux route |
| B. `IMPLEMENTATION_DEFINED` native buffer -> mtk-venc | native format/handoff unsupported by evidence; technically plausible | potentially zero/low copy, otherwise hidden framework copies | high: format, fences, handles, and driver contract | defer until native allocation and venc traces exist |
| C. preview stream -> V4L2 plus recording stream -> native venc | preview half is plausible; second stock stream unsupported/attachment unknown | likely two producer paths; possible shared allocation | highest topology and synchronization risk | do not prototype from current evidence |
| D. Stock surface path: Camera2 -> persistent recorder surface -> MediaRecorder/MediaCodec -> container | strongly supported at app/runtime surface level | surface/BufferQueue internals unknown; no claim of zero-copy | closest to stock behavior, but Android runtime dependent | best first-party direction if the goal is stock recording |

Candidate D is the only design directly supported by the observed stock app behavior. It still does not prove direct access to the Linux mtk-venc V4L2 interface.

## Unknowns

- Concrete native format and layout selected for stock VIDEO format 34.
- Whether the recorder surface was dynamically attached to the already-created Camera2 session.
- Stock venc queue formats, controls, profile/level, bitrate, and DMABUF mode.
- Whether Camera3 and venc shared a buffer FD or passed through GPU/CPU/intermediate components.
- Exact owner of final vendor metadata insertion/translation.
- Whether alternate stock video modes use different stream topologies or native formats.

## Recommended next experiment

Run one read-only stock recording trace that hooks `OutputConfiguration.addSurface/removeSurface` and the native HAL `configureStreams`/output-update boundary while recording starts, logging stream descriptors and opaque buffer-handle FD identities without changing requests or deployment. This decisively addresses the largest topology uncertainty: whether and how the MediaRecorder surface becomes a Camera3 stream.
