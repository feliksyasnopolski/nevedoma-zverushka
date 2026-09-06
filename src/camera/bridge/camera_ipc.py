#!/usr/bin/env python3
"""Reusable protobuf transport for the TB330FU camera daemon."""
from __future__ import annotations

import logging
import socket
import threading
from typing import Callable, Optional

from tb330fu_camera_pb2 import (
    CaptureStillRequest, GetCapabilitiesRequest, GetControlsRequest,
    GetStateRequest, Request, Response, SetCameraRequest, SetControlsRequest,
    SetModeRequest, StatusUpdate, TriggerAutofocusRequest,
    VideoStartRequest, VideoStopRequest, VideoStatusRequest,
)

CONTROL_SOCKET = "/run/tb330fu-camera/control.sock"
EVENTS_SOCKET = "/run/tb330fu-camera/events.sock"
MAX_PACKET = 64 * 1024
LOGGER = logging.getLogger(__name__)


class CameraIpcError(RuntimeError):
    pass


class CameraDaemonError(CameraIpcError):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code
        self.message = message


class CameraIpc:
    """Thread-safe control client plus an independent complete-snapshot reader."""

    def __init__(self, on_status: Optional[Callable[[StatusUpdate], None]] = None,
                 control_path: str = CONTROL_SOCKET, events_path: str = EVENTS_SOCKET):
        self.control_path = control_path
        self.events_path = events_path
        self._control: Optional[socket.socket] = None
        self._control_lock = threading.Lock()
        self._id_lock = threading.Lock()
        self._next_id = 1
        self._status_lock = threading.Lock()
        self._latest: Optional[StatusUpdate] = None
        self._on_status = on_status
        self._stop = threading.Event()
        self._events_socket_lock = threading.Lock()
        self._events_socket: Optional[socket.socket] = None
        self._events_thread = threading.Thread(target=self._events_loop, name="camera-events", daemon=True)
        self._events_thread.start()

    @property
    def latest_status(self) -> Optional[StatusUpdate]:
        with self._status_lock:
            return StatusUpdate.FromString(self._latest.SerializeToString()) if self._latest else None

    def close(self) -> None:
        self._stop.set()
        with self._control_lock:
            self._close_control()
        with self._events_socket_lock:
            if self._events_socket is not None:
                try:
                    self._events_socket.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                self._events_socket.close()
                self._events_socket = None
        self._events_thread.join()

    def _close_control(self) -> None:
        if self._control is not None:
            try:
                self._control.close()
            finally:
                self._control = None

    def _connect_control(self) -> socket.socket:
        if self._control is None:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            try:
                sock.connect(self.control_path)
            except Exception:
                sock.close()
                raise
            self._control = sock
        return self._control

    def _request(self, request: Request) -> Response:
        wire = request.SerializeToString()
        with self._control_lock:
            for attempt in range(2):
                try:
                    sock = self._connect_control()
                except (OSError, ConnectionError):
                    self._close_control()
                    if attempt == 1:
                        raise CameraIpcError("camera control connection lost")
                    continue
                try:
                    sent = sock.send(wire)
                    if sent != len(wire):
                        self._close_control()
                        raise CameraIpcError("camera control request was only partially sent")
                except OSError:
                    self._close_control()
                    if attempt == 1:
                        raise CameraIpcError("camera control request could not be sent")
                    continue

                # A successful send is the at-most-once boundary. Any later
                # transport failure must not replay a potentially executed command.
                try:
                    packet = sock.recv(MAX_PACKET)
                    if not packet:
                        raise ConnectionError("camera control socket closed")
                    response = Response()
                    if not response.ParseFromString(packet):
                        raise CameraIpcError("malformed protobuf Response")
                    if response.request_id != request.request_id:
                        raise CameraIpcError(
                            f"response request_id {response.request_id} != {request.request_id}")
                    if response.HasField("error"):
                        raise CameraDaemonError(
                            response.error.code and response.error.DESCRIPTOR.fields_by_name["code"].enum_type.values_by_number[response.error.code].name or "ERROR",
                            response.error.message)
                    return response
                except (OSError, ConnectionError) as exc:
                    self._close_control()
                    raise CameraIpcError("camera control response was lost after send") from exc
        raise AssertionError("unreachable")

    def _new(self) -> Request:
        with self._id_lock:
            request_id = self._next_id
            self._next_id += 1
        return Request(request_id=request_id)

    def get_state(self) -> Response:
        r = self._new(); r.get_state.CopyFrom(GetStateRequest()); return self._request(r)
    def set_camera(self, camera: int) -> Response:
        r = self._new(); r.set_camera.CopyFrom(SetCameraRequest(camera=camera)); return self._request(r)
    def set_mode(self, mode: int) -> Response:
        r = self._new(); r.set_mode.CopyFrom(SetModeRequest(mode=mode)); return self._request(r)
    def get_capabilities(self) -> Response:
        r = self._new(); r.get_capabilities.CopyFrom(GetCapabilitiesRequest()); return self._request(r)
    def get_controls(self) -> Response:
        r = self._new(); r.get_controls.CopyFrom(GetControlsRequest()); return self._request(r)
    def set_controls(self, controls: SetControlsRequest) -> Response:
        r = self._new(); r.set_controls.CopyFrom(controls); return self._request(r)
    def trigger_autofocus(self) -> Response:
        r = self._new(); r.trigger_autofocus.CopyFrom(TriggerAutofocusRequest()); return self._request(r)
    def capture_still(self) -> Response:
        r = self._new(); r.capture_still.CopyFrom(CaptureStillRequest()); return self._request(r)
    def video_start(self) -> Response:
        r = self._new(); r.video_start.CopyFrom(VideoStartRequest()); return self._request(r)
    def video_stop(self) -> Response:
        r = self._new(); r.video_stop.CopyFrom(VideoStopRequest()); return self._request(r)
    def video_status(self) -> Response:
        r = self._new(); r.video_status.CopyFrom(VideoStatusRequest()); return self._request(r)

    def _events_loop(self) -> None:
        while not self._stop.is_set():
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            with self._events_socket_lock:
                if self._stop.is_set():
                    sock.close()
                    return
                self._events_socket = sock
            try:
                sock.connect(self.events_path)
                while not self._stop.is_set():
                    packet = sock.recv(MAX_PACKET)
                    if not packet:
                        raise ConnectionError("camera events socket closed")
                    update = StatusUpdate()
                    if not update.ParseFromString(packet):
                        continue
                    with self._status_lock:
                        self._latest = update
                    if self._on_status:
                        try:
                            self._on_status(update)
                        except Exception:
                            LOGGER.exception("camera status callback failed")
            except (OSError, ConnectionError):
                if not self._stop.wait(0.25):
                    continue
            finally:
                with self._events_socket_lock:
                    if self._events_socket is sock:
                        self._events_socket = None
                sock.close()
