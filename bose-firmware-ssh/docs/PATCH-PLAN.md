# Patch plan — persistent SSH in Wave IV firmware

## Objective

After one flash (USB or OTA), the pedestal should:

1. Start `sshd` on every normal boot
2. Listen on port 22 on the home LAN IP (WiFi and/or Ethernet)
3. Not require USB `remote_services` or SERVICE serial for initial enable
4. Optionally harden SSH (key-only) via `/mnt/nv/rc.local` template

## Non-goals (this phase)

- FLAC codec add
- Removing Bose cloud clients entirely
- Bypassing all signature checks without documenting risk

## Patch targets (priority)

### P0 — `remote_services_enabled` gate

Stock firmware already contains `remote_services_enabled`, `sshd`, `/mnt/nv/`.

**Patch idea:** invert or NOP the conditional so `mount.sh` / boot path always executes:

```sh
/etc/init.d/sshd start
```

Or always `touch /mnt/nv/remote_services` on first boot.

**RE steps:**

1. Run `python3 scripts/patch-research.py work/Update.stu`
2. Carve ELF(s) containing hits at `0x139c6b4`, `0x5cc007d`, `0x5cca95a`
3. Ghidra → xref `remote_services_enabled` string
4. Document function and branch to patch (ARM thumb)

### P1 — Init hook via `shelby_local`

SoundCork uses `[ -x /mnt/nv/rc.local ] && /mnt/nv/rc.local` at S97.

**Patch idea:** ship default `/mnt/nv/rc.local` in UBIFS image:

```sh
#!/bin/sh
touch /mnt/nv/remote_services
/etc/init.d/sshd start
```

Requires rootfs extraction, not just ELF NOP.

### P2 — Container repack

After rootfs or binary patch:

1. Rebuild BOSE chunk with correct length/CRC
2. Update outer header payload size at 0x90 if needed
3. Re-sign (if required)

**Validation:** `bose-usb-prep.sh --firmware ./work/Update-patched.stu` on test pedestal.

### P3 — OTA delivery

Prerequisites: P2 + working repack + test USB flash.

1. Telnet redirect `swUpdateUrl` to `http://<server>/updates/soundtouch`
2. Server implements Bose update check API
3. Serve patched `Update.stu` when version > current

## Phase timeline

| Phase | Work | Exit criteria |
|-------|------|---------------|
| 0 | Analysis scripts (this fork) | MD5 + strings + header documented |
| 1 | ELF carve + Ghidra | `remote_services_enabled` function located |
| 2 | In-container byte patch | Patched `.stu` same size or documented delta |
| 3 | USB flash test | Port 22 open after reboot, no USB stick |
| 4 | Rootfs-level patch | `rc.local` template in image |
| 5 | OTA | Speaker pulls patch from local server |

## Rollback

Always keep stock `Update.stu` (MD5 `88c63e440cafa969ff19fb98b39be24a`).

```sh
./bose-usb-prep.sh --firmware work/Update.stu.stock
```

## Parallel: no-firmware paths (production today)

| Path | Persistent SSH? |
|------|-----------------|
| USB `remote_services` + `touch /mnt/nv/remote_services` | Yes |
| SERVICE serial 3.5mm @ 115200 | Yes |
| Telnet `sys configuration` only | No — cloud redirect only |

Firmware patch is for Wave IV units where USB SSH never opens port 22.