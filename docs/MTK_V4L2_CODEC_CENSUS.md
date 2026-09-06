# MediaTek V4L2 codec census

## Executive summary

Live reconnaissance was performed over SSH on 2026-09-05 against the running Ubuntu installation, kernel `6.6.82-android15-8-g076ac12bde16-ab13715361-4k`, AArch64.

Confirmed nodes are:

| Node | Driver/card | Classification | Result |
|---|---|---|---|
| `/dev/video0` | `mtk-vcodec-dec`, `platform:mt6768` | video decoder | compressed input and MediaTek/native or raw capture output advertised |
| `/dev/video1` | `mtk-vcodec-enc`, `platform:mt6768` | video encoder | raw output input and H.264/HEVC/MPEG-4/H.263 capture output advertised |
| `/dev/video2` | `mtk-jpeg-enc`, `platform:17030000.venc_jpg` | JPEG encoder | YUV output input and JPEG capture output advertised |
| `/dev/video10` | `v4l2 loopback` | camera loopback | excluded from codec census; busy |
| `/dev/video11` | `v4l2 loopback` | camera loopback | excluded from codec census |

The strongest Linux-native opportunity is `/dev/video1`: it advertises ordinary raw formats and MediaTek `N-C` multi-plane formats, and standard H.264 output. However, a bounded synthetic streaming attempt reached `VIDIOC_STREAMON = EIO`; no known-good encoded stream was produced. Direct Camera3 `IMPLEMENTATION_DEFINED` compatibility is therefore **unknown**, not confirmed.

The live node names and formats are authoritative for this report. No camera, bridge, protobuf, v4l2loopback, Flutter, kernel-module, service, or Android changes were made.

## Device/node inventory

`v4l2-ctl --list-devices` reported:

```text
mtk-vcodec-enc (platform:17030000.venc_jpg): /dev/video2
platform:mt6768 (platform:mt6768): /dev/video0 /dev/video1
mtk-camera-flash (platform:mtk_composite_v4l2_1): /dev/media0
Dummy video device (0x000B) (platform:v4l2loopback-011): /dev/video11
```

`/dev/video-rear -> /dev/video10` and `/dev/video-front -> /dev/video11` are aliases to the project loopbacks. `/dev/video10` was busy during inspection, consistent with the active generic camera path.

All three MediaTek codec nodes report `Video Memory-to-Memory Multiplanar`, `Streaming`, and `Extended Pix Format`. Their device capabilities are `0x04204000`; the general capabilities additionally include `0x80000000` (`Device Capabilities` reporting).

The only media-controller node found was `/dev/media0`, model `mtk-camera-flash`, with no useful topology entities printed by `media-ctl -p`.

## Encoder: `/dev/video1`

Driver: `mtk-vcodec-enc`; card: `platform:mt6768`; bus: `platform:mt6768`.

### Video Output Multiplanar (raw input)

| FourCC | Description | Classification | Planes at 1920x1080 | Bytes per line / sizeimage |
|---|---|---|---|---|
| `NM12` | Y/UV 4:2:0 (N-C) | MediaTek/native N-C | 2 | 1920/2073600; 1920/1036800 |
| `NM21` | Y/VU 4:2:0 (N-C) | MediaTek/native N-C | 2 | 1920/2073600; 1920/1036800 |
| `YM12` | Planar YUV 4:2:0 (N-C) | MediaTek/native N-C | 3 | 1920/2073600; 960/518400; 960/518400 |
| `YM21` | Planar YVU 4:2:0 (N-C) | MediaTek/native N-C | 3 | 1920/2073600; 960/518400; 960/518400 |
| `YU12` | Planar YUV 4:2:0 | standard raw I420 | 1 | 1920/3110400 |
| `YV12` | Planar YVU 4:2:0 | standard raw | 1 | 1920/3110400 |
| `NV12` | Y/UV 4:2:0 | standard raw | 1 | 1920/3110400 |
| `NV21` | Y/VU 4:2:0 | standard raw | 1 | 1920/3110400 |

The advertised input enumeration contains no opaque fourcc beyond the explicit `N-C` family. Exact fourcc spelling is preserved above; layout beyond the ioctl plane descriptions is not inferred.

### Video Capture Multiplanar (encoded output)

`H264` (H.264), `HEVC` (HEVC), `MPG4` (MPEG-4 Part 2 ES), and `H263` (H.263) are advertised. The enumeration ranges are respectively:

```text
H264: 128x128 - 2560x1440, step 16/8
HEVC: 128x128 - 2560x1440, step 16/8
MPG4: 16x16 - 640x480, step 16/16
H263: 176x144 only
```

