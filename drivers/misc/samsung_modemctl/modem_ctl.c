/* modem_ctl.c
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
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/miscdevice.h>
#include <linux/of.h>

#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/irq.h>
#include <linux/delay.h>
#include <linux/regulator/consumer.h>

#include "modem_ctl.h"
#include "modem_ctl_p.h"

/* The modem_ctl portion of this driver handles modem lifecycle
 * transitions (OFF -> ON -> RUNNING -> ABNORMAL), the firmware
 * download mechanism (via /dev/modem_ctl), and interrupts from
 * the modem (direct and via onedram mailbox interrupt).
 *
 * It also handles tracking the ownership of the onedram "semaphore"
 * which governs which processor (AP or BP) has access to the 16MB
 * shared memory region.  The modem_mmio_{acquire,release,request}
 * primitives are used by modem_io.c to obtain access to the shared
 * memory region when necessary to do io.
 *
 * Further, modem_update_state() and modem_handle_io() are called
 * when we gain control over the shared memory region (to update
 * fifo state info) and when there may be io to process, respectively.
 *
 */

#define WAIT_TIMEOUT                (HZ*5)

void modem_request_sem(struct modemctl *mc)
{
	writel(MB_COMMAND | MB_VALID | MBC_REQ_SEM,
	       mc->mmio + OFF_MBOX_AP);
}

static inline int mmio_sem(struct modemctl *mc)
{
	return readl(mc->mmio + OFF_SEM) & 1;
}

int modem_request_mmio(struct modemctl *mc)
{
	unsigned long flags;
	int ret;
	spin_lock_irqsave(&mc->lock, flags);
	mc->mmio_req_count++;
	ret = mc->mmio_owner;
	if (!ret) {
		if (mmio_sem(mc) == 1) {
			/* surprise! we already have control */
			ret = mc->mmio_owner = 1;
			wake_up(&mc->wq);
			modem_update_state(mc);
			MODEM_COUNT(mc,request_no_wait);
		} else {
			/* ask the modem for mmio access */
			if (modem_running(mc))
				modem_request_sem(mc);
			MODEM_COUNT(mc,request_wait);
		}
	} else {
		MODEM_COUNT(mc,request_no_wait);
	}
	/* TODO: timer to retry? */
	spin_unlock_irqrestore(&mc->lock, flags);
	return ret;
}

void modem_release_mmio(struct modemctl *mc, unsigned bits)
{
	unsigned long flags;
	spin_lock_irqsave(&mc->lock, flags);
	mc->mmio_req_count--;
	mc->mmio_signal_bits |= bits;
	if ((mc->mmio_req_count == 0) && modem_running(mc)) {
		if (mc->mmio_bp_request) {
			mc->mmio_bp_request = 0;
			writel(0, mc->mmio + OFF_SEM);
			writel(MB_COMMAND | MB_VALID | MBC_RES_SEM,
			       mc->mmio + OFF_MBOX_AP);
			MODEM_COUNT(mc,release_bp_waiting);
		} else if (mc->mmio_signal_bits) {
			writel(0, mc->mmio + OFF_SEM);
			writel(MB_VALID | mc->mmio_signal_bits,
			       mc->mmio + OFF_MBOX_AP);
			MODEM_COUNT(mc,release_bp_signaled);
		} else {
			MODEM_COUNT(mc,release_no_action);
		}
		mc->mmio_owner = 0;
		mc->mmio_signal_bits = 0;
	}
	spin_unlock_irqrestore(&mc->lock, flags);
}

static int mmio_owner_p(struct modemctl *mc)
{
	unsigned long flags;
	int ret;
	spin_lock_irqsave(&mc->lock, flags);
	ret = mc->mmio_owner || modem_offline(mc);
	spin_unlock_irqrestore(&mc->lock, flags);
	return ret;
}

int modem_acquire_mmio(struct modemctl *mc)
{
	if (modem_request_mmio(mc) == 0) {
		int ret = wait_event_interruptible_timeout(
			mc->wq, mmio_owner_p(mc), 5 * HZ);
		if (ret <= 0) {
			modem_release_mmio(mc, 0);
			if (ret == 0) {
				pr_err("modem_acquire_mmio() TIMEOUT\n");
				return -ENODEV;
			} else {
				return -ERESTARTSYS;
			}
		}
	}
	if (!modem_running(mc)) {
		modem_release_mmio(mc, 0);
		return -ENODEV;
	}
	return 0;
}

