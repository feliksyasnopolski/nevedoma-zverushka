#!/usr/bin/env python3
"""Rebuild an Android vendor_boot v4 image by replacing one named fragment.

This tool deliberately implements the small, documented v4 container layout
instead of asking a generic boot-image repacker to flatten the vendor ramdisk.
"""

import argparse
import hashlib
import json
import struct
from pathlib import Path


VENDOR_MAGIC = b"VNDRBOOT"
AVB_FOOTER_MAGIC = b"AVBf"
HEADER_V4_SIZE = 2128
ENTRY_V4_SIZE = 108
TYPE_NAMES = {0: "none", 1: "platform", 2: "recovery", 3: "dlkm"}


def align(value, alignment):
    return (value + alignment - 1) // alignment * alignment


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def c_string(data):
    return data.split(b"\0", 1)[0].decode("ascii")


def parse_image(path):
    data = Path(path).read_bytes()
    if data[:8] != VENDOR_MAGIC:
        raise ValueError("not an Android vendor_boot image")
    header_version, page_size = struct.unpack_from("<II", data, 8)
    if header_version != 4:
        raise ValueError(f"expected header v4, found v{header_version}")
    if page_size == 0 or page_size & (page_size - 1):
        raise ValueError(f"invalid page size {page_size}")
    vendor_ramdisk_size = struct.unpack_from("<I", data, 24)[0]
    cmdline = c_string(data[28:2076])
    tags_addr = struct.unpack_from("<I", data, 2076)[0]
    name = c_string(data[2080:2096])
    header_size = struct.unpack_from("<I", data, 2096)[0]
    dtb_size = struct.unpack_from("<I", data, 2100)[0]
    dtb_addr = struct.unpack_from("<Q", data, 2104)[0]
    table_size, entry_count, entry_size, bootconfig_size = struct.unpack_from(
        "<IIII", data, 2112
    )
    if header_size != HEADER_V4_SIZE:
        raise ValueError(f"unexpected v4 header size {header_size}")
    if entry_size != ENTRY_V4_SIZE:
        raise ValueError(f"unexpected v4 table entry size {entry_size}")
    if table_size != entry_count * entry_size:
        raise ValueError("vendor ramdisk table size/count mismatch")

    ramdisk_offset = page_size
    dtb_offset = align(ramdisk_offset + vendor_ramdisk_size, page_size)
    table_offset = align(dtb_offset + dtb_size, page_size)
    bootconfig_offset = align(table_offset + table_size, page_size)
    content_size = align(bootconfig_offset + bootconfig_size, page_size)
    if content_size > len(data):
        raise ValueError("declared sections exceed image")

    ramdisk = data[ramdisk_offset : ramdisk_offset + vendor_ramdisk_size]
    dtb = data[dtb_offset : dtb_offset + dtb_size]
    table = data[table_offset : table_offset + table_size]
    bootconfig = data[bootconfig_offset : bootconfig_offset + bootconfig_size]
    entries = []
    for index in range(entry_count):
        raw = table[index * entry_size : (index + 1) * entry_size]
        size, offset, ramdisk_type = struct.unpack_from("<III", raw)
        entry_name = c_string(raw[12:44])
        board_id = list(struct.unpack_from("<16I", raw, 44))
        if offset + size > len(ramdisk):
            raise ValueError(f"fragment {index} exceeds concatenated ramdisk")
        packed = ramdisk[offset : offset + size]
        entries.append(
            {
                "index": index,
                "size": size,
                "offset": offset,
                "type": ramdisk_type,
                "type_name": TYPE_NAMES.get(ramdisk_type, f"unknown-{ramdisk_type}"),
                "name": entry_name,
                "board_id": board_id,
                "sha256": sha256(packed),
                "packed": packed,
                "raw": raw,
            }
        )
    expected_offset = 0
    for entry in entries:
        if entry["offset"] != expected_offset:
            raise ValueError("fragments are not contiguous in table order")
        expected_offset += entry["size"]
    if expected_offset != vendor_ramdisk_size:
        raise ValueError("table fragments do not cover the vendor ramdisk")

    footer = None
    if len(data) >= 64 and data[-64:-60] == AVB_FOOTER_MAGIC:
        magic, major, minor, original_size, vbmeta_offset, vbmeta_size = struct.unpack_from(
            ">4sIIQQQ", data, len(data) - 64
        )
        if vbmeta_offset + vbmeta_size > len(data) - 64:
            raise ValueError("AVB footer points outside image")
        footer = {
            "major": major,
            "minor": minor,
            "original_image_size": original_size,
            "vbmeta_offset": vbmeta_offset,
            "vbmeta_size": vbmeta_size,
            "vbmeta": data[vbmeta_offset : vbmeta_offset + vbmeta_size],
            "raw": data[-64:],
        }
        if original_size != content_size:
            raise ValueError("AVB original image size disagrees with parsed layout")

    return {
        "path": str(Path(path).resolve()),
        "data": data,
        "sha256": sha256(data),
        "image_size": len(data),
        "header": data[:header_size],
        "header_version": header_version,
        "page_size": page_size,
        "vendor_ramdisk_size": vendor_ramdisk_size,
        "cmdline": cmdline,
        "tags_addr": tags_addr,
        "name": name,
        "header_size": header_size,
        "dtb_size": dtb_size,
        "dtb_addr": dtb_addr,
        "table_size": table_size,
        "entry_count": entry_count,
        "entry_size": entry_size,
        "bootconfig_size": bootconfig_size,
        "ramdisk_offset": ramdisk_offset,
        "dtb_offset": dtb_offset,
        "table_offset": table_offset,
        "bootconfig_offset": bootconfig_offset,
        "content_size": content_size,
        "dtb": dtb,
        "dtb_sha256": sha256(dtb),
        "table": table,
        "bootconfig": bootconfig,
        "bootconfig_sha256": sha256(bootconfig),
        "entries": entries,
        "footer": footer,
    }