At the queried 1920x1080 capture format, each negotiated as one plane with `bytesperline=0` and `sizeimage=20480` during `TRY_FMT`. This is a negotiated buffer size, not proof that every encoded frame fits that size under every control configuration.

### Encoder controls

Relevant advertised controls include `video_b_frames` (0..3), `video_gop_size`, `video_bitrate_mode` (Variable Bitrate, Constant Bitrate, Constant Quality), `video_bitrate` (0..400000000, default 20000000), `frame_level_rate_control_enable`, `sequence_header_mode` (Separate Buffer, Joined With 1st Frame), `h264_mb_level_rate_control`, `force_key_frame`, `h264_maximum_qp_value`, `h264_i_frame_period`, `h264_level` (1 through 6.2 menu), `h264_profile` (Baseline through High 10), `hevc_profile` (Main), `hevc_level` (1 through 6.2), and `hevc_tier` (Main/High).

Additional vendor controls are exposed for NAL length, security path, encode scenario, operation rate, ROI, HEIF grid size, maximum width/height, frame QP values, TSVC, color description, and resolution change. Their presence does not establish a usable configuration or Camera3 compatibility.

## Decoder: `/dev/video0`

Driver: `mtk-vcodec-dec`; card/bus: `platform:mt6768`.

### Video Output Multiplanar (compressed input)

`H264`, `HEVC`, `HEIF`, `MPG2`, `MPG4`, `H263`, `VP80`, `VP90`, and `WVC1` are advertised. Ranges are:

```text
H264: 16x16 - 3840x2160, step 16/32
HEVC: 16x16 - 3840x2160, step 64/64
HEIF: 16x16 - 16383x16383, step 64/64
MPG2/MPG4: 16x16 - 2048x1088, step 16/32
H263: 16x16 - 1408x1152, step 16/32
VP80: 16x16 - 2048x1088, step 16/16
VP90: 16x16 - 3840x2160, step 64/64
WVC1: 16x16 - 1920x1088, step 16/16
```

### Video Capture Multiplanar (decoded output)

The complete advertised list is: `M21S`, `M2CS`, `MTSR`, `MTST`, `MCSR`, `MCST`, `MACS`, `MJSR`, `MJST`, `JCSR`, `JCST`, `MM21`, `MT21`, `MT2R`, `MT2T`, `MTCR`, `MTCT`, `YV12`, and `NV12`.

The driver labels the first 17 as Mediatek Video Block, 8-bit Block, Compressed, 10-bit Raster/Tile, or similar compressed/native formats. `YV12` and `NV12` are standard raw formats, though the enumeration text from this driver labels them `compressed`; no layout inference is made from that label. A 1920x1080 `TRY_FMT` was rounded to 1920x1088 for `M21S`, `MM21`, `MT21`, `YV12`, and `NV12`.

Representative 1920x1088 negotiated descriptions were: `M21S`, one plane, stride 1920, sizeimage 3133440; `MM21` and `MT21`, two planes, strides 1920, sizes 2088960 and 1044480; `YV12`/`NV12`, one plane, stride 1920, sizeimage 3133440. These are negotiation observations, not layout documentation.

`v4l2-ctl --list-ctrls` segfaulted on `/dev/video0`, so decoder controls are **unknown** from this census; the tool failure is recorded rather than treated as absence.

## JPEG codec: `/dev/video2`

Driver/card: `mtk-jpeg-enc`; bus: `platform:17030000.venc_jpg`.

Output input advertises `NM12` (Y/UV 4:2:0 (N-C)), continuous 32x32 through 8192x8192. Capture output advertises `JPEG` (JFIF JPEG), continuous 32x32 through 8192x8192. Current format was 32x32, one-plane `YUYV` input with `bytesperline=64`, `sizeimage=2048`, and one-plane JPEG output with `sizeimage=1048576`.

Controls are `restart_interval` (0..100, default 0), `compression_quality` (0..100, default 90), and `active_markers` (bitmask, max `0x00000002`).

The repeated size lines emitted by this target's `v4l2-ctl` for JPEG were not summarized as distinct format capabilities; the stable ioctl result is the continuous 32x32..8192x8192 range. No JPEG encode was claimed: a bounded runtime production test was not completed after the codec streaming probe reached the target's I/O-error boundary.

## TRY_FMT/S_FMT behavior

`TRY_FMT` was used for the decisive matrix. At 1920x1080, every encoder input request above was preserved exactly in fourcc and dimensions; plane counts and bytesperline/sizeimage matched the table. Encoder capture requests for all four advertised codecs were preserved at 1920x1080. Decoder compressed inputs were preserved; decoder raw/native output requests were preserved in fourcc but dimensions were rounded from 1920x1080 to 1920x1088.

