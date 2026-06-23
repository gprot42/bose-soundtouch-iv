#!/usr/bin/env bash
#
# stu-toolbox.sh -- one-stop Linux toolchain for BOSE Update.stu rebuilds.
#
# Wraps a Linux container (mtd-utils + ubi_reader) so the full
# unpack -> edit -> rebuild -> repack loop works from macOS without a native
# mtd-utils port. By default it manages a colima VM, but it transparently uses
# any Docker-compatible runtime already on your PATH (Docker Desktop, OrbStack,
# podman, nerdctl).
#
# Subcommands:
#   up                                 Ensure a runtime + build the image (idempotent)
#   unpack  <stu> <rootfs_dir>         Carve + extract rootfs (root-owned, perms kept)
#   patch   <rootfs_dir> [opts]        Apply SSH + update-URL patches (see patch-rootfs.sh)
#   build-ssh <stu> <out_stu> [opts]   One-shot: unpack -> patch -> rebuild
#   rebuild <rootfs_dir> <out_stu> [--stock <stu>] [--size N]
#                                      Rebuild UBI from rootfs + repack + verify
#   shell                              Interactive shell in the container (repo mounted)
#   status                             Show resolved runtime / colima state
#   down                              Stop colima (if we started it)
#
# Typical full-rebuild workflow:
#   ./scripts/stu-toolbox.sh up
#   ./scripts/stu-toolbox.sh unpack  work/Update.stu work/rootfs
#   # ... edit work/rootfs (add dropbear keys, scripts, etc.) ...
#   ./scripts/stu-toolbox.sh rebuild work/rootfs work/Update-custom.stu
#
# SSH + custom-update-server build in one shot:
#   ./scripts/stu-toolbox.sh up
#   ./scripts/stu-toolbox.sh build-ssh work/Update.stu work/Update-ssh.stu \
#       --authorized-key-file ~/.ssh/id_ed25519.pub \
#       --swupdate-url http://192.168.0.80:18000/updates/soundtouch
#
# For the SSH-only change you do NOT need this -- use the macOS-native
# scripts/inplace-patch-stu.py instead.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"
IMAGE="bose-mtdtools:latest"
DOCKERFILE="$SCRIPT_DIR/mtdtools.Dockerfile"
STOCK_SIZE_DEFAULT=89128960          # 0x5500000, the stock ubi.img section size
COLIMA_PROFILE="bose-mtd"
RUNTIME=""
WE_STARTED_COLIMA=0

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*" >&2; }
warn() { printf '\033[1;33mwarn:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

# --- runtime resolution -----------------------------------------------------

runtime_works() { command -v "$1" >/dev/null 2>&1 && "$1" info >/dev/null 2>&1; }

ensure_runtime() {
    [ -n "$RUNTIME" ] && return 0
    for rt in docker nerdctl podman; do
        if runtime_works "$rt"; then
            RUNTIME="$rt"
            log "using existing container runtime: $rt"
            return 0
        fi
    done

    # Fall back to colima (the documented macOS workaround).
    command -v colima >/dev/null 2>&1 || die \
"no working container runtime found and colima is not installed.
Install the colima workaround with:
    brew install colima docker
then re-run: $0 up"
    command -v docker >/dev/null 2>&1 || die \
"colima is installed but the 'docker' CLI is missing. Install it with:
    brew install docker"

    if ! colima status --profile "$COLIMA_PROFILE" >/dev/null 2>&1; then
        log "starting colima (profile=$COLIMA_PROFILE, 2 CPU / 4G / 20G) ..."
        colima start --profile "$COLIMA_PROFILE" --cpu 2 --memory 4 --disk 20
        WE_STARTED_COLIMA=1
    else
        log "colima profile '$COLIMA_PROFILE' already running"
    fi
    # Point docker at this colima profile for the duration of the script.
    export DOCKER_HOST="unix://$HOME/.colima/$COLIMA_PROFILE/docker.sock"
    runtime_works docker || die "colima started but 'docker info' still fails (check 'colima status')"
    RUNTIME="docker"
}

image_exists() { "$RUNTIME" image inspect "$IMAGE" >/dev/null 2>&1; }

build_image() {
    ensure_runtime
    log "building $IMAGE (cached layers reused) ..."
    "$RUNTIME" build -t "$IMAGE" -f "$DOCKERFILE" "$SCRIPT_DIR"
}

ensure_image() { image_exists || build_image; }

# Translate a host path that lives under $REPO into its /repo mount equivalent.
in_repo_path() {
    local abs
    abs="$(cd "$(dirname "$1")" 2>/dev/null && pwd)/$(basename "$1")" \
        || die "path does not exist: $1"
    case "$abs" in
        "$REPO"/*) printf '/repo/%s\n' "${abs#"$REPO"/}" ;;
        *) die "path must be inside the repo ($REPO): $1" ;;
    esac
}

# Run a command in the container with the repo mounted read-write at /repo.
run_in() {
    ensure_runtime; ensure_image
    "$RUNTIME" run --rm -v "$REPO":/repo -w /repo "$IMAGE" "$@"
}

# --- subcommands ------------------------------------------------------------

cmd_up() {
    ensure_runtime
    build_image
    log "ready. runtime=$RUNTIME image=$IMAGE"
}

cmd_status() {
    if command -v colima >/dev/null 2>&1; then
        colima status --profile "$COLIMA_PROFILE" 2>&1 | sed 's/^/colima: /' || true
    fi
    if [ -n "$RUNTIME" ] || ensure_runtime 2>/dev/null; then
        echo "runtime: $RUNTIME"
        image_exists && echo "image:   $IMAGE (built)" || echo "image:   $IMAGE (not built -- run '$0 up')"
    fi
}

cmd_down() {
    command -v colima >/dev/null 2>&1 || { warn "colima not installed"; return 0; }
    log "stopping colima profile '$COLIMA_PROFILE' ..."
    colima stop --profile "$COLIMA_PROFILE" || true
}

case_insensitive_mount() {
    # APFS/HFS+ default is case-insensitive; the Bose rootfs has colliding
    # paths (e.g. libxt_MARK.so vs libxt_mark.so) that cannot coexist there.
    local mp
    mp="$(df "$REPO" 2>/dev/null | awk 'NR==2{print $NF}')"
    case "$mp" in
        "") return 1 ;;
        /System/Volumes/*|/Volumes/*)
            # Heuristic only — still warn on typical macOS workspace mounts.
            return 0 ;;
        *) return 1 ;;
    esac
}

cmd_unpack() {
    [ $# -eq 2 ] || die "usage: $0 unpack <stu> <rootfs_dir>"
    local stu_host="$1" dest_host="$2"
    [ -f "$stu_host" ] || die "no such file: $stu_host"
    if case_insensitive_mount; then
        warn "unpack copies the rootfs onto the macOS repo mount."
        warn "Case-colliding files (usr/lib/xtables/libxt_MARK.so vs libxt_mark.so, etc.)"
        warn "will be merged — a later 'rebuild' from that tree can break WiFi/setup."
        warn "Use 'build-ssh' (keeps extract in-container) or edit only inside 'shell'."
    fi
    mkdir -p "$dest_host"
    local stu dest sections
    stu="$(in_repo_path "$stu_host")"
    dest="$(in_repo_path "$dest_host")"
    sections="${dest}/.sections"
    log "carving sections + extracting rootfs (as root, preserving perms) ..."
    run_in sh -euc '
        STU="$1"; DEST="$2"; SEC="$3"
        mkdir -p "$SEC"
        python3 /repo/scripts/extract-stu.py "$STU" -o "$SEC" >/dev/null
        tmp="$(mktemp -d)"
        ubireader_extract_files -o "$tmp" "$SEC/05_ubi.img"
        # ubi_reader nests output as <seq>/rootfs; flatten into DEST.
        inner="$(find "$tmp" -type d -name rootfs | head -1)"
        [ -n "$inner" ] || { echo "rootfs not found after extraction" >&2; exit 1; }
        rm -rf "$DEST"; mkdir -p "$DEST"
        cp -a "$inner"/. "$DEST"/
        rm -rf "$tmp"
    ' _ "$stu" "$dest" "$sections"
    log "rootfs ready: $dest_host"
    log "edit it, then: $0 rebuild $dest_host <out.stu>"
}

cmd_rebuild() {
    [ $# -ge 2 ] || die "usage: $0 rebuild <rootfs_dir> <out_stu> [--stock <stu>] [--size N]"
    local rootfs_host="$1" out_host="$2"; shift 2
    local stock_host="$REPO/work/Update.stu" size="$STOCK_SIZE_DEFAULT"
    while [ $# -gt 0 ]; do
        case "$1" in
            --stock) stock_host="$2"; shift 2 ;;
            --size)  size="$2"; shift 2 ;;
            *) die "unknown option: $1" ;;
        esac
    done
    [ -d "$rootfs_host" ] || die "no such rootfs dir: $rootfs_host"
    [ -f "$stock_host" ]  || die "stock .stu not found: $stock_host (pass --stock)"
    if case_insensitive_mount && [[ "$(cd "$(dirname "$rootfs_host")" && pwd)/$(basename "$rootfs_host")" == "$REPO"/* ]]; then
        warn "rebuild is reading a rootfs under the macOS repo mount."
        warn "If it came from 'unpack', iptables xt modules may be corrupted and WiFi setup (amber/AP) can fail."
        warn "Prefer: $0 build-ssh ...  OR  scripts/inplace-patch-stu.py for SSH-only."
    fi

    local rootfs out stock newubi
    rootfs="$(in_repo_path "$rootfs_host")"
    stock="$(in_repo_path "$stock_host")"
    # output .stu may not exist yet -- resolve its directory then append name.
    mkdir -p "$(dirname "$out_host")"
    out="$(in_repo_path "$(cd "$(dirname "$out_host")" && pwd)/$(basename "$out_host")")"
    newubi="/repo/work/.new_ubi.img"

    log "rebuilding UBI + repacking .stu in container ..."
    run_in sh -euc '
        ROOTFS="$1"; OUT="$2"; STOCK="$3"; SIZE="$4"; NEWUBI="$5"
        mkdir -p /repo/work
        sh /repo/scripts/rebuild-ubi.sh "$ROOTFS" "$NEWUBI" "$SIZE"
        python3 /repo/scripts/repack-stu.py "$STOCK" "$NEWUBI" -o "$OUT"
        echo "--- verify ---"
        python3 /repo/scripts/extract-stu.py "$OUT" --no-carve
        rm -f "$NEWUBI"
    ' _ "$rootfs" "$out" "$stock" "$size" "$newubi"
    log "done: $out_host"
}

cmd_patch() {
    [ $# -ge 1 ] || die "usage: $0 patch <rootfs_dir> [patch-rootfs.sh options]"
    local rootfs_host="$1"; shift
    [ -d "$rootfs_host" ] || die "no such rootfs dir: $rootfs_host"
    local rootfs; rootfs="$(in_repo_path "$rootfs_host")"
    log "applying SSH / update-URL patches to rootfs in container ..."
    run_in sh /repo/scripts/patch-rootfs.sh "$rootfs" "$@"
    log "patched: $rootfs_host"
}

# One-shot: extract -> patch -> rebuild, entirely inside the container's
# case-sensitive ext4 (/tmp). Only the input/output .stu touch the macOS mount,
# because the Bose rootfs has case-colliding files (e.g. libxt_MARK.so vs
# libxt_mark.so) that cannot coexist on a case-insensitive host volume.
# Pass-through options go to patch-rootfs.sh.
cmd_build_ssh() {
    [ $# -ge 2 ] || die \
"usage: $0 build-ssh <stu> <out_stu> [patch options]
  e.g. $0 build-ssh work/Update.stu work/Update-ssh.stu \\
         --authorized-key-file ~/.ssh/id_ed25519.pub \\
         --swupdate-url http://192.168.0.80:18000/updates/soundtouch"
    local stu_host="$1" out_host="$2"; shift 2
    [ -f "$stu_host" ] || die "no such file: $stu_host"

    # Translate a host --authorized-key-file (may live outside /repo, so not
    # mounted) into an inline --authorized-key string the container can use.
    local -a popts=()
    while [ $# -gt 0 ]; do
        case "$1" in
            --authorized-key-file)
                [ -f "$2" ] || die "key file not found: $2"
                popts+=( --authorized-key "$(cat "$2")" ); shift 2 ;;
            *) popts+=( "$1" ); shift ;;
        esac
    done

    local stu out
    stu="$(in_repo_path "$stu_host")"
    mkdir -p "$(dirname "$out_host")"
    out="$(in_repo_path "$(cd "$(dirname "$out_host")" && pwd)/$(basename "$out_host")")"

    log "extract -> patch -> rebuild (in-container ext4) ..."
    run_in sh -euc '
        STU="$1"; OUT="$2"; shift 2
        WORK="$(mktemp -d)"
        trap "rm -rf \"$WORK\"" EXIT
        echo "==> carving sections"
        python3 /repo/scripts/extract-stu.py "$STU" -o "$WORK/sec" >/dev/null
        echo "==> extracting rootfs (case-sensitive ext4)"
        ubireader_extract_files -o "$WORK/ex" "$WORK/sec/05_ubi.img"
        inner="$(find "$WORK/ex" -type d -name rootfs | head -1)"
        [ -n "$inner" ] || { echo "rootfs not found" >&2; exit 1; }
        cp -a "$inner" "$WORK/rootfs"
        echo "==> patching rootfs"
        sh /repo/scripts/patch-rootfs.sh "$WORK/rootfs" "$@"
        SIZE="$(stat -c %s "$WORK/sec/05_ubi.img")"
        echo "==> rebuilding UBI (size=$SIZE) + repacking"
        sh /repo/scripts/rebuild-ubi.sh "$WORK/rootfs" "$WORK/new_ubi.img" "$SIZE"
        python3 /repo/scripts/repack-stu.py "$STU" "$WORK/new_ubi.img" -o "$OUT"
        echo "--- verify ---"
        python3 /repo/scripts/extract-stu.py "$OUT" --no-carve
    ' _ "$stu" "$out" "${popts[@]}"
    log "build-ssh complete: $out_host"
}

cmd_shell() {
    ensure_runtime; ensure_image
    log "entering container shell (repo at /repo). Ctrl-D to exit."
    "$RUNTIME" run --rm -it -v "$REPO":/repo -w /repo "$IMAGE" /bin/bash
}

main() {
    local sub="${1:-}"; shift || true
    case "$sub" in
        up)      cmd_up "$@" ;;
        unpack)  cmd_unpack "$@" ;;
        patch)   cmd_patch "$@" ;;
        build-ssh) cmd_build_ssh "$@" ;;
        rebuild) cmd_rebuild "$@" ;;
        shell)   cmd_shell "$@" ;;
        status)  cmd_status "$@" ;;
        down)    cmd_down "$@" ;;
        ""|-h|--help)
            awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0" ;;
        *) die "unknown subcommand: $sub (try --help)" ;;
    esac
}

main "$@"
