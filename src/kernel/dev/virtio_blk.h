#pragma once

/* Finds a legacy/transitional virtio-blk PCI device and registers it as
 * block device "vda". Does nothing if none is present. */
void virtio_blk_init(void);
