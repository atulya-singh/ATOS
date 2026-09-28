#pragma once

/* Mounts the device filesystem at /dev: console (keyboard in, screen and
 * serial out) and null. */
void devfs_init(void);
