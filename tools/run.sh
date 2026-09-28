#!/usr/bin/env bash
# Boots atos.iso in QEMU with serial routed to stdio -- this, not the
# framebuffer, is the fast debug loop (see IMPLEMENTATION_PLAN.md).
# Pass --debug to also pause at the first instruction and wait for gdb on
# localhost:1234 (see .gdbinit).
set -euo pipefail
cd "$(dirname "$0")/.."

ISO=atos.iso
DISK=disk.img

# Persistent scratch disk for the virtio-blk driver (the kernel's disk
# self-test expects this signature in sector 0). Delete it to start over.
if [ ! -f "$DISK" ]; then
    dd if=/dev/zero of="$DISK" bs=1M count=64 status=none
    printf 'ATOSDISK' | dd of="$DISK" conv=notrunc status=none
fi

# disable-legacy=off: the driver speaks the legacy virtio PCI interface.
QEMU_FLAGS=(-M q35 -m 256M -serial stdio -no-reboot -no-shutdown -cdrom "$ISO"
            -drive "file=$DISK,format=raw,if=none,id=disk0"
            -device virtio-blk-pci,drive=disk0,disable-legacy=off)

if [ "${1:-}" = "--debug" ]; then
    QEMU_FLAGS+=(-s -S)
    echo "QEMU is waiting for gdb on localhost:1234 (run: gdb -x .gdbinit)"
fi

qemu-system-x86_64 "${QEMU_FLAGS[@]}"
