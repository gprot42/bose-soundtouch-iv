# Linux toolchain for BOSE Update.stu rebuilds (UBIFS/UBI).
#
# Bundles everything the full-rebuild path needs so rootfs perms, owners and
# device nodes survive an unpack -> edit -> repack cycle when run as root:
#   * mtd-utils  -> mkfs.ubifs / ubinize
#   * ubi_reader -> ubireader_extract_files (pulls in lzallright for LZO)
#   * python3    -> extract-stu.py / repack-stu.py / inplace-patch-stu.py
#
# Built and driven by scripts/stu-toolbox.sh. Works on colima, Docker Desktop,
# OrbStack, podman or nerdctl.
FROM debian:bookworm-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        mtd-utils \
        python3 \
        python3-pip \
        ca-certificates \
    && pip3 install --no-cache-dir --break-system-packages ubi_reader \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# mkfs.ubifs / ubinize live in /usr/sbin; pip installs ubireader_* into
# /usr/local/bin. Keep both (plus the standard dirs) on PATH for any shell.
ENV PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

WORKDIR /repo
