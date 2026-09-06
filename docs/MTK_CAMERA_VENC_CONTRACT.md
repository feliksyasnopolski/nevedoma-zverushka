# MediaTek Camera -> Venc Native Contract

## Executive summary

This is a static analysis of the deployed AArch64 closure on 2026-09-05. It
does not modify, start, stop, or deploy any service.

The strongest result is that the ordinary AVC Codec2 component has two
distinct implementation families:

* `vcodec::v4l2::VCodecV4L2`, using `ioctl`, `mmap`, `poll`, and the V4L2
  encoder node; and
* `vcodec::proprietary::VCodecProprietaryEnc`, using the MediaTek driver
  library and its own buffer/configuration ABI.

The static evidence does not prove that the stock Camera3
`IMPLEMENTATION_DEFINED` buffer is handed directly to `/dev/video1`. It does
prove that the C2 path accepts graphic blocks, can map/copy them, understands
MediaTek and standard YUV formats, and has explicit camera-record heap policy.
Therefore the bounded experiment should begin with one-copy input into
encoder-owned buffers. Direct DMA-BUF handoff remains a later conditional
variant, not a prerequisite.

The exact stock native format is **unresolved**. The allocator contains
`FMT_IMPLEMENTATION_DEFINED`, `MTK_CAMERA_OPAQUE`, `MTK_NV12_BLK`, AFBC/AFRC,
and standard YUV families, but a stripped static pass cannot safely reduce the
1920x1080 stock usage tuple to one internal format without observing the
allocator call and returned handle.

## Targets analyzed

The conclusions below describe the deployed AArch64 closure inspected on
2026-09-05. The extracted proprietary libraries and static-analysis project
are external provenance inputs, not repository dependencies.

| Target | Static result |
|---|---|
| `libmtkcam_grallocutils.so` | AIDL/HIDL allocator and mapper wrappers; camera-side allocation/import boundary |
| `android.hardware.graphics.allocator-V2-mediatek.so` | Mali/MediaTek format selection, allocation, private handle and plane-layout implementation |
| `mapper.mediatek.so` | import/free/lock/unlock, metadata and `get_plane_layouts` path |
| `libcodec2_mtk_c2store.so` | component discovery, `copyBuffer`, camera-record heap policy |
| `libcodec2_mtk_venc.so` | AVC factories, V4L2/proprietary backends, buffer conversion, V4L2 setup/control symbols and strings |
| `libvcodecdrv_v3a.so` | proprietary MediaTek driver ABI; the requested unversioned `libvcodecdrv.so` is absent from the deployed path |
| `libmtkcam_hal_aidl_device.so` | stream/request/buffer-cache/fence conversion boundary |

Deployed hashes remain artifact identity, not source provenance. They are not
required to build or operate the current Linux camera path.

## Camera-side gralloc translation

`libmtkcam_grallocutils.so` exports `GrallocAllocatorAidlV2::AllocatorAidl`,
allocator descriptor construction, AIDL/HIDL import/free, and camera-side
allocation helpers. The camera HAL exports `configureStreams`,
`configureStreamsV2`, `processCaptureRequest`, `convertCaptureRequests`,
`convertStreamBuffer`, and a buffer-handle cache manager.

Confirmed from symbols and strings:

* Android format 34 is represented internally as `FMT_IMPLEMENTATION_DEFINED`.
* `YCBCR_420_888` is checked against the actual resolved YUV format before
  lock; the library rejects a non-YUV result.
* The camera HAL converts AIDL stream and buffer descriptors into the native
  MediaTek Camera3 structures and retains a handle cache.
* Acquire/release fence fields are part of the native stream-buffer conversion
  boundary; this pass did not recover their exact field offsets or ownership
  transitions.

The observed stock tuple remains: 1920x1080, format 34,
`GPU_TEXTURE` (256). No vendor-library-only evidence shows that this tuple is
later supplemented with `VIDEO_ENCODER`; that transformation, if present,
belongs to framework/BufferQueue or a higher codec path.

## Allocator format selection

The allocator exports:

`mali_gralloc_select_format`, `mali_gralloc_derive_format_and_size`,
`mali_gralloc_adjust_dimensions`, `make_private_handle`,
`get_internal_format`, `get_format_info`, `mali_libformat_compute_allocation`,
`mali_libformat_get_layout`, `mali_libformat_get_num_planes`, and
`mali_libformat_get_mali_stride_alignment`.