static int modemctl_open(struct inode *inode, struct file *filp)
{
	struct modemctl *mc = to_modemctl(filp->private_data);
	filp->private_data = mc;

	if (mc->open_count)
		return -EBUSY;

	mc->open_count++;
	return 0;
}

static int modemctl_release(struct inode *inode, struct file *filp)
{
	struct modemctl *mc = filp->private_data;

	mc->open_count = 0;
	filp->private_data = NULL;
	return 0;
}

static ssize_t modemctl_read(struct file *filp, char __user *buf,
			     size_t count, loff_t *ppos)
{
	struct modemctl *mc = filp->private_data;
	loff_t pos;
	int ret;

	mutex_lock(&mc->ctl_lock);
	pos = mc->ramdump_pos;
	if (mc->status != MODEM_DUMPING) {
		pr_err("[MODEM] not in ramdump mode\n");
		ret = -ENODEV;
		goto done;
	}
	if (pos < 0) {
		ret = -EINVAL;
		goto done;
	}
	if (pos >= mc->ramdump_size) {
		pr_err("[MODEM] ramdump EOF\n");
		ret = 0;
		goto done;
	}
	if (count > mc->ramdump_size - pos)
		count = mc->ramdump_size - pos;

	ret = copy_to_user(buf, mc->mmio + pos, count);
	if (ret) {
		ret = -EFAULT;
		goto done;
	}
	pos += count;
	ret = count;

	if (pos == mc->ramdump_size) {
		if (mc->ramdump_size == RAMDUMP_LARGE_SIZE) {
			mc->ramdump_size = 0;
			pr_info("[MODEM] requesting more ram\n");
			writel(0, mc->mmio + OFF_SEM);
			writel(MODEM_CMD_RAMDUMP_MORE, mc->mmio + OFF_MBOX_AP);
			wait_event_timeout(mc->wq, mc->ramdump_size != 0, 10 * HZ);
		} else {
			pr_info("[MODEM] no more ram to dump\n");
			mc->ramdump_size = 0;
		}
		mc->ramdump_pos = 0;
	} else {
		mc->ramdump_pos = pos;
	}
	
done:
	mutex_unlock(&mc->ctl_lock);
	return ret;

}

static ssize_t modemctl_write(struct file *filp, const char __user *buf,
			      size_t count, loff_t *ppos)
{
	struct modemctl *mc = filp->private_data;
	u32 owner;
	char *data;
	loff_t pos = *ppos;
	unsigned long ret;

	mutex_lock(&mc->ctl_lock);
	data = (char __force *)mc->mmio + pos;
	owner = mmio_sem(mc);

	if (mc->status != MODEM_POWER_ON) {
		pr_err("modemctl_write: modem not powered on\n");
		ret = -EINVAL;
		goto done;
	}

	if (!owner) {
		pr_err("modemctl_write: doesn't own semaphore\n");
		ret = -EIO;
		goto done;
	}

	if (pos < 0) {
		ret = -EINVAL;
		goto done;
	}

	if (pos >= mc->mmsize) {
		ret = -EINVAL;
		goto done;
	}

	if (count > mc->mmsize - pos)
		count = mc->mmsize - pos;

	ret = copy_from_user(data, buf, count);
	if (ret) {
		ret = -EFAULT;
		goto done;
	}
	*ppos = pos + count;
	ret = count;

done:
	mutex_unlock(&mc->ctl_lock);
	return ret;
}

static int modem_wait_for_sbl(struct modemctl *mc)
{
	pr_info("[MODEM] modem_wait_for_sbl()\n");

	while (readl(mc->mmio + OFF_MBOX_BP) != MODEM_MSG_SBL_DONE) {
		pr_info("[MODEM] SBL not done yet...");
		msleep(5);
	}

	while (mmio_sem(mc) != 1) {
		pr_info("[MODEM] Doesn't own semaphore");
		msleep(5);
	}

	return 0;
}

