#pragma once

struct block_device;

/* Mounts `dev` at `path` if it holds a FAT32 filesystem (no partition
 * table: the volume starts at sector 0). Returns 0, or -errno (-EINVAL if
 * it isn't FAT32). */
int fat_mount(struct block_device *dev, const char *path);
