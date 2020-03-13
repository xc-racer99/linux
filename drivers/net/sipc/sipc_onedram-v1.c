// SPDX-License-Identifier: GPL-2.0+
//
// OneDRAM interface for modems speaking
// Samsung's IPC v4.x protocol
//
// Copyright (C) 2018 Simon Shields <simon@lineageos.org>
// Copyright (C) 2020 Jonathan Bakker <xc-racer2@live.ca>
//
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/sipc.h>
#include <linux/skbuff.h>

enum modem_type {
	STE_M5730,
	XMM_6160,
}

enum onedram_status {
	MODEM_OFF,
	MODEM_CRASHED,
	MODEM_POWER_ON,
	MODEM_BOOTING_NORMAL,
	MODEM_RUNNING,
};

#define modem_offline(ep) ((ep)->status < MODEM_POWER_ON)
#define modem_running(ep) ((ep)->status == MODEM_RUNNING)

struct sipc_onedram_ep {
	struct device *dev;

	enum modem_type type;

	void __iomem *mmio;

	/* lock and waitqueue for shared memory state */
	spinlock_t lock;
	wait_queue_head_t wq;

	/* TODO - needed? lock for read/write/ioctl */
	struct mutex ctl_lock;

	/* shared memory semaphore management */
	unsigned mmio_req_count;
	unsigned mmio_bp_request;
	unsigned mmio_owner;
	unsigned mmio_signal_bits;

	enum onedram_status status;

	int irq_bp;
	int irq_mbox;
	int irq_resout;
	int irq_cp_pwr_rst;

	struct gpio_desc *gpio_phone_active;
	struct gpio_desc *gpio_pda_active;
	struct gpio_desc *gpio_cp_reset;
	struct gpio_desc *gpio_phone_on;
	struct gpio_desc *gpio_resout;
	struct gpio_desc *gpio_cp_pwr_rst;

	struct regulator *cp_rtc_regulator;
	struct regulator *cp_32khz_regulator;

	struct sk_buff_head tx_q;

	struct sipc_link link_ops;
	struct sipc_link_callback *cb;
};
#define linkops_to_ep(ops) container_of(ops, struct sipc_onedram_ep, link_ops)

/* This driver handles modem lifecycle
 * transitions (OFF -> ON -> RUNNING -> ABNORMAL), the firmware
 * download mechanism, and interrupts from
 * the modem (direct and via onedram mailbox interrupt).
 *
 * It also handles tracking the ownership of the onedram "semaphore"
 * which governs which processor (AP or BP) has access to the 16MB
 * shared memory region.  The modem_mmio_{acquire,release,request}
 * primitives are used to obtain access to the shared
 * memory region when necessary to do io.
 *
 * Further, onedram_update_state() and modem_handle_io() are called
 * when we gain control over the shared memory region (to update
 * fifo state info) and when there may be io to process, respectively.
 */

#define WAIT_TIMEOUT                (HZ*5)

/* Called with ep->lock held whenever we gain access
 * to the mmio region.  Updates how much space we have
 */
// TODO - this probably should simply be wake_up(&ep->wq);
// TODO - Probably change fifos to queues of some sort
static void onedram_update_state(struct sipc_onedram_ep *ep)
{
	/* update our idea of space available in fifos */
	ep->fmt_tx.avail = fifo_space(&ep->fmt_tx);
	ep->fmt_rx.avail = fifo_count(&ep->fmt_rx);
	if (ep->fmt_rx.avail)
		pm_stay_awake(ep->cmd_pipe.dev.this_device);
	else
		pm_relax(ep->cmd_pipe.dev.this_device);

	ep->rfs_tx.avail = fifo_space(&ep->rfs_tx);
	ep->rfs_rx.avail = fifo_count(&ep->rfs_rx);
	if (ep->rfs_rx.avail)
		pm_stay_awake(ep->rfs_pipe.dev.this_device);
	else
		pm_relax(ep->rfs_pipe.dev.this_device);

	ep->raw_tx.avail = fifo_space(&ep->raw_tx);
	ep->raw_rx.avail = fifo_count(&ep->raw_rx);

	/* wake up blocked or polling read/write operations */
	wake_up(&ep->wq);
}

void onedram_request_sem(struct sipc_onedram_ep *ep)
{
	writel(MB_COMMAND | MB_VALID | MBC_REQ_SEM,
	       ep->mmio + OFF_MBOX_AP);
}

