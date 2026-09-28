#pragma once

/* Unmasks IRQ1 and starts queuing decoded keystrokes. */
void keyboard_init(void);

/* Blocks the calling task until a character is available. */
char keyboard_getc(void);

/* Returns the next character, or -1 immediately if the queue is empty. */
int keyboard_try_getc(void);
