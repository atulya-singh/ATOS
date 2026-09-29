#pragma once

/* Brings up the first legacy/transitional virtio-net PCI device and
 * registers it with the network stack (see net/net.h). */
void virtio_net_init(void);
