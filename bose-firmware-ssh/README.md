# Bose Firmware SSH Research (Wave SoundTouch IV)

Fork of [`bose-usb-flash/`](../bose-usb-flash/) for **reverse-engineering `Update.stu`** and exploring a rebuilt firmware image with **persistent SSH**.

The parent folder handles proven USB recovery (`Update.stu` flash + `remote_services` stick). This folder is the **research track**: analyze, patch, repack, and eventually deliver custom firmware via USB or redirected `swUpdateUrl`.

## Status (2026-06)

| Milestone | State |
|-----------|--------|
| Download & fingerprint 27.00.06 `Update.stu` | Done |
| Reverse-engineer the `.stu` container (9 sections, CRC32 map) | **Done** — see [`docs/STU-FORMAT.md`](docs/STU-FORMAT.md) |
| Extract rootfs (UBI) cleanly | **Done** — `ubi.img` section -> `ubi_reader` |
| Locate the SSH gate | **Done** — `/usr/bin/remote_services_enabled` (180-byte shell script) |
| Build patched flashable `.stu` (macOS-native, in-place) | **Done** — `scripts/inplace-patch-stu.py` (123 bytes changed) |
| Full unpack/edit/rebuild loop (Linux toolchain) | **Done** — `scripts/stu-toolbox.sh` (colima) |
| USB flash validation on hardware | Pending (needs sacrificial unit) |
| OTA via `swUpdateUrl` | Optional / not started |

## Quick start

```sh
cd bose-firmware-ssh

# 1. Download firmware (~96 MB) into work/
./scripts/fetch-firmware.sh 27.00.06

# 2. Parse + verify + carve all 9 container sections
python3 scripts/extract-stu.py work/Update.stu -o work/sections
```

### A. SSH patch the easy way (macOS-native, no Linux tools)

Surgically flips `remote_services_enabled` to `exit 0` inside the UBIFS image
without rebuilding it (changes only 123 bytes; fixes the two affected CRC32s):

```sh
python3 -m venv /tmp/ubienv && /tmp/ubienv/bin/pip install ubi_reader
/tmp/ubienv/bin/python scripts/inplace-patch-stu.py work/Update.stu -o work/Update-ssh.stu
python3 scripts/extract-stu.py work/Update-ssh.stu --no-carve   # all CRCs OK
```

### B. Arbitrary rootfs edits (Linux toolchain via colima)

For bigger changes (extra files, dropbear keys, init tweaks) use the containerized
mtd-utils loop. Needs a Docker-compatible runtime; `stu-toolbox.sh` will manage
colima for you (`brew install colima docker`):

```sh
./scripts/stu-toolbox.sh up                                   # start runtime + build image
./scripts/stu-toolbox.sh unpack  work/Update.stu work/rootfs  # extract (root, perms kept)
#   ... edit work/rootfs ...
./scripts/stu-toolbox.sh rebuild work/rootfs work/Update-custom.stu   # rebuild + repack + verify
```

### C. SSH + your own update server, one shot (recommended)

`build-ssh` runs unpack -> patch -> rebuild for you. It (a) enables sshd +
telnetd permanently, (b) installs a real root credential, and (c) rewrites the
firmware's update **index URL** so future updates come from your LAN instead of
`worldwide.bose.com`:

```sh
./scripts/stu-toolbox.sh up
./scripts/stu-toolbox.sh build-ssh work/Update.stu work/Update-ssh.stu \
    --authorized-key-file ~/.ssh/id_ed25519.pub \
    --swupdate-url http://192.168.0.80:18000/updates/soundtouch
```

Auth modes (pick one; required so we never ship an open root sshd):

| Flag | Effect |
|------|--------|
| `--authorized-key-file <pub>` / `--authorized-key "<str>"` | key-based root login (recommended) |
| `--root-password "<pw>"` | sets a SHA-512 root password |
| `--allow-empty-password` | empty-password root login (LAN-only, insecure) |

Other flags: `--swupdate-url URL` (omit `/index.xml`), `--no-ssh` (URL change
only). The same patches can be applied to an already-unpacked tree with
`./scripts/stu-toolbox.sh patch work/rootfs <opts>`.

Why this is the robust fix: the **stock** firmware's `swUpdateCheck` reads its
index URL from `/opt/Bose/etc/SoundTouchSdkPrivateCfg.xml` (default
`https://worldwide.bose.com/...`) and that value is **not** overridable at
runtime via the port-17000 CLI (`sys configuration` writes a different PDO).
Baking the URL into the rootfs is the only reliable redirect, and it lets you
use plain `http://` on your LAN (no TLS cert needed).

