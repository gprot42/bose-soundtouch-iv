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
  python3 send-to-bose.py --debug album/          # verbose transport/stream/transcode logs
  python3 send-to-bose.py track.flac              # auto-transcode FLAC→MP3 at serve time
  python3 send-to-bose.py track.flac --no-transcode   # serve as-is (Wave IV won't decode FLAC)
  ffmpeg -i track.flac -f mp3 - | python3 send-to-bose.py -   # pipe pre-transcoded MP3

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
5. Sends DIDL-Lite metadata (title/artist/album) in SetAVTransportURI so
   :8090/nowPlaying reflects the track. The Wave IV front VFD is updated via
   the :17000 abl rdset/rdsend CLI (see README.display-text.md), re-pushed
   every ~1.5 s while playing because firmware overwrites it on refresh.

Requirements: Python 3.8+ (stdlib only, no pip installs needed)

Supported formats on the Bose
-----------------------------
The script can serve any file extension below, but the Bose pedestal must decode it.
On a Wave SoundTouch IV: MP3, AAC/M4A, WMA, and WAV play; FLAC/OGG/Opus do not
(cast may succeed with silence). Newer SoundTouch 10/20/30 units reportedly accept
FLAC — test on your hardware, use auto-transcode (default), or pipe via ffmpeg.
"""

import argparse
import http.server
import importlib.util
import mimetypes
import os
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
from typing import BinaryIO, Callable

DISPLAY_INTERVAL = 1.0   # re-push title; firmware overwrites on now-playing refresh
DISPLAY_BURST_DELAYS = (0.5, 1.0, 2.0)  # extra pushes after each track change

DEBUG = False


def debug_log(msg: str) -> None:
    if DEBUG:
        print(f"[debug] {msg}", file=sys.stderr, flush=True)


# ── SSDP ──────────────────────────────────────────────────────────────────────

SSDP_ADDR = "239.255.255.250"
SSDP_PORT = 1900
SSDP_MX   = 3          # seconds for devices to reply
ST_RENDERER = "urn:schemas-upnp-org:device:MediaRenderer:1"

AUDIO_EXTENSIONS = {
    ".aac", ".flac", ".m4a", ".mp3", ".ogg", ".opus", ".wav", ".wma",
}

# Wave IV decodes these natively; others are transcoded unless --no-transcode.
BOSE_NATIVE_EXTENSIONS = {".aac", ".m4a", ".mp3", ".wav", ".wma"}
TRANSCODE_EXTENSIONS = AUDIO_EXTENSIONS - BOSE_NATIVE_EXTENSIONS

FFMPEG_MP3_ARGS = [
    "-hide_banner", "-loglevel", "error", "-nostdin",
    "-f", "mp3", "-codec:a", "libmp3lame", "-q:a", "2",
]


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
    if DEBUG:
        debug_log(f"SOAP AVTransport.{action}")
    resp = soap(av_ctrl, AV_SVC, action, args)
    if DEBUG and resp is None:
        debug_log(f"SOAP AVTransport.{action} failed")
    return resp


def rc_soap(rc_ctrl: str, action: str, args: str = "") -> str | None:
    return soap(rc_ctrl, RC_SVC, action, args)


# ── Local media paths ───────────────────────────────────────────────────────────

def normalize_local_path(path: str) -> str:
    """Expand ~ and undo shell-style backslash escapes inside quoted args."""
    path = path.strip()
    if len(path) >= 2 and path[0] == path[-1] and path[0] in "\"'":
        path = path[1:-1]
    path = os.path.expanduser(path)
    if "\\" in path:
        path = re.sub(r"\\(.)", r"\1", path)
    return path


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
    debug: bool = False
    _CHUNK = 65536

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

    @staticmethod
    def _parse_range(range_header: str, size: int) -> tuple[int, int] | None:
        """Parse a Range header; return inclusive (start, end) byte offsets."""
        if not range_header.startswith("bytes="):
            return None
        spec = range_header[6:].strip().split(",", 1)[0].strip()
        try:
            if spec.startswith("-"):
                suffix = int(spec[1:])
                if suffix <= 0:
                    return None
                start = max(0, size - suffix)
                return start, size - 1
            start_s, end_s = spec.split("-", 1)
            start = int(start_s) if start_s else 0
            end = int(end_s) if end_s else size - 1
        except ValueError:
            return None
        end = min(end, size - 1)
        if start < 0 or start > end or start >= size:
            return None
        return start, end

    def _file_headers(self, file_path: str) -> tuple[int, str]:
        try:
            size = os.path.getsize(file_path)
        except OSError:
            return 0, ""
        mime, _ = mimetypes.guess_type(file_path)
        if not mime:
            mime = "application/octet-stream"
        return size, mime

    def _send_file(
        self,
        file_path: str,
        *,
        range_header: str | None = None,
        send_body: bool = True,
    ) -> None:
        size, mime = self._file_headers(file_path)
        if size <= 0:
            self.send_error(404)
            return

        start, end = 0, size - 1
        partial = False
        if range_header:
            parsed = self._parse_range(range_header, size)
            if parsed is None:
                self.send_error(416, "Requested Range Not Satisfiable")
                self.send_header("Content-Range", f"bytes */{size}")
                self.end_headers()
                return
            start, end = parsed
            partial = True

        length = end - start + 1
        if partial:
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        else:
            self.send_response(200)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(length))
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Connection", "close")
        self.end_headers()
        if not send_body:
            return
        try:
            with open(file_path, "rb") as fh:
                fh.seek(start)
                remaining = length
                while remaining > 0:
                    chunk = fh.read(min(self._CHUNK, remaining))
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    remaining -= len(chunk)
        except (OSError, BrokenPipeError, ConnectionResetError):
            pass

    def do_HEAD(self):
        file_path = self._resolve_path()
        if not file_path:
            self.send_error(404)
            return
        self._send_file(file_path, send_body=False)

    def do_GET(self):
        file_path = self._resolve_path()
        if not file_path:
            self.send_error(404)
            return
        self._send_file(file_path, range_header=self.headers.get("Range"))

    def log_message(self, fmt: str, *args) -> None:
        if type(self).debug:
            debug_log(f"HTTP {self.address_string()} {fmt % args}")


class _StreamHandler(http.server.BaseHTTPRequestHandler):
    """Stream bytes from a pipe (stdin or ffmpeg) with chunked encoding."""

    byte_source: BinaryIO | None = None
    mime: str = "audio/mpeg"
    on_close: Callable[[], None] | None = None
    debug: bool = False

    def do_GET(self):
        src = self.byte_source
        if src is None:
            self.send_error(503)
            return

        self.send_response(200)
        self.send_header("Content-Type", self.mime)
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("Accept-Ranges", "none")
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            while True:
                chunk = src.read(65536)
                if not chunk:
                    break
                self.wfile.write(f"{len(chunk):x}\r\n".encode())
                self.wfile.write(chunk)
                self.wfile.write(b"\r\n")
                self.wfile.flush()
            self.wfile.write(b"0\r\n\r\n")
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            cb = type(self).on_close
            if cb:
                cb()

    def log_message(self, fmt: str, *args) -> None:
        if type(self).debug:
            debug_log(f"HTTP {self.address_string()} {fmt % args}")


def needs_transcode(path: str) -> bool:
    return os.path.splitext(path)[1].lower() in TRANSCODE_EXTENSIONS


def require_ffmpeg() -> str:
    exe = shutil.which("ffmpeg")
    if not exe:
        sys.exit(
            "ffmpeg not found in PATH — install it or pass --no-transcode.\n"
            "  brew install ffmpeg"
        )
    debug_log(f"ffmpeg: {exe}")
    return exe


def probe_duration(path: str) -> float | None:
    """Return media duration in seconds via ffprobe, if available."""
    ffprobe = shutil.which("ffprobe")
    if not ffprobe:
        debug_log("ffprobe: not found in PATH (duration detection disabled)")
        return None
    result = subprocess.run(
        [
            ffprobe, "-v", "error",
            "-show_entries", "format=duration",
            "-of", "default=noprint_wrappers=1:nokey=1",
            path,
        ],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        debug_log(f"ffprobe failed for {path}: {(result.stderr or '').strip()}")
        return None
    try:
        return float(result.stdout.strip())
    except ValueError:
        return None


def parse_upnp_time(value: str | None) -> float | None:
    """Parse UPnP RelTime/TrackDuration ('H:MM:SS' or 'MM:SS') to seconds."""
    if not value or value == "NOT_IMPLEMENTED":
        return None
    parts = value.strip().split(":")
    try:
        if len(parts) == 3:
            hours, minutes, seconds = parts
            return int(hours) * 3600 + int(minutes) * 60 + float(seconds)
        if len(parts) == 2:
            minutes, seconds = parts
            return int(minutes) * 60 + float(seconds)
        if len(parts) == 1:
            return float(parts[0])
    except ValueError:
        return None
    return None


def format_duration(seconds: float | None) -> str:
    if seconds is None:
        return "?"
    whole = max(0, int(seconds))
    return f"{whole // 60}:{whole % 60:02d}"


def debug_print_toolchain() -> None:
    for name in ("ffmpeg", "ffprobe"):
        path = shutil.which(name)
        if not path:
            debug_log(f"{name}: not found in PATH")
            continue
        try:
            result = subprocess.run(
                [path, "-version"], capture_output=True, text=True, timeout=5,
            )
            first = (result.stdout or result.stderr or "").splitlines()
            debug_log(f"{name}: {path} ({first[0] if first else '?'})")
        except Exception as exc:
            debug_log(f"{name}: {path} (version check failed: {exc})")


def debug_print_device(device: dict, bose_ip: str | None = None) -> None:
    debug_log(
        f"renderer: {device['friendly_name']} av={device['av_ctrl']} "
        f"rc={device.get('rc_ctrl') or '(none)'}"
    )
    if bose_ip:
        debug_log(f"local route to {bose_ip}: {local_ip_toward(bose_ip)}")


def folder_metadata(folder_path: str) -> tuple[str, str]:
    """Derive artist/album from a folder path (parent dir / folder name)."""
    folder = os.path.abspath(folder_path)
    album = os.path.basename(folder)
    parent = os.path.dirname(folder)
    artist = os.path.basename(parent) if parent and parent != "/" else ""
    return artist, album


def transcode_to_mp3(
    input_path: str,
    *,
    title: str = "",
    artist: str = "",
    album: str = "",
) -> tuple[str, Callable[[], None]]:
    """
    Transcode input to a temporary MP3 file.
    Bose renderers need Content-Length (no chunked HTTP); pipe streaming fails silently.
    """
    ffmpeg = require_ffmpeg()
    fd, temp_path = tempfile.mkstemp(suffix=".mp3", prefix="send-to-bose-")
    os.close(fd)
    os.unlink(temp_path)   # mkstemp leaves a 0-byte file; ffmpeg won't overwrite without -y

    def cleanup() -> None:
        try:
            os.unlink(temp_path)
        except OSError:
            pass

    meta_args: list[str] = []
    if title:
        meta_args += ["-metadata", f"title={title}"]
    if artist:
        meta_args += ["-metadata", f"artist={artist}"]
    if album:
        meta_args += ["-metadata", f"album={album}"]

    debug_log(f"transcode start: {input_path} -> {temp_path}")
    started = time.monotonic()
    result = subprocess.run(
        [ffmpeg, "-i", input_path, *meta_args, *FFMPEG_MP3_ARGS, temp_path],
        stderr=subprocess.PIPE,
        text=True,
    )
    elapsed = time.monotonic() - started
    if result.returncode != 0 or not os.path.isfile(temp_path):
        cleanup()
        err = (result.stderr or "").strip()
        sys.exit(f"ffmpeg failed for {input_path}:\n{err}")
    size = os.path.getsize(temp_path)
    if size == 0:
        cleanup()
        sys.exit(f"ffmpeg produced an empty file for {input_path}")
    duration = probe_duration(temp_path)
    debug_log(
        f"transcode done: {format_duration(duration)} "
        f"size={size} bytes elapsed={elapsed:.1f}s"
    )
    return temp_path, cleanup


def ffmpeg_mp3_pipe(input_path: str) -> subprocess.Popen:
    ffmpeg = require_ffmpeg()
    return subprocess.Popen(
        [ffmpeg, "-i", input_path, *FFMPEG_MP3_ARGS, "pipe:1"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def start_stream_server(
    bose_ip: str,
    byte_source: BinaryIO,
    *,
    on_close: Callable[[], None] | None = None,
    path: str = "/stream.mp3",
) -> tuple[http.server.HTTPServer, str]:
    """Serve a byte stream over HTTP; return (httpd, media_url)."""
    _StreamHandler.byte_source = byte_source
    _StreamHandler.on_close = on_close
    _StreamHandler.debug = DEBUG
    httpd = http.server.HTTPServer(("0.0.0.0", 0), _StreamHandler)
    port = httpd.server_address[1]
    my_ip = local_ip_toward(bose_ip)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    quoted = urllib.parse.quote(path.lstrip("/"))
    return httpd, f"http://{my_ip}:{port}/{quoted}"


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
    _MediaHandler.debug = DEBUG
    httpd = http.server.HTTPServer(("0.0.0.0", 0), _MediaHandler)
    port  = httpd.server_address[1]
    my_ip = local_ip_toward(bose_ip)
    base  = f"http://{my_ip}:{port}"
    if file_path:
        debug_log(
            f"HTTP server :{port} single-file "
            f"{os.path.basename(file_path)} "
            f"({os.path.getsize(file_path)} bytes)"
        )
    else:
        debug_log(f"HTTP server :{port} dir={_MediaHandler.serve_dir}")
    threading.Thread(target=httpd.serve_forever, daemon=True).start()

    def url_for_file(path: str) -> str:
        if file_path:
            fname = urllib.parse.quote(os.path.basename(path))
            return f"{base}/{fname}"
        rel = os.path.relpath(os.path.abspath(path), _MediaHandler.serve_dir)
        return f"{base}/{urllib.parse.quote(rel)}"

    return httpd, base, url_for_file


# ── Commands ──────────────────────────────────────────────────────────────────

def stop_renderer(device: dict) -> None:
    av_soap(device["av_ctrl"], "Stop",
            "<InstanceID>0</InstanceID><Speed>1</Speed>")


def cmd_stop(device: dict):
    print(f"Stopping {device['friendly_name']} ...")
    stop_renderer(device)
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
    status = getattr(root.find(".//CurrentTransportStatus"), "text", None)
    uri   = None
    resp2 = av_soap(device["av_ctrl"], "GetMediaInfo",
                    "<InstanceID>0</InstanceID>")
    if resp2:
        r2 = ET.fromstring(resp2)
        for el in r2.iter():
            el.tag = el.tag.split("}")[-1]
        uri = getattr(r2.find(".//CurrentURI"), "text", None)
    rel, track_dur = get_position_info(device)
    print(f"State : {state}")
    if status:
        print(f"Status: {status}")
    if rel is not None:
        pos = format_duration(rel)
        if track_dur:
            pos += f" / {format_duration(track_dur)}"
        print(f"Position: {pos}")
    if uri:
        print(f"URI   : {uri}")


def display_title(label: str) -> str:
    """Derive a short title for the VFD from a track label or URL."""
    name = os.path.basename(label.rstrip("/"))
    root, ext = os.path.splitext(name)
    if ext.lower() in AUDIO_EXTENSIONS:
        return root or name
    return name


def get_transport_state(device: dict) -> str | None:
    resp = av_soap(device["av_ctrl"], "GetTransportInfo",
                   "<InstanceID>0</InstanceID>")
    if not resp:
        return None
    root = ET.fromstring(resp)
    for el in root.iter():
        el.tag = el.tag.split("}")[-1]
    return getattr(root.find(".//CurrentTransportState"), "text", None)


def get_position_info(device: dict) -> tuple[float | None, float | None]:
    """Return (RelTime seconds, TrackDuration seconds) from GetPositionInfo."""
    resp = av_soap(device["av_ctrl"], "GetPositionInfo",
                   "<InstanceID>0</InstanceID>")
    if not resp:
        return None, None
    root = ET.fromstring(resp)
    for el in root.iter():
        el.tag = el.tag.split("}")[-1]
    rel = parse_upnp_time(getattr(root.find(".//RelTime"), "text", None))
    track_dur = parse_upnp_time(getattr(root.find(".//TrackDuration"), "text", None))
    return rel, track_dur


def xml_escape(text: str) -> str:
    return (
        text.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
    )


def build_didl_metadata(
    title: str,
    media_url: str,
    *,
    artist: str = "",
    album: str = "",
    mime: str = "audio/mpeg",
) -> str:
    """Build escaped DIDL-Lite for SetAVTransportURI CurrentURIMetaData."""
    parts = [
        '<DIDL-Lite xmlns="urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/" ',
        'xmlns:dc="http://purl.org/dc/elements/1.1/" ',
        'xmlns:upnp="urn:schemas-upnp-org:metadata-1-0/upnp/">',
        '<item id="0" parentID="-1" restricted="1">',
        f"<dc:title>{xml_escape(title)}</dc:title>",
    ]
    if artist:
        parts.append(f"<dc:creator>{xml_escape(artist)}</dc:creator>")
    if album:
        parts.append(f"<upnp:album>{xml_escape(album)}</upnp:album>")
    parts.extend([
        "<upnp:class>object.item.audioItem.musicTrack</upnp:class>",
        (
            f'<res protocolInfo="http-get:*:{xml_escape(mime)}:*">'
            f"{xml_escape(media_url)}</res>"
        ),
        "</item></DIDL-Lite>",
    ])
    return xml_escape("".join(parts))


def guess_audio_mime(label: str, media_url: str) -> str:
    for candidate in (label, media_url):
        mime, _ = mimetypes.guess_type(candidate)
        if mime:
            return mime
    return "audio/mpeg"


def play_uri(
    device: dict,
    media_url: str,
    *,
    title: str = "",
    artist: str = "",
    album: str = "",
    mime: str = "",
) -> bool:
    shown = title or display_title(media_url)
    if not mime:
        mime = guess_audio_mime(title, media_url)
    metadata = build_didl_metadata(
        shown, media_url, artist=artist, album=album, mime=mime,
    )
    r = av_soap(
        device["av_ctrl"], "SetAVTransportURI",
        f"<InstanceID>0</InstanceID>"
        f"<CurrentURI>{xml_escape(media_url)}</CurrentURI>"
        f"<CurrentURIMetaData>{metadata}</CurrentURIMetaData>",
    )
    if r is None:
        return False
    time.sleep(0.5)
    r = av_soap(device["av_ctrl"], "Play",
                "<InstanceID>0</InstanceID><Speed>1</Speed>")
    return r is not None


def _sleep_interruptible(seconds: float) -> None:
    """Sleep in short slices so Ctrl+C is handled promptly."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        time.sleep(min(0.25, deadline - time.monotonic()))


