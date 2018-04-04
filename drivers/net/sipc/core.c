// SPDX-License-Identifier: GPL-2.0+
//
// Common code for Samsung IPC v4.x modems.
//
// Copyright (C) 2018 Simon Shields <simon@lineageos.org>
//
#include <dt-bindings/net/samsung_ipc.h>
#include <linux/miscdevice.h>
#include <linux/netdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sipc.h>

#include "sipc.h"

#define LINK_CMD_STOP_RAW ((unsigned short)(0x00ca))
#define LINK_CMD_START_RAW ((unsigned short)(0x00cb))

/*
 * how this should work:
 * DT has io devices: format, name, channel, io type
 * this module registers the interfaces and provides an API for USB devices
 * (and eventually others) to provide the transport layer.
 *
 * USB module only needs to know channel ID and format.
 *
 * TODO: check 3.x kernel and see how many instances of hsic
 * link device are made.
 */

struct sipc_link *cur_links[SAMSUNG_IPC_FORMAT_MAX] = {NULL};
struct sipc_link_callback *callbacks = NULL;

int sipc_set_link(struct sipc_link *link, unsigned int type)
{
	if (type >= SAMSUNG_IPC_FORMAT_MAX)
		return -EINVAL;

	if (cur_links[type])
		return -EBUSY;

	if (!callbacks)
		return -EPROBE_DEFER;

	link->set_callbacks(link, callbacks);
	cur_links[type] = link;

	return 0;
}
EXPORT_SYMBOL_GPL(sipc_set_link);

void sipc_clear_link(unsigned int type)
{
	if (type >= SAMSUNG_IPC_FORMAT_MAX)
		return;

	cur_links[type] = NULL;
}
EXPORT_SYMBOL_GPL(sipc_clear_link);

static struct sipc_io_channel *find_io_channel(struct samsung_ipc *sipc, int channel,
		int format)
{
	int i;

	for (i = 0; i < sipc->nchannels; i++) {
		if (sipc->channels[i].format == format) {
			if (channel == -1 || sipc->channels[i].channel == channel)
				return &sipc->channels[i];
		}
	}
	return NULL;
}

int sipc_get_header_size(int format) {
	switch (format) {
	case SAMSUNG_IPC_FORMAT_FMT:
		return sizeof(struct fmt_header);
	case SAMSUNG_IPC_FORMAT_RAW:
	case SAMSUNG_IPC_FORMAT_MULTI_RAW:
		return sizeof(struct raw_header);
	case SAMSUNG_IPC_FORMAT_RFS:
		return sizeof(struct rfs_header);
	default:
		return 0;
	}
}

static int sipc_hdlc_header_check(struct hdlc_header *hdr, char *buf, size_t bufsz, int format)
{
	int len = 0;
	int done = 0;
	int head_size = sipc_get_header_size(format);

	/* first frame */
	if (!hdr->start) {
		if (buf[0] == HDLC_START)
			len = 1;
		else
			return -EBADMSG;

		hdr->start = HDLC_START;
		hdr->len = 0;

		buf++;
		done++;
		bufsz--;
	}

	if (hdr->len < head_size) {
		len = min(bufsz, head_size - hdr->len);
		memcpy(&hdr->sipc_header, buf, len);
		hdr->len += len;
		done += len;
	}

	return done;
}

static int sipc_get_message_size(struct sipc_io_channel *chan)
{
	switch (chan->format) {
	case SAMSUNG_IPC_FORMAT_FMT:
		if (chan->sipc->version == SAMSUNG_IPC_VERSION_42)
			return chan->pending_rx_header.sipc_header.fmt.len & 0x3fff;
		return chan->pending_rx_header.sipc_header.fmt.len;
	case SAMSUNG_IPC_FORMAT_RAW:
	case SAMSUNG_IPC_FORMAT_MULTI_RAW:
		return chan->pending_rx_header.sipc_header.raw.len;
	case SAMSUNG_IPC_FORMAT_RFS:
		return chan->pending_rx_header.sipc_header.rfs.len;
	default:
		return 0;
	}
}


/*
 * Demultiplex multiplexed network data.
 */
