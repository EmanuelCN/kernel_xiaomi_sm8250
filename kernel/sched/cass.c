// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023-2024 Sultan Alsawaf <sultan@kerneltoast.com>.
 */

/**
 * DOC: Capacity Aware Superset Scheduler (CASS) description
 *
 * The Capacity Aware Superset Scheduler (CASS) optimizes runqueue selection of
 * CFS tasks. By using CPU capacity as a basis for comparing the relative
 * utilization between different CPUs, CASS fairly balances load across CPUs of
 * varying capacities. This results in improved multi-core performance,
 * especially when CPUs are overutilized because CASS doesn't clip a CPU's
 * utilization when it eclipses the CPU's capacity.
 *
 * As a superset of capacity aware scheduling, CASS implements a hierarchy of
 * criteria to determine the better CPU to wake a task upon between CPUs that
 * have the same relative utilization. This way, single-core performance,
 * latency, and cache affinity are all optimized where possible.
 *
 * CASS doesn't feature explicit energy awareness but its basic load balancing
 * principle results in decreased overall energy, often better than what is
 * possible with explicit energy awareness. By fairly balancing load based on
 * relative utilization, all CPUs are kept at their lowest P-state necessary to
 * satisfy the overall load at any given moment.
 */

struct cass_cpu_cand {
	int cpu;
	unsigned int exit_lat;
	unsigned long cap;
	unsigned long cap_max;
	unsigned long cap_no_therm;
	unsigned long cap_orig;
	unsigned long eff_util;
	unsigned long hard_util;
	unsigned long util;
};

static __always_inline
void cass_cpu_util(struct cass_cpu_cand *c, int this_cpu, bool sync)
{
	struct rq *rq = cpu_rq(c->cpu);
	struct cfs_rq *cfs_rq = &rq->cfs;
	unsigned long est;

	/* Get this CPU's utilization from CFS tasks */
	c->util = READ_ONCE(cfs_rq->avg.util_avg);
	if (sched_feat(UTIL_EST)) {
		est = READ_ONCE(cfs_rq->avg.util_est.enqueued);
		if (est > c->util) {
			/* Don't deduct @current's util from estimated util */
			sync = false;
			c->util = est;
		}
	}

	/*
	 * Deduct @current's util from this CPU if this is a sync wake, unless
	 * @current is an RT task; RT tasks don't have per-entity load tracking.
	 */
	if (sync && c->cpu == this_cpu && !rt_task(current))
		c->util -= min(c->util, task_util(current));

	/* Get the utilization of everything other than CFS tasks */
	c->hard_util = cpu_util_rt(rq) + cpu_util_dl(rq) + cpu_util_irq(rq);

	/*
	 * Account for lost capacity due to time spent in RT/DL tasks and IRQs.
	 * Capacity is considered lost to RT tasks even when @p is an RT task in
	 * order to produce consistently balanced task placement results between
	 * CFS and RT tasks when CASS selects a CPU for them.
	 */
	c->cap = c->cap_max - min(c->hard_util, c->cap_max - 1);

	/* Get the current capacity with thermal pressure excluded */
	c->cap_no_therm = c->cap_orig - min(c->hard_util, c->cap_orig - 1);
}

/*
 * Returns true if @c is a CPU with the maximum possible original capacity and
 * there's only one such CPU in the system (i.e., if @c is the prime CPU).
 */
static __always_inline
bool cass_prime_cpu(const struct cass_cpu_cand *c)
{
	/*
	 * On arm64, the prime CPU is always the last CPU. If it doesn't have
	 * the same original capacity as the prior CPU, then it is prime.
	 */
	return c->cpu == nr_cpu_ids - 1 &&
	       arch_scale_cpu_capacity(nr_cpu_ids - 2) != SCHED_CAPACITY_SCALE;
}