def wait_for_playback_start(device: dict, timeout: float = 15.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if get_transport_state(device) == "PLAYING":
            return True
        _sleep_interruptible(0.5)
    return False


def wait_for_track_end(
    device: dict,
    poll: float = 2.0,
    on_poll: Callable[[], None] | None = None,
    *,
    expected_duration: float | None = None,
    stopped_polls: int = 2,
    error_polls: int = 5,
) -> None:
    """
    Block until the current track finishes, or raise KeyboardInterrupt.

    Bose SoundTouch often reports STOPPED for only one poll at track end, then
    returns to PLAYING at 0:00:00 on the same URI (loop).  TrackDuration from
    UPnP is usually 0:00:00, so we also use ffprobe duration and RelTime
    position/reset detection.
    """
    if expected_duration:
        debug_log(f"track wait: expected duration {format_duration(expected_duration)}")
    if not wait_for_playback_start(device):
        debug_log("track wait: playback never started")
        return
    stopped_streak = 0
    error_streak = 0
    last_rel = 0.0
    end_margin = 2.0
    while True:
        if on_poll:
            on_poll()
        state = get_transport_state(device)
        rel, track_dur = get_position_info(device)
        if track_dur and not expected_duration:
            expected_duration = track_dur

        if rel is not None and expected_duration:
            if rel >= expected_duration - end_margin:
                debug_log(
                    f"track end: position {format_duration(rel)} reached "
                    f"expected {format_duration(expected_duration)} "
                    f"(state={state})"
                )
                return

        if state in ("STOPPED", "NO_MEDIA_PRESENT"):
            stopped_streak += 1
            error_streak = 0
            near_end = (
                expected_duration is not None
                and (
                    (rel is not None and rel >= expected_duration - 5)
                    or last_rel >= expected_duration - 5
                )
            )
            if near_end or stopped_streak >= stopped_polls:
                debug_log(
                    f"track end: state={state} streak={stopped_streak} "
                    f"rel={format_duration(rel)} last={format_duration(last_rel)}"
                )
                return
        elif state is None:
            error_streak += 1
            stopped_streak = 0
            if error_streak >= error_polls:
                print(
                    "  Warning: lost contact with renderer; assuming track ended.",
                    file=sys.stderr,
                )
                debug_log("track end: renderer contact lost")
                return
        elif (
            state == "PLAYING"
            and rel is not None
            and last_rel >= 30
            and rel <= 5
            and (expected_duration is None or last_rel >= expected_duration * 0.85)
        ):
            debug_log(
                f"track end: position reset {format_duration(last_rel)} -> "
                f"{format_duration(rel)} (renderer looped same URI)"
            )
            return
        else:
            stopped_streak = 0
            error_streak = 0

        if rel is not None:
            last_rel = rel
        if DEBUG:
            debug_log(
                f"transport poll: state={state} rel={format_duration(rel)} "
                f"expected={format_duration(expected_duration)} "
                f"stopped={stopped_streak} errors={error_streak}"
            )
        _sleep_interruptible(poll)


# ── Front display (Wave SoundTouch IV :17000 CLI) ─────────────────────────────

_display_mod = None


def _display_helpers():
    """Lazy-load send-display-text.py (hyphenated filename)."""
    global _display_mod
    if _display_mod is None:
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "send-display-text.py")
        spec = importlib.util.spec_from_file_location("send_display_text", path)
        if spec is None or spec.loader is None:
            raise ImportError(f"cannot load {path}")
        _display_mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(_display_mod)
    return _display_mod


