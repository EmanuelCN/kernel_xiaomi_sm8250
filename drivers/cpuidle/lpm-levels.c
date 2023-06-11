// SPDX-License-Identifier: GPL-2.0-only

/* Copyright (c) 2012-2020, The Linux Foundation. All rights reserved.
 * Copyright (C) 2006-2007 Adam Belay <abelay@novell.com>
 * Copyright (C) 2009 Intel Corporation
 */

#define pr_fmt(fmt) "%s: " fmt, KBUILD_MODNAME

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/cpu.h>
#include <linux/of.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/tick.h>
#include <linux/suspend.h>
#include <linux/pm_qos.h>
#include <linux/of_platform.h>
#include <linux/smp.h>
#include <linux/dma-mapping.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/cpu_pm.h>
#include <linux/cpuhotplug.h>
#include <linux/regulator/machine.h>
#include <linux/sched/clock.h>
#include <soc/qcom/pm.h>
#include <soc/qcom/event_timer.h>
#include <soc/qcom/lpm_levels.h>
#include <soc/qcom/lpm-stats.h>
#include <asm/arch_timer.h>
#include <asm/suspend.h>
#include <asm/cpuidle.h>
#include "lpm-levels.h"
#include <trace/events/power.h>
#include "../clk/clk.h"
#define CREATE_TRACE_POINTS
#include <trace/events/trace_msm_low_power.h>

#define SCLK_HZ (32768)
#define PSCI_POWER_STATE(reset) (reset << 30)
#define PSCI_AFFINITY_LEVEL(lvl) ((lvl & 0x3) << 24)

static struct system_pm_ops *sys_pm_ops;
struct lpm_cluster *lpm_root_node;

static DEFINE_PER_CPU(struct lpm_cpu*, cpu_lpm);
static bool suspend_in_progress;

static void cluster_unprepare(struct lpm_cluster *cluster,
		const struct cpumask *cpu, int child_idx, bool from_idle,
		int64_t time, bool success);
static void cluster_prepare(struct lpm_cluster *cluster,
		const struct cpumask *cpu, int child_idx, bool from_idle,
		int64_t time);

static bool print_parsed_dt;
module_param_named(print_parsed_dt, print_parsed_dt, bool, 0664);

/**
 * msm_cpuidle_get_deep_idle_latency - Get deep idle latency value
 *
 * Returns an s32 latency value
 */
s32 msm_cpuidle_get_deep_idle_latency(void)
{
	return 10;
}
EXPORT_SYMBOL(msm_cpuidle_get_deep_idle_latency);

uint32_t register_system_pm_ops(struct system_pm_ops *pm_ops)
{
	if (sys_pm_ops)
		return -EUSERS;

	sys_pm_ops = pm_ops;

	return 0;
}

static int lpm_dying_cpu(unsigned int cpu)
{
	struct lpm_cluster *cluster = per_cpu(cpu_lpm, cpu)->parent;

	cluster_prepare(cluster, get_cpu_mask(cpu), NR_LPM_LEVELS, false, 0);
	return 0;
}

static int lpm_starting_cpu(unsigned int cpu)
{
	struct lpm_cluster *cluster = per_cpu(cpu_lpm, cpu)->parent;

	cluster_unprepare(cluster, get_cpu_mask(cpu), NR_LPM_LEVELS, false,
						0, true);
	return 0;
}

static unsigned int get_next_online_cpu(bool from_idle)
{
	unsigned int cpu;
	ktime_t next_event;
	unsigned int next_cpu = raw_smp_processor_id();

	if (!from_idle)
		return next_cpu;
	next_event = KTIME_MAX;
	for_each_online_cpu(cpu) {
		ktime_t *next_event_c;

		next_event_c = get_next_event_cpu(cpu);
		if (*next_event_c < next_event) {
			next_event = *next_event_c;
			next_cpu = cpu;
		}
	}
	return next_cpu;
}

static uint64_t get_cluster_sleep_time(struct lpm_cluster *cluster,
		bool from_idle)
{
	int cpu;
	ktime_t next_event;
	struct cpumask online_cpus_in_cluster;

	if (!from_idle)
		return ~0ULL;

	next_event = KTIME_MAX;
	cpumask_and(&online_cpus_in_cluster,
			&cluster->num_children_in_sync, cpu_online_mask);

	for_each_cpu(cpu, &online_cpus_in_cluster) {
		ktime_t *next_event_c;

		next_event_c = get_next_event_cpu(cpu);
		if (*next_event_c < next_event)
			next_event = *next_event_c;
	}

	if (ktime_to_us(next_event) > ktime_to_us(ktime_get()))
		return ktime_to_us(ktime_sub(next_event, ktime_get()));
	else
		return 0;
}

