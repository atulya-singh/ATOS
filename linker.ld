OUTPUT_FORMAT(elf64-x86-64)
ENTRY(kmain)

PHDRS
{
    limine_requests PT_LOAD FLAGS(6);
    text            PT_LOAD FLAGS(5);
    rodata          PT_LOAD FLAGS(4);
    data            PT_LOAD FLAGS(6);    
}

SECTIONS
{
    . = 0xffffffff80000000;

    /* Everything from here through kernel_end is what the VMM maps as the
     * kernel image (see src/kernel/mm/vmm.c). kernel_image_start covers
     * .limine_requests too, since boot_info.c re-reads Limine's response
     * pointers from that section, and it must stay mapped after we switch
     * to our own page tables. */
    kernel_image_start = .;

    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :limine_requests

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    kernel_text_start = .;
    .text : {
        *(.text .text.*)
    } : text
    kernel_text_end = .;

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    kernel_rodata_start = .;
    .rodata : {
        *( .rodata .rodata*)
    } :rodata

    .eh_frame : {
        *(.eh_frame)
        *(.eh_frame_hdr)
    } :rodata

    .note.gnu.build-id : {
        *(.note.gnu.build-id)
    } :rodata
    kernel_rodata_end = .;

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    kernel_data_start = .;
    .data : {
        *(.data .data*)
    } :data

    .init_array : {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array .ctors))
        __init_array_end = .;
    } :data

    .fini_array : {
        __fini_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP(*(.fini_array .dtors))
        __fini_array_end = .;
    } :data

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data
    kernel_end = .;

    /DISCARD/ : {
        *(.note .note.*)
    }
}