def _bose_volume_level(ip: str) -> int:
    try:
        with urllib.request.urlopen(f"http://{ip}:8090/volume", timeout=3) as resp:
            root = ET.fromstring(resp.read())
        for el in root.iter():
            el.tag = el.tag.split("}")[-1]
        vol = root.find("targetvolume") or root.find("actualvolume")
        if vol is not None and vol.text:
            return int(vol.text)
    except Exception:
        pass
    return 30


def get_now_playing(ip: str) -> dict[str, str]:
    """Read :8090/nowPlaying title lines (empty dict on failure)."""
    try:
        with urllib.request.urlopen(f"http://{ip}:8090/nowPlaying", timeout=3) as resp:
            root = ET.fromstring(resp.read())
    except Exception as exc:
        debug_log(f"nowPlaying fetch failed: {exc}")
        return {}
    for el in root.iter():
        el.tag = el.tag.split("}")[-1]
    out: dict[str, str] = {}
    for tag in ("track", "artist", "album", "stationName"):
        el = root.find(tag)
        if el is not None and el.text:
            out[tag] = el.text.strip()
    item = root.find(".//itemName")
    if item is not None and item.text:
        out["itemName"] = item.text.strip()
    return out


def now_playing_track_title(info: dict[str, str]) -> str:
    return info.get("track") or info.get("itemName") or ""


