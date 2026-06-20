#!/usr/bin/env python3
"""
send-to-bose.py — stream any audio file or URL to a Bose SoundTouch (or any
UPnP/DLNA MediaRenderer) from the command line. No VLC, no extra apps needed.

Usage
-----
  python3 send-to-bose.py /path/to/song.mp3        # serve local file, play on Bose
  python3 send-to-bose.py /path/to/album/          # play all audio files in folder
  python3 send-to-bose.py http://host/stream.mp3   # direct URL — no server needed
  python3 send-to-bose.py --stop                   # stop current playback
  python3 send-to-bose.py --status                 # show transport state
  python3 send-to-bose.py --volume 40              # set volume 0-100
  python3 send-to-bose.py --ip 192.168.1.50 ...   # skip SSDP, use known IP

How it works
------------
1. Discovers the Bose via SSDP (M-SEARCH for MediaRenderer:1) — takes ~3 s.
   Skip discovery with --ip if you know the UPnP port (usually 8091).
2. For local files or folders: starts a temporary HTTP server on a random high
   port so the Bose can fetch files directly from your Mac. Folders play in
   sorted filename order, one track after another.
3. Sends a SOAP SetAVTransportURI call, then a Play call, to the Bose
   AVTransport endpoint.
4. Waits for Ctrl+C, then sends Stop and shuts down the local server.

Requirements: Python 3.8+ (stdlib only, no pip installs needed)
"""

import argparse
import http.server
import mimetypes
import os
import socket
import struct
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
from typing import Callable

# ── SSDP ──────────────────────────────────────────────────────────────────────

SSDP_ADDR = "239.255.255.250"
SSDP_PORT = 1900
SSDP_MX   = 3          # seconds for devices to reply
ST_RENDERER = "urn:schemas-upnp-org:device:MediaRenderer:1"

AUDIO_EXTENSIONS = {
    ".aac", ".flac", ".m4a", ".mp3", ".ogg", ".opus", ".wav", ".wma",
}


def ssdp_discover(timeout: float = SSDP_MX + 1) -> list[dict]:
    """Send an M-SEARCH and collect all MediaRenderer:1 replies."""
    msg = (
        "M-SEARCH * HTTP/1.1\r\n"
        f"HOST: {SSDP_ADDR}:{SSDP_PORT}\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        f"MX: {SSDP_MX}\r\n"
        f"ST: {ST_RENDERER}\r\n"
        "\r\n"
    ).encode()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 4)
    sock.settimeout(timeout)
    try:
        sock.sendto(msg, (SSDP_ADDR, SSDP_PORT))
        results, seen = [], set()
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            sock.settimeout(remaining)
            try:
                data, _ = sock.recvfrom(4096)
            except socket.timeout:
                break
            text = data.decode(errors="replace")
            location = next(
                (line.split(":", 1)[1].strip()
                 for line in text.splitlines()
                 if line.upper().startswith("LOCATION:")),
                None,
            )
            if location and location not in seen:
                seen.add(location)
                results.append({"location": location})
    finally:
        sock.close()
    return results


# ── UPnP device-description parsing ───────────────────────────────────────────

NS = {
    "upnp": "urn:schemas-upnp-org:device-1-0",
    "av":   "urn:schemas-upnp-org:service:AVTransport:1",
    "rc":   "urn:schemas-upnp-org:service:RenderingControl:1",
}
AV_SVC  = "urn:schemas-upnp-org:service:AVTransport:1"
RC_SVC  = "urn:schemas-upnp-org:service:RenderingControl:1"


def _find(root, tag):
    """ElementTree find with or without namespace prefix."""
    el = root.find(tag)
    if el is None:
        el = root.find(".//" + tag)
    return el


def parse_device(location: str) -> dict | None:
    """
    Fetch device description XML and extract friendlyName + control URLs.
    Returns None if the device does not expose AVTransport (not a renderer).
    """
    try:
        with urllib.request.urlopen(location, timeout=5) as resp:
            xml_bytes = resp.read()
    except Exception as exc:
        print(f"  Could not fetch {location}: {exc}", file=sys.stderr)
        return None

    try:
        root = ET.fromstring(xml_bytes)
    except ET.ParseError:
        return None

    # Strip namespace noise so simple tag finds work
    for el in root.iter():
        el.tag = el.tag.split("}")[-1] if "}" in el.tag else el.tag

    base_url = location.rsplit("/", 1)[0]   # e.g. http://ip:port
    parsed   = urllib.parse.urlparse(location)
    base_url = f"{parsed.scheme}://{parsed.netloc}"

    name_el  = _find(root, "friendlyName")
    friendly = name_el.text if name_el is not None else location

    av_ctrl = rc_ctrl = None
    for svc in root.iter("service"):
        stype_el  = _find(svc, "serviceType")
        sctrl_el  = _find(svc, "controlURL")
        if stype_el is None or sctrl_el is None:
            continue
        stype = stype_el.text or ""
        sctrl = sctrl_el.text or ""
        if not sctrl.startswith("/"):
            sctrl = "/" + sctrl
        if "AVTransport" in stype:
            av_ctrl = base_url + sctrl
        elif "RenderingControl" in stype:
            rc_ctrl = base_url + sctrl

    if not av_ctrl:
        return None   # no AVTransport → not a renderer we can use

    return {
        "friendly_name": friendly,
        "location":      location,
        "av_ctrl":       av_ctrl,
        "rc_ctrl":       rc_ctrl,
    }


