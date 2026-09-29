#include "fbcon.h"
#include "../lib/kprintf.h"
#include <limine.h>
#include <stddef.h>
#include <stdint.h>

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0,
};

extern const uint8_t console_font_start[], console_font_end[];

#define PSF1_MAGIC0 0x36
#define PSF1_MAGIC1 0x04
#define PSF1_MODE512    0x01
#define PSF1_MODEHASTAB 0x02
#define PSF1_SEPARATOR  0xFFFF
#define PSF1_STARTSEQ   0xFFFE
#define FONT_WIDTH 8

static uint8_t *fb;
static uint64_t fb_width, fb_height, fb_pitch;
static uint32_t fg_color, bg_color;

static const uint8_t *glyphs;
static unsigned glyph_height;
/* Font glyph index for each ASCII code. PSF fonts with a Unicode table
 * (like Terminus) store glyphs in their own order, not code point order. */
static uint16_t ascii_to_glyph[128];

static unsigned cols, rows, cur_col, cur_row;
static int enabled;
static const struct limine_framebuffer *fb_info;

static uint32_t pack_rgb(const struct limine_framebuffer *f, uint8_t r, uint8_t g, uint8_t b) {
    /* Trust the reported channel layout rather than assuming BGRX. */
    return ((uint32_t)r >> (8 - f->red_mask_size)) << f->red_mask_shift
         | ((uint32_t)g >> (8 - f->green_mask_size)) << f->green_mask_shift
         | ((uint32_t)b >> (8 - f->blue_mask_size)) << f->blue_mask_shift;
}

static int load_font(void) {
    const uint8_t *psf = console_font_start;
    size_t size = (size_t)(console_font_end - console_font_start);
    if (size < 4 || psf[0] != PSF1_MAGIC0 || psf[1] != PSF1_MAGIC1) return 0;

    uint8_t mode = psf[2];
    glyph_height = psf[3];
    unsigned count = (mode & PSF1_MODE512) ? 512 : 256;
    glyphs = psf + 4;
    if (4 + (size_t)count * glyph_height > size) return 0;

    for (unsigned c = 0; c < 128; c++) ascii_to_glyph[c] = (uint16_t)(c < count ? c : 0);
    if (!(mode & PSF1_MODEHASTAB)) return 1;

    /* Unicode table: per glyph, a list of little-endian UCS-2 code points
     * ended by 0xFFFF. 0xFFFE opens multi-code-point sequences, which a
     * single ASCII byte can never match, so skip to the glyph's end. */
    for (unsigned c = 0; c < 128; c++) ascii_to_glyph[c] = (uint16_t)'?';
    const uint8_t *p = glyphs + (size_t)count * glyph_height;
    const uint8_t *end = console_font_end;
    for (unsigned g = 0; g < count && p + 1 < end; g++) {
        int in_seq = 0;
        for (; p + 1 < end; p += 2) {
            uint16_t cp = (uint16_t)(p[0] | (p[1] << 8));
            if (cp == PSF1_SEPARATOR) { p += 2; break; }
            if (cp == PSF1_STARTSEQ) in_seq = 1;
            if (!in_seq && cp < 128) ascii_to_glyph[cp] = (uint16_t)g;
        }
    }
    return 1;
}

static void fill_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint32_t color) {
    for (uint64_t row = y; row < y + h; row++) {
        uint32_t *px = (uint32_t *)(fb + row * fb_pitch) + x;
        for (uint64_t i = 0; i < w; i++) px[i] = color;
    }
}

static void draw_glyph(unsigned col, unsigned row, char c) {
    unsigned char uc = (unsigned char)c;
    const uint8_t *g = glyphs + (size_t)ascii_to_glyph[uc < 128 ? uc : '?'] * glyph_height;
    uint64_t x = (uint64_t)col * FONT_WIDTH, y = (uint64_t)row * glyph_height;
    for (unsigned gy = 0; gy < glyph_height; gy++) {
        uint32_t *px = (uint32_t *)(fb + (y + gy) * fb_pitch) + x;
        uint8_t bits = g[gy];
        for (unsigned gx = 0; gx < FONT_WIDTH; gx++) {
            px[gx] = (bits & (0x80 >> gx)) ? fg_color : bg_color;
        }
    }
}

/* Moves every text row up by one. Uses qword `rep movsq` (the byte-wise
 * libk memmove is far too slow for multiple megabytes of framebuffer),
 * so the pitch is assumed to be a multiple of 8, which holds for 32 bpp. */
static void scroll(void) {
    uint64_t line_bytes = fb_pitch * glyph_height;
    uint64_t move_bytes = line_bytes * (rows - 1);
    void *dst = fb, *src = fb + line_bytes;
    uint64_t qwords = move_bytes / 8;
    asm volatile("rep movsq" : "+D"(dst), "+S"(src), "+c"(qwords) : : "memory");
    fill_rect(0, (uint64_t)(rows - 1) * glyph_height, fb_width, glyph_height, bg_color);
}

static void newline(void) {
    cur_col = 0;
    if (++cur_row == rows) {
        scroll();
        cur_row = rows - 1;
    }
}

void fbcon_putc(char c) {
    if (!enabled) return;
    switch (c) {
    case '\n':
        newline();
        return;
    case '\r':
        cur_col = 0;
        return;
    case '\b':
        if (cur_col) draw_glyph(--cur_col, cur_row, ' ');
        return;
    case '\t':
        do fbcon_putc(' '); while (cur_col % 4);
        return;
    }
    draw_glyph(cur_col, cur_row, c);
    if (++cur_col == cols) newline();
}

void fbcon_init(void) {
    struct limine_framebuffer_response *resp = framebuffer_request.response;
    if (!resp || resp->framebuffer_count == 0) return;
    struct limine_framebuffer *f = resp->framebuffers[0];
    if (f->bpp != 32 || f->memory_model != LIMINE_FRAMEBUFFER_RGB) return;
    if (!load_font()) return;

    fb_info = f;
    fb = f->address;
    fb_width = f->width;
    fb_height = f->height;
    fb_pitch = f->pitch;
    fg_color = pack_rgb(f, 0xD0, 0xD0, 0xD0);
    bg_color = pack_rgb(f, 0x10, 0x10, 0x18);

    cols = (unsigned)(fb_width / FONT_WIDTH);
    rows = (unsigned)(fb_height / glyph_height);
    cur_col = cur_row = 0;
    fill_rect(0, 0, fb_width, fb_height, bg_color);
    enabled = 1;
    kprintf("ATOS: framebuffer console %lux%lu (%ux%u text)\n", fb_width, fb_height, cols, rows);
}

void fbcon_panic_screen(void) {
    if (!enabled) return;
    fg_color = pack_rgb(fb_info, 0xFF, 0xFF, 0xFF);
    bg_color = pack_rgb(fb_info, 0x80, 0x10, 0x10);
    fill_rect(0, 0, fb_width, fb_height, bg_color);
    cur_col = cur_row = 0;
}
