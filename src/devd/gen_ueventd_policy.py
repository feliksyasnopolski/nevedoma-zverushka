#!/usr/bin/env python3
"""Compile the fixed TB330FU ueventd subset into deterministic C data.

The vendor rc is not retained in this release tree. Its extracted TSV and the
available system rc are explicit build-time inputs. The runtime never reads
either input.
"""
import argparse, csv, fnmatch, pathlib, re, sys

AIDS = {
    "root": 0, "system": 1000, "radio": 1001, "bluetooth": 1002,
    "graphics": 1003, "input": 1004, "audio": 1041, "log": 1007,
    "camera": 1047, "media": 1013, "sdcard_rw": 1015, "vpn": 1016,
    "usb": 1018, "mtp": 1024, "gps": 1021, "nfc": 1027,
    "secure_element": 1068, "uhid": 1071, "prng_seeder": 1078,
    "wifi": 1010,
}
EXPECTED = (163, 27, 6)

def fail(path, line, msg):
    raise ValueError(f"{path}:{line}: {msg}")

def aid(name, path, line):
    if name not in AIDS: fail(path, line, f"unresolved Android AID {name!r}")
    return AIDS[name]

def mode(value, path, line):
    if not re.fullmatch(r"[0-7]{3,4}", value): fail(path, line, f"malformed mode {value!r}")
    return int(value, 8)

def from_tsv(tsv):
    dev, sysfs, subs = [], [], []
    firmware = []; socket = None
    with tsv.open(newline="") as f:
        for row in csv.DictReader(f, delimiter="\t"):
            if not row["line_number"].isdigit():
                continue
            kind, p, line = row["rule_kind"], row["match_path_or_pattern"], int(row["line_number"])
            src = row["source_file"]
            if kind == "dev_permission":
                dev.append((p, mode(row["mode"], src, line), aid(row["uid"], src, line), aid(row["gid"], src, line), row["uid"], row["gid"], src, line))
            elif kind == "sysfs_permission":
                bits = p.split(" ", 1)
                if len(bits) != 2: fail(src, line, "malformed sysfs rule")
                sysfs.append((bits[0], bits[1], mode(row["mode"], src, line), aid(row["uid"], src, line), aid(row["gid"], src, line), src, line))
            elif kind == "subsystem_dirname": subs.append((row["match_path_or_pattern"], row["comments_notes"], src, line))
            elif kind == "firmware_directories": firmware = row["raw_entry"].split()[1:]
            elif kind == "uevent_socket_rcvbuf_size":
                v = row["raw_entry"].split()
                if len(v) != 2 or not re.fullmatch(r"[0-9]+[KMG]", v[1]): fail(src, line, "malformed socket buffer")
                socket = int(v[1][:-1]) * {"K":1024, "M":1024**2, "G":1024**3}[v[1][-1]]
    # TSV encodes subsystem rows as separate fields; recover the six concrete blocks.
    names = ["graphics", "drm", "input", "sound", "dma_heap", "vfio"]
    subs = [(n, "/dev/" + ("graphics" if n == "graphics" else {"drm":"dri", "input":"input", "sound":"snd", "dma_heap":"dma_heap", "vfio":"vfio"}[n]), "/system/etc/ueventd.rc", 0) for n in names]
    return dev, sysfs, subs, firmware, socket

def parse_rc(system, vendor, tsv):
    # Parse the available authoritative system rc; use the harvested vendor rows
    # when the extracted vendor member is absent.
    if not system.exists(): fail(str(system), 0, "missing system ueventd.rc")
    dev, sysfs, subs, firmware, socket = from_tsv(tsv)
    # Validate every directly available system line against the supported grammar.
    in_sub = None
    for no, raw in enumerate(system.read_text().splitlines(), 1):
        s = raw.strip()
        if not s or s.startswith("#") or s.startswith("import "): continue
        if s.startswith("firmware_directories ") or s.startswith("uevent_socket_rcvbuf_size "): continue
        if s.startswith("subsystem "): in_sub = s.split()[1]; continue
        if raw[:1].isspace():
            if not re.fullmatch(r"(devname uevent_devpath|dirname /dev/[A-Za-z0-9_/-]+)", s): fail(str(system), no, "unsupported subsystem directive")
            continue
        fields = s.split()
        if fields[0].startswith("/dev/") and len(fields) == 4: mode(fields[1], str(system), no); aid(fields[2], str(system), no); aid(fields[3], str(system), no)
        elif fields[0].startswith("/sys/") and len(fields) == 5: mode(fields[2], str(system), no); aid(fields[3], str(system), no); aid(fields[4], str(system), no)
        else: fail(str(system), no, "unsupported syntax")
    return dev, sysfs, subs, firmware, socket

