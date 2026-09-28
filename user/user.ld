/* Layout for ATOS userspace programs: static, non-PIE, loaded at 4 MiB.
 * Each segment starts on its own page so the kernel's ELF loader can give
 * code R-X, read-only data R--, and data/bss RW- with no page shared
 * between two permission sets. */
ENTRY(_start)

PHDRS {
    text   PT_LOAD FLAGS(5); /* R-X */
    rodata PT_LOAD FLAGS(4); /* R-- */
    data   PT_LOAD FLAGS(6); /* RW- */
}

SECTIONS {
    . = 0x400000;

    .text : {
        *(.text._start)
        *(.text .text.*)
    } :text

    /* Plain location-counter alignment, not ALIGN() on the sections: an
     * empty output section is discarded along with its alignment, which
     * would slide the next segment into the previous one's last page. */
    . = ALIGN(4096);
    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(4096);
    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        *(COMMON)
        *(.bss .bss.*)
    } :data

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.comment)
    }
}
