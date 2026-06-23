#!/usr/bin/env python3
"""
Research helper: locate remote_services-related regions for future binary patches.

Does NOT produce a flashable Update.stu. Output is for manual RE in Ghidra/rizin.
"""

from __future__ import annotations

import argparse
from pathlib import Path


def find_regions(data: bytes, needle: bytes, context: int = 128) -> None:
    start = 0
    n = 0
    while True:
        i = data.find(needle, start)
        if i < 0:
            break
        n += 1
        lo = max(0, i - context)
        hi = min(len(data), i + len(needle) + context)
        print(f"\n--- match #{n} @{hex(i)} ({needle.decode()}) ---")
        chunk = data[lo:hi]
        # hex dump around match
        rel = i - lo
        for row in range(0, len(chunk), 16):
            addr = lo + row
            row_bytes = chunk[row : row + 16]
            hexpart = " ".join(f"{b:02x}" for b in row_bytes)
            asc = "".join(chr(b) if 32 <= b < 127 else "." for b in row_bytes)
            mark = " <--" if row <= rel < row + 16 else ""
            print(f"  {addr:08x}  {hexpart:<48}  {asc}{mark}")
        start = i + 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("stu", type=Path)
    ap.add_argument(
        "--needle",
        default="remote_services_enabled",
        help="String to search (default: remote_services_enabled)",
    )
    args = ap.parse_args()
    data = args.stu.read_bytes()
    needle = args.needle.encode()
    print(f"Scanning {args.stu} ({len(data):,} bytes) for {needle!r}")
    find_regions(data, needle)
    print("\nNext steps:")
    print("  1. Identify which embedded ELF or filesystem image owns this string.")
    print("  2. In Ghidra, locate remote_services_enabled() and the USB mount path.")
    print("  3. Patch to always call /etc/init.d/sshd start OR default-touch /mnt/nv/remote_services.")
    print("  4. Recompute container checksums (format TBD) and repack Update.stu.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())