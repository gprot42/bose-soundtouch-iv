#!/usr/bin/env bash
# Launch VLC with UPnP renderer plugins from: /Users/aicoder/.local/vlc/plugins
set -euo pipefail

PLUGIN_DIR="/Users/aicoder/.local/vlc/plugins"
VLC_BIN="/Applications/VLC.app/Contents/MacOS/VLC"
VLC_PLUGINS="/Applications/VLC.app/Contents/MacOS/plugins"

if pgrep -xq VLC 2>/dev/null || pgrep -f "${VLC_BIN}" >/dev/null 2>&1; then
    echo "VLC is already running. Quit it first, then retry." >&2
    echo "  pkill -f '${VLC_BIN}'" >&2
    exit 1
fi

# Interrupted runs can leave a half-written cache and hang the next launch.
rm -f "${PLUGIN_DIR}/plugins.dat"

export VLC_PLUGIN_PATH="${PLUGIN_DIR}:${VLC_PLUGINS}"
exec "${VLC_BIN}" "$@"
