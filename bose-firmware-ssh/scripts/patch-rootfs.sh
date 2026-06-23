#!/bin/sh
# patch-rootfs.sh -- apply the "SSH + custom update server" patches to an
# UNPACKED Bose SoundTouch rootfs, in place.
#
# Designed to run INSIDE the mtdtools container (as root) so file owners and
# permissions are preserved for the subsequent UBIFS rebuild. Driven by
# scripts/stu-toolbox.sh (`patch` / `build-ssh` subcommands), but can also be
# run standalone in any Linux environment.
#
# It performs up to three independent changes:
#
#   (a) ENABLE SSH/TELNET PERMANENTLY
#       - /usr/bin/remote_services_enabled  ->  `exit 0` stub
#       - touch /etc/remote_services          (marker, belt-and-suspenders)
#       so the rc5.d S50sshd / S10telnetd init scripts start the daemons on
#       every boot.
#
#   (b) MAKE ROOT LOGIN ACTUALLY WORK (stock root password is empty, which
#       OpenSSH refuses). Pick ONE auth mode:
#         --authorized-key "ssh-ed25519 AAAA..."   (recommended; key auth)
#         --authorized-key-file <path>             (same, from a file)
#         --root-password "<pw>"                   (sets a SHA-512 hash)
#         --allow-empty-password                   (PermitEmptyPasswords yes)
#       If none is given the script refuses, to avoid shipping an
#       unauthenticated sshd. sshd_config is updated to permit the chosen mode.
#
#   (c) REDIRECT THE FIRMWARE UPDATE INDEX URL
#         --swupdate-url "http://192.168.0.80:18000/updates/soundtouch"
#       rewrites /opt/Bose/etc/SoundTouchSdkPrivateCfg.xml <swUpdateUrl>. This
#       is the value the device reports as `indexFileUrl` from
#       GET :8090/swUpdateCheck -- i.e. where it looks for <url>/index.xml.
#       With this baked in you can host updates on your own LAN (http is fine).
#
# Usage:
#   patch-rootfs.sh <rootfs_dir> [options]
#
# Options:
#   --swupdate-url URL          Set update index base URL (no trailing /index.xml)
#   --authorized-key STR        Add an SSH public key to root's authorized_keys
#   --authorized-key-file FILE  Read the public key from FILE
#   --root-password PW          Set root's password (SHA-512 crypt)
#   --allow-empty-password      Permit empty-password root login (insecure)
#   --no-ssh                    Skip the SSH-enable changes (URL change only)
#   --help
#
# Examples:
#   patch-rootfs.sh work/rootfs \
#       --authorized-key-file ~/.ssh/id_ed25519.pub \
#       --swupdate-url http://192.168.0.80:18000/updates/soundtouch
set -eu

ROOTFS=""
SWUPDATE_URL=""
AUTH_KEY=""
AUTH_KEY_FILE=""
ROOT_PW=""
ALLOW_EMPTY=0
DO_SSH=1

log()  { printf '[patch-rootfs] %s\n' "$*" >&2; }
die()  { printf '[patch-rootfs] ERROR: %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --swupdate-url)        SWUPDATE_URL="$2"; shift 2 ;;
        --authorized-key)      AUTH_KEY="$2"; shift 2 ;;
        --authorized-key-file) AUTH_KEY_FILE="$2"; shift 2 ;;
        --root-password)       ROOT_PW="$2"; shift 2 ;;
        --allow-empty-password) ALLOW_EMPTY=1; shift ;;
        --no-ssh)              DO_SSH=0; shift ;;
        -h|--help)
            sed -n '2,200p' "$0" | sed 's/^# \{0,1\}//' | sed '/^[^ ].*() {/q' | head -60
            exit 0 ;;
        -*)  die "unknown option: $1" ;;
        *)   [ -z "$ROOTFS" ] && ROOTFS="$1" || die "unexpected arg: $1"; shift ;;
    esac
done

[ -n "$ROOTFS" ] || die "usage: $0 <rootfs_dir> [options]"
[ -d "$ROOTFS" ] || die "no such rootfs dir: $ROOTFS"
[ -e "$ROOTFS/etc/passwd" ] || die "'$ROOTFS' does not look like a rootfs (no etc/passwd)"

# --- (a)+(b) SSH ------------------------------------------------------------
if [ "$DO_SSH" -eq 1 ]; then
    # Require an auth mode so we never ship an unauthenticated root sshd.
    if [ -z "$AUTH_KEY" ] && [ -z "$AUTH_KEY_FILE" ] && [ -z "$ROOT_PW" ] && [ "$ALLOW_EMPTY" -eq 0 ]; then
        die "SSH enable needs an auth mode: --authorized-key(-file), --root-password, or --allow-empty-password (or pass --no-ssh)"
    fi

    log "enabling remote services (sshd + telnetd) permanently"
    gate="$ROOTFS/usr/bin/remote_services_enabled"
    cat > "$gate" <<'EOF'
