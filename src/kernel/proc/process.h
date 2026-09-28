#pragma once
#include <stdint.h>

struct task;

/* Most bytes of argv strings + pointers a new program's stack may carry;
 * they all go in the top stack page. */
#define ARGS_MAX 3072
#define ARGV_MAX 32

/* Starts the ELF program at `path` as a new process with the given argv
 * and fds 0-2 on /dev/console. Kernel-side entry point for launching
 * init; user processes use fork + exec instead. */
struct task *process_spawn(const char *path, int argc, const char *const argv[]);

struct registers;
/* Replaces the calling process's program with the one at `path`, keeping
 * its pid and open fds. On success rewrites `regs` (the syscall frame) so
 * the iretq lands at the new entry point; on failure (-errno) nothing
 * about the caller has changed. */
int process_exec(struct registers *regs, const char *path, int argc, const char *const argv[]);

/* Moves the calling process's heap end to `addr` (0 queries it). Returns
 * the resulting break, which is the old one if the request can't be met,
 * the same contract as Linux's brk. */
uint64_t process_brk(uint64_t addr);