static inline int mmio_sem(struct sipc_onedram_ep *ep)
{
	return readl(ep->mmio + OFF_SEM) & 1;
}

static int onedram_request_mmio(struct sipc_onedram_ep *ep)
{
	unsigned long flags;
	int ret;
	spin_lock_irqsave(&ep->lock, flags);
	ep->mmio_req_count++;
	ret = ep->mmio_owner;
	if (!ret) {
		if (mmio_sem(ep) == 1) {
			/* surprise! we already have control */
			ret = ep->mmio_owner = 1;
			wake_up(&ep->wq);
			onedram_update_state(ep);
		} else {
			/* ask the modem for mmio access */
			if (modem_running(ep))
				onedram_request_sem(ep);
		}
	}
	/* TODO: timer to retry? */
	spin_unlock_irqrestore(&ep->lock, flags);
	return ret;
}

static void onedram_release_mmio(struct sipc_onedram_ep *ep, unsigned bits)
{
	unsigned long flags;
	spin_lock_irqsave(&ep->lock, flags);
	ep->mmio_req_count--;
	ep->mmio_signal_bits |= bits;
	if ((ep->mmio_req_count == 0) && modem_running(ep)) {
		if (ep->mmio_bp_request) {
			ep->mmio_bp_request = 0;
			writel(0, ep->mmio + OFF_SEM);
			writel(MB_COMMAND | MB_VALID | MBC_RES_SEM,
			       ep->mmio + OFF_MBOX_AP);
		} else if (ep->mmio_signal_bits) {
			writel(0, ep->mmio + OFF_SEM);
			writel(MB_VALID | ep->mmio_signal_bits,
			       ep->mmio + OFF_MBOX_AP);
		}
		ep->mmio_owner = 0;
		ep->mmio_signal_bits = 0;
	}
	spin_unlock_irqrestore(&ep->lock, flags);
}

static int mmio_owner_p(struct sipc_onedram_ep *ep)
{
	unsigned long flags;
	int ret;
	spin_lock_irqsave(&ep->lock, flags);
	ret = ep->mmio_owner || modem_offline(ep);
	spin_unlock_irqrestore(&ep->lock, flags);
	return ret;
}

static int onedram_acquire_mmio(struct sipc_onedram_ep *ep)
{
	if (onedram_request_mmio(ep) == 0) {
		int ret = wait_event_interruptible_timeout(
			ep->wq, mmio_owner_p(ep), 5 * HZ);
		if (ret <= 0) {
			onedram_release_mmio(ep, 0);
			if (ret == 0) {
				pr_err("onedram_acquire_mmio() TIMEOUT\n");
				return -ENODEV;
			} else {
				return -ERESTARTSYS;
			}
		}
	}
	if (!modem_running(ep)) {
		onedram_release_mmio(ep, 0);
		return -ENODEV;
	}
	return 0;
}

static int sipc_start_rx(struct sipc_onedram_ep *ep)
{
	int ret = 0;

	// TODO

	return ret;
}

static int sipc_link_transmit(struct sipc_link *link, struct sk_buff *skb)
{
	struct sipc_onedram_ep *ep = linkops_to_ep(link);
	struct urb *urb;
	int ret;

	// TODO

	return 0;
}

static int sipc_link_open(struct sipc_link *link, int channel, int format)
{
	/* Nothing needed here */
	return 0;
}

static void sipc_set_callbacks(struct sipc_link *link,
		struct sipc_link_callback *cb)
{
	struct sipc_onedram_ep *ep = linkops_to_ep(link);
	ep->cb = cb;
}

static void sipc_onedram_handle_offline(struct sipc_onedram_ep *ep, unsigned cmd)
{
	switch (ep->status) {
	case MODEM_BOOTING_NORMAL:
		if (cmd == MODEM_MSG_BINARY_DONE) {
			pr_info("[MODEM] binary load done\n");

			/* Some modems are poorly implemented and need this written now,
			 * not when MBC_PHONE_START as that is too late
			 */
			if (ep->data->quirks & QUIRK_INIT_END_EARLY)
				writel(MB_VALID | MB_COMMAND | MBC_INIT_END | CP_BOOT_AIRPLANE, ep->mmio + OFF_MBOX_AP);

			ep->status = MODEM_RUNNING;
			wake_up(&ep->wq);
		}
		break;
	}
}

