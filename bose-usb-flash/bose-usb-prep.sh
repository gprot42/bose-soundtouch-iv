#!/usr/bin/env bash
# bose-usb-prep.sh
# Prepare a USB stick for Bose Wave SoundTouch IV pedestal operations.
#
# Modes (choose one or both):
#   --flash          Download latest WST4 firmware and write Update.stu  (default)
#   --ssh            Write empty remote_services file (enables passwordless root SSH via SoundCork)
#   --both           Both --flash and --ssh on the same stick
#
# Options:
#   --version VER    Use a specific firmware version (e.g. 27.00.06). Default: latest.
#   --firmware FILE  Use a local zip or Update.stu instead of downloading.
#   --dry-run        Show every action but do not format, write, or eject anything.
#   --yes            Skip interactive confirmations (only for automation; requires exactly 1 USB disk).
#   --list-versions  Print known firmware versions and exit.
#   --help           Show this help.
#
# Usage examples:
#   ./bose-usb-prep.sh                          # interactive, flash mode
#   ./bose-usb-prep.sh --both                   # flash + SSH mode
#   ./bose-usb-prep.sh --ssh --dry-run          # preview SSH mode, no changes
#   ./bose-usb-prep.sh --firmware ./my.zip      # use already-downloaded firmware zip
#   ./bose-usb-prep.sh --list-versions          # see all known firmware versions
# ---------------------------------------------------------------------------

set -euo pipefail
IFS=$'\n\t'

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
# Constants
# ---------------------------------------------------------------------------
ARCHIVE_BASE="https://archive.org/download/bose-soundtouch-software-and-firmware/Firmware/2015-2020_Bluetooth/Bluetooth_Wave_SoundTouch_IV"
VOLUME_LABEL="BOSEFLASH"
MAX_SIZE_GB=32
FIRMWARE_FILENAME="Update.stu"
SSH_TRIGGER_FILE="remote_services"
LATEST_ZIP="Bluetooth_WST4_Update_ti_27.00.06.46330.5043500.nelson.sm2.zip"
MIN_STU_SIZE_BYTES=$((10 * 1024 * 1024))
MAX_STU_SIZE_BYTES=$((1024 * 1024 * 1024))

declare -a KNOWN_VERSIONS=(
    "27.00.06:Bluetooth_WST4_Update_ti_27.00.06.46330.5043500.nelson.sm2.zip"
    "27.00.03:Bluetooth_WST4_Update_ti_27.00.03.46298.4608935.nelson.sm2.zip"
    "27.00.02:Bluetooth_WST4_Update_ti_27.00.02.46286.4536626.nelson.sm2.zip"
    "27.00.01:Bluetooth_WST4_Update_ti_27.00.01.46282.4378406.nelson.sm2.zip"
    "26.00.01:Bluetooth_WST4_Update_ti_26.00.01.46256.3990103.nelson.sm2.zip"
    "25.00.00:Bluetooth_WST4_Update_ti_25.00.00.46176.3844119.nelson.sm2.zip"
    "24.00.07:Bluetooth_WST4_Update_ti_24.00.07.46067.3722005.nelson.sm2.zip"
    "14.00.33:Bluetooth_WST4_14.00.33.zip"
    "09.00.41:Bluetooth_WST4_09.00.41.zip"
)

# ---------------------------------------------------------------------------
# Argument parsing  (before banner so --help/--list-versions exit fast)
# ---------------------------------------------------------------------------
MODE_FLASH=false; MODE_SSH=false; DRY_RUN=false; SKIP_CONFIRM=false
REQUESTED_VERSION=""; LOCAL_FIRMWARE=""

usage() { grep '^#' "$0" | grep -v '^#!/' | sed 's/^# \?//'; exit 0; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --flash)         MODE_FLASH=true ;;
        --ssh)           MODE_SSH=true ;;
        --both)          MODE_FLASH=true; MODE_SSH=true ;;
        --dry-run)       DRY_RUN=true ;;
        --yes)           SKIP_CONFIRM=true ;;
        --version)       REQUESTED_VERSION="${2:?--version requires a value}"; shift ;;
        --firmware)      LOCAL_FIRMWARE="${2:?--firmware requires a path}"; shift ;;
        --list-versions)
            echo "Known Wave SoundTouch IV firmware versions (newest first):"
            for e in "${KNOWN_VERSIONS[@]}"; do
                printf "  %-12s  %s\n" "${e%%:*}" "${e##*:}"
            done
            echo -e "\nArchive: ${ARCHIVE_BASE}/"; exit 0 ;;
        --help|-h) usage ;;
        *) fatal "Unknown argument: $1  (use --help)" ;;
    esac
    shift
done
if ! $MODE_FLASH && ! $MODE_SSH; then MODE_FLASH=true; fi

# ---------------------------------------------------------------------------
# OS detection
# ---------------------------------------------------------------------------
case "$(uname -s)" in
    Darwin) OS=macos ;;
    Linux)  OS=linux ;;
    *)      fatal "Unsupported OS: $(uname -s)" ;;
esac