def public_metadata(image):
    result = {key: value for key, value in image.items() if key not in {
        "data", "header", "dtb", "table", "bootconfig", "entries", "footer"
    }}
    result["entries"] = [
        {key: value for key, value in entry.items() if key not in {"packed", "raw"}}
        for entry in image["entries"]
    ]
    if image["footer"]:
        result["avb_footer"] = {
            key: value
            for key, value in image["footer"].items()
            if key not in {"vbmeta", "raw"}
        }
        result["avb_footer"]["vbmeta_sha256"] = sha256(image["footer"]["vbmeta"])
    else:
        result["avb_footer"] = None
    return result


def extract(image, output_dir):
    out = Path(output_dir)
    out.mkdir(parents=True, exist_ok=True)
    (out / "header.bin").write_bytes(image["header"])
    (out / "dtb.bin").write_bytes(image["dtb"])
    (out / "vendor-ramdisk-table.bin").write_bytes(image["table"])
    (out / "bootconfig.bin").write_bytes(image["bootconfig"])
    for entry in image["entries"]:
        safe_name = entry["name"] or "empty"
        filename = f"fragment-{entry['index']:02d}-{entry['type_name']}-{safe_name}.lz4"
        (out / filename).write_bytes(entry["packed"])
    if image["footer"]:
        (out / "vbmeta.bin").write_bytes(image["footer"]["vbmeta"])
        (out / "avb-footer.bin").write_bytes(image["footer"]["raw"])
    (out / "metadata.json").write_text(
        json.dumps(public_metadata(image), indent=2, sort_keys=True) + "\n"
    )


def rebuild(source, replacement_path, output_path):
    matches = [
        entry for entry in source["entries"]
        if entry["name"] == "init_boot" and entry["type"] == 1
    ]
    if len(matches) != 1:
        raise ValueError(
            f"refusing ambiguous target: found {len(matches)} platform/init_boot entries"
        )
    if len([entry for entry in source["entries"] if entry["name"] == "init_boot"]) != 1:
        raise ValueError("refusing image with another entry also named init_boot")
    target = matches[0]
    replacement = Path(replacement_path).read_bytes()
    if not replacement:
        raise ValueError("replacement fragment is empty")

    fragments = []
    new_entries = []
    running_offset = 0
    for entry in source["entries"]:
        packed = replacement if entry is target else entry["packed"]
        raw = bytearray(entry["raw"])
        struct.pack_into("<II", raw, 0, len(packed), running_offset)
        fragments.append(packed)
        new_entries.append(bytes(raw))
        running_offset += len(packed)

    header = bytearray(source["header"])
    struct.pack_into("<I", header, 24, running_offset)
    table = b"".join(new_entries)
    if len(table) != source["table_size"]:
        raise AssertionError("table size changed")

    page = source["page_size"]
    content = bytearray(header)
    content.extend(b"\0" * (page - len(content)))
    content.extend(b"".join(fragments))
    content.extend(b"\0" * (align(len(content), page) - len(content)))
    content.extend(source["dtb"])
    content.extend(b"\0" * (align(len(content), page) - len(content)))
    content.extend(table)
    content.extend(b"\0" * (align(len(content), page) - len(content)))
    content.extend(source["bootconfig"])
    content.extend(b"\0" * (align(len(content), page) - len(content)))

    if source["footer"]:
        # Preserve the source vbmeta blob byte-for-byte. The source uses AVB
        # algorithm NONE and its hash descriptor already reflects the older
        # stock-derived layout rather than this Magisk-patched source payload.
        # Do not fabricate new signing/hash semantics: retain that known-booting
        # structural trailer and update only the footer's layout pointers.
        footer = source["footer"]
        vbmeta_offset = len(content)
        vbmeta = footer["vbmeta"]
        if vbmeta_offset + len(vbmeta) + 64 > source["image_size"]:
            raise ValueError("candidate plus AVB trailer exceeds source/partition size")
        output = bytearray(content)
        output.extend(vbmeta)
        output.extend(b"\0" * (source["image_size"] - 64 - len(output)))
        new_footer = bytearray(footer["raw"])
        struct.pack_into(
            ">4sIIQQQ", new_footer, 0, AVB_FOOTER_MAGIC,
            footer["major"], footer["minor"], len(content), vbmeta_offset, len(vbmeta)
        )
        output.extend(new_footer)
    else:
        output = content

    Path(output_path).write_bytes(output)
    candidate = parse_image(output_path)
    if candidate["entry_count"] != source["entry_count"]:
        raise AssertionError("entry count changed")
    if candidate["entries"][target["index"]]["packed"] != replacement:
        raise AssertionError("replacement did not survive fresh parse")


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    inspect_parser = sub.add_parser("inspect")
    inspect_parser.add_argument("image")
    inspect_parser.add_argument("--extract")
    rebuild_parser = sub.add_parser("rebuild")
    rebuild_parser.add_argument("source")
    rebuild_parser.add_argument("replacement")
    rebuild_parser.add_argument("output")
    args = parser.parse_args()
    if args.command == "inspect":
        image = parse_image(args.image)
        if args.extract:
            extract(image, args.extract)
        print(json.dumps(public_metadata(image), indent=2, sort_keys=True))
    else:
        source = parse_image(args.source)
        rebuild(source, args.replacement, args.output)


if __name__ == "__main__":
    main()