static irqreturn_t sipc_onedram_bp_irq_handler(int irq, void *_ep)
{
	pr_debug("[MODEM] bp_irq()\n");
	return IRQ_HANDLED;

}

static irqreturn_t resout_irq_handler(int irq, void *_ep)
{
	struct sipc_onedram_ep *ep = _ep;

	pr_debug("[MODEM] resout_irq()\n");
	if (!gpiod_get_value(ep->gpio_resout) && gpiod_get_value(ep->gpio_phone_on))
		pm_wakeup_event(ep->dev.this_device, 600 * HZ);

	return IRQ_HANDLED;
}

static irqreturn_t cp_pwr_rst_irq_handler(int irq, void *_ep)
{
	struct sipc_onedram_ep *ep = _ep;

	pr_debug("[MODEM] cp_pwr_rst_irq()\n");
	if (!gpiod_get_value(ep->gpio_cp_pwr_rst) && gpiod_get_value(ep->gpio_phone_on))
		pm_wakeup_event(ep->dev.this_device, 600 * HZ);

	return IRQ_HANDLED;
}

static irqreturn_t sipc_onedram_mbox_irq_handler(int irq, void *_ep)
{
	struct sipc_onedram_ep *ep = _ep;
	unsigned cmd;
	unsigned long flags;

	cmd = readl(ep->mmio + OFF_MBOX_BP);

	if (unlikely(ep->status != MODEM_RUNNING)) {
		sipc_onedram_handle_offline(ep, cmd);
		return IRQ_HANDLED;
	}

	if (!(cmd & MB_VALID)) {
		pr_err("unknown invalid cmd %08x\n", cmd);
		return IRQ_HANDLED;
	}

	spin_lock_irqsave(&ep->lock, flags);

	if (cmd & MB_COMMAND) {
		switch (cmd & 15) {
		case MBC_REQ_SEM:
			if (mmio_sem(ep) == 0) {
				/* Sometimes the modem may ask for the
				 * sem when it already owns it.  Humor
				 * it and ack that request.
				 */
				writel(MB_COMMAND | MB_VALID | MBC_RES_SEM,
				       ep->mmio + OFF_MBOX_AP);
			} else if (ep->mmio_req_count == 0) {
				/* No references? Give it to the modem. */
				onedram_update_state(ep);
				ep->mmio_owner = 0;
				writel(0, ep->mmio + OFF_SEM);
				writel(MB_COMMAND | MB_VALID | MBC_RES_SEM,
				       ep->mmio + OFF_MBOX_AP);
				goto done;
			} else {
				/* Busy now, remember the modem needs it. */
				ep->mmio_bp_request = 1;
				break;
			}
		case MBC_RES_SEM:
			break;
		case MBC_PHONE_START:
			/* TODO: should we avoid sending any other messages
			 * to the modem until this message is received and
			 * acknowledged?
			 */
			writel(MB_COMMAND | MB_VALID |
			       MBC_INIT_END | CP_BOOT_AIRPLANE | AP_OS_ANDROID,
			       ep->mmio + OFF_MBOX_AP);

			/* TODO: probably unsafe to send this back-to-back
			 * with the INIT_END message.
			 */
			/* if somebody is waiting for mmio access... */
			if (ep->mmio_req_count)
				onedram_request_sem(ep);
			break;
		case MBC_RESET:
			pr_err("$$$ MODEM RESET $$$\n");
			ep->status = MODEM_CRASHED;
			wake_up(&ep->wq);
			break;
		case MBC_ERR_DISPLAY: {
			char buf[SIZ_ERROR_MSG + 1];
			int i;
			pr_err("$$$ MODEM ERROR $$$\n");
			ep->status = MODEM_CRASHED;
			wake_up(&ep->wq);
			memcpy(buf, ep->mmio + OFF_ERROR_MSG, SIZ_ERROR_MSG);
			for (i = 0; i < SIZ_ERROR_MSG; i++)
				if ((buf[i] < 0x20) || (buf[1] > 0x7e))
					buf[i] = 0x20;
			buf[i] = 0;
			i--;
			while ((i > 0) && (buf[i] == 0x20))
				buf[i--] = 0;
			pr_err("$$$ %s $$$\n", buf);
			break;
		}
		case MBC_SUSPEND:
			break;
		case MBC_RESUME:
			break;
		}
	} else if (ep->type == STE_M5730 && mmio_sem(ep) == 0) {
		/* Some modems don't automatically release the semaphore
		 * we need to request it when we don't have it
		 */
		onedram_request_sem(ep);
		goto done;
	}

	/* On *any* interrupt from the modem it may have given
	 * us ownership of the mmio hw semaphore.  If that
	 * happens, we should claim the semaphore if we have
	 * threads waiting for it and we should process any
	 * messages that the modem has enqueued in its fifos
	 * by calling modem_handle_io().
	 */
	if (mmio_sem(ep) == 1) {
		if (!ep->mmio_owner) {
			onedram_update_state(ep);
			if (ep->mmio_req_count) {
				ep->mmio_owner = 1;
				wake_up(&ep->wq);
			}
		}

		modem_handle_io(ep);

		/* If we have a signal to send and we're not
		 * hanging on to the mmio hw semaphore, give
		 * it back to the modem and send the signal.
		 * Otherwise this will happen when we give up
		 * the mmio hw sem in onedram_release_mmio().
		 */
		if (ep->mmio_signal_bits && !ep->mmio_owner) {
			writel(0, ep->mmio + OFF_SEM);
			writel(MB_VALID | ep->mmio_signal_bits,
			       ep->mmio + OFF_MBOX_AP);
			ep->mmio_signal_bits = 0;
		}
	}
done:
	spin_unlock_irqrestore(&ep->lock, flags);
	return IRQ_HANDLED;
}


