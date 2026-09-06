# Camera

## Purpose

The camera subsystem exposes the retained TB330FU Android Camera3 hardware path as ordinary Linux V4L2 devices.
Camera applications and desktop consumers use V4L2; they do not use Android camera APIs.

## Architecture

```text
Android Camera3 provider/HAL
  device@1.1/internal/0 or /1
        |
        | AIDL NDK CameraDeviceSession, Binder, result metadata FMQ
        v
tb330fu-camera-daemon (AArch64 bionic)
        |
        | localhost length-prefixed JSON control
        | Camera3 buffers imported through mapper and DMABUF OUTPUT
        v
v4l2loopback-tb330fu
        |
        | ordinary V4L2 CAPTURE, YU12/I420
        v
/dev/video-rear and /dev/video-front
```

Android/vendor ownership ends at the Camera3 provider, allocator, mapper, and their retained runtime prerequisites.
The daemon is project compatibility code.
The module and V4L2 nodes are native Linux-facing transport.

## Components

| Path | Role |
|---|---|
| `src/camera/daemon/tb330fu-camera-daemon.cpp` | resident bionic Camera3-to-V4L2 daemon |
| `src/camera/capture/tb330fu-camera-capture.cpp` | shared Camera3/AIDL/FMQ/mapper implementation |
| `src/camera/bridge/camera_bridge.py` | FastAPI/uvicorn app-facing HTTP and WebSocket bridge |
| `src/camera/bridge/openapi.json` | generated v1 HTTP contract for the future frontend |
| `modules/v4l2loopback-tb330fu/v4l2loopback.c` | GPL v4l2loopback fork with DMABUF OUTPUT and producer events |
| `modules/v4l2loopback-tb330fu/build.sh` | target-kernel build and BTF gate |
| `modules/v4l2loopback-tb330fu/v4l2loopback.h` | daemon/module event ABI |
| `modules/v4l2loopback-tb330fu/v4l2loopback_formats.h` | single YU12 format declaration |
| `systemd/tb330fu-camera-module.service` | loads the target module and creates stable aliases |
| `systemd/tb330fu-camera-daemon.service` | starts the daemon after module/readiness gates |

## Vendor-side prerequisites

The daemon looks up `android.hardware.camera.provider.ICameraProvider/internal/0` through the Android Binder service manager.
It then opens provider device interfaces `device@1.1/internal/0` (rear) or `device@1.1/internal/1` (front).
The retained `cameraserver`, `camerahalserver`, `cameraconfig`, allocator, mapper, Binder, property, device-node, firmware, and module prerequisites remain owned by the vendor runtime graph.
See [VENDOR_RUNTIME.md](VENDOR_RUNTIME.md) and [PROPERTIES.md](PROPERTIES.md).

The daemon waits up to ten seconds for provider publication.
It does not start or repair the provider.

## Camera selection

The provider currently exposes two physical/internal IDs:

| Linux name | Camera3 ID | Facing | Advertised modes used by the daemon |
|---|---|---|---|
| `rear` | `device@1.1/internal/0` | BACK | 640x480, 1280x720, 1920x1080, 3264x2448 |
| `front` | `device@1.1/internal/1` | FRONT | 640x480, 1280x720, 1920x1080 |

The daemon exposes one V4L2 node per camera.
It permits only one active HAL owner at a time.
Both cameras are enumerated, but simultaneous capture is not supported by the provider and is not claimed here.
V4L2 consumer selection is by stable node, not by a Camera3 ID.

## Stream configuration

For video/preview the daemon creates one Camera3 `OUTPUT` stream with:

| Field | Value |
|---|---|
| requested format | `IMPLEMENTATION_DEFINED` |
| requested usage | `GPU_TEXTURE` (`0x100`) |
| dataspace | `UNKNOWN` |
| rotation | `ROTATION_0` |
| template | `VIDEO_RECORD` |
| operation mode | `NORMAL_MODE` |
| default mode | 1920x1080 at 30 fps |
| allocation pool | `ANDROID_REQUEST_PIPELINE_MAX_DEPTH`, capped at 8; fallback 3 |

V4L2 `S_FMT` and `S_PARM` events select the supported width, height, and frame interval while the producer is idle.
Preview and video use the same daemon stream configuration.
The daemon uses the returned `HalStream` format and usage for allocation and
buffer import. The Linux-facing format is YU12 after conversion of the
mapper-visible Y-plus-interleaved-VU payload. The separate still stream uses
`YCBCR_420_888` with `CPU_READ_OFTEN`.

## Scene control

