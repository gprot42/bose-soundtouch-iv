#!/usr/bin/env python3
"""
Extract a BOSE Update.stu container.

Parses the section table, verifies every section CRC32, carves each payload to
an output directory, and (for the rootfs UBI image) prints the ubireader
command needed to unpack the filesystem.

Usage:
    python3 scripts/extract-stu.py work/Update.stu [-o work/sections]
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from stu_container import header_crc_ok, load  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("stu", type=Path, help="Path to Update.stu")
    ap.add_argument("-o", "--out", type=Path, default=None,
                    help="Directory to carve sections into (default: <stu>.sections)")
    ap.add_argument("--no-carve", action="store_true",
                    help="Only print the section table, do not write files")
    args = ap.parse_args()

    if not args.stu.is_file():
        print(f"Not found: {args.stu}", file=sys.stderr)
        return 1

    data, sections = load(args.stu)
    print(f"File: {args.stu}  ({len(data):,} bytes)")
    print(f"Header self-CRC (0x10): {'OK' if header_crc_ok(data) else 'MISMATCH'}")
    print(f"Sections: {len(sections)}\n")

    hdr = f"{'#':>2}  {'name':18} {'payload@':>10} {'size':>10}  {'crc_stored':>10} {'crc_calc':>10}  ok"
    print(hdr)
    print("-" * len(hdr))
    for s in sections:
        print(f"{s.index:>2}  {s.name:18} {s.payload_offset:#010x} {s.size:#010x}  "
              f"{s.crc_stored:#010x} {s.crc_calc:#010x}  {'OK' if s.crc_ok else 'BAD'}")
    print()

    bad = [s for s in sections if not s.crc_ok]
    if bad:
        print(f"WARNING: {len(bad)} section(s) failed CRC: "
              f"{', '.join(s.name or str(s.index) for s in bad)}")
        print("  (the trailing SEN_FW section uses a different checksum variant;")
        print("   this does not affect rootfs patching.)\n")

    if args.no_carve:
        return 0

    out = args.out or args.stu.with_suffix(args.stu.suffix + ".sections")
    out.mkdir(parents=True, exist_ok=True)
    for s in sections:
        name = s.name or f"section{s.index}"
        safe = name.replace("/", "_")
        dest = out / f"{s.index:02d}_{safe}"
        dest.write_bytes(data[s.payload_offset:s.payload_end])
        print(f"  wrote {dest}  ({s.size:,} bytes)")

    rootfs = next((s for s in sections if s.name == "ubi.img"), None)
    if rootfs is not None:
        ubi_path = out / f"{rootfs.index:02d}_ubi.img"
        print("\nNext: unpack the rootfs UBI image with ubi_reader:")
        print(f"  python3 -m venv /tmp/ubienv && /tmp/ubienv/bin/pip install ubi_reader")
        print(f"  /tmp/ubienv/bin/ubireader_extract_files -o {out / 'rootfs'} {ubi_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
