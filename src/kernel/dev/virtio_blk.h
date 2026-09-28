#pragma once

/* Registers every legacy/transitional virtio-blk PCI device as a block
 * device, named vda, vdb, ... in PCI enumeration order. */
void virtio_blk_init(void);
