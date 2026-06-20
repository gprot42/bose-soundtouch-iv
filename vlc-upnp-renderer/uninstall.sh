#!/usr/bin/env bash
# uninstall.sh — remove the VLC UPnP renderer plugins installed by install.sh.
#
# Only removes this project's plugins:
#   libupnp_renderer_plugin.{dylib,so}
#   libupnp_cast_plugin.{dylib,so}
# VLC's built-in libupnp_plugin is never touched.
#
# Options:
#   --vlc-dir DIR       VLC prefix (default: auto-detected)
#   --plugin-dir DIR    Plugins directory to clean (default: $VLC_DIR/plugins or $VLC_DIR/lib/vlc/plugins)
#   --all-locations     Also remove from ~/.local/vlc/plugins
#   --remove-build      Remove built plugin artifacts from build/ as well
#   --dry-run           Show actions without deleting anything
#   --help              Show this help
#
# Usage:
#   ./uninstall.sh
#   ./uninstall.sh --plugin-dir "$HOME/.local/vlc/plugins"
#   ./uninstall.sh --all-locations --remove-build
# ---------------------------------------------------------------------------

set -euo pipefail
IFS=$'\n\t'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ---------------------------------------------------------------------------
# Colours (disabled when not a terminal)
# ---------------------------------------------------------------------------
if [ -t 1 ]; then
    RED='\033[0;31m'; YELLOW='\033[1;33m'; GREEN='\033[0;32m'
    CYAN='\033[0;36m'; BOLD='\033[1m'; RESET='\033[0m'
else
    RED=''; YELLOW=''; GREEN=''; CYAN=''; BOLD=''; RESET=''
fi

info()  { echo -e "${CYAN}[INFO]${RESET}  $*"; }
ok()    { echo -e "${GREEN}[ OK ]${RESET}  $*"; }
warn()  { echo -e "${YELLOW}[WARN]${RESET}  $*"; }
error() { echo -e "${RED}[ERROR]${RESET} $*" >&2; }
fatal() { error "$*"; exit 1; }
step()  { echo -e "\n${BOLD}==> $*${RESET}"; }
dr()    { echo -e "${YELLOW}[DRY-RUN]${RESET} would: $*"; }
hr()    { echo "────────────────────────────────────────────────────────"; }

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
VLC_DIR=""
PLUGIN_DIR=""
ALL_LOCATIONS=false
REMOVE_BUILD=false
DRY_RUN=false

PLUGIN_NAMES=(upnp_renderer upnp_cast)
BUILD_DIR="build"

usage() {
    grep '^#' "$0" | grep -v '^#!/' | sed 's/^# \?//'
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --vlc-dir)        VLC_DIR="${2:?--vlc-dir requires a value}"; shift ;;
        --plugin-dir)     PLUGIN_DIR="${2:?--plugin-dir requires a value}"; shift ;;
        --all-locations)  ALL_LOCATIONS=true ;;
        --remove-build)   REMOVE_BUILD=true ;;
        --dry-run)        DRY_RUN=true ;;
        --help|-h)        usage ;;
        *)                fatal "Unknown argument: $1  (use --help)" ;;
    esac
    shift
done

# ---------------------------------------------------------------------------
# OS detection
# ---------------------------------------------------------------------------
case "$(uname -s)" in
    Darwin) OS=macos; MODULE_SUFFIX=".dylib" ;;
    Linux)  OS=linux; MODULE_SUFFIX=".so" ;;
    *)      fatal "Unsupported OS: $(uname -s)" ;;
esac

plugin_filename() {
    echo "lib${1}_plugin${MODULE_SUFFIX}"
}

# ---------------------------------------------------------------------------
# Resolve VLC paths
# ---------------------------------------------------------------------------
resolve_vlc_dir() {
    if [[ -n "$VLC_DIR" ]]; then
        [[ -d "$VLC_DIR" ]] || fatal "VLC directory not found: $VLC_DIR"
        return
    fi

    if [[ "$OS" == macos ]]; then
        local candidates=(
            "/Applications/VLC.app/Contents/MacOS"
            "$HOME/Applications/VLC.app/Contents/MacOS"
        )
        for dir in "${candidates[@]}"; do
            if [[ -d "$dir/lib" ]]; then
                VLC_DIR="$dir"
                return
            fi
        done
        warn "VLC not found — skipping default VLC plugin directory"
        VLC_DIR=""
        return
    fi

    for dir in /usr /usr/local; do
        if [[ -f "$dir/lib/libvlccore.so" || -f "$dir/lib/x86_64-linux-gnu/libvlccore.so" ]]; then
            VLC_DIR="$dir"
            return
        fi
    done
    warn "VLC not found — skipping default VLC plugin directory"
    VLC_DIR=""
}

default_plugin_dir() {
    if [[ -z "$VLC_DIR" ]]; then
        return 1
    fi
    if [[ "$OS" == macos ]]; then
        echo "${VLC_DIR}/plugins"
    else
        echo "${VLC_DIR}/lib/vlc/plugins"
    fi
}

