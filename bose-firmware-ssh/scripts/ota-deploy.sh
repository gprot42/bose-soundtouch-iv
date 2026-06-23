#!/usr/bin/env bash
# ota-deploy.sh -- push patched firmware to a Bose SoundTouch device via OTA.
#
# What it does (in order):
#   1. Query GET http://<DEVICE>:8090/info  -> device ID, HW rev, current version
#   2. Start scripts/ota-update-server.py  serving the patched .stu on LAN
#   3. Connect to <DEVICE>:17000 (diagnostic telnet shell) and set swUpdateUrl
#      to point at our server, then do envswitch boseurls to make it persistent
#   4. Poll GET http://<DEVICE>:8090/info  until the firmware version changes
#      (or timeout), then restore the original Bose swUpdateUrl
#
# Usage:
#   ./scripts/ota-deploy.sh <device-ip> [firmware.stu] [options]
#
#   device-ip    IP or hostname of the SoundTouch on your LAN
#   firmware.stu Path to patched .stu  (default: work/Update-ssh.stu)
#
# Options:
#   --port PORT          HTTP port for the local update server (default 18000)
#   --bose-update-url U  Original Bose swUpdateUrl to restore after update
#                        (default https://downloads.bose.com/updates/soundtouch)
#   --my-ip IP           LAN IP to advertise in the update index
#                        (auto-detected if omitted)
#   --no-restore         Don't restore the original Bose update URL when done
#   --dry-run            Print what would happen; don't send telnet commands
#   --timeout SECS       Seconds to wait for device to pull and apply firmware
#                        (default 600)
#   --hw-rev REV         Hardware revision override (default: read from /info)
#
# Requirements:
#   python3  nc (netcat, macOS built-in)  curl  xmllint (macOS built-in via libxml2)
#
# Example:
#   ./scripts/ota-deploy.sh 192.168.1.42 work/Update-ssh.stu

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# ---- defaults ---------------------------------------------------------------
DEVICE_IP=""
FIRMWARE=""
SERVER_PORT=18000
BOSE_UPDATE_URL="https://downloads.bose.com/updates/soundtouch"
MY_IP=""
DRY_RUN=0
NO_RESTORE=0
TIMEOUT_SECS=600
HW_REV_OVERRIDE=""

# ---- arg parse --------------------------------------------------------------
if [[ $# -lt 1 ]]; then
    echo "Usage: $0 <device-ip> [firmware.stu] [--port N] [--my-ip IP] [--dry-run] [--no-restore] [--timeout N] [--hw-rev REV]"
    exit 1
fi

DEVICE_IP="$1"; shift

I_UNDERSTAND=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --port)          SERVER_PORT="$2"; shift 2 ;;
        --bose-update-url) BOSE_UPDATE_URL="$2"; shift 2 ;;
        --my-ip)         MY_IP="$2"; shift 2 ;;
        --dry-run)       DRY_RUN=1; shift ;;
        --no-restore)    NO_RESTORE=1; shift ;;
        --i-understand-ota-cannot-enable-ssh) I_UNDERSTAND=1; shift ;;
        --timeout)       TIMEOUT_SECS="$2"; shift 2 ;;
        --hw-rev)        HW_REV_OVERRIDE="$2"; shift 2 ;;
        -*)              echo "Unknown option: $1"; exit 1 ;;
        *)
            if [[ -z "$FIRMWARE" ]]; then FIRMWARE="$1"
            else echo "Unexpected argument: $1"; exit 1
            fi
            shift ;;
    esac
done

FIRMWARE="${FIRMWARE:-$REPO_ROOT/work/Update-ssh.stu}"

if [[ ! -f "$FIRMWARE" ]]; then
    echo "ERROR: firmware file not found: $FIRMWARE"
    echo "  Run: python3 scripts/inplace-patch-stu.py work/Update.stu -o work/Update-ssh.stu"
    exit 1
fi

# ---- helpers ----------------------------------------------------------------
info()  { echo "[ota-deploy] $*"; }
warn()  { echo "[ota-deploy] WARNING: $*" >&2; }
die()   { echo "[ota-deploy] ERROR: $*" >&2; exit 1; }

require() {
    for cmd in "$@"; do
        command -v "$cmd" >/dev/null 2>&1 || die "Required tool not found: $cmd"
    done
}

require curl python3 nc

# ---- HARD BLOCKER NOTICE ----------------------------------------------------
# Remote OTA cannot enable SSH on this firmware. Confirmed live against the
# device. See scripts/enable-ssh-usb.sh for the method that actually works.
if [[ $DRY_RUN -eq 0 && $I_UNDERSTAND -eq 0 ]]; then
    cat >&2 <<'BANNER'
