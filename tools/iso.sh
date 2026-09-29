#!/usr/bin/env bash
# Stages a bootable ISO: kernel.elf + initrd.tar + limine.conf + the Limine bootloader
# binaries, then BIOS-installs the Limine stage1/2 into the ISO itself so
# it boots under both BIOS and UEFI QEMU.
set -euo pipefail
cd "$(dirname "$0")/.."

KERNEL=kernel.elf
INITRD=initrd.tar
# Overridable so tests can build variants with their own boot config.
ISO_ROOT=${ISO_ROOT:-iso_root}
ISO=${ISO:-atos.iso}
LIMINE_CONF=${LIMINE_CONF:-limine.conf}
LIMINE_DIR=third_party/limine

for f in "$KERNEL" "$INITRD"; do
    if [ ! -f "$f" ]; then
        echo "error: $f not found, run 'make' first" >&2
        exit 1
    fi
done

rm -rf "$ISO_ROOT"
mkdir -p "$ISO_ROOT/boot/limine" "$ISO_ROOT/EFI/BOOT"

cp "$KERNEL" "$INITRD" "$ISO_ROOT/boot/"
cp "$LIMINE_CONF" "$ISO_ROOT/boot/limine/limine.conf"
cp "$LIMINE_DIR/limine-bios.sys" "$LIMINE_DIR/limine-bios-cd.bin" "$LIMINE_DIR/limine-uefi-cd.bin" \
    "$ISO_ROOT/boot/limine/"
cp "$LIMINE_DIR/BOOTX64.EFI" "$LIMINE_DIR/BOOTIA32.EFI" "$ISO_ROOT/EFI/BOOT/"

xorriso -as mkisofs -quiet -R -r -J -b boot/limine/limine-bios-cd.bin \
    -no-emul-boot -boot-load-size 4 -boot-info-table \
    --efi-boot boot/limine/limine-uefi-cd.bin \
    -efi-boot-part --efi-boot-image --protective-msdos-label \
    "$ISO_ROOT" -o "$ISO"

if [ ! -x "$LIMINE_DIR/limine" ]; then
    make -C "$LIMINE_DIR" limine
fi
"$LIMINE_DIR/limine" bios-install "$ISO"

echo "built $ISO"
