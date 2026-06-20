# Offline USB Firmware Reflash — Bose Wave SoundTouch IV Pedestal

Use this guide when the pedestal is in the **stuck-firmware** state: the
`Bose Wave ST (…)` WiFi AP broadcasts, DHCP hands out leases, and the
speaker is ARP-reachable — but **every TCP port refuses instantly** and no
app (including the official Bose app) can connect. This is not a phone or
app problem. The setup web server inside the pedestal never launched. The
fix is a forced offline firmware reflash via a USB stick.

See [README.SoundTouchIV-wifi.md](README.SoundTouchIV-wifi.md) for the
full diagnosis and root-cause explanation.

---

## Why the Bose Updater website no longer works

Bose shut down the SoundTouch cloud servers, including the online
firmware-update servers, on **6 May 2026**. `btu.bose.com` returns 404.
OTA updates are gone. The only remaining path is the **offline USB method**
documented here.

---

## Overview

```
1. Get firmware   →  2. Prepare USB   →  3. Flash pedestal   →  4. Setup WiFi   →  5. Enable SSH   →  6. SoundCork
```

Steps 3 and 5 are **separate pedestal procedures**. Putting both files on one stick
(`--both`) does not enable SSH during the firmware flash.

---

## Step 1 — Identify your firmware

The Wave SoundTouch IV pedestal uses the **Bluetooth/SM2 platform** — do
not use the "Non-Bluetooth (2013-2015)" folder. Every firmware file for
your unit is named `Bluetooth_WST4_*.zip`.

### Community firmware archive (Internet Archive)

The full release history is preserved here — this is the only working
source since Bose's own servers are down:

