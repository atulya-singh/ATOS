#!/bin/sh
# Emits the kernel symbol table (every function's address and name, sorted
# by address) as assembly, for panic backtraces.
# Usage: gensyms.sh <nm> <kernel.elf>   ("-" instead of an ELF: empty table)
#
# The kernel is linked twice: first with an empty table, then with the
# table generated from that first link. The table lands at the very end of
# .rodata, after all of .text, so function addresses are the same in both.
set -eu
NM=$1
ELF=$2

echo '    .section .note.GNU-stack, "", @progbits'
echo '    .section .rodata.zz_ksyms, "a"'
echo '    .balign 8'
echo '    .global ksyms_table'
echo '    .global ksyms_count'
if [ "$ELF" = - ]; then
    echo 'ksyms_count: .quad 0'
    echo 'ksyms_table:'
    exit 0
fi
"$NM" -n --defined-only "$ELF" | awk '
    BEGIN { n = 0 }
    $2 == "t" || $2 == "T" { addr[n] = $1; name[n] = $3; n++ }
    END {
        printf "ksyms_count: .quad %d\n", n
        print "ksyms_table:"
        for (i = 0; i < n; i++) printf "    .quad 0x%s, .Lks%d\n", addr[i], i
        for (i = 0; i < n; i++) printf ".Lks%d: .asciz \"%s\"\n", i, name[i]
    }'
