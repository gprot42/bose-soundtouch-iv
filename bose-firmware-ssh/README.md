# Bose Firmware SSH Research (Wave SoundTouch IV)

> **DISCLAIMER — USE AT YOUR OWN RISK**
> Modifying, patching, or reflashing your device's firmware may permanently
> damage it, void your warranty, or leave it in an unrecoverable state.
> The tools and instructions in this repository are provided **as-is, with no
> warranty of any kind**. The authors accept no responsibility for bricked
> hardware, data loss, or any other damage resulting from following these
> instructions. You are solely responsible for any actions you take on your
> own equipment.

Fork of [`bose-usb-flash/`](../bose-usb-flash/) for **analyzing `Update.stu`** and building a firmware image with **persistent SSH**.

The parent folder handles proven USB recovery (`Update.stu` flash + `remote_services` stick). This folder is the **research track**: analyze, patch, repack, and eventually deliver custom firmware via USB or redirected `swUpdateUrl`.

## Status (2026-06)

| Milestone | State |
|-----------|--------|
| Download & fingerprint 27.00.06 `Update.stu` | Done |
| Analyze + document the `.stu` container (9 sections, CRC32 map) | **Done** — see [`docs/STU-FORMAT.md`](docs/STU-FORMAT.md) |
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


### D. Flash the patched firmware via USB stick (recommended install path)

USB flash **bypasses the version check** that blocks OTA — the pedestal
installs whatever `Update.stu` it finds on the stick regardless of version.
This is the primary way to deliver `work/Update-ssh.stu` to the device.

#### Requirements

- USB-A stick, **32 GB or smaller**
- Partition table: **MBR** (GPT will not work)
- Filesystem: **FAT32** (not exFAT, not NTFS)
- Exactly **one file at the USB root**, named `Update.stu` (the pedestal
  looks for this exact name — rename your patched file)

#### Step 1 — Prepare the USB stick

**Automated (recommended):**
```sh
# Format + clean a FAT32/MBR stick in one command (replace diskN):
diskutil eraseDisk FAT32 BOSEFLASH MBRFormat /dev/diskN
```
Or use the parent project's helper which handles format + junk removal:
```sh
../bose-usb-prep.sh --dry-run    # preview
../bose-usb-prep.sh              # formats and preps the stick
```

#### Step 2 — Copy the patched firmware

The pedestal requires the file to be named exactly **`Update.stu`**:

```sh
# Mount point may differ — check `diskutil list` for yours
cp work/Update-ssh.stu /Volumes/BOSEFLASH/Update.stu
```

#### Step 3 — Remove macOS junk files (critical)

macOS silently creates hidden files that prevent the pedestal from reading
the firmware. Remove them **after** copying:

```sh
mdutil -i off /Volumes/BOSEFLASH
rm -rf /Volumes/BOSEFLASH/.fseventsd /Volumes/BOSEFLASH/.Spotlight-V100
rm -f  /Volumes/BOSEFLASH/._* /Volumes/BOSEFLASH/.DS_Store
```

Verify the stick contains exactly one file:

```sh
ls -la /Volumes/BOSEFLASH/
# Expected:  Update.stu   (only visible file)
```

Then eject cleanly:
```sh
diskutil eject /dev/diskN
```

#### Step 4 — Flash the pedestal

The USB firmware port is on the **pedestal** (bottom unit), labelled
**SETUP / SETUP B / SERVICE** — not the network (RJ45) port.

**Method A — auto-flash (try first):**
1. Leave the system **powered on**.
2. Insert the USB stick into the pedestal's **SETUP / SETUP B / SERVICE** jack.
3. The WiFi LED blinks white while reading. The pedestal **reboots on its
   own** (~30 s – 5 min) when done — that is the success signal.
4. Do not press any buttons on the console.

**Method B — Control button (if Method A does nothing after 2 min):**
1. Unplug the AC power cord.
2. Insert the USB stick into **SETUP / SETUP B / SERVICE**.
3. Hold the **Control button** on the back of the pedestal.
4. Reconnect AC power while still holding — hold ~5 s then release.
5. Wait up to 5 minutes for the automatic reboot.

**LED signals:**

| LED / display | Meaning |
|---------------|---------|
| WiFi LED blinks white | Reading/flashing USB (good) |
| Progress bar or "UPDATE" on display | Flash in progress |
| Pedestal reboots | Flash complete — remove USB |
| `SOUNDTOUCH NOT CONFIGURED`, no reboot | Stick not read — wrong jack, FAT32/MBR issue, or junk files; try another USB 2.0 drive |
| 3× white blink then amber | USB read but firmware rejected |

#### Step 5 — Verify SSH

After the pedestal reboots, SSH should come up automatically (no USB stick
or extra steps needed — the patched firmware enables it unconditionally):

```sh
# Connect using the key you passed to build-ssh
ssh root@<device-ip>

# Confirm the gate is patched
remote_services_enabled && echo "SSH gate: enabled"

# Make SSH permanent in NV storage (survives firmware re-flash if you ever
# reflash the same patched .stu again)
touch /mnt/nv/remote_services
```

Find the device IP from your router's DHCP list or:
```sh
curl -s http://<device-ip>:8090/info | grep -o 'ip>[^<]*' | head -1
```

> **Connection refused?** The patched firmware starts sshd on first boot
> after flashing. If port 22 is still closed, the flash may not have applied
> (device saw no reboot / used an old cached image). Re-run Method B and
> watch for the white LED blink.

#### Step 6 — Rollback (if needed)

Flash stock firmware to revert all changes:
```sh
cp work/Update.stu /Volumes/BOSEFLASH/Update.stu   # use the original stock file
# remove junk files (Step 3), then re-flash (Step 4)
```

Stock MD5 for 27.00.06: `88c63e440cafa969ff19fb98b39be24a`

> Full USB flash procedure (stock firmware, Wave IV quirks, SSH enable
> separately): see [`README.flash.md`](README.flash.md).


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
2. **Phase B — Unpack/repack** the `SoftwareUpdateInstaller` container once format is documented.
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