**Browse:**
[https://archive.org/download/bose-soundtouch-software-and-firmware/Firmware/2015-2020_Bluetooth/Bluetooth_Wave_SoundTouch_IV/](https://archive.org/download/bose-soundtouch-software-and-firmware/Firmware/2015-2020_Bluetooth/Bluetooth_Wave_SoundTouch_IV/)

### Available versions (newest first)

| Version | Zip filename | Size |
|---------|-------------|------|
| **27.00.06** ← recommended | `Bluetooth_WST4_Update_ti_27.00.06.46330.5043500.nelson.sm2.zip` | 73.4 MB |
| 27.00.03 | `Bluetooth_WST4_Update_ti_27.00.03.46298.4608935.nelson.sm2.zip` | 73.4 MB |
| 27.00.02 | `Bluetooth_WST4_Update_ti_27.00.02.46286.4536626.nelson.sm2.zip` | 73.4 MB |
| 27.00.01 | `Bluetooth_WST4_Update_ti_27.00.01.46282.4378406.nelson.sm2.zip` | 73.4 MB |
| 26.00.01 | `Bluetooth_WST4_Update_ti_26.00.01.46256.3990103.nelson.sm2.zip` | 73.3 MB |
| 25.00.00 | `Bluetooth_WST4_Update_ti_25.00.00.46176.3844119.nelson.sm2.zip` | 73.0 MB |
| 14.00.33 | `Bluetooth_WST4_14.00.33.zip` | 58.5 MB |
| 09.00.41 | `Bluetooth_WST4_09.00.41.zip` | 55.4 MB |

**Use 27.00.06** (the newest in the archive). If that fails to flash,
try the previous 27.x release, then work backwards.

> **Why not downgrade?** Downgrading is only needed if a specific version
> caused the problem (e.g. an interrupted mid-version update). Start with
> the newest and only go backwards if reflashing the newest fails.

### Direct download link for 27.00.06

```
https://archive.org/download/bose-soundtouch-software-and-firmware/Firmware/2015-2020_Bluetooth/Bluetooth_Wave_SoundTouch_IV/Bluetooth_WST4_Update_ti_27.00.06.46330.5043500.nelson.sm2.zip
```

---

## Step 2 — Prepare the USB stick

### Requirements

- USB-A stick, **32 GB or smaller**
- Partition table: **MBR (Master Boot Record / FDisk)** — GPT will not work;
  the pedestal's embedded USB host stack does not parse GPT
- Filesystem: **FAT32** — not exFAT (pedestal kernel has no exFAT module),
  not NTFS, not FAT16
- Firmware flash: only **`Update.stu`** at the USB root — no folders, no extra files
- SSH enable: only **`remote_services`** at the USB root (see [Flash vs SSH](#flash-vs-ssh-two-separate-procedures))

### Automated method (recommended)

`bose-usb-prep.sh` handles all of the above — format, junk removal,
download, extraction, MD5 verify, and format validation — in one command:

```sh
./bose-usb-prep.sh              # firmware flash only (default)
./bose-usb-prep.sh --both       # both files on stick — still two pedestal passes
./bose-usb-prep.sh --ssh        # SSH-only stick (use after flash + WiFi setup)
./bose-usb-prep.sh --dry-run    # preview all steps without writing anything
```

### Flash vs SSH: two separate procedures

`bose-usb-prep.sh --both` writes **`Update.stu`** and **`remote_services`** onto
the same USB stick. That saves you from preparing two sticks, but it does **not**
run both operations in one insertion. The pedestal treats them as different
boot-time behaviors:

| | Firmware flash | SSH enable |
|---|----------------|------------|
| **USB file** | `Update.stu` | `remote_services` (empty, no extension) |
| **When** | Stuck firmware / first recovery | After flash and WiFi setup, before SoundCork |
| **Power state** | Powered on, or power-cycle + Control button | **Power off** → insert USB → power on |
| **USB port** | SETUP / SETUP B / SERVICE jack | **Setup B** (USB-A on pedestal back) |
| **Success signal** | Pedestal reboots on its own | `ssh root@<ip>` connects (port 22 open) |
| **After** | Remove USB, enter setup mode | `touch /mnt/nv/remote_services` to persist |

**Common mistake:** run `--both`, flash firmware, remove the USB, join home WiFi,
then try `ssh` — and get `Connection refused`. The flash path processed
`Update.stu`; `remote_services` was never consumed because you never did the
SSH power-cycle pass.

**Recommended workflow:**

1. `./bose-usb-prep.sh` or `./bose-usb-prep.sh --both` — flash the pedestal.
2. Remove USB when the pedestal reboots. Complete WiFi setup (Step 4).
3. `./bose-usb-prep.sh --ssh` — prepare an **SSH-only** stick (no `Update.stu`).
4. Power off → insert SSH stick into **Setup B** → power on → wait ~60 s.
5. `ssh root@<speaker-ip>` then `touch /mnt/nv/remote_services`.

Use `--ssh` for step 3, not the original `--both` stick. If `Update.stu` is still
on the stick, re-inserting it may trigger another firmware update instead of SSH.

> **Wave IV caveat:** The `remote_services` USB method is documented for SoundTouch
> speakers and works on many units. Some Wave SoundTouch IV owners report SSH still
> refusing after the correct procedure ([soundcork#309](https://github.com/deborahgu/soundcork/issues/309)).
> If that happens, try Ethernet for the SSH boot, a different USB 2.0 stick, and
> confirm the stick has only `remote_services` with no macOS junk files.

### Manual method

**macOS:**
```sh
diskutil list   # find your USB — look for "external"
diskutil eraseDisk FAT32 BOSEFLASH MBRFormat /dev/diskN
```

**Windows:**
Use the [HP USB Disk Storage Format Tool](https://www.bleepingcomputer.com/download/hp-usb-disk-storage-format-tool/) —
choose FAT32, leave all other options unchecked. Windows cannot natively
format drives >32 GB as FAT32 — use this tool regardless of size.

**Linux:**
```sh
parted -s /dev/sdX mklabel msdos
parted -s /dev/sdX mkpart primary fat32 1MiB 100%
mkfs.vfat -F 32 -n BOSEFLASH /dev/sdX1
```

### macOS junk files — CRITICAL

macOS silently creates hidden files on every FAT32 volume that **prevent
the pedestal from detecting the firmware file**. Remove them after copying:

```sh
mdutil -i off /Volumes/BOSEFLASH
rm -rf /Volumes/BOSEFLASH/.fseventsd /Volumes/BOSEFLASH/.Spotlight-V100
rm -f  /Volumes/BOSEFLASH/._* /Volumes/BOSEFLASH/.DS_Store
```

Run these commands **after** copying `Update.stu` — macOS recreates the
`._Update.stu` AppleDouble file whenever you write to the volume.

### Extract and copy the firmware file

1. Download the zip (e.g. `Bluetooth_WST4_Update_ti_27.00.06…zip`)
2. Unzip it — you will find a file called **`Update.stu`** inside
3. Copy **only `Update.stu`** to the **root of the USB stick** — do not
   put it inside any folder
4. Remove macOS junk files (see above)
5. Eject the USB stick safely

Your USB stick should look like this:

```
BOSEFLASH/
└── Update.stu        ← the only file on the drive
```

---

## Step 3 — Flash the pedestal

> **Important — this is a two-unit system.** The Wave SoundTouch IV is a
> **Wave console** (top unit: display, numbered buttons 1–6, volume, radio/CD)
> sitting on a **SoundTouch pedestal** (bottom unit: WiFi/Bluetooth radios,
> the firmware brain, the USB port, and a single **Control button** on the
> back), joined by the Bose link cable. **The pedestal is what flashes
> `Update.stu`.** The console's numbered buttons have nothing to do with the
> firmware update.

The firmware USB port is on the **pedestal**, labelled **SETUP / SETUP B /
SERVICE** (next to the Control button and the Bose link connector).

> **Which port?** Use the jack labelled **SETUP / SETUP B / SERVICE** only —
> not the network/computer-setup port. Some pedestal revisions use a
> **Micro-USB** SETUP jack, which needs a USB-A-female → Micro-USB-male
> adapter for a standard USB stick.

> **Do NOT use the "Button 4 + Volume Down" sequence.** That is the procedure
> for **standalone SoundTouch 10/20/30 speakers**, where the USB port and the
> numbered buttons are on the same unit. On the Wave SoundTouch IV those
> buttons are on the console and do not drive the pedestal — holding them
> just boots normally to `SOUNDTOUCH NOT CONFIGURED`.

### Method 1 — auto-flash (primary, no buttons)

1. Leave the system **powered ON**.
2. **Insert the USB stick** into the pedestal's **SETUP / SETUP B / SERVICE**
   jack (use a Micro-USB adapter if that jack is Micro-USB).
3. Wait **30 seconds to a few minutes**. The pedestal **reboots on its own**
   when the flash completes — that is the success signal.
4. **Do not touch the console's numbered buttons.**

### Method 2 — pedestal Control button (fallback)

If Method 1 produces no reboot:

1. **Unplug the AC power cord** completely.
2. **Insert the USB** into the SETUP / SETUP B / SERVICE jack.
3. Press and **hold the Control button on the BACK of the pedestal**.
4. While still holding, **reconnect AC power**. Keep holding ~5 seconds,
   then release.
5. Wait up to 5 minutes — the pedestal reboots automatically when done.

### Signs the flash is working

- Pedestal **WiFi LED blinks white** (it is reading/processing the USB)
- Display may show a progress bar, "UPDATE", or "PLEASE WAIT", or go blank
- Pedestal **reboots on its own** after ~30 s to a few minutes

### Signs something went wrong

- **`SOUNDTOUCH NOT CONFIGURED`** appears and nothing else happens → the
  pedestal booted normally without reading the USB. Most common causes:
  wrong jack (use SETUP/SETUP B/SERVICE, add a Micro-USB adapter if needed),
  macOS junk files on the stick (re-run `bose-usb-prep.sh`), or the drive
  is not being enumerated — try a different small USB 2.0 drive
- **WiFi LED never changes** → the stick is not being read at all; recheck
  the jack, FAT32 format, and try another drive
- **LED blinks white 3× then goes amber** → the USB was read but the
  firmware was rejected; try the previous firmware version
  (`./bose-usb-prep.sh --version 27.00.03`, then `26.00.01`)
- Pedestal reboots into the same stuck state → try the previous firmware
  version and repeat the procedure

---

## Step 4 — Verify and enter setup mode

After the reflash reboot:

1. Remove the USB stick.
2. Wait 60 seconds for the system to fully boot.
3. **Hold the Control button** on the back of the pedestal for ~3 seconds.
4. The WiFi light should go **solid amber** and the display should show
   **`SETUP SEE INSTRUCTIONS`**.
5. On your phone, join the `Bose Wave ST (…)` WiFi network and tap
   **Stay connected** when Android warns about no internet.
6. Open **BosMan** — the WiFi Setup panel should now be able to reach the
   speaker at `192.0.2.1` (or `192.168.1.1`) on port 80.
7. Pick your home WiFi, enter the password, submit. The pedestal reboots
   onto your home network. The `Bose Wave ST` AP disappears.

If setup mode still produces no port-80 response after reflashing, see
the [Field diagnostics section of README.SoundTouchIV-wifi.md](README.SoundTouchIV-wifi.md#field-diagnostics-hard-won-read-this-before-debugging)
for how to probe directly through the app.

---

## Step 5 — Replace the dead Bose cloud with SoundCork

Once the pedestal is on your home WiFi and reachable on port 8090, the
speaker works locally (BosMan can control it). However the **Bose cloud is
permanently gone** (shut down 6 May 2026), so:

- TuneIn presets no longer resolve
- The official Bose SoundTouch app loses most functionality
- You cannot configure new presets via the cloud

**SoundCork** is a self-hosted drop-in replacement for all four Bose cloud
servers. You run it on a Raspberry Pi, NAS, Docker host, or any always-on
machine, and redirect the speaker's cloud traffic to your server.

### Resources

- Project: <https://github.com/timvw/soundcork>
- Speaker setup guide: <https://github.com/timvw/soundcork/blob/main/docs/speaker-setup.md>
- Companion CLI: <https://github.com/timvw/bose>
- Write-up: [Keep your Bose SoundTouch alive after the shutdown](https://timvw.be/2026/02/17/keep-your-bose-soundtouch-speaker-alive-after-the-shutdown/)

### SoundCork setup in brief

The full procedure is in the [speaker-setup guide](https://github.com/timvw/soundcork/blob/main/docs/speaker-setup.md).
Summary:

#### 1. Enable SSH on the pedestal (USB method)

This is a **separate procedure** from the firmware flash in Steps 2–4. If you
used `--both` earlier, that only put both files on the stick — you still need
this pass after WiFi setup. Prepare an SSH-only stick:

```sh
./bose-usb-prep.sh --ssh
```

Or manually: create a **blank file called `remote_services`** (no extension) at
the root of a clean FAT32 USB stick. On macOS, remove junk files first (same
commands as Step 2 above). The stick must **not** contain `Update.stu`.

```
BOSEFLASH/
└── remote_services       ← empty file, no extension
```

Then on the pedestal (speaker must already be on home WiFi):

1. Power off completely (pull AC cord)
2. Insert the stick into **Setup B** (USB-A jack on the pedestal back)
3. Power on and wait ~60 seconds
4. SSH in — no password required:

```sh
ssh root@<speaker-ip>
```

Use `root@` — `ssh <ip>` alone tries your Mac username and will fail even when
SSH is running.

Find the speaker's IP from your router's DHCP list or:

```sh
# Once BosMan found it on port 8090, the IP is shown in the app
# Or check your router, or:
curl -s http://<speaker-ip>:8090/info | grep -o 'ip>[^<]*' | head -1
```

#### 2. Extract speaker data

```sh
curl http://<speaker-ip>:8090/presets    > Presets.xml
curl http://<speaker-ip>:8090/recents    > Recents.xml
curl http://<speaker-ip>:8090/info       > DeviceInfo.xml

# Sources.xml (Spotify tokens) — requires SSH:
ssh root@<speaker-ip> cat /mnt/nv/BoseApp-Persistence/1/Sources.xml > Sources.xml
```

Get your account UUID from `DeviceInfo.xml` — look for `margeAccountUUID`.

#### 3. Run SoundCork and redirect the speaker

Follow the [SoundCork README](https://github.com/timvw/soundcork) to
start the server, then on the pedestal:

```sh
ssh root@<speaker-ip>
rw    # make filesystem read-write
vi /opt/Bose/etc/SoundTouchSdkPrivateCfg.xml
```

Change all four server URLs to your SoundCork instance:

| Server | Original | Replace with |
|--------|---------|--------------|
| marge | `https://streaming.bose.com` | `https://your-soundcork-server` |
| bmx | `https://content.api.bose.io` | `https://your-soundcork-server` |
| updates | `https://worldwide.bose.com` | `https://your-soundcork-server` |
| stats | `https://events.api.bosecm.com` | `https://your-soundcork-server` |

Reboot the speaker. TuneIn presets and streaming resume through your
SoundCork server.

#### 4. Make SSH persistent (optional but recommended)

```sh
ssh root@<speaker-ip>
touch /mnt/nv/remote_services
```

This keeps SSH accessible across reboots so you can manage SoundCork
config without re-inserting the USB each time.

---

## Quick reference

| Stage | What to see | What to do next |
|-------|------------|-----------------|
| Before flash | AP broadcasts, DHCP works, port 80 refused | Prepare USB with `bose-usb-prep.sh` |
| Inserting USB | Pedestal WiFi LED blinks white | Insert in SETUP/SETUP B/SERVICE jack, leave it |
| Flash in progress | Progress bar / "UPDATE" / WiFi LED blinks | Wait, do not unplug |
| Flash complete | Pedestal reboots automatically (~30 s–5 min) | Remove USB, hold Control ~3 s for setup mode |
| Setup mode OK | Solid amber + `SETUP SEE INSTRUCTIONS` | Join Bose AP, open BosMan, enter home WiFi |
| On home WiFi | Bose AP disappears | Open BosMan, search devices |
| SSH needed | Port 8090 works, port 22 refused | `./bose-usb-prep.sh --ssh`, power-cycle with stick in Setup B |
| SSH enabled | `ssh root@<ip>` connects | `touch /mnt/nv/remote_services`, then SoundCork setup |
| Cloud dead | Presets / TuneIn broken | Set up SoundCork |
| SoundCork running | Presets resolve, streaming works | Done |
| **Wrong — USB ignored** | `SOUNDTOUCH NOT CONFIGURED`, no reboot | Check SETUP jack/adapter, FAT32, junk files, try another drive; do NOT use console buttons |

---

## References

- Firmware archive: <https://archive.org/download/bose-soundtouch-software-and-firmware/Firmware/2015-2020_Bluetooth/Bluetooth_Wave_SoundTouch_IV/>
- Community reflash guide: <https://www.reddit.com/r/bose/comments/1lb1uav/solved_bose_soundtouch_firmware_downgrade_guide/>
- Bose Wikia reflash guide: <https://bose.fandom.com/wiki/SoundTouch_Firmware_Downgrade_Guide>
- SoundCork project: <https://github.com/timvw/soundcork>
- SoundCork speaker setup: <https://github.com/timvw/soundcork/blob/main/docs/speaker-setup.md>
- Bose cloud EOL notice: <https://www.bose.com/soundtouch-end-of-life>
- WiFi setup guide: [README.SoundTouchIV-wifi.md](README.SoundTouchIV-wifi.md)
