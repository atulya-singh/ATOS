#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../fs/vfs.h"

enum task_state {
    TASK_READY,    /* runnable -- including the one currently running */
    TASK_SLEEPING, /* waiting for pit ticks to reach wake_tick */
    TASK_BLOCKED,  /* parked on a wait_queue until something wakes it */
    TASK_ZOMBIE,   /* exited; stack and memory freed by the next schedule() */
    TASK_DEAD,     /* resources freed; struct kept only for a parent's waitpid */
};

/* A list of tasks blocked on some event (input arriving, I/O finishing).
 * Zero-initialized is empty. */
struct wait_queue {
    struct task *head;
};

struct task {
    uint64_t id;
    char name[16];
    enum task_state state;

    uint64_t rsp;        /* saved kernel RSP while switched out (see switch.S) */
    uint64_t cr3;        /* physical address of this task's PML4 */
    uint64_t kstack_top; /* loaded into TSS.rsp0 whenever this task runs */
    int kstack_slot;     /* -1 for the idle task, which runs on the boot stack */

    uint64_t user_rip, user_rsp; /* ring 3 entry point; unused by kernel threads */
    uint64_t brk_start, brk;     /* process heap: [brk_start, brk), grown by sys_brk */

    uint64_t wake_tick;
    int slice;           /* ticks left in the current time slice */

    struct file *fds[MAX_FDS]; /* see fd_* in fs/vfs.h; kernel threads have none */

    /* Process relationships. A task with a parent is kept (as TASK_DEAD)
     * after exiting until that parent collects exit_code with waitpid; one
     * without is freed outright. A parent that exits first orphans its
     * children, which are then freed as soon as they exit. */
    struct task *parent;
    int exit_code;
    struct wait_queue child_exited;

    struct task *next;   /* circular list of every task, in round-robin order */
    struct task *wait_next; /* link while parked on a wait_queue */
};

/* Turns the currently running boot context into the idle task (id 0),
 * which runs only when nothing else is runnable. Call once, before the
 * first task_create and before interrupts are enabled. */
void sched_init(void);

/* Creates a kernel thread running entry(arg). Returning from entry exits
 * the thread with code 0. Returns NULL if out of memory. */
struct task *task_create_kernel(const char *name, void (*entry)(void *), void *arg);

/* Creates a ring-3 task in a fresh address space, with `code` copied to
 * USER_CODE_BASE (read + execute) and a writable, non-executable stack
 * below USER_STACK_TOP. `code` must be position-independent. fds 0-2 are
 * opened on /dev/console. */
struct task *task_create_user(const char *name, const void *code, size_t code_size);

/* Creates a ring-3 task that enters `rip` with `rsp` in the already
 * populated address space `cr3`, taking ownership of it. It is NOT yet
 * runnable: finish setting it up (fds, heap bounds), then task_start it.
 * Returns NULL if out of memory (the address space is then left to the
 * caller). */
struct task *task_create_user_space(const char *name, uint64_t cr3, uint64_t rip, uint64_t rsp);

/* Puts a fully set-up task on the run queue. */
void task_start(struct task *t);

struct registers;
/* Duplicates the calling user process: a deep copy of its address space,
 * its fds, and its heap bounds. The child resumes from the same syscall
 * frame (`regs`) with %rax = 0. Returns the child, or NULL on OOM. */
struct task *task_fork(const struct registers *regs);

/* Waits for a child (pid -1: any child) to exit and returns its pid, with
 * its exit code in *code. -ECHILD if there is no such child. */
int64_t task_wait(int64_t pid, int *code);

/* Points fds 0-2 of `t` at one shared open file on /dev/console. */
void task_open_console_fds(struct task *t);

#define USER_CODE_BASE  0x0000000000400000ULL
#define USER_STACK_TOP  0x00007FFFFFFFF000ULL
#define USER_STACK_SIZE (16 * 1024ULL)

struct task *sched_current(void);
void task_set_name(struct task *t, const char *name);
/* Number of tasks that still hold resources (includes idle and unreaped zombies). */
uint64_t sched_task_count(void);

/* Called from the timer IRQ after EOI; preempts when the slice runs out. */
void sched_tick(void);

void sched_yield(void);
void task_sleep(uint64_t ticks);
__attribute__((noreturn)) void task_exit(int code);

/* Blocks the current task on `wq` until wait_queue_wake_all. Call with
 * interrupts disabled, *after* re-checking the condition under that same
 * disabled section: that ordering is what rules out a lost wakeup when
 * the event fires between the check and the sleep. Returns with
 * interrupts still disabled; callers loop, since waking is a hint and
 * the condition may already be consumed again. */
void wait_queue_sleep(struct wait_queue *wq);
/* Makes every waiter runnable. Safe from IRQ context. */
void wait_queue_wake_all(struct wait_queue *wq);

/* Sleeping lock for sections that may block or run long (device I/O).
 * Zero-initialized is unlocked. Task context only: never from an IRQ. */
struct mutex {
    int locked;
    struct wait_queue waiters;
};

void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);
