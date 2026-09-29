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
run_cmd "echo hello fat > /disk/new.txt"
run_cmd "echo line two >> /disk/new.txt"
run_cmd "cat /disk/new.txt"
run_cmd "echo long name ok > \"/disk/docs/Mixed Case Name.txt\""
run_cmd "ls /disk/docs"
run_cmd "cat /disk/numbers.txt > /disk/copy.txt" 30
run_cmd "wc /disk/copy.txt" 20
run_cmd "echo tiny > /disk/shrunk.txt"
run_cmd "cat /disk/numbers.txt > /disk/shrunk.txt" 30
run_cmd "echo tiny > /disk/shrunk.txt"
run_cmd "wc /disk/shrunk.txt"
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
check_line "hello fat"
check_line "line two"
check_re "^ +13  Mixed Case Name.txt$"
check_line "20000 20000 108894 /disk/copy.txt"
check_line "1 1 5 /disk/shrunk.txt"
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

# Files the guest wrote to the FAT disk must read back through mtools, and
# the volume must pass fsck (when dosfstools is installed).
host_check() { # host_check <description> <command...>
    local what=$1
    shift
    if "$@" >/dev/null 2>&1; then echo "PASS  host: $what"; else echo "FAIL  host: $what"; status=1; fi
}
fat_is() { # fat_is <path on the FAT disk> <file with the expected contents>
    mtype -i "$FATDISK" "::$1" | cmp -s - "$2"
}
printf 'hello fat\nline two\n' > smoke-expect-new.txt
printf 'long name ok\n' > smoke-expect-long.txt
seq 1 20000 > smoke-expect-copy.txt
printf 'tiny\n' > smoke-expect-tiny.txt
host_check "new.txt contents" fat_is /new.txt smoke-expect-new.txt
host_check "long name created" fat_is "/docs/Mixed Case Name.txt" smoke-expect-long.txt
host_check "copy.txt matches numbers.txt" fat_is /copy.txt smoke-expect-copy.txt
host_check "shrunk.txt truncated" fat_is /shrunk.txt smoke-expect-tiny.txt
rm -f smoke-expect-*.txt
if command -v fsck.fat >/dev/null; then
    host_check "fsck.fat finds no errors" fsck.fat -n "$FATDISK"
else
    echo "SKIP  host: fsck.fat (dosfstools not installed)"
fi

if [ $status -ne 0 ]; then
    echo "--- serial log ---"
    cat "$LOG"
fi

# Crash tests: boot a kernel told (via its command line) to crash on
# purpose, and check the panic report names the right functions.
crash_boot() { # crash_boot <kind>
    local conf=smoke-crash.conf log=smoke-crash.log
    printf 'timeout: 0\n/ATOS crash test\n    protocol: limine\n    kernel_path: boot():/boot/kernel.elf\n    module_path: boot():/boot/initrd.tar\n    cmdline: crashtest=%s\n' "$1" > "$conf"
    ISO=smoke-crash.iso ISO_ROOT=smoke-crash-root LIMINE_CONF=$conf ./tools/iso.sh >/dev/null 2>&1
    rm -f "$log"
    qemu-system-x86_64 -M q35 -m 256M -display none -no-reboot \
        -serial "file:$log" -cdrom smoke-crash.iso 2>/dev/null &
    local pid=$!
    for _ in $(seq 1 150); do
        grep -qF -- "--- system halted ---" "$log" 2>/dev/null && break
        sleep 0.2
    done
    kill $pid 2>/dev/null || true
    wait $pid 2>/dev/null || true
    LOG=$log
}
crash_status=$status
status=0
crash_boot pagefault
check "*** ATOS KERNEL PANIC ***"
check "unhandled exception 14 (Page Fault)"
check "faulting address: 0x0000000000000000"
check_re "\] crash_deref\+0x"
check_re "\] crash_level2\+0x"
check_re "\] crash_level1\+0x"
check_re "\] crashtest_task\+0x"
check "task: "
check "(crashtest)"
check "--- system halted ---"
[ $status -ne 0 ] && cat "$LOG"
crash_status=$(( crash_status | status )); status=0

crash_boot stackoverflow
check "unhandled exception 8 (Double Fault)"
check "likely cause: kernel stack overflow"
check_re "\] crash_recurse\+0x"
check_re "same frame [0-9]+ more time"
check_re "\] crashtest_task\+0x"
check "--- system halted ---"
[ $status -ne 0 ] && cat "$LOG"
crash_status=$(( crash_status | status )); status=0

crash_boot assert
check "assertion failed: value == 42 (src/kernel/crashtest.c:"
check_re "\] crash_assert\+0x"
check "--- system halted ---"
[ $status -ne 0 ] && cat "$LOG"
status=$(( crash_status | status ))
rm -rf smoke-crash.conf smoke-crash.iso smoke-crash-root smoke-crash.log
exit $status
