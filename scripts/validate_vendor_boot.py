#!/usr/bin/env python3
"""Independent, read-only validator for the TB330FU vendor_boot candidate."""

import argparse
import hashlib
import json
import struct
from pathlib import Path


def digest(data):
    return hashlib.sha256(data).hexdigest()


def aligned(value, page):
    return (value + page - 1) // page * page


def parse(path):
    blob = Path(path).read_bytes()
    if blob[:8] != b"VNDRBOOT":
        raise RuntimeError("bad vendor_boot magic")
    version, page = struct.unpack_from("<II", blob, 8)
    ramdisk_size = struct.unpack_from("<I", blob, 24)[0]
    cmdline = blob[28:2076].split(b"\0", 1)[0].decode("ascii")
    header_size, dtb_size = struct.unpack_from("<II", blob, 2096)
    table_size, count, entry_size, bootconfig_size = struct.unpack_from(
        "<IIII", blob, 2112
    )
    if (version, header_size, entry_size, table_size) != (
        4, 2128, 108, count * 108
    ):
        raise RuntimeError("unsupported or inconsistent v4 header/table")
    ramdisk_at = page
    dtb_at = aligned(ramdisk_at + ramdisk_size, page)
    table_at = aligned(dtb_at + dtb_size, page)
    bootconfig_at = aligned(table_at + table_size, page)
    content_size = aligned(bootconfig_at + bootconfig_size, page)
    ramdisk = blob[ramdisk_at : ramdisk_at + ramdisk_size]
    table = blob[table_at : table_at + table_size]
    entries = []
    for index in range(count):
        record = table[index * 108 : (index + 1) * 108]
        size, offset, kind = struct.unpack_from("<III", record)
        name = record[12:44].split(b"\0", 1)[0].decode("ascii")
        board_id = list(struct.unpack_from("<16I", record, 44))
        packed = ramdisk[offset : offset + size]
        if len(packed) != size:
            raise RuntimeError(f"short fragment {index}")
        entries.append({
            "index": index,
            "size": size,
            "offset": offset,
            "type": kind,
            "name": name,
            "board_id": board_id,
            "sha256": digest(packed),
            "packed": packed,
        })
    if sum(item["size"] for item in entries) != ramdisk_size:
        raise RuntimeError("fragment sizes do not sum to ramdisk size")
    footer = None
    if blob[-64:-60] == b"AVBf":
        _, major, minor, original_size, vbmeta_at, vbmeta_size = struct.unpack_from(
            ">4sIIQQQ", blob, len(blob) - 64
        )
        if original_size != content_size:
            raise RuntimeError("AVB original size is inconsistent")
        vbmeta = blob[vbmeta_at : vbmeta_at + vbmeta_size]
        if len(vbmeta) != vbmeta_size:
            raise RuntimeError("short vbmeta")
        footer = {
            "major": major,
            "minor": minor,
            "original_image_size": original_size,
            "vbmeta_offset": vbmeta_at,
            "vbmeta_size": vbmeta_size,
            "vbmeta_sha256": digest(vbmeta),
        }
    return {
        "blob": blob,
        "header": blob[:header_size],
        "sha256": digest(blob),
        "size": len(blob),
        "version": version,
        "page_size": page,
        "ramdisk_size": ramdisk_size,
        "cmdline": cmdline,
        "dtb": blob[dtb_at : dtb_at + dtb_size],
        "bootconfig": blob[bootconfig_at : bootconfig_at + bootconfig_size],
        "entries": entries,
        "footer": footer,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("original")
    ap.add_argument("candidate")
    ap.add_argument("--extract-candidate")
    args = ap.parse_args()
    original = parse(args.original)
    candidate = parse(args.candidate)
    original_header_masked = bytearray(original["header"])
    candidate_header_masked = bytearray(candidate["header"])
    original_header_masked[24:28] = b"\0" * 4
    candidate_header_masked[24:28] = b"\0" * 4
    checks = {
        "header_version_unchanged": candidate["version"] == original["version"] == 4,
        "page_size_unchanged": candidate["page_size"] == original["page_size"],
        "header_except_ramdisk_size_unchanged": candidate_header_masked == original_header_masked,
        "entry_count_unchanged": len(candidate["entries"]) == len(original["entries"]),
        "entry_offsets_unchanged": [item["offset"] for item in candidate["entries"]] == [item["offset"] for item in original["entries"]],
        "entry_0_metadata_unchanged": all(
            candidate["entries"][0][key] == original["entries"][0][key]
            for key in ("type", "name", "board_id")
        ),
        "entry_0_packed_unchanged": candidate["entries"][0]["packed"] == original["entries"][0]["packed"],
        "entry_1_metadata_unchanged": all(
            candidate["entries"][1][key] == original["entries"][1][key]
            for key in ("type", "name", "board_id")
        ),
        "entry_1_packed_unchanged": candidate["entries"][1]["packed"] == original["entries"][1]["packed"],
        "entry_2_identity": candidate["entries"][2]["type"] == 1 and candidate["entries"][2]["name"] == "init_boot",
        "entry_2_replaced": candidate["entries"][2]["packed"] != original["entries"][2]["packed"],
        "dtb_unchanged": candidate["dtb"] == original["dtb"],
        "bootconfig_unchanged": candidate["bootconfig"] == original["bootconfig"],
        "cmdline_unchanged": candidate["cmdline"] == original["cmdline"],
        "image_size_unchanged": candidate["size"] == original["size"],
        "vbmeta_blob_unchanged": candidate["footer"]["vbmeta_sha256"] == original["footer"]["vbmeta_sha256"],
    }
    if not all(checks.values()):
        raise SystemExit("validation failure: " + json.dumps(checks, sort_keys=True))
    if args.extract_candidate:
        out = Path(args.extract_candidate)
        out.mkdir(parents=True, exist_ok=True)
        for entry in candidate["entries"]:
            (out / f"fragment-{entry['index']:02d}.lz4").write_bytes(entry["packed"])
        (out / "dtb.bin").write_bytes(candidate["dtb"])
        (out / "bootconfig.bin").write_bytes(candidate["bootconfig"])
    clean_entries = []
    for entry in candidate["entries"]:
        clean_entries.append({key: value for key, value in entry.items() if key != "packed"})
    print(json.dumps({
        "original_sha256": original["sha256"],
        "candidate_sha256": candidate["sha256"],
        "checks": checks,
        "entries": clean_entries,
        "dtb_sha256": digest(candidate["dtb"]),
        "bootconfig_sha256": digest(candidate["bootconfig"]),
        "cmdline": candidate["cmdline"],
        "footer": candidate["footer"],
    }, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