# ── SOAP ──────────────────────────────────────────────────────────────────────

def soap(url: str, service: str, action: str, args_xml: str = "") -> str | None:
    body = (
        '<?xml version="1.0"?>'
        '<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
        's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">'
        "<s:Body>"
        f'<u:{action} xmlns:u="{service}">'
        f"{args_xml}"
        f"</u:{action}>"
        "</s:Body>"
        "</s:Envelope>"
    )
    req = urllib.request.Request(url, data=body.encode(), method="POST")
    req.add_header("Content-Type", 'text/xml; charset="utf-8"')
    req.add_header("SOAPAction",   f'"{service}#{action}"')
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            return resp.read().decode()
    except urllib.error.HTTPError as exc:
        err_body = exc.read().decode(errors="replace")
        print(f"  SOAP HTTP {exc.code} error:\n{err_body}", file=sys.stderr)
    except Exception as exc:
        print(f"  SOAP error: {exc}", file=sys.stderr)
    return None


def av_soap(av_ctrl: str, action: str, args: str = "") -> str | None:
    return soap(av_ctrl, AV_SVC, action, args)


def rc_soap(rc_ctrl: str, action: str, args: str = "") -> str | None:
    return soap(rc_ctrl, RC_SVC, action, args)


# ── Local media paths ───────────────────────────────────────────────────────────

def list_audio_files(folder_path: str) -> list[str]:
    """Return absolute paths to playable audio files in folder_path (non-recursive)."""
    folder = os.path.abspath(folder_path)
    files = []
    for name in sorted(os.listdir(folder)):
        path = os.path.join(folder, name)
        if os.path.isfile(path) and os.path.splitext(name)[1].lower() in AUDIO_EXTENSIONS:
            files.append(path)
    return files


# ── Local HTTP server ───────────────────────────────────────────────────────────

