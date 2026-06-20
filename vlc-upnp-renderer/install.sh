#!/usr/bin/env bash
# install.sh — build and deploy the VLC UPnP renderer plugins.
#
# Options:
#   --vlc-dir DIR       VLC prefix (default: /Applications/VLC.app/Contents/MacOS on macOS, /usr on Linux)
#   --vlc-src-dir DIR   VLC source tree for headers (cloned automatically if missing)
#   --plugin-dir DIR    Destination plugins directory (default: $VLC_DIR/plugins or $VLC_DIR/lib/vlc/plugins)
#   --build-dir DIR     CMake build directory (default: build)
#   --vlc-version VER   VLC tag to clone when headers are missing (default: 3.0.23)
#   --skip-build        Install existing build artifacts only
#   --skip-install      Build (and test) only; do not copy plugins
#   --skip-tests        Skip ctest after build
#   --dry-run           Show actions without building or copying
#   --help              Show this help
#
# Usage:
#   ./install.sh
#   ./install.sh --skip-tests
#   ./install.sh --plugin-dir "$HOME/.local/vlc/plugins"
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
VLC_VERSION="3.0.23"
BUILD_DIR="build"
VLC_DIR=""
VLC_SRC_DIR=""
PLUGIN_DIR=""
PLUGIN_DIR_USER_SET=false
PLUGIN_DIR_FALLBACK=false
SKIP_BUILD=false
SKIP_INSTALL=false
SKIP_TESTS=false
DRY_RUN=false

PLUGIN_NAMES=(upnp_renderer upnp_cast)

usage() {
    grep '^#' "$0" | grep -v '^#!/' | sed 's/^# \?//'
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --vlc-dir)      VLC_DIR="${2:?--vlc-dir requires a value}"; shift ;;
        --vlc-src-dir)  VLC_SRC_DIR="${2:?--vlc-src-dir requires a value}"; shift ;;
        --plugin-dir)   PLUGIN_DIR="${2:?--plugin-dir requires a value}"; PLUGIN_DIR_USER_SET=true; shift ;;
        --build-dir)    BUILD_DIR="${2:?--build-dir requires a value}"; shift ;;
        --vlc-version)  VLC_VERSION="${2:?--vlc-version requires a value}"; shift ;;
        --skip-build)   SKIP_BUILD=true ;;
        --skip-install) SKIP_INSTALL=true ;;
        --skip-tests)   SKIP_TESTS=true ;;
        --dry-run)      DRY_RUN=true ;;
        --help|-h)      usage ;;
        *)              fatal "Unknown argument: $1  (use --help)" ;;
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
        fatal "VLC not found. Install VLC 3.0.x or pass --vlc-dir."
    else
        for dir in /usr /usr/local; do
            if [[ -f "$dir/lib/libvlccore.so" || -f "$dir/lib/x86_64-linux-gnu/libvlccore.so" ]]; then
                VLC_DIR="$dir"
                return
            fi
        done
        fatal "VLC not found. Install vlc (e.g. apt install vlc) or pass --vlc-dir."
    fi
}

user_plugin_dir() {
    echo "${HOME}/.local/vlc/plugins"
}

dir_is_writable() {
    local dir="$1"
    mkdir -p "$dir" 2>/dev/null || return 1
    local probe="${dir}/.install-write-test-$$"
    if touch "$probe" 2>/dev/null; then
        rm -f "$probe"
        return 0
    fi
    return 1
}

resolve_plugin_dir() {
    if $PLUGIN_DIR_USER_SET; then
        return
    fi

    local default_dir
    if [[ "$OS" == macos ]]; then
        default_dir="${VLC_DIR}/plugins"
    else
        default_dir="${VLC_DIR}/lib/vlc/plugins"
    fi

    if dir_is_writable "$default_dir"; then
        PLUGIN_DIR="$default_dir"
        return
    fi

    if [[ "$OS" == macos ]]; then
        PLUGIN_DIR="$(user_plugin_dir)"
        PLUGIN_DIR_FALLBACK=true
        warn "Cannot write to ${default_dir} (macOS app bundle protection)."
        info "Using user plugin directory: ${PLUGIN_DIR}"
    else
        PLUGIN_DIR="$default_dir"
    fi
}

