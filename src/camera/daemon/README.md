# TB330FU camera daemon

The daemon keeps the existing V4L2 video bridge as its `VIDEO` path. The
Still capture reuses the proven Camera3 sequence:
preview requests, result-metadata 3A convergence, then the following
`STILL_CAPTURE` request. Video recording attaches to the already-running
camera worker's shared-memory ring and does not start or stop that worker.

The internal control endpoint is Unix `SOCK_SEQPACKET` at
`/run/tb330fu-camera/control.sock`; asynchronous snapshots are published at
`/run/tb330fu-camera/events.sock`. Each control packet is one protobuf v1
`Request`, with one matching `Response` packet. Packets are limited to 64 KiB;
images and file descriptors are never sent over IPC. The schema is
[`../protocol/tb330fu_camera.proto`](../protocol/tb330fu_camera.proto), and the
direct test client uses the generated binding
[`../protocol/tb330fu_camera_pb2.py`](../protocol/tb330fu_camera_pb2.py) through
[`../protocol/tb330fu_camera_client.py`](../protocol/tb330fu_camera_client.py).

Regenerate and verify the bindings with the matching AOSP compiler (the
canonical build host keeps this at the default path below):

```bash
PROTOC=/home/felix/protobuf-3.21-build/protoc-3.21.12.0 \
  python3 src/camera/protocol/regenerate_camera_proto.py
PROTOC=/home/felix/protobuf-3.21-build/protoc-3.21.12.0 \
  python3 src/camera/protocol/verify_camera_ipc_schema.py
```

The daemon build compiles the AOSP `libprotobuf-cpp-lite` sources into the
target binary and checks that the checked-in generated files are reproducible.

```text
python3 src/camera/protocol/tb330fu_camera_client.py get_state
python3 src/camera/protocol/tb330fu_camera_client.py set_mode photo
python3 src/camera/protocol/tb330fu_camera_client.py capture_still
python3 src/camera/protocol/tb330fu_camera_client.py events
```

`capture_still` requests one still output per capture: JPEG by default or RAW16
when selected by the typed request. Outputs are
`/run/tb330fu-camera/last.jpg`, `/run/tb330fu-camera/last.raw`,
`/run/tb330fu-camera/last-metadata.json`, and
`/run/tb330fu-camera/last-raw-layout.json`. Each capture replaces the prior
files; no capture history or IDs are maintained.
For JPEG product mode the daemon first publishes `last-still.yuv` and
`last-still-yuv-layout.json`; the persistent glibc `tb330fu-camera-processor`
then overwrites `last.jpg` synchronously through its processor socket. The
daemon does not encode JPEG or depend on glibc/libjpeg.
The Camera3 session configures preview plus exactly one full-resolution still
stream: processed YUV for JPEG or RAW16 for raw. Switching between these
formats explicitly closes and reopens the session; YUV and RAW16 are never
configured concurrently.
The events stream sends a complete snapshot immediately and continues
at approximately 30 Hz; each packet is independently current and it is not a
durable event log. Slow consumers may drop packets without blocking capture.