The daemon constructs session parameters using standard camera metadata operations.
It sets vendor tag `0x90010000`, `com.lenovo.camera.mode.value`, to `2` (`INT32`) for the VIDEO_RECORD policy.
This is a target-specific Camera3 metadata value, not a copied application resource.
It is applied during `configureStreams` for both cameras.
The current implementation does not claim a separate rear/front value.

## Control plane

The daemon exposes local protobuf v1 `SOCK_SEQPACKET` endpoints:
`/run/tb330fu-camera/control.sock` for synchronous request/reply and
`/run/tb330fu-camera/events.sock` for asynchronous current-state snapshots.
Packets are bounded to 64 KiB; image bytes and file descriptors are excluded.
The schema and dependency-free direct client are under `src/camera/protocol/`.
Despite the historical description as an HTTP control plane, the current implementation is not an HTTP server: it has no HTTP request line, headers, content types, or HTTP status codes.
It uses a length-prefixed JSON protocol.

Each request is `ASCII decimal byte length` immediately followed by a JSON object.
The response uses the same framing.
A request may include a `payload_length`; that many bytes are consumed and ignored.
JSON parsing is bounded.
Invalid framing or oversized values disconnect the client.

| Operation | Request fields | Response/effect |
|---|---|---|
| `get_cameras` | `id`, `op` | `id`, `cameras:["rear","front"]` |
| `get_status` | `id`, `op`, optional `camera` | state, mode, frame count, active owner, last error |
| `get_capabilities` | `id`, `op`, optional `camera` | `fourcc:"YU12"`, `fps:30`, supported modes |

Unsupported operations return an `error:"unsupported_op"` response.
The daemon does not expose HTTP start/stop, controls, or still-capture endpoints.
V4L2 CAPTURE `STREAMON` and `STREAMOFF` drive producer lifetime.

## Data plane

The control and data planes are separate:

```text

Stream lifecycle control: native V4L2 consumer -> loopback producer event -> daemon
         -> Camera3 request/settings

Data:    Camera3 result metadata FMQ + allocated image buffer
         -> mapper import/lock -> daemon staging buffer
         -> loopback OUTPUT DMABUF QBUF -> ordinary V4L2 CAPTURE
```

### FMQ

`ICameraDeviceSession::getCaptureResultMetadataQueue` supplies the result metadata FMQ.
The callback reads metadata from this synchronized FMQ when `fmqResultSize` is nonzero and falls back to inline Binder metadata when the result carries it.
FMQ carries result metadata, not image pixels.
The daemon tracks returned `(frameNumber, bufferId)` pairs and recycles buffers.
An unavailable or unreadable FMQ is logged; missing metadata is not treated as image data.

### DMA-BUF handling

The allocator's `allocate2` result supplies native handles.
The daemon duplicates their FDs, imports the image handle through the AIDL mapper v5, and locks it for `CPU_READ_OFTEN`.
The tested linear layout is a Y plane followed by interleaved VU; the valid canonical prefix is `width * height * 3 / 2` bytes.
Acquire fences are `-1`; the mapper release fence is returned by unlock and is not submitted to V4L2.

The daemon retains ownership of duplicated handles/imported buffers until the camera worker stops, then unlocks/frees/closes them.
The module obtains a temporary DMA-BUF reference during OUTPUT `QBUF`, performs CPU access and `dma_buf_vmap`, copies into its existing vmalloc-backed queue storage, then unmaps, ends CPU access, and releases the reference before returning.
This is not an end-to-end zero-copy path.

### Frame conversion

The daemon keeps the mapper's canonical NV21 payload until just before V4L2 submission.
It copies Y unchanged and deinterleaves VU into planar U then V in a reusable staging buffer.
The module advertises only `V4L2_PIX_FMT_YUV420` (`YU12`/I420).
This is one CPU conversion in userspace; there is no kernel conversion.

## v4l2loopback ABI

The fork is based on v4l2loopback version `0.15.4`; the exact upstream commit was not recoverable from the old workspace.
Upstream GPL notices and `COPYING` are retained.
The project changes are confined to the tracked source's format table, DMA-BUF OUTPUT path, and producer-event lifecycle.

The daemon includes `v4l2loopback.h`.
The internal event ABI is:

```c
struct v4l2loopback_producer_event {
    __u32 event;       /* START=1, STOP=2, MODE_CHANGED=3 */
    __u32 width;
    __u32 height;
    __u32 fourcc;
    __u32 fps_num;
    __u32 fps_den;
};
```

