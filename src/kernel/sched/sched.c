#include "sched.h"
#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/gdt.h"
#include "../arch/x86_64/idt.h"
#include "../arch/x86_64/percpu.h"
#include "../arch/x86_64/smp.h"
#include "../dev/timer.h"
#include "../lib/kprintf.h"
#include "../lib/spinlock.h"
#include "../lib/string.h"
#include "../mm/boot_info.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"

/* 50 ms at 100 Hz: short enough to feel interactive, long enough that
 * switching overhead stays negligible. */
#define TIME_SLICE_TICKS 5

/* Kernel stacks live in their own region, one fixed 32 KiB slot per task.
 * Only the top 16 KiB of each slot is mapped, so the unmapped bottom half
 * acts as a guard: overflowing a stack page-faults immediately, and that
 * escalates to #DF on its own IST stack, instead of silently corrupting
 * whatever sits next to it in memory. */
#define KSTACK_REGION     0xFFFFB00000000000ULL
#define KSTACK_SLOT_SIZE  (32 * 1024ULL)
#define KSTACK_SIZE       (16 * 1024ULL)
#define KSTACK_PAGES      (KSTACK_SIZE / PAGE_SIZE)
#define MAX_TASKS         64

/* One lock for all scheduler state: the run list, every task's state,
 * on_cpu, and wait-queue links. Always taken with interrupts off, since
 * the timer interrupt takes it too.
 *
 * It is held *across* context switches: the task that calls schedule()
 * takes it, and whichever task the CPU switches to releases it (after
 * sched_finish_switch marks the previous task as no longer on the CPU).
 * That hand-off is what stops another CPU from picking up, or the reaper
 * from freeing, a task whose stack is still in use mid-switch. */
static struct spinlock sched_lock;

extern void context_switch(uint64_t *old_rsp, uint64_t new_rsp, uint64_t new_cr3);
extern void task_trampoline(void);
extern void fork_return(void);
extern __attribute__((noreturn)) void jump_to_user(uint64_t rip, uint64_t rsp);

static struct task bsp_idle;
static struct task *rr_cursor; /* the run list's last-picked task; NULL if empty */
static uint64_t next_id = 1;
static uint64_t task_count; /* every task holding resources, idle ones included */
static uint64_t list_len;   /* tasks on the run list */
static uint8_t kstack_slot_used[MAX_TASKS];

static struct wait_queue reaper_wq;
static uint64_t reaper_work; /* bumped whenever there may be something to free */

static void reaper(void *arg);

void sched_init(void) {
    memcpy(bsp_idle.name, "idle0", 6);
    bsp_idle.id = 0;
    bsp_idle.state = TASK_READY;
    bsp_idle.cr3 = vmm_kernel_cr3();
    bsp_idle.kstack_slot = -1;
    bsp_idle.is_idle = 1;
    bsp_idle.on_cpu = 1;
    this_cpu()->current = &bsp_idle;
    this_cpu()->idle = &bsp_idle;
    task_count = 1;

    /* Fixed up front, so allocating and freeing stacks later never touches
     * page-table pages -- which also keeps the boot-time leak check exact. */
    vmm_prealloc_tables(KSTACK_REGION, MAX_TASKS * KSTACK_SLOT_SIZE);

    task_create_kernel("reaper", reaper, NULL);
    kprintf("ATOS: scheduler: round-robin over all CPUs, %d-tick slices\n", TIME_SLICE_TICKS);
}

uint64_t sched_task_count(void) { return __atomic_load_n(&task_count, __ATOMIC_RELAXED); }

static uint64_t kstack_base(int slot) {
    return KSTACK_REGION + (uint64_t)slot * KSTACK_SLOT_SIZE + (KSTACK_SLOT_SIZE - KSTACK_SIZE);
}

/* Unmaps and frees a stack. Unless the stack never ran (`used` = 0), other
 * CPUs may still cache its translations, so they are shot down before the
 * pages go back to the allocator -- which requires interrupts on and no
 * spinlock held (see tlb_shootdown). */