resolve_vlc_src_dir() {
    if [[ -n "$VLC_SRC_DIR" ]]; then
        return
    fi

    local repo_src="${SCRIPT_DIR}/../.vlc-src"
    local tmp_src="/tmp/vlc-${VLC_VERSION}"

    if [[ -f "${repo_src}/include/vlc_common.h" ]]; then
        VLC_SRC_DIR="$repo_src"
    elif [[ -f "${tmp_src}/include/vlc_common.h" ]]; then
        VLC_SRC_DIR="$tmp_src"
    else
        VLC_SRC_DIR="$tmp_src"
    fi
}

plugin_filename() {
    echo "lib${1}_plugin${MODULE_SUFFIX}"
}

# ---------------------------------------------------------------------------
# Dependency checks
# ---------------------------------------------------------------------------
check_dependencies() {
    step "Checking dependencies"

    check_cmd() {
        command -v "$1" &>/dev/null || fatal "Required command not found: $1  —  $2"
        ok "$1"
    }

    if ! $SKIP_BUILD; then
        check_cmd cmake "install CMake 3.16+  (brew install cmake)"
        check_cmd cc    "install a C compiler  (xcode-select --install on macOS)"
        if [[ -z "${VLC_SRC_DIR:-}" || ! -f "${VLC_SRC_DIR}/include/vlc_common.h" ]]; then
            check_cmd git "install git  (brew install git)"
        fi
    fi
}

ensure_vlc_headers() {
    if [[ -f "${VLC_SRC_DIR}/include/vlc_common.h" ]]; then
        ok "VLC headers: ${VLC_SRC_DIR}"
        return
    fi

    step "Fetching VLC ${VLC_VERSION} source headers"
    info "Destination: ${VLC_SRC_DIR}"

    if $DRY_RUN; then
        dr "git clone --depth 1 --branch ${VLC_VERSION} https://github.com/videolan/vlc.git \"${VLC_SRC_DIR}\""
        return
    fi

    rm -rf "${VLC_SRC_DIR}"
    git clone --depth 1 --branch "${VLC_VERSION}" \
        https://github.com/videolan/vlc.git "${VLC_SRC_DIR}" \
        || fatal "Failed to clone VLC ${VLC_VERSION} headers."

    [[ -f "${VLC_SRC_DIR}/include/vlc_common.h" ]] \
        || fatal "Cloned tree is missing include/vlc_common.h"
    ok "VLC headers ready"
}

detect_vlc_version() {
    local vlc_bin=""
    if [[ "$OS" == macos && -x "${VLC_DIR}/VLC" ]]; then
        vlc_bin="${VLC_DIR}/VLC"
    elif command -v vlc &>/dev/null; then
        vlc_bin="$(command -v vlc)"
    fi

    if [[ -z "$vlc_bin" ]]; then
        warn "Could not locate vlc binary to verify version"
        return
    fi

    local ver
    ver=$("$vlc_bin" --version 2>&1 | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1 || true)
    if [[ -z "$ver" ]]; then
        warn "Could not parse VLC version from: $vlc_bin"
        return
    fi

    info "Installed VLC: ${ver}"
    if [[ "$ver" != "$VLC_VERSION" ]]; then
        warn "Plugin is built for VLC ${VLC_VERSION}; installed VLC is ${ver}."
        warn "ABI mismatches can prevent plugins from loading — consider matching versions."
    else
        ok "VLC version matches build target (${VLC_VERSION})"
    fi
}

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
configure_build() {
    step "Configuring CMake build"
    info "Build dir : ${SCRIPT_DIR}/${BUILD_DIR}"
    info "VLC_DIR   : ${VLC_DIR}"
    info "VLC_SRC   : ${VLC_SRC_DIR}"
    info "Plugins → : ${PLUGIN_DIR}"

    local -a cmake_args=(
        -B "${SCRIPT_DIR}/${BUILD_DIR}"
        -DCMAKE_BUILD_TYPE=Release
        -DVLC_DIR="${VLC_DIR}"
        -DVLC_SRC_DIR="${VLC_SRC_DIR}"
        -DVLC_PLUGIN_DIR="${PLUGIN_DIR}"
    )

    if $DRY_RUN; then
        dr "cmake ${cmake_args[*]}"
        return
    fi

    cmake "${cmake_args[@]}"
    ok "CMake configure complete"
}