static int cluster_select(struct lpm_cluster *cluster, bool from_idle)
{
	int best_level = -1;
	int i;
	struct cpumask mask;
	uint32_t latency_us = ~0U;
	uint32_t sleep_us;

	if (!cluster)
		return -EINVAL;

	sleep_us = (uint32_t)get_cluster_sleep_time(cluster, from_idle);

	if (cpumask_and(&mask, cpu_online_mask, &cluster->child_cpus))
		latency_us = pm_qos_request_for_cpumask(PM_QOS_CPU_DMA_LATENCY,
							&mask);

	for (i = 0; i < cluster->nlevels; i++) {
		struct lpm_cluster_level *level = &cluster->levels[i];
		struct power_params *pwr_params = &level->pwr;

		if (!lpm_cluster_mode_allow(cluster, i, from_idle))
			continue;

		if (!cpumask_equal(&cluster->num_children_in_sync,
					&level->num_cpu_votes))
			continue;

		if (from_idle && latency_us < pwr_params->exit_latency)
			break;

		if (sleep_us < (pwr_params->exit_latency +
						pwr_params->entry_latency))
			break;

		if (suspend_in_progress && from_idle && level->notify_rpm)
			continue;

		if (level->notify_rpm) {
			if (!(sys_pm_ops && sys_pm_ops->sleep_allowed))
				continue;
			if (!sys_pm_ops->sleep_allowed())
				continue;
		}

		best_level = i;

		if (from_idle && sleep_us <= pwr_params->max_residency)
			break;
	}

	return best_level;
}

static int cluster_configure(struct lpm_cluster *cluster, int idx,
		bool from_idle)
{
	struct lpm_cluster_level *level = &cluster->levels[idx];
	struct cpumask online_cpus, cpumask;
	unsigned int cpu;

	cpumask_and(&online_cpus, &cluster->num_children_in_sync,
					cpu_online_mask);

	if (!cpumask_equal(&cluster->num_children_in_sync, &cluster->child_cpus)
				|| is_IPI_pending(&online_cpus))
		return -EPERM;

	if (idx != cluster->default_level) {
		trace_cluster_enter(cluster->cluster_name, idx,
			cluster->num_children_in_sync.bits[0],
			cluster->child_cpus.bits[0], from_idle);
		lpm_stats_cluster_enter(cluster->stats, idx);
	}

	if (level->notify_rpm) {
		cpu = get_next_online_cpu(from_idle);
		cpumask_copy(&cpumask, cpumask_of(cpu));
		if (sys_pm_ops && sys_pm_ops->enter)
			if ((sys_pm_ops->enter(&cpumask)))
				return -EBUSY;
	}

	cluster->last_level = idx;

	return 0;
}

static void cluster_prepare(struct lpm_cluster *cluster,
		const struct cpumask *cpu, int child_idx, bool from_idle,
		int64_t start_time)
{
	int i;

	if (!cluster)
		return;

	if (cluster->min_child_level > child_idx)
		return;

	raw_spin_lock(&cluster->sync_lock);
	cpumask_or(&cluster->num_children_in_sync, cpu,
			&cluster->num_children_in_sync);

	for (i = 0; i < cluster->nlevels; i++) {
		struct lpm_cluster_level *lvl = &cluster->levels[i];

		if (child_idx >= lvl->min_child_level)
			cpumask_or(&lvl->num_cpu_votes, cpu,
					&lvl->num_cpu_votes);
	}

	/*
	 * cluster_select() does not make any configuration changes. So its ok
	 * to release the lock here. If a core wakes up for a rude request,
	 * it need not wait for another to finish its cluster selection and
	 * configuration process
	 */

	if (!cpumask_equal(&cluster->num_children_in_sync,
				&cluster->child_cpus))
		goto failed;

	i = cluster_select(cluster, from_idle);

	if (i < 0)
		goto failed;

	if (cluster_configure(cluster, i, from_idle))
		goto failed;

	if (!IS_ERR_OR_NULL(cluster->stats))
		cluster->stats->sleep_time = start_time;
	cluster_prepare(cluster->parent, &cluster->num_children_in_sync, i,
			from_idle, start_time);

	raw_spin_unlock(&cluster->sync_lock);
	return;
failed:
	raw_spin_unlock(&cluster->sync_lock);
	if (!IS_ERR_OR_NULL(cluster->stats))
		cluster->stats->sleep_time = 0;
}

