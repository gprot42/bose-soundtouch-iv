#!/usr/bin/env python3
"""
Repack a BOSE Update.stu after patching the rootfs UBI image.

Given the stock Update.stu and a rebuilt ubi.img (see rebuild-ubi.sh), this
swaps the "ubi.img" section payload in place and recomputes that section's
CRC32. The outer header self-CRC (offset 0x10) covers only the file-size
header bytes, so it stays valid as long as the total file size is unchanged.

The replacement UBI image MUST be exactly the same size as the stock section
(0x5500000 bytes for 27.00.06). rebuild-ubi.sh pads it for you.

Usage:
    python3 scripts/repack-stu.py work/Update.stu new_ubi.img -o work/Update-ssh.stu

Verify afterwards:
    python3 scripts/extract-stu.py work/Update-ssh.stu --no-carve
"""
from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from stu_container import header_crc_ok, parse_sections  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("stu", type=Path, help="Stock Update.stu")
    ap.add_argument("ubi", type=Path, help="Rebuilt ubi.img (patched rootfs)")
    ap.add_argument("-o", "--out", type=Path, required=True,
                    help="Output patched Update.stu path")
    ap.add_argument("--section", default="ubi.img",
                    help="Section name to replace (default: ubi.img)")
    args = ap.parse_args()

    data = bytearray(args.stu.read_bytes())
    new_payload = args.ubi.read_bytes()
    sections = parse_sections(bytes(data))
    target = next((s for s in sections if s.name == args.section), None)
    if target is None:
        print(f"Section {args.section!r} not found", file=sys.stderr)
        return 1

    if len(new_payload) != target.size:
        print(f"ERROR: replacement is {len(new_payload):#x} bytes but section "
              f"{args.section!r} is {target.size:#x} bytes.", file=sys.stderr)
        print("Pad/truncate the UBI image to match (rebuild-ubi.sh does this).",
              file=sys.stderr)
        return 1

    # Splice payload.
    data[target.payload_offset:target.payload_end] = new_payload
    # Recompute the section CRC32 and write it big-endian at the descriptor.
    new_crc = zlib.crc32(new_payload) & 0xFFFFFFFF
    struct.pack_into(">I", data, target.crc_offset, new_crc)

    args.out.write_bytes(bytes(data))

    # Re-verify.
    out_sections = parse_sections(bytes(data))
    ok = header_crc_ok(bytes(data)) and all(
        s.crc_ok for s in out_sections if s.name != "SEN_FW_update.bos"
    )
    print(f"Wrote {args.out}  ({len(data):,} bytes)")
    print(f"  {args.section} CRC32: {target.crc_stored:#010x} -> {new_crc:#010x}")
    print(f"  header self-CRC: {'OK' if header_crc_ok(bytes(data)) else 'MISMATCH'}")
    print(f"  all section CRCs (excl. SEN_FW): {'OK' if ok else 'CHECK FAILED'}")
    return 0 if ok else 2


if __name__ == "__main__":
    raise SystemExit(main())
