/* SPDX-License-Identifier: GPL-2.0 */
/*
 * include/linux/arch_topology.h - arch specific cpu topology information
 */
#ifndef _LINUX_ARCH_TOPOLOGY_H_
#define _LINUX_ARCH_TOPOLOGY_H_

#include <linux/types.h>
#include <linux/percpu.h>

void topology_normalize_cpu_scale(void);
int topology_update_cpu_topology(void);

struct device_node;
bool topology_parse_cpu_capacity(struct device_node *cpu_node, int cpu);

DECLARE_PER_CPU(unsigned long, cpu_scale);

struct sched_domain;
static inline
unsigned long topology_get_cpu_scale(int cpu)
{
	return per_cpu(cpu_scale, cpu);
}

void topology_set_cpu_scale(unsigned int cpu, unsigned long capacity);

DECLARE_PER_CPU(unsigned long, freq_scale);

static inline
unsigned long topology_get_freq_scale(int cpu)
{
	return per_cpu(freq_scale, cpu);
}

DECLARE_PER_CPU(unsigned long, max_freq_scale);

static inline
unsigned long topology_get_max_freq_scale(struct sched_domain *sd, int cpu)
{
	return per_cpu(max_freq_scale, cpu);
}

DECLARE_PER_CPU(unsigned long, arch_min_freq_scale);

static inline unsigned long topology_get_min_freq_scale(int cpu)
{
	return per_cpu(arch_min_freq_scale, cpu);
}

DECLARE_PER_CPU(unsigned long, thermal_pressure);

static inline unsigned long topology_get_thermal_pressure(int cpu)
{
	return per_cpu(thermal_pressure, cpu);
}

void arch_set_thermal_pressure(struct cpumask *cpus,
			       unsigned long th_pressure);
#endif /* _LINUX_ARCH_TOPOLOGY_H_ */