[ota-deploy] ============================================================
[ota-deploy]  REMOTE OTA CANNOT ENABLE SSH ON THIS FIRMWARE
[ota-deploy] ============================================================
[ota-deploy]  Two independent, live-confirmed blockers:
[ota-deploy]
[ota-deploy]   1. The index URL used by /swUpdateCheck
[ota-deploy]      ("https://worldwide.bose.com/...") is NOT overridable.
[ota-deploy]      `sys configuration swUpdateUrl` writes a different PDO;
[ota-deploy]      after a reboot the device STILL reports worldwide.bose.com
[ota-deploy]      (and it is https://, so DNS spoofing fails on the TLS cert).
[ota-deploy]
[ota-deploy]   2. The patched .stu carries the SAME version string, so the
[ota-deploy]      updater treats it as "already up to date" and never fetches.
[ota-deploy]
[ota-deploy]  WORKING METHOD (firmware's own intended path, no flash needed):
[ota-deploy]      ./scripts/enable-ssh-usb.sh
[ota-deploy]    -> writes an empty "remote_services" file to a USB stick;
[ota-deploy]       inserting it starts sshd (22) + telnetd (23) immediately.
[ota-deploy]
[ota-deploy]  To run this OTA flow anyway (e.g. for research / a real newer
[ota-deploy]  build with a bumped version), re-run with:
[ota-deploy]      --i-understand-ota-cannot-enable-ssh
[ota-deploy] ============================================================
BANNER
    exit 2
fi

# ---- 1. Query /info on port 8090 --------------------------------------------
info "Querying http://$DEVICE_IP:8090/info ..."
INFO_XML="$(curl -sf --max-time 10 "http://$DEVICE_IP:8090/info")" \
    || die "Could not reach device at $DEVICE_IP:8090 -- is it on the same LAN?"

# Parse /info using ElementTree (deviceID is an XML attribute on <info>,
# not a child element; softwareVersion has a build suffix to strip).
# Also map <type> -> swUpdateDeviceId used in the OTA INDEX XML.
read -r PRODUCT_NAME SW_VERSION DEVICE_TYPE SW_UPDATE_ID < <(python3 - "$INFO_XML" <<'PYEOF'
import sys, xml.etree.ElementTree as ET

# Known <type> -> swUpdateDeviceId (from BoseApp-*.xml / bonjour.xml)
TYPE_TO_ID = {
    "Wave SoundTouch":    "0x093D",
    "SoundTouch 10":      "0x0939",
    "SoundTouch 20":      "0x093B",
    "SoundTouch 30":      "0x093C",
    "SoundTouch Stereo":  "0x0940",
    "Cinemate":           "0x0942",
    "Lifestyle":          "0x093F",
    "VideoWave":          "0x093E",
}

root = ET.fromstring(sys.argv[1])
name        = (root.findtext("name")       or "").strip().replace(" ", "_")
device_type = (root.findtext("type")       or "").strip()
sw_ver      = ""
for comp in root.findall(".//component"):
    if comp.findtext("componentCategory") == "SCM":
        raw = (comp.findtext("softwareVersion") or "").strip()
        sw_ver = raw.split()[0]   # strip build-date suffix
        break
sw_id = TYPE_TO_ID.get(device_type, "0x093D")
print(name, sw_ver, device_type.replace(" ", "_"), sw_id)
PYEOF
)

[[ -z "$SW_VERSION"    ]] && die "Could not parse softwareVersion from /info response"
[[ -z "$SW_UPDATE_ID"  ]] && SW_UPDATE_ID="0x093D"

# DEVICE_ID_HEX is the product-type ID used in the OTA INDEX XML
DEVICE_ID_HEX="$SW_UPDATE_ID"

# Hardware revision: optional, default to 00.01.00
if [[ -n "$HW_REV_OVERRIDE" ]]; then
    HW_REV="$HW_REV_OVERRIDE"
else
    HW_REV="00.01.00"
fi

info "Device  : '${PRODUCT_NAME//_/ }'  type=${DEVICE_TYPE//_/ }  OTA-ID=$DEVICE_ID_HEX  HW=$HW_REV"
info "Firmware: current=$SW_VERSION"
info "File    : $FIRMWARE  ($(du -sh "$FIRMWARE" | cut -f1))"

# ---- 2. Detect LAN IP -------------------------------------------------------
if [[ -z "$MY_IP" ]]; then
    MY_IP="$(python3 -c "
import socket
with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
    s.connect(('$DEVICE_IP', 80))
    print(s.getsockname()[0])
")"
fi
info "My LAN IP: $MY_IP"
# Full URL to the index file -- passed verbatim to swUpdateCheck (the device
# does NOT auto-append /index.xml, so include the filename).
INDEX_URL="http://$MY_IP:$SERVER_PORT/updates/soundtouch/index.xml"

# ---- 3. Start update server -------------------------------------------------
SERVER_LOG="$(mktemp /tmp/bose-ota-server.XXXXXX.log)"
SERVER_PID=""
TELNET_PY=""

cleanup() {
    if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        info "Stopping update server (PID $SERVER_PID) ..."
        kill "$SERVER_PID" 2>/dev/null || true
    fi
    # Keep the server log for debugging (copy to repo work/ before temp cleanup)
    if [[ -f "$SERVER_LOG" ]]; then
        cp "$SERVER_LOG" "$REPO_ROOT/work/ota-server.log" 2>/dev/null || true
        rm -f "$SERVER_LOG"
    fi
    [[ -f "$TELNET_PY"   ]] && rm -f "$TELNET_PY"
}
trap cleanup EXIT INT TERM

if [[ $DRY_RUN -eq 0 ]]; then
    info "Starting update server on port $SERVER_PORT ..."
    python3 "$SCRIPT_DIR/ota-update-server.py" \
        --firmware "$FIRMWARE" \
        --device-id "$DEVICE_ID_HEX" \
        --hw-rev "$HW_REV" \
        --product-name "${DEVICE_TYPE//_/ }" \
        --current-version "$SW_VERSION" \
        --port "$SERVER_PORT" \
        --advertise-host "$MY_IP" \
        >"$SERVER_LOG" 2>&1 &
    SERVER_PID=$!
    sleep 2
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        cat "$SERVER_LOG"
        die "Update server failed to start"
    fi
    info "Update server running (PID $SERVER_PID)  log: $SERVER_LOG"
else
    info "[DRY RUN] Would start: python3 scripts/ota-update-server.py --firmware $FIRMWARE --device-id $DEVICE_ID_HEX --hw-rev $HW_REV --current-version $SW_VERSION --port $SERVER_PORT --advertise-host $MY_IP"
fi

# ---- 4. Redirect device via port-17000 telnet shell -------------------------
# Write the telnet helper to a temp file so its stdin stays free for commands.
TELNET_PY="$(mktemp /tmp/bose-telnet.XXXXXX.py)"
cat > "$TELNET_PY" << 'TELNET_SCRIPT'
#!/usr/bin/env python3
# Usage: echo "cmd1\ncmd2" | python3 this.py <host> <port>
import socket, sys, time

host, port = sys.argv[1], int(sys.argv[2])
cmds = [l.rstrip('\n') for l in sys.stdin]   # commands from stdin

s = socket.socket()
s.settimeout(15)
try:
    s.connect((host, port))
except Exception as e:
    print(f"ERROR: could not connect to {host}:{port} -- {e}")
    sys.exit(1)

time.sleep(0.5)
s.settimeout(2)
buf = b""
try:
    buf = s.recv(4096)
except Exception:
    pass
if buf:
    print(f"  << {buf.decode(errors='replace').strip()}", flush=True)

for cmd in cmds:
    cmd = cmd.strip()
    if not cmd or cmd.startswith('#'):
        continue
    print(f"  >> {cmd}", flush=True)
    s.sendall((cmd + "\r\n").encode())
    time.sleep(1.5)
    resp = b""
    s.settimeout(3)
    try:
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            resp += chunk
    except socket.timeout:
        pass
    if resp:
        print(f"  << {resp.decode(errors='replace').strip()}", flush=True)

s.close()
TELNET_SCRIPT

telnet_cmd() {
    local cmds="$1"
    if [[ $DRY_RUN -eq 1 ]]; then
        info "[DRY RUN] Would send to $DEVICE_IP:17000:"
        while IFS= read -r line; do
            printf "  > %s\n" "$line"
        done <<< "$cmds"
        return 0
    fi
    # Pipe commands via stdin (script is a file, not a heredoc -- no conflict)
    printf '%s\n' "$cmds" | python3 "$TELNET_PY" "$DEVICE_IP" "17000"
}

info "Pointing swUpdateUrl at our server (config fallback) ..."
TELNET_CMDS="$(cat <<EOF
sys configuration swUpdateUrl $INDEX_URL
EOF
)"
telnet_cmd "$TELNET_CMDS"

# ---- 5. Trigger update via the port-8090 HTTP API ---------------------------
# The reliable trigger is the SoundTouch HTTP API, NOT the bare telnet
# 'swupdate query' (which only checks cloud cache). swUpdateCheck takes an
# explicit indexUrl (highest priority) -- the device fetches it verbatim,
# parses our INDEX, and stages the update. swUpdateStart then downloads+installs.
http_post() {
    # $1 = endpoint (without leading slash), $2 = XML body
    local ep="$1" body="$2"
    if [[ $DRY_RUN -eq 1 ]]; then
        info "[DRY RUN] Would POST http://$DEVICE_IP:8090/$ep"
        printf '         body: %s\n' "$body"
        return 0
    fi
    local resp
    resp="$(curl -s --max-time 15 \
        -X POST "http://$DEVICE_IP:8090/$ep" \
        -H "Content-Type: application/xml" \
        --data "$body" 2>&1)" || true
    info "  <- /$ep: ${resp:-<no body>}"
}