build_plugins() {
    step "Building plugins"
    if $DRY_RUN; then
        dr "cmake --build \"${SCRIPT_DIR}/${BUILD_DIR}\""
        return
    fi

    cmake --build "${SCRIPT_DIR}/${BUILD_DIR}"
    ok "Build complete"
}

run_tests() {
    if $SKIP_TESTS; then
        info "Skipping tests (--skip-tests)"
        return
    fi

    step "Running unit tests"
    if $DRY_RUN; then
        dr "ctest --test-dir \"${SCRIPT_DIR}/${BUILD_DIR}\" --output-on-failure"
        return
    fi

    ctest --test-dir "${SCRIPT_DIR}/${BUILD_DIR}" --output-on-failure
    ok "Tests passed"
}

verify_build_artifacts() {
    step "Verifying build artifacts"
    local name path missing=false
    for name in "${PLUGIN_NAMES[@]}"; do
        path="${SCRIPT_DIR}/${BUILD_DIR}/$(plugin_filename "$name")"
        if [[ ! -f "$path" ]]; then
            error "Missing: $path"
            missing=true
        else
            ok "$(basename "$path")"
        fi
    done
    if $missing; then
        fatal "Build artifacts missing — run without --skip-build."
    fi
}

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install_plugins() {
    step "Installing plugins to ${PLUGIN_DIR}"

    if $DRY_RUN; then
        dr "mkdir -p \"${PLUGIN_DIR}\""
        for name in "${PLUGIN_NAMES[@]}"; do
            dr "cp \"${SCRIPT_DIR}/${BUILD_DIR}/$(plugin_filename "$name")\" \"${PLUGIN_DIR}/\""
        done
        return
    fi

    mkdir -p "${PLUGIN_DIR}"
    rm -f "${PLUGIN_DIR}/plugins.dat"

    local name src dst
    for name in "${PLUGIN_NAMES[@]}"; do
        src="${SCRIPT_DIR}/${BUILD_DIR}/$(plugin_filename "$name")"
        dst="${PLUGIN_DIR}/$(plugin_filename "$name")"
        cp -f "$src" "$dst" || install_copy_failed "$dst"
        ok "Installed $(basename "$dst")"
    done
}

install_copy_failed() {
    local dst="$1"
    echo ""
    hr
    error "Could not write to: $dst"
    hr
    echo ""
    if [[ "$OS" == macos ]]; then
        echo "macOS often blocks writes into VLC.app (Gatekeeper / app bundle protection)."
        echo ""
        echo "Try one of these:"
        echo "  1. Re-run without --plugin-dir (auto-installs to ~/.local/vlc/plugins):"
        echo "       ./install.sh"
        echo "  2. Drag the built .dylib files from ${SCRIPT_DIR}/${BUILD_DIR}/"
        echo "     into VLC.app → Contents → MacOS → plugins (Finder)."
        echo "  3. Load without installing:"
        echo "       ${SCRIPT_DIR}/open-vlc.sh"
    else
        echo "Install to a writable plugins directory, for example:"
        echo "  ./install.sh --plugin-dir \"\$HOME/.local/vlc/plugins\""
        echo "  VLC_PLUGIN_PATH=\"\$HOME/.local/vlc/plugins\" vlc"
        echo ""
        echo "Or re-run with sudo only if you intend to modify system VLC:"
        echo "  sudo ./install.sh"
    fi
    echo ""
    fatal "Install failed."
}

