#!/usr/bin/env python3
"""FastAPI app bridge for the TB330FU protobuf camera daemon."""
from __future__ import annotations

import argparse
import asyncio
import io
import json
import threading
from contextlib import asynccontextmanager
from enum import Enum
from http import HTTPStatus
from pathlib import Path
from typing import Any, Literal, Optional

from fastapi import FastAPI, Query, Request, WebSocket, WebSocketDisconnect
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse, Response
from pydantic import BaseModel, ConfigDict, model_validator
from starlette.exceptions import HTTPException as StarletteHTTPException

from tb330fu_camera_pb2 import CameraStatus, SetControlsRequest, StatusUpdate

try:
    from .camera_ipc import CameraDaemonError, CameraIpc, CameraIpcError
except ImportError:  # direct execution from this directory
    from camera_ipc import CameraDaemonError, CameraIpc, CameraIpcError

RAW_PATH = Path("/run/tb330fu-camera/last.raw")
LAYOUT_PATH = Path("/run/tb330fu-camera/last-layout.json")


class CameraName(str, Enum):
    rear = "rear"
    front = "front"


class ModeName(str, Enum):
    photo = "photo"
    video = "video"


class CameraStatusName(str, Enum):
    unspecified = "unspecified"
    rear = "rear"
    front = "front"


class ModeStatusName(str, Enum):
    unspecified = "unspecified"
    photo = "photo"
    video = "video"


class CameraStateName(str, Enum):
    unspecified = "unspecified"
    idle = "idle"
    configuring = "configuring"
    streaming = "streaming"
    capturing = "capturing"
    error = "error"


class AeStateName(str, Enum):
    unspecified = "unspecified"
    inactive = "inactive"
    searching = "searching"
    converged = "converged"
    locked = "locked"
    flash_required = "flash_required"
    precapture = "precapture"


class AfStateName(str, Enum):
    unspecified = "unspecified"
    inactive = "inactive"
    passive_scan = "passive_scan"
    passive_focused = "passive_focused"
    active_scan = "active_scan"
    focused_locked = "focused_locked"
    not_focused_locked = "not_focused_locked"
    passive_unfocused = "passive_unfocused"


class AwbStateName(str, Enum):
    unspecified = "unspecified"
    inactive = "inactive"
    searching = "searching"
    converged = "converged"
    locked = "locked"


class PublicModel(BaseModel):
    model_config = ConfigDict(extra="forbid")


class CameraSelection(PublicModel):
    camera: CameraName


class ModeSelection(PublicModel):
    mode: ModeName


class ResultMetadata(PublicModel):
    ae_state: AeStateName
    af_state: AfStateName
    awb_state: AwbStateName
    exposure_time_ns: int
    sensitivity_iso: int
    frame_duration_ns: int
    focus_distance_diopters: float
    frame_number: int
    sensor_timestamp_ns: int


class ErrorBody(PublicModel):
    code: str
    message: str
    daemon_code: Optional[str] = None


class ErrorEnvelope(PublicModel):
    error: ErrorBody


class CameraStatusModel(PublicModel):
    camera: CameraStatusName
    mode: ModeStatusName
    state: CameraStateName
    preview_width: int
    preview_height: int
    preview_fps: int
    result: Optional[ResultMetadata] = None
    last_error: Optional[ErrorBody] = None


class SizeModel(PublicModel):
    width: int
    height: int


class CapabilitiesModel(PublicModel):
    preview_sizes: list[SizeModel]
    still_sizes: list[SizeModel]
    focus_modes: list[str]
    white_balance_modes: list[str]
    ae_lock_supported: Optional[bool] = None
    awb_lock_supported: Optional[bool] = None
    exposure_compensation_min: Optional[int] = None
    exposure_compensation_max: Optional[int] = None
    manual_exposure_time_min_ns: Optional[int] = None
    manual_exposure_time_max_ns: Optional[int] = None
    iso_min: Optional[int] = None
    iso_max: Optional[int] = None
    focus_distance_min_diopters: Optional[float] = None
    focus_distance_max_diopters: Optional[float] = None


