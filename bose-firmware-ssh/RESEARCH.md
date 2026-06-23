# Research log — Wave SoundTouch IV firmware SSH

## Target hardware / firmware

| Field | Value |
|-------|-------|
| Model | Wave SoundTouch IV pedestal |
| Variant | `lisa` / `sm2` / `NoAP` (from live `8090/info`) |
| Firmware | 27.0.6.46330.5043500 (`epdbuild.trunk.hepdswbld04.2022-08-04`) |
| Package | `Bluetooth_WST4_Update_ti_27.00.06.46330.5043500.nelson.sm2.zip` |
| `Update.stu` size | 100,297,132 bytes (~96 MiB) |
| `Update.stu` MD5 | `88c63e440cafa969ff19fb98b39be24a` |

## Executive summary

SSH support **already exists** in stock firmware. It is gated by `remote_services_enabled()` which checks:

- USB stick file `/media/sda1/remote_services` (via `mount.sh` / udev), and/or
- Persistent flag `/mnt/nv/remote_services` (UBI NV partition)

Firmware 27.x **removed** the TAP command `remote_services on` (port 17000). Wave IV often still has TAP for `sys configuration` / `getpdo` but not shell unlock.

**Modifying firmware** means changing the gate (always enable SSH, or auto-create `/mnt/nv/remote_services` on first boot) inside the embedded Linux image, then repacking `Update.stu`.

## `Update.stu` container — SOLVED

The container is fully reverse-engineered. See `docs/STU-FORMAT.md` for the
field-level spec. Summary:

- 20-byte outer header: `BOSE` magic, total file size at 0x08, descriptor
  length `0x124` at 0x0C, and a **CRC32 of bytes [0:0x10]** at 0x10.
- A flat chain of **9 sections**, each `[0x124-byte descriptor][payload]`.
  Descriptor carries the section **name** (+0x04), **size** (+0x84), and a
  **CRC32 of the payload** (+0x118).
- Sections (by name): `SoftwareUpdateInstaller`, `ARMUpdate`, `UbootUpdate`,
  `ABL_Update`, `linuxPatched` (kernel uImage), **`ubi.img` (rootfs)**, `MLO`,
  `u-boot.img`, `SEN_FW_update.bos`.
- **Integrity = CRC32 only.** No RSA/signature in the container. 8/9 section
  CRC32s verify exactly; the trailing `SEN_FW` section uses a different
  checksum variant and is irrelevant to the rootfs patch.

Proof: `scripts/extract-stu.py` parses and verifies all CRCs; a no-op
`scripts/repack-stu.py` round-trip reproduces the **exact stock MD5**
`88c63e440cafa969ff19fb98b39be24a`.

### Rootfs extraction — SOLVED

The rootfs is the **`ubi.img`** section at `0x009341b4`, size `0x05500000`
(680 PEBs x 128 KiB, standard UBI, vol `rootfs`, lzo). It was previously missed
because naive `hsqs`/`SQLi` carving matched unrelated bytes. Correct path:

```sh
python3 scripts/extract-stu.py work/Update.stu -o work/sections
/tmp/ubienv/bin/ubireader_extract_files -o work/sections/rootfs work/sections/05_ubi.img
```

This yields a clean Linux root tree (3700+ entries: `bin etc lib sbin usr ...`).
The lone `hsqs`/`SQLi` magics elsewhere in the file are coincidental matches,
not standalone squashfs images.

### SSH-related strings (confirmed in 27.00.06 blob)

| String | Occurrences | Notes |
|--------|-------------|-------|
| `remote_services_enabled` | 3 | Primary patch target candidate |
| `sshd` | many | OpenSSH init scripts present |
| `telnetd` | present | Legacy remote shell |
| `/mnt/nv/` | many | NV persistence partition |
| `shelby_local` | 8+ | Init hook runs `/mnt/nv/rc.local` (SoundCork docs) |
| `ubimount.sh` | 1+ | UBI mount helper |

Example context at `0x139c6b4`:

```
... remote_services_enabled ...
```

### The gate is a 180-byte shell script — SOLVED

After extracting the rootfs, `remote_services_enabled` turns out to **not** be
an ARM binary at all. It is `/usr/bin/remote_services_enabled`, a 180-byte
POSIX shell script:

```sh
#!/bin/sh
# true if remote services are enabled.
# false otherwise.
[ -e /etc/remote_services ] ||
[ -e /mnt/nv/remote_services ] ||
[ -e /tmp/remote_services ] ||
! is-production
```

It is the single gate referenced by all three callers found in the rootfs:
`etc/init.d/sshd`, `etc/init.d/telnetd`, and `etc/udev/scripts/mount.sh`.

