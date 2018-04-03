// SPDX-License-Identifier: GPL-2.0+
//
// Samsung IPC v4.x misc device userspace
// interface.
//
// Copyright (C) 2018 Simon Shields <simon@lineageos.org>
//
#include <dt-bindings/net/samsung_ipc.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/poll.h>
#include <linux/sipc.h>
#include <linux/skbuff.h>

#include "sipc.h"

static int sipc_get_header(struct sipc_io_channel *chan, union sipc_header *hdr, size_t len)
{
	switch (chan->format) {
	case SAMSUNG_IPC_FORMAT_FMT:
		hdr->fmt.len = len + sizeof(struct fmt_header);
		hdr->fmt.control = 0;
		return sizeof(struct fmt_header);
	case SAMSUNG_IPC_FORMAT_RAW:
	case SAMSUNG_IPC_FORMAT_MULTI_RAW:
		hdr->raw.len = len + sizeof(struct raw_header);
		hdr->raw.channel = chan->channel & 0x1f;
		hdr->raw.control = 0;
		return sizeof(struct raw_header);
	case SAMSUNG_IPC_FORMAT_RFS:
		hdr->rfs.len = len + sizeof(struct rfs_header);
		hdr->rfs.id = chan->channel;
		return sizeof(struct rfs_header);
	default:
		return 0;
	}

	return 0;
}

static int sipc_misc_open(struct inode *inode, struct file *filp)
{
	struct sipc_io_channel *chan = misc_to_chan(filp->private_data);
	int ret;

	if (!cur_links[chan->format])
		return -ENODEV;

	ret = cur_links[chan->format]->open(cur_links[chan->format], chan->channel, chan->format);
	if (ret < 0) {
		dev_err(chan->sipc->dev, "Failed to open communication: %d\n", ret);
		return ret;
	}

	atomic_inc(&chan->use_count);
	return 0;
}

static int sipc_misc_release(struct inode *inode, struct file *filp)
{
	struct sipc_io_channel *chan = misc_to_chan(filp->private_data);

	atomic_dec(&chan->use_count);
	skb_queue_purge(&chan->rx_queue);

	return 0;
}

static unsigned int sipc_misc_poll(struct file *filp, struct poll_table_struct *wait)
{
	struct sipc_io_channel *chan = misc_to_chan(filp->private_data);

	poll_wait(filp, &chan->wq, wait);

	if (!skb_queue_empty(&chan->rx_queue))
		return POLLIN | POLLRDNORM;
	return 0;
}

static ssize_t sipc_misc_write(struct file *filp, const char __user *buf,
		size_t count, loff_t *ppos)
{
	struct sipc_io_channel *chan = misc_to_chan(filp->private_data);
	struct sk_buff *skb;
	union sipc_header hdr;
	unsigned char *data;
	int frame_len, header_size, tx_size, ret;

	header_size = sipc_get_header_size(chan->format);
	frame_len = sizeof(HDLC_START) + header_size
			+ count + sizeof(HDLC_END);

	skb = alloc_skb(frame_len, GFP_KERNEL);
	if (!skb) {
		dev_err(chan->sipc->dev, "Failed to allocate skb\n");
		return -ENOMEM;
	}

	if (chan->format != SAMSUNG_IPC_FORMAT_RAMDUMP) {
		data = skb_put(skb, sizeof(HDLC_START));
		data[0] = HDLC_START;
		if (chan->format != SAMSUNG_IPC_FORMAT_RFS) {
			header_size = sipc_get_header(chan, &hdr, count);
			memcpy(skb_put(skb, header_size), &hdr, header_size);
		}
	}

	if (copy_from_user(skb_put(skb, count), buf, count) != 0) {
		dev_kfree_skb_any(skb);
		return -EFAULT;
	}

	switch (chan->format) {
	case SAMSUNG_IPC_FORMAT_FMT:
		skb_queue_tail(&chan->sipc->tx_queue_fmt, skb);
		break;
	case SAMSUNG_IPC_FORMAT_RFS:
		skb_queue_tail(&chan->sipc->tx_queue_rfs, skb);
		break;
	case SAMSUNG_IPC_FORMAT_RAW:
		skb_queue_tail(&chan->sipc->tx_queue_raw, skb);
		break;
	default:
		dev_err(chan->sipc->dev, "Don't know how to tx format %d\n", chan->format);
	}

	return count;
}

static ssize_t sipc_misc_read(struct file *filp, char *buf, size_t count,
		loff_t *f_pos)
{
	struct sipc_io_channel *chan = misc_to_chan(filp->private_data);
	struct sk_buff *skb;
	int pktsize;

	skb = skb_dequeue(&chan->rx_queue);
	if (!skb) {
		dev_info(chan->sipc->dev, "No pending RX data\n");
		return 0;
	}

	if (skb->len > count) {
		dev_err(chan->sipc->dev, "Read buffer not big enough for whole packet (%d)\n", skb->len);
		dev_kfree_skb_any(skb);
		return -EFAULT;
	}

	pktsize = skb->len;
	if (copy_to_user(buf, skb->data, pktsize) != 0) {
		dev_kfree_skb_any(skb);
		return -EFAULT;
	}

	dev_kfree_skb_any(skb);

	return pktsize;
}

const struct file_operations sipc_misc_fops = {
	.owner = THIS_MODULE,
	.open = sipc_misc_open,
	.release = sipc_misc_release,
	.poll = sipc_misc_poll,
	//.unlocked_ioctl = sipc_misc_ioctl,
	.write = sipc_misc_write,
	.read = sipc_misc_read,
};
