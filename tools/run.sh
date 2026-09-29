#!/usr/bin/env bash
# Boots atos.iso in QEMU with serial routed to stdio -- this, not the
# framebuffer, is the fast debug loop (see IMPLEMENTATION_PLAN.md).
# Pass --debug to also pause at the first instruction and wait for gdb on
# localhost:1234 (see .gdbinit).
set -euo pipefail
cd "$(dirname "$0")/.."

ISO=atos.iso
DISK=disk.img

# Persistent FAT32 disk, mounted at /disk. Delete it to start over.
if [ ! -f "$DISK" ]; then
    ./tools/mkfatdisk.sh "$DISK"
fi

# disable-legacy=off: the drivers speak the legacy virtio PCI interface.
# User-mode networking: the guest is 10.0.2.15 behind QEMU's NAT, and
# host port 8080 forwards to the guest's port 80 (try httpd in the guest,
# then curl localhost:8080/README on the host).
QEMU_FLAGS=(-M q35 -smp 4 -m 256M -serial stdio -no-reboot -cdrom "$ISO"
            -drive "file=$DISK,format=raw,if=none,id=disk0"
            -device virtio-blk-pci,drive=disk0,disable-legacy=off
            -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:8080-:80"
            -device virtio-net-pci,netdev=net0,disable-legacy=off)

if [ "${1:-}" = "--debug" ]; then
    QEMU_FLAGS+=(-s -S)
    echo "QEMU is waiting for gdb on localhost:1234 (run: gdb -x .gdbinit)"
fi

qemu-system-x86_64 "${QEMU_FLAGS[@]}"