static int do_raw_rx(struct sk_buff *skb, struct sipc_io_channel *chan)
{
	u8 id;
	struct sipc_io_channel *real_chan;
	struct raw_header *hdr = &chan->pending_rx_header.sipc_header.raw;

	id = hdr->channel;

	real_chan = find_io_channel(chan->sipc, 0x20 | id, SAMSUNG_IPC_FORMAT_RAW);
	if (!real_chan) {
		dev_err(chan->sipc->dev, "Invalid raw multipdp channel %#x\n", id);
		return -ENODEV;
	}

	skb_queue_tail(&real_chan->rx_queue, skb);
	// schedule_delayed_work
	return 0;
}

#define SIPC_FMT_MORE_FRAMES (1 << 7)
static int do_fmt_rx(struct sk_buff *rx_skb, struct sipc_io_channel *chan)
{
	struct fmt_header *hdr = &chan->pending_rx_header.sipc_header.fmt;
	u8 id = hdr->control & 0x7f;
	struct sk_buff *skb = chan->fmt_skb[id];
	char *data = rx_skb->data;
	unsigned int len = rx_skb->len;

	if (!skb) {
		/* no other frame with this ID so far */
		if (!(hdr->control & SIPC_FMT_MORE_FRAMES)) {
			/* no more frames, just queue this buff as-is */
			skb_queue_tail(&chan->rx_queue, rx_skb);
			wake_up(&chan->wq);
			return 0;
		} else {
			skb = alloc_skb(MAX_MULTI_RX_SIZE, GFP_KERNEL);
			if (!skb)
				return -ENOMEM;

			chan->fmt_skb[id] = skb;

			if (len < sizeof(*hdr)) {
				dev_err(chan->sipc->dev, "Header too short!\n");
				return -EINVAL;
			}
			hdr = (struct fmt_header *)data;
		}
	}

	memcpy(skb_put(skb, len), data, len);
	dev_kfree_skb_any(rx_skb);

	if (hdr->control & SIPC_FMT_MORE_FRAMES) {
		/* last frame isn't here yet */
		return 0;
	}

	skb_queue_tail(&chan->rx_queue, skb);
	chan->fmt_skb[id] = NULL;
	wake_up(&chan->wq);
	return 0;
}

static int sipc_do_rx(struct sk_buff *skb, struct sipc_io_channel *chan)
{
	switch (chan->format) {
	case SAMSUNG_IPC_FORMAT_FMT:
		if (chan->sipc->version == SAMSUNG_IPC_VERSION_42)
			dev_warn(chan->sipc->dev, "Don't support IPC version 42 yet\n");
		else
			return do_fmt_rx(skb, chan);
		return 0;
	case SAMSUNG_IPC_FORMAT_MULTI_RAW:
		return do_raw_rx(skb, chan);
	default:
		skb_queue_tail(&chan->rx_queue, skb);
		wake_up(&chan->wq);
		return 0;
	}

}

static void sipc_rx_cmd(struct samsung_ipc *sipc, void *buf, size_t bufsz)
{
	unsigned short *cmd;
	unsigned short *end = buf + bufsz;
	int i;

	for (cmd = (unsigned short *)buf; cmd < end; cmd++) {
		switch (*cmd) {
		case LINK_CMD_STOP_RAW:
			for (i = 0; i < sipc->nchannels; i++) {
				if (sipc->channels[i].netdev && sipc->channels[i].type == SAMSUNG_IPC_TYPE_NETDEV) {
					netif_stop_queue(sipc->channels[i].netdev);
				}
			}

			reinit_completion(&sipc->raw_tx_resumed);
			sipc->raw_tx_suspended = true;
			break;
		case LINK_CMD_START_RAW:
			for (i = 0; i < sipc->nchannels; i++) {
				if (sipc->channels[i].netdev && sipc->channels[i].type == SAMSUNG_IPC_TYPE_NETDEV) {
					netif_start_queue(sipc->channels[i].netdev);
				}
			}

			sipc->raw_tx_suspended = false;
			complete_all(&sipc->raw_tx_resumed);
			break;
		default:
			dev_info(sipc->dev, "Unknown flow control command %#x\n", *cmd);
			break;
		}
	}
}

