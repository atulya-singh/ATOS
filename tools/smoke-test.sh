#!/usr/bin/env bash
# Headless boot test: boots atos.iso, drives it through QEMU's monitor
# (keystrokes now; screenshots/disks as later phases need them), and
# checks the serial log for every boot-time self-test's success line.
# Used by CI and runnable locally via ./dev.sh tools/smoke-test.sh.
# Needs only qemu + perl, both present on the CI runner and in the
# dev container.
set -euo pipefail
cd "$(dirname "$0")/.."

LOG=serial.log
DISK=smoke-disk.img
MON=$(mktemp -u /tmp/atos-mon.XXXXXX)
rm -f "$LOG"

# Fresh scratch disk each run: zeros plus a signature the kernel checks.
dd if=/dev/zero of="$DISK" bs=1M count=4 status=none
printf 'ATOSDISK' | dd of="$DISK" conv=notrunc status=none

qemu-system-x86_64 -M q35 -m 256M -display none -no-reboot \
    -serial "file:$LOG" -monitor "unix:$MON,server,nowait" \
    -drive "file=$DISK,format=raw,if=none,id=disk0" \
    -device virtio-blk-pci,drive=disk0,disable-legacy=off \
    -cdrom atos.iso &
QEMU_PID=$!
trap 'kill $QEMU_PID 2>/dev/null || true; rm -f "$MON"' EXIT

wait_for() { # wait_for <text> <seconds>
    for _ in $(seq 1 $(( $2 * 5 ))); do
        grep -qF -- "$1" "$LOG" 2>/dev/null && return 0
        sleep 0.2
    done
    return 1
}

monitor() { # monitor <hmp command>
    perl -MIO::Socket::UNIX -e '
        my $s = IO::Socket::UNIX->new(Peer => $ARGV[0]) or die "monitor: $!\n";
        print $s "$ARGV[1]\n";
        select(undef, undef, undef, 0.1); # let QEMU act before we hang up
    ' "$MON" "$1"
}

status=0
check() { # check <text>
    if grep -qF -- "$1" "$LOG"; then echo "PASS  $1"; else echo "FAIL  $1"; status=1; fi
}

wait_for "entering idle loop" 20 || true

# Keyboard: type "hi!" + Enter; the kbd-line service should echo the line.
for key in h i shift-1 ret; do monitor "sendkey $key"; done

wait_for "all tasks reaped" 30 || true
wait_for "(alive)" 20 || true

check "framebuffer console"
check "PMM:"
check "VMM: page tables built"
check "heap self-test: alloc/free/coalesce ok"
check "8086:29c0 class 06.00.00 host bridge"
check "functions enumerated"
check "entering idle loop"
check "(alive)"
check "preemption ok"
check "user: kernel pointer rejected with -EFAULT"
check "user: hello from ring 3!"
check "(user-hello) exited with code 42"
check "(user-fault) killed: Page Fault"
check "(user-spin) exited with code 7"
check "all tasks reaped cleanly"
check "keyboard line: hi!"
check "block: registered vda (8192 sectors, 4 MiB)"
check "disk self-test: signature + 160-sector write/readback ok"

# The guest's writes must have actually reached the disk image. Stop QEMU
# first so everything is flushed, then recompute the pattern host-side.
kill $QEMU_PID 2>/dev/null || true
wait $QEMU_PID 2>/dev/null || true
if perl -e '
    open(my $f, "<:raw", $ARGV[0]) or die; seek($f, 64 * 512, 0);
    read($f, my $d, 160 * 512) == 160 * 512 or exit 1;
    for my $i (0 .. length($d) - 1) {
        exit 1 if ord(substr($d, $i, 1)) != ((($i * 7) ^ ($i >> 9)) & 0xFF);
    }' "$DISK"; then
    echo "PASS  host sees guest-written pattern in $DISK"
else
    echo "FAIL  host sees guest-written pattern in $DISK"
    status=1
fi

if [ $status -ne 0 ]; then
    echo "--- serial log ---"
    cat "$LOG"
fi
exit $status