collect_plugin_dirs() {
    PLUGIN_DIRS=()

    if [[ -n "$PLUGIN_DIR" ]]; then
        PLUGIN_DIRS+=("$PLUGIN_DIR")
        return
    fi

    local default_dir=""
    if default_dir=$(default_plugin_dir); then
        PLUGIN_DIRS+=("$default_dir")
    fi

    if $ALL_LOCATIONS; then
        PLUGIN_DIRS+=("${HOME}/.local/vlc/plugins")
    fi

    # Deduplicate while preserving order.
    local -a unique=()
    local dir seen=false
    for dir in "${PLUGIN_DIRS[@]}"; do
        seen=false
        for existing in "${unique[@]:-}"; do
            if [[ "$existing" == "$dir" ]]; then
                seen=true
                break
            fi
        done
        if ! $seen; then
            unique+=("$dir")
        fi
    done
    PLUGIN_DIRS=("${unique[@]}")
}

remove_plugin_files() {
    local dir="$1"
    local name path removed=false found=false

    if [[ ! -d "$dir" ]]; then
        info "Skipping missing directory: $dir"
        return 0
    fi

    step "Removing plugins from ${dir}"

    for name in "${PLUGIN_NAMES[@]}"; do
        path="${dir}/$(plugin_filename "$name")"
        if [[ ! -f "$path" ]]; then
            info "Not installed: $(basename "$path")"
            continue
        fi
        found=true
        if $DRY_RUN; then
            dr "rm -f \"${path}\""
        else
            rm -f "$path" || uninstall_remove_failed "$path"
            ok "Removed $(basename "$path")"
        fi
        removed=true
    done

    if ! $found; then
        info "No UPnP renderer plugins found in ${dir}"
    elif ! $removed && ! $DRY_RUN; then
        warn "Expected plugins in ${dir} but none were removed"
    fi
}

remove_build_artifacts() {
    step "Removing build artifacts from ${SCRIPT_DIR}/${BUILD_DIR}"
    local name path removed=false

    for name in "${PLUGIN_NAMES[@]}"; do
        path="${SCRIPT_DIR}/${BUILD_DIR}/$(plugin_filename "$name")"
        if [[ ! -f "$path" ]]; then
            info "Not present: $(basename "$path")"
            continue
        fi
        if $DRY_RUN; then
            dr "rm -f \"${path}\""
        else
            rm -f "$path"
            ok "Removed $(basename "$path")"
        fi
        removed=true
    done

    if ! $removed; then
        info "No built plugin artifacts to remove"
    fi
}

uninstall_remove_failed() {
    local path="$1"
    echo ""
    hr
    error "Could not remove: $path"
    hr
    echo ""
    if [[ "$OS" == macos ]]; then
        echo "macOS often blocks writes into VLC.app (Gatekeeper / app bundle protection)."
        echo ""
        echo "Remove the files manually via Finder:"
        echo "  VLC.app → Contents → MacOS → plugins"
        echo "  Delete:"
        for name in "${PLUGIN_NAMES[@]}"; do
            echo "    $(plugin_filename "$name")"
        done
        echo ""
        echo "Or re-run against a user-writable directory if you installed there:"
        echo "  ./uninstall.sh --plugin-dir \"\$HOME/.local/vlc/plugins\""
    else
        echo "Remove the files manually, or re-run with elevated permissions if needed:"
        echo "  sudo ./uninstall.sh"
    fi
    echo ""
    fatal "Uninstall failed."
}

print_summary() {
    echo ""
    hr
    echo -e "${GREEN}${BOLD}  VLC UPnP plugins removed.${RESET}"
    hr
    echo ""
    echo "Restart VLC if it was already running."
    echo ""
    if $ALL_LOCATIONS || [[ -n "$PLUGIN_DIR" ]]; then
        echo "If you still load plugins via VLC_PLUGIN_PATH, unset it before launching VLC."
    fi
    echo ""
    echo "Reinstall later with:"
    echo "  ./install.sh"
    echo ""
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
main() {
    echo ""
    hr
    echo -e "${BOLD}  VLC UPnP Renderer — uninstall${RESET}"
    hr
    echo ""
    echo -e "  OS      : $OS"
    $DRY_RUN && echo -e "  ${YELLOW}DRY-RUN : no changes will be made${RESET}"
    $ALL_LOCATIONS && echo -e "  Scope   : default + ~/.local/vlc/plugins"
    $REMOVE_BUILD && echo -e "  Scope   : includes build/ artifacts"
    echo ""

    resolve_vlc_dir
    collect_plugin_dirs

    if [[ ${#PLUGIN_DIRS[@]} -eq 0 ]]; then
        fatal "No plugin directories to clean. Pass --plugin-dir or install VLC first."
    fi

    info "Plugin directories:"
    for dir in "${PLUGIN_DIRS[@]}"; do
        echo "  - ${dir}"
    done

    local dir
    for dir in "${PLUGIN_DIRS[@]}"; do
        remove_plugin_files "$dir"
    done

    if $REMOVE_BUILD; then
        remove_build_artifacts
    fi

    $DRY_RUN || print_summary
}

main "$@"