static int sipc_do_tx(struct samsung_ipc *sipc, struct sk_buff *skb, int format) {
	struct sipc_link *link;
	int ret;

	if (format >= SAMSUNG_IPC_FORMAT_MAX || !cur_links[format]) {
		ret = -ENODEV;
		goto exit;
	}

	if (format == SAMSUNG_IPC_FORMAT_RAW || format == SAMSUNG_IPC_FORMAT_MULTI_RAW) {
		if (unlikely(sipc->raw_tx_suspended)) {
			if (in_irq()) {
				return -EBUSY;
			}
		}
		wait_for_completion(&sipc->raw_tx_resumed);
	}

	link = cur_links[format];
	ret = link->transmit(link, skb);

exit:
	if (ret == -ENODEV || ret == -ENOENT) {
		dev_kfree_skb_any(skb);
	}
	return ret;
}

static void sipc_tx_work(struct work_struct *work) {
	struct samsung_ipc *sipc = container_of(work, struct samsung_ipc, tx_work.work);
	struct sk_buff *skb;
	int ret = 0;

	while (sipc->tx_queue_rfs.qlen || sipc->tx_queue_fmt.qlen || sipc->tx_queue_raw.qlen) {
		// TODO: runtime PM
		skb = skb_dequeue(&sipc->tx_queue_rfs);
		if (skb)
			ret = sipc_do_tx(sipc, skb, SAMSUNG_IPC_FORMAT_RFS);
		if (ret) {
			if (ret == -ENODEV || ret == -ENOENT)
				break;
			skb_queue_head(&sipc->tx_queue_rfs, skb);
			break;
		}

		skb = skb_dequeue(&sipc->tx_queue_fmt);
		if (skb)
			ret = sipc_do_tx(sipc, skb, SAMSUNG_IPC_FORMAT_FMT);
		if (ret) {
			if (ret == -ENODEV || ret == -ENOENT)
				break;
			skb_queue_head(&sipc->tx_queue_fmt, skb);
			break;
		}

		skb = skb_dequeue(&sipc->tx_queue_raw);
		if (skb)
			ret = sipc_do_tx(sipc, skb, SAMSUNG_IPC_FORMAT_RAW);
		if (ret) {
			if (ret == -ENODEV || ret == -ENOENT)
				break;
			skb_queue_head(&sipc->tx_queue_raw, skb);
			break;
		}
	}

	if (ret == -ENODEV || ret == -ENOENT || ret >= 0)
		return;

	queue_delayed_work(sipc->tx_wq, &sipc->tx_work, msecs_to_jiffies(20));
}

