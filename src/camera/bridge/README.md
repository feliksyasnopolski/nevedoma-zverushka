# TB330FU camera FastAPI bridge

`camera_bridge.py` is a FastAPI application served by uvicorn. It exposes the
app-facing v1 HTTP API and `/v1/events` as a WebSocket. It is a thin
translation layer over the daemon's protobuf `SOCK_SEQPACKET` sockets; it does
not own camera state.

Run it from this directory with:

```text
python3 camera_bridge.py --listen 127.0.0.1 --port 3909
```

The checked-in [openapi.json](openapi.json) is the canonical generated HTTP
contract. Regenerate it with:

```text
python3 generate_openapi.py
```

Build the persistent processor separately on the Linux build host. Set
`JPEGINC` and `JPEGLIB` to the target glibc/libjpeg-turbo sysroot when needed:

```text
JPEGINC=/path/to/jpeg/include \
JPEGLIB=/path/to/aarch64/libjpeg.so.8 \
  sh src/camera/jpeg/build.sh
```

The runtime needs FastAPI, uvicorn, and the generated protobuf Python
dependency. JPEG capture requests conversion from the persistent
`tb330fu-camera-processor` over `/run/tb330fu-camera/processor.sock`; the raw
endpoint serves the daemon's RAW16 artifact.

Still-capture smoke tests must start a V4L2 preview consumer first, for
example `single-consumer-test-release /dev/video-rear`. Keep that consumer
running while calling `POST /v1/capture?format=jpeg` and
`POST /v1/capture?format=dng`, stop it afterward, and confirm `GET /v1/state`
returns `state: idle`. Calling either capture endpoint without an active
preview consumer is expected to return HTTP `502` (`bad_gateway`) with daemon
code `CAPTURE_FAILED` and message `camera is not the active HAL owner`.

Status camera/mode values are `unspecified`, `rear`/`front`, and
`unspecified`, `photo`/`video`. PATCH controls preserve omitted fields, accept
explicit zero/false values, and reject null or unknown fields. Capture responses
contain metadata only; raw and JPEG bytes are served by `/v1/last-shot`.
Stable operation IDs are `getState`, `setCamera`, `setMode`, `getCapabilities`,
`getControls`, `setControls`, `autofocus`, `capture`, and `getLastShot`.