def clear_display(ip: str) -> bool:
    """Blank all remote-display fields and push to the VFD."""
    try:
        mod = _display_helpers()
        last = mod.push_fields(ip, {}, clear=True)
        debug_log("display cleared")
        return "OK" in last
    except OSError as exc:
        debug_log(f"display clear skipped: {exc}")
        return False


def push_display_title(
    ip: str,
    title: str,
    *,
    artist: str = "",
    album: str = "",
    source: str = "DLNA",
    clear: bool = False,
) -> bool:
    """
    Push now-playing text to the Wave IV VFD via :17000 abl rdset/rdsend.
    Also nudges the console to redraw (sys volume N updateDisplay).
    """
    try:
        mod = _display_helpers()
        fields: dict[str, str] = {"title": display_title(title)}
        if artist:
            fields["artist"] = artist
        if album:
            fields["album"] = album
        if source:
            fields["source"] = source
        last = mod.push_fields(ip, fields, clear=clear)
        if "OK" not in last:
            print(f"  Display warning: rdsend unclear: {last!r}", file=sys.stderr)
            return False
        debug_log(
            f"display push: title={fields['title']!r} clear={clear} "
            f"nowPlaying={now_playing_track_title(get_now_playing(ip))!r}"
        )
        return True
    except OSError as exc:
        print(f"  Display update skipped: {exc}", file=sys.stderr)
        return False