The code/data strings confirm these decision inputs and outputs:

```text
requested HAL/base format + usage + dimensions
    -> internal format / modifier policy
    -> adjusted dimensions and libformat layout
    -> allocation size and per-plane description
    -> private_handle_t + vector of plane FDs
```

Confirmed format families in the deployed allocator include standard `NV12`,
`NV21`, `YU12`, `YV12`, `MTK_I420`, `MTK_YUV_PRIVATE`, `MTK_CAMERA_OPAQUE`,
`MTK_NV12_BLK`, `MTK_NV12_BLK_FCM`, 10-bit MediaTek variants, and AFBC/AFRC
variants. This is capability/implementation evidence, not proof of the
selected stock format.

The allocator explicitly validates producer/consumer AFBC support, can fall
back from unsupported AFBC/block-linear requests, computes YUV plane sizes,
and rejects invalid usage/format combinations. It also exposes
`drm_modifier_from_handle` and stores an internal format in the private
handle. The exact numeric internal-format value and modifier for stock VIDEO
were not recoverable with confidence from the stripped artifact.

## VIDEO_ENCODER usage policy

The vendor allocator contains usage validation and the C2 stack contains a
separate camera-record policy. `libcodec2_mtk_c2store.so` has a camera-record
logging path with the effective tuple:

```text
usage, flags, heapName
```

The encoder library logs `Using mm_heap for camera record output buffer` and
has `C2StoreIonUsageStruct`, `C2StoreDmaBufUsageStruct`, `setIonUsage`, and
`setDmaBufUsage` interfaces. This establishes a dedicated `mm_heap` policy
for camera-record output/allocation in the C2 path. The stripped code does
not expose the final numeric usage mask or flag word; those are **unknown**.

The allocator also names `ion_mm_heap`, `ion_sec_heap`,
`ion_sec_heap_2d_fr`, `ion_sec_heap_wfd`, and protected heaps. Ordinary AVC
recording should not use the secure variants unless the C2 secure mode is
selected.

## Native handle / plane layout

The mapper explicitly exposes `get_plane_layouts`, standard metadata access,
`importBuffer`, `lock`, `unlock`, and a registered-handle pool. The allocator
constructs a `private_handle_t` using a plane-info array of three entries and
the mapper reports a vector of plane FDs. The encoder’s diagnostic string
prints:

```text
ionfd, grallocId, size, crop, alignment padding, buffer array,
yPitch, uvPitch, planeNum, secureMode, useFmt, outAfbc
```

Thus the handle can contain one or multiple plane FDs and explicit pitches;
the exact stock handle contents are not established. Do not assume that an
FD count equal to the visible plane count, or the presence of an FD, proves
zero-copy compatibility.

The only concrete 1920x1080 layouts currently measured are the V4L2 census
layouts: one-plane `YU12/NV12/NV21` with 1920 bytes/line and sizeimage
3110400, two-plane `NM12/NM21` with 1920/1920 bytes/line and sizes
2073600/1036800, and three-plane `YM12/YM21` with 1920/960/960 and
2073600/518400/518400. These are V4L2 negotiated layouts, not proof of
gralloc layout identity.

## Codec2 camera-record policy

`C2MtkComponentStore` exports `copyBuffer` and component enumeration/creation.
The store dynamically loads `libcodec2_mtk_venc.so` and exposes ordinary
`c2.mtk.avc.encoder` plus secure/low-latency variants. The camera-record
policy is distinct from the decoder/encoder generic heap messages and selects
`mm_heap` by name in the observed logging path.

`libcodec2_mtk_venc.so` imports both graphic and linear block APIs:
`C2GraphicBlock::map`, `C2GraphicBlock::share`, `C2LinearBlock::share`,
`C2BlockPool::fetchLinearBlock`, `IsYUV420`, `IsNV12`, and `IsI420`. This is
direct evidence that the component can consume graphic input and can use a
linear/copy conversion path. It is not evidence that every graphic input is
passed as its original DMA-BUF.

## AVC encoder backend selection

The AVC factory is exported as `CreateCodec2AvcFactory`; the implementation
contains both `VCodecV4L2Enc` and `VCodecProprietaryEnc`. The V4L2 class has
`V4L2Config::setupConfigEnc`, `V4L2Device`, `EnqueInputBuffer`,
`pushOutputBuffer`, `pullOutputBuffer`, `DeviceStreamOn`, and
`DeviceStreamOff`. The proprietary class has `ProprietaryDevice`,
`QueryCapabilityEncoder`, `DeviceOpenEncoder`, and MediaTek encoder parameter
structures.