#!/bin/sh
# PATCHED: remote services always enabled (persistent SSH/telnet).
# Original logic checked for marker files in /etc /mnt/nv /tmp.
exit 0
EOF
    chmod 0755 "$gate"
    : > "$ROOTFS/etc/remote_services"   # marker, in case the gate is restored

    # Auth: authorized_keys
    keymat=""
    if [ -n "$AUTH_KEY_FILE" ]; then
        [ -f "$AUTH_KEY_FILE" ] || die "key file not found: $AUTH_KEY_FILE"
        keymat="$(cat "$AUTH_KEY_FILE")"
    fi
    if [ -n "$AUTH_KEY" ]; then
        keymat="$keymat
$AUTH_KEY"
    fi
    if [ -n "$keymat" ]; then
        # root home is /home/root per /etc/passwd
        rhome="$ROOTFS/home/root"
        mkdir -p "$rhome/.ssh"
        printf '%s\n' "$keymat" | sed '/^$/d' >> "$rhome/.ssh/authorized_keys"
        chmod 0700 "$rhome/.ssh"
        chmod 0600 "$rhome/.ssh/authorized_keys"
        chown -R 0:0 "$rhome/.ssh" 2>/dev/null || true
        log "installed authorized_keys for root ($(grep -c . "$rhome/.ssh/authorized_keys" 2>/dev/null || echo 0) key(s))"
    fi

    # Auth: root password
    if [ -n "$ROOT_PW" ]; then
        hash="$(python3 -c 'import crypt,sys; print(crypt.crypt(sys.argv[1], crypt.mksalt(crypt.METHOD_SHA512)))' "$ROOT_PW")"
        python3 - "$ROOTFS/etc/shadow" "$hash" <<'PYEOF'
import sys
path, h = sys.argv[1], sys.argv[2]
lines = open(path).read().splitlines()
out = []
for ln in lines:
    f = ln.split(':')
    if f and f[0] == 'root':
        f[1] = h
        ln = ':'.join(f)
    out.append(ln)
open(path, 'w').write('\n'.join(out) + '\n')
PYEOF
        log "set root password hash in etc/shadow"
    fi

    # sshd_config: make the chosen mode explicit. Stock firmware ships NO
    # sshd_config (sshd runs on built-in defaults), so create it if absent.
    cfg="$ROOTFS/etc/ssh/sshd_config"
    if [ ! -f "$cfg" ]; then
        mkdir -p "$ROOTFS/etc/ssh"
        : > "$cfg"
        chmod 0644 "$cfg"
        log "no stock sshd_config found -- creating one"
    fi
    # strip any prior managed block, then append a fresh one
    python3 - "$cfg" <<'PYEOF'
import sys
p = sys.argv[1]
txt = open(p).read()
marker = "# >>> bose-firmware-ssh managed >>>"
end = "# <<< bose-firmware-ssh managed <<<"
if marker in txt and end in txt:
    pre = txt.split(marker)[0]
    post = txt.split(end,1)[1]
    txt = pre.rstrip() + "\n" + post.lstrip()
open(p,'w').write(txt.rstrip() + "\n")
PYEOF
    {
        echo "# >>> bose-firmware-ssh managed >>>"
        echo "PermitRootLogin yes"
        if [ -n "$keymat" ]; then
            echo "PubkeyAuthentication yes"
        fi
        if [ -n "$ROOT_PW" ]; then
            echo "PasswordAuthentication yes"
        fi
        if [ "$ALLOW_EMPTY" -eq 1 ]; then
            echo "PasswordAuthentication yes"
            echo "PermitEmptyPasswords yes"
            log "WARNING: empty-password root login enabled -- anyone on the LAN can log in as root"
        fi
        echo "# <<< bose-firmware-ssh managed <<<"
    } >> "$cfg"
    log "updated sshd_config"
fi

# --- (c) update index URL ---------------------------------------------------
if [ -n "$SWUPDATE_URL" ]; then
    cfgxml="$ROOTFS/opt/Bose/etc/SoundTouchSdkPrivateCfg.xml"
    [ -f "$cfgxml" ] || die "missing $cfgxml -- cannot set swUpdateUrl"
    python3 - "$cfgxml" "$SWUPDATE_URL" <<'PYEOF'
import sys, re
path, url = sys.argv[1], sys.argv[2]
txt = open(path).read()
new, n = re.subn(r'(<swUpdateUrl>).*?(</swUpdateUrl>)',
                 lambda m: m.group(1) + url + m.group(2), txt, flags=re.S)
if n == 0:
    sys.exit("no <swUpdateUrl> element found in " + path)
open(path, 'w').write(new)
print("set swUpdateUrl -> %s (%d occurrence)" % (url, n))
PYEOF
    log "redirected firmware update index URL"
fi

log "rootfs patched: $ROOTFS"
