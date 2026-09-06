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

def normalized_bytes(path: Path) -> bytes:
    """Keep protoc output reproducible without generated trailing whitespace."""
    return b"\n".join(line.rstrip() for line in path.read_bytes().splitlines()) + b"\n"

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
                if normalized_bytes(expected) != normalized_bytes(actual): raise SystemExit(f"stale generated file: {expected.name}")
        else:
            for expected, actual in zip(GENERATED, fresh): expected.write_bytes(normalized_bytes(actual))

if __name__ == "__main__": main()