The V4L2 path imports `open`, `ioctl`, `mmap`, `poll`, `dup`, `eventfd`, and
`sync_wait`. It logs `VIDIOC_QUERYCAP`, `VIDIOC_S_FMT`, `VIDIOC_REQBUFS`,
`VIDIOC_QBUF`, `VIDIOC_DQBUF`, `VIDIOC_S_CTRL`, `VIDIOC_S_EXT_CTRLS`,
`VIDIOC_S_SELECTION`, and stream-on/off status. The proprietary path instead
resolves the versioned driver (`libvcodecdrv_v3a.so` on this tablet), uses
MediaTek encoder parameter structures, and contains its own allocation/cache
operations.

No static selector condition was strong enough to say which backend ordinary
stock AVC chooses for this device. Backend choice is therefore **unknown**;
the disposable experiment must force/observe the V4L2 component path rather
than infer it from the presence of `/dev/video1`.

## V4L2 device and format setup

`/dev/video1` is the live `mtk-vcodec-enc` node. At TRY_FMT it accepts:

| Input | Planes | 1920x1080 negotiated layout |
|---|---:|---|
| `NM12/NM21` | 2 | 1920/2073600 + 1920/1036800 |
| `YM12/YM21` | 3 | 1920/2073600 + 960/518400 + 960/518400 |
| `YU12/YV12/NV12/NV21` | 1 | 1920 / 3110400 |

Capture `H264` is one plane with `bytesperline=0`, `sizeimage=20480` at the
same TRY_FMT dimensions. The driver accepts dimensions in the reported
range, but this does not prove streaming success or that 20480 fits every
bitrate/GOP configuration.

The binary contains explicit handling for all standard inputs above and
MediaTek/block formats including `MT21CS*`; it also queries graphic-buffer
format, stride, color format, plane FDs, and fences. The exact mapping from a
specific gralloc internal format to a V4L2 fourcc is not statically resolved.

## Recovered MediaTek V4L2 AVC Contract

This section is based on actual Ghidra decompilation of the deployed
`libcodec2_mtk_venc.so`, not only symbol or string inspection. Relevant recovered
functions include `V4L2Device::DeviceOpen` (`0x2913b0`), the V4L2 device
constructor (`0x172420`), `VCodecV4L2::initDriver` (`0x1832f0`), encoder
initialization (`0x180930`), `setInputFormat` (`0x16f160`),
`setOutputFormat` (`0x16f630`), slot-count helpers, buffer query/enqueue and
dequeue methods, `DevicePoll`, `DeviceStreamOn`, `DeviceStreamOff`, and the
start/stop command helpers. `setupConfigEnc` was identified at `0x1d2480`,
but the decompiler exceeded its response buffer; conclusions below do not
treat that failed decompilation as proof.

### Device discovery and backend boundary

`DeviceOpen` constructs `/dev/video0` through `/dev/video255` and opens each
candidate with flags `0x80802` (`O_RDWR | O_NONBLOCK | O_CLOEXEC`). It issues
`VIDIOC_QUERYCAP` (`0x80685600`) and compares the returned driver name with
the constructor-supplied target string. A matching driver identity is retained
as the device FD; failure to find one is logged. An `eventfd(0, 0x80800)` is
also created for the device poll/interrupt path.

The AVC library contains both V4L2 and proprietary implementations. The V4L2
constructor receives an encoder boolean and a driver-name string, but this
static pass does not recover the ordinary AVC selector predicate or exact
target string at the factory call site. Backend selection therefore remains
**unknown**; presence of `/dev/video1` is not sufficient evidence.

### Queue and format operations

The following ioctl values are recovered from actual call sites:

