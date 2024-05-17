// SPDX-License-Identifier: GPL-2.0

#include <linux/syscore_ops.h>

/* ========================= define data struct =========================== */
struct rotation_data {
	struct task_struct *rotation_thread;
	struct task_struct *src_task;
	struct task_struct *dst_task;
	int src_cpu;
	int dst_cpu;
};

#define ENABLE_DELAY_SEC	60
#define BIG_TASK_NUM		4
/* default enable rotation feature */
static bool rotation_enable;
/* default threshold value is 40ms */
static u64 threshold_time = 40000000;

/* after system start 30s, start rotation feature.*/
static struct timer_list rotation_timer;

static DEFINE_PER_CPU(struct rotation_data, rotation_datas);

struct kobject *rotation_global_kobject;

DEFINE_PER_CPU_SHARED_ALIGNED(bool, cpu_reserved);
static ktime_t ktime_last;
static __read_mostly bool sched_ktime_suspended;

/* core function */
void check_for_task_rotation(struct rq *src_rq)
{
	int i, src_cpu = cpu_of(src_rq);
	struct rq *dst_rq;
	int deserved_cpu = nr_cpu_ids, dst_cpu = nr_cpu_ids;
	struct rotation_data *rd = NULL;
	u64 wc, wait, max_wait = 0;
	u64 run, max_run = 0;
	int big_task = 0;

	if (!rotation_enable)
		return;

	if (!cpumask_test_cpu(src_cpu, &min_cap_cpu_mask))
		return;

	for_each_possible_cpu(i) {
		struct rq *rq = cpu_rq(i);
		struct task_struct *curr_task = rq->curr;

		if (curr_task->sched_class == &fair_sched_class &&
		    !fits_capacity(task_util_est(curr_task), capacity_of(i)))
			big_task += 1;
	}
	if (big_task < BIG_TASK_NUM)
		return;

	wc = sched_ktime_clock();
	for_each_cpu(i, &min_cap_cpu_mask) {
		struct rq *rq = cpu_rq(i);
		struct task_struct *curr_task = rq->curr;

		if (!rq->misfit_task_load || is_reserved(i) ||
		    curr_task->sched_class != &fair_sched_class ||
		    fits_capacity(task_util_est(curr_task), capacity_of(i)))
			continue;

		wait = wc - curr_task->last_enqueue_ts;
		if (wait > max_wait) {
			max_wait = wait;
			deserved_cpu = i;
		}
	}

	if (deserved_cpu != src_cpu)
		return;

	for_each_cpu_not(i, &min_cap_cpu_mask) {
		struct rq *rq = cpu_rq(i);

		if (is_reserved(i))
			continue;

		if (rq->curr->sched_class != &fair_sched_class)
			continue;

		if (rq->nr_running > 1)
			continue;

		run = wc - rq->curr->last_enqueue_ts;

		if (run < threshold_time)
			continue;

		if (run > max_run) {
			max_run = run;
			dst_cpu = i;
		}
	}

	if (dst_cpu == nr_cpu_ids)
		return;

	dst_rq = cpu_rq(dst_cpu);

	double_rq_lock(src_rq, dst_rq);
	if (dst_rq->curr->sched_class == &fair_sched_class) {

		if (!cpumask_test_cpu(dst_cpu, src_rq->curr->cpus_ptr) ||
		    !cpumask_test_cpu(src_cpu, dst_rq->curr->cpus_ptr)) {
			double_rq_unlock(src_rq, dst_rq);
			return;
		}

		get_task_struct(src_rq->curr);
		get_task_struct(dst_rq->curr);

		mark_reserved(src_cpu);
		mark_reserved(dst_cpu);

		rd = &per_cpu(rotation_datas, src_cpu);

		rd->src_task = src_rq->curr;
		rd->dst_task = dst_rq->curr;

		rd->src_cpu = src_cpu;
		rd->dst_cpu = dst_cpu;
	}
	double_rq_unlock(src_rq, dst_rq);

	if (rd) {
		wake_up_process(rd->rotation_thread);
		trace_sched_task_rotation(rd->src_cpu, rd->dst_cpu,
				rd->src_task->pid, rd->dst_task->pid);
	}
}

