/* Terminus 8x16 console font (PSF1 with a Unicode table), embedded as raw
 * bytes and parsed at runtime by fbcon.c. License: third_party/fonts/. */
.section .rodata
.global console_font_start, console_font_end
.balign 16
console_font_start:
    .incbin "third_party/fonts/terminus-8x16.psf"
console_font_end:

.section .note.GNU-stack,"",@progbits