| Operation | ioctl value | Recovered use |
|---|---:|---|
| `VIDIOC_QUERYCAP` | `0x80685600` | Per-node capability probe in `DeviceOpen` |
| `VIDIOC_S_FMT` | `0xc0d05605` | Separate input/output setters; returned structures are copied back |
| `VIDIOC_G_CTRL` | `0xc008561b` | Suggested input/output slot-count query |
| `VIDIOC_REQBUFS` | `0xc0145608` | Slot-count setup with type and memory fields |
| `VIDIOC_QUERYBUF` | `0xc0585609` | Input/output buffer query |
| `VIDIOC_QBUF` | `0xc058560f` | Enqueue; index/type/memory and payload/plane fields are logged |
| `VIDIOC_STREAMON` | `0x40045612` | Independently issued for the two stored queue types |
| `VIDIOC_STREAMOFF` | `0x40045613` | Independently issued for the two stored queue types |
| `VIDIOC_ENCODER_CMD` | `0xc028564d` | Start command `0`, stop command `1` |

The constructor loads two 128-bit static records with `ldr q1,[...,#0xa70]`
and `ldr q2,[...,#0xda0]`, then stores them at `this+0x2c8` and
`this+0x2d8`. The underlying bytes, not the decompiler’s `_DAT_` names, are:
`0x10aa70: 0, 0, 10, 9` and `0x10ada0: 4, 4, 1, 1` as little-endian 32-bit
words. Consequently the initial fields are recovered as:

| Device offset | Literal | Interpretation |
|---|---:|---|
| `0x2d0` | `10` | `V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE` (Linux enum interpretation) |
| `0x2d4` | `9` | `V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE` (Linux enum interpretation) |
| `0x2d8` | `4` | initial input memory mode, `V4L2_MEMORY_DMABUF` by enum interpretation |
| `0x2dc` | `4` | initial output memory mode, `V4L2_MEMORY_DMABUF` by enum interpretation |
| `0x2e0` | `1, 1` | initial two 32-bit plane/state words; exact vendor meaning remains unknown |

`setInputFormat()` independently writes type `10`, and `setOutputFormat()`
writes type `9`, before their `S_FMT` dispatch. `DeviceStreamOn(true,true)`
therefore issues `VIDIOC_STREAMON` for raw-input OUTPUT_MPLANE first and
encoded-output CAPTURE_MPLANE second. This queue mapping is recovered from
decompiled stores plus Linux enum definitions, not inferred from naming.

The memory field and plane-count state are carried into `v4l2_buffer`; for
multi-plane memory (`memory == 4`), an additional plane structure is
allocated. `QUERYBUF` returns a shared buffer plus the optional plane object,
and `QBUF` logs the returned buffer fields on failure.

`getSuggestedOutputSlotCount` queries `0x980927`, while
`getSuggestedInputSlotCount` queries `0x980928`; each falls back to 12 when
the driver query returns less than one. The common `setSlotCount(device,
type,memory,unused)` helper constructs `v4l2_requestbuffers` with
`count = 0`, `type = type`, and `memory = memory`, then issues
`VIDIOC_REQBUFS`. The reset helpers use `(0x2d0,0x2d8)` for input and
`(0x2d4,0x2dc)` for output to release the old queue, then re-request the
stored count (`0x330` or `0x334`) with the same type/memory pair.
`setInputSlotCount` and `setOutputSlotCount` store those requested counts and
invoke the corresponding virtual queue operation only while its stream is
stopped. The constructor default is therefore recovered as DMABUF, but the
effective stock allocation mode/count after all configuration remains
**unknown**; the earlier four-buffer MMAP proposal is not a recovered stock
contract.

The individual format helpers do not populate the complete format payload.
`InputFormat::updateToDriver()` sets only `format_i->type` from `0x2d0` and
dispatches the object to `setInputFormat()`; `OutputFormat::updateToDriver()`
does the same from `0x2d4`. The setters issue `S_FMT` and copy the returned
structure back. Width, height, pixel format, plane count, bytesperline,
sizeimage, field, and color fields are therefore payload fields, but their
stock values are still unknown at this static boundary. `VIDIOC_S_SELECTION`
is present in the binary, but its AVC call-site ordering and exact rectangle
are **unknown**.

### Stream, poll, drain, and EOS behavior

`DeviceStreamOn(bool in, bool out)` gates each queue independently. The first
boolean issues `VIDIOC_STREAMON` using offset `0x2d0` and records status at
`0x2c8`; the second uses offset `0x2d4` and records status at `0x2cc`. If both
queues are on, pending tasks are flushed. With the recovered type fields this
is raw-input OUTPUT_MPLANE first, followed by encoded-output CAPTURE_MPLANE.