**Boot wiring confirmed:** `etc/rc5.d/S50sshd -> ../init.d/sshd` and
`etc/rc5.d/S10telnetd` run at boot, so sshd/telnetd are *already* invoked every
boot — they just exit early because the gate returns false. `is-production` is
`#!/bin/sh\ntrue`, so `! is-production` is false on retail units.

**Minimal clean patch:** rewrite `/usr/bin/remote_services_enabled` to
`#!/bin/sh\nexit 0`. SSH **and** telnet then start on every boot with no USB
stick and no NV flag. (Equivalent alternative: ship an empty `/etc/remote_services`
marker file in the rootfs.) This is a pure-rootfs change — no ARM RE needed.

## How stock SSH enable works (runtime)

From community `mount.sh` analysis ([source](https://technologicalmisadventures.wordpress.com/2018/12/05/soundtouch-20iii-ssh-telnet-etc/)):

1. USB mounted at `/media/sda1`
2. If `remote_services` file exists → `touch /tmp/remote_services`, start `sshd` + `telnetd`
3. Persistence: `touch /mnt/nv/remote_services` (SoundCork / manual SSH)

SoundCork 27.x path: empty `remote_services` on FAT32 USB, **separate** power-cycle from firmware flash.

## Patch strategies (ordered by feasibility)

### 1. Patch the gate script (preferred — IMPLEMENTED)

Rewrite `/usr/bin/remote_services_enabled` to always succeed. The boot-time
`S50sshd`/`S10telnetd` then start the daemons every boot. Two ways to apply it:

**(a) macOS-native, no Linux tools** — `scripts/inplace-patch-stu.py`.
The gate file is a single LZO-compressed UBIFS *data node*. Because the UBIFS
index stores `(key, LEB, offset, len)` but not the node's data-CRC, we can swap
the node payload for a 180-byte `#!/bin/sh\nexit 0\n#...` script whose LZO form
is tuned to the exact original 128-byte length, recompute just that node's CRC32
and the outer `.stu` section CRC32, and leave everything else untouched.
Verified: only **123 bytes** change in the whole 96 MiB file; the patched rootfs
re-extracts cleanly and the gate runs `exit 0`.

```sh
/tmp/ubienv/bin/python scripts/inplace-patch-stu.py work/Update.stu -o work/Update-ssh.stu
```

**(b) Full rebuild** — edit the extracted rootfs and rebuild the UBI image with
`scripts/rebuild-ubi.sh` (Linux `mtd-utils`), then `scripts/repack-stu.py`. Use
this when the change is larger than one same-length file (adding files, dropbear
keys, etc.).

### 2. NV/marker seed (alternative)

Ship an empty `/etc/remote_services` (read-only rootfs) or have init
`touch /mnt/nv/remote_services`. Same effect via the script's first test.

### 3. Repack container — SOLVED

After the rootfs edit:

1. Rebuild the UBI image with the stock geometry (`scripts/rebuild-ubi.sh`,
   needs Linux `mtd-utils`); it pads to the exact stock section size.
2. Splice it into `Update.stu` and recompute the section CRC32
   (`scripts/repack-stu.py`).
3. Header self-CRC is unchanged (file size preserved). No signature to forge.

### 4. OTA delivery

Point `swUpdateUrl` at a local server (telnet `sys configuration`) → serve the
repacked `Update.stu`. Depends on a working Bose update API mock.

## Blockers

| Blocker | Status |
|---------|--------|
| No public `.stu` unpacker | **RESOLVED** — format cracked; `scripts/extract-stu.py` parses + carves all sections |
| Clean squashfs/UBI extract | **RESOLVED** — rootfs is the `ubi.img` section; `ubi_reader` unpacks it fully |
| Repacking unknown | **RESOLVED** — CRC32-only; `scripts/repack-stu.py` proven by byte-identical round-trip |
| Checksum/signature scheme | **RESOLVED** — header self-CRC + per-section CRC32; no container-level RSA signature found |
| UBIFS rebuild on macOS | **WORKAROUND** — `scripts/inplace-patch-stu.py` patches the gate node in place (no mtd-utils). Full rebuild (`rebuild-ubi.sh`) only needed for larger rootfs changes |
| On-device kernel/ELF signature check | Unverified — we leave those sections byte-identical, so any such signatures stay valid |
| Wave IV USB `remote_services` unreliable | Motivation for the firmware patch (unchanged) |
| No test harness | Every flash test risks a brick — keep stock `.stu` for rollback |

## Tools in this fork

```sh
./scripts/fetch-firmware.sh 27.00.06
python3 scripts/analyze-stu.py work/Update.stu      # strings/markers overview
python3 scripts/extract-stu.py work/Update.stu -o work/sections   # parse + verify + carve
# macOS-native one-shot SSH patch (no Linux tools):
/tmp/ubienv/bin/python scripts/inplace-patch-stu.py work/Update.stu -o work/Update-ssh.stu
# full unpack/edit/rebuild loop via colima (any rootfs change):
./scripts/stu-toolbox.sh up
./scripts/stu-toolbox.sh unpack  work/Update.stu work/rootfs
./scripts/stu-toolbox.sh rebuild work/rootfs work/Update-custom.stu
# (under the hood: rebuild-ubi.sh + repack-stu.py inside the mtdtools image)
python3 scripts/patch-research.py work/Update.stu   # legacy hexdump helper
```

`scripts/stu_container.py` holds the shared container parser used by both
extract and repack.

## Next RE tasks

1. Run `rebuild-ubi.sh` in a Linux container to produce the patched `ubi.img`,
   then `repack-stu.py` → `Update-ssh.stu`; confirm all CRCs.
2. Diff 27.00.06 vs an older release around the gate to confirm stability.
3. USB-flash the patched `.stu` on **sacrificial** hardware; verify port 22.
4. (Optional) Investigate whether the bootloader checks the kernel uImage
   signature independently before trusting the rootfs.

## Remote OTA: confirmed DEAD END for enabling SSH

Live-tested against a real Wave SoundTouch (`192.168.0.119`, FW
`27.0.6.46330.5043500`). Two independent blockers, both verified:

1. **Index URL not overridable.** `GET :8090/swUpdateCheck` always returns
   `indexFileUrl="https://worldwide.bose.com/updates/soundtouch"`.
   `sys configuration swUpdateUrl <ours>` returns `OK` and `getpdo
   CurrentSystemConfiguration` shows our URL — but `swUpdateCheck` reads a
   *different* PDO. Even after `sys reboot` it still reports
   `worldwide.bose.com`. The URL is `https://`, so DNS/ARP spoofing fails on
   TLS validation. The `<swUpdateCheck/>` POST API accepts **no** URL params
   (any attribute/child → `CLIENT_XML_ERROR`).
2. **Same-version rejection.** The patched `.stu` carries the identical
   version string, so the updater treats it as "already up to date" and never
   downloads (server logs showed `firmware served 0x` every run).

The port-17000 CLI has **no shell escape**, no `setpdo`, and `sys
configuration` can only write two fixed XML files — none of which is a
`remote_services` marker. So there is no remote primitive to flip the gate.

## SSH gate = marker file (the actual solution)

`remote_services_enabled` is purely an OR of three file-existence checks
(`is-production` is always true, so its fallback never fires):

| Marker | Persistence |
|--------|-------------|
| `/etc/remote_services`     | rootfs (RO) |
| `/mnt/nv/remote_services`  | NV — survives reboots & updates |
| `/tmp/remote_services`     | volatile (this boot) |

The firmware's own udev rule (`etc/udev/scripts/mount.sh`) does, on USB
insert of `/dev/sda1`:

```sh
if [ -e "$mnt/remote_services" ]; then
    touch /tmp/remote_services
    /etc/init.d/sshd start
    /etc/init.d/telnetd start
fi
```

**Therefore: a USB stick with an empty file named `remote_services` at its
root brings up sshd (22) + telnetd (23) immediately — no flash, no reboot,
no version games.** Tooling: `scripts/enable-ssh-usb.sh`. For permanence,
`touch /mnt/nv/remote_services` from the resulting shell.

## Deliverables checklist

- [x] Fork folder `bose-firmware-ssh/`
- [x] Download + MD5 stock firmware
- [x] Header / string analysis scripts
- [x] Container format spec (complete — `docs/STU-FORMAT.md`)
- [x] Rootfs extraction (UBI → clean root tree)
- [x] Patched rootfs with persistent SSH (gate script → `exit 0`)
- [x] Repack tooling proven (byte-identical round-trip)
- [x] Patched flashable `Update.stu` (macOS-native in-place patch; `work/Update-ssh.stu`)
- [ ] USB flash validation on test hardware
- [x] OTA server endpoint built (`scripts/ota-update-server.py` + `ota-deploy.sh`)
- [x] OTA enable-SSH path proven IMPOSSIBLE on this FW (see section above)
- [x] USB `remote_services` enable tooling (`scripts/enable-ssh-usb.sh`) — the
      reliable, no-flash way to get SSH