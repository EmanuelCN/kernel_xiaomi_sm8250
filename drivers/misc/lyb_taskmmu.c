// SPDX-License-Identifier: GPL-2.0

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <misc/lyb_taskmmu.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("lybxlpsv");
MODULE_DESCRIPTION("lyb taskmmu");
MODULE_VERSION("0.0.2");

bool lyb_sultan_pid = false;
module_param(lyb_sultan_pid, bool, 0644);

bool lyb_sultan_pid_shrink = false;
module_param(lyb_sultan_pid_shrink, bool, 0644);

static int __init lyb_driver_init(void) {
 printk(KERN_INFO "lyb taskmmu initialized");
 lyb_sultan_pid = false;
 lyb_sultan_pid_shrink = false;
 return 0;
}
static void __exit lyb_driver_exit(void) {
 printk(KERN_INFO "lyb taskmmu exit");
}

module_init(lyb_driver_init);
module_exit(lyb_driver_exit);