static void sipc_receive_callback(struct sipc_link_callback *cb, void *b,
		size_t bufsz, int format)
{
	struct samsung_ipc *sipc = cb_to_ipc(cb);
	struct sipc_io_channel *chan = find_io_channel(sipc, -1, format);
	struct sk_buff *skb = NULL;
	size_t len, done, packet_size;
	int header_size = sipc_get_header_size(format);
	size_t data_size, rest_size;
	char *buf = (char *)b;

	if (format == SAMSUNG_IPC_FORMAT_CMD) {
		sipc_rx_cmd(sipc, buf, bufsz);
		return;
	}

	if (!chan) {
		dev_err(sipc->dev, "Couldn't find channel with format=%d. Dropping packet!\n", format);
		return;
	}

	/* don't check header if we're waiting for more data */
	if (chan->pending_rx_header.frag_len)
		goto skip_header_check;

next_frame:
	len = sipc_hdlc_header_check(&chan->pending_rx_header, buf, bufsz, format);
	if (len < 0) {
		dev_err(sipc->dev, "Invalid message format=%d\n", format);
		return;
	}

	buf += len;
	done += len;
	bufsz -= len;

	if (!bufsz)
		return;

skip_header_check:
	data_size = sipc_get_message_size(chan) - header_size;
	rest_size = data_size - chan->pending_rx_header.frag_len;
	/* TODO: what happens if we fail to allocate skbuff? */
	if (!chan->pending_rx_skb) {
		len = min(data_size, (size_t)MAX_RX_SIZE);
		len = min(len, rest_size);

		if (format == SAMSUNG_IPC_FORMAT_RFS) {
			/* for RFS: separate header SKB */
			skb = alloc_skb(header_size, GFP_KERNEL);
			if (!skb) {
				dev_err(sipc->dev, "Out of memory\n");
				return;
			}
			memcpy(skb_put(skb, header_size), &chan->pending_rx_header.sipc_header.rfs, header_size);
			
			sipc_do_rx(skb, chan);
		}

		skb = alloc_skb(len, GFP_KERNEL);
		if (!skb) {
			dev_err(sipc->dev, "Out of memory\n");
			return;
		}

		chan->pending_rx_skb = skb;
	}

	/* line 331, sipc4_io_device.c */
	while (bufsz > 0) {
		len = min(bufsz, rest_size);
		len = min(len, (size_t)skb_tailroom(chan->pending_rx_skb));
		len = min(len, (size_t)MAX_RX_SIZE);

		memcpy(skb_put(chan->pending_rx_skb, len), buf, len);

		buf += len;
		done += len;
		bufsz -= len;
		rest_size -= len;
		chan->pending_rx_header.frag_len += len;

		/* done receiving */
		if (!bufsz || !rest_size)
			break;

		sipc_do_rx(chan->pending_rx_skb, chan);

		len = min(rest_size, (size_t)MAX_RX_SIZE);
		skb = alloc_skb(len, GFP_KERNEL);
		if (!skb) {
			dev_err(sipc->dev, "Out of memory\n");
			return;
		}

		chan->pending_rx_skb = skb;
	}

	dev_info(sipc->dev, "Processed %d bytes\n", done);

	if (!bufsz && chan->pending_rx_header.frag_len) {
		/* Still waiting for end-of-packet. It will come in the next frame */
		return;
	}

	if (buf[0] != HDLC_END) {
		dev_err(sipc->dev, "Invalid HDLC end-of-frame %#x\n", buf[0]);
		return;
	}

	buf += sizeof(HDLC_END);
	len -= sizeof(HDLC_END);

	packet_size = sipc_get_message_size(chan) + sizeof(HDLC_END) + sizeof(HDLC_START);

	sipc_do_rx(chan->pending_rx_skb, chan);

	chan->pending_rx_skb = NULL;
	memset(&chan->pending_rx_header, 0, sizeof(struct hdlc_header));

	/* Still more data to receive in this frame */
	if (bufsz)
		goto next_frame;

	/* TODO: clean up properly on error */
	/*return done;*/
}

static int sipc_parse_dt(struct platform_device *pdev,
		struct samsung_ipc *sipc)
{
	int count, ret;
	int i = 0;
	struct device_node *np = pdev->dev.of_node, *child;

	count = of_get_available_child_count(np);
	if (count == 0) {
		dev_err(&pdev->dev, "No channels!\n");
		return -EINVAL;
	}

	sipc->channels = devm_kzalloc(&pdev->dev, sizeof(*sipc->channels) * count, GFP_KERNEL);

	ret = of_property_read_u32(child, "protocol", &sipc->version);
	if (ret != 0) {
		dev_warn(&pdev->dev, "Failed to read protocol version, assuming v4.0: %d\n", ret);
		sipc->version = SAMSUNG_IPC_VERSION_40;
	}

	for_each_available_child_of_node(np, child) {

		ret = of_property_read_u32(child, "reg", &sipc->channels[i].channel);
		if (ret != 0 || !sipc->channels[i].channel) {
			dev_err(&pdev->dev, "Couldn't read channel number: %d\n",
					ret);
			count--;
			continue;
		}

		ret = of_property_read_u32(child, "type", &sipc->channels[i].type);
		if (ret != 0 || sipc->channels[i].type >= SAMSUNG_IPC_TYPE_MAX) {
			dev_err(&pdev->dev, "Coulnd't read channel type: %d\n", ret);
			count--;
			continue;
		}

		ret = of_property_read_u32(child, "format", &sipc->channels[i].format);
		if (ret != 0 || sipc->channels[i].format >= SAMSUNG_IPC_FORMAT_MAX) {
			dev_err(&pdev->dev, "Couldn't read channel format: %d\n", ret);
			count--;
			continue;
		}

		ret = of_property_read_string(child, "label", &sipc->channels[i].name);
		if (ret != 0) {
			dev_err(&pdev->dev, "Couldn't read channel name: %d\n", ret);
			count--;
			continue;
		}

		i++;
	}
	sipc->nchannels = count;