# ---------------------------------------------------------------------------
# Banner + requirements  (printed immediately on launch)
# ---------------------------------------------------------------------------
echo ""
hr
echo -e "${BOLD}  Bose Wave SoundTouch IV — USB preparation tool${RESET}"
hr
echo ""
echo -e "  Mode    : $(
    parts=()
    $MODE_FLASH && parts+=("firmware flash  (writes Update.stu)")
    $MODE_SSH   && parts+=("SSH enable      (writes remote_services)")
    IFS=', '; echo "${parts[*]}"
)"
echo -e "  OS      : $OS"
$DRY_RUN && echo -e "  ${YELLOW}DRY-RUN : no changes will be made${RESET}"
echo ""
hr
echo -e "${BOLD}  Requirements${RESET}"
hr
echo "  • A USB stick ≤ ${MAX_SIZE_GB} GB — it will be completely erased and reformatted FAT32"
echo "  • Do NOT select your system disk or any internal drive"
if $MODE_FLASH; then
echo "  • ~73 MB internet download from archive.org  (or --firmware <path> to use local file)"
fi
echo ""
hr
echo -e "${BOLD}  Safety checks this script performs${RESET}"
hr
echo "  [1] Refuses the system boot disk"
echo "  [2] Refuses any disk larger than ${MAX_SIZE_GB} GB"
echo "  [3] Requires you to type YES before any data is erased"
echo "  [4] Verifies Update.stu MD5 checksum after writing"
echo "  [5] Strips macOS junk files (.fseventsd, .Spotlight-V100, .DS_Store, ._*)"
echo "  [6] Confirms expected files on USB root before ejecting"
echo ""
hr
echo ""

# ---------------------------------------------------------------------------
# Dependency checks
# ---------------------------------------------------------------------------
step "Checking dependencies"

check_cmd() {
    command -v "$1" &>/dev/null || fatal "Required command not found: $1  —  $2"
    ok "$1"
}
check_cmd curl  "install curl  (brew install curl)"
check_cmd unzip "install unzip (brew install unzip)"
check_cmd stat  "built-in on macOS/Linux"
if [[ "$OS" == macos ]]; then
    check_cmd diskutil "built-in on macOS"
    check_cmd awk      "built-in on macOS"
else
    check_cmd lsblk     "install util-linux"
    check_cmd mkfs.vfat "install dosfstools: sudo apt install dosfstools"
    check_cmd awk       "built-in on most Linux distros"
    [[ $EUID -eq 0 ]] || fatal "On Linux this script must run as root: sudo $0 $*"
fi

# ---------------------------------------------------------------------------
# Firmware version resolver
# ---------------------------------------------------------------------------
resolve_firmware_zip() {
    local ver="${1:-27.00.06}"
    for e in "${KNOWN_VERSIONS[@]}"; do
        [[ "${e%%:*}" == "$ver" ]] && { echo "${e##*:}"; return 0; }
    done
    fatal "Unknown version: $ver  (run --list-versions to see valid values)"
}

file_size_bytes() {
    local path="$1"
    if [[ "$OS" == macos ]]; then
        stat -f%z "$path" 2>/dev/null || echo 0
    else
        stat -c%s "$path" 2>/dev/null || echo 0
    fi
}

validate_update_stu_file() {
    local stu="$1" label="${2:-$FIRMWARE_FILENAME}"
    [[ -f "$stu" ]] || fatal "$label not found: $stu"
    local size
    size=$(file_size_bytes "$stu")
    (( size >= MIN_STU_SIZE_BYTES )) || fatal "$label is too small ($(du -sh "$stu" | cut -f1)); firmware file may be incomplete."
    (( size <= MAX_STU_SIZE_BYTES )) || fatal "$label is unexpectedly large ($(du -sh "$stu" | cut -f1)); refusing to write suspicious firmware."
    ok "$label size sanity check passed ($(du -sh "$stu" | cut -f1))" >&2
}

validate_zip_archive() {
    local zip="$1" entry found=false
    step "Validating firmware archive" >&2
    [[ -f "$zip" ]] || fatal "Firmware zip not found: $zip"
    unzip -tq "$zip" >/dev/null 2>&1 || fatal "Firmware zip failed integrity test — re-download and retry."
    while IFS= read -r entry; do
        [[ "${entry##*/}" == "$FIRMWARE_FILENAME" ]] && { found=true; break; }
    done < <(unzip -Z1 "$zip" 2>/dev/null || true)
    $found || fatal "$FIRMWARE_FILENAME not found in firmware zip."
    ok "Zip integrity check passed and contains $FIRMWARE_FILENAME" >&2
}

# ---------------------------------------------------------------------------
# Disk detection helpers  — pure awk/grep/text, no Python, no gawk extensions
# ---------------------------------------------------------------------------

# macOS: returns tab-separated lines: diskN <TAB> size_bytes <TAB> media_name
# Filters out disk images (Protocol: Disk Image) and synthesized APFS containers.
list_usb_candidates_macos() {
    local disks
    disks=$(diskutil list external 2>/dev/null \
        | awk '/^\/dev\/disk/{gsub("/dev/",""); print $1}')
    [[ -z "$disks" ]] && return 0

    while read -r d; do
        [[ -z "$d" ]] && continue
        local ifo
        ifo=$(diskutil info "$d" 2>/dev/null) || continue
        # Skip virtual / disk-image / synthesized entries
        echo "$ifo" | grep -qiE 'Virtual:\s+Yes|Protocol:\s+Disk Image|Synthesized' && continue
        local size_bytes name
        size_bytes=$(echo "$ifo" | grep "Disk Size" \
            | grep -oE '\([0-9]+ Bytes\)' | grep -oE '[0-9]+' | head -1)
        name=$(echo "$ifo" | awk -F': ' '/Media Name/{sub(/^[[:space:]]*/,"",$2); print $2; exit}')
        [[ -z "$size_bytes" ]] && size_bytes=0
        [[ -z "$name"       ]] && name="(unknown)"
        printf '%s\t%s\t%s\n' "$d" "$size_bytes" "$name"
    done <<< "$disks"
}

