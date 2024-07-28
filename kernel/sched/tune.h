
#ifdef CONFIG_SCHED_TUNE

#include <linux/reciprocal_div.h>

/*
 * System energy normalization constants
 */
struct target_nrg {
	unsigned long min_power;
	unsigned long max_power;
	struct reciprocal_value rdiv;
};

int schedtune_cpu_boost_with(int cpu, struct task_struct *p);
int schedtune_task_boost(struct task_struct *tsk);

int schedtune_prefer_idle(struct task_struct *tsk);
bool schedtune_prefer_high_cap(struct task_struct *tsk);

void schedtune_enqueue_task(struct task_struct *p, int cpu);
void schedtune_dequeue_task(struct task_struct *p, int cpu);

#else /* CONFIG_SCHED_TUNE */

#define schedtune_cpu_boost_with(cpu, p) 0
#ifdef CONFIG_UCLAMP_TASK
#define schedtune_task_boost(tsk) uclamp_eff_value(p, UCLAMP_MIN) > 0
#else
#define schedtune_task_boost(tsk) 0
#endif

#ifdef CONFIG_UCLAMP_TASK_GROUP
#define schedtune_prefer_idle(tsk) uclamp_latency_sensitive(tsk)
#else
#define schedtune_prefer_idle(tsk) 0
#endif

#ifdef CONFIG_UCLAMP_TASK
#define schedtune_prefer_high_cap(tsk) uclamp_boosted(tsk)
#else
#define schedtune_prefer_high_cap(tsk) 0
#endif

#define schedtune_enqueue_task(task, cpu) do { } while (0)
#define schedtune_dequeue_task(task, cpu) do { } while (0)

#endif /* CONFIG_SCHED_TUNE */