def establish_track_display(
    ip: str,
    label: str,
    *,
    artist: str = "",
    album: str = "",
    burst: bool = False,
    sync_now_playing: bool = False,
) -> bool:
    """
    Clear stale remote-display fields, push the new track, and (for albums)
    burst re-push so firmware now-playing refreshes do not leave the previous
    track on the VFD.
    """
    shown = display_title(label)
    ok = push_display_title(ip, label, artist=artist, album=album, clear=True)
    if burst:
        for delay in DISPLAY_BURST_DELAYS:
            _sleep_interruptible(delay)
            ok = push_display_title(ip, label, artist=artist, album=album) or ok
    if sync_now_playing:
        deadline = time.monotonic() + 8.0
        while time.monotonic() < deadline:
            np_title = now_playing_track_title(get_now_playing(ip))
            if np_title and (np_title == shown or shown in np_title or np_title in shown):
                debug_log(f"nowPlaying synced: {np_title!r}")
                break
            debug_log(f"nowPlaying still {np_title!r}; re-pushing {shown!r}")
            ok = push_display_title(ip, label, artist=artist, album=album) or ok
            _sleep_interruptible(0.5)
    return ok


def display_refresher(
    ip: str,
    title: str,
    *,
    artist: str = "",
    album: str = "",
    interval: float = DISPLAY_INTERVAL,
) -> Callable[[], None]:
    """Return a callback that re-pushes the title on each poll (throttled)."""
    last = 0.0
    expected = display_title(title)

    def refresh() -> None:
        nonlocal last
        now = time.monotonic()
        if now - last < interval:
            return
        np_title = now_playing_track_title(get_now_playing(ip))
        if np_title and np_title != expected:
            debug_log(
                f"display drift: VFD buffer expected {expected!r}, "
                f"nowPlaying has {np_title!r}; re-pushing"
            )
        push_display_title(ip, title, artist=artist, album=album)
        last = now

    return refresh


