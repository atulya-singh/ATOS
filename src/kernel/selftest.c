#include "selftest.h"
#include "dev/block.h"
#include "dev/timer.h"
#include "fs/vfs.h"
#include "arch/x86_64/percpu.h"
#include "lib/kprintf.h"
#include "lib/spinlock.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "sched/sched.h"

/* Boot-time self-tests. Each prints one result line that
 * tools/smoke-test.sh (and so CI) greps for, and all of them feed one
 * summary line -- the headless pass/fail signal. None of them are
 * throwaway debug code. */

static int tests_passed, tests_failed;

static void record(int ok) {
    __atomic_add_fetch(ok ? &tests_passed : &tests_failed, 1, __ATOMIC_SEQ_CST);
}

/* Exercises split + coalesce so a broken heap shows up as wrong numbers
 * on every boot, not just when someone happens to stress it later. */
void selftest_heap(void) {
    uint64_t free_before = pmm_free_page_count();

    void *a = kmalloc(64);
    void *b = kmalloc(128);
    void *c = kmalloc(256);
    kfree(b);
    void *d = kmalloc(96); /* should reuse b's freed block */
    kfree(a);
    kfree(c);
    kfree(d);

    uint64_t free_after = pmm_free_page_count();
    int ok = d == b && free_before == free_after;
    record(ok);
    kprintf("ATOS: heap self-test: alloc/free/coalesce %s (pmm free pages %lu -> %lu)\n",
            ok ? "ok" : "FAILED", free_before, free_after);
}

/* --- Phase 3 boot-time self-tests: multitasking + user mode --- */

extern const char user_hello_start[], user_hello_end[];
extern const char user_fault_start[], user_fault_end[];
extern const char user_spin_start[], user_spin_end[];

static volatile uint64_t worker_progress;

static void worker(void *arg) {
    const char *tag = arg;
    for (int i = 1; i <= 5; i++) {
        kprintf("ATOS: [%s] iteration %d at tick=%lu\n", tag, i, timer_ticks());
        __atomic_add_fetch(&worker_progress, 1, __ATOMIC_RELAXED);
        task_sleep(5);
    }
}

/* Busy-waits without ever yielding. Only a timer-driven preemption can
 * let anything else run meanwhile, so if worker_progress moves while it
 * spins, preemption works. */
static void spinner(void *arg) {
    (void)arg;
    uint64_t start = timer_ticks();
    uint64_t progress_before = worker_progress;
    while (timer_ticks() - start < 40) {
        asm volatile("pause");
    }
    uint64_t others = worker_progress - progress_before;
    record(others != 0);
    kprintf("ATOS: preemption %s: workers ran %lu times while spinner held the CPU for 40 ticks\n",
            others ? "ok" : "FAILED", others);
}

/* Pairs with tools/smoke-test.sh, which writes the signature into sector 0
 * of a scratch disk image and, after shutdown, checks the pattern landed on
 * the host side at DISK_TEST_LBA. 160 sectors crosses the driver's
 * 128-sector bounce-buffer boundary, so chunking is exercised too. */
#define DISK_TEST_LBA     64
#define DISK_TEST_SECTORS 160

static void disk_self_test(void *arg) {
    (void)arg;
    size_t bytes = (size_t)DISK_TEST_SECTORS * BLOCK_SECTOR_SIZE;
    uint8_t *buf = kmalloc(bytes);
    if (!buf) return;

    /* Only a disk carrying the scratch signature may be written to: the
     * others may hold real filesystems. */
    struct block_device *dev = NULL;
    for (unsigned i = 0; (dev = block_device_at(i)) != NULL; i++) {
        if (block_read(dev, 0, 1, buf) == 0 && memcmp(buf, "ATOSDISK", 8) == 0) break;
    }
    if (!dev) {
        kprintf("ATOS: disk self-test: no scratch disk, skipped\n");
        kfree(buf);
        return;
    }

    const char *verdict = "FAILED";

    for (size_t i = 0; i < bytes; i++) buf[i] = (uint8_t)((i * 7) ^ (i >> 9));
    if (block_write(dev, DISK_TEST_LBA, DISK_TEST_SECTORS, buf) != 0) {
        verdict = "FAILED (write)";
        goto out;
    }
    memset(buf, 0, bytes);
    if (block_read(dev, DISK_TEST_LBA, DISK_TEST_SECTORS, buf) != 0) {
        verdict = "FAILED (read back)";
        goto out;
    }
    for (size_t i = 0; i < bytes; i++) {
        if (buf[i] != (uint8_t)((i * 7) ^ (i >> 9))) {
            verdict = "FAILED (data mismatch)";
            goto out;
        }
    }
    if (block_read(dev, dev->sector_count, 1, buf) >= 0) {
        verdict = "FAILED (out-of-range read accepted)";
        goto out;
    }
    verdict = "ok";

out:
    record(strcmp(verdict, "ok") == 0);
    kprintf("ATOS: disk self-test: signature + %d-sector write/readback %s\n",
            DISK_TEST_SECTORS, verdict);
    kfree(buf);
}

