# Update.stu container format (Wave IV / 27.00.06)

> **Status: fully reverse-engineered.** Verified by a byte-identical
> extract -> repack round-trip (`scripts/repack-stu.py`) reproducing the stock
> MD5 `88c63e440cafa969ff19fb98b39be24a`.

## File identity

| Property | Value |
|----------|-------|
| Filename inside zip | `Update.stu` |
| Zip folder | `Bluetooth [WST] Update_ti_27.0.6.46330.5043500.nelson.sm2/` |
| Size | 100,297,132 bytes (`0x05fa69ac`) |
| MD5 | `88c63e440cafa969ff19fb98b39be24a` |
| `file(1)` | `data` (custom container) |

## Outer header (bytes 0x00-0x13)

| Offset | Size | Content | Notes |
|--------|------|---------|-------|
| 0x00 | u32 be | `0x00000014` | magic-block length (20) |
| 0x04 | 4 | ASCII `BOSE` | magic |
| 0x08 | u32 be | `0x05fa69ac` | **total file size in bytes** |
| 0x0C | u32 be | `0x00000124` | **section-descriptor length (292)** |
| 0x10 | u32 be | `0xf011aa3a` | **CRC32 of bytes [0x00:0x10]** (header self-CRC) |

`CRC32(data[0:0x10]) == data[0x10]`. Because the only mutable field it covers
is the total size at 0x08, the header CRC is unchanged by any **same-length**
payload edit.

## Section chain

After the 20-byte header, the file is a flat chain of sections:

```
[ 0x124-byte descriptor ][ payload ] [ 0x124-byte descriptor ][ payload ] ...
```

The first descriptor begins at `0x14`. Each subsequent descriptor begins
immediately after the previous payload.

### Descriptor fields (relative to descriptor start)

| Rel offset | Size | Content |
|------------|------|---------|
| +0x04 | char[] | section name, NUL-terminated (e.g. `ubi.img`) |
| +0x84 | u32 be | payload size |
| +0x88 | u32 be | payload size (duplicate) |
| +0x118 | u32 be | **CRC32 of the payload** |

(The first descriptor's name field overlaps the `SoftwareUpdateInstaller`
string in the header region; size@0x98 and crc@0x12c follow the same rule.)

## Section map (stock 27.00.06)

| # | Name | payload offset | size | payload CRC32 | type |
|---|------|----------------|------|---------------|------|
| 0 | `SoftwareUpdateInstaller` | 0x00000138 | 0x001cc60c | 0xb4eee693 | ARM ELF (installer) |
| 1 | `ARMUpdate`   | 0x001cc868 | 0x0015a80c | 0x67d0f3c3 | ARM ELF |
| 2 | `UbootUpdate` | 0x00327198 | 0x0015b634 | 0x993c6aff | ARM ELF |
| 3 | `ABL_Update`  | 0x004828f0 | 0x001a167c | 0x5abf4fba | ARM ELF |
| 4 | `linuxPatched`| 0x00624090 | 0x00310000 | 0x78f8fae2 | U-Boot uImage (kernel) |
| 5 | **`ubi.img`**| **0x009341b4** | **0x05500000** | **0xd291845c** | **UBI rootfs** |
| 6 | `MLO`         | 0x05e342d8 | 0x00007630 | 0x038582b6 | X-loader (SPL) |
| 7 | `u-boot.img`  | 0x05e3ba2c | 0x00067e5c | 0xbcdcddcb | U-Boot uImage |
| 8 | `SEN_FW_update.bos` | 0x05ea39ac | 0x00103000 | 0x81ea0387* | Sensory voice FW |

\* Section 8's stored value does not match a plain CRC32 of its payload (it
uses a different/again-wrapped checksum variant). It is the trailing section
and is unrelated to the rootfs; it can be left untouched.

## Integrity model

- **Header self-CRC** over `[0x00:0x10]` (protects the size field).
- **Per-section CRC32** over each payload, stored at descriptor +0x118.
- **No RSA / public-key signature** is present in the container structure.

Implication: a patched rootfs only requires recomputing the section-5 CRC32.
The header CRC is unaffected as long as the total file size is preserved
(`rebuild-ubi.sh` pads the new UBI image to the exact stock section size).

> Caveat: the on-device installer/bootloader may still verify the *kernel* or
> *individual ELF* payloads by their own internal means. We only patch the
> rootfs UBI, leaving sections 0-4 and 6-8 byte-identical, so any signatures on
> those remain valid.

## UBI rootfs geometry (section 5)

From `ubireader_utils_info` on the carved `ubi.img`:

| Parameter | Value |
|-----------|-------|
| PEB size | 131072 (128 KiB) |
| LEB size | 126976 |
| min I/O / sub-page | 2048 |
| VID header offset | 2048 |
| max LEB count (`-c`) | 744 |
| compression | **lzo** |
| fanout / log_lebs / orph_lebs | 8 / 5 / 1 |
| key hash | r5 |
| volume | id 0, name `rootfs`, dynamic, **autoresize** |
| image sequence | 778987469 |
| total PEBs in image | 680 (2 layout + 678 data) |

Rebuild commands (Linux `mtd-utils`, wrapped by `scripts/rebuild-ubi.sh`):

```sh
mkfs.ubifs -m 2048 -e 126976 -c 744 -x lzo -f 8 -k r5 -p 1 -l 5 -r <rootfs> root.ubifs
ubinize  -p 131072 -m 2048 -O 2048 -s 2048 -x 1 -Q 778987469 -o root.ubi ubinize.ini
# then pad root.ubi with 0xFF up to 0x5500000 bytes
```

## End-to-end workflow

### Fast path -- SSH only (macOS-native, no Linux tools)

```sh
python3 -m venv /tmp/ubienv && /tmp/ubienv/bin/pip install ubi_reader
/tmp/ubienv/bin/python scripts/inplace-patch-stu.py work/Update.stu -o work/Update-ssh.stu
python3 scripts/extract-stu.py work/Update-ssh.stu --no-carve   # all CRCs OK
```

In-place node surgery; no UBIFS rebuild. Changes only 123 bytes (128-byte LZO
node payload + node CRC32 + container section CRC32).

### General path -- any rootfs edit (Linux toolchain via colima)

`scripts/stu-toolbox.sh` wraps mtd-utils + ubi_reader in a container so the
unpack/rebuild runs as root (perms, owners and symlinks preserved). It manages
colima by default but uses any Docker-compatible runtime already present.

```sh
./scripts/stu-toolbox.sh up                                   # runtime + build mtdtools image
./scripts/stu-toolbox.sh unpack  work/Update.stu work/rootfs  # carve ubi.img + extract rootfs
#   ... edit work/rootfs ...
./scripts/stu-toolbox.sh rebuild work/rootfs work/Update-custom.stu   # mkfs.ubifs+ubinize -> repack -> verify
```

One-time setup if you have no runtime: `brew install colima docker`.

> **Rebuild fidelity caveat:** `ubi_reader` extraction does not recreate device
> nodes or carry xattrs (e.g. file capabilities). Embedded Linux normally
> repopulates `/dev` via devtmpfs/mdev at boot, and this firmware shows no use of
> file capabilities, so a rebuild is typically faithful -- but for a minimal,
> provably-identical change prefer the in-place fast path above.