def cmd_volume(device: dict, level: int):
    if not device.get("rc_ctrl"):
        print("No RenderingControl endpoint found.", file=sys.stderr)
        return
    rc_soap(device["rc_ctrl"], "SetVolume",
            f"<InstanceID>0</InstanceID>"
            f"<Channel>Master</Channel>"
            f"<DesiredVolume>{max(0, min(100, level))}</DesiredVolume>")
    print(f"Volume set to {level}.")


TrackSpec = tuple[str, str] | tuple[str, str, float | None]


def _track_parts(track: TrackSpec) -> tuple[str, str, float | None]:
    if len(track) >= 3:
        return track[0], track[1], track[2]
    return track[0], track[1], None


def _play_tracks(
    device: dict,
    tracks: list[TrackSpec],
    *,
    block: bool | None = None,
    album_mode: bool = False,
    artist: str = "",
    album: str = "",
) -> None:
    if block is None:
        block = not album_mode and len(tracks) == 1
    album_mode = album_mode or len(tracks) > 1

    bose_ip = urllib.parse.urlparse(device["location"]).hostname or ""
    debug_print_device(device, bose_ip or None)

    print(f"Renderer: {device['friendly_name']}  ({device['av_ctrl']})")
    try:
        for i, track in enumerate(tracks, 1):
            if album_mode and i > 1:
                debug_log("album: stop before next track (clear stale now-playing)")
                stop_renderer(device)
                _sleep_interruptible(0.5)
            label, media_url, expected_duration = _track_parts(track)
            if album_mode or len(tracks) > 1:
                print(f"\n[{i}/{len(tracks)}] {label}")
                print(f"URL: {media_url}")
            else:
                print("Setting URI ...")

            shown = display_title(label)

            debug_log(
                f"play track {i}/{len(tracks)}: {label} "
                f"duration={format_duration(expected_duration)} url={media_url}"
            )
            if not play_uri(device, media_url, title=shown, artist=artist, album=album):
                sys.exit("Play command failed — check device IP and port.")

            if bose_ip:
                # DIDL populates :8090/nowPlaying; the VFD is driven via :17000 and
                # firmware refresh can repaint stale lines unless we clear + burst.
                pushed = establish_track_display(
                    bose_ip, label, artist=artist, album=album,
                    burst=album_mode,
                    sync_now_playing=album_mode,
                )
                if wait_for_playback_start(device, timeout=10):
                    pushed = (
                        push_display_title(bose_ip, label, artist=artist, album=album)
                        or pushed
                    )
                if pushed:
                    detail = f"{shown}" + (f" — {artist}" if artist else "")
                    print(f"Display : {detail}")
                else:
                    print("Display : push failed (is telnet :17000 reachable?)",
                          file=sys.stderr)
            refresh_display = (
                display_refresher(bose_ip, label, artist=artist, album=album)
                if bose_ip else None
            )

            print("Playing. Press Ctrl+C to stop.")
            if block:
                while True:
                    if refresh_display:
                        refresh_display()
                    _sleep_interruptible(2)
            else:
                wait_for_track_end(
                    device,
                    on_poll=refresh_display,
                    expected_duration=expected_duration,
                )
                debug_log(f"track finished: {label}")

        if len(tracks) > 1:
            print("\nFolder finished.")
            if bose_ip:
                clear_display(bose_ip)
    except KeyboardInterrupt:
        print("\nStopping ...")
        stop_renderer(device)
        if bose_ip:
            clear_display(bose_ip)
        raise