static void do_rotation_task(struct rotation_data *rd)
{
	migrate_swap(rd->src_task, rd->dst_task, rd->dst_cpu, rd->src_cpu);

	put_task_struct(rd->src_task);
	put_task_struct(rd->dst_task);

	clear_reserved(rd->src_cpu);
	clear_reserved(rd->dst_cpu);
}

static int __ref try_rotation_task(void *data)
{
	struct rotation_data *rd = data;

	do {
		do_rotation_task(rd);
		set_current_state(TASK_INTERRUPTIBLE);
		schedule();
	} while (!kthread_should_stop());

	return 0;
}

static void set_rotation_enable(struct timer_list *t)
{
	rotation_enable = true;
	pr_info("start rotation feature\n");
}

/****************sysfs interface***********************/
static ssize_t show_rd_enable(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", rotation_enable);
}

static ssize_t store_rd_enable(struct kobject *kobj, struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	int val;
	int ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret || val < 0 || val > 1)
		return -EINVAL;

	rotation_enable = !!val;

	return count;
}

static ssize_t show_rd_threshold(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	u64 threshold_ms = threshold_time;

	do_div(threshold_ms, 1000000);
	return sprintf(buf, "%llums\n", threshold_ms);
}

static ssize_t store_rd_threshold(struct kobject *kobj, struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	unsigned int val;
	int ret;

	ret = kstrtou32(buf, 10, &val);
	if (ret || val <= 16 || val >= 1000)
		return -EINVAL;

	/*value is ms unit*/
	threshold_time = val * 1000000;

	return count;
}

static struct kobj_attribute rd_enable_attr =
	__ATTR(enable, 0644, show_rd_enable, store_rd_enable);

static struct kobj_attribute rd_threshold_attr =
	__ATTR(threshold, 0644, show_rd_threshold, store_rd_threshold);

/* can add more than info. */
static struct attribute *attrs[] = {
	&rd_enable_attr.attr,
	&rd_threshold_attr.attr,
	NULL,
};

static struct attribute_group rotation_attr_group = {
	.attrs = attrs,
};

static int __init rotation_task_init(void)
{
	int ret = 0;
	int i;

	rotation_enable = false;

	for_each_possible_cpu(i) {
		struct rotation_data *rd = &per_cpu(rotation_datas, i);
		struct sched_param param = { .sched_priority = MAX_RT_PRIO - 1 };
		struct task_struct *thread;

		thread = kthread_create(try_rotation_task, (void *)rd,
					"sprd-rotation/%d", i);
		if (IS_ERR(thread))
			return PTR_ERR(thread);

		ret = sched_setscheduler_nocheck(thread, SCHED_FIFO, &param);
		if (ret) {
			kthread_stop(thread);
			return ret;
		}

		rd->rotation_thread = thread;
	}

	rotation_global_kobject = kobject_create_and_add("sprd_rotation",
						&cpu_subsys.dev_root->kobj);
	if (!rotation_global_kobject)
		return -ENOMEM;

	ret = sysfs_create_group(rotation_global_kobject, &rotation_attr_group);
	if (ret) {
		pr_err("create rotation attribute info fail\n");
		kobject_put(rotation_global_kobject);
		return ret;
	}

	timer_setup(&rotation_timer, set_rotation_enable, 0);
	rotation_timer.expires = jiffies + ENABLE_DELAY_SEC * HZ;
	add_timer(&rotation_timer);

	pr_info("%s OK\n", __func__);

	return ret;
}

late_initcall(rotation_task_init);

u64 sched_ktime_clock(void)
{
	if (unlikely(sched_ktime_suspended))
		return ktime_to_ns(ktime_last);
	return ktime_get_ns();
}

static void sched_resume(void)
{
	sched_ktime_suspended = false;
}

static int sched_suspend(void)
{
	ktime_last = ktime_get();
	sched_ktime_suspended = true;
	return 0;
}

static struct syscore_ops sched_syscore_ops = {
	.resume	= sched_resume,
	.suspend = sched_suspend
};

static int __init sched_init_ops(void)
{
	register_syscore_ops(&sched_syscore_ops);
	return 0;
}
late_initcall(sched_init_ops);
