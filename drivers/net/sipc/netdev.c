// SPDX-License-Identifier: GPL-2.0+
//
// Net device code for Samsung IPC v4.x modems.
//
// Copyright (C) 2018 Simon Shields <simon@lineageos.org>
//
#include <dt-bindings/net/samsung_ipc.h>
#include <linux/if_arp.h>
#include <linux/netdevice.h>
#include <linux/sipc.h>

#include "sipc.h"

static int sipc_netdev_open(struct net_device *ndev)
{
	struct sipc_netdev_priv *priv = netdev_priv(ndev);
	netif_start_queue(ndev);
	atomic_inc(&priv->chan->use_count);
	return 0;
}

static int sipc_netdev_stop(struct net_device *ndev)
{
	struct sipc_netdev_priv *priv = netdev_priv(ndev);
	atomic_dec(&priv->chan->use_count);
	netif_stop_queue(ndev);
	return 0;
}

static int sipc_netdev_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct sipc_netdev_priv *priv = netdev_priv(ndev);
	struct samsung_ipc *sipc = priv->chan->sipc;
	struct raw_header raw_hdr;
	struct sk_buff *new_skb;
	const u8 hdlc_start = HDLC_START;
	const u8 hdlc_end = HDLC_END;
	int headroom, ret, tailroom;

	raw_hdr.channel = priv->chan->channel & 0x1f;
	raw_hdr.len = skb->len + sizeof(raw_hdr);
	raw_hdr.control = 0;

	headroom = sizeof(HDLC_START) + sizeof(raw_hdr);
	tailroom = sizeof(HDLC_END);

	if (skb_headroom(skb) < headroom || skb_tailroom(skb) < tailroom) {
		new_skb = skb_copy_expand(skb, headroom, tailroom, GFP_ATOMIC);
		dev_kfree_skb_any(skb);
		if (!new_skb)
			return -ENOMEM;
		skb = new_skb;
	}

	memcpy(skb_push(skb, sizeof(raw_hdr)), &raw_hdr, sizeof(raw_hdr));
	memcpy(skb_push(skb, sizeof(HDLC_START)), &hdlc_start, sizeof(HDLC_START));
	memcpy(skb_put(skb, sizeof(HDLC_END)), &hdlc_end, sizeof(HDLC_END));

	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += skb->len;

	skb_queue_tail(&sipc->tx_queue_raw, skb);

	return NETDEV_TX_OK;
}

static struct net_device_ops sipc_netdevice_ops = {
	.ndo_open = sipc_netdev_open,
	.ndo_stop = sipc_netdev_stop,
	.ndo_start_xmit = sipc_netdev_xmit,
};

void sipc_netdev_setup(struct net_device *ndev)
{
	ndev->netdev_ops = &sipc_netdevice_ops;
	ndev->type = ARPHRD_PPP;
	ndev->flags = IFF_POINTOPOINT | IFF_NOARP | IFF_MULTICAST;
	ndev->addr_len = 0;
	ndev->hard_header_len = 0;
	ndev->tx_queue_len = 1000;
	ndev->mtu = ETH_DATA_LEN;
	/* FIXME
	ndev->watchdog_timeo = 5 * HZ; */
}