static void cluster_unprepare(struct lpm_cluster *cluster,
		const struct cpumask *cpu, int child_idx, bool from_idle,
		int64_t end_time, bool success)
{
	struct lpm_cluster_level *level;
	bool first_cpu;
	int last_level, i;

	if (!cluster)
		return;

	if (cluster->min_child_level > child_idx)
		return;

	raw_spin_lock(&cluster->sync_lock);
	last_level = cluster->default_level;
	first_cpu = cpumask_equal(&cluster->num_children_in_sync,
				&cluster->child_cpus);
	cpumask_andnot(&cluster->num_children_in_sync,
			&cluster->num_children_in_sync, cpu);

	for (i = 0; i < cluster->nlevels; i++) {
		struct lpm_cluster_level *lvl = &cluster->levels[i];

		if (child_idx >= lvl->min_child_level)
			cpumask_andnot(&lvl->num_cpu_votes,
					&lvl->num_cpu_votes, cpu);
	}

	if (!first_cpu || cluster->last_level == cluster->default_level)
		goto unlock_return;

	if (!IS_ERR_OR_NULL(cluster->stats) && cluster->stats->sleep_time)
		cluster->stats->sleep_time = end_time -
			cluster->stats->sleep_time;
	lpm_stats_cluster_exit(cluster->stats, cluster->last_level, success);

	level = &cluster->levels[cluster->last_level];

	if (level->notify_rpm)
		if (sys_pm_ops && sys_pm_ops->exit)
			sys_pm_ops->exit(success);

	trace_cluster_exit(cluster->cluster_name, cluster->last_level,
			cluster->num_children_in_sync.bits[0],
			cluster->child_cpus.bits[0], from_idle);

	last_level = cluster->last_level;
	cluster->last_level = cluster->default_level;

	cluster_unprepare(cluster->parent, &cluster->child_cpus,
			last_level, from_idle, end_time, success);
unlock_return:
	raw_spin_unlock(&cluster->sync_lock);
}

static inline void cpu_prepare(struct lpm_cpu *cpu, int cpu_index,
				bool from_idle)
{
	struct lpm_cpu_level *cpu_level = &cpu->levels[cpu_index];

	/* Use broadcast timer for aggregating sleep mode within a cluster.
	 * A broadcast timer could be used in the following scenarios
	 * 1) The architected timer HW gets reset during certain low power
	 * modes and the core relies on a external(broadcast) timer to wake up
	 * from sleep. This information is passed through device tree.
	 * 2) The CPU low power mode could trigger a system low power mode.
	 * The low power module relies on Broadcast timer to aggregate the
	 * next wakeup within a cluster, in which case, CPU switches over to
	 * use broadcast timer.
	 */

	if (from_idle && cpu_level->is_reset)
		cpu_pm_enter();

}

static inline void cpu_unprepare(struct lpm_cpu *cpu, int cpu_index,
				bool from_idle)
{
	struct lpm_cpu_level *cpu_level = &cpu->levels[cpu_index];

	if (from_idle && cpu_level->is_reset)
		cpu_pm_exit();
}

static int get_cluster_id(struct lpm_cluster *cluster, int *aff_lvl,
				bool from_idle)
{
	int state_id = 0;

	if (!cluster)
		return 0;

	raw_spin_lock(&cluster->sync_lock);

	if (!cpumask_equal(&cluster->num_children_in_sync,
				&cluster->child_cpus))
		goto unlock_and_return;

	state_id += get_cluster_id(cluster->parent, aff_lvl, from_idle);

	if (cluster->last_level != cluster->default_level) {
		struct lpm_cluster_level *level
			= &cluster->levels[cluster->last_level];

		state_id += (level->psci_id & cluster->psci_mode_mask)
					<< cluster->psci_mode_shift;

		/*
		 * We may have updated the broadcast timers, update
		 * the wakeup value by reading the bc timer directly.
		 */
		if (level->notify_rpm)
			if (sys_pm_ops && sys_pm_ops->update_wakeup)
				sys_pm_ops->update_wakeup(from_idle);
		if (cluster->psci_mode_shift)
			(*aff_lvl)++;
	}
unlock_and_return:
	raw_spin_unlock(&cluster->sync_lock);
	return state_id;
}