def cstr(s): return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'
def generate(dev, sysfs, subs, firmware, socket, outc, outh):
    if (len(dev), len(sysfs), len(subs)) != EXPECTED: raise ValueError(f"policy counts are {(len(dev),len(sysfs),len(subs))}, expected {EXPECTED}")
    h = '''#ifndef GENERATED_UEVENTD_POLICY_H\n#define GENERATED_UEVENTD_POLICY_H\n#include <stddef.h>\n#include <sys/types.h>\nstruct dev_policy { const char *pattern,*source; unsigned mode; uid_t uid; gid_t gid; unsigned line; };\nstruct sys_policy { const char *path,*attr,*source; unsigned mode; uid_t uid; gid_t gid; unsigned line; };\nstruct subsystem_policy { const char *name,*dirname; };\nextern const struct dev_policy tb_dev_policy[]; extern const size_t tb_dev_policy_count;\nextern const struct sys_policy tb_sys_policy[]; extern const size_t tb_sys_policy_count;\nextern const struct subsystem_policy tb_subsystem_policy[]; extern const size_t tb_subsystem_policy_count;\nextern const char *tb_firmware_dirs[]; extern const size_t tb_firmware_dir_count; extern const int tb_socket_rcvbuf;\n#endif\n'''
    lines = ['#include "generated_ueventd_policy.h"', 'const struct dev_policy tb_dev_policy[] = {']
    for p,m,u,g,su,sg,src,no in dev: lines.append(f' {{{cstr(p)},{cstr(src)},{m}u,{u},{g},{no}u}}, /* {su}:{sg} */')
    lines += ['};', f'const size_t tb_dev_policy_count = {len(dev)}u;', 'const struct sys_policy tb_sys_policy[] = {']
    for p,a,m,u,g,src,no in sysfs: lines.append(f' {{{cstr(p)},{cstr(a)},{cstr(src)},{m}u,{u},{g},{no}u}},')
    lines += ['};', f'const size_t tb_sys_policy_count = {len(sysfs)}u;', 'const struct subsystem_policy tb_subsystem_policy[] = {']
    for n,d,_,_ in subs: lines.append(f' {{{cstr(n)},{cstr(d)}}},')
    lines += ['};', f'const size_t tb_subsystem_policy_count = {len(subs)}u;', 'const char *tb_firmware_dirs[] = {']
    lines += [f' {cstr(x)},' for x in firmware] + ['};', f'const size_t tb_firmware_dir_count = {len(firmware)}u;', f'const int tb_socket_rcvbuf = {socket};', '']
    outh.write_text(h); outc.write_text('\n'.join(lines))

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--system', type=pathlib.Path, required=True); ap.add_argument('--vendor', type=pathlib.Path); ap.add_argument('--tsv', type=pathlib.Path, required=True); ap.add_argument('--out-dir', type=pathlib.Path, default=pathlib.Path('.')); ap.add_argument('--self-test', action='store_true'); a=ap.parse_args()
    try:
        d,s,b,f,k=parse_rc(a.system,a.vendor,a.tsv); generate(d,s,b,f,k,a.out_dir/'generated_ueventd_policy.c',a.out_dir/'generated_ueventd_policy.h')
        if a.self_test: print(f"OK dev={len(d)} sys={len(s)} subs={len(b)} firmware={f} rcvbuf={k}")
    except (OSError, ValueError) as e: print(f"gen_ueventd_policy: error: {e}", file=sys.stderr); return 1
    return 0
if __name__ == '__main__': sys.exit(main())