/* Exercises path normalization, devfs lookup/readdir, char-device I/O,
 * and error paths through the same entry points the syscalls use. */
static void vfs_self_test(void *arg) {
    (void)arg;
    const char *verdict = "ok";
    struct file *f;
    struct atos_dirent ent;

    if (vfs_open("/dev/console", O_WRONLY, &f) != 0) {
        verdict = "FAILED (open /dev/console)";
        goto out;
    }
    static const char msg[] = "ATOS: vfs: hello through /dev/console\n";
    int64_t n = vfs_write(f, msg, sizeof(msg) - 1);
    file_close(f);
    if (n != (int64_t)sizeof(msg) - 1) { verdict = "FAILED (console write)"; goto out; }

    if (vfs_open("/dev/../dev/./null", O_RDWR, &f) != 0) { verdict = "FAILED (normalized path)"; goto out; }
    char c;
    int ok = vfs_write(f, "x", 1) == 1 && vfs_read(f, &c, 1) == 0 && vfs_seek(f, 0, SEEK_SET) == -ESPIPE;
    file_close(f);
    if (!ok) { verdict = "FAILED (/dev/null semantics)"; goto out; }

    if (vfs_open("/dev", O_RDONLY, &f) != 0) { verdict = "FAILED (open /dev)"; goto out; }
    unsigned count = 0;
    while (vfs_readdir(f, count, &ent) == 0) count++;
    int readdir_ok = count == 2 && vfs_readdir(f, 0, &ent) == 0 && memcmp(ent.name, "console", 8) == 0;
    int write_dir = vfs_write(f, "x", 1);
    file_close(f);
    if (!readdir_ok) { verdict = "FAILED (readdir /dev)"; goto out; }
    if (write_dir != -EBADF) { verdict = "FAILED (write to read-only dir fd)"; goto out; }

    if (vfs_open("/dev/nope", O_RDONLY, &f) != -ENOENT) { verdict = "FAILED (missing file)"; goto out; }
    if (vfs_open("/dev", O_WRONLY, &f) != -EISDIR) { verdict = "FAILED (dir opened for write)"; goto out; }
    if (vfs_open("relative", O_RDONLY, &f) != -ENOENT) { verdict = "FAILED (relative path)"; goto out; }

out:
    record(strcmp(verdict, "ok") == 0);
    kprintf("ATOS: vfs self-test %s\n", verdict);
}

/* Checks the initrd mounted at / against rootfs/ in the repo. */
static void initrd_self_test(void *arg) {
    (void)arg;
    const char *verdict = "ok";
    struct file *f;
    struct atos_dirent ent;
    char buf[64];

    if (vfs_open("/etc/motd", O_RDONLY, &f) != 0) { verdict = "FAILED (open /etc/motd)"; goto out; }
    int64_t n = vfs_read(f, buf, 16);
    int64_t n2 = vfs_seek(f, -3, SEEK_END) >= 0 ? vfs_read(f, buf + 16, sizeof(buf) - 16) : -1;
    int64_t eof = vfs_read(f, buf, 1);
    struct atos_stat st;
    vfs_stat(f, &st);
    file_close(f);
    if (n != 16 || memcmp(buf, "Welcome to ATOS!", 16) != 0) { verdict = "FAILED (motd contents)"; goto out; }
    if (n2 != 3 || memcmp(buf + 16, "s.\n", 3) != 0 || eof != 0) { verdict = "FAILED (seek/EOF)"; goto out; }
    if (st.type != ATOS_TYPE_FILE || st.size < 16) { verdict = "FAILED (stat)"; goto out; }

    if (vfs_open("/", O_RDONLY, &f) != 0) { verdict = "FAILED (open /)"; goto out; }
    int saw_etc = 0, saw_dev = 0, saw_disk = 0;
    for (uint64_t i = 0; vfs_readdir(f, i, &ent) == 0; i++) {
        if (strcmp(ent.name, "etc") == 0 && ent.type == ATOS_TYPE_DIR) saw_etc = 1;
        if (strcmp(ent.name, "dev") == 0) saw_dev = 1;
        if (strcmp(ent.name, "disk") == 0) saw_disk = 1;
    }
    file_close(f);
    if (!saw_etc || !saw_dev || !saw_disk) { verdict = "FAILED (readdir /)"; goto out; }

    if (vfs_open("/etc/motd", O_WRONLY, &f) == 0) {
        n = vfs_write(f, "x", 1);
        file_close(f);
        if (n != -EROFS) { verdict = "FAILED (write to read-only initrd)"; goto out; }
    }

out:
    record(strcmp(verdict, "ok") == 0);
    kprintf("ATOS: initrd self-test %s\n", verdict);
}

