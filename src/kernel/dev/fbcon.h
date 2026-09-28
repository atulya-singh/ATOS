#pragma once

/* Text console on the Limine-provided framebuffer. Safe to call before the
 * VMM switch: the framebuffer sits in the HHDM, which our own page tables
 * reproduce. Leaves the console disabled (fbcon_putc a no-op) if there's
 * no usable 32-bpp framebuffer. */
void fbcon_init(void);
void fbcon_putc(char c);
