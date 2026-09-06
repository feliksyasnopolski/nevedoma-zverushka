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

Status camera/mode values are `unspecified`, `rear`/`front`, and
`unspecified`, `photo`/`video`. PATCH controls preserve omitted fields, accept
explicit zero/false values, and reject null or unknown fields. Capture responses
contain metadata only; raw and JPEG bytes are served by `/v1/last-shot`.
Stable operation IDs are `getState`, `setCamera`, `setMode`, `getCapabilities`,
`getControls`, `setControls`, `autofocus`, `capture`, and `getLastShot`.
