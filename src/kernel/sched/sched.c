#include "sched.h"
#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/gdt.h"
#include "../dev/pit.h"
#include "../lib/kprintf.h"
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
#define MAX_TASKS         64

extern void context_switch(uint64_t *old_rsp, uint64_t new_rsp, uint64_t new_cr3);
extern void task_trampoline(void);
extern __attribute__((noreturn)) void jump_to_user(uint64_t rip, uint64_t rsp);

static struct task idle_task;
static struct task *current;
static uint64_t next_id = 1;
static uint64_t task_count;
static uint8_t kstack_slot_used[MAX_TASKS];

void sched_init(void) {
    memcpy(idle_task.name, "idle", 5);
    idle_task.id = 0;
    idle_task.state = TASK_READY;
    idle_task.cr3 = vmm_kernel_cr3();
    idle_task.kstack_slot = -1;
    idle_task.next = &idle_task;
    current = &idle_task;
    task_count = 1;

    /* Fixed up front, so allocating and freeing stacks later never touches
     * page-table pages -- which also keeps the boot-time leak check exact. */
    vmm_prealloc_tables(KSTACK_REGION, MAX_TASKS * KSTACK_SLOT_SIZE);

    kprintf("ATOS: scheduler: round-robin, %d-tick slices, idle task on boot stack\n",
            TIME_SLICE_TICKS);
}

struct task *sched_current(void) { return current; }
uint64_t sched_task_count(void) { return task_count; }

static uint64_t kstack_base(int slot) {
    return KSTACK_REGION + (uint64_t)slot * KSTACK_SLOT_SIZE + (KSTACK_SLOT_SIZE - KSTACK_SIZE);
}

static void kstack_free(int slot) {
    uint64_t base = kstack_base(slot);
    for (uint64_t off = 0; off < KSTACK_SIZE; off += PAGE_SIZE) {
        uint64_t phys;
        if (vmm_translate(base + off, &phys)) {
            vmm_unmap(base + off);
            pmm_free_page(phys);
        }
    }
    kstack_slot_used[slot] = 0;
}

/* Returns the slot index, or -1 if out of slots or memory. */
static int kstack_alloc(void) {
    uint64_t flags = irq_save(); /* slots are released from IRQ context */
    int slot = -1;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (!kstack_slot_used[i]) { slot = i; break; }
    }
    if (slot >= 0) kstack_slot_used[slot] = 1;
    irq_restore(flags);
    if (slot < 0) return -1;

    uint64_t base = kstack_base(slot);
    for (uint64_t off = 0; off < KSTACK_SIZE; off += PAGE_SIZE) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            kstack_free(slot);
            return -1;
        }
        vmm_map(base + off, phys, VMM_PRESENT | VMM_WRITABLE | VMM_NX);
    }
    return slot;
}

static void list_insert(struct task *t) {
    /* Right after current, so a new task gets the CPU soon rather than
     * after a full lap of the run queue. */
    uint64_t flags = irq_save();
    t->next = current->next;
    current->next = t;
    task_count++;
    irq_restore(flags);
}

struct task *task_create_kernel(const char *name, void (*entry)(void *), void *arg) {
    struct task *t = kmalloc(sizeof(*t));
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));

    t->kstack_slot = kstack_alloc();
    if (t->kstack_slot < 0) {
        kfree(t);
        return NULL;
    }

    size_t len = strlen(name);
    if (len > sizeof(t->name) - 1) len = sizeof(t->name) - 1;
    memcpy(t->name, name, len);

    t->id = next_id++;
    t->state = TASK_READY;
    t->cr3 = vmm_kernel_cr3();
    t->kstack_top = kstack_base(t->kstack_slot) + KSTACK_SIZE;
    t->slice = TIME_SLICE_TICKS;

    /* Forge the frame context_switch expects to pop: six callee-saved
     * registers, then a return address. The return address is placed at
     * top-8 so %rsp is 16-byte aligned once `ret` pops it, which gives
     * task_trampoline's `call` the alignment the SysV ABI promises. */
    uint64_t *sp = (uint64_t *)(t->kstack_top - 8);
    *sp = (uint64_t)task_trampoline;
    *--sp = 0;                /* rbp */
    *--sp = (uint64_t)entry;  /* rbx */
    *--sp = (uint64_t)arg;    /* r12 */
    *--sp = 0;                /* r13 */
    *--sp = 0;                /* r14 */
    *--sp = 0;                /* r15 */
    t->rsp = (uint64_t)sp;

    list_insert(t);
    return t;
}

