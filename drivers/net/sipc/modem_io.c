/* modem_io.c
 *
 * Copyright (C) 2010 Google, Inc.
 * Copyright (C) 2010 Samsung Electronics.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>

#include <linux/circ_buf.h>
#include <uapi/linux/samsung_ipc.h>

#include "modem_ctl.h"
#include "modem_ctl_p.h"
#include "sipc.h"

#define RAW_CH_VNET0 10
#define CHANNEL_TO_NETDEV_ID(id) (id - RAW_CH_VNET0)
#define NETDEV_TO_CHANNEL_ID(id) (id + RAW_CH_VNET0)
#define MAX_PDP_CONTEXTS 3

/* general purpose fifo access routines */
static unsigned fifo_write(struct m_fifo *q, void *src,
			    unsigned count)
{
	unsigned n;
	unsigned head = *q->head;
	unsigned tail = *q->tail;
	unsigned size = q->size;

	if (CIRC_SPACE(head, tail, size) < count)
		return 0;

	n = CIRC_SPACE_TO_END(head, tail, size);

	if (likely(n >= count)) {
		memcpy(q->data + head, src, count);
	} else {
		memcpy(q->data + head, src, n);
		memcpy(q->data, src + n, count - n);
	}
	*q->head = (head + count) & (size - 1);

	return count;
}

static void fifo_purge(struct m_fifo *q)
{
	*q->head = 0;
	*q->tail = 0;
}

static void fifo_skip(struct m_fifo *q, unsigned count)
{
	*q->tail = (*q->tail + count) & (q->size - 1);
}

#define fifo_count(mf) CIRC_CNT(*(mf)->head, *(mf)->tail, (mf)->size)
#define fifo_count_end(mf) CIRC_CNT_TO_END(*(mf)->head, *(mf)->tail, (mf)->size)
#define fifo_space(mf) CIRC_SPACE(*(mf)->head, *(mf)->tail, (mf)->size)

/* Called with mc->lock held whenever we gain access
 * to the mmio region.
 */
void modem_update_state(struct modemctl *mc)
{
	/* update our idea of space available in fifos */
	mc->cmd_pipe.tx.avail = fifo_space(&mc->cmd_pipe.tx);
	mc->cmd_pipe.rx.avail = fifo_count(&mc->cmd_pipe.rx);

	mc->rfs_pipe.tx.avail = fifo_space(&mc->rfs_pipe.tx);
	mc->rfs_pipe.rx.avail = fifo_count(&mc->rfs_pipe.rx);

	mc->raw_pipe.tx.avail = fifo_space(&mc->raw_pipe.tx);
	mc->raw_pipe.rx.avail = fifo_count(&mc->raw_pipe.rx);

	/* wake up blocked or polling read/write operations */
	wake_up(&mc->wq);
}

void modem_update_pipe(struct m_pipe *pipe)
{
	unsigned long flags;
	spin_lock_irqsave(&pipe->mc->lock, flags);
	pipe->tx.avail = fifo_space(&pipe->tx);
	pipe->rx.avail = fifo_count(&pipe->rx);
	spin_unlock_irqrestore(&pipe->mc->lock, flags);
}


/* must be called with pipe->tx_lock held */
int modem_pipe_send(struct m_pipe *pipe, struct sk_buff *skb)
{
	int ret;

	if (skb->len >= (pipe->tx.size - 1))
		return -EINVAL;

	for (;;) {
		ret = modem_acquire_mmio(pipe->mc);
		if (ret)
			return ret;

		modem_update_pipe(pipe);

		if (pipe->tx.avail >= skb->len) {
			fifo_write(&pipe->tx, skb->data, skb->len);
			modem_update_pipe(pipe);
			modem_release_mmio(pipe->mc, pipe->tx.bits);
			MODEM_COUNT(pipe->mc, pipe_tx);
			return 0;
		}

		pr_info("modem_pipe_send: wait for space\n");
		MODEM_COUNT(pipe->mc, pipe_tx_delayed);
		modem_release_mmio(pipe->mc, 0);

		ret = wait_event_interruptible_timeout(
			pipe->mc->wq,
			(pipe->tx.avail >= skb->len) || modem_offline(pipe->mc),
			5 * HZ);
		if (ret == 0)
			return -ENODEV;
		if (ret < 0)
			return ret;
	}
}

static int modem_pipe_recv(struct m_pipe *pipe)
{
	unsigned int n, count;
	int ret;

	ret = modem_acquire_mmio(pipe->mc);
	if (ret)
		return ret;

	count = fifo_count(&pipe->rx);
	n = fifo_count_end(&pipe->rx);

	if (n >= count) {
		/* Only send to end, then send from start of circ buffer */
		ret = pipe->cb->receive(pipe->cb,
				pipe->rx.data + *pipe->rx.tail,
				count, pipe->format);
	} else {
		/* Only send to end, then send from start of circ buffer */
		ret = pipe->cb->receive(pipe->cb,
				pipe->rx.data + *pipe->rx.tail,
				n, pipe->format);

		ret |= pipe->cb->receive(pipe->cb, pipe->rx.data,
				count - n, pipe->format);
	}

	if (ret < 0) {
		pr_err("%s: callback error %d\n", __func__, ret);
		fifo_purge(&pipe->rx);
	} else {
		fifo_skip(&pipe->rx, count);
		modem_update_pipe(pipe);
	}

	modem_release_mmio(pipe->mc, 0);

	return ret;
}

