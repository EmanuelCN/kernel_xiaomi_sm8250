// SPDX-License-Identifier: GPL-2.0
/*
 * TEE driver for goodix fingerprint sensor
 * Copyright (C) 2016 Goodix
 * Copyright (C) 2022 Jebaitedneko <Jebaitedneko@gmail.com>
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/ioctl.h>
#include <linux/irq.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <net/netlink.h>
#include <net/sock.h>

static struct gf_dev {
	dev_t devt;
	struct list_head device_entry;
	struct platform_device *spi;
	struct input_dev *input;
	struct regulator *vreg;
        struct wakeup_source *fp_wakelock;
	signed irq_gpio, rst_gpio, pwr_gpio;
	int irq, irq_enabled;
} gf;

#define MAX_MSGSIZE 2
static struct sock *nl_sk = NULL;
static int pid = -1;
static inline int sendnlmsg(void) {
	struct sk_buff *skb_1 = alloc_skb(NLMSG_SPACE(MAX_MSGSIZE), GFP_KERNEL);
	struct nlmsghdr *nlh = nlmsg_put(skb_1, 0, 0, 0, MAX_MSGSIZE, 0);
	char msg[MAX_MSGSIZE] = { 1, 0 };
	memcpy(NLMSG_DATA(nlh), msg, MAX_MSGSIZE);
	NETLINK_CB(skb_1).portid = NETLINK_CB(skb_1).dst_group = 0;
	netlink_unicast(nl_sk, skb_1, pid, MSG_DONTWAIT);
	return 1;
}

static inline void nl_data_ready(struct sk_buff *__skb) {
	struct sk_buff *skb = skb_get(__skb);
	struct nlmsghdr *nlh = nlmsg_hdr(skb);
	pid = nlh->nlmsg_pid;
	kfree_skb(skb);
}

#define NETLINK_TEST 25
static inline void netlink_init(void) {
	struct netlink_kernel_cfg netlink_cfg;
	netlink_cfg.groups = netlink_cfg.flags = 0;
	netlink_cfg.input = nl_data_ready;
	netlink_cfg.cb_mutex = NULL;
	nl_sk = netlink_kernel_create(&init_net, NETLINK_TEST, &netlink_cfg);
}

static inline void netlink_exit(void) {
	netlink_kernel_release(nl_sk);
	nl_sk = NULL;
}

static inline void irq_switch(struct gf_dev *gf_dev, int status) {
	if (status) {
		if (!gf_dev->irq_enabled) {
			enable_irq_wake(gf_dev->irq);
			gf_dev->irq_enabled = 1;
		}
	} else {
		if (gf_dev->irq_enabled) {
			disable_irq_wake(gf_dev->irq);
			gf_dev->irq_enabled = 0;
		}
	}
}

#define WAKELOCK_HOLD_TIME 400
static inline irqreturn_t gf_irq(int irq, void *handle) {

	struct gf_dev *gf_dev = handle;

	if (gf_dev && gf_dev->fp_wakelock)
		__pm_wakeup_event(gf_dev->fp_wakelock, WAKELOCK_HOLD_TIME);

	return sendnlmsg();
}

static inline void gf_setup(struct gf_dev *gf_dev) {
	gf_dev->pwr_gpio = of_get_named_gpio(gf_dev->spi->dev.of_node,
		"fp-gpio-pwr", 0);
	gpio_request(gf_dev->pwr_gpio, "goodix_pwr");
	gpio_direction_output(gf_dev->pwr_gpio, 1);
	gf_dev->rst_gpio = of_get_named_gpio(gf_dev->spi->dev.of_node,
		"goodix,gpio-reset", 0);
	gpio_request(gf_dev->rst_gpio, "gpio-reset");
	gpio_direction_output(gf_dev->rst_gpio, 1);
	gf_dev->irq_gpio = of_get_named_gpio(gf_dev->spi->dev.of_node,
		"goodix,gpio-irq", 0);
	gpio_request(gf_dev->irq_gpio, "gpio-irq");
	gpio_direction_input(gf_dev->irq_gpio);
	gf_dev->irq = gpio_to_irq(gf_dev->irq_gpio);
	if (request_threaded_irq(gf_dev->irq, NULL, gf_irq,
			IRQF_TRIGGER_RISING | IRQF_ONESHOT, "gf", gf_dev))
		irq_switch(gf_dev, 1);
	gf_dev->vreg = regulator_get(&gf_dev->spi->dev, "fp_vdd_vreg");
	if (regulator_is_enabled(gf_dev->vreg)) {
		regulator_set_load(gf_dev->vreg, 100000);
	} else if (!regulator_is_enabled(gf_dev->vreg)) {
		regulator_set_voltage(gf_dev->vreg, 3000000, 3000000);
		if (regulator_enable(gf_dev->vreg))
			return;
	}
	return;
}

static inline void gf_cleanup(struct gf_dev *gf_dev) {
	if (gf_dev->irq_enabled) {
		irq_switch(gf_dev, 0);
		free_irq(gf_dev->irq, gf_dev);
	}
	if (gpio_is_valid(gf_dev->irq_gpio))
		gpio_free(gf_dev->irq_gpio);
	if (gpio_is_valid(gf_dev->rst_gpio))
		gpio_free(gf_dev->rst_gpio);
	if (gpio_is_valid(gf_dev->pwr_gpio))
		gpio_free(gf_dev->pwr_gpio);
	if (regulator_is_enabled(gf_dev->vreg)) {
		regulator_disable(gf_dev->vreg);
		regulator_put(gf_dev->vreg);
		gf_dev->vreg = NULL;
	}
}

static inline void gpio_reset(struct gf_dev *gf_dev) {
	gpio_direction_output(gf_dev->rst_gpio, 1);
	gpio_set_value(gf_dev->rst_gpio, 0);
	mdelay(3);
	gpio_set_value(gf_dev->rst_gpio, 1);
	mdelay(3);
}

#define GF_IOC_MAGIC 'g'
#define GF_IOC_INIT _IOR(GF_IOC_MAGIC, 0, uint8_t)
#define GF_IOC_RESET _IO(GF_IOC_MAGIC, 2)
#define GF_IOC_ENABLE_IRQ _IO(GF_IOC_MAGIC, 3)
#define GF_IOC_DISABLE_IRQ _IO(GF_IOC_MAGIC, 4)
static inline long gf_ioctl(struct file *filp, unsigned int cmd,
							unsigned long arg) {
	struct gf_dev *gf_dev = &gf;
	u8 netlink_route = NETLINK_TEST;
	switch (cmd) {
	case GF_IOC_INIT:
		if (copy_to_user((void __user *)arg, (void *)&netlink_route, sizeof(u8)))
			break;
		break;
	case GF_IOC_DISABLE_IRQ:
		irq_switch(gf_dev, 0);
		break;
	case GF_IOC_ENABLE_IRQ:
		irq_switch(gf_dev, 1);
		break;
	case GF_IOC_RESET:
		gpio_reset(gf_dev);
		break;
	default:
		break;
	}
	return 0;
}

static DEFINE_MUTEX(gf_lock);
static inline int gf_open(struct inode *inode, struct file *filp) {
	struct gf_dev *gf_dev = &gf;
	mutex_lock(&gf_lock);
	filp->private_data = gf_dev;
	nonseekable_open(inode, filp);
	gf_setup(gf_dev);
	gpio_reset(gf_dev);
	mutex_unlock(&gf_lock);
	return 0;
}

static inline int gf_release(struct inode *inode, struct file *filp) {
	struct gf_dev *gf_dev = &gf;
	mutex_lock(&gf_lock);
	gf_dev = filp->private_data;
	filp->private_data = NULL;
	gf_cleanup(gf_dev);
	mutex_unlock(&gf_lock);
	return 0;
}

static const struct file_operations gf_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = gf_ioctl,
	.open = gf_open,
	.release = gf_release,
};

static struct class *gf_class;
#define N_SPI_MINORS 256
static DECLARE_BITMAP(minors, N_SPI_MINORS);
static LIST_HEAD(device_list);
static int SPIDEV_MAJOR;
#define GF_DEV_NAME "goodix_fp"
#define GF_INPUT_NAME "uinput-goodix"
static inline int gf_probe(struct platform_device *pdev) {
	struct gf_dev *gf_dev = &gf;
	unsigned long minor = find_first_zero_bit(minors, N_SPI_MINORS);
	INIT_LIST_HEAD(&gf_dev->device_entry);
	gf_dev->spi = pdev;
	gf_dev->irq_gpio = gf_dev->rst_gpio = gf_dev->pwr_gpio = -EINVAL;
	gf_dev->devt = MKDEV(SPIDEV_MAJOR, minor);
	mutex_lock(&gf_lock);
	device_create(gf_class, &gf_dev->spi->dev, gf_dev->devt, gf_dev,
					GF_DEV_NAME);
	set_bit(minor, minors);
	list_add(&gf_dev->device_entry, &device_list);
	mutex_unlock(&gf_lock);
	gf_dev->input = input_allocate_device();
	gf_dev->input->name = GF_INPUT_NAME;
	gf_dev->fp_wakelock = wakeup_source_register(&gf_dev->spi->dev, "fp_wakelock");
	if (!gf_dev->fp_wakelock)
		dev_warn(&gf_dev->spi->dev, "Failed to register wakelock\n");
	return input_register_device(gf_dev->input);
}

static inline int gf_remove(struct platform_device *pdev) {
	struct gf_dev *gf_dev = &gf;

	if (gf_dev->fp_wakelock) {
		wakeup_source_unregister(gf_dev->fp_wakelock);
		gf_dev->fp_wakelock = NULL;
	}

	if (gf_dev->input) {
		input_unregister_device(gf_dev->input);
		input_free_device(gf_dev->input);
	}
	mutex_lock(&gf_lock);
	list_del(&gf_dev->device_entry);
	clear_bit(MINOR(gf_dev->devt), minors);
	device_destroy(gf_class, gf_dev->devt);
	mutex_unlock(&gf_lock);
	return 0;
}

static const struct of_device_id gx_match_table[] = {
	{.compatible = "goodix,fingerprint"},
	{},
};

static struct platform_driver gf_driver = {
	.driver = {
		.name = GF_DEV_NAME,
		.owner = THIS_MODULE,
		.of_match_table = gx_match_table,
	},
	.probe = gf_probe,
	.remove = gf_remove,
};

#define CHRD_DRIVER_NAME "goodix_fp_spi"
#define CLASS_NAME "goodix_fp"
static inline int __init gf_init(void) {
	int status = register_chrdev(SPIDEV_MAJOR, CHRD_DRIVER_NAME, &gf_fops);
	SPIDEV_MAJOR = status;
	gf_class = class_create(THIS_MODULE, CLASS_NAME);
	platform_driver_register(&gf_driver);
	netlink_init();
	return 0;
}
module_init(gf_init);

static inline void __exit gf_exit(void) {
	netlink_exit();
	platform_driver_unregister(&gf_driver);
	class_destroy(gf_class);
	unregister_chrdev(SPIDEV_MAJOR, gf_driver.driver.name);
}
module_exit(gf_exit);

MODULE_AUTHOR("Jiangtao Yi, <yijiangtao@goodix.com>");
MODULE_AUTHOR("Jandy Gou, <gouqingsong@goodix.com>");
MODULE_AUTHOR("Jebaitedneko, <Jebaitedneko@gmail.com>");
MODULE_DESCRIPTION("goodix fingerprint sensor device driver");
MODULE_LICENSE("GPL");