static const struct of_device_id sipc_onedram_of_match[] = {
	{
		.compatible = "samsung,ste-m5730",
		.data = STE_M5730,
	}, {
		.compatible = "samsung,intel-xmm6160",
		.data = XMM_6160,
	}, {
		/* sentinel */
	},
};
MODULE_DEVICE_TABLE(of, sipc_onedram_of_match);

static int sipc_onedram_probe(struct platform_device *pdev)
{
	int ret, i, r;
	struct device_node *np;
	struct resource *res;
	struct device *dev = &pdev->dev;
	struct sipc_onedram_ep *ep;

	np = pdev->dev.of_node;
	if (!np) {
		dev_err(dev, "must be instantiated via DT");
		return -EINVAL;
	}

	ep = devm_kzalloc(dev, sizeof(*ep), GFP_KERNEL);
	if (!ep)
		return -ENOMEM;

	init_waitqueue_head(&ep->wq);
	spin_lock_init(&ep->lock);
	mutex_init(&ep->ctl_lock);

	ep->type = (enum modem_type) of_match_node(sipc_onedram_of_match, np);

	ep->dev = dev;

	ep->gpio_phone_active = devm_gpiod_get(dev, "phone_active", 0);
	if (IS_ERR(ep->gpio_phone_active)) {
		pr_err("no phone_active gpio");
		return PTR_ERR(ep->gpio_phone_active);
	}

	ep->gpio_pda_active = devm_gpiod_get(dev, "pda_active", 0);
	if (IS_ERR(ep->gpio_pda_active)) {
		pr_err("no pda_active gpio");
		return PTR_ERR(ep->gpio_pda_active);
	}

	ep->gpio_cp_reset = devm_gpiod_get(dev, "cp_reset", 0);
	if (IS_ERR(ep->gpio_cp_reset)) {
		pr_err("no cp_reset gpio");
		return PTR_ERR(ep->gpio_cp_reset);
	}

	if (ep->type == STE_M5730) {
		ep->gpio_phone_on = devm_gpiod_get(dev, "phone_on", 0);
		if (IS_ERR(ep->gpio_phone_on)) {
			pr_err("no phone_on gpio");
			return PTR_ERR(ep->gpio_phone_on);
		}

		ep->gpio_resout = devm_gpiod_get(dev, "resout", 0);
		if (IS_ERR(ep->gpio_resout)) {
			pr_err("no resout gpio");
			return IS_ERR(ep->gpio_resout);
		}

		ep->irq_resout = gpiod_to_irq(ep->gpio_resout);
		if (ep->irq_resout < 0) {
			pr_err("no resout irq");
			return ep->irq_resout;
		}

		r = devm_request_irq(dev, ep->irq_resout, resout_irq_handler,
			IRQF_TRIGGER_FALLING, "resout", ep);
		if (r < 0) {
			pr_err("couldn't request resout irq");
			return r;
		}

		enable_irq_wake(ep->irq_resout);

		ep->gpio_cp_pwr_rst = devm_gpiod_get(dev, "cp_pwr_rst", 0);
		if (IS_ERR(ep->gpio_cp_pwr_rst)) {
			pr_err("no cp_pwr_rst gpio");
			return PTR_ERR(ep->gpio_cp_pwr_rst);
		}

		ep->irq_cp_pwr_rst = gpiod_to_irq(ep->gpio_cp_pwr_rst);
		if (ep->irq_cp_pwr_rst < 0) {
			pr_err("no cp_pwr_rst irq");
			return ep->irq_cp_pwr_rst;
		}

		r = devm_request_irq(dev, ep->irq_cp_pwr_rst, cp_pwr_rst_irq_handler,
			IRQF_TRIGGER_FALLING, "cp_pwr_rst", ep);
		if (r < 0) {
			pr_err("failed to request cp_pwr_rst irq");
			return r;
		}

		enable_irq_wake(ep->irq_cp_pwr_rst);
	}

	ep->irq_bp = gpiod_to_irq(ep->gpio_phone_active);
	ep->irq_mbox = platform_get_irq(pdev, 0);

	if (ep->type == STE_M5730) {
		ep->cp_rtc_regulator = devm_regulator_get(dev, "cp_rtc");
		if (IS_ERR_OR_NULL(ep->cp_rtc_regulator))
			return -ENODEV;
		ep->cp_32khz_regulator = devm_regulator_get(dev, "cp_32khz");
		if (IS_ERR_OR_NULL(ep->cp_32khz_regulator))
			return -ENODEV;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENOMEM;
	ep->mmbase = res->start;
	ep->mmsize = resource_size(res);

	ep->mmio = devm_ioremap_nocache(dev, ep->mmbase, ep->mmsize);
	if (!ep->mmio)
		return -EADDRNOTAVAIL;

	platform_set_drvdata(pdev, ep);

	/* hide control registers from userspace */
	ep->mmsize -= 0x800;
	ep->status = MODEM_OFF;

	r = devm_request_irq(dev, ep->irq_bp, sipc_onedram_bp_irq_handler,
			IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
			"sipc_onedram_bp", ep);
	if (r)
		goto err_misc_register;

	r = devm_request_irq(dev, ep->irq_mbox, sipc_onedram_mbox_irq_handler,
			IRQF_TRIGGER_LOW, "sipc_onedram_mbox", ep);
	if (r)
		goto err_misc_register;

	enable_irq_wake(ep->irq_bp);
	enable_irq_wake(ep->irq_mbox);

	device_init_wakeup(ep->dev.this_device, true);

	ep->link_ops.transmit = sipc_link_transmit;
	ep->link_ops.open = sipc_link_open;
	ep->link_ops.set_callbacks = sipc_set_callbacks;

	for (i = SAMSUNG_IPC_FORMAT_FMT; i < SAMSUNG_IPC_FORMAT_MULTI_RAW; i++) {
		ret = sipc_set_link(&ep->link_ops, i);
		if (ret < 0) {
			dev_err(&intf->dev,
				"Fail setting SIPC link for fmt %u", i);
			return ret;
		}
	}

	return sipc_start_rx(ep);
}

static int sipc_onedram_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	// TODO

	return 0;
}

static int sipc_onedram_suspend(struct device *pdev)
{
	struct sipc_onedram_ep *ep = dev_get_drvdata(pdev);

	gpiod_set_value(ep->gpio_pda_active, 0);

	return 0;
}

static int sipc_onedram_resume(struct device *pdev)
{
	struct sipc_onedram_ep *ep = dev_get_drvdata(pdev);

	gpiod_set_value(ep->gpio_pda_active, 1);

	return 0;
}

static const struct dev_pm_ops sipc_onedram_pm_ops = {
	.suspend    = sipc_onedram_suspend,
	.resume     = sipc_onedram_resume,
};

static struct platform_driver sipc_onedram_driver = {
	.probe = sipc_onedram_probe,
	.remove = sipc_onedram_remove,
	.driver = {
		.name = "sipc_onedram",
		.of_match_table = of_match_ptr(sipc_onedram_of_match),
		.pm   = &sipc_onedram_pm_ops,
	},
};

static int __init sipc_onedram_init(void)
{
	return platform_driver_register(&sipc_onedram_driver);
}

module_init(sipc_onedram_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Samsung OneDRAM SIPC Driver");
MODULE_AUTHOR("Jonathan Bakker <xc-racer2@live.ca>");
