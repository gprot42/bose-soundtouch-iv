#!/usr/bin/env python3
"""
macOS-native in-place SSH patch for BOSE Update.stu -- NO mtd-utils required.

Instead of rebuilding the whole UBIFS (which needs Linux mkfs.ubifs/ubinize),
this performs surgery on the single UBIFS data node that backs
/usr/bin/remote_services_enabled, keeping the node's total length byte-identical
so the UBIFS index (TNC), LPT, master node and journal stay valid. Only the
node payload + the node's own CRC32 change, then the outer .stu section CRC32 is
recomputed.

Why this is safe:
  * UBIFS index stores (key, LEB, offset, len) -- NOT the node data CRC. Keeping
    `len` constant means the index keeps pointing correctly.
  * The LPT tracks free/dirty space per LEB -- unchanged because nothing moved.
  * UBI only CRCs its EC/VID headers, not the volume data payload.
  * The .stu container is CRC32-only (see docs/STU-FORMAT.md); the header
    self-CRC covers just the file-size word, which is unchanged.

The replacement is a 180-byte `#!/bin/sh\nexit 0\n#...` script (line 3 is a
comment, so it passes `sh -n`) whose LZO1X-compressed form is tuned to exactly
the original 128-byte payload length.

Requires: lzallright (ships with ubi_reader). Run with the venv python, e.g.
    /tmp/ubienv/bin/python scripts/inplace-patch-stu.py work/Update.stu -o work/Update-ssh.stu

Verify afterwards:
    python3 scripts/extract-stu.py work/Update-ssh.stu --no-carve
"""
from __future__ import annotations

import argparse
import random
import struct
import sys
import zlib
from pathlib import Path

try:
    from lzallright import LZOCompressor
except ImportError:
    sys.exit("lzallright not found. Use the ubi_reader venv python, e.g. "
             "/tmp/ubienv/bin/python scripts/inplace-patch-stu.py ...")

sys.path.insert(0, str(Path(__file__).resolve().parent))
from stu_container import find_section, header_crc_ok, parse_sections  # noqa: E402

UBIFS_NODE_MAGIC = bytes([0x31, 0x18, 0x10, 0x06])  # 0x06101831 LE
UBIFS_DATA_NODE = 0x01
CH_LEN_OFF = 16          # ch.len (u32 le)
CH_TYPE_OFF = 20         # node_type (u8)
DN_SIZE_OFF = 0x28       # data_node.size (u32 le) -- after ch(24)+key(16)
DN_COMPR_OFF = 0x2C      # data_node.compr_type (u16 le)
DN_DATA_OFF = 0x30       # start of (compressed) data


def lzo(data: bytes) -> bytes:
    return LZOCompressor().compress(data)


def unlzo(data: bytes, out_len: int) -> bytes:
    try:
        return LZOCompressor().decompress(data, output_size_hint=out_len)
    except TypeError:
        return LZOCompressor().decompress(data)


def ubifs_crc(node: bytes) -> int:
    # UBIFS uses crc32_le(0xffffffff, buf+8, len-8) with no final inversion,
    # which equals (zlib.crc32(buf[8:len]) ^ 0xffffffff).
    ln = struct.unpack_from("<I", node, CH_LEN_OFF)[0]
    return (zlib.crc32(node[8:ln]) & 0xFFFFFFFF) ^ 0xFFFFFFFF


def find_gate_node(ubi: bytes, stock_script_prefix: bytes = b"#!/bin/sh\n# true if remote services") -> int:
    """Locate the data node whose decompressed payload is the gate script."""
    pos = 0
    while True:
        m = ubi.find(UBIFS_NODE_MAGIC, pos)
        if m < 0:
            return -1
        pos = m + 1
        if ubi[m + CH_TYPE_OFF] != UBIFS_DATA_NODE:
            continue
        ln = struct.unpack_from("<I", ubi, m + CH_LEN_OFF)[0]
        size = struct.unpack_from("<I", ubi, m + DN_SIZE_OFF)[0]
        if not (0 < ln < 0x2000) or not (0 < size <= 4096):
            continue
        comp = ubi[m + DN_DATA_OFF:m + ln]
        try:
            dec = unlzo(comp, size)
        except Exception:
            continue
        if dec[:len(stock_script_prefix)] == stock_script_prefix:
            return m
    # unreachable