def cmd_play(device: dict, source: str, *, no_transcode: bool = False):
    bose_host = urllib.parse.urlparse(device["location"]).hostname

    if source not in ("-",) and not (
        source.startswith("http://") or source.startswith("https://")
    ):
        source = normalize_local_path(source)

    if source == "-":
        print("Serving : stdin (audio/mpeg stream)")
        httpd, media_url = start_stream_server(
            bose_host, sys.stdin.buffer, path="/stream.mp3",
        )
        print(f"URL     : {media_url}")
        try:
            _play_tracks(device, [("stdin", media_url)])
        finally:
            httpd.shutdown()
        return

    if source.startswith("http://") or source.startswith("https://"):
        _play_tracks(device, [(source, source)])
        return

    if os.path.isdir(source):
        files = list_audio_files(source)
        if not files:
            exts = ", ".join(sorted(AUDIO_EXTENSIONS))
            sys.exit(
                f"No audio files found in {source}\n"
                f"Supported extensions: {exts}"
            )
        print(f"Folder  : {os.path.abspath(source)} ({len(files)} tracks)")
        artist, album = folder_metadata(source)

        if not any(not no_transcode and needs_transcode(f) for f in files):
            httpd, _, url_for = start_media_server(bose_host, dir_path=source)
            tracks = [
                (os.path.basename(f), url_for(f), probe_duration(f))
                for f in files
            ]
            try:
                _play_tracks(
                    device, tracks, artist=artist, album=album, album_mode=True,
                )
            finally:
                httpd.shutdown()
            return

        try:
            for track_idx, path in enumerate(files):
                if track_idx > 0:
                    debug_log("album: stop before next track (clear stale now-playing)")
                    stop_renderer(device)
                    _sleep_interruptible(0.5)
                label = os.path.basename(path)
                shown = display_title(label)
                if not no_transcode and needs_transcode(path):
                    print(f"\nTranscoding: {label}")
                    temp_path, cleanup = transcode_to_mp3(
                        path, title=shown, artist=artist, album=album,
                    )
                    duration = probe_duration(temp_path)
                    httpd, _, url_for = start_media_server(
                        bose_host, file_path=temp_path,
                    )
                    try:
                        _play_tracks(
                            device, [(label, url_for(temp_path), duration)],
                            block=False,
                            album_mode=True,
                            artist=artist, album=album,
                        )
                    finally:
                        httpd.shutdown()
                        cleanup()
                else:
                    duration = probe_duration(path)
                    httpd, _, url_for = start_media_server(bose_host, file_path=path)
                    try:
                        _play_tracks(
                            device, [(label, url_for(path), duration)],
                            block=False,
                            album_mode=True,
                            artist=artist, album=album,
                        )
                    finally:
                        httpd.shutdown()
        except KeyboardInterrupt:
            if bose_host:
                clear_display(bose_host)
            raise
        print("\nFolder finished.")
        if bose_host:
            clear_display(bose_host)
        return

    if not os.path.isfile(source):
        sys.exit(f"Not found: {source}")

    if not no_transcode and needs_transcode(source):
        label = os.path.basename(source)
        shown = display_title(label)
        artist, album = folder_metadata(os.path.dirname(source))
        print(f"Transcoding: {source}")
        temp_path, cleanup = transcode_to_mp3(
            source, title=shown, artist=artist, album=album,
        )
        httpd, _, url_for = start_media_server(bose_host, file_path=temp_path)
        media_url = url_for(temp_path)
        duration = probe_duration(temp_path)
        print(f"URL     : {media_url}")
        try:
            _play_tracks(
                device, [(label, media_url, duration)],
                artist=artist, album=album,
            )
        finally:
            httpd.shutdown()
            cleanup()
        return

    print(f"Serving : {source}")
    httpd, _, url_for = start_media_server(bose_host, file_path=source)
    media_url = url_for(source)
    duration = probe_duration(source)
    print(f"URL     : {media_url}")
    try:
        _play_tracks(device, [(os.path.basename(source), media_url, duration)])
    finally:
        httpd.shutdown()


