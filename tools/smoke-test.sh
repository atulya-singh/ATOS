#!/usr/bin/env bash
# Headless boot test: boots atos.iso, checks the serial log for every
# boot-time self-test's success line, then types commands into the shell
# through QEMU's monitor (sendkey) and checks their output.
# Used by CI and runnable locally via ./dev.sh tools/smoke-test.sh.
# Needs only qemu + perl, both present on the CI runner and in the
# dev container.
set -euo pipefail
cd "$(dirname "$0")/.."

LOG=serial.log
DISK=smoke-disk.img
FATDISK=smoke-fat.img
MON=$(mktemp -u /tmp/atos-mon.XXXXXX)
rm -f "$LOG"

# Fresh scratch disk each run: zeros plus a signature the kernel checks.
dd if=/dev/zero of="$DISK" bs=1M count=4 status=none
printf 'ATOSDISK' | dd of="$DISK" conv=notrunc status=none
./tools/mkfatdisk.sh "$FATDISK"

qemu-system-x86_64 -M q35 -m 256M -display none -no-reboot \
    -serial "file:$LOG" -monitor "unix:$MON,server,nowait" \
    -drive "file=$DISK,format=raw,if=none,id=disk0" \
    -device virtio-blk-pci,drive=disk0,disable-legacy=off \
    -drive "file=$FATDISK,format=raw,if=none,id=disk1" \
    -device virtio-blk-pci,drive=disk1,disable-legacy=off \
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

monitor() { # monitor <hmp command>...  (one connection, all commands)
    perl -MIO::Socket::UNIX -e '
        my $s = IO::Socket::UNIX->new(Peer => shift) or die "monitor: $!\n";
        for (@ARGV) {
            print $s "$_\n";
            select(undef, undef, undef, 0.03); # key-repeat-ish pacing
        }
        select(undef, undef, undef, 0.1); # let QEMU act before we hang up
    ' "$MON" "$@"
}

# Types a line into the guest's keyboard, then Enter.
type_line() {
    local s="$1" keys=() c i
    for (( i = 0; i < ${#s}; i++ )); do
        c="${s:i:1}"
        case "$c" in
            [a-z0-9]) keys+=("sendkey $c") ;;
            [A-Z])    keys+=("sendkey shift-$(printf '%s' "$c" | tr A-Z a-z)") ;;
            ' ')      keys+=("sendkey spc") ;;
            /)        keys+=("sendkey slash") ;;
            .)        keys+=("sendkey dot") ;;
            -)        keys+=("sendkey minus") ;;
            '>')      keys+=("sendkey shift-dot") ;;
            '"')      keys+=("sendkey shift-apostrophe") ;;
            '!')      keys+=("sendkey shift-1") ;;
            *) echo "type_line: no key mapping for '$c'" >&2; return 1 ;;
        esac
    done
    monitor "${keys[@]}" "sendkey ret"
}

prompts() { tr -d '\r' < "$LOG" | { grep -o 'atos\$ ' || true; } | wc -l; }

# Types a command once the shell is at a fresh prompt, and waits for the
# next prompt (i.e. the command finished) before returning.
run_cmd() { # run_cmd <command line> [seconds]
    local before
    before=$(prompts)
    type_line "$1"
    for _ in $(seq 1 $(( ${2:-10} * 5 ))); do
        [ "$(prompts)" -gt "$before" ] && return 0
        sleep 0.2
    done
    echo "note: no prompt after '$1'" >&2
}

status=0
check() { # check <text>: some line contains it
    if grep -qF -- "$1" "$LOG"; then echo "PASS  $1"; else echo "FAIL  $1"; status=1; fi
}
check_line() { # check_line <text>: some line is exactly it
    if tr -d '\r' < "$LOG" | grep -qxF -- "$1"; then echo "PASS  line: $1"; else echo "FAIL  line: $1"; status=1; fi
}
check_re() { # check_re <extended regex>: some line matches it
    if tr -d '\r' < "$LOG" | grep -qE -- "$1"; then echo "PASS  re: $1"; else echo "FAIL  re: $1"; status=1; fi
}
check_absent() { # check_absent <text>
    if grep -qF -- "$1" "$LOG"; then echo "FAIL  absent: $1"; status=1; else echo "PASS  absent: $1"; fi
}

wait_for "all tasks reaped" 40 || true
wait_for "atos\$ " 20 || true

run_cmd "echo hi from the shell"
run_cmd "libctest one two" 20
run_cmd "ls /bin"
run_cmd "cat /README"
run_cmd "nosuchcmd"
run_cmd "echo nope > /README"
run_cmd "ls /disk"
run_cmd "cat /disk/HELLO.TXT"
run_cmd "cat \"/disk/docs/a long file name.txt\""
run_cmd "wc /disk/numbers.txt" 20
run_cmd "exit 3"

check "framebuffer console"
check "PMM:"
check "VMM: page tables built"
check "heap self-test: alloc/free/coalesce ok"
check "8086:29c0 class 06.00.00 host bridge"
check "functions enumerated"
check "entering idle loop"
check "preemption ok"
check "user: kernel pointer rejected with -EFAULT"
check "user: hello from ring 3!"
check "(user-hello) exited with code 42"
check "(user-fault) killed: Page Fault"
check "(user-spin) exited with code 7"
check "all tasks reaped cleanly"
check "vfs: hello through /dev/console"
check "vfs self-test ok"
check "initrd self-test ok"
check "self-tests done, starting /bin/init"
check_line "Welcome to ATOS!"
check_line "hi from the shell"
check_line "libctest: argv ok"
check_line "libctest: printf formatting ok"
check_line "libctest: malloc/free over brk ok"
check_line "libctest: errno ok"
check_line "libctest: fork/waitpid/getpid ok"
check_line "libctest: exec argv passing ok"
check_line "libctest: exec errors ok"
check_line "libctest: write to .text kills the process ok"
check_line "libctest: dup2 redirection ok"
check_line "libctest: 0 failure(s)"
check_absent "this line must not appear"
check_re "^ +[0-9]+  echo$"
check_re "^ +[0-9]+  libctest$"
check_line "This file lives in the initrd, which is read-only."
check_line "sh: nosuchcmd: no such file or directory"
check_line "sh: /README: read-only file system"
check_line "init: shell exited with code 3, restarting"
check "fat32: vdb mounted at /disk"
check_re "^ +25  hello.txt$"
check_re "^ +<dir>  docs/$"
check_re "^ +108894  NUMBERS.TXT$"
check_line "Hello from a FAT32 disk!"
check_line "Long file names work."
check_line "20000 20000 108894 /disk/numbers.txt"
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
