/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The least significant 2 bits of the owner value has the following
 * meanings when set.
 *  - RWSEM_READER_OWNED (bit 0): The rwsem is owned by readers
 *  - RWSEM_ANONYMOUSLY_OWNED (bit 1): The rwsem is anonymously owned,
 *    i.e. the owner(s) cannot be readily determined. It can be reader
 *    owned or the owning writer is indeterminate.
 *
 * When a writer acquires a rwsem, it puts its task_struct pointer
 * into the owner field. It is cleared after an unlock.
 *
 * When a reader acquires a rwsem, it will also puts its task_struct
 * pointer into the owner field with both the RWSEM_READER_OWNED and
 * RWSEM_ANONYMOUSLY_OWNED bits set. On unlock, the owner field will
 * largely be left untouched. So for a free or reader-owned rwsem,
 * the owner value may contain information about the last reader that
 * acquires the rwsem. The anonymous bit is set because that particular
 * reader may or may not still own the lock.
 *
 * That information may be helpful in debugging cases where the system
 * seems to hang on a reader owned rwsem especially if only one reader
 * is involved. Ideally we would like to track all the readers that own
 * a rwsem, but the overhead is simply too big.
 */
#define RWSEM_READER_OWNED	(1UL << 0)
#define RWSEM_ANONYMOUSLY_OWNED	(1UL << 1)

#ifdef CONFIG_DEBUG_RWSEMS
# define DEBUG_RWSEMS_WARN_ON(c)	DEBUG_LOCKS_WARN_ON(c)
#else
# define DEBUG_RWSEMS_WARN_ON(c)
#endif

enum rwsem_waiter_type {
	RWSEM_WAITING_FOR_WRITE,
	RWSEM_WAITING_FOR_READ
};

struct rwsem_waiter {
	struct list_head list;
	struct task_struct *task;
	enum rwsem_waiter_type type;
};

#ifdef CONFIG_RWSEM_SPIN_ON_OWNER
/*
 * All writes to owner are protected by WRITE_ONCE() to make sure that
 * store tearing can't happen as optimistic spinners may read and use
 * the owner value concurrently without lock. Read from owner, however,
 * may not need READ_ONCE() as long as the pointer value is only used
 * for comparison and isn't being dereferenced.
 */
static inline void rwsem_set_owner(struct rw_semaphore *sem)
{
	WRITE_ONCE(sem->owner, current);
}

static inline void rwsem_clear_owner(struct rw_semaphore *sem)
{
	WRITE_ONCE(sem->owner, NULL);
}

/*
 * The task_struct pointer of the last owning reader will be left in
 * the owner field.
 *
 * Note that the owner value just indicates the task has owned the rwsem
 * previously, it may not be the real owner or one of the real owners
 * anymore when that field is examined, so take it with a grain of salt.
 */
static inline void __rwsem_set_reader_owned(struct rw_semaphore *sem,
					    struct task_struct *owner)
{
	unsigned long val = (unsigned long)owner | RWSEM_READER_OWNED
						 | RWSEM_ANONYMOUSLY_OWNED;

	WRITE_ONCE(sem->owner, (struct task_struct *)val);
}

static inline void rwsem_set_reader_owned(struct rw_semaphore *sem)
{
	__rwsem_set_reader_owned(sem, current);
}

/*
 * Return true if the a rwsem waiter can spin on the rwsem's owner
 * and steal the lock, i.e. the lock is not anonymously owned.
 * N.B. !owner is considered spinnable.
 */
static inline bool is_rwsem_owner_spinnable(struct task_struct *owner)
{
	return !((unsigned long)owner & RWSEM_ANONYMOUSLY_OWNED);
}

/*
 * Return true if rwsem is owned by an anonymous writer or readers.
 */
static inline bool rwsem_has_anonymous_owner(struct task_struct *owner)
{
	return (unsigned long)owner & RWSEM_ANONYMOUSLY_OWNED;
}

#ifdef CONFIG_DEBUG_RWSEMS
/*
 * With CONFIG_DEBUG_RWSEMS configured, it will make sure that if there
 * is a task pointer in owner of a reader-owned rwsem, it will be the
 * real owner or one of the real owners. The only exception is when the
 * unlock is done by up_read_non_owner().
 */
#define rwsem_clear_reader_owned rwsem_clear_reader_owned
static inline void rwsem_clear_reader_owned(struct rw_semaphore *sem)
{
	unsigned long val = (unsigned long)current | RWSEM_READER_OWNED
						   | RWSEM_ANONYMOUSLY_OWNED;
	if (READ_ONCE(sem->owner) == (struct task_struct *)val)
		cmpxchg_relaxed((unsigned long *)&sem->owner, val,
				RWSEM_READER_OWNED | RWSEM_ANONYMOUSLY_OWNED);
}
#endif

#else
static inline void rwsem_set_owner(struct rw_semaphore *sem)
{
}

static inline void rwsem_clear_owner(struct rw_semaphore *sem)
{
}

static inline void __rwsem_set_reader_owned(struct rw_semaphore *sem,
					   struct task_struct *owner)
{
}

static inline void rwsem_set_reader_owned(struct rw_semaphore *sem)
{
}
#endif

#ifndef rwsem_clear_reader_owned
static inline void rwsem_clear_reader_owned(struct rw_semaphore *sem)
{
}
#endif

#ifdef CONFIG_RWSEM_PRIO_AWARE

#define RWSEM_MAX_PREEMPT_ALLOWED 3000

/*
 * Return true if current waiter is added in the front of the rwsem wait list.
 */
static inline bool rwsem_list_add_per_prio(struct rwsem_waiter *waiter_in,
				    struct rw_semaphore *sem)
{
	struct list_head *pos;
	struct list_head *head;
	struct rwsem_waiter *waiter = NULL;

	pos = head = &sem->wait_list;
	/*
	 * Rules for task prio aware rwsem wait list queueing:
	 * 1:	Only try to preempt waiters with which task priority
	 *	which is higher than DEFAULT_PRIO.
	 * 2:	To avoid starvation, add count to record
	 *	how many high priority waiters preempt to queue in wait
	 *	list.
	 *	If preempt count is exceed RWSEM_MAX_PREEMPT_ALLOWED,
	 *	use simple fifo until wait list is empty.
	 */
	if (list_empty(head)) {
		list_add_tail(&waiter_in->list, head);
		sem->m_count = 0;
		return true;
	}

	if (waiter_in->task->prio < DEFAULT_PRIO
		&& sem->m_count < RWSEM_MAX_PREEMPT_ALLOWED) {

		list_for_each(pos, head) {
			waiter = list_entry(pos, struct rwsem_waiter, list);
			if (waiter->task->prio > waiter_in->task->prio) {
				list_add(&waiter_in->list, pos->prev);
				sem->m_count++;
				return &waiter_in->list == head->next;
			}
		}
	}

	list_add_tail(&waiter_in->list, head);

	return false;
}
#else
static inline bool rwsem_list_add_per_prio(struct rwsem_waiter *waiter_in,
				    struct rw_semaphore *sem)
{
	list_add_tail(&waiter_in->list, &sem->wait_list);
	return false;
}
#endif
