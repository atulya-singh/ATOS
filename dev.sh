#!/usr/bin/env bash
# Runs a command inside the ATOS dev container. macOS's native gcc is
# actually Apple clang (no elf64-x86-64 output), its ld is ld64 (no
# `-m elf_x86_64`), and it has neither xorriso nor qemu -- this container
# supplies a real x86_64-linux-gnu cross toolchain plus both, so the same
# build/run/debug workflow works regardless of host OS.
#
# Usage: ./dev.sh make        # build kernel.elf
#        ./dev.sh make run    # build + boot in QEMU (serial on stdio)
#        ./dev.sh make debug  # build + boot paused, waiting for gdb :1234
set -euo pipefail
cd "$(dirname "$0")"

IMAGE=atos-dev

if [ -z "$(docker images -q $IMAGE 2>/dev/null)" ]; then
    docker build -f Dockerfile.dev -t "$IMAGE" .
fi

docker run --rm -it \
    -v "$(pwd):/workspace" \
    -w /workspace \
    -e CC=x86_64-linux-gnu-gcc \
    -e LD=x86_64-linux-gnu-ld \
    -p 1234:1234 \
    "$IMAGE" "$@"