# Linux: returns tab-separated lines: sdX <TAB> size_bytes <TAB> model
list_usb_candidates_linux() {
    lsblk -o NAME,SIZE,TYPE,TRAN,MODEL -b -n 2>/dev/null \
    | awk '$3=="disk" && ($4=="usb" || $4=="") {
        name=$1; size=$2; model=""
        for(i=5;i<=NF;i++) model=model" "$i
        sub(/^ /,"",model)
        if (size+0 > 0) printf "%s\t%s\t%s\n", name, size, model
    }'
}

# Print a human-readable list of ALL currently attached disks.
show_all_disks() {
    echo "  ── All disks currently visible ──────────────────────────────────"
    if [[ "$OS" == macos ]]; then
        diskutil list 2>/dev/null | grep -E '^/dev/disk|\s+[0-9]+:' | sed 's/^/  /'
    else
        lsblk -o NAME,SIZE,TYPE,TRAN,MODEL 2>/dev/null | sed 's/^/  /'
    fi
    echo "  ─────────────────────────────────────────────────────────────────"
}

# Print the example of what a USB stick looks like.
show_usb_example() {
    if [[ "$OS" == macos ]]; then
        echo "  Your USB stick appears as:  /dev/disk2  (external, physical)"
        echo "  Type just the short name without /dev/ — e.g.:  disk2"
    else
        echo "  Your USB stick appears with TRAN=usb — e.g.:  sdb"
        echo "  Type just the short name without /dev/ — e.g.:  sdb"
    fi
}

# ---------------------------------------------------------------------------
# Guard: confirm a disk is a safe, removable USB stick before touching it.
# Returns 0 (safe) or prints a reason and returns 1 (unsafe).
# Called at SELECTION TIME for every candidate — auto-detected or manually typed.
# ---------------------------------------------------------------------------
is_safe_usb() {
    local disk="$1" dev="/dev/$1"
    local reason=""

    # Must exist as a block device
    if [[ ! -b "$dev" ]]; then
        reason="$dev does not exist as a block device."
        warn "  BLOCKED: $reason"; return 1
    fi

    if [[ "$OS" == macos ]]; then
        local ifo
        ifo=$(diskutil info "$disk" 2>/dev/null)

        # Reject if internal
        if echo "$ifo" | grep -qiE 'Internal:\s+Yes'; then
            reason="$disk is an INTERNAL disk."
            warn "  BLOCKED: $reason  Internal drives cannot be selected."
            return 1
        fi

        # Reject disk images / virtual disks
        if echo "$ifo" | grep -qiE 'Virtual:\s+Yes|Protocol:\s+Disk Image|Synthesized'; then
            reason="$disk is a virtual disk or disk image."
            warn "  BLOCKED: $reason  Only physical USB sticks are allowed."
            return 1
        fi

        # Reject the boot/system disk (belt-and-suspenders — diskutil list external
        # already excludes it, but a manual entry bypasses that filter)
        local boot_disk
        boot_disk=$(diskutil info / 2>/dev/null | awk '/Part of Whole/{print $NF}')
        if [[ "$disk" == "$boot_disk" ]]; then
            reason="$disk is the system boot disk  ($boot_disk)."
            warn "  BLOCKED: $reason  The system disk cannot be selected."
            return 1
        fi

        # Reject any disk whose parent is the boot disk (boot disk partitions)
        local parent
        parent=$(echo "$ifo" | awk '/Part of Whole/{print $NF}')
        if [[ -n "$parent" && "$parent" == "$boot_disk" && "$disk" != "$boot_disk" ]]; then
            reason="$disk is a PARTITION of the system boot disk ($boot_disk)."
            warn "  BLOCKED: $reason"
            return 1
        fi

        # Reject oversized disks
        local sb
        sb=$(echo "$ifo" | grep "Disk Size" \
            | grep -oE '\([0-9]+ Bytes\)' | grep -oE '[0-9]+' | head -1)
        local sg=$(( ${sb:-0} / 1024 / 1024 / 1024 ))
        if (( sg > MAX_SIZE_GB )); then
            reason="$disk is ${sg} GB — larger than the ${MAX_SIZE_GB} GB limit."
            warn "  BLOCKED: $reason  Use a USB stick that is ≤ ${MAX_SIZE_GB} GB."
            return 1
        fi

        # Warn (don't block) if the device is not flagged as removable
        if echo "$ifo" | grep -qiE 'Ejectable:\s+No|Removable Media:\s+Fixed'; then
            warn "  WARNING: $disk does not appear to be removable media."
            warn "           If this is an external SSD or hard drive (not a USB stick),"
            warn "           type a different identifier or Ctrl+C to abort."
        fi

    else  # Linux

        # Reject if transport is internal (sata, nvme, ata, mmc, scsi on internal bus)
        local tran
        tran=$(lsblk -o TRAN -n "$dev" 2>/dev/null | head -1 | tr -d ' ')
        case "$tran" in
            sata|ata|nvme|mmc|scsi)
                reason="$disk has transport '$tran' — this is an INTERNAL drive."
                warn "  BLOCKED: $reason  Only USB transport drives are allowed."
                return 1 ;;
        esac

        # Reject root filesystem disk
        local root_dev
        root_dev=$(df / | awk 'NR==2{gsub(/[0-9]+$/,"",$1); print $1}')
        if [[ "$dev" == "$root_dev" ]]; then
            reason="$disk is the root filesystem device."
            warn "  BLOCKED: $reason"
            return 1
        fi

        # Reject /boot disk
        if mountpoint -q /boot 2>/dev/null; then
            local bd
            bd=$(df /boot | awk 'NR==2{gsub(/[0-9]+$/,"",$1); print $1}')
            if [[ "$dev" == "$bd" ]]; then
                reason="$disk contains /boot."
                warn "  BLOCKED: $reason"
                return 1
            fi
        fi

        # Reject oversized
        local sb sg
        sb=$(lsblk -b -o SIZE -n "$dev" 2>/dev/null | head -1 | tr -d ' ')
        sg=$(( ${sb:-0} / 1024 / 1024 / 1024 ))
        if (( sg > MAX_SIZE_GB )); then
            reason="$disk is ${sg} GB — larger than the ${MAX_SIZE_GB} GB limit."
            warn "  BLOCKED: $reason  Use a USB stick that is ≤ ${MAX_SIZE_GB} GB."
            return 1
        fi
    fi

    return 0  # safe
}

