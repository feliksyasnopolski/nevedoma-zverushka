#!/usr/bin/env python3
"""Regenerate checked-in bindings from the one authoritative schema."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent
PROTO = ROOT / "tb330fu_camera.proto"
GENERATED = (ROOT / "tb330fu_camera.pb.h", ROOT / "tb330fu_camera.pb.cc", ROOT / "tb330fu_camera_pb2.py")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--protoc", default=os.environ.get("PROTOC", "protoc"))
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    protoc = shutil.which(args.protoc) or args.protoc
    with tempfile.TemporaryDirectory(prefix="tb330fu-camera-proto-") as tmp:
        subprocess.run([protoc, "--experimental_allow_proto3_optional", "--cpp_out=lite:" + tmp, "--python_out=" + tmp, "-I", str(ROOT), str(PROTO)], check=True)
        fresh = (Path(tmp) / "tb330fu_camera.pb.h", Path(tmp) / "tb330fu_camera.pb.cc", Path(tmp) / "tb330fu_camera_pb2.py")
        if args.check:
            for expected, actual in zip(GENERATED, fresh):
                if expected.read_bytes() != actual.read_bytes(): raise SystemExit(f"stale generated file: {expected.name}")
        else:
            for expected, actual in zip(GENERATED, fresh): expected.write_bytes(actual.read_bytes())

if __name__ == "__main__": main()