	return 0;
}

static int sipc_probe(struct platform_device *pdev)
{
	struct samsung_ipc *sipc;
	struct sipc_io_channel *chan;
	struct sipc_netdev_priv *priv;
	int ret, i;

	sipc = devm_kzalloc(&pdev->dev, sizeof(*sipc), GFP_KERNEL);
	if (!sipc)
		return -ENOMEM;

	sipc->dev = &pdev->dev;

	ret = sipc_parse_dt(pdev, sipc);
	if (ret < 0)
		return ret;

	platform_set_drvdata(pdev, sipc);

	sipc->link_cb.receive = sipc_receive_callback;

	callbacks = &sipc->link_cb;

	skb_queue_head_init(&sipc->tx_queue_fmt);
	skb_queue_head_init(&sipc->tx_queue_rfs);
	skb_queue_head_init(&sipc->tx_queue_raw);

	init_completion(&sipc->raw_tx_resumed);
	INIT_DELAYED_WORK(&sipc->tx_work, sipc_tx_work);
	sipc->tx_wq = create_singlethread_workqueue("sipc_tx_wq");

	for (i = 0; i < sipc->nchannels; i++) {
		chan = &sipc->channels[i];
		chan->sipc = sipc;
		switch (chan->type) {
		case SAMSUNG_IPC_TYPE_MISC:
			init_waitqueue_head(&chan->wq);
			skb_queue_head_init(&chan->rx_queue);

			chan->miscdev.minor = MISC_DYNAMIC_MINOR;
			chan->miscdev.name = chan->name;
			chan->miscdev.fops = &sipc_misc_fops;

			ret = misc_register(&chan->miscdev);
			if (ret)
				dev_err(sipc->dev, "Failed to register misc device '%s': %d\n", chan->name,
						ret);
			break;
		case SAMSUNG_IPC_TYPE_NETDEV:
			skb_queue_head_init(&chan->rx_queue);
			chan->netdev = alloc_netdev(sizeof(struct sipc_netdev_priv),
					chan->name, NET_NAME_UNKNOWN, sipc_netdev_setup);

			if (!chan->netdev) {
				dev_err(sipc->dev, "Failed to alloc netdev %s\n", chan->name);
				return -ENOMEM;
			}

			ret = register_netdev(chan->netdev);
			if (ret) {
				free_netdev(chan->netdev);
				break;
			}

			priv = netdev_priv(chan->netdev);
			priv->chan = chan;

			break;
		case SAMSUNG_IPC_TYPE_DUMMY:
		default:
			break;
		}
	}
	return 0;
}

static int sipc_remove(struct platform_device *pdev)
{
	int i;
	struct sipc_io_channel *chan;
	struct samsung_ipc *sipc = platform_get_drvdata(pdev);

	for (i = 0; i < SAMSUNG_IPC_FORMAT_MAX; i++) {
		if (cur_links[i])
			cur_links[i]->set_callbacks(cur_links[i], NULL);
	}

	destroy_workqueue(sipc->tx_wq);
	for (i = 0; i < sipc->nchannels; i++) {
		chan = &sipc->channels[i];
		switch (chan->type) {
		case SAMSUNG_IPC_TYPE_MISC:
			misc_deregister(&chan->miscdev);
			break;
		case SAMSUNG_IPC_TYPE_NETDEV:
			unregister_netdev(chan->netdev);
			free_netdev(chan->netdev);
			break;
		}
	}
	return 0;
}

static const struct of_device_id sipc_of_match[] = {
	{
		.compatible = "samsung,sipc4-modem",
	},
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, sipc_of_match);

static struct platform_driver sipc_driver = {
	.probe = sipc_probe,
	.remove = sipc_remove,
	.driver = {
		.name = "samsung_ipc",
		.of_match_table = of_match_ptr(sipc_of_match),
	},
};

module_platform_driver(sipc_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Simon Shields <simon@lineageos.org>");
