#!/usr/bin/env python3
"""Regenerate the checked-in OpenAPI contract for the camera bridge."""
from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT.parent / "protocol"))
from camera_bridge import app


(ROOT / "openapi.json").write_text(json.dumps(app.openapi(), indent=2, sort_keys=True) + "\n")
