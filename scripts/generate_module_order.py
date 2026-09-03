#!/usr/bin/env python3
"""Generate the physically available hard closure of stock modules.load."""

import argparse
import hashlib
import json
from pathlib import Path


def module_name(path):
    return Path(path).name.removesuffix(".ko")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("module_dir", type=Path)
    ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--report", type=Path, required=True)
    args = ap.parse_args()
    d = args.module_dir

    requested = [x.strip() for x in (d / "modules.load").read_text().splitlines()
                 if x.strip() and not x.startswith("#")]
    requested_files = [x if x.endswith(".ko") else x + ".ko" for x in requested]
    request_pos = {x: i for i, x in enumerate(requested_files)}
    available = {p.name for p in d.glob("*.ko")}

    deps = {}
    for line in (d / "modules.dep").read_text().splitlines():
        if not line.strip() or ":" not in line:
            continue
        consumer, raw = line.split(":", 1)
        consumer = Path(consumer.strip()).name
        deps[consumer] = [Path(x).name for x in raw.split()]

    closure = set()
    missing_hard = set()

    def visit(node):
        if node in closure:
            return
        closure.add(node)
        for dep in deps.get("/lib/modules/" + node, deps.get(node, [])):
            if dep in available:
                visit(dep)
            else:
                missing_hard.add(dep)

    for node in requested_files:
        visit(node)

    # Stable DFS: request order first, then the authoritative modules.dep order.
    order = []
    seen = set()

    def emit(node):
        if node in seen:
            return
        seen.add(node)
        for dep in deps.get(node, []):
            if dep in closure:
                emit(dep)
        order.append(node)

    for node in requested_files:
        emit(node)
    for node in sorted(closure):
        emit(node)

    index = {x: i for i, x in enumerate(order)}
    inversions = []
    for consumer in sorted(closure):
        for dep in deps.get(consumer, []):
            if dep in closure and index[dep] > index[consumer]:
                inversions.append({"consumer": consumer, "dependency": dep})

    softdeps = (d / "modules.softdep").read_text().splitlines()
    soft_aliases = {
        "mt6685_core": ("mt6685_core", "mt6685-core"),
        "mtk_dvfsrc_devfreq": ("mtk_dvfsrc_devfreq", "mtk-dvfsrc-devfreq"),
    }
    soft_targets = [name for name, aliases in soft_aliases.items()
                    if any(alias in line for line in softdeps for alias in aliases)]
    result = {
        "requested_count": len(requested_files),
        "closure_count": len(closure),
        "available_ko_count": len(available),
        "requested": requested_files,
        "closure": sorted(closure),
        "order": order,
        "missing_hard_from_metadata": sorted(missing_hard),
        "softdep_targets_documented": soft_targets,
        "hard_order_inversions": inversions,
        "order_sha256": hashlib.sha256(
            ("".join(x + "\n" for x in order)).encode()
        ).hexdigest(),
    }
    if len(requested_files) != 151 or len(closure) != 154 or inversions:
        raise SystemExit(json.dumps(result, indent=2))
    args.output.write_text("".join(x + "\n" for x in order))
    args.report.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")


if __name__ == "__main__":
    main()