# ── Discovery helper ──────────────────────────────────────────────────────────

BOSE_XD_PREFIX = "BO5EBO5E-F00D-F00D-FEED-"


def _split_host_port(ip_port: str, default_port: int) -> tuple[str, int]:
    if ":" in ip_port:
        host, port_s = ip_port.rsplit(":", 1)
        return host, int(port_s)
    return ip_port, default_port


def bose_description_url(host: str, upnp_port: int = 8091) -> str | None:
    """
    Bose Wave/SoundTouch renderers expose UPnP at /XD/BO5EBO5E-F00D-F00D-FEED-{id}.xml.
    The id matches deviceID from the REST API on :8090/info.
    """
    info_url = f"http://{host}:8090/info"
    try:
        with urllib.request.urlopen(info_url, timeout=5) as resp:
            xml_bytes = resp.read()
    except Exception:
        return None

    try:
        root = ET.fromstring(xml_bytes)
    except ET.ParseError:
        return None

    for el in root.iter():
        el.tag = el.tag.split("}")[-1] if "}" in el.tag else el.tag

    device_id = root.get("deviceID")
    if not device_id:
        id_el = _find(root, "deviceID")
        device_id = id_el.text if id_el is not None else None
    if not device_id:
        return None

    return f"http://{host}:{upnp_port}/XD/{BOSE_XD_PREFIX}{device_id}.xml"


def ssdp_location_for_host(host: str, timeout: float = SSDP_MX + 1) -> str | None:
    """M-SEARCH for MediaRenderer:1 and return LOCATION for host, if any."""
    for r in ssdp_discover(timeout=timeout):
        loc = r["location"]
        if urllib.parse.urlparse(loc).hostname == host:
            return loc
    return None


def find_device(args) -> dict:
    """Return a device dict either from --ip or SSDP discovery."""
    if args.ip:
        host, upnp_port = _split_host_port(args.ip, 8091)
        ip_port = f"{host}:{upnp_port}"

        bose_loc = bose_description_url(host, upnp_port)
        if bose_loc:
            dev = parse_device(bose_loc)
            if dev:
                return dev

        ssdp_loc = ssdp_location_for_host(host)
        if ssdp_loc:
            dev = parse_device(ssdp_loc)
            if dev:
                return dev

        for path in ("/description.xml", "/device_description.xml"):
            dev = parse_device(f"http://{ip_port}{path}")
            if dev:
                return dev

        sys.exit(
            f"Could not auto-detect description URL for {ip_port}.\n"
            f"Try: python3 send-to-bose.py --location http://{ip_port}/XD/{BOSE_XD_PREFIX}<deviceID>.xml ..."
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
    signal.signal(signal.SIGINT, signal.default_int_handler)

    p = argparse.ArgumentParser(
        description="Stream audio to a Bose SoundTouch (or any DLNA renderer).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    p.add_argument("source", nargs="?",
                   help="Local file, folder, http(s):// URL, or '-' for stdin MP3")
    p.add_argument("--no-transcode", action="store_true",
                   help="Serve files as-is (no ffmpeg FLAC/OGG/Opus→MP3 conversion)")
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
    p.add_argument("--debug", action="store_true",
                   help="Verbose diagnostics (ffmpeg, transport, HTTP, transcode)")

    args = p.parse_args()

    global DEBUG
    DEBUG = args.debug
    if DEBUG:
        debug_print_toolchain()

    if not (args.source or args.stop or args.status or args.volume is not None):
        p.print_help()
        sys.exit(0)

    try:
        device = find_device(args)

        if args.stop:
            cmd_stop(device)
        elif args.status:
            cmd_status(device)
        elif args.volume is not None:
            cmd_volume(device, args.volume)

        if args.source:
            cmd_play(device, args.source, no_transcode=args.no_transcode)
    except KeyboardInterrupt:
        print()
        sys.exit(130)


if __name__ == "__main__":
    main()