/* Returns true if @a is a better CPU than @b */
static __always_inline
bool cass_cpu_better(const struct cass_cpu_cand *a,
		     const struct cass_cpu_cand *b, unsigned long p_util,
		     int this_cpu, int prev_cpu, bool sync)
{
#define cass_cmp(a, b) ({ res = (a) - (b); })
#define cass_eq(a, b) ({ res = (a) == (b); })
	long res;

	/* Prefer the CPU that's not overloaded */
	if (cass_cmp(b->eff_util / b->cap_max, a->eff_util / a->cap_max))
		goto done;

	/* Prefer the CPU that's less overloaded if they're both overloaded */
	if (b->eff_util > b->cap_max && a->eff_util > a->cap_max &&
	    cass_cmp(b->eff_util * SCHED_CAPACITY_SCALE / b->cap_max,
		     a->eff_util * SCHED_CAPACITY_SCALE / a->cap_max))
		goto done;

	/* Prefer the CPU that fits the task */
	if (cass_cmp(fits_capacity(p_util, a->cap_max),
		     fits_capacity(p_util, b->cap_max)))
		goto done;

	/* Prefer the CPU that isn't the single fastest one in the system */
	if (cass_cmp(cass_prime_cpu(b), cass_prime_cpu(a)))
		goto done;

	/* Prefer the CPU with lower relative utilization */
	if (cass_cmp(b->util, a->util))
		goto done;

	/* Prefer the CPU that is idle (only relevant for uclamped tasks) */
	if (cass_cmp(!!a->exit_lat, !!b->exit_lat))
		goto done;

	/* Prefer the current CPU for sync wakes */
	if (sync && (cass_eq(a->cpu, this_cpu) || !cass_cmp(b->cpu, this_cpu)))
		goto done;

	/* Prefer the CPU with higher capacity */
	if (cass_cmp(a->cap, b->cap))
		goto done;

	/* Prefer the CPU with lower idle exit latency */
	if (cass_cmp(b->exit_lat, a->exit_lat))
		goto done;

	/* Prefer the previous CPU */
	if (cass_eq(a->cpu, prev_cpu) || !cass_cmp(b->cpu, prev_cpu))
		goto done;

	/* Prefer the CPU that shares a cache with the previous CPU */
	if (cass_cmp(cpus_share_cache(a->cpu, prev_cpu),
		     cpus_share_cache(b->cpu, prev_cpu)))
		goto done;

	/* @a isn't a better CPU than @b. @res must be <=0 to indicate such. */
done:
	/* @a is a better CPU than @b if @res is positive */
	return res > 0;
}