static void kstack_free(int slot, int used) {
    uint64_t base = kstack_base(slot);
    uint64_t phys[KSTACK_PAGES];
    for (uint64_t i = 0; i < KSTACK_PAGES; i++) {
        phys[i] = 0;
        if (vmm_translate(base + i * PAGE_SIZE, &phys[i])) vmm_unmap(base + i * PAGE_SIZE);
    }
    if (used) tlb_shootdown(base, KSTACK_SIZE);
    for (uint64_t i = 0; i < KSTACK_PAGES; i++) {
        if (phys[i]) pmm_free_page(phys[i]);
    }
    __atomic_store_n(&kstack_slot_used[slot], 0, __ATOMIC_RELEASE);
}

/* Returns the slot index, or -1 if out of slots or memory. */
static int kstack_alloc(void) {
    int slot = -1;
    for (int i = 0; i < MAX_TASKS && slot < 0; i++) {
        if (!__atomic_exchange_n(&kstack_slot_used[i], 1, __ATOMIC_ACQUIRE)) slot = i;
    }
    if (slot < 0) return -1;

    uint64_t base = kstack_base(slot);
    for (uint64_t off = 0; off < KSTACK_SIZE; off += PAGE_SIZE) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            kstack_free(slot, 0);
            return -1;
        }
        vmm_map(base + off, phys, VMM_PRESENT | VMM_WRITABLE | VMM_NX);
    }
    return slot;
}

/* --- the run list (sched_lock held) --- */

static void list_insert(struct task *t) {
    /* Right after the cursor, so a new task gets a CPU soon rather than
     * after a full lap of the run list. */
    if (!rr_cursor) {
        t->next = t;
        rr_cursor = t;
    } else {
        t->next = rr_cursor->next;
        rr_cursor->next = t;
    }
    list_len++;
    __atomic_add_fetch(&task_count, 1, __ATOMIC_RELAXED);
}

static void list_remove(struct task *t) {
    struct task *prev = t;
    while (prev->next != t) prev = prev->next;
    if (prev == t) {
        rr_cursor = NULL;
    } else {
        prev->next = t->next;
        if (rr_cursor == t) rr_cursor = prev;
    }
    list_len--;
    __atomic_sub_fetch(&task_count, 1, __ATOMIC_RELAXED);
}

static void wake_all_locked(struct wait_queue *wq) {
    struct task *t = wq->head;
    while (t) {
        struct task *next = t->wait_next;
        t->wait_next = NULL;
        t->waiting_on = NULL;
        if (t->state == TASK_BLOCKED) t->state = TASK_READY;
        t = next;
    }
    wq->head = NULL;
}

static void kick_reaper_locked(void) {
    reaper_work++;
    wake_all_locked(&reaper_wq);
}

/* --- task creation --- */

void task_set_name(struct task *t, const char *name) {
    size_t len = strlen(name);
    if (len > sizeof(t->name) - 1) len = sizeof(t->name) - 1;
    memcpy(t->name, name, len);
    t->name[len] = '\0';
}

/* A task struct with a kernel stack, not yet on the run list. */
static struct task *task_alloc(const char *name) {
    struct task *t = kmalloc(sizeof(*t));
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));

    t->kstack_slot = kstack_alloc();
    if (t->kstack_slot < 0) {
        kfree(t);
        return NULL;
    }
    task_set_name(t, name);
    t->id = __atomic_fetch_add(&next_id, 1, __ATOMIC_RELAXED);
    t->state = TASK_READY;
    t->cr3 = vmm_kernel_cr3();
    t->kstack_top = kstack_base(t->kstack_slot) + KSTACK_SIZE;
    t->slice = TIME_SLICE_TICKS;
    fpu_init_state(&t->fpu);
    return t;
}

struct task *task_create_idle(unsigned cpu_index) {
    char name[8] = "idle";
    name[4] = (char)('0' + cpu_index / 10 % 10);
    name[5] = (char)('0' + cpu_index % 10);
    if (cpu_index < 10) {
        name[4] = name[5];
        name[5] = '\0';
    }
    struct task *t = task_alloc(name);
    if (!t) return NULL;
    t->is_idle = 1;
    t->on_cpu = 1; /* never picked by anyone else */
    __atomic_add_fetch(&task_count, 1, __ATOMIC_RELAXED);
    return t;
}