void modem_handle_io(struct modemctl *mc)
{
	if (mc->cmd_pipe.rx.avail)
		modem_pipe_recv(&mc->cmd_pipe);

	if (mc->rfs_pipe.rx.avail)
		modem_pipe_recv(&mc->rfs_pipe);

	if (mc->raw_pipe.rx.avail)
		modem_pipe_recv(&mc->raw_pipe);
}

static void sipc_fmt_set_callbacks(struct sipc_link *link,
		struct sipc_link_callback *cb)
{
	struct modemctl *mc = fmt_ops_to_mc(link);
	mc->cmd_pipe.cb = cb;
}

static void sipc_rfs_set_callbacks(struct sipc_link *link,
		struct sipc_link_callback *cb)
{
	struct modemctl *mc = rfs_ops_to_mc(link);
	mc->rfs_pipe.cb = cb;
}

static void sipc_raw_set_callbacks(struct sipc_link *link,
		struct sipc_link_callback *cb)
{
	struct modemctl *mc = raw_ops_to_mc(link);
	mc->raw_pipe.cb = cb;
}

static int sipc_link_open(struct sipc_link *link, int channel, int format)
{
	// No link specific initialization
	return 0;
}

static int sipc_fmt_transmit(struct sipc_link *link, struct sk_buff *skb)
{
	struct modemctl *mc = fmt_ops_to_mc(link);
	struct m_pipe *pipe = &mc->cmd_pipe;
	int ret;

	if (mutex_lock_interruptible(&pipe->tx_lock))
		return -EINTR;
	ret = modem_pipe_send(pipe, skb);
	mutex_unlock(&pipe->tx_lock);

	dev_kfree_skb_any(skb);
	return ret;
}

static int sipc_rfs_transmit(struct sipc_link *link, struct sk_buff *skb)
{
	struct modemctl *mc = rfs_ops_to_mc(link);
	struct m_pipe *pipe = &mc->rfs_pipe;
	int ret;

	if (mutex_lock_interruptible(&pipe->tx_lock))
		return -EINTR;
	ret = modem_pipe_send(pipe, skb);
	mutex_unlock(&pipe->tx_lock);

	dev_kfree_skb_any(skb);
	return ret;
}

static int sipc_raw_transmit(struct sipc_link *link, struct sk_buff *skb)
{
	struct modemctl *mc = raw_ops_to_mc(link);
	struct m_pipe *pipe = &mc->raw_pipe;
	int ret;

	if (mutex_lock_interruptible(&pipe->tx_lock))
		return -EINTR;
	ret = modem_pipe_send(pipe, skb);
	mutex_unlock(&pipe->tx_lock);

	dev_kfree_skb_any(skb);
	return ret;
}

static int modemctl_sipc_init(struct modemctl *mc)
{
	int r;

	mc->fmt_ops.transmit = sipc_fmt_transmit;
	mc->fmt_ops.open = sipc_link_open;
	mc->fmt_ops.set_callbacks = sipc_fmt_set_callbacks;
	r = sipc_set_link(&mc->fmt_ops, SAMSUNG_IPC_FORMAT_FMT);
	if (r < 0) {
		pr_err("Fail setting SIPC FMT link: %d", r);
		return r;
	}

	mc->rfs_ops.transmit = sipc_rfs_transmit;
	mc->rfs_ops.open = sipc_link_open;
	mc->rfs_ops.set_callbacks = sipc_rfs_set_callbacks;
	r = sipc_set_link(&mc->rfs_ops, SAMSUNG_IPC_FORMAT_RFS);
	if (r < 0) {
		pr_err("Fail setting SIPC RFS link: %d", r);
		return r;
	}

	mc->raw_ops.transmit = sipc_raw_transmit;
	mc->raw_ops.open = sipc_link_open;
	mc->raw_ops.set_callbacks = sipc_raw_set_callbacks;
	r = sipc_set_link(&mc->rfs_ops, SAMSUNG_IPC_FORMAT_RAW);
	if (r < 0) {
		pr_err("Fail setting SIPC raw link: %d", r);
		return r;
	}

	return 0;
}

int modem_io_init(struct modemctl *mc, void __iomem *mmio)
{
	INIT_M_FIFO(mc->cmd_pipe.tx, FMT, TX, mmio);
	INIT_M_FIFO(mc->cmd_pipe.rx, FMT, RX, mmio);
	INIT_M_FIFO(mc->rfs_pipe.tx, RFS, TX, mmio);
	INIT_M_FIFO(mc->rfs_pipe.rx, RFS, RX, mmio);
	INIT_M_FIFO(mc->raw_pipe.tx, RAW, TX, mmio);
	INIT_M_FIFO(mc->raw_pipe.rx, RAW, RX, mmio);

	mc->cmd_pipe.tx.bits = MBD_SEND_FMT;
	mc->cmd_pipe.format = SAMSUNG_IPC_FORMAT_FMT;
	mc->cmd_pipe.mc = mc;
	mutex_init(&mc->cmd_pipe.tx_lock);

	mc->rfs_pipe.tx.bits = MBD_SEND_RFS;
	mc->rfs_pipe.format = SAMSUNG_IPC_FORMAT_RFS;
	mc->rfs_pipe.mc = mc;
	mutex_init(&mc->rfs_pipe.tx_lock);

	mc->raw_pipe.tx.bits = MBD_SEND_RAW;
	mc->raw_pipe.format = SAMSUNG_IPC_FORMAT_RAW;
	mc->raw_pipe.mc = mc;
	mutex_init(&mc->raw_pipe.tx_lock);

	return modemctl_sipc_init(mc);
}
