#!/usr/bin/env bash
# Builds a 64 MiB FAT32 disk image with sample files, for the kernel's FAT
# driver to mount at /disk. Uses mtools, so no root or loop devices.
#   usage: tools/mkfatdisk.sh <image>
# The sample set deliberately covers each way FAT stores a name: a
# lowercase 8.3 name (case flags), a plain 8.3 name, and a VFAT long name
# in a subdirectory, plus a file spanning a couple hundred clusters.
set -euo pipefail
IMG=${1:?usage: mkfatdisk.sh <image>}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count=64 status=none
mformat -i "$IMG" -F -v ATOS ::

printf 'Hello from a FAT32 disk!\n' > "$TMP/hello.txt"
printf 'Long file names work.\n' > "$TMP/long.txt"
seq 1 20000 > "$TMP/numbers.txt"

mcopy -i "$IMG" "$TMP/hello.txt" ::/hello.txt
mcopy -i "$IMG" "$TMP/numbers.txt" ::/NUMBERS.TXT
mmd -i "$IMG" ::/docs
mcopy -i "$IMG" "$TMP/long.txt" "::/docs/A long file name.txt"