/* A kernel thread that will start in entry(arg), not yet runnable. */
static struct task *task_new_kernel(const char *name, void (*entry)(void *), void *arg) {
    struct task *t = task_alloc(name);
    if (!t) return NULL;

    /* Forge the frame context_switch expects to pop: six callee-saved
     * registers, then a return address. The return address is placed at
     * top-8 so %rsp is 16-byte aligned once `ret` pops it, which gives
     * task_trampoline's `call`s the alignment the SysV ABI promises. */
    uint64_t *sp = (uint64_t *)(t->kstack_top - 8);
    *sp = (uint64_t)task_trampoline;
    *--sp = 0;                /* rbp */
    *--sp = (uint64_t)entry;  /* rbx */
    *--sp = (uint64_t)arg;    /* r12 */
    *--sp = 0;                /* r13 */
    *--sp = 0;                /* r14 */
    *--sp = 0;                /* r15 */
    t->rsp = (uint64_t)sp;
    return t;
}

void task_start(struct task *t) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    list_insert(t);
    spin_unlock_irqrestore(&sched_lock, flags);
}

struct task *task_create_kernel(const char *name, void (*entry)(void *), void *arg) {
    struct task *t = task_new_kernel(name, entry, arg);
    if (t) task_start(t);
    return t;
}

static void user_task_entry(void *unused) {
    (void)unused;
    struct task *self = sched_current();
    jump_to_user(self->user_rip, self->user_rsp);
}

/* stdin, stdout, and stderr all share one open file on /dev/console. */
void task_open_console_fds(struct task *t) {
    struct file *con;
    if (vfs_open("/dev/console", O_RDWR, &con) != 0) return;
    t->fds[0] = con;
    file_ref(con);
    t->fds[1] = con;
    file_ref(con);
    t->fds[2] = con;
}

struct task *task_create_user_space(const char *name, uint64_t cr3, uint64_t rip, uint64_t rsp) {
    struct task *t = task_new_kernel(name, user_task_entry, NULL);
    if (t) {
        t->cr3 = cr3;
        t->user_rip = rip;
        t->user_rsp = rsp;
    }
    return t;
}

struct task *task_create_user(const char *name, const void *code, size_t code_size) {
    uint64_t cr3 = vmm_create_address_space();
    if (!cr3) return NULL;

    const uint8_t *src = code;
    for (size_t off = 0; off < code_size; off += PAGE_SIZE) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) goto fail;
        size_t n = code_size - off < PAGE_SIZE ? code_size - off : PAGE_SIZE;
        memcpy(phys_to_virt(phys), src + off, n);
        vmm_map_user(cr3, USER_CODE_BASE + off, phys, VMM_PRESENT); /* R-X */
    }

    for (uint64_t va = USER_STACK_TOP - USER_STACK_SIZE; va < USER_STACK_TOP; va += PAGE_SIZE) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) goto fail;
        vmm_map_user(cr3, va, phys, VMM_PRESENT | VMM_WRITABLE | VMM_NX);
    }

    struct task *t = task_create_user_space(name, cr3, USER_CODE_BASE, USER_STACK_TOP);
    if (t) {
        task_open_console_fds(t);
        task_start(t);
        return t;
    }

fail:
    vmm_destroy_address_space(cr3);
    return NULL;
}

struct task *task_fork(const struct registers *regs) {
    struct task *parent = sched_current();
    uint64_t cr3 = vmm_clone_address_space(parent->cr3);
    if (!cr3) return NULL;
    struct task *t = task_alloc(parent->name);
    if (!t) {
        vmm_destroy_address_space(cr3);
        return NULL;
    }
    t->cr3 = cr3;
    t->brk_start = parent->brk_start;
    t->brk = parent->brk;
    t->parent = parent;
    fd_inherit(t, parent);
    fpu_save(&t->fpu); /* the parent's live registers */

    /* The child's first switch-in "returns" into fork_return, which pops a
     * copy of the parent's syscall frame and irets straight to user mode,
     * as if the child had made the same int 0x80, only with %rax = 0. */
    struct registers *frame = (struct registers *)(t->kstack_top - sizeof(*frame));
    *frame = *regs;
    frame->rax = 0;
    uint64_t *sp = (uint64_t *)(t->kstack_top - sizeof(*frame));
    *--sp = (uint64_t)fork_return;
    for (int i = 0; i < 6; i++) *--sp = 0; /* rbp, rbx, r12-r15 */
    t->rsp = (uint64_t)sp;

