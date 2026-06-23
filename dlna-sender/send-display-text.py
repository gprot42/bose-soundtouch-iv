#!/usr/bin/env python3
"""
send-display-text.py — put custom text on the Wave SoundTouch IV front display.

The Wave SoundTouch IV (firmware codename "triode") drives its front VFD via
ABLServer's "remote display" commands, exposed on the CLIServer telnet CLI
(:17000):

    abl rdset <field> "<text>"      # set a remote-display field (free text)
    abl rdsend ttag                 # push now-playing lines (title/artist/…) to the VFD
    abl rdsend state                # push the static state line to the VFD
    sys volume <N> updateDisplay    # nudge the console to repaint (required)

Free-text fields confirmed in firmware 27.00.06:
    title, artist, album, source, station   (now-playing text lines)
    state                                    (dedicated static message line)

Clearing a field: set it to a single space (" "). An empty string ("") is
parsed as a missing argument and ignored, so this script blanks with " ".

Usage
-----
  python3 send-display-text.py "HELLO WORLD"
  python3 send-display-text.py --title "DINNER" --artist "READY"
  python3 send-display-text.py --state "BE RIGHT BACK"
  python3 send-display-text.py --title "MEETING" --hold 60      # keep up ~60s
  python3 send-display-text.py --title "STAY" --hold 0          # until Ctrl+C
  python3 send-display-text.py --clear
  BOSE_IP=192.168.0.119 python3 send-display-text.py "HI"

Notes
-----
- With a single positional argument, the text goes into the `title` field.
- Now-playing fields (title, artist, …) use `rdsend ttag` during playback and
  `rdsend state` to paint the VFD (required when idle — ttag alone returns OK
  but shows nothing). After rdsend, `sys volume N updateDisplay` repaints the
  panel (this script runs all three in one telnet session).
- The remote-display buffer is global server state, but the firmware repaints
  the panel on the next now-playing/volume/source refresh, which overwrites it.
  Use --hold to re-push periodically and keep the text visible.
- The firmware also has a `timeout` (auto-expiring) message field, but it did
  not populate via the CLI in testing, so it is not exposed here.
- This drives an internal diagnostic/manufacturing command; it is unsupported
  by Bose and firmware-specific. Verified on Wave SoundTouch IV / 27.00.06.

Requirements: Python 3.8+ (stdlib only)
"""

from __future__ import annotations

import argparse
import os
import socket
import sys
import time
import urllib.request
import xml.etree.ElementTree as ET

DEFAULT_IP = os.environ.get("BOSE_IP", "192.168.0.119")
TELNET_PORT = 17000

# Now-playing free-text lines.
TEXT_FIELDS = ("title", "artist", "album", "source", "station")
# Dedicated static message line (separate field in the buffer).
STATE_FIELD = "state"
ALL_FIELDS = TEXT_FIELDS + (STATE_FIELD,)
# Setting a field to a single space blanks it ("" is ignored by the parser).
BLANK = " "


def cli_session(ip: str, commands: list[str], wait: float = 0.8, timeout: float = 4.0) -> list[str]:
    """Open one :17000 telnet session and run commands in order; return responses."""
    responses: list[str] = []
    with socket.create_connection((ip, TELNET_PORT), timeout=timeout) as sock:
        sock.settimeout(0.5)
        try:  # drain the "->" prompt banner
            sock.recv(4096)
        except socket.timeout:
            pass
        for cmd in commands:
            sock.sendall((cmd + "\n").encode("utf-8", errors="replace"))
            time.sleep(wait)
            buf = b""
            try:
                while True:
                    chunk = sock.recv(4096)
                    if not chunk:
                        break
                    buf += chunk
            except socket.timeout:
                pass
            responses.append(buf.decode("latin1"))
    return responses


def build_commands(fields: dict[str, str], clear: bool) -> list[str]:
    cmds: list[str] = []
    if clear:
        for f in ALL_FIELDS:
            cmds.append(f'abl rdset {f} "{BLANK}"')
    for field, text in fields.items():
        safe = text.replace('"', "'") or BLANK
        cmds.append(f'abl rdset {field} "{safe}"')
    has_np = any(f in fields for f in TEXT_FIELDS)
    has_state = STATE_FIELD in fields
    if clear and not fields:
        cmds.extend(("abl rdsend ttag", "abl rdsend state"))
    else:
        if has_np:
            cmds.append("abl rdsend ttag")
        # rdsend state actually paints the VFD; ttag alone only updates the
        # now-playing overlay while audio is active.
        if has_np or has_state:
            cmds.append("abl rdsend state")
    return cmds


