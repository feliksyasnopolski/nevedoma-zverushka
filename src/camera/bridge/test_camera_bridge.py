#!/usr/bin/env python3
"""Focused contract and transport tests for the camera bridge."""
from __future__ import annotations

import io
import threading
import unittest
from unittest.mock import patch

from fastapi.testclient import TestClient
from PIL import Image

import camera_bridge
import camera_ipc
from tb330fu_camera_pb2 import Request, Response


class FakeIpc:
    def __init__(self, on_status):
        self.last_controls = None

    def close(self):
        pass


class PatchIpc(FakeIpc):
    def set_controls(self, controls):
        self.last_controls = controls
        values = {}
        for name in controls.DESCRIPTOR.fields_by_name:
            if controls.HasField(name):
                values[name] = getattr(controls, name)
        return Response(controls=values)


class BridgeContractTests(unittest.TestCase):
    def test_controls_presence_and_null_semantics(self):
        model = camera_bridge.ControlsPatch.model_validate({"exposure_compensation": 0, "ae_lock": False})
        self.assertEqual(model.model_dump(exclude_unset=True), {"exposure_compensation": 0, "ae_lock": False})
        self.assertEqual(camera_bridge.ControlsPatch().model_dump(exclude_unset=True), {})
        with self.assertRaises(ValueError):
            camera_bridge.ControlsPatch.model_validate({"exposure_compensation": None})
        with self.assertRaises(ValueError):
            camera_bridge.ControlsPatch.model_validate({"ae_lock": None})
        with self.assertRaises(ValueError):
            camera_bridge.ControlsPatch.model_validate({"not_a_control": 1})

    def test_http_patch_preserves_presence_and_rejects_null(self):
        with patch.object(camera_bridge, "CameraIpc", PatchIpc):
            with TestClient(camera_bridge.app) as client:
                self.assertEqual(client.patch("/v1/controls", json={"exposure_compensation": 0}).status_code, 200)
                ipc = camera_bridge.app.state.bridge.ipc
                self.assertTrue(ipc.last_controls.HasField("exposure_compensation"))
                self.assertEqual(ipc.last_controls.exposure_compensation, 0)
                self.assertEqual(client.patch("/v1/controls", json={"ae_lock": False}).status_code, 200)
                self.assertTrue(ipc.last_controls.HasField("ae_lock"))
                self.assertFalse(ipc.last_controls.ae_lock)
                self.assertEqual(client.patch("/v1/controls", json={"ae_lock": None}).status_code, 422)
                self.assertEqual(client.patch("/v1/controls", json={"unknown": 1}).status_code, 422)

    def test_openapi_freeze(self):
        paths = camera_bridge.app.openapi()["paths"]
        expected_ids = {
            "/v1/state": "getState", "/v1/capabilities": "getCapabilities", "/v1/controls": "getControls",
            "/v1/camera": "setCamera", "/v1/mode": "setMode", "/v1/autofocus": "autofocus",
            "/v1/capture": "capture", "/v1/last-shot": "getLastShot",
        }
        for path, operation_id in expected_ids.items():
            operation = next(iter(paths[path].values()))
            self.assertEqual(operation["operationId"], operation_id)
        last_shot = paths["/v1/last-shot"]["get"]
        self.assertEqual(last_shot["parameters"][0]["schema"]["enum"], ["raw", "jpg"])
        self.assertEqual(set(last_shot["responses"]["200"]["content"]), {"application/octet-stream", "image/jpeg"})
        self.assertNotIn("path", camera_bridge.app.openapi()["components"]["schemas"]["CaptureResultModel"]["properties"])

    def test_vectorized_jpeg_honors_strides_and_offsets(self):
        width, height = 4, 4
        raw = bytearray(80)
        y_offset, uv_offset = 3, 40
        y_stride, uv_stride = 8, 7
        for row in range(height):
            for col in range(width):
                raw[y_offset + row * y_stride + col] = 80 + row * 10 + col
        for row in range(height // 2):
            for col in range(width // 2):
                base = uv_offset + row * uv_stride + col * 2
                raw[base] = 100 + row * 5 + col
                raw[base + 1] = 150 + row * 5 + col
        layout = {"width": width, "height": height, "planes": [
            {"offset": y_offset, "row_stride": y_stride, "sample_increment_bits": 8},
            {"offset": uv_offset, "row_stride": uv_stride, "sample_increment_bits": 16,
             "components": [{"value": 2, "offset_bits": 0}, {"value": 4, "offset_bits": 8}]},
        ]}
        encoded = camera_bridge._jpeg(bytes(raw), layout)
        with Image.open(io.BytesIO(encoded)) as image:
            self.assertEqual(image.size, (width, height))
            self.assertGreater(image.convert("L").getextrema()[1], image.convert("L").getextrema()[0])


class FakeSocket:
    def __init__(self, *, connect_error=False, receive_error=False):
        self.connect_error = connect_error
        self.receive_error = receive_error
        self.sent = []
        self.closed = False

    def connect(self, path):
        if self.connect_error:
            raise OSError("connect failed")

    def send(self, payload):
        self.sent.append(payload)
        return len(payload)

    def recv(self, size):
        if self.receive_error:
            raise ConnectionResetError("response lost")
        request = Request.FromString(self.sent[-1])
        return Response(request_id=request.request_id).SerializeToString()

    def close(self):
        self.closed = True

    def shutdown(self, how):
        self.closed = True


def bare_ipc():
    ipc = object.__new__(camera_ipc.CameraIpc)
    ipc.control_path = "control"
    ipc._control = None
    ipc._control_lock = threading.Lock()
    ipc._id_lock = threading.Lock()
    ipc._next_id = 1
    return ipc


class CameraIpcTransportTests(unittest.TestCase):
    def test_request_id_allocation_is_unique(self):
        ipc = bare_ipc()
        ids = []
        lock = threading.Lock()

        def allocate():
            request_id = ipc._new().request_id
            with lock:
                ids.append(request_id)

        threads = [threading.Thread(target=allocate) for _ in range(100)]
        for thread in threads: thread.start()
        for thread in threads: thread.join()
        self.assertEqual(sorted(ids), list(range(1, 101)))

    def test_connect_failure_before_send_retries(self):
        ipc = bare_ipc()
        sockets = [FakeSocket(connect_error=True), FakeSocket()]
        with patch.object(camera_ipc.socket, "socket", side_effect=sockets):
            response = ipc._request(Request(request_id=17))
        self.assertEqual(response.request_id, 17)
        self.assertEqual(len(sockets[1].sent), 1)

    def test_successful_transaction(self):
        ipc = bare_ipc()
        sock = FakeSocket()
        with patch.object(camera_ipc.socket, "socket", return_value=sock):
            response = ipc._request(Request(request_id=18))
        self.assertEqual(response.request_id, 18)
        self.assertEqual(len(sock.sent), 1)

    def test_post_send_failure_is_not_replayed(self):
        ipc = bare_ipc()
        sock = FakeSocket(receive_error=True)
        with patch.object(camera_ipc.socket, "socket", return_value=sock):
            with self.assertRaises(camera_ipc.CameraIpcError):
                ipc._request(Request(request_id=19))
        self.assertEqual(len(sock.sent), 1)

    def test_close_unblocks_event_reader(self):
        class BlockingSocket(FakeSocket):
            def __init__(self):
                super().__init__()
                self.released = threading.Event()

            def recv(self, size):
                self.released.wait(2)
                raise OSError("socket closed")

            def shutdown(self, how):
                self.closed = True
                self.released.set()

        sock = BlockingSocket()
        with patch.object(camera_ipc.socket, "socket", return_value=sock):
            ipc = camera_ipc.CameraIpc(events_path="events")
            self.assertTrue(sock.released.wait(0.2) is False)
            ipc.close()
        self.assertFalse(ipc._events_thread.is_alive())


if __name__ == "__main__":
    unittest.main()