# ---------------------------------------------------------------------------
# Disk selection  — sets global SELECTED_DISK
# (NOT called via $() so display output goes straight to the terminal)
# ---------------------------------------------------------------------------
SELECTED_DISK=""

select_disk() {
    step "Detecting removable USB disks"
    info "Scanning — this is fast (no internet, no writes)..."

    local candidates
    if [[ "$OS" == macos ]]; then
        candidates=$(list_usb_candidates_macos)
    else
        candidates=$(list_usb_candidates_linux)
    fi

    # ── At least one real USB stick found ──────────────────────────────────
    if [[ -n "$candidates" ]]; then
        _pick_from_candidates "$candidates"
        return
    fi

    # ── Nothing found — show disk list and let user act ────────────────────
    $SKIP_CONFIRM && fatal "No USB stick found and --yes is set — plug in a USB stick and re-run."

    echo ""
    warn "No USB stick detected automatically."
    echo "  (Disk images and internal/synthesized disks are excluded.)"
    echo ""
    echo "  Plug in a USB stick (≤ ${MAX_SIZE_GB} GB), then press Enter to re-scan."
    echo "  Or type the disk identifier directly if you can already see it in the list below."
    echo ""
    show_all_disks
    echo ""
    show_usb_example
    echo ""

    while true; do
        read -rp "  Press Enter to re-scan, or type identifier (e.g. disk2): " SELECTED_DISK

        if [[ -z "$SELECTED_DISK" ]]; then
            echo ""
            info "Re-scanning..."
            if [[ "$OS" == macos ]]; then
                candidates=$(list_usb_candidates_macos)
            else
                candidates=$(list_usb_candidates_linux)
            fi

            if [[ -n "$candidates" ]]; then
                _pick_from_candidates "$candidates"
                return
            fi

            echo ""
            show_all_disks
            echo ""
            warn "Still nothing detected automatically."
            echo "  If your stick IS in the list above, type its identifier (e.g. disk2)."
            echo "  Otherwise unplug, wait 5 s, replug, then press Enter again."
            echo ""
        else
            # Strip any /dev/ prefix the user may have typed
            SELECTED_DISK="${SELECTED_DISK#/dev/}"
            # Run safety guard — if it fails, loop back for another attempt
            if ! is_safe_usb "$SELECTED_DISK"; then
                echo ""
                warn "That disk was rejected — choose a different identifier."
                echo ""
                show_all_disks
                echo ""
                show_usb_example
                echo ""
                SELECTED_DISK=""
                continue
            fi
            return
        fi
    done
}

