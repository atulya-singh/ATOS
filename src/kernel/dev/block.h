#pragma once
#include <stdint.h>

#define BLOCK_SECTOR_SIZE 512

/* A disk-like device addressed in 512-byte sectors. Drivers register one
 * per disk; filesystems only ever see this interface. read/write block
 * the calling task until the transfer completes and return 0 or -errno. */
struct block_device {
    char name[16];
    uint64_t sector_count;
    int (*read)(struct block_device *dev, uint64_t lba, uint32_t count, void *buf);
    int (*write)(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf);
    void *driver_data;
};

void block_register(struct block_device *dev);
struct block_device *block_get(const char *name);
/* The index-th registered device, or NULL past the end. */
struct block_device *block_device_at(unsigned index);

/* Bounds-checked entry points: reject ranges past the end of the device
 * before any driver sees them. */
int block_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buf);
int block_write(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf);
