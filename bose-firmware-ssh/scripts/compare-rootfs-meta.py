#!/usr/bin/env python3
"""
Compare two unpacked rootfs trees and report metadata drift.

Designed for diagnosing extract -> mkfs.ubifs -> ubinize round-trip fidelity.
Compares path inventory, file types, mode/uid/gid, symlink targets, and
content hashes for regular files.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import stat
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Entry:
    kind: str  # reg, lnk, dir, chr, blk, fifo, sock
    mode: int
    uid: int
    gid: int
    size: int
    target: str  # symlink target or ""
    sha256: str  # regular files only


def classify(mode: int) -> str:
    if stat.S_ISREG(mode):
        return "reg"
    if stat.S_ISLNK(mode):
        return "lnk"
    if stat.S_ISDIR(mode):
        return "dir"
    if stat.S_ISCHR(mode):
        return "chr"
    if stat.S_ISBLK(mode):
        return "blk"
    if stat.S_ISFIFO(mode):
        return "fifo"
    if stat.S_ISSOCK(mode):
        return "sock"
    return "other"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def scan(root: Path) -> dict[str, Entry]:
    out: dict[str, Entry] = {}
    for dirpath, dirnames, filenames in os.walk(root, topdown=True, followlinks=False):
        dp = Path(dirpath)
        rel_dir = dp.relative_to(root).as_posix()
        if rel_dir != ".":
            st = dp.lstat()
            out[rel_dir] = Entry(
                kind=classify(st.st_mode),
                mode=st.st_mode & 0o7777,
                uid=st.st_uid,
                gid=st.st_gid,
                size=st.st_size,
                target="",
                sha256="",
            )
        for name in sorted(dirnames + filenames):
            p = dp / name
            rel = p.relative_to(root).as_posix()
            st = p.lstat()
            kind = classify(st.st_mode)
            target = ""
            digest = ""
            if kind == "lnk":
                target = os.readlink(p)
            elif kind == "reg":
                digest = sha256_file(p)
            out[rel] = Entry(
                kind=kind,
                mode=st.st_mode & 0o7777,
                uid=st.st_uid,
                gid=st.st_gid,
                size=st.st_size,
                target=target,
                sha256=digest,
            )
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("stock", type=Path, help="Reference (stock) rootfs dir")
    ap.add_argument("rebuilt", type=Path, help="Round-trip / patched rootfs dir")
    ap.add_argument("--wifi-only", action="store_true",
                    help="Only report paths touching wifi/wlan/hostapd/wpa/wl18")
    ap.add_argument("--top", type=int, default=0,
                    help="Limit printed rows per category (0 = all)")
    args = ap.parse_args()

    stock = scan(args.stock)
    rebuilt = scan(args.rebuilt)

    def keep(path: str) -> bool:
        if not args.wifi_only:
            return True
        p = path.lower()
        keys = ("wifi", "wlan", "hostapd", "wpa", "wl18", "wireless", "udhcp")
        return any(k in p for k in keys)

    only_stock = sorted(p for p in stock if p not in rebuilt and keep(p))
    only_rebuilt = sorted(p for p in rebuilt if p not in stock and keep(p))

    meta_diffs: list[tuple[str, str, Entry, Entry]] = []
    content_diffs: list[str] = []
    for p in sorted(set(stock) & set(rebuilt)):
        if not keep(p):
            continue
        a, b = stock[p], rebuilt[p]
        if a != b:
            reason = []
            if a.kind != b.kind:
                reason.append(f"kind {a.kind}->{b.kind}")
            if a.mode != b.mode:
                reason.append(f"mode {oct(a.mode)}->{oct(b.mode)}")
            if a.uid != b.uid:
                reason.append(f"uid {a.uid}->{b.uid}")
            if a.gid != b.gid:
                reason.append(f"gid {a.gid}->{b.gid}")
            if a.size != b.size:
                reason.append(f"size {a.size}->{b.size}")
            if a.target != b.target:
                reason.append(f"target {a.target!r}->{b.target!r}")
            if a.sha256 and b.sha256 and a.sha256 != b.sha256:
                reason.append("content")
                content_diffs.append(p)
            meta_diffs.append((p, ", ".join(reason), a, b))

    def cap(lines: list[str]) -> list[str]:
        if args.top and len(lines) > args.top:
            return lines[:args.top] + [f"... ({len(lines) - args.top} more)"]
        return lines

    print(f"stock:   {args.stock}  ({len(stock)} entries)")
    print(f"rebuilt: {args.rebuilt}  ({len(rebuilt)} entries)")
    print()
    print(f"only in stock:   {len(only_stock)}")
    for p in cap(only_stock):
        e = stock[p]
        print(f"  - {p}  [{e.kind} mode={oct(e.mode)} uid={e.uid} gid={e.gid}]")
    print()
    print(f"only in rebuilt: {len(only_rebuilt)}")
    for p in cap(only_rebuilt):
        e = rebuilt[p]
        print(f"  + {p}  [{e.kind} mode={oct(e.mode)} uid={e.uid} gid={e.gid}]")
    print()
    print(f"metadata/content diffs: {len(meta_diffs)}")
    print(f"  content-changed regular files: {len(content_diffs)}")
    for p, why, a, b in cap(meta_diffs):
        print(f"  * {p}: {why}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())