class ControlsModel(PublicModel):
    focus_mode: Optional[str] = None
    white_balance_mode: Optional[str] = None
    ae_lock: Optional[bool] = None
    awb_lock: Optional[bool] = None
    exposure_compensation: Optional[int] = None
    manual_exposure_time_ns: Optional[int] = None
    manual_sensitivity_iso: Optional[int] = None
    manual_focus_distance_diopters: Optional[float] = None


class ControlsPatch(PublicModel):
    model_config = ConfigDict(
        extra="forbid",
        json_schema_extra={"description": "Omitted fields are unchanged; explicit null values are rejected."},
    )
    focus_mode: Optional[str] = None
    white_balance_mode: Optional[str] = None
    ae_lock: Optional[bool] = None
    awb_lock: Optional[bool] = None
    exposure_compensation: Optional[int] = None
    manual_exposure_time_ns: Optional[int] = None
    manual_sensitivity_iso: Optional[int] = None
    manual_focus_distance_diopters: Optional[float] = None

    @model_validator(mode="before")
    @classmethod
    def reject_nulls(cls, obj: Any) -> Any:
        if isinstance(obj, dict):
            null_fields = [name for name in cls.model_fields if name in obj and obj[name] is None]
            if null_fields:
                raise ValueError("control fields cannot be null: " + ", ".join(null_fields))
        return obj

    @classmethod
    def __get_pydantic_json_schema__(cls, core_schema: Any, handler: Any) -> dict[str, Any]:
        schema = handler(core_schema)
        for property_schema in schema.get("properties", {}).values():
            if "anyOf" in property_schema:
                property_schema["anyOf"] = [
                    item for item in property_schema["anyOf"]
                    if item != {"type": "null"}
                ]
        return schema


class CaptureResultModel(PublicModel):
    width: int
    height: int
    frame_number: int
    result: Optional[ResultMetadata] = None


class EmptyModel(PublicModel):
    pass


class StatusSnapshot(PublicModel):
    sequence: int
    status: CameraStatusModel


CAMERAS = {CameraName.rear: 1, CameraName.front: 2}
MODES = {ModeName.photo: 1, ModeName.video: 2}
ENUMS = {
    "camera": tuple(x.value for x in CameraStatusName),
    "mode": tuple(x.value for x in ModeStatusName),
    "state": tuple(x.value for x in CameraStateName),
    "ae_state": tuple(x.value for x in AeStateName),
    "af_state": tuple(x.value for x in AfStateName),
    "awb_state": tuple(x.value for x in AwbStateName),
}


def _enum(field: str, value: int) -> str:
    values = ENUMS[field]
    return values[value] if 0 <= value < len(values) else "unspecified"


def result_json(result: Any) -> dict[str, Any]:
    return {"ae_state": _enum("ae_state", result.ae_state), "af_state": _enum("af_state", result.af_state),
            "awb_state": _enum("awb_state", result.awb_state), "exposure_time_ns": result.exposure_time_ns,
            "sensitivity_iso": result.sensitivity_iso, "frame_duration_ns": result.frame_duration_ns,
            "focus_distance_diopters": result.focus_distance_diopters, "frame_number": result.frame_number,
            "sensor_timestamp_ns": result.sensor_timestamp_ns}


def status_json(status: CameraStatus) -> dict[str, Any]:
    out: dict[str, Any] = {"camera": _enum("camera", status.camera), "mode": _enum("mode", status.mode),
                           "state": _enum("state", status.state), "preview_width": status.preview_width,
                           "preview_height": status.preview_height, "preview_fps": status.preview_fps}
    if status.HasField("result"):
        out["result"] = result_json(status.result)
    if status.HasField("last_error"):
        code = status.last_error.DESCRIPTOR.fields_by_name["code"].enum_type.values_by_number[status.last_error.code].name
        out["last_error"] = {"code": code, "message": status.last_error.message}
    return out


def update_json(update: StatusUpdate) -> dict[str, Any]:
    return {"sequence": update.sequence, "status": status_json(update.status)}


