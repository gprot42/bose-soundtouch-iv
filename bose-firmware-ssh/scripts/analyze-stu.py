#!/usr/bin/env python3
"""Analyze Bose Wave SoundTouch IV Update.stu firmware packages."""

from __future__ import annotations

import argparse
import hashlib
import re
import struct
import sys
from pathlib import Path

SSH_MARKERS = (
    b"remote_services",
    b"remote_services_enabled",
    b"sshd",
    b"telnetd",
    b"/mnt/nv/",
    b"shelby_local",
    b"ubimount.sh",
    b"/etc/init.d/sshd",
)

FORMAT_MARKERS = (
    (b"\x7fELF", "ELF"),
    (b"\x1f\x8b", "gzip"),
    (b"hsqs", "squashfs4"),
    (b"sqsh", "squashfs3_be"),
    (b"SQLi", "squashfs_legacy"),
    (b"UBI#", "ubi"),
    (b"CrAU", "chromeos_update"),
)


def md5_hex(path: Path) -> str:
    h = hashlib.md5()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def parse_bose_header(data: bytes) -> dict:
    if len(data) < 0x90:
        return {"error": "file too small"}
    if data[4:8] != b"BOSE":
        return {"error": f"bad magic {data[4:8]!r} (expected BOSE at offset 4)"}

    header_len = struct.unpack(">I", data[0:4])[0]
    tag = data[8:12].hex()
    field_a = struct.unpack(">I", data[12:16])[0]
    field_b = struct.unpack(">I", data[16:20])[0]
    name = data[20:84].split(b"\x00")[0].decode("latin1", errors="replace")

    payload_size = struct.unpack(">I", data[0x90:0x94])[0] if len(data) >= 0x94 else None

    return {
        "header_len_field": header_len,
        "tag": tag,
        "field_a": hex(field_a),
        "field_b": hex(field_b),
        "installer_name": name or "(empty — name may follow in container)",
        "payload_size_field_at_0x90": payload_size,
        "first_elf_offset": data.find(b"\x7fELF"),
    }


def find_all(data: bytes, needle: bytes, limit: int = 32) -> list[int]:
    out: list[int] = []
    start = 0
    while len(out) < limit:
        i = data.find(needle, start)
        if i < 0:
            break
        out.append(i)
        start = i + 1
    return out


def context(data: bytes, offset: int, radius: int = 64) -> str:
    lo = max(0, offset - radius)
    hi = min(len(data), offset + radius)
    chunk = data[lo:hi]
    return "".join(chr(c) if 32 <= c < 127 else "." for c in chunk)


def analyze(path: Path) -> int:
    data = path.read_bytes()
    print(f"File: {path}")
    print(f"Size: {len(data):,} bytes ({len(data) / (1024 * 1024):.1f} MiB)")
    print(f"MD5:  {md5_hex(path)}")
    print()

    hdr = parse_bose_header(data)
    print("=== BOSE container header (offset 0) ===")
    for k, v in hdr.items():
        print(f"  {k}: {v}")
    print()

    print("=== Embedded format signatures (first occurrence) ===")
    for sig, label in FORMAT_MARKERS:
        off = data.find(sig)
        print(f"  {label:16} {hex(off) if off >= 0 else 'not found'}")
    print(f"  ELF total count   {data.count(b'\x7fELF')}")
    print()

    print("=== SSH / persistence markers ===")
    for marker in SSH_MARKERS:
        offs = find_all(data, marker, limit=8)
        print(f"  {marker.decode('latin1'):28} {len(offs)} hit(s)")
        for off in offs[:3]:
            print(f"    @{hex(off)}  {context(data, off, 48)}")
    print()

    print("=== BOSE in-band markers (sample) ===")
    for m in re.finditer(b"BOSE_[A-Z_]{3,}", data):
        off = m.start()
        snippet = data[off : off + 48].split(b"\x00")[0].decode("latin1", errors="replace")
        print(f"  @{hex(off)}  {snippet}")
        if off > 0x600000:
            break
    print()

    print("=== Research notes ===")
    print("  • Update.stu is a BOSE SoftwareUpdateInstaller container, not a raw rootfs.")
    print("  • Strings like remote_services_enabled are present — SSH logic is in firmware.")
    print("  • Squashfs/UBI images are embedded but not exposed at clean file offsets;")
    print("    extraction requires reverse-engineering the installer container format.")
    print("  • Known-good MD5 for 27.00.06 WST4: 88c63e440cafa969ff19fb98b39be24a")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("stu", type=Path, help="Path to Update.stu")
    args = ap.parse_args()
    if not args.stu.is_file():
        print(f"Not found: {args.stu}", file=sys.stderr)
        return 1
    return analyze(args.stu)


if __name__ == "__main__":
    raise SystemExit(main())