# Helper: pick from a non-empty candidates string (tab-separated lines).
_pick_from_candidates() {
    local candidates="$1"
    echo ""
    echo -e "  ${BOLD}Removable USB disks found:${RESET}"
    echo ""
    local i=1
    local -a ids=()
    while IFS=$'\t' read -r id size_bytes name; do
        # Run safety guard — skip any candidate that fails
        is_safe_usb "$id" || continue
        local size_gb=$(( ${size_bytes:-0} / 1024 / 1024 / 1024 ))
        printf "  [%d]  /dev/%-8s  %4d GB  %s\n" "$i" "$id" "$size_gb" "$name"
        ids+=("$id")
        (( i++ ))
    done <<< "$candidates"
    echo ""

    if [[ ${#ids[@]} -eq 0 ]]; then
        warn "All detected disks were blocked by safety checks."
        warn "Plug in a USB stick (≤ ${MAX_SIZE_GB} GB, external, physical) and re-run."
        SELECTED_DISK=""; return
    fi

    if [[ ${#ids[@]} -eq 1 ]]; then
        info "Only one removable disk — pre-selecting: ${ids[0]}"
        SELECTED_DISK="${ids[0]}"; return
    fi

    $SKIP_CONFIRM && fatal "Multiple disks found and --yes is set — re-run without --yes to choose interactively."

    local choice
    while true; do
        read -rp "  Select disk number [1-${#ids[@]}]: " choice
        if [[ "$choice" =~ ^[0-9]+$ ]] && (( choice >= 1 && choice <= ${#ids[@]} )); then
            SELECTED_DISK="${ids[$((choice-1))]}"; return
        fi
        warn "Enter a number between 1 and ${#ids[@]}."
    done
}

# ---------------------------------------------------------------------------
# Safety checks
# ---------------------------------------------------------------------------
check_disk_safety() {
    local disk="$1" dev="/dev/$1"

    [[ -b "$dev" ]] || fatal "Device not found: $dev — is the USB stick still plugged in?"

    # [1] Not the boot disk
    if [[ "$OS" == macos ]]; then
        local boot
        boot=$(diskutil info / 2>/dev/null | awk '/Part of Whole/{print $NF}')
        [[ "$disk" == "$boot" ]] && fatal "SAFETY [1]: $disk is the system boot disk. Refusing."
    else
        local root_dev
        root_dev=$(df / | awk 'NR==2{gsub(/[0-9]+$/,"",$1); print $1}')
        [[ "$dev" == "$root_dev" ]] && fatal "SAFETY [1]: $dev is the root filesystem. Refusing."
        if mountpoint -q /boot 2>/dev/null; then
            local bd
            bd=$(df /boot | awk 'NR==2{gsub(/[0-9]+$/,"",$1); print $1}')
            [[ "$dev" == "$bd" ]] && fatal "SAFETY [1]: $dev contains /boot. Refusing."
        fi
    fi
    ok "Not the system disk"

    # [2] Size ≤ MAX_SIZE_GB
    local sb=0
    if [[ "$OS" == macos ]]; then
        sb=$(diskutil info "$disk" 2>/dev/null \
            | grep "Disk Size" | grep -oE '\([0-9]+ Bytes\)' | grep -oE '[0-9]+' | head -1)
    else
        sb=$(lsblk -b -o SIZE -n "$dev" 2>/dev/null | head -1 | tr -d ' ')
    fi
    local sg=$(( ${sb:-0} / 1024 / 1024 / 1024 ))
    (( sg > MAX_SIZE_GB )) && \
        fatal "SAFETY [2]: $disk is ${sg} GB — larger than ${MAX_SIZE_GB} GB limit."
    ok "Disk size ${sg} GB — within ${MAX_SIZE_GB} GB limit"
}

# ---------------------------------------------------------------------------
# Confirmation prompt
# ---------------------------------------------------------------------------
confirm_destructive() {
    local disk="$1"
    echo ""
    hr
    echo -e "  ${RED}${BOLD}!! DESTRUCTIVE ACTION — READ CAREFULLY !!${RESET}"
    hr
    echo ""
    echo -e "  Disk : /dev/$disk"
    if [[ "$OS" == macos ]]; then
        diskutil info "$disk" 2>/dev/null \
            | awk '/Media Name|Disk Size|Volume Name/' | sed 's/^/  /'
    else
        lsblk "/dev/$disk" 2>/dev/null | sed 's/^/  /' || true
    fi
    echo ""
    echo "  ALL DATA on this disk will be permanently erased."
    echo "  It will be formatted FAT32 with label: $VOLUME_LABEL"
    echo ""
    if $SKIP_CONFIRM; then
        warn "--yes flag set: skipping confirmation."; return
    fi
    local ans
    read -rp "  Type YES (capitals) to continue, anything else to abort: " ans
    echo ""
    [[ "$ans" == "YES" ]] || fatal "Aborted."
}

# ---------------------------------------------------------------------------
# Format FAT32
# ---------------------------------------------------------------------------
format_fat32() {
    local disk="$1"
    step "Formatting /dev/$disk as FAT32  (label: $VOLUME_LABEL)"
    if $DRY_RUN; then
        [[ "$OS" == macos ]] \
            && dr "diskutil eraseDisk FAT32 $VOLUME_LABEL MBRFormat /dev/$disk" \
            || dr "wipefs + parted + mkfs.vfat -F 32 /dev/${disk}1"
        return
    fi
    if [[ "$OS" == macos ]]; then
        diskutil eraseDisk FAT32 "$VOLUME_LABEL" MBRFormat "/dev/$disk" \
            || fatal "diskutil eraseDisk failed."
        local mp="/Volumes/$VOLUME_LABEL" t=0
        while [[ ! -d "$mp" ]] && (( t < 15 )); do sleep 1; (( t++ )); done
        [[ -d "$mp" ]] || fatal "Volume $mp did not mount."
        ok "Mounted at $mp"
    else
        wipefs -a "/dev/$disk" &>/dev/null || true
        parted -s "/dev/$disk" mklabel msdos || fatal "parted mklabel failed"
        parted -s "/dev/$disk" mkpart primary fat32 1MiB 100% || fatal "parted mkpart failed"
        partprobe "/dev/$disk" 2>/dev/null || true; sleep 2
        local part="/dev/${disk}1"
        [[ -b "$part" ]] || fatal "Partition $part not found."
        mkfs.vfat -F 32 -n "$VOLUME_LABEL" "$part" || fatal "mkfs.vfat failed"
        ok "Formatted $part as FAT32"
    fi
}

# ---------------------------------------------------------------------------
# Mount point
# ---------------------------------------------------------------------------
get_mount_point() {
    local disk="$1"
    if [[ "$OS" == macos ]]; then echo "/Volumes/$VOLUME_LABEL"; return; fi
    local part="/dev/${disk}1"
    local mp
    mp=$(lsblk -o MOUNTPOINT -n "$part" 2>/dev/null | tr -d ' ')
    if [[ -n "$mp" ]]; then echo "$mp"; return; fi
    local tmp="/mnt/boseflash_$$"
    mkdir -p "$tmp"
    mount "$part" "$tmp" || fatal "Could not mount $part"
    echo "$tmp"
}

validate_usb_format() {
    local disk="$1" mp="$2"
    step "Validating USB format"
    if $DRY_RUN; then dr "confirm MBR/FAT32/label/root files on $mp"; return; fi
    if [[ "$OS" == macos ]]; then
        local disk_info volume_info
        disk_info=$(diskutil info "$disk" 2>/dev/null)
        volume_info=$(diskutil info "$mp" 2>/dev/null)
        echo "$disk_info" | grep -qiE 'Partition Map Scheme:[[:space:]]+Master Boot Record|FDisk_partition_scheme|MBR' \
            || fatal "USB partition map is not MBR/FDisk. Bose units are most reliable with MBR + FAT32."
        echo "$volume_info" | grep -qiE 'File System Personality:[[:space:]]+MS-DOS FAT32|Type \(Bundle\):[[:space:]]+msdos' \
            || fatal "USB filesystem is not FAT32/MS-DOS."
        echo "$volume_info" | grep -q "Volume Name:.*$VOLUME_LABEL" \
            || warn "USB label is not reported as $VOLUME_LABEL; continuing because mount point is $mp."
    else
        local part="/dev/${disk}1"
        local fstype label pttype
        fstype=$(lsblk -no FSTYPE "$part" 2>/dev/null | tr -d ' ')
        label=$(lsblk -no LABEL "$part" 2>/dev/null | tr -d ' ')
        pttype=$(lsblk -no PTTYPE "/dev/$disk" 2>/dev/null | tr -d ' ')
        [[ "$pttype" == dos ]] || fatal "USB partition table is not MBR/dos."
        [[ "$fstype" == vfat ]] || fatal "USB filesystem is not FAT32/vfat."
        [[ "$label" == "$VOLUME_LABEL" ]] || warn "USB label is '$label', expected '$VOLUME_LABEL'."
    fi
    ok "USB format looks Bose-compatible: MBR + FAT32"
}

# ---------------------------------------------------------------------------
# macOS junk file removal  [safety check 5]
# ---------------------------------------------------------------------------
clean_macos_junk() {
    local mp="$1"
    step "Removing macOS junk files  [safety check 5]"
    if $DRY_RUN; then
        dr "mdutil -i off + rm .fseventsd .Spotlight-V100 .DS_Store .Trashes ._*"; return
    fi
    mdutil -i off "$mp" 2>/dev/null || true
    rm -rf "$mp/.fseventsd" "$mp/.Spotlight-V100" 2>/dev/null || true
    rm -f  "$mp/.DS_Store"  "$mp/.Trashes"        2>/dev/null || true
    find "$mp" -maxdepth 2 -name '._*'       -delete 2>/dev/null || true
    find "$mp" -maxdepth 2 -name '.DS_Store' -delete 2>/dev/null || true
    ok "Junk files removed"
}

# ---------------------------------------------------------------------------
# Download firmware
# ---------------------------------------------------------------------------
download_firmware() {
    local dest_dir="$1"
    local zip="${LATEST_ZIP}"
    [[ -n "$REQUESTED_VERSION" ]] && zip=$(resolve_firmware_zip "$REQUESTED_VERSION")
    local url="$ARCHIVE_BASE/$zip" dest="$dest_dir/$zip"
    step "Downloading firmware" >&2
    info "File   : $zip" >&2
    info "Source : $url" >&2
    if $DRY_RUN; then dr "curl -L --progress-bar --retry 3 -C - -o \"$dest\" \"$url\"" >&2; echo "$dest"; return; fi
    curl -L --progress-bar --retry 3 --retry-delay 5 -C - -o "$dest" "$url" \
        || fatal "Download failed. Check internet connection."
    [[ -f "$dest" ]] || fatal "Download produced no file."
    ok "Downloaded: $(du -sh "$dest" | cut -f1)" >&2
    echo "$dest"
}

# ---------------------------------------------------------------------------
# Extract Update.stu
# ---------------------------------------------------------------------------
extract_update_stu() {
    local zip="$1" dest_dir="$2" out="$2/$FIRMWARE_FILENAME"
    step "Extracting $FIRMWARE_FILENAME" >&2
    info "Zip: $zip" >&2
    if $DRY_RUN; then dr "unzip -j \"$zip\" \"*$FIRMWARE_FILENAME\" -d \"$dest_dir\"" >&2; echo "$out"; return; fi
    validate_zip_archive "$zip"
    unzip -j -o "$zip" "*$FIRMWARE_FILENAME" -d "$dest_dir" >/dev/null 2>&1 \
        || unzip -j -o "$zip" "$FIRMWARE_FILENAME" -d "$dest_dir" >/dev/null 2>&1 || true
    if [[ ! -f "$out" ]]; then
        warn "Contents of zip:" >&2; unzip -l "$zip" | head -20 >&2
        fatal "$FIRMWARE_FILENAME not found in zip — file may be corrupted. Re-download and retry."
    fi
    validate_update_stu_file "$out" "Extracted $FIRMWARE_FILENAME"
    ok "Extracted $FIRMWARE_FILENAME ($(du -sh "$out" | cut -f1))" >&2
    echo "$out"
}

# ---------------------------------------------------------------------------
# Write firmware  [safety check 4: MD5 verify]
# ---------------------------------------------------------------------------
write_firmware() {
    local src="$1" mp="$2" dst="$2/$FIRMWARE_FILENAME"
    step "Writing $FIRMWARE_FILENAME to USB"
    if $DRY_RUN; then dr "cp + md5 verify \"$src\" → \"$dst\""; return; fi
    validate_update_stu_file "$src" "Source $FIRMWARE_FILENAME"
    cp "$src" "$dst" || fatal "cp failed — disk may be full or read-only."
    sync
    local s d
    if   command -v md5sum &>/dev/null; then s=$(md5sum "$src"|cut -d' ' -f1); d=$(md5sum "$dst"|cut -d' ' -f1)
    elif command -v md5     &>/dev/null; then s=$(md5 -q "$src"); d=$(md5 -q "$dst")
    else warn "md5 not available — skipping checksum."; s=skip; d=skip; fi
    [[ "$s" == "$d" ]] || fatal "SAFETY [4]: Checksum mismatch after copy. Disk may be faulty."
    validate_update_stu_file "$dst" "USB $FIRMWARE_FILENAME"
    ok "$FIRMWARE_FILENAME written and verified  (md5: $d)"
}

# ---------------------------------------------------------------------------
# Write remote_services (SSH trigger)
# ---------------------------------------------------------------------------
write_ssh_trigger() {
    local mp="$1" dst="$1/$SSH_TRIGGER_FILE"
    step "Writing $SSH_TRIGGER_FILE  (enables passwordless root SSH)"
    if $DRY_RUN; then dr "touch \"$dst\""; return; fi
    touch "$dst" || fatal "Failed to create $SSH_TRIGGER_FILE"
    ok "$SSH_TRIGGER_FILE written  (empty file — firmware reads its presence only)"
}

# ---------------------------------------------------------------------------
# Verify USB contents  [safety check 6]
# ---------------------------------------------------------------------------
verify_usb_contents() {
    local mp="$1"
    step "Verifying USB contents  [safety check 6]"
    if $DRY_RUN; then
        dr "ls -lah $mp  (expect: ${FIRMWARE_FILENAME}${MODE_SSH:+ $SSH_TRIGGER_FILE})"; return
    fi
    rm -rf "$mp/.Spotlight-V100" "$mp/.fseventsd" 2>/dev/null || true
    find "$mp" -maxdepth 2 -name '._*'       -delete 2>/dev/null || true
    find "$mp" -maxdepth 2 -name '.DS_Store' -delete 2>/dev/null || true
    echo ""
    local junk=false
    while IFS= read -r f; do
        [[ "$f" == "$mp" ]] && continue
        local fn; fn=$(basename "$f")
        case "$fn" in
            .Spotlight-V100|.fseventsd)
                continue
                ;;
        esac
        if [[ "$fn" == .* ]]; then warn "  [JUNK] $fn"; junk=true
        else ok "  [FILE] $fn"; fi
    done < <(find "$mp" -maxdepth 1 | sort)
    echo ""
    $junk && warn "Hidden/junk files remain — they may prevent the pedestal detecting the firmware."
    $MODE_FLASH && [[ ! -f "$mp/$FIRMWARE_FILENAME" ]] && fatal "$FIRMWARE_FILENAME missing from USB."
    $MODE_SSH   && [[ ! -f "$mp/$SSH_TRIGGER_FILE"  ]] && fatal "$SSH_TRIGGER_FILE missing from USB."
    $MODE_FLASH && validate_update_stu_file "$mp/$FIRMWARE_FILENAME" "USB $FIRMWARE_FILENAME"
    local unexpected=false fn
    while IFS= read -r f; do
        fn=$(basename "$f")
        case "$fn" in
            "$FIRMWARE_FILENAME"|"$SSH_TRIGGER_FILE"|.Spotlight-V100|.fseventsd) ;;
            *) warn "  [EXTRA] $fn"; unexpected=true ;;
        esac
    done < <(find "$mp" -mindepth 1 -maxdepth 1 ! -name '.*' | sort)
    $unexpected && warn "Extra visible files are present. Bose usually expects only ${FIRMWARE_FILENAME}${MODE_SSH:+ and $SSH_TRIGGER_FILE} at USB root."
    sync
    ok "USB verification passed"
}

# ---------------------------------------------------------------------------
# Eject
# ---------------------------------------------------------------------------
eject_disk() {
    local disk="$1"
    step "Ejecting /dev/$disk safely"
    if $DRY_RUN; then dr "eject /dev/$disk"; return; fi
    if [[ "$OS" == macos ]]; then
        diskutil eject "/dev/$disk" && ok "Ejected."
    else
        local mp
        mp=$(lsblk -o MOUNTPOINT -n "/dev/${disk}1" 2>/dev/null | tr -d ' ') || mp=""
        [[ -n "$mp" ]] && umount "$mp" && ok "Unmounted $mp"
        [[ "$mp" == /mnt/boseflash_* ]] && rmdir "$mp" 2>/dev/null || true
        sync; ok "Sync complete."
    fi
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
main() {
    # Validate local firmware path early, before any disk I/O
    local stu_path="" zip_path="" tmp_dir=""
    if $MODE_FLASH && [[ -n "$LOCAL_FIRMWARE" ]]; then
        [[ -f "$LOCAL_FIRMWARE" ]] || fatal "Local firmware file not found: $LOCAL_FIRMWARE"
        case "${LOCAL_FIRMWARE##*.}" in
            stu) stu_path="$LOCAL_FIRMWARE"; validate_update_stu_file "$stu_path" "Local $FIRMWARE_FILENAME"; info "Using local Update.stu: $stu_path" ;;
            zip) zip_path="$LOCAL_FIRMWARE"; validate_zip_archive "$zip_path"; info "Using local zip: $zip_path" ;;
            *)   fatal "Unrecognised file type: $LOCAL_FIRMWARE  (expected .stu or .zip)" ;;
        esac
    fi

    # Select disk (sets global SELECTED_DISK — not called via $())
    select_disk
    local disk="$SELECTED_DISK"
    [[ -n "$disk" ]] || fatal "No disk selected — aborting."
    info "Selected: /dev/$disk"

    # Safety checks
    step "Running safety checks on /dev/$disk"
    check_disk_safety "$disk"

    # Confirm destructive action
    confirm_destructive "$disk"

    # Format
    format_fat32 "$disk"

    # Mount point
    local mp
    if $DRY_RUN; then
        mp="/tmp/boseflash_dryrun"; mkdir -p "$mp"
        info "(dry-run) simulated mount: $mp"
    else
        mp=$(get_mount_point "$disk")
        info "Mount point: $mp"
    fi

    # Confirm the resulting USB layout is what older Bose firmware expects.
    validate_usb_format "$disk" "$mp"

    # Clean junk
    clean_macos_junk "$mp"

    # Flash
    if $MODE_FLASH; then
        if [[ -z "$stu_path" && -z "$zip_path" ]]; then
            tmp_dir=$(mktemp -d)
            zip_path=$(download_firmware "$tmp_dir")
        fi
        if [[ -z "$stu_path" ]]; then
            tmp_dir="${tmp_dir:-$(mktemp -d)}"
            stu_path=$(extract_update_stu "$zip_path" "$tmp_dir")
        fi
        write_firmware "$stu_path" "$mp"
    fi

    # SSH
    $MODE_SSH && write_ssh_trigger "$mp"

    # Clean junk a second time — macOS creates ._* and .Spotlight-V100 when
    # writing files to a FAT32 volume, so we must clean after all writes.
    clean_macos_junk "$mp"

    # Verify
    verify_usb_contents "$mp"

    # Eject
    eject_disk "$disk"

    # Cleanup
    [[ -n "$tmp_dir" && -d "$tmp_dir" ]] && rm -rf "$tmp_dir"

    # Done
    echo ""
    hr
    echo -e "${GREEN}${BOLD}  USB stick is ready.${RESET}"
    hr
    echo ""

    if $MODE_FLASH; then
        echo -e "${BOLD}Next steps — firmware flash:${RESET}"
        echo "  1. Make sure the pedestal is powered on."
        echo "  2. Insert the USB stick into the USB-A port on the BACK of the pedestal."
        echo "  3. Wait up to 30 seconds for the pedestal to detect the file."
        echo "     If nothing happens: pull the AC cord, plug back in WITH the USB inserted."
        echo "  4. Do NOT unplug power or USB during the update (2–5 minutes)."
        echo "  5. The pedestal reboots automatically when done. Remove the USB stick."
        echo "  6. Hold the Control button (~3 s) → solid amber → WiFi setup mode."
        echo "  7. Join 'Bose Wave ST (…)' WiFi on your phone, open BosMan, enter home WiFi."
        echo ""
    fi

    if $MODE_SSH; then
        echo -e "${BOLD}Next steps — SSH enable (SoundCork):${RESET}"
        echo "  1. Power the pedestal OFF — pull the AC cord completely."
        echo "  2. Insert the USB stick into the USB-A port on the back."
        echo "  3. Plug the AC cord back in and wait ~60 seconds."
        echo "  4. SSH in (no password):  ssh root@<speaker-ip>"
        echo "  5. Make SSH persistent:   ssh root@<speaker-ip> 'touch /mnt/nv/remote_services'"
        echo "  6. Follow README.flash.md §5 for the full SoundCork setup."
        echo ""
    fi
}

main "$@"
