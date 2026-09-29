#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/fpu.h"
#include "../fs/vfs.h"

enum task_state {
    TASK_READY,    /* runnable -- including while running on some CPU */
    TASK_SLEEPING, /* waiting for timer ticks to reach wake_tick */
    TASK_BLOCKED,  /* parked on a wait_queue until something wakes it */
    TASK_ZOMBIE,   /* exited; its stack and memory not yet freed by the reaper */
    TASK_DEAD,     /* resources freed; struct kept only for a parent's waitpid */
};

/* A set of tasks blocked on some event (input arriving, a lock freeing
 * up). Zero-initialized is empty. See wait_event. */
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
    int kstack_slot;     /* -1 for the BSP's idle task, which runs on the boot stack */

    /* Set while some CPU is running this task, or still standing on its
     * stack mid-switch; no other CPU may pick it (or free it) until the
     * switch away has completed. */
    int on_cpu;
    int is_idle;         /* a per-CPU idle task: never on the run list */
    int reaping;         /* the reaper is freeing this zombie's resources */

    uint64_t user_rip, user_rsp; /* ring 3 entry point; unused by kernel threads */
    uint64_t brk_start, brk;     /* process heap: [brk_start, brk), grown by sys_brk */

    /* User x87/SSE registers while switched out (see schedule()). */
    struct fpu_state fpu;

    uint64_t wake_tick;
    int slice;           /* ticks left in the current time slice */

    struct file *fds[MAX_FDS]; /* see fd_* in fs/vfs.h; kernel threads have none */

    /* Process relationships. A task with a parent is kept (as TASK_DEAD)
     * after exiting until that parent collects exit_code with waitpid; one
     * without is freed outright. A parent that exits first orphans its
     * children, which are then freed as soon as they exit. */
    struct task *parent;
    int exit_code;
    uint64_t child_events;       /* bumped whenever a child exits */
    struct wait_queue child_exited;

    struct task *next;           /* circular run list of every non-idle task */
    struct wait_queue *waiting_on;
    struct task *wait_next;      /* link while parked on a wait_queue */
};

/* Turns the running boot context into CPU 0's idle task (id 0) and starts
 * the reaper thread. Call once, before the first task_create. */
void sched_init(void);

/* An idle task for CPU `cpu_index`, with its own stack but no entry point:
 * the AP moves onto that stack and becomes the task itself. */
struct task *task_create_idle(unsigned cpu_index);

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

/* The task running on the calling CPU. */
static inline struct task *sched_current(void) {
    struct task *t;
    asm volatile("mov %%gs:8, %0" : "=r"(t)); /* struct cpu's `current` */
    return t;
}

void task_set_name(struct task *t, const char *name);
/* Number of tasks that still hold resources (includes idle tasks and
 * tasks the reaper hasn't freed yet). */
uint64_t sched_task_count(void);

/* Called from each CPU's timer interrupt after EOI; preempts when the
 * slice runs out. */
void sched_tick(void);

void sched_yield(void);
void task_sleep(uint64_t ticks);
__attribute__((noreturn)) void task_exit(int code);

/* Called by every task right after it is switched to for the first time
 * (task_trampoline, fork_return); see schedule(). */
void sched_finish_switch(void);

/* --- blocking ---
 * The race-free way to sleep until `cond` holds: register as a waiter
 * *before* the final check of the condition, so a wakeup that lands in
 * between turns the sleep into a no-op instead of being lost. Interrupts
 * stay off on this CPU throughout, so a timer tick can't put the task to
 * sleep while it is registered but has already seen the condition hold.
 * `cond` must become true only before a wait_queue_wake_all on `wq`. */
#define wait_event(wq, cond)                     \
    do {                                         \
        uint64_t wait_flags_ = irq_save();       \
        while (!(cond)) {                        \
            wait_prepare(wq);                    \
            if (!(cond)) wait_sleep();           \
            wait_finish(wq);                     \
        }                                        \
        irq_restore(wait_flags_);                \
    } while (0)

void wait_prepare(struct wait_queue *wq);
void wait_sleep(void);
void wait_finish(struct wait_queue *wq);
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