info "Triggering swUpdateCheck with indexUrl=$INDEX_URL ..."
http_post "swUpdateCheck" "<SwUpdateCheck><indexUrl>${INDEX_URL}</indexUrl></SwUpdateCheck>"

info "Waiting 8s for the device to fetch + parse the index ..."
[[ $DRY_RUN -eq 0 ]] && sleep 8

# Show what the check found (best effort)
if [[ $DRY_RUN -eq 0 ]]; then
    QRESP="$(curl -s --max-time 10 "http://$DEVICE_IP:8090/swUpdateQuery" 2>&1 || true)"
    info "  swUpdateQuery: ${QRESP:-<no body>}"
fi

info "Triggering swUpdateStart (force) -- download + install begins ..."
http_post "swUpdateStart" "<SwUpdateStart><indexUrl>${INDEX_URL}</indexUrl><force>true</force></SwUpdateStart>"

# ---- 6. Monitor for completion ----------------------------------------------
# Success signal: device reboots (goes unreachable) then SSH port 22 opens.
# The version number does NOT change (patched firmware has the same version
# string as stock), so version polling would always be a false positive.
info "Watching for device reboot then SSH on port 22 (up to ${TIMEOUT_SECS}s) ..."
info "Server log: $SERVER_LOG  (tail -f to watch downloads)"
DEADLINE=$(( $(date +%s) + TIMEOUT_SECS ))
UPDATED=0
REBOOTING=0

