#!/usr/bin/env python3
"""
ota-update-server.py -- minimal Bose SoundTouch OTA update server.

Serves two things:
  GET /updates/soundtouch           -> XML INDEX telling the device to pull the firmware
  GET /updates/<firmware-filename>  -> the .stu file itself

Usage (called by ota-deploy.sh; can also be run standalone):
  python3 scripts/ota-update-server.py \
      --firmware  work/Update-ssh.stu \
      --device-id 0x0923 \
      --hw-rev    00.01.00 \
      --version   27.0.7.00001.0000001 \
      --port      18000

The --version must be strictly greater than the device's current version to
trigger an update.  ota-deploy.sh auto-computes a bumped version string.
"""

import argparse
import hashlib
import http.server
import os
import struct
import sys
import threading
import time
import zlib
from pathlib import Path
from urllib.parse import urlparse

INDEX_TEMPLATE = """\
<?xml version="1.0" encoding="UTF-8" ?>
<INDEX REVISION="{index_rev}">
  <DEVICE ID="{device_id}" PRODUCTNAME="{product_name}">
    <HARDWARE REVISION="{hw_rev}">
      <RELEASE REVISION="{version}" HTTPHOST="{httphost}" URLPATH="{urlpath}">
        <IMAGE SUBID="0" LENGTH="{length}" CRC="{crc}" FILENAME="{filename}" />
      </RELEASE>
    </HARDWARE>
  </DEVICE>
</INDEX>
"""

NO_UPDATE_XML = '<?xml version="1.0" encoding="UTF-8"?>\n<software_update_status />\n'


def crc32_of_file(path: Path) -> str:
    """Return CRC32 of file as 0xHHHHHHHH string (unsigned, same as Bose server)."""
    crc = 0
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            crc = zlib.crc32(chunk, crc)
    return hex(crc & 0xFFFFFFFF)


class Handler(http.server.BaseHTTPRequestHandler):
    firmware_path: Path
    index_xml: str
    filename: str

    def log_message(self, fmt, *args):
        ts = time.strftime("%H:%M:%S")
        print(f"[{ts}] {self.address_string()} -- {fmt % args}", flush=True)

    def do_GET(self):
        path = urlparse(self.path).path.rstrip("/")

        # The device uses the indexUrl we pass to swUpdateCheck verbatim (it does
        # NOT auto-append /index.xml). Serve the INDEX for the canonical
        # index.xml path and the legacy /soundtouch alias.
        if path.endswith("/index.xml") or path in ("/updates/soundtouch", "/updates/soundtouch/"):
            body = self.index_xml.encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/xml")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            print(f"  -> served update INDEX ({len(body)} bytes) for {self.path}", flush=True)

        elif self.filename and path.endswith("/" + self.filename):
            # Match the firmware regardless of URL prefix the device builds
            # (HTTPHOST + URLPATH + FILENAME may join with or without slashes).
            size = self.firmware_path.stat().st_size
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(size))
            self.end_headers()
            sent = 0
            with open(self.firmware_path, "rb") as fh:
                while True:
                    chunk = fh.read(1 << 16)
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    sent += len(chunk)
            print(f"  -> served firmware ({sent} bytes)", flush=True)

        else:
            self.send_response(404)
            self.end_headers()
            print(f"  -> 404 (unmatched path: {self.path!r})", flush=True)


def bump_version(current: str) -> str:
    """Increment the last numeric field to guarantee version > current."""
    parts = current.split(".")
    try:
        parts[-1] = str(int(parts[-1]) + 1)
    except ValueError:
        parts.append("1")
    return ".".join(parts)


def main():
    ap = argparse.ArgumentParser(description="Bose SoundTouch OTA update server")
    ap.add_argument("--firmware",   required=True,  help="Path to patched .stu file")
    ap.add_argument("--device-id",  required=True,  help="Hex device ID, e.g. 0x0923")
    ap.add_argument("--hw-rev",     default="00.01.00", help="Hardware revision string")
    ap.add_argument("--current-version", default="",   help="Device current version (auto-bumped)")
    ap.add_argument("--version",    default="",     help="Override release version string")
    ap.add_argument("--host",       default="0.0.0.0", help="Bind address (default 0.0.0.0)")
    ap.add_argument("--port",       type=int, default=18000, help="HTTP port (default 18000)")
    ap.add_argument("--advertise-host", default="", help="LAN IP to embed in index XML")
    ap.add_argument("--product-name", default="Wave SoundTouch", help="PRODUCTNAME attr")
    ap.add_argument("--index-rev",  default="99.99.99", help="INDEX catalog REVISION attr")
    ap.add_argument("--urlpath",    default="updates", help="URLPATH attr (joins host+file)")
    ap.add_argument("--no-update",  action="store_true", help="Serve empty no-update response")
    args = ap.parse_args()

    fw = Path(args.firmware).expanduser().resolve()
    if not fw.exists():
        sys.exit(f"firmware not found: {fw}")

    if args.no_update:
        # ---- restore mode: tell device there is no update available ----------
        Handler.index_xml = NO_UPDATE_XML
        Handler.filename = ""
        Handler.firmware_path = fw
        print("Mode: NO-UPDATE (device will be told no firmware is available)")
    else:
        # ---- serve the patched firmware --------------------------------------
        version = args.version
        if not version:
            if args.current_version:
                version = bump_version(args.current_version)
            else:
                version = "27.0.7.00001.0000001"

        advertise = args.advertise_host or args.host
        if advertise == "0.0.0.0":
            # best-effort: pick the first non-loopback address
            import socket
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
                try:
                    s.connect(("8.8.8.8", 80))
                    advertise = s.getsockname()[0]
                except Exception:
                    advertise = "127.0.0.1"

        httphost = f"http://{advertise}:{args.port}"
        filename = fw.name
        length   = fw.stat().st_size
        crc      = crc32_of_file(fw)

        index_xml = INDEX_TEMPLATE.format(
            index_rev=args.index_rev,
            version=version,
            device_id=args.device_id,
            product_name=args.product_name,
            hw_rev=args.hw_rev,
            httphost=httphost,
            urlpath=args.urlpath,
            filename=filename,
            length=length,
            crc=crc,
        )
        Handler.index_xml    = index_xml
        Handler.filename     = filename
        Handler.firmware_path = fw

        print(f"Firmware : {fw.name}  ({length:,} bytes, CRC {crc})")
        print(f"Device ID: {args.device_id}  HW rev: {args.hw_rev}  product: {args.product_name}")
        print(f"Version  : {args.current_version or '(unknown)'} -> {version}  (INDEX rev {args.index_rev})")
        print(f"Index URL: {httphost}/updates/soundtouch")
        print(f"Image URL: {httphost}/{args.urlpath}/{filename}")
        print("---- INDEX XML ----")
        print(index_xml)
        print("-------------------")

    server = http.server.HTTPServer((args.host, args.port), Handler)
    print(f"\nListening on {args.host}:{args.port}  (Ctrl-C to stop)\n", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nServer stopped.")


if __name__ == "__main__":
    main()