static int cass_best_cpu(struct task_struct *p, int prev_cpu, bool sync, bool rt)
{
	/* Initialize @best such that @best always has a valid CPU at the end */
	struct cass_cpu_cand cands[2], *best = cands;
	int this_cpu = raw_smp_processor_id();
	unsigned long p_util, uc_min;
	bool has_idle = false;
	int cidx = 0, cpu;

	/*
	 * Get the utilization and uclamp minimum threshold for this task. Note
	 * that RT tasks don't have per-entity load tracking.
	 */
	p_util = rt ? 0 : task_util_est(p);
	uc_min = uclamp_eff_value(p, UCLAMP_MIN);

	/*
	 * Find the best CPU to wake @p on. Although idle_get_state() requires
	 * an RCU read lock, an RCU read lock isn't needed because we're not
	 * preemptible and RCU-sched is unified with normal RCU. Therefore,
	 * non-preemptible contexts are implicitly RCU-safe.
	 *
	 * Note: @curr->cpu must be initialized before this loop ends. This is
	 * necessary to ensure @best->cpu contains a valid CPU upon returning;
	 * otherwise, if only one CPU is allowed and it is skipped before
	 * @curr->cpu is set, then @best->cpu will be garbage.
	 */
	for_each_cpu_and(cpu, &p->cpus_allowed, cpu_active_mask) {
		/* Use the free candidate slot for @curr */
		struct cass_cpu_cand *curr = &cands[cidx];
		struct cpuidle_state *idle_state;
		struct rq *rq = cpu_rq(cpu);

		/* Get the original, maximum _possible_ capacity of this CPU */
		curr->cap_orig = arch_scale_cpu_capacity(cpu);

		/* Get the _current_, throttled maximum capacity of this CPU */
		curr->cap_max = curr->cap_orig - thermal_load_avg(rq);

		/* Prefer the CPU that more closely meets the uclamp minimum */
		if (curr->cap_max < uc_min && curr->cap_max < best->cap_max)
			continue;

		/*
		 * Check if this CPU is idle or only has SCHED_IDLE tasks. For
		 * sync wakes, treat the current CPU as idle if @current is the
		 * only running task.
		 */
		curr->cpu = cpu;
		if ((sync && cpu == this_cpu && rq->nr_running == 1) ||
		    available_idle_cpu(cpu) || sched_idle_cpu(cpu)) {
			/*
			 * A non-idle candidate may be better for energy
			 * efficiency when @p is uclamp boosted, or when the
			 * only idle candidate found so far is the prime CPU.
			 * Otherwise, prefer idle candidates.
			 */
			if (!uc_min && !cass_prime_cpu(curr)) {
				/* Discard any previous non-idle candidate */
				if (!has_idle)
					best = curr;
				has_idle = true;
			}

			/* Nonzero exit latency indicates this CPU is idle */
			curr->exit_lat = 1;

			/* Add on the actual idle exit latency, if any */
			idle_state = idle_get_state(rq);
			if (idle_state)
				curr->exit_lat += idle_state->exit_latency;
		} else {
			/* Skip non-idle CPUs if there's an idle candidate */
			if (has_idle)
				continue;

			/* Zero exit latency indicates this CPU isn't idle */
			curr->exit_lat = 0;
		}

		/* Get this CPU's capacity and utilization */
		cass_cpu_util(curr, this_cpu, sync);

		/*
		 * Add @p's utilization to this CPU if it's not @p's CPU, to
		 * find what this CPU's relative utilization would look like if
		 * @p were on it.
		 */
		if (cpu != task_cpu(p))
			curr->util += p_util;

		/*
		 * Calculate the effective utilization for this CPU candidate;
		 * i.e., the utilization calculated by the CPU governor. This is
		 * needed to evaluate whether or not a throttled CPU is
		 * overloaded, since the relative utilization calculation
		 * disregards thermal pressure.
		 */
		curr->eff_util = max(curr->util + curr->hard_util, uc_min);

		/* Clamp the utilization to the minimum performance threshold */
		if (curr->util < uc_min)
			curr->util = uc_min;

		/*
		 * Calculate the relative utilization for this CPU candidate
		 * without thermal pressure included. Thermal pressure needs to
		 * be disregarded in order to fairly distribute load such that
		 * higher P-states aren't pushed on CPUs that are throttled to a
		 * lesser degree. For example, if CPU A were throttled to 50% of
		 * its maximum possible capacity, and CASS targeted 20% relative
		 * load on all CPUs, CPU A would receive (20% * 50%) = 10% load
		 * relative to its maximum possible P-state. This burden would
		 * then be redistributed to other CPUs, causing a load imbalance
		 * that would reduce CASS's energy efficiency due to
		 * disproportionate P-states.
		 */
		curr->util =
			curr->util * SCHED_CAPACITY_SCALE / curr->cap_no_therm;

		/*
		 * Check if this CPU is better than the best CPU found so far.
		 * If @best == @curr then there's no need to compare them, but
		 * cidx still needs to be changed to the other candidate slot.
		 */
		if (best == curr ||
		    cass_cpu_better(curr, best, p_util, this_cpu, prev_cpu,
				    sync)) {
			best = curr;
			cidx ^= 1;
		}
	}

	return best->cpu;
}

static int cass_select_task_rq(struct task_struct *p, int prev_cpu,
			       int wake_flags, bool rt)
{
	bool sync;

	/* Don't balance on exec since we don't know what @p will look like */
	if (wake_flags & SD_BALANCE_EXEC)
		return prev_cpu;

	/*
	 * If there aren't any valid CPUs which are active, then just return the
	 * first valid CPU since it's possible for certain types of tasks to run
	 * on inactive CPUs.
	 */
	if (unlikely(!cpumask_intersects(&p->cpus_allowed, cpu_active_mask)))
		return cpumask_first(&p->cpus_allowed);

	/* cass_best_cpu() needs the CFS task's utilization, so sync it up */
	if (!rt && !(wake_flags & SD_BALANCE_FORK))
		sync_entity_load_avg(&p->se);

	sync = (wake_flags & WF_SYNC) && !(current->flags & PF_EXITING);
	return cass_best_cpu(p, prev_cpu, sync, rt);
}

static int cass_select_task_rq_fair(struct task_struct *p, int prev_cpu,
				    int sd_flag, int wake_flags)
{
	return cass_select_task_rq(p, prev_cpu, wake_flags, false);
}

int cass_select_task_rq_rt(struct task_struct *p, int prev_cpu,
 			   int sd_flag, int wake_flags)
{
	return cass_select_task_rq(p, prev_cpu, wake_flags, true);
}