def message_json(message: Any) -> dict[str, Any]:
    if message.HasField("video"):
        v = message.video
        states = ("unspecified", "idle", "starting", "recording", "stopping", "completed", "error")
        out = {"state": states[v.state] if 0 <= v.state < len(states) else "unspecified",
               "frames_received": v.frames_received, "frames_transport_dropped": v.frames_transport_dropped,
               "frames_encoded": v.frames_encoded, "last_video_timestamp_ns": v.last_video_timestamp_ns,
               "bytes_written": v.bytes_written}
        if v.path: out["path"] = v.path
        if v.last_error: out["last_error"] = v.last_error
        return out
    if message.HasField("state"):
        return status_json(message.state)
    if message.HasField("capabilities"):
        c = message.capabilities
        out: dict[str, Any] = {"preview_sizes": [{"width": x.width, "height": x.height} for x in c.preview_sizes],
                               "still_sizes": [{"width": x.width, "height": x.height} for x in c.still_sizes],
                               "focus_modes": list(c.focus_modes), "white_balance_modes": list(c.white_balance_modes)}
        for name in ("ae_lock_supported", "awb_lock_supported", "exposure_compensation_min", "exposure_compensation_max",
                     "manual_exposure_time_min_ns", "manual_exposure_time_max_ns", "iso_min", "iso_max",
                     "focus_distance_min_diopters", "focus_distance_max_diopters"):
            if c.HasField(name):
                out[name] = getattr(c, name)
        return out
    if message.HasField("controls"):
        c = message.controls
        return {name: getattr(c, name) for name in ("focus_mode", "white_balance_mode", "ae_lock", "awb_lock",
                                                     "exposure_compensation", "manual_exposure_time_ns",
                                                     "manual_sensitivity_iso", "manual_focus_distance_diopters")
                if c.HasField(name)}
    if message.HasField("capture"):
        c = message.capture
        return {"width": c.width, "height": c.height, "frame_number": c.frame_number,
                "result": result_json(c.result) if c.HasField("result") else None}
    return {}


