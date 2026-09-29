#include "keyboard.h"
#include "../lib/ring.h"
#include "../lib/spinlock.h"
#include "../arch/x86_64/idt.h"
#include "../lib/io.h"
#include "../lib/kprintf.h"
#include "../sched/sched.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_STATUS_OUTPUT_FULL 0x01

/* Scancode set 1, which is what the 8042 controller hands us by default
 * (it translates the keyboard's native set 2). Index = make code. Keys
 * with no ASCII meaning map to 0 and are dropped. */
static const char keymap[128] = {
    0,    27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*',  0,   ' ',
};

static const char keymap_shift[128] = {
    0,    27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*',  0,   ' ',
};

#define SC_LSHIFT   0x2A
#define SC_RSHIFT   0x36
#define SC_CTRL     0x1D
#define SC_CAPSLOCK 0x3A
#define SC_EXTENDED 0xE0
#define SC_RELEASE  0x80

/* Scancodes become bytes in a lock-free ring: the IRQ handler (always on
 * the bootstrap CPU) is its single producer; readers, on any CPU, take
 * reader_lock to act as its single consumer. A full ring drops new keys
 * rather than overwriting ones the reader hasn't seen yet. */
static struct spsc_ring queue;
static struct spinlock reader_lock;
static struct wait_queue readers;

static int shift, ctrl, capslock, extended;

static void enqueue(char c) {
    if (ring_push(&queue, (uint8_t)c)) wait_queue_wake_all(&readers);
}

static void keyboard_irq(void) {
    uint8_t sc = inb(PS2_DATA);

    if (sc == SC_EXTENDED) {
        extended = 1;
        return;
    }
    /* E0-prefixed keys (arrows, right ctrl, keypad enter...) have no
     * mapping yet, except that right ctrl should still act as ctrl. */
    int was_extended = extended;
    extended = 0;

    int released = sc & SC_RELEASE;
    uint8_t code = sc & ~SC_RELEASE;

    if (code == SC_LSHIFT || code == SC_RSHIFT) { if (!was_extended) shift = !released; return; }
    if (code == SC_CTRL) { ctrl = !released; return; }
    if (released || was_extended) return;
    if (code == SC_CAPSLOCK) { capslock = !capslock; return; }

    char c = shift ? keymap_shift[code] : keymap[code];
    if (!c) return;
    if (capslock && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) c ^= 0x20;
    if (ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) c &= 0x1F; /* ^A.. ^Z */
    enqueue(c);
}

void keyboard_init(void) {
    /* Drain anything the firmware left in the output buffer: a stale byte
     * there keeps the controller from raising IRQ1 again. */
    while (inb(PS2_STATUS) & PS2_STATUS_OUTPUT_FULL) inb(PS2_DATA);
    irq_install_handler(1, keyboard_irq);
    kprintf("ATOS: PS/2 keyboard on IRQ1\n");
}

int keyboard_try_getc(void) {
    uint64_t flags = spin_lock_irqsave(&reader_lock);
    int c = ring_pop(&queue);
    spin_unlock_irqrestore(&reader_lock, flags);
    return c;
}

char keyboard_getc(void) {
    for (;;) {
        int c = keyboard_try_getc();
        if (c >= 0) return (char)c;
        wait_event(&readers, !ring_empty(&queue));
    }
}
