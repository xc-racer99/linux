// SPDX-License-Identifier: GPL-2.0+
//
// Copyright (C) 2018 Simon Shields <simon@lineageos.org>
//
#ifndef _NET_SIPC_SIPC_H
#define _NET_SIPC_SIPC_H 1
#include <linux/miscdevice.h>
#include <linux/netdevice.h>
#include <linux/sipc.h>
#include <linux/wait.h>
/* Keep each incoming skbuff below 1 page */
#define MAX_RX_SIZE (4096 - 512)
#define MAX_MULTI_RX_SIZE (16 * 1024)
#define HDLC_START ((u8)0x7f)
#define HDLC_END ((u8)0x7e)

extern struct sipc_link *cur_links[SAMSUNG_IPC_FORMAT_MAX];
extern struct sipc_link_callback *callbacks;

union sipc_header {
	struct fmt_header fmt;
	struct raw_header raw;
	struct rfs_header rfs;
};

struct hdlc_header {
	union sipc_header sipc_header;
	u32 len;
	u32 frag_len;
	char start;
};

struct sipc_io_channel {
	/* populated from dt */
	u32 format;
	u32 type;
	u32 channel;
	const char *name;

	/* initialised at runtime */
	struct samsung_ipc *sipc;
	struct sk_buff_head rx_queue;
	wait_queue_head_t wq;
	atomic_t use_count;

	struct miscdevice miscdev;
	struct net_device *netdev;

	/* pending sk buffs */
	struct sk_buff *fmt_skb[128];

	/* sometimes a packet might come over multiple frames */
	struct hdlc_header pending_rx_header;
	struct sk_buff *pending_rx_skb;
};
#define misc_to_chan(m) container_of(m, struct sipc_io_channel, miscdev)

struct sipc_netdev_priv {
	struct sipc_io_channel *chan;
};

struct samsung_ipc {
	struct device *dev;
	struct sipc_io_channel *channels;
	int nchannels;
	int version;

	struct sk_buff_head tx_queue_rfs;
	struct sk_buff_head tx_queue_fmt;
	struct sk_buff_head tx_queue_raw;
	bool raw_tx_suspended;
	struct completion raw_tx_resumed;

	struct delayed_work tx_work;
	struct workqueue_struct *tx_wq;

	struct sipc_link_callback link_cb;
};
#define cb_to_ipc(cb) container_of(cb, struct samsung_ipc, link_cb)

extern const struct file_operations sipc_misc_fops;
int sipc_get_header_size(int format);
void sipc_netdev_setup(struct net_device *ndev);
#endif