The encoder command helpers issue `VIDIOC_ENCODER_CMD` (`0xc028564d`) with
command 0 for start and 1 for stop, but only when both stream-status fields
are nonzero. Otherwise the command is deferred through the device task queue.
`DeviceStreamOff` uses the same queue fields and `VIDIOC_STREAMOFF`.
`pullOutputBuffer` and the dequeue path handle encoded output after polling;
no separate Camera2-style drain protocol or numeric EOS-buffer contract was
recovered.

`DevicePoll` waits on two descriptors: the internal eventfd with
`POLLIN|POLLPRI`, and the V4L2 device FD with
`POLLIN|POLLPRI|POLLOUT|POLLERR`. It calls `poll(..., 2, 5000)`, maps timeout
to a zero result, maps poll failure to `-errno`, clears a readable eventfd,
and reports the device `POLLPRI` bit separately.

### Controls and prior EIO

The slot-count path recovers `VIDIOC_G_CTRL`; the configuration path exposes
named setters and the binary contains `VIDIOC_S_CTRL`, `VIDIOC_S_EXT_CTRLS`,
and `VIDIOC_S_SELECTION`. Individual helper analysis recovers these client
edges:

| Helper | Path and recovered payload/condition |
|---|---|
| `BFrameCount` | `S_CTRL`, ID `0x9909ca`, value from the helper’s stored value; submitted when dirty/device memory is valid |
| `BitrateMode` | `S_CTRL`, ID `0x9909ce`, value from stored value; submitted when dirty/device memory is valid |
| `DummyNalMode` | `S_CTRL`, ID `0x99202f`, value from stored value; submitted only when the device flag is enabled and memory is valid |
| `RequestSyncFrame` | `S_EXT_CTRLS`, ID `0x9909e5`, stored value; submitted only for a nonzero request, then the request is cleared |
| `NalLength` | `S_EXT_CTRLS`, ID/value are loaded from the helper/config record; submitted when dirty and memory is valid |
| `Bitrate`, `Gop`, `IntraFrameInterval`, `Profile`, `Level`, `OperatingRate` | helper-specific values are packed into the config/ext-control records and dispatched through `S_EXT_CTRLS` or the corresponding virtual driver updater; exact numeric IDs and values are not recovered |
| `SecureMode` | `S_CTRL`, ID/value loaded from the config record; exact numeric ID/value unknown |
| `InputBufferCrop` | direct `ioctl 0xc040565f`; rectangle payload copied from the helper record; submitted only when width and height are nonzero |
| `InputStreamParam` | direct `ioctl 0xc0cc5616`; `type` is `0x2d0` and time-per-frame fields come from the stream-param record; submitted when encoder mode and dirty state are enabled |
| `PrependHeader` | no direct ioctl in helper; dirty value invokes the device virtual operation at vtable offset `0x148` |
| `Input/OutputBufferCrop` | output crop helper aliases/shared implementation could not be assigned a unique standalone body; exact AVC selection path remains unknown |

`V4L2Config::setToDriver(mask)` iterates registered helpers and calls each
helper only when its registration mask intersects `mask`; a helper with a
successful update marks the configuration as having submitted controls. The
remaining IDs, payload encodings, and exact ordering are not inferred from
names or from the driver census.

The previous `STREAMON` `EIO` can now be tied to concrete unverified
preconditions in the implementation: both queue `REQBUFS` operations,
correct `QUERYBUF`/plane construction, required `QBUF` state, configuration
updates, and both stream-status transitions. It still cannot be attributed to
one missing control or one raw fourcc. The strongest static explanation
remains incomplete or mismatched queue preparation, followed by missing
vendor configuration or a backend/firmware prerequisite. No runtime retry was
performed.

## Controls and queue ordering

The recovered contract above supersedes this earlier summary where the two
sections differ. The binary advertises controls for bitrate mode, bitrate,
GOP, B-frames, frame-level rate control, H.264 profile/level, I-frame period,
sequence-header mode, NAL length, security path, encode scenario, operation
rate, ROI, QP, maximum dimensions, and resolution change. Their exact IDs,
values, and AVC ordering remain unresolved.

The static call path establishes this bounded high-level ordering:

1. query capability;
2. configure source and destination formats;
3. request buffers for the two V4L2 queue types;
4. queue available input/output buffers;
5. stream on both queues; and
6. poll/dequeue input and encoded output, returning/recycling buffers.