static int modem_binary_load(struct modemctl *mc)
{
	int ret;

	pr_info("[MODEM] modem_load_binary\n");

	writel(0, mc->mmio + OFF_SEM);
	pr_err("onedram: write_sem 0");

	mc->status = MODEM_BOOTING_NORMAL;
	writel(MODEM_CMD_BINARY_LOAD, mc->mmio + OFF_MBOX_AP);
	pr_err("onedram: send %x\n", MODEM_CMD_BINARY_LOAD);

	ret = wait_event_timeout(mc->wq,
				modem_running(mc), 25 * HZ);
	if (ret == 0)
		return -ENODEV;

	return 0;
}

static int modem_start(struct modemctl *mc, int ramdump)
{
	int ret;

	pr_info("[MODEM] modem_start() %s\n",
		ramdump ? "ramdump" : "normal");

	if (mc->status != MODEM_POWER_ON) {
		pr_err("[MODEM] modem not powered on\n");
		return -EINVAL;
	}

	if (readl(mc->mmio + OFF_MBOX_BP) != MODEM_MSG_SBL_DONE) {
		pr_err("[MODEM] bootloader not ready\n");
		return -EIO;
	}

	writel(0, mc->mmio + OFF_SEM);
	if (ramdump) {
		mc->status = MODEM_BOOTING_RAMDUMP;
		mc->ramdump_size = 0;
		mc->ramdump_pos = 0;
		writel(MODEM_CMD_RAMDUMP_START, mc->mmio + OFF_MBOX_AP);

		ret = wait_event_timeout(mc->wq, mc->status == MODEM_DUMPING, 25 * HZ);
		if (ret == 0)
			return -ENODEV;
	} else {
		mc->status = MODEM_BOOTING_NORMAL;
		writel(MODEM_CMD_BINARY_LOAD, mc->mmio + OFF_MBOX_AP);

		ret = wait_event_timeout(mc->wq, modem_running(mc), 25 * HZ);
		if (ret == 0)
			return -ENODEV;
	}

	pr_info("[MODEM] modem_start() DONE\n");
	return 0;
}

static int modem_reset(struct modemctl *mc)
{
	pr_info("[MODEM] modem_reset()\n");

	/* ensure pda active pin set to low */
	gpiod_set_value(mc->gpio_pda_active, 0);

	/* read inbound mbox to clear pending IRQ */
	(void) readl(mc->mmio + OFF_MBOX_BP);

	/* write outbound mbox to assert outbound IRQ */
	writel(0, mc->mmio + OFF_MBOX_AP);

	if (mc->variant == STE_M5730) {
		/* ensure cp_reset pin set to low */
		gpiod_set_value(mc->gpio_cp_reset, 0);
		msleep(100);

		gpiod_set_value(mc->gpio_phone_on, 1);
		msleep(18);

		regulator_set_voltage(mc->cp_rtc_regulator, 1800000, 1800000);

		if (!regulator_is_enabled(mc->cp_rtc_regulator)) {
			if (regulator_enable(mc->cp_rtc_regulator)) {
				pr_err("Failed to enable CP_RTC_1.8V regulator.\n");
				return -1;
			}
		}

		if (!regulator_is_enabled(mc->cp_32khz_regulator)) {
			if (regulator_enable(mc->cp_32khz_regulator)) {
				pr_err("Failed to enable CP_32KHz regulator.\n");
				return -1;
			}
		}

		gpiod_set_value(mc->gpio_pda_active, 1);

		msleep(150); /*wait modem stable */
	} else {
		/* ensure cp_reset pin set to low */
		gpiod_set_value(mc->gpio_cp_reset, 0);
		msleep(100);

		gpiod_set_value(mc->gpio_cp_reset, 0);
		msleep(100);

		gpiod_set_value(mc->gpio_cp_reset, 1);

		/* Follow RESET timming delay not Power-On timming,
		because CP_RST & PHONE_ON have been set high already. */
		msleep(100); /*wait modem stable */

		gpiod_set_value(mc->gpio_pda_active, 1);
	}

	mc->status = MODEM_POWER_ON;

	return 0;
}