def build_replacement(comp_len: int, unc_len: int, seed: int = 7) -> bytes:
    """A valid exit-0 script of `unc_len` bytes whose LZO form is `comp_len`."""
    head = b"#!/bin/sh\nexit 0\n#"  # line 3 = comment -> passes `sh -n`
    blen = unc_len - len(head)
    if blen < 0:
        raise ValueError("uncompressed length too small")
    alpha = bytes(c for c in range(32, 127) if c not in (0x0A, 0x5C))
    rnd = random.Random(seed)
    body = bytearray(rnd.choice(alpha) for _ in range(blen))

    def cl(b: bytes) -> int:
        return len(lzo(head + bytes(b)))

    cur = cl(body)
    best = abs(cur - comp_len)
    bb = bytearray(body)
    for _ in range(2_000_000):
        if cur == comp_len:
            break
        i = rnd.randrange(blen)
        old = body[i]
        body[i] = rnd.choice(alpha)
        c = cl(body)
        if abs(c - comp_len) <= best:
            best, cur, bb = abs(c - comp_len), c, bytearray(body)
        else:
            body[i] = old
    content = head + bytes(bb)
    if cl(bb) != comp_len:
        raise RuntimeError(f"could not hit compressed length {comp_len} "
                           f"(got {cl(bb)}); try a different seed")
    if unlzo(lzo(content), unc_len) != content:
        raise RuntimeError("LZO round-trip mismatch")
    return content


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("stu", type=Path)
    ap.add_argument("-o", "--out", type=Path, required=True)
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()

    data = bytearray(args.stu.read_bytes())
    sections = parse_sections(bytes(data))
    ubi_sec = find_section(sections, "ubi.img")
    if ubi_sec is None:
        print("No ubi.img section found", file=sys.stderr)
        return 1

    base = ubi_sec.payload_offset
    ubi = bytes(data[base:ubi_sec.payload_end])

    rel = find_gate_node(ubi)
    if rel < 0:
        print("Could not locate remote_services_enabled data node", file=sys.stderr)
        return 2
    node_abs = base + rel
    ln = struct.unpack_from("<I", data, node_abs + CH_LEN_OFF)[0]
    size = struct.unpack_from("<I", data, node_abs + DN_SIZE_OFF)[0]
    compr = struct.unpack_from("<H", data, node_abs + DN_COMPR_OFF)[0]
    data_len = ln - DN_DATA_OFF
    print(f"gate data node @ {node_abs:#x} (ubi+{rel:#x}): "
          f"len={ln} size={size} compr={compr} payload={data_len}B")

    content = build_replacement(data_len, size, seed=args.seed)
    comp = lzo(content)
    assert len(comp) == data_len and len(content) == size

    # Splice payload (same length) and recompute the UBIFS node CRC.
    data[node_abs + DN_DATA_OFF:node_abs + ln] = comp
    new_crc = ubifs_crc(bytes(data[node_abs:node_abs + ln]))
    struct.pack_into("<I", data, node_abs + 4, new_crc)
    print(f"  node CRC -> {new_crc:#010x}  (len & size unchanged)")

    # Recompute the outer .stu section CRC32 for ubi.img.
    new_sec_crc = zlib.crc32(bytes(data[ubi_sec.payload_offset:ubi_sec.payload_end])) & 0xFFFFFFFF
    struct.pack_into(">I", data, ubi_sec.crc_offset, new_sec_crc)
    print(f"  ubi.img section CRC -> {new_sec_crc:#010x}")

    args.out.write_bytes(bytes(data))

    out_sections = parse_sections(bytes(data))
    ok = header_crc_ok(bytes(data)) and all(
        s.crc_ok for s in out_sections if s.name != "SEN_FW_update.bos"
    )
    print(f"Wrote {args.out} ({len(data):,} bytes)")
    print(f"  header self-CRC: {'OK' if header_crc_ok(bytes(data)) else 'MISMATCH'}")
    print(f"  all section CRCs (excl. SEN_FW): {'OK' if ok else 'FAILED'}")
    return 0 if ok else 3


if __name__ == "__main__":
    raise SystemExit(main())