def _jpeg(raw: bytes, layout: dict[str, Any]) -> bytes:
    try:
        import numpy as np
        from PIL import Image
    except ImportError as exc:
        raise RuntimeError("JPEG export requires Pillow") from exc
    planes = layout.get("planes")
    if not isinstance(planes, list) or len(planes) < 2:
        raise ValueError("mapper layout does not contain Y and chroma planes")
    width, height = int(layout["width"]), int(layout["height"])
    y, uv = planes[0], planes[1]
    y_stride, uv_stride = int(y["row_stride"]), int(uv["row_stride"])
    y_inc, uv_inc = max(1, int(y["sample_increment_bits"]) // 8), max(1, int(uv["sample_increment_bits"]) // 8)
    y_off, uv_off = int(y["offset"]), int(uv["offset"])
    components = {int(x["value"]): int(x["offset_bits"]) // 8 for x in uv.get("components", [])}
    cr, cb = components.get(4), components.get(2)
    if cr is None or cb is None:
        raise ValueError("mapper chroma components are incomplete")
    needed = max(y_off + (height - 1) * y_stride + (width - 1) * y_inc + 1,
                 uv_off + (height // 2 - 1) * uv_stride + (width // 2 - 1) * uv_inc + max(cr, cb) + 1)
    if needed > len(raw):
        raise ValueError("last.raw is shorter than its mapper layout")
    source = np.frombuffer(raw, dtype=np.uint8)
    rows = np.arange(height, dtype=np.int64)[:, None]
    columns = np.arange(width, dtype=np.int64)[None, :]
    y_indices = y_off + rows * y_stride + columns * y_inc
    uv_indices = uv_off + (rows // 2) * uv_stride + (columns // 2) * uv_inc
    y_plane = source[y_indices]
    cb_plane = source[uv_indices + cb]
    cr_plane = source[uv_indices + cr]
    ycbcr = np.stack((y_plane, cb_plane, cr_plane), axis=-1)
    image = Image.fromarray(ycbcr, mode="YCbCr").convert("RGB")
    out = io.BytesIO()
    image.save(out, format="JPEG", quality=90, optimize=True)
    return out.getvalue()


class Bridge:
    def __init__(self) -> None:
        self._clients: set[tuple[asyncio.AbstractEventLoop, asyncio.Queue[dict[str, Any]]]] = set()
        self._lock = threading.Lock()
        self.ipc = CameraIpc(self._status)

    def _status(self, update: StatusUpdate) -> None:
        payload = update_json(update)
        with self._lock:
            clients = tuple(self._clients)
        for loop, queue in clients:
            loop.call_soon_threadsafe(self._enqueue, queue, payload)

    @staticmethod
    def _enqueue(queue: asyncio.Queue[dict[str, Any]], payload: dict[str, Any]) -> None:
        if queue.full():
            try:
                queue.get_nowait()
            except asyncio.QueueEmpty:
                pass
        try:
            queue.put_nowait(payload)
        except asyncio.QueueFull:
            pass

    def subscribe(self, loop: asyncio.AbstractEventLoop, queue: asyncio.Queue[dict[str, Any]]) -> None:
        with self._lock:
            self._clients.add((loop, queue))

    def unsubscribe(self, loop: asyncio.AbstractEventLoop, queue: asyncio.Queue[dict[str, Any]]) -> None:
        with self._lock:
            self._clients.discard((loop, queue))

    def close(self) -> None:
        self.ipc.close()


class BridgeError(Exception):
    def __init__(self, status_code: int, message: str, daemon_code: Optional[str] = None) -> None:
        self.status_code, self.message, self.daemon_code = status_code, message, daemon_code


DAEMON_STATUS = {"INVALID_REQUEST": 400, "INVALID_STATE": 409, "UNSUPPORTED": 422, "BUSY": 409,
                 "CAMERA_UNAVAILABLE": 503, "CAPTURE_FAILED": 502, "INTERNAL_ERROR": 500}


def call(bridge: Bridge, fn: Any) -> Any:
    try:
        return fn()
    except CameraDaemonError as exc:
        raise BridgeError(DAEMON_STATUS.get(exc.code, 502), exc.message, exc.code) from exc
    except (CameraIpcError, OSError) as exc:
        raise BridgeError(502, str(exc)) from exc


def error_response(status_code: int, message: str, daemon_code: Optional[str] = None) -> JSONResponse:
    body: dict[str, Any] = {"code": HTTPStatus(status_code).name.lower(), "message": message}
    if daemon_code:
        body["daemon_code"] = daemon_code
    return JSONResponse(status_code=status_code, content={"error": body})


@asynccontextmanager
async def lifespan(app: FastAPI):
    app.state.bridge = Bridge()
    try:
        yield
    finally:
        app.state.bridge.close()


ERROR_RESPONSES = {
    code: {"description": "Structured error response", "model": ErrorEnvelope}
    for code in (400, 404, 409, 422, 502, 503)
}
app = FastAPI(title="TB330FU Camera Bridge", version="1.0.0",
              description="App-facing camera API over the native protobuf daemon.", lifespan=lifespan)


@app.middleware("http")
async def no_store_middleware(request: Request, call_next: Any) -> Response:
    response = await call_next(request)
    response.headers.setdefault("Cache-Control", "no-store")
    return response


@app.exception_handler(BridgeError)
async def bridge_error_handler(_: Request, exc: BridgeError) -> JSONResponse:
    return error_response(exc.status_code, exc.message, exc.daemon_code)


@app.exception_handler(RequestValidationError)
async def validation_error_handler(_: Request, exc: RequestValidationError) -> JSONResponse:
    return error_response(422, "request validation failed: " + "; ".join(e["msg"] for e in exc.errors()))


@app.exception_handler(StarletteHTTPException)
async def http_error_handler(_: Request, exc: StarletteHTTPException) -> JSONResponse:
    return error_response(exc.status_code, str(exc.detail))


def get_bridge(request: Request) -> Bridge:
    return request.app.state.bridge


@app.get("/v1/state", operation_id="getState", response_model=CameraStatusModel, response_model_exclude_none=True, responses=ERROR_RESPONSES)
def get_state(request: Request) -> Any:
    b = get_bridge(request)
    return message_json(call(b, b.ipc.get_state))


@app.get("/v1/capabilities", operation_id="getCapabilities", response_model=CapabilitiesModel, response_model_exclude_none=True, responses=ERROR_RESPONSES)
def get_capabilities(request: Request) -> Any:
    b = get_bridge(request)
    return message_json(call(b, b.ipc.get_capabilities))


@app.get("/v1/controls", operation_id="getControls", response_model=ControlsModel, response_model_exclude_none=True, responses=ERROR_RESPONSES)
def get_controls(request: Request) -> Any:
    b = get_bridge(request)
    return message_json(call(b, b.ipc.get_controls))


@app.put("/v1/camera", operation_id="setCamera", response_model=EmptyModel, responses=ERROR_RESPONSES)
def set_camera(request: Request, body: CameraSelection) -> Any:
    b = get_bridge(request)
    return message_json(call(b, lambda: b.ipc.set_camera(CAMERAS[body.camera])))


@app.put("/v1/mode", operation_id="setMode", response_model=EmptyModel, responses=ERROR_RESPONSES)
def set_mode(request: Request, body: ModeSelection) -> Any:
    b = get_bridge(request)
    return message_json(call(b, lambda: b.ipc.set_mode(MODES[body.mode])))


@app.patch("/v1/controls", operation_id="setControls", response_model=ControlsModel, response_model_exclude_none=True, responses=ERROR_RESPONSES)
def set_controls(request: Request, body: ControlsPatch) -> Any:
    controls = SetControlsRequest()
    for name, value in body.model_dump(exclude_unset=True).items():
        setattr(controls, name, value)
    b = get_bridge(request)
    return message_json(call(b, lambda: b.ipc.set_controls(controls)))


@app.post("/v1/autofocus", operation_id="autofocus", response_model=EmptyModel, responses=ERROR_RESPONSES)
def autofocus(request: Request) -> Any:
    b = get_bridge(request)
    return message_json(call(b, b.ipc.trigger_autofocus))


@app.post("/v1/capture", operation_id="capture", response_model=CaptureResultModel, responses=ERROR_RESPONSES)
def capture(request: Request) -> Any:
    b = get_bridge(request)
    return message_json(call(b, b.ipc.capture_still))


@app.post("/video/start", operation_id="videoStart", responses=ERROR_RESPONSES)
def video_start(request: Request) -> Any:
    b = get_bridge(request)
    return message_json(call(b, b.ipc.video_start))


@app.get("/video/status", operation_id="videoStatus", responses=ERROR_RESPONSES)
def video_status(request: Request) -> Any:
    b = get_bridge(request)
    return message_json(call(b, b.ipc.video_status))


@app.post("/video/stop", operation_id="videoStop", responses=ERROR_RESPONSES)
def video_stop(request: Request) -> Any:
    b = get_bridge(request)
    return message_json(call(b, b.ipc.video_stop))


@app.get("/v1/last-shot", operation_id="getLastShot", response_class=Response, responses={
    **ERROR_RESPONSES,
    200: {"description": "Raw or JPEG still image", "content": {
        "application/octet-stream": {}, "image/jpeg": {}
    }},
})
def last_shot(request: Request, shot_type: Literal["raw", "jpg"] = Query("raw", alias="type")) -> Response:
    del request
    try:
        if shot_type == "raw":
            data, media_type = RAW_PATH.read_bytes(), "application/octet-stream"
        else:
            data = _jpeg(RAW_PATH.read_bytes(), json.loads(LAYOUT_PATH.read_text()))
            media_type = "image/jpeg"
    except FileNotFoundError as exc:
        raise BridgeError(404, "no still capture available") from exc
    except (ValueError, RuntimeError, KeyError, json.JSONDecodeError) as exc:
        raise BridgeError(422, str(exc)) from exc
    return Response(content=data, media_type=media_type, headers={"Cache-Control": "no-store"})


@app.websocket("/v1/events")
async def events(websocket: WebSocket) -> None:
    await websocket.accept()
    b: Bridge = websocket.app.state.bridge
    loop = asyncio.get_running_loop()
    queue: asyncio.Queue[dict[str, Any]] = asyncio.Queue(maxsize=1)
    b.subscribe(loop, queue)
    try:
        current = b.ipc.latest_status
        if current:
            await websocket.send_json(update_json(current))
        while True:
            await websocket.send_json(await queue.get())
    except (WebSocketDisconnect, RuntimeError):
        pass
    finally:
        b.unsubscribe(loop, queue)


def main() -> None:
    import uvicorn
    parser = argparse.ArgumentParser()
    parser.add_argument("--listen", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=3909)
    args = parser.parse_args()
    uvicorn.run(app, host=args.listen, port=args.port, log_level="info")


if __name__ == "__main__":
    main()