static int modem_off(struct modemctl *mc)
{
	pr_info("[MODEM] modem_off()\n");

	if (mc->variant == STE_M5730) {
		gpiod_set_value(mc->gpio_phone_on, 0);
		gpiod_set_value(mc->gpio_cp_reset, 0);

		if (!gpiod_get_value(mc->gpio_int_resout) && !gpiod_get_value(mc->gpio_cp_pwr_rst)) {
			if (regulator_is_enabled(mc->cp_32khz_regulator)) {
				if (regulator_disable(mc->cp_32khz_regulator)) {
					pr_err("Failed to disable CP_32KHz regulator.\n");
					return -1;
				}
			}
			goto done;
		}

		if (gpiod_get_value(mc->gpio_cp_pwr_rst)) {
			pr_err ("%s, GPIO_CP_PWR_RST is high\n", __func__);
			gpiod_set_value(mc->gpio_cp_reset, 1);
			while (gpiod_get_value(mc->gpio_cp_pwr_rst)) {
				pr_err ("[%s] waiting 1 sec for modem to stabilize. \n", __func__);
				msleep(1000); /*wait modem stable */
			}
		}

		if (regulator_is_enabled(mc->cp_32khz_regulator)) {
			if (regulator_disable(mc->cp_32khz_regulator)) {
				pr_err ("Failed to disable CP_32KHz regulator.\n");
				return -1;
			}
		}
	}

	gpiod_set_value(mc->gpio_cp_reset, 0);

done:
	mc->status = MODEM_OFF;
	return 0;
}

static long modemctl_ioctl(struct file *filp,
			   unsigned int cmd, unsigned long arg)
{
	struct modemctl *mc = filp->private_data;
	int ret;

	mutex_lock(&mc->ctl_lock);
	switch (cmd) {
	case IOCTL_MODEM_RESET:
		ret = modem_reset(mc);
		MODEM_COUNT(mc,resets);
		break;
	case IOCTL_MODEM_START:
		ret = modem_start(mc, 0);
		break;
	case IOCTL_MODEM_RAMDUMP:
		ret = modem_start(mc, 1);
		break;
	case IOCTL_MODEM_OFF:
		ret = modem_off(mc);
		break;
	case IOCTL_MODEM_WAIT_FOR_SBL:
		ret = modem_wait_for_sbl(mc);
		break;
	case IOCTL_MODEM_BINARY_LOAD:
		ret = modem_binary_load(mc);
		break;
	default:
		ret = -EINVAL;
	}
	mutex_unlock(&mc->ctl_lock);
	pr_info("modemctl_ioctl() %d\n", ret);
	return ret;
}

static const struct file_operations modemctl_fops = {
	.owner =		THIS_MODULE,
	.llseek =		default_llseek,
	.open =			modemctl_open,
	.release =		modemctl_release,
	.read =			modemctl_read,
	.write =		modemctl_write,
	.unlocked_ioctl =	modemctl_ioctl,
};

static irqreturn_t modemctl_bp_irq_handler(int irq, void *_mc)
{
	pr_debug("[MODEM] bp_irq()\n");
	return IRQ_HANDLED;
}

static irqreturn_t resout_irq_handler(int irq, void *_mc)
{
	struct modemctl *mc = _mc;

	pr_debug("[MODEM] resout_irq()\n");
	if (!gpiod_get_value(mc->gpio_int_resout))
		pm_wakeup_event(mc->dev.this_device, 600 * HZ);

	return IRQ_HANDLED;
}

static irqreturn_t cp_pwr_rst_irq_handler(int irq, void *_mc)
{
	struct modemctl *mc = _mc;

	pr_debug("[MODEM] cp_pwr_rst_irq()\n");
	if (!gpiod_get_value(mc->gpio_cp_pwr_rst))
		pm_wakeup_event(mc->dev.this_device, 600 * HZ);

	return IRQ_HANDLED;
}

static void modemctl_handle_offline(struct modemctl *mc, unsigned cmd)
{
	switch (mc->status) {
	case MODEM_BOOTING_NORMAL:
		if (cmd == MODEM_MSG_BINARY_DONE) {
			pr_info("[MODEM] binary load done\n");

			/* STE modems are poorly implemented and need this written now,
			 * not when MBC_PHONE_START as that is too late
			 */
			if (mc->variant == STE_M5730)
				writel(MB_VALID | MB_COMMAND | MBC_INIT_END | CP_BOOT_AIRPLANE, mc->mmio + OFF_MBOX_AP);

			mc->status = MODEM_RUNNING;
			wake_up(&mc->wq);
		}
		break;
	case MODEM_BOOTING_RAMDUMP:
	case MODEM_DUMPING:
		if (cmd == MODEM_MSG_RAMDUMP_LARGE) {
			mc->status = MODEM_DUMPING;
			mc->ramdump_size = RAMDUMP_LARGE_SIZE;
			wake_up(&mc->wq);
			pr_info("[MODEM] ramdump - %d bytes available\n",
				mc->ramdump_size);
		} else if (cmd == MODEM_MSG_RAMDUMP_SMALL) {
			mc->status = MODEM_DUMPING;
			mc->ramdump_size = RAMDUMP_SMALL_SIZE;
			wake_up(&mc->wq);
			pr_info("[MODEM] ramdump - %d bytes available\n",
				mc->ramdump_size);
		} else {
			pr_err("[MODEM] unknown msg %08x in ramdump mode\n", cmd);
		}
		break;
	}
}