static int psci_enter_sleep(struct lpm_cpu *cpu, int idx, bool from_idle)
{
	int affinity_level = 0, state_id = 0, power_state = 0;
	int ret, success;
	/*
	 * idx = 0 is the default LPM state
	 */

	if (!idx) {
		cpu_do_idle();
		return 0;
	}

	if (from_idle && cpu->levels[idx].use_bc_timer) {
		ret = tick_broadcast_enter();
		if (ret)
			return ret;
	}

	state_id = get_cluster_id(cpu->parent, &affinity_level, from_idle);
	power_state = PSCI_POWER_STATE(cpu->levels[idx].is_reset);
	affinity_level = PSCI_AFFINITY_LEVEL(affinity_level);
	state_id += power_state + affinity_level + cpu->levels[idx].psci_id;

	ret = !arm_cpuidle_suspend(state_id);
	success = (ret == 0);

	if (from_idle && cpu->levels[idx].use_bc_timer)
		tick_broadcast_exit();

	return ret;
}

static int lpm_cpuidle_select(struct cpuidle_driver *drv,
		struct cpuidle_device *dev, bool *stop_tick)
{
#ifdef CONFIG_NO_HZ_COMMON
	ktime_t delta_next;
	s64 duration_ns = tick_nohz_get_sleep_length(&delta_next);

	if (duration_ns <= TICK_NSEC)
		*stop_tick = false;
#endif

	return 0;
}

static int lpm_cpuidle_enter(struct cpuidle_device *dev,
		struct cpuidle_driver *drv, int idx)
{
	wfi();
	return idx;
}

static void lpm_cpuidle_s2idle(struct cpuidle_device *dev,
		struct cpuidle_driver *drv, int idx)
{
	struct lpm_cpu *cpu = per_cpu(cpu_lpm, dev->cpu);
	const struct cpumask *cpumask = get_cpu_mask(dev->cpu);
	bool success;
	int ret;

	for (; idx >= 0; idx--) {
		if (lpm_cpu_mode_allow(dev->cpu, idx, false))
			break;
	}
	if (idx < 0) {
		pr_err("Failed suspend\n");
		return;
	}

	cpu_prepare(cpu, idx, true);
	cluster_prepare(cpu->parent, cpumask, idx, false, 0);

	ret = psci_enter_sleep(cpu, idx, false);
	success = (ret == 0);

	cluster_unprepare(cpu->parent, cpumask, idx, false, 0, success);
	cpu_unprepare(cpu, idx, true);
}

#ifdef CONFIG_CPU_IDLE_MULTIPLE_DRIVERS
static int cpuidle_register_cpu(struct cpuidle_driver *drv,
		struct cpumask *mask)
{
	struct cpuidle_device *device;
	int cpu, ret;

	if (!mask || !drv)
		return -EINVAL;

	drv->cpumask = mask;
	ret = cpuidle_register_driver(drv);
	if (ret) {
		pr_err("Failed to register cpuidle driver %d\n", ret);
		goto failed_driver_register;
	}

	for_each_cpu(cpu, mask) {
		device = &per_cpu(cpuidle_dev, cpu);
		device->cpu = cpu;

		ret = cpuidle_register_device(device);
		if (ret) {
			pr_err("Failed to register cpuidle driver for cpu:%u\n",
					cpu);
			goto failed_driver_register;
		}
	}
	return ret;
failed_driver_register:
	for_each_cpu(cpu, mask)
		cpuidle_unregister_driver(drv);
	return ret;
}
#else
static int cpuidle_register_cpu(struct cpuidle_driver *drv,
		struct  cpumask *mask)
{
	return cpuidle_register(drv, NULL);
}
#endif

static struct cpuidle_governor lpm_governor = {
	.name =		"qcom",
	.rating =	30,
	.select =	lpm_cpuidle_select,
};

