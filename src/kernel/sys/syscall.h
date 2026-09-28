#pragma once

/* int 0x80 ABI: number in %rax, arguments in %rdi, %rsi, %rdx (the SysV
 * argument order, so a future syscall/sysret path can keep the same
 * layout), result in %rax. Errors come back as negative errno values. */
#define SYS_WRITE 0 /* write(fd, buf, len) -> bytes written */
#define SYS_EXIT  1 /* exit(code) -> does not return */
#define SYS_YIELD 2 /* yield() -> 0 */
#define SYS_READ  3 /* read(fd, buf, len) -> bytes read; fd 0 blocks for >= 1 */

#define EBADF  9
#define EFAULT 14
#define ENOSYS 38