The event type is `V4L2_EVENT_PRIVATE_START + 0x330`.
Consumers trigger events with standard CAPTURE `STREAMON`, `STREAMOFF`, `S_FMT`, and `S_PARM`; the daemon subscribes with `VIDIOC_SUBSCRIBE_EVENT` and reads events with `VIDIOC_DQEVENT` after `POLLPRI`.

For producer OUTPUT, the module accepts standard `V4L2_MEMORY_DMABUF`.
Userspace submits an FD and `bytesused`; no private image ioctl is used.
The module copies the FD's contents into its normal capture queue.
Import failure, invalid size, unsupported memory type, and ownership violations return V4L2 errors.

## Linux-facing devices

The module creates dynamic `/dev/video10` and `/dev/video11`.
The module unit creates these aliases:

```text
/dev/video-rear  -> /dev/video10
/dev/video-front -> /dev/video11
```

The aliases are systemd-created symlinks, not udev rules.
[`tb330fu-devd`](DEVD.md) creates the character nodes from kernel uevents; udev remains the policy consumer.
Nodes are owned by `root:felix` with mode `0660` in the observed deployment.
The daemon runs as `felix` with `video` and `render` supplementary groups.

Consumers use ordinary V4L2 CAPTURE.
The validated format is YU12/I420 (`yuv420p`), with the camera-specific modes listed above.
MMAP CAPTURE is supported; OUTPUT DMABUF is an internal producer path.
Consumers do not need the project ABI or Camera3 knowledge.

## App-facing bridge

The Python bridge exposes the stable v1 API through FastAPI and uvicorn. Its
canonical generated contract is [`src/camera/bridge/openapi.json`](../src/camera/bridge/openapi.json).
The bridge translates public string/enumeration models to the daemon's
protobuf requests and does not become authoritative for camera state.

The API includes synchronous still capture and raw/JPEG retrieval from the
daemon-owned last-shot files. `/v1/events` uses Starlette WebSocket support and
sends complete status snapshots, beginning with the latest available snapshot.
The generated contract freezes operation IDs, cleaned status enums, and binary
`/v1/last-shot` responses. Control PATCH fields are optional for omission, but
explicit `null` and unknown fields are rejected. Capture responses expose only
image dimensions, frame number, and result metadata.

## Startup and lifecycle

`tb330fu-camera-module.service` requires the Android readiness gate, loads `v4l2loopback-events-btf.ko` with two devices, `max_buffers=4`, and `max_openers=3`, then creates the stable aliases.
Its stop action removes the aliases and unloads the module.

`tb330fu-camera-daemon.service` requires the module unit, starts after the module and `tb330fu-android-boot-complete.service`, runs the target-bionic `/usr/local/lib/tb330fu/tb330fu-camera-daemon` as `felix`, and uses `Restart=on-failure` with a two-second delay.
The daemon remains alive while both cameras are stopped.
A camera worker opens one Camera3 device session when its V4L2 producer starts and
keeps that session alive until the producer stops. Video recording attaches to
the active worker's shared-memory ring and does not acquire or release camera
ownership.

The recorder is a separate native glibc process. The daemon publishes Camera3
frames to the bounded four-slot ring declared in
[`src/camera/video/tb330fu_video_ring.h`](../src/camera/video/tb330fu_video_ring.h);
the recorder owns MP4 finalization. Control/status protobuf, HTTP, and WebSocket
messages never carry frame payloads. For `h264_v4l2m2m`, the recorder supplies
independent 32-byte-aligned `AVBufferRef`s for the NV21 Y and VU planes before
`avcodec_send_frame()`.

## Failure behavior

Provider lookup timeout, missing video nodes, event subscription failure, camera open/configuration failure, allocator/mapper failure, invalid layout, DMA-BUF failure, and V4L2 producer setup failure are logged.
Worker failures set the camera state to `error`; daemon initialization failures terminate the daemon and systemd may restart it.
Camera3 error notifications and failed buffer results are recorded in the daemon log.

Malformed control framing disconnects the client.
A client disconnect does not itself stop a camera worker; V4L2 `STREAMOFF` or close generates the stop event.
A competing camera is rejected by the daemon's single-owner rule.
The observed busy-consumer behavior can be a wait for frames rather than an immediate synchronous `EBUSY` and remains a limitation.

## Build

### Camera daemon

Build on the AArch64-capable Linux host with Android NDK clang, target `aarch64-linux-android35`, and the frozen Android 15 r17 graphics allocator snapshot.
The build links target `libbinder_ndk`, `libcamera_metadata`, `libfmq`, `libcutils`, `libutils`, `libbase`, `liblog`, `libpng`, and `libz`.
All source and library roots are explicit environment variables; the build script contains no private workspace paths.