create_launcher_script() {
    local launcher="${SCRIPT_DIR}/open-vlc.sh"
    local vlc_bin="${VLC_DIR}/VLC"
    local vlc_plugins="${VLC_DIR}/plugins"

    if $DRY_RUN; then
        dr "write launcher script: ${launcher}"
        return
    fi

    cat >"$launcher" <<EOF
#!/usr/bin/env bash
# Launch VLC with UPnP renderer plugins from: ${PLUGIN_DIR}
set -euo pipefail

PLUGIN_DIR="${PLUGIN_DIR}"
VLC_BIN="${vlc_bin}"
VLC_PLUGINS="${vlc_plugins}"

if pgrep -xq VLC 2>/dev/null || pgrep -f "\${VLC_BIN}" >/dev/null 2>&1; then
    echo "VLC is already running. Quit it first, then retry." >&2
    echo "  pkill -f '\${VLC_BIN}'" >&2
    exit 1
fi

# Interrupted runs can leave a half-written cache and hang the next launch.
rm -f "\${PLUGIN_DIR}/plugins.dat"

export VLC_PLUGIN_PATH="\${PLUGIN_DIR}:\${VLC_PLUGINS}"
exec "\${VLC_BIN}" "\$@"
EOF
    chmod +x "$launcher"
    ok "Launcher script: ${launcher}"
}

verify_installation() {
    if $DRY_RUN; then
        info "Skipping install verification (--dry-run)"
        return
    fi

    step "Verifying installation"
    local name path missing=false
    for name in "${PLUGIN_NAMES[@]}"; do
        path="${PLUGIN_DIR}/$(plugin_filename "$name")"
        if [[ ! -f "$path" ]]; then
            error "Not found: $path"
            missing=true
        else
            ok "$(basename "$path")"
        fi
    done
    if $missing; then
        fatal "Installed plugins are missing."
    fi
}

uses_custom_plugin_dir() {
    [[ "$PLUGIN_DIR" != "${VLC_DIR}/plugins" && "$PLUGIN_DIR" != "${VLC_DIR}/lib/vlc/plugins" ]]
}

print_next_steps() {
    echo ""
    hr
    echo -e "${GREEN}${BOLD}  VLC UPnP plugins deployed.${RESET}"
    hr
    echo ""
    if $PLUGIN_DIR_FALLBACK; then
        echo "Plugins are in ${PLUGIN_DIR} (VLC.app is not writable on this Mac)."
        echo ""
        echo "Launch VLC with the plugins:"
        echo "  ${SCRIPT_DIR}/open-vlc.sh"
        echo ""
    elif uses_custom_plugin_dir; then
        echo "Custom plugin directory — launch VLC with:"
        echo "  ${SCRIPT_DIR}/open-vlc.sh"
        echo ""
    else
        echo "Restart VLC if it was already running."
        echo ""
    fi
    echo "Usage:"
    echo "  1. Open VLC → Playback → Renderer"
    echo "  2. Select your UPnP device (e.g. Bose SoundTouch)"
    echo "  3. Play a local audio file or http(s) stream"
    echo ""
    echo "Verify plugins load (verbose log):"
    if [[ "$OS" == macos ]]; then
        echo "  VLC_PLUGIN_PATH=\"${PLUGIN_DIR}\" ${VLC_DIR}/VLC -vvv 2>&1 | rg -i 'upnp|renderer'"
    else
        echo "  VLC_PLUGIN_PATH=\"${PLUGIN_DIR}\" vlc -vvv 2>&1 | rg -i 'upnp|renderer'"
    fi
    echo ""
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
main() {
    echo ""
    hr
    echo -e "${BOLD}  VLC UPnP Renderer — build & install${RESET}"
    hr
    echo ""
    echo -e "  OS         : $OS"
    echo -e "  VLC target : ${VLC_VERSION}"
    $DRY_RUN && echo -e "  ${YELLOW}DRY-RUN    : no changes will be made${RESET}"
    $SKIP_BUILD && echo -e "  Mode       : install only"
    $SKIP_INSTALL && echo -e "  Mode       : build only"
    echo ""

    resolve_vlc_dir
    resolve_plugin_dir
    resolve_vlc_src_dir

    check_dependencies
    detect_vlc_version

    if ! $SKIP_BUILD; then
        ensure_vlc_headers
        configure_build
        build_plugins
        run_tests
    else
        info "Skipping build (--skip-build)"
    fi

    verify_build_artifacts

    if ! $SKIP_INSTALL; then
        install_plugins
        verify_installation
        if uses_custom_plugin_dir || $PLUGIN_DIR_FALLBACK; then
            create_launcher_script
        fi
        $DRY_RUN || print_next_steps
    else
        info "Skipping install (--skip-install)"
        ok "Plugins built in ${SCRIPT_DIR}/${BUILD_DIR}/"
    fi
}

main "$@"