#include "block.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"
#include <stddef.h>

#define EIO    5
#define EINVAL 22

#define MAX_BLOCK_DEVICES 8
static struct block_device *devices[MAX_BLOCK_DEVICES];
static unsigned device_count;

void block_register(struct block_device *dev) {
    if (device_count == MAX_BLOCK_DEVICES) {
        kprintf("ATOS: block: too many devices, ignoring %s\n", dev->name);
        return;
    }
    devices[device_count++] = dev;
    kprintf("ATOS: block: registered %s (%lu sectors, %lu MiB)\n", dev->name,
            dev->sector_count, dev->sector_count * BLOCK_SECTOR_SIZE / (1024 * 1024));
}

struct block_device *block_get(const char *name) {
    size_t len = strlen(name);
    for (unsigned i = 0; i < device_count; i++) {
        if (strlen(devices[i]->name) == len && memcmp(devices[i]->name, name, len) == 0) {
            return devices[i];
        }
    }
    return NULL;
}

static int range_ok(struct block_device *dev, uint64_t lba, uint32_t count) {
    return count > 0 && lba < dev->sector_count && count <= dev->sector_count - lba;
}

int block_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buf) {
    if (!range_ok(dev, lba, count)) return -EINVAL;
    return dev->read ? dev->read(dev, lba, count, buf) : -EIO;
}

int block_write(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf) {
    if (!range_ok(dev, lba, count)) return -EINVAL;
    return dev->write ? dev->write(dev, lba, count, buf) : -EIO;
}
