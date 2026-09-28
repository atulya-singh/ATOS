#!/usr/bin/env bash
# Boots atos.iso in QEMU with serial routed to stdio -- this, not the
# framebuffer, is the fast debug loop (see IMPLEMENTATION_PLAN.md).
# Pass --debug to also pause at the first instruction and wait for gdb on
# localhost:1234 (see .gdbinit).
set -euo pipefail
cd "$(dirname "$0")/.."

ISO=atos.iso
QEMU_FLAGS=(-M q35 -m 256M -serial stdio -no-reboot -no-shutdown -cdrom "$ISO")

if [ "${1:-}" = "--debug" ]; then
    QEMU_FLAGS+=(-s -S)
    echo "QEMU is waiting for gdb on localhost:1234 (run: gdb -x .gdbinit)"
fi

qemu-system-x86_64 "${QEMU_FLAGS[@]}"
