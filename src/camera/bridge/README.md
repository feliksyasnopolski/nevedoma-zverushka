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

The runtime needs FastAPI, uvicorn, the generated protobuf Python dependency,
Pillow, and NumPy for vectorized JPEG extraction. The raw endpoint does not
require Pillow or NumPy. JPEG conversion reads
`/run/tb330fu-camera/last-layout.json`, which is the daemon's copied mapper
plane-layout sidecar, and therefore honors allocation offsets and strides.

Status camera/mode values are `unspecified`, `rear`/`front`, and
`unspecified`, `photo`/`video`. PATCH controls preserve omitted fields, accept
explicit zero/false values, and reject null or unknown fields. Capture responses
contain metadata only; raw and JPEG bytes are served by `/v1/last-shot`.
Stable operation IDs are `getState`, `setCamera`, `setMode`, `getCapabilities`,
`getControls`, `setControls`, `autofocus`, `capture`, and `getLastShot`.