    task_start(t);
    return t;
}

/* --- switching --- */

static void finish_switch_locked(void) {
    struct cpu *c = this_cpu();
    struct task *prev = c->switch_prev;
    c->switch_prev = NULL;
    if (prev && !prev->is_idle) {
        prev->on_cpu = 0; /* its stack is finally free for others */
        if (prev->state == TASK_ZOMBIE) kick_reaper_locked();
    }
}

void sched_finish_switch(void) {
    finish_switch_locked();
    spin_unlock(&sched_lock);
}

/* Picks the next task for this CPU -- round-robin over the run list from
 * the global cursor, skipping tasks running elsewhere -- and switches to
 * it, or to this CPU's idle task if nothing is runnable. Called with
 * sched_lock held and interrupts off, and returns (in the task that
 * called it, whenever it next runs, on whichever CPU) the same way. */
static void schedule(void) {
    struct cpu *c = this_cpu();
    struct task *cur = c->current;
    uint64_t now = timer_ticks();
    struct task *next = NULL;

    if (rr_cursor) {
        struct task *t = rr_cursor->next, *start = t;
        do {
            if (t->state == TASK_SLEEPING && now >= t->wake_tick) t->state = TASK_READY;
            if (t->state == TASK_READY && (!t->on_cpu || t == cur)) {
                next = t;
                break;
            }
            t = t->next;
        } while (t != start);
    }
    if (next) rr_cursor = next;
    else next = c->idle;

    next->slice = TIME_SLICE_TICKS;
    if (next == cur) return;

    next->on_cpu = 1;
    c->switch_prev = cur;
    c->current = next;
    if (next->kstack_top) tss_set_rsp0(next->kstack_top);
    /* Swap user FPU/SSE state eagerly. No kernel code runs FPU
     * instructions, so the registers stay next's from here on. */
    fpu_save(&cur->fpu);
    fpu_restore(&next->fpu);
    context_switch(&cur->rsp, next->rsp, next->cr3);
    finish_switch_locked();
}

void sched_tick(void) {
    spin_lock(&sched_lock);
    struct task *cur = sched_current();
    /* Idle reschedules at every tick so a sleeper that just woke doesn't
     * wait out a whole slice of idling. */
    if (cur->is_idle || --cur->slice <= 0) schedule();
    spin_unlock(&sched_lock);
}

void sched_yield(void) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    schedule();
    spin_unlock_irqrestore(&sched_lock, flags);
}

void task_sleep(uint64_t ticks) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    struct task *cur = sched_current();
    cur->wake_tick = timer_ticks() + ticks;
    cur->state = TASK_SLEEPING;
    schedule();
    spin_unlock_irqrestore(&sched_lock, flags);
}

/* --- wait queues and mutexes --- */

void wait_prepare(struct wait_queue *wq) {
    spin_lock(&sched_lock);
    struct task *cur = sched_current();
    if (cur->waiting_on != wq) {
        cur->wait_next = wq->head;
        wq->head = cur;
        cur->waiting_on = wq;
    }
    cur->state = TASK_BLOCKED;
    spin_unlock(&sched_lock);
}

void wait_sleep(void) {
    spin_lock(&sched_lock);
    if (sched_current()->state == TASK_BLOCKED) schedule(); /* else already woken */
    spin_unlock(&sched_lock);
}

void wait_finish(struct wait_queue *wq) {
    spin_lock(&sched_lock);
    struct task *cur = sched_current();
    if (cur->waiting_on == wq) {
        for (struct task **p = &wq->head; *p; p = &(*p)->wait_next) {
            if (*p == cur) {
                *p = cur->wait_next;
                break;
            }
        }
        cur->wait_next = NULL;
        cur->waiting_on = NULL;
    }
    cur->state = TASK_READY;
    spin_unlock(&sched_lock);
}

void wait_queue_wake_all(struct wait_queue *wq) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    wake_all_locked(wq);
    spin_unlock_irqrestore(&sched_lock, flags);
}