/* --- SMP: many tasks, spread over every CPU, hammering one spinlock and
 * one mutex. Any hole in either lock (or in the scheduler handing tasks
 * between CPUs) shows up as lost increments. */

#define SMP_WORKERS    8
#define SMP_ITERATIONS 20000

static struct spinlock smp_spin;
static struct mutex smp_mutex;
static uint64_t smp_spin_count, smp_mutex_count;
static uint32_t smp_cpus_seen; /* bitmask of CPU indexes any worker ran on */
static uint32_t smp_workers_done;

static void smp_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < SMP_ITERATIONS; i++) {
        __atomic_or_fetch(&smp_cpus_seen, 1u << this_cpu()->index, __ATOMIC_RELAXED);
        uint64_t flags = spin_lock_irqsave(&smp_spin);
        uint64_t v = smp_spin_count; /* a deliberately non-atomic update */
        smp_spin_count = v + 1;
        spin_unlock_irqrestore(&smp_spin, flags);

        if (i % 50 == 0) {
            mutex_lock(&smp_mutex);
            uint64_t m = smp_mutex_count;
            if (i % 1000 == 0) sched_yield(); /* block others on the mutex */
            smp_mutex_count = m + 1;
            mutex_unlock(&smp_mutex);
        }
    }
    __atomic_add_fetch(&smp_workers_done, 1, __ATOMIC_RELEASE);
}

static void smp_self_test(void *arg) {
    (void)arg;
    for (int i = 0; i < SMP_WORKERS; i++) task_create_kernel("smp-worker", smp_worker, NULL);
    while (__atomic_load_n(&smp_workers_done, __ATOMIC_ACQUIRE) < SMP_WORKERS) task_sleep(2);

    uint64_t want_spin = (uint64_t)SMP_WORKERS * SMP_ITERATIONS;
    uint64_t want_mutex = (uint64_t)SMP_WORKERS * (SMP_ITERATIONS / 50);
    unsigned seen = 0; /* no libgcc for __builtin_popcount */
    for (uint32_t m = smp_cpus_seen; m; m &= m - 1) seen++;
    /* With several CPUs, work must actually have spread across them. */
    int ok = smp_spin_count == want_spin && smp_mutex_count == want_mutex &&
             (cpu_count == 1 || seen > 1);
    record(ok);
    kprintf("ATOS: smp self-test %s: %lu/%lu spinlocked and %lu/%lu mutexed increments, workers ran on %u of %u CPU(s)\n",
            ok ? "ok" : "FAILED", smp_spin_count, want_spin, smp_mutex_count, want_mutex, seen,
            cpu_count);
}

static void spawn_self_tests(void) {
    task_create_kernel("smp-test", smp_self_test, NULL);
    task_create_kernel("initrd-test", initrd_self_test, NULL);
    task_create_kernel("vfs-test", vfs_self_test, NULL);
    task_create_kernel("disk-test", disk_self_test, NULL);
    task_create_kernel("worker-a", worker, "worker-a");
    task_create_kernel("worker-b", worker, "worker-b");
    task_create_kernel("spinner", spinner, NULL);
    task_create_user("user-hello", user_hello_start,
                     (size_t)(user_hello_end - user_hello_start));
    task_create_user("user-fault", user_fault_start,
                     (size_t)(user_fault_end - user_fault_start));
    task_create_user("user-spin", user_spin_start,
                     (size_t)(user_spin_end - user_spin_start));
}

static uint64_t baseline_tasks, pmm_before, heap_before;
static int reaped_reported;

void selftest_spawn(void) {
    /* Everything spawned here should exit and hand back every resource it
     * took; selftest_poll checks that once they're gone. */
    baseline_tasks = sched_task_count();
    pmm_before = pmm_free_page_count();
    heap_before = heap_free_bytes();
    spawn_self_tests();
}

int selftest_failures(void) {
    return tests_failed;
}

int selftest_poll(void) {
    if (reaped_reported) return 1;
    if (sched_task_count() != baseline_tasks) return 0;
    /* Only long-lived services are left, so every stack, address space and
     * task struct should be back where it came from. */
    uint64_t pmm_after = pmm_free_page_count();
    uint64_t heap_after = heap_free_bytes();
    int clean = pmm_after == pmm_before && heap_after == heap_before;
    record(clean);
    kprintf("ATOS: sched self-test: all tasks reaped %s (pmm free pages %lu -> %lu, heap free bytes %lu -> %lu)\n",
            clean ? "cleanly" : "WITH LEAKS", pmm_before, pmm_after, heap_before, heap_after);
    kprintf("ATOS: self-test summary: %d passed, %d failed\n", tests_passed, tests_failed);
    reaped_reported = 1;
    return 1;
}