No persistent `S_FMT` result is used as evidence of a working pipeline. A bounded explicit `YU12` + `H264` stream setup was attempted, but `VIDIOC_STREAMON` returned `EIO` and no independently decodable multi-frame H.264 elementary stream was obtained. The driver subsequently reported its default 160x128 `NM12` + `H264` state when queried.

## Memory models

The nodes are multiplanar streaming devices and `MMAP` buffer allocation reached successful `VIDIOC_REQBUFS`/buffer creation in the encoder probe. That establishes an MMAP setup path, but not successful codec streaming.

The target `v4l2-ctl` user-pointer attempt reached `VIDIOC_QBUF = EINVAL`; this is insufficient to distinguish a bad generated buffer setup from a driver rejection. The DMABUF attempt did not reach a clean, independently attributable completion. Therefore the conservative result is:

| Node | MMAP | USERPTR | DMABUF |
|---|---|---|---|
| `/dev/video1` encoder | setup observed; streaming failed `EIO` | not established (`QBUF EINVAL`) | not established |
| `/dev/video0` decoder | not established | not established | not established |
| `/dev/video2` JPEG | not established | not established | not established |

No DMABUF claim is made merely from mem2mem capability.

## Known-good encode/decode tests

None. The synthetic encoder run was bounded and intentionally stopped at the first meaningful runtime boundary: `VIDIOC_STREAMON returned -1 (Input/output error)`. `ffprobe` saw an H.264-labelled pipe but could not determine dimensions or frame count; this is not a valid encode result. No decoder stream or JPEG stream was promoted to a known-good result.

## Camera3 IMPLEMENTATION_DEFINED comparison

Stock VIDEO is known from prior repository evidence as Camera3 format 34 (`IMPLEMENTATION_DEFINED`), usage `GPU_TEXTURE`, 1920x1080 at 30 fps. The live V4L2 census cannot resolve Android format 34 into a concrete fourcc, modifier, stride, plane layout, native handle, or DMA-BUF identity.

The only defensible relationships are:

| Comparison | Classification |
|---|---|
| Camera3 `IMPLEMENTATION_DEFINED` vs encoder `NM12/NM21/YM12/YM21` | unknown; naming is not proof |
| Camera3 `IMPLEMENTATION_DEFINED` vs decoder native family | unknown |
| Camera3 `IMPLEMENTATION_DEFINED` vs JPEG `NM12` input | unknown |
| encoder standard `YU12/NV12/NV21` vs current Linux converted path | confirmed fourcc-level compatibility for input negotiation, not zero-copy compatibility |

## Camera -> venc feasibility

The measured answer is partial:

- `mtk-venc` accepts a MediaTek-specific/native-looking raw family: **confirmed** at `TRY_FMT` level.
- `mtk-venc` accepts ordinary `YU12`, `YV12`, `NV12`, and `NV21`: **confirmed** at `TRY_FMT` level.
- `mtk-venc` advertises standard H.264 output: **confirmed**.
- Successful DMABUF input queueing: **unknown**.
- Successful hardware H.264 production from Linux: **not proven; streamon returned EIO**.
- Equivalence to Camera3 `IMPLEMENTATION_DEFINED`: **unknown**.

Consequently, the path

```text
Camera3 IMPLEMENTATION_DEFINED/native buffer -> DMABUF -> mtk-venc -> H.264
```

is technically plausible but not currently evidenced as usable. Direct proof still requires one real Camera3 1920x1080 buffer's mapper/native-handle layout and a controlled queue of that exact DMA-BUF into the matching encoder input contract, including fences and ownership.

## Preserved baseline

This work did not modify or benchmark away the existing generic path:

```text
Camera3 YCBCR_420_888 -> I420/YU12 -> /dev/video-rear -> ffmpeg/ffplay
```

That path remains the confirmed generic Linux consumer path. The codec nodes remain reconnaissance results only.

## Unknowns

- Why the live encoder stream setup returns `EIO` (firmware, queue contents, exact userspace ABI, or another runtime prerequisite).
- Actual DMABUF support and required allocator/fence ownership on each queue.
- Concrete native layout behind Camera3 `IMPLEMENTATION_DEFINED`.
- Whether any decoder native output family is the same allocation family used by stock Camera3 VIDEO.
- Stock MediaRecorder/MediaCodec V4L2 queue settings, controls, and selected raw input.
- Whether a stock surface path uses these V4L2 nodes directly or an intermediate vendor component.

## Recommended next experiment

Allocate one real Camera3 `IMPLEMENTATION_DEFINED` 1920x1080 buffer and inspect its mapper/native-handle plane layout and DMA-BUF metadata, without changing the current daemon path; use that evidence to select the exact encoder input format before attempting any queue test.
