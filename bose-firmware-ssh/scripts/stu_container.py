#!/usr/bin/env python3
"""
Shared parser for the BOSE SoftwareUpdateInstaller (.stu) container.

Container layout (fully reverse-engineered from 27.00.06 WST4):

  Offset 0x00  u32 be  0x14            magic-block length
  Offset 0x04  4       "BOSE"          magic
  Offset 0x08  u32 be  total file size (e.g. 0x05fa69ac)
  Offset 0x0C  u32 be  0x124           section-descriptor length
  Offset 0x10  u32 be  CRC32 of bytes [0x00:0x10]   (header self-CRC)

  Then a chain of sections, each:
    [ 0x124-byte descriptor ][ payload ]
  Descriptor fields (relative to descriptor start):
    +0x04  char[]  section name (NUL terminated), e.g. "ubi.img"
    +0x84  u32 be  payload size
    +0x88  u32 be  payload size (duplicate)
    +0x118 u32 be  CRC32 of payload

The very first descriptor starts at 0x14 and its name field overlaps the
"SoftwareUpdateInstaller" string; size@0x98 / crc@0x12c follow the same rule.

Integrity model: CRC32 only (header self-CRC + per-section payload CRC32).
No RSA/signature found in the container structure itself. This means a
same-length payload swap (e.g. patched UBI image) only requires recomputing
that section's CRC32 -- the header CRC is unchanged because the file size is.
"""
from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass
from pathlib import Path

MAGIC_OFFSET = 0x04
MAGIC = b"BOSE"
SIZE_OFFSET = 0x08
DESC_LEN_OFFSET = 0x0C
HEADER_CRC_OFFSET = 0x10
FIRST_DESC_OFFSET = 0x14
DESC_LEN = 0x124

NAME_REL = 0x04
SIZE_REL = 0x84
SIZE2_REL = 0x88
CRC_REL = 0x118


@dataclass
class Section:
    index: int
    desc_offset: int
    name: str
    payload_offset: int
    size: int
    size_dup: int
    crc_stored: int
    crc_calc: int

    @property
    def payload_end(self) -> int:
        return self.payload_offset + self.size

    @property
    def crc_offset(self) -> int:
        return self.desc_offset + CRC_REL

    @property
    def crc_ok(self) -> bool:
        return self.crc_stored == self.crc_calc


def be32(data: bytes, off: int) -> int:
    return struct.unpack_from(">I", data, off)[0]


def check_magic(data: bytes) -> None:
    if data[MAGIC_OFFSET:MAGIC_OFFSET + 4] != MAGIC:
        raise ValueError(
            f"bad magic {data[MAGIC_OFFSET:MAGIC_OFFSET + 4]!r} (expected BOSE)"
        )


def header_crc_ok(data: bytes) -> bool:
    return (zlib.crc32(data[0:0x10]) & 0xFFFFFFFF) == be32(data, HEADER_CRC_OFFSET)


def parse_sections(data: bytes) -> list[Section]:
    check_magic(data)
    n = len(data)
    sections: list[Section] = []
    off = FIRST_DESC_OFFSET
    idx = 0
    while off + DESC_LEN <= n:
        name = data[off + NAME_REL:off + NAME_REL + 0x60].split(b"\x00")[0]
        name_s = name.decode("latin1", "replace")
        size = be32(data, off + SIZE_REL)
        size2 = be32(data, off + SIZE2_REL)
        crc_stored = be32(data, off + CRC_REL)
        payload = off + DESC_LEN
        end = payload + size
        if size == 0 or end > n:
            break
        crc_calc = zlib.crc32(data[payload:end]) & 0xFFFFFFFF
        sections.append(
            Section(idx, off, name_s, payload, size, size2, crc_stored, crc_calc)
        )
        off = end
        idx += 1
    return sections


def find_section(sections: list[Section], name: str) -> Section | None:
    for s in sections:
        if s.name == name:
            return s
    return None


def load(path: str | Path) -> tuple[bytes, list[Section]]:
    data = Path(path).read_bytes()
    return data, parse_sections(data)