static void user_task_entry(void *unused) {
    (void)unused;
    struct task *self = current;
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
    /* Interrupts stay off until the user fields are filled in: the kernel
     * thread is on the run queue as soon as task_create_kernel returns, and
     * a timer tick could otherwise pick it up half-initialized. */
    uint64_t flags = irq_save();
    struct task *t = task_create_kernel(name, user_task_entry, NULL);
    if (t) {
        t->cr3 = cr3;
        t->user_rip = rip;
        t->user_rsp = rsp;
    }
    irq_restore(flags);
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

    uint64_t flags = irq_save();
    struct task *t = task_create_user_space(name, cr3, USER_CODE_BASE, USER_STACK_TOP);
    if (t) task_open_console_fds(t);
    irq_restore(flags);
    if (t) return t;

fail:
    vmm_destroy_address_space(cr3);
    return NULL;
}

/* Frees every zombie except the running task, which is still standing on
 * its own kernel stack and address space. Called with interrupts off. */
static void reap_zombies(void) {
    struct task *prev = current;
    struct task *t = current->next;
    while (t != current) {
        struct task *next = t->next;
        if (t->state == TASK_ZOMBIE) {
            prev->next = next;
            kstack_free(t->kstack_slot);
            if (t->cr3 != vmm_kernel_cr3()) vmm_destroy_address_space(t->cr3);
            kfree(t);
            task_count--;
        } else {
            prev = t;
        }
        t = next;
    }
}

/* Picks the next runnable task, round-robin starting after the current
 * one, and switches to it. Idle runs only when nothing else can. Must be
 * called with interrupts disabled; the task that resumes restores its own
 * interrupt state when it unwinds its interrupt/syscall frame (or, for a
 * brand-new task, in task_trampoline). */
static void schedule(void) {
    reap_zombies();

    uint64_t now = pit_get_ticks();
    struct task *next = &idle_task;
    struct task *t = current->next;
    for (uint64_t n = 0; n < task_count; n++, t = t->next) {
        if (t->state == TASK_SLEEPING && now >= t->wake_tick) t->state = TASK_READY;
        if (t != &idle_task && t->state == TASK_READY) {
            next = t;
            break;
        }
    }

    next->slice = TIME_SLICE_TICKS;
    if (next == current) return;

    struct task *prev = current;
    current = next;
    if (next->kstack_top) tss_set_rsp0(next->kstack_top);
    context_switch(&prev->rsp, next->rsp, next->cr3);
}

void sched_tick(void) {
    /* Idle yields at every tick so a sleeper that just woke doesn't wait
     * out a whole slice of idling. */
    if (current == &idle_task || --current->slice <= 0) schedule();
}

void sched_yield(void) {
    uint64_t flags = irq_save();
    schedule();
    irq_restore(flags);
}

void task_sleep(uint64_t ticks) {
    uint64_t flags = irq_save();
    current->wake_tick = pit_get_ticks() + ticks;
    current->state = TASK_SLEEPING;
    schedule();
    irq_restore(flags);
}

void wait_queue_sleep(struct wait_queue *wq) {
    current->state = TASK_BLOCKED;
    current->wait_next = wq->head;
    wq->head = current;
    schedule();
}

void wait_queue_wake_all(struct wait_queue *wq) {
    uint64_t flags = irq_save();
    struct task *t = wq->head;
    while (t) {
        struct task *next = t->wait_next;
        t->wait_next = NULL;
        if (t->state == TASK_BLOCKED) t->state = TASK_READY;
        t = next;
    }
    wq->head = NULL;
    irq_restore(flags);
}

void mutex_lock(struct mutex *m) {
    uint64_t flags = irq_save();
    while (m->locked) wait_queue_sleep(&m->waiters);
    m->locked = 1;
    irq_restore(flags);
}

void mutex_unlock(struct mutex *m) {
    uint64_t flags = irq_save();
    m->locked = 0;
    wait_queue_wake_all(&m->waiters);
    irq_restore(flags);
}

void task_exit(int code) {
    /* In task context still (even when called from a fault handler), so
     * closing files may block on filesystem I/O. */
    fd_close_all(current);

    irq_save();
    kprintf("ATOS: task %lu (%s) exited with code %d\n", current->id, current->name, code);
    current->state = TASK_ZOMBIE;
    schedule();
    __builtin_unreachable(); /* a zombie is never picked again */
}
