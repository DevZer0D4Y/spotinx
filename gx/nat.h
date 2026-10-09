/*
 * nat - the network of the guest: a network device for LKL whose other end
 * is ordinary sockets of the host. See nat.c.
 */
#ifndef GX_NAT_H
#define GX_NAT_H

#include <stdint.h>

/* The network the device is on, the way user-mode network stacks usually lay
 * it out. Host byte order. */
#define NAT_GUEST 0x0a00020fu    /* 10.0.2.15, the address to give the kernel */
#define NAT_GATEWAY 0x0a000202u  /* 10.0.2.2, also the host's loopback */
#define NAT_DNS 0x0a000203u      /* 10.0.2.3, forwarded to the host's resolver */
#define NAT_PREFIX 24

struct lkl_netdev;

/* dns is the resolver of the host, as an IPv4 address in network byte order.
 * mac receives the address to give the device (lkl_netdev_args). */
struct lkl_netdev *nat_create(uint32_t dns, uint8_t mac[6]);

#endif
