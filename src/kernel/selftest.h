#pragma once

/* Exercises kmalloc/kfree split + coalesce; call right after heap_init. */
void selftest_heap(void);

/* Spawns the scheduler/user-mode/disk self-test tasks. Call after every
 * long-lived service task exists, since it snapshots resource counts. */
void selftest_spawn(void);

/* Called from the idle loop; reports once all self-test tasks are reaped,
 * and from then on returns 1 (0 while tests are still running). */
int selftest_poll(void);

/* Failed self-tests so far (final once selftest_poll has returned 1). */
int selftest_failures(void);
