#!/bin/sh
# Rebuild the rootfs UBI image from a patched root tree, padded to the exact
# stock section size so it can be spliced back into Update.stu.
#
# Geometry is taken verbatim from the stock 27.00.06 WST4 image
# (ubireader_utils_info on section "ubi.img"):
#
#   mkfs.ubifs -m 2048 -e 126976 -c 744 -x lzo -f 8 -k r5 -p 1 -l 5
#   ubinize    -p 131072 -m 2048 -O 2048 -s 2048 -x 1 -Q 778987469
#   volume: id 0, name rootfs, dynamic, autoresize
#
# Requires mtd-utils (mkfs.ubifs, ubinize). These are Linux-only; on macOS run
# this inside a container, e.g.:
#
#   docker run --rm -v "$PWD":/w -w /w debian:bookworm sh -c \
#     'apt-get update && apt-get install -y mtd-utils && \
#      sh scripts/rebuild-ubi.sh <rootfs_dir> <out_ubi.img>'
#
# Usage: rebuild-ubi.sh <rootfs_dir> <out_ubi.img> [stock_section_size]
set -eu

ROOTFS="${1:?usage: rebuild-ubi.sh <rootfs_dir> <out_ubi.img> [stock_size]}"
OUT="${2:?usage: rebuild-ubi.sh <rootfs_dir> <out_ubi.img> [stock_size]}"
STOCK_SIZE="${3:-89128960}"   # 0x5500000

IMG_SEQ=778987469
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/ubinize.ini" <<EOF
[rootfs]
mode = ubi
image = $TMP/root.ubifs
vol_type = dynamic
vol_flags = autoresize
vol_id = 0
vol_name = rootfs
vol_alignment = 1
EOF

echo "mkfs.ubifs ..."
mkfs.ubifs -m 2048 -e 126976 -c 744 -x lzo -f 8 -k r5 -p 1 -l 5 \
    -r "$ROOTFS" "$TMP/root.ubifs"

echo "ubinize ..."
ubinize -p 131072 -m 2048 -O 2048 -s 2048 -x 1 -Q "$IMG_SEQ" \
    -o "$TMP/root.ubi" "$TMP/ubinize.ini"

CUR=$(wc -c < "$TMP/root.ubi")
echo "rebuilt UBI: $CUR bytes (stock section: $STOCK_SIZE)"
if [ "$CUR" -gt "$STOCK_SIZE" ]; then
    echo "ERROR: rebuilt UBI ($CUR) larger than stock section ($STOCK_SIZE)." >&2
    echo "The patched rootfs grew too much; remove files or shrink the patch." >&2
    exit 1
fi

# Pad with 0xFF (UBI erased-flash value) up to the exact stock section size.
cp "$TMP/root.ubi" "$OUT"
PAD=$((STOCK_SIZE - CUR))
if [ "$PAD" -gt 0 ]; then
    # write PAD 0xFF bytes
    head -c "$PAD" /dev/zero | tr '\000' '\377' >> "$OUT"
fi
echo "wrote $OUT ($(wc -c < "$OUT") bytes) -- ready for repack-stu.py"
