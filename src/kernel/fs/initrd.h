#pragma once

/* Mounts the ustar archive Limine loaded as a module (limine.conf's
 * module_path) read-only at /. File contents are served in place from
 * the module's memory; only the directory tree is built on the heap. */
void initrd_init(void);