ssh_open() {
    # Returns 0 if TCP port 22 accepts a connection (SSH banner present)
    python3 -c "
import socket, sys
s = socket.socket()
s.settimeout(4)
try:
    s.connect(('$DEVICE_IP', 22))
    banner = s.recv(64)
    sys.exit(0 if banner else 1)
except Exception:
    sys.exit(1)
finally:
    s.close()
" 2>/dev/null
}

while [[ $(date +%s) -lt $DEADLINE ]]; do
    sleep 10
    if [[ $DRY_RUN -eq 1 ]]; then
        info "[DRY RUN] Would watch for reboot + port 22"
        break
    fi

    # Check if device is reachable
    if ! curl -sf --max-time 5 "http://$DEVICE_IP:8090/info" >/dev/null 2>&1; then
        if [[ $REBOOTING -eq 0 ]]; then
            info "  ... device went offline -- applying firmware / rebooting ..."
            REBOOTING=1
        fi
        continue
    fi

    # Device is reachable -- if it was rebooting, check SSH
    if [[ $REBOOTING -eq 1 ]]; then
        info "  ... device back online -- probing SSH port 22 ..."
        if ssh_open; then
            info "SUCCESS -- SSH port 22 is open!"
            UPDATED=1
            break
        else
            info "  ... device up but port 22 still closed -- firmware may not have applied yet ..."
            REBOOTING=0   # reset; wait for another reboot cycle if needed
        fi
    fi

    ELAPSED=$(( $(date +%s) - (DEADLINE - TIMEOUT_SECS) ))
    # Show server activity summary every ~60s
    if [[ $(( ELAPSED % 60 )) -lt 10 ]]; then
        HITS="$(grep -c 'served firmware' "$SERVER_LOG" 2>/dev/null || echo 0)"
        info "  ... waiting (${ELAPSED}s elapsed, firmware served ${HITS}x) ..."
    fi
done

if [[ $DRY_RUN -eq 0 && $UPDATED -eq 0 ]]; then
    # Final SSH check even if no reboot was observed
    if ssh_open; then
        info "SUCCESS -- SSH port 22 is open (update applied on a previous run?)"
        UPDATED=1
    else
        warn "Timed out.  Server log: $SERVER_LOG"
        warn "The device may still be downloading -- keep it powered on."
        warn "Re-run with --no-restore to keep our swUpdateUrl active."
    fi
fi

# ---- 7. Restore original Bose update URL ------------------------------------
if [[ $NO_RESTORE -eq 0 ]]; then
    info "Restoring swUpdateUrl -> $BOSE_UPDATE_URL ..."
    RESTORE_CMDS="$(cat <<EOF
sys configuration swUpdateUrl $BOSE_UPDATE_URL
envswitch boseurls set https://streaming.bose.com $BOSE_UPDATE_URL
EOF
    )"
    telnet_cmd "$RESTORE_CMDS"
fi

info "Done."
if [[ $UPDATED -eq 1 ]]; then
    echo ""
    echo "  Firmware applied successfully."
    echo "  Verify SSH: ssh root@$DEVICE_IP"
fi
