#pragma once

/* Exercises kmalloc/kfree split + coalesce; call right after heap_init. */
void selftest_heap(void);

/* Kernel thread entry: echoes completed keyboard lines (keyboard test). */
void selftest_kbd_line_service(void *arg);

/* Spawns the scheduler/user-mode/disk self-test tasks. Call after every
 * long-lived service task exists, since it snapshots resource counts. */
void selftest_spawn(void);

/* Called from the idle loop; reports once all self-test tasks are reaped. */
void selftest_poll(void);
