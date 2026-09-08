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

## Capture smoke-test order

The V4L2 preview consumer must be started before requesting a still. The
consumer establishes the active preview path on `/dev/video-rear` (or
`/dev/video-front`); the HTTP bridge's JPEG and DNG capture requests depend on
that active HAL owner. A bounded smoke test therefore follows this order:

```bash
/home/felix/single-consumer-test-release /dev/video-rear 120 \
  >/tmp/tb330fu-preview-smoke.log 2>&1 &
consumer_pid=$!
sleep 2
curl -X POST http://127.0.0.1:3909/v1/capture?format=jpeg
curl -X POST http://127.0.0.1:3909/v1/capture?format=dng
kill "$consumer_pid"
wait "$consumer_pid" 2>/dev/null || true
curl http://127.0.0.1:3909/v1/state
```

The consumer must remain active for both capture requests. Stop it only after
the captures complete, then verify that the bridge reports camera state
`idle`. If the consumer was not started, or has already stopped, capture is
expected to fail with HTTP `502` and a structured error such as:

```json
{"error":{"code":"bad_gateway",
"message":"camera is not the active HAL owner",
"daemon_code":"CAPTURE_FAILED"}}
```