> **OTA install caveat:** the device only *installs* an update whose index
> `REVISION` is **higher** than the running version. Serve the patched `.stu`
> from `scripts/ota-update-server.py` with a bumped `INDEX REVISION`, otherwise
> the device treats it as "already up to date" and skips it. (To get SSH the
> first time without any of this, the USB `remote_services` marker --
> `scripts/enable-ssh-usb.sh` -- is still the simplest path.)


## Layout

| Path | Purpose |
|------|---------|
| [`RESEARCH.md`](RESEARCH.md) | Findings summary and references |
| [`docs/STU-FORMAT.md`](docs/STU-FORMAT.md) | `Update.stu` container spec (complete) |
| [`docs/PATCH-PLAN.md`](docs/PATCH-PLAN.md) | Proposed patch strategies & phases |
| [`scripts/stu_container.py`](scripts/stu_container.py) | Shared `.stu` container parser |
| [`scripts/extract-stu.py`](scripts/extract-stu.py) | Parse + verify CRCs + carve sections |
| [`scripts/inplace-patch-stu.py`](scripts/inplace-patch-stu.py) | macOS-native in-place SSH patch (no mtd-utils) |
| [`scripts/stu-toolbox.sh`](scripts/stu-toolbox.sh) | colima/Docker loop: unpack / patch / build-ssh / rebuild |
| [`scripts/patch-rootfs.sh`](scripts/patch-rootfs.sh) | In-container rootfs patcher: SSH enable+auth, swUpdateUrl |
| [`scripts/enable-ssh-usb.sh`](scripts/enable-ssh-usb.sh) | Prep a USB `remote_services` stick (no-flash SSH enable) |
| [`scripts/ota-update-server.py`](scripts/ota-update-server.py) | Local update server (index.xml + .stu) |
| [`scripts/ota-deploy.sh`](scripts/ota-deploy.sh) | OTA push helper (see its blocker banner) |
| [`scripts/rebuild-ubi.sh`](scripts/rebuild-ubi.sh) | `mkfs.ubifs`/`ubinize` wrapper (runs in container) |
| [`scripts/repack-stu.py`](scripts/repack-stu.py) | Splice rebuilt UBI + fix section CRC32 |
| [`scripts/mtdtools.Dockerfile`](scripts/mtdtools.Dockerfile) | Linux image: mtd-utils + ubi_reader |
| [`scripts/analyze-stu.py`](scripts/analyze-stu.py) | Firmware string/marker analyzer |
| [`scripts/patch-research.py`](scripts/patch-research.py) | Hexdump scout (legacy) |
| [`scripts/fetch-firmware.sh`](scripts/fetch-firmware.sh) | Download from Internet Archive |
| [`bose-usb-prep.sh`](bose-usb-prep.sh) | USB flash / SSH stick (from parent) |
| [`work/`](work/) | Downloaded firmware (gitignored) |

## Goal

Bake **persistent SSH** into firmware so owners do not depend on:

- USB `remote_services` power-cycle (fails on some Wave IV units)
- Ethernet reachability during SSH enable
- SERVICE serial cable

## Realistic paths (short)

1. **Phase A — Binary patch in place** inside the container: force `remote_services_enabled` true or auto-create `/mnt/nv/remote_services` at boot.
2. **Phase B — Unpack/repack** the `SoftwareUpdateInstaller` container once format is RE'd.
3. **Phase C — Deliver** via USB (`bose-usb-prep.sh --firmware`) or OTA (`swUpdateUrl` + local update server).

## Safety

- Keep a copy of stock `Update.stu` (MD5 `88c63e440cafa969ff19fb98b39be24a` for 27.00.06 WST4).
- Test only on hardware you own. Bad repacks can brick the pedestal.
- USB recovery with stock firmware remains the rollback path.

## References

- Parent USB guide: [`README.flash.md`](README.flash.md)
- SoundCork telnet migration: [gesellix TELNET-MIGRATION-METHOD](https://github.com/gesellix/Bose-SoundTouch/blob/main/docs/content/docs/analysis/TELNET-MIGRATION-METHOD.md)
- USB SSH trigger (`mount.sh`): [Technological Misadventures](https://technologicalmisadventures.wordpress.com/2018/12/05/soundtouch-20iii-ssh-telnet-etc/)
- Firmware archive: [Internet Archive WST4](https://archive.org/download/bose-soundtouch-software-and-firmware/Firmware/2015-2020_Bluetooth/Bluetooth_Wave_SoundTouch_IV/)