static int cluster_cpuidle_register(struct lpm_cluster *cl)
{
	int i = 0, ret = 0;
	unsigned int cpu;
	struct lpm_cluster *p = NULL;
	struct lpm_cpu *lpm_cpu;

	if (list_empty(&cl->cpu)) {
		struct lpm_cluster *n;

		list_for_each_entry(n, &cl->child, list) {
			ret = cluster_cpuidle_register(n);
			if (ret)
				break;
		}
		return ret;
	}

	list_for_each_entry(lpm_cpu, &cl->cpu, list) {
		lpm_cpu->drv = kcalloc(1, sizeof(*lpm_cpu->drv), GFP_KERNEL);
		if (!lpm_cpu->drv)
			return -ENOMEM;

		lpm_cpu->drv->name = "msm_idle";

		for (i = 0; i < lpm_cpu->nlevels; i++) {
			struct cpuidle_state *st = &lpm_cpu->drv->states[i];
			struct lpm_cpu_level *cpu_level = &lpm_cpu->levels[i];

			snprintf(st->name, CPUIDLE_NAME_LEN, "C%u\n", i);
			strlcpy(st->desc, cpu_level->name, CPUIDLE_DESC_LEN);

			st->flags = 0;
			st->exit_latency = cpu_level->pwr.exit_latency;
			st->target_residency = 0;
			st->enter = lpm_cpuidle_enter;
			if (i == lpm_cpu->nlevels - 1)
				st->enter_s2idle = lpm_cpuidle_s2idle;
		}

		lpm_cpu->drv->state_count = lpm_cpu->nlevels;
		lpm_cpu->drv->safe_state_index = 0;
		for_each_cpu(cpu, &lpm_cpu->related_cpus)
			per_cpu(cpu_lpm, cpu) = lpm_cpu;

		for_each_possible_cpu(cpu) {
			if (cpu_online(cpu))
				continue;
			if (per_cpu(cpu_lpm, cpu))
				p = per_cpu(cpu_lpm, cpu)->parent;
			while (p) {
				int j;

				raw_spin_lock(&p->sync_lock);
				cpumask_set_cpu(cpu, &p->num_children_in_sync);
				for (j = 0; j < p->nlevels; j++)
					cpumask_copy(
						&p->levels[j].num_cpu_votes,
						&p->num_children_in_sync);
				raw_spin_unlock(&p->sync_lock);
				p = p->parent;
			}
		}
		ret = cpuidle_register_cpu(lpm_cpu->drv,
					&lpm_cpu->related_cpus);

		if (ret) {
			kfree(lpm_cpu->drv);
			return -ENOMEM;
		}
	}
	return 0;
}

/**
 * init_lpm - initializes the governor
 */
static int __init init_lpm(void)
{
	return cpuidle_register_governor(&lpm_governor);
}

postcore_initcall(init_lpm);

static void register_cpu_lpm_stats(struct lpm_cpu *cpu,
		struct lpm_cluster *parent)
{
	const char **level_name;
	int i;

	level_name = kcalloc(cpu->nlevels, sizeof(*level_name), GFP_KERNEL);

	if (!level_name)
		return;

	for (i = 0; i < cpu->nlevels; i++)
		level_name[i] = cpu->levels[i].name;

	lpm_stats_config_level("cpu", level_name, cpu->nlevels,
			parent->stats, &cpu->related_cpus);

	kfree(level_name);
}

static void register_cluster_lpm_stats(struct lpm_cluster *cl,
		struct lpm_cluster *parent)
{
	const char **level_name;
	struct lpm_cluster *child;
	struct lpm_cpu *cpu;
	int i;

	if (!cl)
		return;

	level_name = kcalloc(cl->nlevels, sizeof(*level_name), GFP_KERNEL);

	if (!level_name)
		return;

	for (i = 0; i < cl->nlevels; i++)
		level_name[i] = cl->levels[i].level_name;

	cl->stats = lpm_stats_config_level(cl->cluster_name, level_name,
			cl->nlevels, parent ? parent->stats : NULL, NULL);
	if (IS_ERR_OR_NULL(cl->stats))
		pr_info("Cluster (%s) stats not registered\n",
			cl->cluster_name);

	kfree(level_name);

	list_for_each_entry(cpu, &cl->cpu, list) {
		register_cpu_lpm_stats(cpu, cl);
	}
	if (!list_empty(&cl->cpu))
		return;

	list_for_each_entry(child, &cl->child, list)
		register_cluster_lpm_stats(child, cl);
}

static int lpm_suspend_prepare(void)
{
	suspend_in_progress = true;
	lpm_stats_suspend_enter();

	return 0;
}

static void lpm_suspend_wake(void)
{
	suspend_in_progress = false;
	lpm_stats_suspend_exit();
}

