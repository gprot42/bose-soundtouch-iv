#!/usr/bin/env bash
# Download Wave SoundTouch IV firmware into work/ for offline analysis.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
WORK="$ROOT/work"

ARCHIVE_BASE="https://archive.org/download/bose-soundtouch-software-and-firmware/Firmware/2015-2020_Bluetooth/Bluetooth_Wave_SoundTouch_IV"
DEFAULT_ZIP="Bluetooth_WST4_Update_ti_27.00.06.46330.5043500.nelson.sm2.zip"
VERSION="${1:-27.00.06}"

case "$VERSION" in
    27.00.06) ZIP="$DEFAULT_ZIP" ;;
    27.00.03) ZIP="Bluetooth_WST4_Update_ti_27.00.03.46298.4608935.nelson.sm2.zip" ;;
    *)
        echo "Unknown version: $VERSION" >&2
        echo "Supported: 27.00.06, 27.00.03" >&2
        exit 1
        ;;
esac

mkdir -p "$WORK"
URL="$ARCHIVE_BASE/$ZIP"
DEST="$WORK/firmware-${VERSION}.zip"

echo "Downloading $URL"
curl -fsSL -o "$DEST" "$URL"
unzip -j -o "$DEST" -d "$WORK" "*/Update.stu" "Update.stu" 2>/dev/null || unzip -j -o "$DEST" -d "$WORK"

STU="$WORK/Update.stu"
[[ -f "$STU" ]] || { echo "Update.stu not found in zip" >&2; exit 1; }

echo "Saved: $STU ($(du -sh "$STU" | cut -f1))"
md5 -q "$STU" 2>/dev/null || md5sum "$STU"
echo "Run: python3 scripts/analyze-stu.py work/Update.stu"