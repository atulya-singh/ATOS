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
MON=$(mktemp -u /tmp/atos-mon.XXXXXX)
rm -f "$LOG"

qemu-system-x86_64 -M q35 -m 256M -display none -no-reboot \
    -serial "file:$LOG" -monitor "unix:$MON,server,nowait" \
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

if [ $status -ne 0 ]; then
    echo "--- serial log ---"
    cat "$LOG"
fi
exit $status
