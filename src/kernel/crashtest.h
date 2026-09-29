#pragma once

/* If the kernel command line holds crashtest=<kind>, starts a kernel task
 * that crashes on purpose, to exercise the panic path end to end:
 *   pagefault      NULL dereference a few calls deep
 *   stackoverflow  unbounded recursion into the stack's guard page (#DF)
 *   assert         a failing KASSERT */
void crashtest_start(const char *cmdline);