static int lpm_suspend_enter(suspend_state_t state)
{
	int cpu = raw_smp_processor_id();
	struct lpm_cpu *lpm_cpu = per_cpu(cpu_lpm, cpu);
	struct lpm_cluster *cluster = lpm_cpu->parent;
	const struct cpumask *cpumask = get_cpu_mask(cpu);
	int idx;
	bool success;
	int ret;

	for (idx = lpm_cpu->nlevels - 1; idx >= 0; idx--) {
		if (lpm_cpu_mode_allow(cpu, idx, false))
			break;
	}
	if (idx < 0) {
		pr_err("Failed suspend\n");
		return 0;
	}

	/*
	 * Print the clocks and regulators which are enabled during
	 * system suspend.  This debug information is useful to know
	 * which resources are enabled and preventing the system level
	 * LPMs (XO and Vmin).
	 */
	clock_debug_print_enabled();
	regulator_debug_print_enabled();

	cpu_prepare(lpm_cpu, idx, false);
	cluster_prepare(cluster, cpumask, idx, false, 0);

	ret = psci_enter_sleep(lpm_cpu, idx, false);
	success = (ret == 0);

	cluster_unprepare(cluster, cpumask, idx, false, 0, success);
	cpu_unprepare(lpm_cpu, idx, false);
	return 0;
}

static const struct platform_suspend_ops lpm_suspend_ops = {
	.enter = lpm_suspend_enter,
	.valid = suspend_valid_only_mem,
	.prepare_late = lpm_suspend_prepare,
	.wake = lpm_suspend_wake,
};

static const struct platform_s2idle_ops lpm_s2idle_ops = {
	.prepare = lpm_suspend_prepare,
	.restore = lpm_suspend_wake,
};

static int lpm_probe(struct platform_device *pdev)
{
	int ret;
	struct kobject *module_kobj = NULL;

	get_online_cpus();
	lpm_root_node = lpm_of_parse_cluster(pdev);

	if (IS_ERR_OR_NULL(lpm_root_node)) {
		pr_err("Failed to probe low power modes\n");
		put_online_cpus();
		return PTR_ERR(lpm_root_node);
	}

	if (print_parsed_dt)
		cluster_dt_walkthrough(lpm_root_node);

	/*
	 * Register hotplug notifier before broadcast time to ensure there
	 * to prevent race where a broadcast timer might not be setup on for a
	 * core.  BUG in existing code but no known issues possibly because of
	 * how late lpm_levels gets initialized.
	 */
	suspend_set_ops(&lpm_suspend_ops);
	s2idle_set_ops(&lpm_s2idle_ops);

	register_cluster_lpm_stats(lpm_root_node, NULL);

	ret = cluster_cpuidle_register(lpm_root_node);
	put_online_cpus();
	if (ret) {
		pr_err("Failed to register with cpuidle framework\n");
		goto failed;
	}

	ret = cpuhp_setup_state(CPUHP_AP_QCOM_SLEEP_STARTING,
			"AP_QCOM_SLEEP_STARTING",
			lpm_starting_cpu, lpm_dying_cpu);
	if (ret)
		goto failed;

	module_kobj = kset_find_obj(module_kset, KBUILD_MODNAME);
	if (!module_kobj) {
		pr_err("Cannot find kobject for module %s\n", KBUILD_MODNAME);
		ret = -ENOENT;
		goto failed;
	}

	ret = create_cluster_lvl_nodes(lpm_root_node, module_kobj);
	if (ret) {
		pr_err("Failed to create cluster level nodes\n");
		goto failed;
	}

	return 0;
failed:
	free_cluster_node(lpm_root_node);
	lpm_root_node = NULL;
	return ret;
}

static const struct of_device_id lpm_mtch_tbl[] = {
	{.compatible = "qcom,lpm-levels"},
	{},
};

static struct platform_driver lpm_driver = {
	.probe = lpm_probe,
	.driver = {
		.name = "lpm-levels",
		.suppress_bind_attrs = true,
		.of_match_table = lpm_mtch_tbl,
	},
};

static int __init lpm_levels_module_init(void)
{
	int rc;

#ifdef CONFIG_ARM
	int cpu;

	for_each_possible_cpu(cpu) {
		rc = arm_cpuidle_init(cpu);
		if (rc) {
			pr_err("CPU%d ARM CPUidle init failed (%d)\n", cpu, rc);
			return rc;
		}
	}
#endif

	rc = platform_driver_register(&lpm_driver);
	if (rc)
		pr_info("Error registering %s rc=%d\n", lpm_driver.driver.name,
									rc);

	return rc;
}
late_initcall(lpm_levels_module_init);