static irqreturn_t modemctl_mbox_irq_handler(int irq, void *_mc)
{
	struct modemctl *mc = _mc;
	unsigned cmd;
	unsigned long flags;

	cmd = readl(mc->mmio + OFF_MBOX_BP);

	if (unlikely(mc->status != MODEM_RUNNING)) {
		modemctl_handle_offline(mc, cmd);
		return IRQ_HANDLED;
	}

	if (!(cmd & MB_VALID)) {
		if (cmd == MODEM_MSG_LOGDUMP_DONE) {
			pr_info("modem: logdump done!\n");
			mc->logdump_data = 1;
			wake_up(&mc->wq);
		} else {
			pr_info("modem: what is %08x\n",cmd);
		}
		return IRQ_HANDLED;
	}

	spin_lock_irqsave(&mc->lock, flags);

	if (cmd & MB_COMMAND) {
		switch (cmd & 15) {
		case MBC_REQ_SEM:
			if (mmio_sem(mc) == 0) {
				/* Sometimes the modem may ask for the
				 * sem when it already owns it.  Humor
				 * it and ack that request.
				 */
				writel(MB_COMMAND | MB_VALID | MBC_RES_SEM,
				       mc->mmio + OFF_MBOX_AP);
				MODEM_COUNT(mc,bp_req_confused);
			} else if (mc->mmio_req_count == 0) {
				/* No references? Give it to the modem. */
				modem_update_state(mc);
				mc->mmio_owner = 0;
				writel(0, mc->mmio + OFF_SEM);
				writel(MB_COMMAND | MB_VALID | MBC_RES_SEM,
				       mc->mmio + OFF_MBOX_AP);
				MODEM_COUNT(mc,bp_req_instant);
				goto done;
			} else {
				/* Busy now, remember the modem needs it. */
				mc->mmio_bp_request = 1;
				MODEM_COUNT(mc,bp_req_delayed);
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
			       mc->mmio + OFF_MBOX_AP);

			/* TODO: probably unsafe to send this back-to-back
			 * with the INIT_END message.
			 */
			/* if somebody is waiting for mmio access... */
			if (mc->mmio_req_count)
				modem_request_sem(mc);
			break;
		case MBC_RESET:
			pr_err("$$$ MODEM RESET $$$\n");
			mc->status = MODEM_CRASHED;
			wake_up(&mc->wq);
			break;
		case MBC_ERR_DISPLAY: {
			char buf[SIZ_ERROR_MSG + 1];
			int i;
			pr_err("$$$ MODEM ERROR $$$\n");
			mc->status = MODEM_CRASHED;
			wake_up(&mc->wq);
			memcpy(buf, mc->mmio + OFF_ERROR_MSG, SIZ_ERROR_MSG);
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
	} else if (mc->variant == STE_M5730 && mmio_sem(mc) == 0) {
		/* STE modems don't automatically release the semaphore
		 * we need to request it when we don't have it
		 */
		modem_request_sem(mc);
		goto done;
	}

	/* On *any* interrupt from the modem it may have given
	 * us ownership of the mmio hw semaphore.  If that
	 * happens, we should claim the semaphore if we have
	 * threads waiting for it and we should process any
	 * messages that the modem has enqueued in its fifos
	 * by calling modem_handle_io().
	 */
	if (mmio_sem(mc) == 1) {
		if (!mc->mmio_owner) {
			modem_update_state(mc);
			if (mc->mmio_req_count) {
				mc->mmio_owner = 1;
				wake_up(&mc->wq);
			}
		}

		modem_handle_io(mc);

		/* If we have a signal to send and we're not
		 * hanging on to the mmio hw semaphore, give
		 * it back to the modem and send the signal.
		 * Otherwise this will happen when we give up
		 * the mmio hw sem in modem_release_mmio().
		 */
		if (mc->mmio_signal_bits && !mc->mmio_owner) {
			writel(0, mc->mmio + OFF_SEM);
			writel(MB_VALID | mc->mmio_signal_bits,
			       mc->mmio + OFF_MBOX_AP);
			mc->mmio_signal_bits = 0;
		}
	}
done:
	spin_unlock_irqrestore(&mc->lock, flags);
	return IRQ_HANDLED;
}

void modem_force_crash(struct modemctl *mc)
{
	unsigned long int flags;
	pr_info("modem_force_crash() BOOM!\n");
	spin_lock_irqsave(&mc->lock, flags);
	mc->status = MODEM_CRASHED;
	wake_up(&mc->wq);
	spin_unlock_irqrestore(&mc->lock, flags);
}

static ssize_t modemctl_show_type(struct device *dev,
        struct device_attribute *attr, char *buf)
{
	struct modemctl *mc = dev_get_drvdata(dev);
	int len;

	if (mc->variant == STE_M5730) {
		len = sprintf(buf, "%s\n", "ste");
	} else {
		len = sprintf(buf, "%s\n", "xmm");
	}

    return len;
}

static DEVICE_ATTR(type, S_IRUGO, modemctl_show_type, NULL);

static struct attribute *modemctl_attrs[] = {
	&dev_attr_type.attr,
	NULL
};

ATTRIBUTE_GROUPS(modemctl);

#if IS_ENABLED(CONFIG_OF)
static const struct of_device_id modemctl_of_match[] = {
	{
		.compatible = "samsung,ste-m5730",
		.data = (void *)STE_M5730,
	}, {
		.compatible = "samsung,intel-xmm6160",
		.data = (void *)INTEL_XMM6160,
	}, {
		/* sentinel */
	},
};
MODULE_DEVICE_TABLE(of, modemctl_of_match);
#endif

static int modemctl_probe(struct platform_device *pdev)
{
	const struct of_device_id *match;
	struct modemctl *mc;
	struct resource *res;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	int r;
	unsigned int mmbase;

	if (!np) {
		dev_err(dev, "must be instantiated via DT");
		return -EINVAL;
	}

	mc = devm_kzalloc(dev, sizeof(*mc), GFP_KERNEL);
	if (!mc)
		return -ENOMEM;

	init_waitqueue_head(&mc->wq);
	spin_lock_init(&mc->lock);
	mutex_init(&mc->ctl_lock);

	match = of_match_node(modemctl_of_match, pdev->dev.of_node);
	mc->variant = (enum modemctl_variant) match->data;

	mc->gpio_pda_active = devm_gpiod_get(dev, "pda_active", GPIOD_OUT_HIGH);
	if (IS_ERR(mc->gpio_pda_active)) {
		pr_err("no pda_active gpio");
		return PTR_ERR(mc->gpio_pda_active);
	}

	mc->gpio_cp_reset = devm_gpiod_get(dev, "cp_reset", GPIOD_OUT_HIGH);
	if (IS_ERR(mc->gpio_cp_reset)) {
		pr_err("no cp_reset gpio");
		return PTR_ERR(mc->gpio_cp_reset);
	}

	if (mc->variant == STE_M5730) {
		mc->gpio_phone_on = devm_gpiod_get(dev, "phone_on", GPIOD_OUT_HIGH);
		if (IS_ERR(mc->gpio_phone_on)) {
			pr_err("no phone_on gpio");
			return PTR_ERR(mc->gpio_phone_on);
		}

		mc->gpio_int_resout = devm_gpiod_get(dev, "int_resout", GPIOD_IN);
		if (IS_ERR(mc->gpio_int_resout)) {
			pr_err("no int_resout gpio");
			return PTR_ERR(mc->gpio_int_resout);
		}

		mc->irq_resout = gpiod_to_irq(mc->gpio_int_resout);
		if (mc->irq_resout < 0) {
			pr_err("no resout irq");
			return mc->irq_resout;
		}

		r = devm_request_irq(dev, mc->irq_resout, resout_irq_handler,
			IRQF_TRIGGER_FALLING, "modemctl_resout", mc);
		if (r < 0) {
			pr_err("couldn't request resout irq");
			return r;
		}

		enable_irq_wake(mc->irq_resout);

		mc->gpio_cp_pwr_rst = devm_gpiod_get(dev, "cp_pwr_rst", GPIOD_IN);
		if (IS_ERR(mc->gpio_cp_pwr_rst)) {
			pr_err("no cp_pwr_rst gpio");
			return PTR_ERR(mc->gpio_cp_pwr_rst);
		}

		mc->irq_cp_pwr_rst = gpiod_to_irq(mc->gpio_cp_pwr_rst);
		if (mc->irq_cp_pwr_rst < 0) {
			pr_err("no cp_pwr_rst irq");
			return mc->irq_cp_pwr_rst;
		}

		r = devm_request_irq(dev, mc->irq_cp_pwr_rst, cp_pwr_rst_irq_handler,
			IRQF_TRIGGER_FALLING, "modemctl_cp_pwr_rst", mc);
		if (r < 0) {
			pr_err("failed to request cp_pwr_rst irq");
			return r;
		}

		enable_irq_wake(mc->irq_cp_pwr_rst);
	}

	mc->irq_bp = platform_get_irq_byname(pdev, "active");
	mc->irq_mbox = platform_get_irq_byname(pdev, "onedram");

	if (mc->variant == STE_M5730) {
		mc->cp_rtc_regulator = devm_regulator_get(dev, "cp_rtc");
		if (IS_ERR_OR_NULL(mc->cp_rtc_regulator))
			return -ENODEV;
		mc->cp_32khz_regulator = devm_regulator_get(dev, "cp_32khz");
		if (IS_ERR_OR_NULL(mc->cp_32khz_regulator))
			return -ENODEV;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENOMEM;
	mmbase = res->start;
	mc->mmsize = resource_size(res);

	mc->mmio = devm_ioremap_nocache(dev, mmbase, mc->mmsize);
	if (!mc->mmio)
		return -EADDRNOTAVAIL;

	platform_set_drvdata(pdev, mc);

	mc->dev.name = "modem_ctl";
	mc->dev.minor = MISC_DYNAMIC_MINOR;
	mc->dev.fops = &modemctl_fops;
	mc->dev.groups = modemctl_groups;
	r = misc_register(&mc->dev);
	if (r)
		return r;

	dev_set_drvdata(mc->dev.this_device, mc);

	/* hide control registers from userspace */
	mc->mmsize -= 0x800;
	mc->status = MODEM_OFF;

	modem_io_init(mc, mc->mmio);

	r = devm_request_irq(dev, mc->irq_bp, modemctl_bp_irq_handler,
			IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
			"modemctl_bp", mc);
	if (r)
		goto err_misc_register;

	r = devm_request_irq(dev, mc->irq_mbox, modemctl_mbox_irq_handler,
			IRQF_TRIGGER_LOW, "modemctl_mbox", mc);
	if (r)
		goto err_misc_register;

	enable_irq_wake(mc->irq_bp);
	enable_irq_wake(mc->irq_mbox);

	device_init_wakeup(mc->dev.this_device, true);

	modem_debugfs_init(mc);

	return 0;

err_misc_register:
	misc_deregister(&mc->dev);
	return r;
}

static int modemctl_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct modemctl *mc = dev_get_drvdata(dev);

	misc_deregister(&mc->dev);

	return 0;
}

static int modemctl_suspend(struct device *pdev)
{
	struct modemctl *mc = dev_get_drvdata(pdev);

	gpiod_set_value(mc->gpio_pda_active, 0);

	return 0;
}

static int modemctl_resume(struct device *pdev)
{
	struct modemctl *mc = dev_get_drvdata(pdev);

	gpiod_set_value(mc->gpio_pda_active, 1);

	return 0;
}

static const struct dev_pm_ops modemctl_pm_ops = {
	.suspend    = modemctl_suspend,
	.resume     = modemctl_resume,
};

static struct platform_driver modemctl_driver = {
	.probe = modemctl_probe,
	.remove = modemctl_remove,
	.driver = {
		.name = "modemctl",
		.of_match_table = of_match_ptr(modemctl_of_match),
		.pm   = &modemctl_pm_ops,
	},
};

static int __init modemctl_init(void)
{
	return platform_driver_register(&modemctl_driver);
}

module_init(modemctl_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Samsung Modem Control Driver");
