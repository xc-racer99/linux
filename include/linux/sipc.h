// SPDX-License-Identifier: GPL-2.0+
//
// Samsung IPC v4.x modem glue code
// Link drivers use this to communicate
// with the userspace-facing driver.
//
#ifndef _LINUX_SIPC_H
#define _LINUX_SIPC_H
#include <dt-bindings/net/samsung_ipc.h>
#include <linux/skbuff.h>
#include <uapi/linux/samsung_ipc.h>

struct sipc_link;
struct sipc_link_callback;

struct sipc_link {
    /* transmit a packet on this link */
    int (*transmit)(struct sipc_link *link, struct sk_buff *skb);

    /* called when channel is opened */
    int (*open)(struct sipc_link *link, int channel, int format);

    /* set callbacks */
    void (*set_callbacks)(struct sipc_link *link, struct sipc_link_callback *cb);
};

struct sipc_link_callback {
    /* called when a new packet is ready to be received */
    void (*receive)(struct sipc_link_callback *cb, void *buf, size_t bufsz, int format);
};

int sipc_set_link(struct sipc_link *new_link, unsigned int type);
void sipc_clear_link(unsigned int type);
#endif