def current_volume(ip: str) -> int:
    """Read target volume from REST :8090 (for updateDisplay refresh)."""
    try:
        with urllib.request.urlopen(f"http://{ip}:8090/volume", timeout=3) as resp:
            root = ET.fromstring(resp.read())
        for el in root.iter():
            el.tag = el.tag.split("}")[-1]
        vol = root.find("targetvolume")
        if vol is None:
            vol = root.find("actualvolume")
        if vol is not None and vol.text:
            return int(vol.text)
    except Exception:
        pass
    return 30


def refresh_display(ip: str) -> str:
    """Nudge the Wave console to repaint after rdsend (sys volume N updateDisplay)."""
    vol = current_volume(ip)
    responses = cli_session(ip, [f"sys volume {vol} updateDisplay"], wait=0.5)
    return responses[-1] if responses else ""


def push(
    ip: str,
    commands: list[str],
    *,
    refresh: bool = True,
    wait: float = 0.8,
) -> str:
    all_cmds = list(commands)
    if refresh:
        vol = current_volume(ip)
        all_cmds.append(f"sys volume {vol} updateDisplay")
    responses = cli_session(ip, all_cmds, wait=wait)
    last = responses[-1] if responses else ""
    if refresh and "OK" not in last:
        print(f"Warning: updateDisplay response unclear: {last!r}", file=sys.stderr)
    return last


def prepare_display_fields(fields: dict[str, str], *, clear: bool = False) -> dict[str, str]:
    """
    Copy fields for VFD push. Mirrors title → state so rdsend state paints the
    panel (ttag alone returns OK but may not show when idle or after refresh).
    """
    display_fields = dict(fields)
    if not clear and STATE_FIELD not in display_fields and "title" in display_fields:
        display_fields[STATE_FIELD] = display_fields["title"]
    return display_fields


def push_fields(
    ip: str,
    fields: dict[str, str],
    *,
    clear: bool = False,
    refresh: bool = True,
    wait: float = 0.8,
) -> str:
    """Set remote-display fields and push them to the VFD."""
    commands = build_commands(prepare_display_fields(fields, clear=clear), clear)
    return push(ip, commands, refresh=refresh, wait=wait)


def main() -> None:
    p = argparse.ArgumentParser(
        description="Show custom text on the Wave SoundTouch IV front display via the :17000 CLI.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    p.add_argument("text", nargs="?", help="Text for the title field (shorthand for --title)")
    for f in TEXT_FIELDS:
        p.add_argument(f"--{f}", metavar="STR", help=f"Set the '{f}' display field")
    p.add_argument("--state", metavar="STR", help="Set the dedicated static message line")
    p.add_argument("--clear", action="store_true", help="Blank all fields before setting")
    p.add_argument("--hold", type=int, metavar="SECS",
                   help="Keep re-pushing to survive now-playing refreshes; "
                        "0 = until Ctrl+C")
    p.add_argument("--interval", type=float, default=3.0, metavar="SECS",
                   help="Re-push interval when using --hold (default: 3.0)")
    p.add_argument("--ip", default=DEFAULT_IP, metavar="IP",
                   help=f"Speaker IP (default: {DEFAULT_IP} or $BOSE_IP)")
    args = p.parse_args()

    fields: dict[str, str] = {}
    if args.text is not None:
        fields["title"] = args.text
    for f in TEXT_FIELDS:
        v = getattr(args, f)
        if v is not None:
            fields[f] = v
    if args.state is not None:
        fields[STATE_FIELD] = args.state

    if not fields and not args.clear:
        p.print_help()
        sys.exit(0)

    shown = ", ".join(f"{k}={v!r}" for k, v in fields.items()) or "(cleared)"

    try:
        last = push_fields(args.ip, fields, clear=args.clear)
        if "OK" not in last:
            print(f"Warning: rdsend response unclear: {last!r}", file=sys.stderr)
        print(f"Pushed to {args.ip} display: {shown}")

        if args.hold is not None:
            deadline = None if args.hold == 0 else time.monotonic() + args.hold
            interval = max(0.5, args.interval)
            print(f"Holding (interval {interval}s){'' if deadline is None else f', {args.hold}s'} — Ctrl+C to stop.")
            while deadline is None or time.monotonic() < deadline:
                time.sleep(interval)
                try:
                    push_fields(args.ip, fields, clear=args.clear)
                except OSError as e:
                    print(f"re-push failed: {e}", file=sys.stderr)
    except KeyboardInterrupt:
        print("\nStopped.")
    except OSError as e:
        sys.exit(f"Network error talking to {args.ip}:{TELNET_PORT}: {e}")


if __name__ == "__main__":
    main()