void mutex_lock(struct mutex *m) {
    for (;;) {
        int unlocked = 0;
        if (__atomic_compare_exchange_n(&m->locked, &unlocked, 1, 0, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) {
            return;
        }
        wait_event(&m->waiters, __atomic_load_n(&m->locked, __ATOMIC_RELAXED) == 0);
    }
}

void mutex_unlock(struct mutex *m) {
    __atomic_store_n(&m->locked, 0, __ATOMIC_RELEASE);
    wait_queue_wake_all(&m->waiters);
}

/* --- exit, wait, and reaping --- */

int64_t task_wait(int64_t pid, int *code) {
    struct task *cur = sched_current();
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&sched_lock);
        uint64_t seen = cur->child_events;
        int have_child = 0;
        struct task *t = rr_cursor;
        for (uint64_t n = 0; t && n < list_len; n++, t = t->next) {
            if (t->parent != cur || (pid != -1 && (int64_t)t->id != pid)) continue;
            have_child = 1;
            if (t->state != TASK_ZOMBIE && t->state != TASK_DEAD) continue;

            int64_t id = (int64_t)t->id;
            *code = t->exit_code;
            t->parent = NULL; /* collected: nobody else will ask for it */
            kick_reaper_locked();
            spin_unlock_irqrestore(&sched_lock, flags);
            return id;
        }
        spin_unlock_irqrestore(&sched_lock, flags);
        if (!have_child) return -ECHILD;
        wait_event(&cur->child_exited, __atomic_load_n(&cur->child_events, __ATOMIC_ACQUIRE) != seen);
    }
}

void task_exit(int code) {
    struct task *cur = sched_current();
    /* In task context still (even when called from a fault handler), so
     * closing files may block on filesystem I/O. */
    fd_close_all(cur);

    /* A parent will collect (and can report) the status itself; log only
     * the exits nobody is going to wait for. */
    if (!cur->parent) kprintf("ATOS: task %lu (%s) exited with code %d\n", cur->id, cur->name, code);

    irq_save();
    spin_lock(&sched_lock);
    cur->exit_code = code;
    cur->state = TASK_ZOMBIE;

    /* Orphan our children: nobody will wait for them now. */
    struct task *t = rr_cursor;
    for (uint64_t n = 0; t && n < list_len; n++, t = t->next) {
        if (t->parent == cur) t->parent = NULL;
    }
    kick_reaper_locked();
    if (cur->parent) {
        cur->parent->child_events++;
        wake_all_locked(&cur->parent->child_exited);
    }

    schedule(); /* the reaper frees us once the next task is off our stack */
    __builtin_unreachable();
}

/* Frees what exited tasks held. A zombie can only be reaped once no CPU
 * is on its stack any more (on_cpu clear), and the TLB shootdown for that
 * stack must run with interrupts on -- hence a thread of its own, rather
 * than doing this inside schedule(). The struct itself stays (TASK_DEAD)
 * while a parent may still waitpid for it. */
static void reaper(void *arg) {
    (void)arg;
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&sched_lock);
        uint64_t seen = reaper_work;
        struct task *victim = NULL, *t = rr_cursor;
        for (uint64_t n = 0; t && n < list_len; n++, t = t->next) {
            if ((t->state == TASK_ZOMBIE && !t->on_cpu && !t->reaping) ||
                (t->state == TASK_DEAD && !t->parent)) {
                victim = t;
                break;
            }
        }
        if (!victim) {
            spin_unlock_irqrestore(&sched_lock, flags);
            wait_event(&reaper_wq, __atomic_load_n(&reaper_work, __ATOMIC_ACQUIRE) != seen);
            continue;
        }
        if (victim->state == TASK_DEAD) {
            list_remove(victim);
            spin_unlock_irqrestore(&sched_lock, flags);
            kfree(victim);
            continue;
        }
        victim->reaping = 1;
        spin_unlock_irqrestore(&sched_lock, flags);

        kstack_free(victim->kstack_slot, 1);
        if (victim->cr3 != vmm_kernel_cr3()) vmm_destroy_address_space(victim->cr3);

        flags = spin_lock_irqsave(&sched_lock);
        victim->state = TASK_DEAD;
        victim->reaping = 0;
        spin_unlock_irqrestore(&sched_lock, flags);
    }
}
