#!/usr/bin/env python3
"""Verify generated Python protobuf bindings preserve the v1 IPC contract."""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))
from tb330fu_camera_pb2 import CameraCapabilities, CameraControls, Request, Response, StatusUpdate  # noqa: E402

def round_trip(message):
    wire = message.SerializeToString()
    parsed = type(message)()
    if not parsed.ParseFromString(wire) or parsed != message:
        raise SystemExit(f"generated Python round-trip failed for {type(message).__name__}")
    return wire

request = Request(request_id=42)
request.set_controls.focus_mode = "continuous_picture"
request.set_controls.ae_lock = True
request.set_controls.manual_exposure_time_ns = 10_000_000
request.set_controls.manual_sensitivity_iso = 400
request.set_controls.manual_focus_distance_diopters = 1.25
round_trip(request)

response = Response(request_id=42)
response.capture.width = 3264
response.capture.height = 2448
response.capture.frame_number = 17
response.capture.result.ae_state = 3
response.capture.result.sensitivity_iso = 400
response.capture.path = "/run/tb330fu-camera/last.raw"
response.capture.jpeg_path = "/run/tb330fu-camera/last.jpg"
response.capture.raw_path = "/run/tb330fu-camera/last.raw"
response.capture.raw_metadata_path = "/run/tb330fu-camera/last-metadata.json"
round_trip(response)

update = StatusUpdate(sequence=9)
update.status.camera = 1
update.status.mode = 1
update.status.state = 3
update.status.preview_width = 1920
update.status.preview_height = 1080
update.status.preview_fps = 30
update.status.result.frame_number = 17
round_trip(update)

capabilities = CameraCapabilities()
capabilities.ae_lock_supported = False
capabilities.exposure_compensation_min = 0
round_trip(capabilities)
controls = CameraControls()
controls.ae_lock = False
controls.manual_focus_distance_diopters = 0.0
round_trip(controls)

for message, expected in ((request, (15 << 3) | 2), (response, (14 << 3) | 2), (update, (2 << 3) | 2)):
    if expected not in round_trip(message):
        raise SystemExit(f"expected v1 field {expected} missing from {type(message).__name__}")
subprocess.run([sys.executable, str(ROOT / "regenerate_camera_proto.py"), "--check"], check=True)
print("generated Python protobuf contract checks: PASS")
