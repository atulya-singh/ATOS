#pragma once
#include <stdint.h>

/* Loads a static x86-64 ELF executable from the VFS into the (empty lower
 * half of the) address space `cr3`. Each PT_LOAD segment gets its own
 * pages with W and X taken from its p_flags, and .bss comes out zeroed.
 * On success sets *entry and *image_end (the page-aligned end of the
 * highest segment, where the heap can start). Pages mapped before a
 * failure stay in `cr3` for the caller's teardown. */
int elf_load(const char *path, uint64_t cr3, uint64_t *entry, uint64_t *image_end);