```bash
HDR=/path/to/aosp-headers \
NDK=/path/to/android-ndk \
AIDL=/path/to/aidl \
BINDER_NDK=/path/to/libbinder_ndk.so \
META_LIB=/path/to/libcamera_metadata.so \
FMQ_LIB=/path/to/libfmq.so \
CUTILS_LIB=/path/to/libcutils.so \
UTILS_LIB=/path/to/libutils.so \
BASE_LIB=/path/to/libbase.so \
LOG_LIB=/path/to/liblog.so \
FMQ_SRC_DIR=/path/to/aosp/system/libfmq \
META_INC=/path/to/camera/metadata/headers \
GRAPHICS_SNAPSHOT=/path/to/aidl-graphics-v2 \
LIBPNG_INC=/path/to/libpng/headers \
LIBPNG_LIB=/path/to/libpng.so \
Z_LIB=/path/to/libz.so \
  sh src/camera/daemon/build.sh
```

The resulting ELF uses `/system/bin/linker64` and target bionic libraries.
Do not run it against real Camera3 hardware on the build host.

### Native video recorder

Build the glibc/AArch64 recorder on `entertainment.local` with the external
FFmpeg and AOSP protobuf sources required by
[`src/camera/recorder/build.sh`](../src/camera/recorder/build.sh):

```bash
sh src/camera/recorder/build.sh
```

Install the resulting `out/tb330fu-video-recorder` as
`/usr/local/lib/tb330fu/tb330fu-video-recorder` on the target and use
[`systemd/tb330fu-video-recorder.service`](../systemd/tb330fu-video-recorder.service).
The service is root-owned because it writes the runtime ring and
`/var/lib/tb330fu-camera/video.mp4`; the camera daemon remains the Camera3
owner.

### Kernel module

Build against the reconstructed target kernel tree with the exact target `Module.symvers`, recovered symbol CRCs, Android clang, and module BTF enabled.
Set `ARCH=arm64`, `CROSS_COMPILE`, and `KERNEL_DIR` for the build host.
The target workflow must preserve `CONFIG_DEBUG_INFO_BTF_MODULES=y`; modules without matching BTF have caused kernel panics on this device.

```bash
make -C modules/v4l2loopback-tb330fu \
  KERNEL_DIR=/path/to/target/kernel \
  ARCH=arm64 CROSS_COMPILE=/path/to/aarch64-linux-android-
```

The module build must be checked for exact vermagic, import CRC closure, `__versions`, and a `.BTF` section.
Do not load it on the build host.

## Verification

Read-only target checks:

```bash
systemctl is-active tb330fu-camera-daemon.service tb330fu-camera-module.service
pgrep -af tb330fu-camera
ls -l /dev/video-rear /dev/video-front /dev/video10 /dev/video11
ls -l /run/tb330fu-camera/control.sock /run/tb330fu-camera/events.sock
grep '^v4l2loopback' /proc/modules
v4l2-ctl --all -d /dev/video-rear
v4l2-ctl --list-formats-ext -d /dev/video-rear
```

The first five checks are non-invasive liveness checks.
`v4l2-ctl` queries must not be combined with stream creation when preserving a live consumer.
The deployed validation established active units, stable aliases, a listening localhost socket, loaded BTF-enabled module, one YU12 format on both nodes, and rear/front frame delivery.
Sustained delivery was measured below nominal 30 fps; that does not change the V4L2 contract.

## Known limitations

- The daemon control interface is protobuf over Unix `SOCK_SEQPACKET`; the
  separate app-facing bridge provides HTTP/WebSocket through FastAPI.
- `focus_mode`, `white_balance_mode`, AE/AWB lock, exposure compensation,
  manual exposure, ISO, and manual focus are daemon-owned requested controls;
  they are applied to subsequent Camera3 requests where the target metadata
  supports them. Autofocus is a one-shot Camera3 AF trigger.
- Rear/front simultaneous HAL capture is unsupported.
- Still capture returns result metadata and the canonical last-shot path;
  image bytes are served by the bridge from daemon-owned files and are not
  transported over IPC.
- The DMA-BUF path performs a module copy and the daemon performs an NV21 to I420 conversion; it is not zero-copy.
- Busy consumers may wait instead of receiving immediate `EBUSY`.
- Nominal 30 fps is not reached in the validated deployment.
- Exact upstream v4l2loopback commit provenance is unknown; the version and upstream license material are retained.