def local_ip_toward(dest_ip: str) -> str:
    """Return the local IP the OS would use to reach dest_ip."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((dest_ip, 1))
        return s.getsockname()[0]
    finally:
        s.close()


class _MediaHandler(http.server.BaseHTTPRequestHandler):
    """Serve files from serve_dir; single-file mode also accepts any URL path."""

    serve_dir: str = ""
    single_file: str | None = None

    def _resolve_path(self) -> str | None:
        if self.single_file:
            return self.single_file

        rel = urllib.parse.unquote(urllib.parse.urlparse(self.path).path).lstrip("/")
        if not rel or rel == "..":
            return None

        base = os.path.abspath(self.serve_dir)
        file_path = os.path.normpath(os.path.join(base, rel))
        if not file_path.startswith(base + os.sep):
            return None
        return file_path if os.path.isfile(file_path) else None

    def do_GET(self):
        file_path = self._resolve_path()
        if not file_path:
            self.send_error(404)
            return

        try:
            size = os.path.getsize(file_path)
        except OSError:
            self.send_error(404)
            return

        mime, _ = mimetypes.guess_type(file_path)
        if not mime:
            mime = "application/octet-stream"

        self.send_response(200)
        self.send_header("Content-Type",   mime)
        self.send_header("Content-Length", str(size))
        self.send_header("Accept-Ranges",  "bytes")
        self.send_header("Connection",     "close")
        self.end_headers()
        try:
            with open(file_path, "rb") as fh:
                self.wfile.write(fh.read())
        except (OSError, BrokenPipeError):
            pass

    def log_message(self, *_):
        pass   # suppress access log


def start_media_server(
    bose_ip: str,
    *,
    file_path: str | None = None,
    dir_path: str | None = None,
) -> tuple[http.server.HTTPServer, str, Callable[[str], str]]:
    """Start a temporary HTTP server; return (httpd, base_url, url_for_file)."""
    if bool(file_path) == bool(dir_path):
        raise ValueError("Provide exactly one of file_path or dir_path")

    _MediaHandler.single_file = os.path.abspath(file_path) if file_path else None
    _MediaHandler.serve_dir = os.path.abspath(dir_path or os.path.dirname(file_path))
    httpd = http.server.HTTPServer(("0.0.0.0", 0), _MediaHandler)
    port  = httpd.server_address[1]
    my_ip = local_ip_toward(bose_ip)
    base  = f"http://{my_ip}:{port}"
    threading.Thread(target=httpd.serve_forever, daemon=True).start()

    def url_for_file(path: str) -> str:
        if file_path:
            fname = urllib.parse.quote(os.path.basename(path))
            return f"{base}/{fname}"
        rel = os.path.relpath(os.path.abspath(path), _MediaHandler.serve_dir)
        return f"{base}/{urllib.parse.quote(rel)}"

    return httpd, base, url_for_file


# ── Commands ──────────────────────────────────────────────────────────────────

def cmd_stop(device: dict):
    print(f"Stopping {device['friendly_name']} ...")
    av_soap(device["av_ctrl"], "Stop",
            "<InstanceID>0</InstanceID><Speed>1</Speed>")
    print("Done.")


def cmd_status(device: dict):
    resp = av_soap(device["av_ctrl"], "GetTransportInfo",
                   "<InstanceID>0</InstanceID>")
    if not resp:
        print("No response.")
        return
    root = ET.fromstring(resp)
    for el in root.iter():
        el.tag = el.tag.split("}")[-1]
    state = getattr(root.find(".//CurrentTransportState"), "text", "?")
    uri   = None
    resp2 = av_soap(device["av_ctrl"], "GetMediaInfo",
                    "<InstanceID>0</InstanceID>")
    if resp2:
        r2 = ET.fromstring(resp2)
        for el in r2.iter():
            el.tag = el.tag.split("}")[-1]
        uri = getattr(r2.find(".//CurrentURI"), "text", None)
    print(f"State : {state}")
    if uri:
        print(f"URI   : {uri}")


def get_transport_state(device: dict) -> str | None:
    resp = av_soap(device["av_ctrl"], "GetTransportInfo",
                   "<InstanceID>0</InstanceID>")
    if not resp:
        return None
    root = ET.fromstring(resp)
    for el in root.iter():
        el.tag = el.tag.split("}")[-1]
    return getattr(root.find(".//CurrentTransportState"), "text", None)


def play_uri(device: dict, media_url: str) -> bool:
    r = av_soap(
        device["av_ctrl"], "SetAVTransportURI",
        f"<InstanceID>0</InstanceID>"
        f"<CurrentURI>{media_url}</CurrentURI>"
        f"<CurrentURIMetaData></CurrentURIMetaData>",
    )
    if r is None:
        return False
    time.sleep(0.5)
    r = av_soap(device["av_ctrl"], "Play",
                "<InstanceID>0</InstanceID><Speed>1</Speed>")
    return r is not None


def wait_for_playback_start(device: dict, timeout: float = 15.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if get_transport_state(device) == "PLAYING":
            return True
        time.sleep(0.5)
    return False


def wait_for_track_end(device: dict, poll: float = 2.0) -> None:
    """Block until the current track finishes, or raise KeyboardInterrupt."""
    if not wait_for_playback_start(device):
        return
    while True:
        state = get_transport_state(device)
        if state in ("STOPPED", "NO_MEDIA_PRESENT", None):
            return
        time.sleep(poll)


def cmd_volume(device: dict, level: int):
    if not device.get("rc_ctrl"):
        print("No RenderingControl endpoint found.", file=sys.stderr)
        return
    rc_soap(device["rc_ctrl"], "SetVolume",
            f"<InstanceID>0</InstanceID>"
            f"<Channel>Master</Channel>"
            f"<DesiredVolume>{max(0, min(100, level))}</DesiredVolume>")
    print(f"Volume set to {level}.")


def cmd_play(device: dict, source: str):
    bose_host = urllib.parse.urlparse(device["location"]).hostname

    httpd = None
    tracks: list[tuple[str, str]] = []   # (label, media_url)

    if source.startswith("http://") or source.startswith("https://"):
        tracks = [(source, source)]
    elif os.path.isdir(source):
        files = list_audio_files(source)
        if not files:
            exts = ", ".join(sorted(AUDIO_EXTENSIONS))
            sys.exit(
                f"No audio files found in {source}\n"
                f"Supported extensions: {exts}"
            )
        print(f"Serving folder: {os.path.abspath(source)} ({len(files)} tracks)")
        httpd, _, url_for = start_media_server(bose_host, dir_path=source)
        tracks = [(os.path.basename(f), url_for(f)) for f in files]
    else:
        if not os.path.isfile(source):
            sys.exit(f"Not found: {source}")
        print(f"Serving : {source}")
        httpd, _, url_for = start_media_server(bose_host, file_path=source)
        tracks = [(os.path.basename(source), url_for(source))]
        print(f"URL     : {tracks[0][1]}")

    print(f"Renderer: {device['friendly_name']}  ({device['av_ctrl']})")

    try:
        for i, (label, media_url) in enumerate(tracks, 1):
            if len(tracks) > 1:
                print(f"\n[{i}/{len(tracks)}] {label}")
                print(f"URL: {media_url}")
            else:
                print("Setting URI ...")

            if not play_uri(device, media_url):
                sys.exit("Play command failed — check device IP and port.")

            if len(tracks) == 1:
                print("Playing. Press Ctrl+C to stop.\n")
                while True:
                    time.sleep(2)
            else:
                print("Playing ...")
                wait_for_track_end(device)

        if len(tracks) > 1:
            print("\nFolder finished.")
    except KeyboardInterrupt:
        print("\nStopping ...")
        av_soap(device["av_ctrl"], "Stop",
                "<InstanceID>0</InstanceID><Speed>1</Speed>")
    finally:
        if httpd:
            httpd.shutdown()


# ── Discovery helper ──────────────────────────────────────────────────────────

def find_device(args) -> dict:
    """Return a device dict either from --ip or SSDP discovery."""
    if args.ip:
        # User supplied IP:port directly — build a minimal location URL and probe
        ip_port = args.ip
        if ":" not in ip_port:
            ip_port += ":8091"   # default Bose UPnP port
        # Try common Bose description URL; fall back to /description.xml
        for path in [
            "/XD/",
            "/description.xml",
            "/device_description.xml",
        ]:
            loc = f"http://{ip_port}{path}"
            # Try to find the actual XML via a listing-style URL, or just probe
            dev = parse_device(loc)
            if dev:
                return dev
        # Last resort: tell user to supply full URL
        sys.exit(
            f"Could not auto-detect description URL for {ip_port}.\n"
            f"Try: python3 send-to-bose.py --location http://{ip_port}/XD/your-uuid.xml ..."
        )

    if args.location:
        dev = parse_device(args.location)
        if not dev:
            sys.exit(f"No AVTransport found at {args.location}")
        return dev

    # SSDP
    print(f"Scanning for DLNA renderers ({SSDP_MX + 1}s) ...")
    results = ssdp_discover()
    devices = []
    for r in results:
        dev = parse_device(r["location"])
        if dev:
            devices.append(dev)

    if not devices:
        sys.exit(
            "No UPnP MediaRenderer found on the network.\n"
            "Make sure the Bose is powered on and on the same Wi-Fi.\n"
            "Or use --ip 192.168.0.119 to skip discovery."
        )

    if len(devices) == 1:
        print(f"Found: {devices[0]['friendly_name']}")
        return devices[0]

    print("Multiple renderers found:")
    for i, d in enumerate(devices):
        print(f"  [{i}] {d['friendly_name']}  — {d['location']}")
    try:
        idx = int(input("Select [0]: ").strip() or "0")
    except (ValueError, EOFError):
        idx = 0
    return devices[idx]


# ── CLI ───────────────────────────────────────────────────────────────────────

def main():
    p = argparse.ArgumentParser(
        description="Stream audio to a Bose SoundTouch (or any DLNA renderer).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    p.add_argument("source", nargs="?",
                   help="Local file, folder, or http(s):// URL to play")
    p.add_argument("--stop",     action="store_true",
                   help="Stop current playback")
    p.add_argument("--status",   action="store_true",
                   help="Print transport state and current URI")
    p.add_argument("--volume",   type=int, metavar="0-100",
                   help="Set playback volume")
    p.add_argument("--ip",       metavar="IP[:PORT]",
                   help="Skip SSDP; use known device IP (default port 8091)")
    p.add_argument("--location", metavar="URL",
                   help="Skip SSDP; use full device-description URL")

    args = p.parse_args()

    if not (args.source or args.stop or args.status or args.volume is not None):
        p.print_help()
        sys.exit(0)

    device = find_device(args)

    if args.stop:
        cmd_stop(device)
    elif args.status:
        cmd_status(device)
    elif args.volume is not None:
        cmd_volume(device, args.volume)

    if args.source:
        cmd_play(device, args.source)


if __name__ == "__main__":
    main()
