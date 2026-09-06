#!/usr/bin/env python3
"""Direct protobuf/SEQPACKET v1 validation client."""
import argparse
import json
import socket
from google.protobuf.json_format import MessageToDict

from tb330fu_camera_pb2 import (
    CaptureStillRequest, GetCapabilitiesRequest, GetControlsRequest,
    GetStateRequest, Request, SetCameraRequest, SetControlsRequest,
    SetModeRequest, StatusUpdate, TriggerAutofocusRequest,
)

CONTROL = "/run/tb330fu-camera/control.sock"
EVENTS = "/run/tb330fu-camera/events.sock"

def request(req_id, command, value=None):
    out = Request(request_id=req_id)
    if command == "get_state": out.get_state.CopyFrom(GetStateRequest())
    elif command == "set_camera": out.set_camera.CopyFrom(SetCameraRequest(camera={"rear": 1, "front": 2}[value]))
    elif command == "set_mode": out.set_mode.CopyFrom(SetModeRequest(mode={"photo": 1, "video": 2}[value]))
    elif command == "get_capabilities": out.get_capabilities.CopyFrom(GetCapabilitiesRequest())
    elif command == "get_controls": out.get_controls.CopyFrom(GetControlsRequest())
    elif command == "set_controls":
        controls = SetControlsRequest()
        for item in value or []:
            name, raw = item.split("=", 1)
            if name in ("focus_mode", "white_balance_mode"):
                setattr(controls, name, raw)
            elif name in ("ae_lock", "awb_lock"):
                setattr(controls, name, raw.lower() == "true")
            elif name == "manual_focus_distance_diopters":
                setattr(controls, name, float(raw))
            else:
                setattr(controls, name, int(raw))
        out.set_controls.CopyFrom(controls)
    elif command == "trigger_autofocus": out.trigger_autofocus.CopyFrom(TriggerAutofocusRequest())
    elif command == "capture_still": out.capture_still.CopyFrom(CaptureStillRequest())
    else: raise ValueError(command)
    return out

def call(command, value=None):
    with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as sock:
        sock.connect(CONTROL)
        sock.send(request(1, command, value).SerializeToString())
        from tb330fu_camera_pb2 import Response
        response = Response()
        response.ParseFromString(sock.recv(65536))
        return response

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("operation", nargs="?", default="get_state", choices=["get_state", "set_camera", "set_mode", "get_capabilities", "get_controls", "set_controls", "trigger_autofocus", "capture_still", "events"])
    ap.add_argument("value", nargs="*")
    args = ap.parse_args()
    if args.operation == "events":
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as sock:
            sock.connect(EVENTS)
            while True:
                update = StatusUpdate(); update.ParseFromString(sock.recv(65536))
                print(json.dumps(MessageToDict(update, preserving_proto_field_name=True), sort_keys=True))
    else:
        value = args.value if args.operation == "set_controls" else (args.value[0] if args.value else None)
        print(json.dumps(MessageToDict(call(args.operation, value), preserving_proto_field_name=True), sort_keys=True))

if __name__ == "__main__": main()
