// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014-2020, The Linux Foundation. All rights reserved.
 */

#define pr_fmt(fmt)	"core_ctl: " fmt

#include <linux/init.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/cpufreq.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/sched/rt.h>
#include <linux/syscore_ops.h>
#include <uapi/linux/sched/types.h>
#include <linux/sched/core_ctl.h>

#include <trace/events/sched.h>
#include "sched.h"
#include "walt.h"

int core_ctl_set_boost(bool boost)
{
	return 0;
}
EXPORT_SYMBOL(core_ctl_set_boost);

static int __init core_ctl_init(void)
{
	return 0;
}

late_initcall(core_ctl_init);