The code records two stream-on statuses (`first` and `second`). Their semantic
order is recovered as OUTPUT_MPLANE followed by CAPTURE_MPLANE; exact buffer
count and final stock queue memory mode remain **unknown**. This is
documentation of the recovered call structure, not a runtime test recipe.

## DMA-BUF and fence ownership

The C2 encoder imports `dup`, `sync_wait`, `C2Fence::wait`, Android Fence
duplication, `BufferAllocator` CPU sync, and graphic/linear block mapping.
The camera HAL owns the Camera3 request/result buffer-cache conversion and
passes acquire/release fence fields through its native stream buffers.

Confirmed: fences are first-class inputs to the C2/native-buffer path.
Unknown: which component waits the Camera3 acquire fence in the stock
recording route, which component creates the release fence, and whether the
V4L2 queue receives an explicit fence FD or only a synchronized DMA-BUF.
The current project camera path uses acquire `-1` and does not submit mapper
release fences to V4L2; that is not a stock venc contract.

## Camera allocation -> venc format mapping

Classification: **mapping unresolved; direct native handoff not confirmed**.

The evidence supports these bounded alternatives:

```text
Camera3 format 34 / GPU_TEXTURE
  -> gralloc internal format and handle (unknown)
  -> C2 graphic block
  -> either graphic/native V4L2 input or C2 conversion/copy
```

The C2 imports `C2GraphicBlock` and has explicit YUV/I420/NV12 tests and
conversion helpers. Therefore a copy/repack path is strongly supported as a
safe fallback. No evidence equates `MTK_CAMERA_OPAQUE`, `MTK_NV12_BLK`,
`NM12`, `MT21*`, or AFBC names with the stock 1920x1080 allocation.

## Standard H.264 output

The V4L2 capture fourcc is standard `H264`. The C2 code exposes profile/level,
bitrate, GOP, B-frame, I-frame, sequence-header, and NAL-length configuration.
The driver advertises `sequence_header_mode` as separate buffer or joined to
the first frame, and the vendor NAL-length controls are present.

What is confirmed: a standard H.264 capture queue exists and the component has
an AVC factory. What is not confirmed: Annex-B versus length-prefixed output,
profile/level selected by stock, bitrate/GOP/B-frame values, and the exact
numeric NAL-length control needed for a stream directly accepted by ffprobe.
No experiment values are prescribed in this static-only iteration.

## Previous STREAMON EIO explanation

The EIO result cannot be attributed to one missing control from this static
pass. The code does show several concrete boundaries that the earlier
synthetic probe did not establish: both queue buffer sets must be allocated,
the expected number of planes and `sizeimage` values must be supplied,
encoded capture buffers must be queued before stream-on, and the V4L2 C2 path
applies format/selection/control state before the two stream-on calls.

Ranked causes:

1. incomplete mem2mem queue preparation or wrong queue/memory/plane setup;
2. missing MediaTek scenario/operation-rate or extended control state;
3. a raw fourcc accepted by TRY_FMT but not accepted by the selected runtime
   backend/firmware path; and
4. missing platform/firmware prerequisite.

The analysis does not prove that `YU12` itself is rejected, nor does it expose
an EIO-specific source condition. The earlier EIO remains a runtime blocker,
not evidence that `/dev/video1` is unusable.

## Deferred runtime experiment

No camera-native or encoder-streaming proof of concept is specified in this
iteration. The recovered queue contract is not complete enough to authorize
a runtime probe, and this task intentionally performs no Camera3 buffer
acquisition, no gralloc inspection, no V4L2 STREAMON attempt, and no service
or deployment change.

## Remaining unknowns

* exact stock internal format, modifier, usage mask, heap flags, FD/plane
  offsets and strides;
* exact V4L2-versus-proprietary backend selector for ordinary AVC;
* effective stock V4L2 buffer count and final allocation mode after all
  configuration, plus the selection rectangle;
* numeric vendor control IDs/values still unresolved for bitrate/GOP,
  profile/level, scenario, operation rate, NAL mode, security, and
  camera-record usage;
* fence wait/release ownership across Camera3, C2, gralloc, and V4L2;
* stock AVC profile, level, bitrate, GOP, B-frame, and elementary-stream
  framing;
* the deployed `libvcodecdrv.so` identity: only `libvcodecdrv_v3a.so` and a
  header stub were present at inspection time.

No runtime services, camera daemon, bridge, kernel/module, init corpus,
generated graph, or deployment artifact was changed.
