/*
 * Interface for controlling IO bandwidth on a request queue
 *
 * Copyright (C) 2010 Vivek Goyal <vgoyal@redhat.com>
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/blktrace_api.h>
#include "blk-cgroup.h"
#include "blk.h"

/* Max dispatch from a group in 1 round */
static int throtl_grp_quantum = 8;

/* Total max dispatch from all groups in one round */
static int throtl_quantum = 32;

/* Throttling is performed over 100ms slice and after that slice is renewed */
static unsigned long throtl_slice = HZ/10;	/* 100 ms */

static struct blkcg_policy blkcg_policy_throtl;

/* A workqueue to queue throttle related work */
static struct workqueue_struct *kthrotld_workqueue;

struct throtl_service_queue {
	/*
	 * Bios queued directly to this service_queue or dispatched from
	 * children throtl_grp's.
	 */
	struct bio_list		bio_lists[2];	/* queued bios [READ/WRITE] */
	unsigned int		nr_queued[2];	/* number of queued bios */

	/*
	 * RB tree of active children throtl_grp's, which are sorted by
	 * their ->disptime.
	 */
	struct rb_root		pending_tree;	/* RB tree of active tgs */
	struct rb_node		*first_pending;	/* first node in the tree */
	unsigned int		nr_pending;	/* # queued in the tree */
	unsigned long		first_pending_disptime;	/* disptime of the first tg */
};

enum tg_state_flags {
	THROTL_TG_PENDING	= 1 << 0,	/* on parent's pending tree */
	THROTL_TG_WAS_EMPTY	= 1 << 1,	/* bio_lists[] became non-empty */
};

#define rb_entry_tg(node)	rb_entry((node), struct throtl_grp, rb_node)

/* Per-cpu group stats */
struct tg_stats_cpu {
	/* total bytes transferred */
	struct blkg_rwstat		service_bytes;
	/* total IOs serviced, post merge */
	struct blkg_rwstat		serviced;
};

struct throtl_grp {
	/* must be the first member */
	struct blkg_policy_data pd;

	/* active throtl group service_queue member */
	struct rb_node rb_node;

	/* throtl_data this group belongs to */
	struct throtl_data *td;

	/* this group's service queue */
	struct throtl_service_queue service_queue;

	/*
	 * Dispatch time in jiffies. This is the estimated time when group
	 * will unthrottle and is ready to dispatch more bio. It is used as
	 * key to sort active groups in service tree.
	 */
	unsigned long disptime;

	unsigned int flags;

	/* bytes per second rate limits */
	uint64_t bps[2];

	/* IOPS limits */
	unsigned int iops[2];

	/* Number of bytes disptached in current slice */
	uint64_t bytes_disp[2];
	/* Number of bio's dispatched in current slice */
	unsigned int io_disp[2];

	/* When did we start a new slice */
	unsigned long slice_start[2];
	unsigned long slice_end[2];

	/* Per cpu stats pointer */
	struct tg_stats_cpu __percpu *stats_cpu;

	/* List of tgs waiting for per cpu stats memory to be allocated */
	struct list_head stats_alloc_node;
};

struct throtl_data
{
	/* service tree for active throtl groups */
	struct throtl_service_queue service_queue;

	struct request_queue *queue;

	/* Total Number of queued bios on READ and WRITE lists */
	unsigned int nr_queued[2];

	/*
	 * number of total undestroyed groups
	 */
	unsigned int nr_undestroyed_grps;

	/* Work for dispatching throttled bios */
	struct delayed_work dispatch_work;
};

/* list and work item to allocate percpu group stats */
static DEFINE_SPINLOCK(tg_stats_alloc_lock);
static LIST_HEAD(tg_stats_alloc_list);

static void tg_stats_alloc_fn(struct work_struct *);
static DECLARE_DELAYED_WORK(tg_stats_alloc_work, tg_stats_alloc_fn);

static inline struct throtl_grp *pd_to_tg(struct blkg_policy_data *pd)
{
	return pd ? container_of(pd, struct throtl_grp, pd) : NULL;
}

static inline struct throtl_grp *blkg_to_tg(struct blkcg_gq *blkg)
{
	return pd_to_tg(blkg_to_pd(blkg, &blkcg_policy_throtl));
}

static inline struct blkcg_gq *tg_to_blkg(struct throtl_grp *tg)
{
	return pd_to_blkg(&tg->pd);
}

static inline struct throtl_grp *td_root_tg(struct throtl_data *td)
{
	return blkg_to_tg(td->queue->root_blkg);
}

/**
 * sq_to_tg - return the throl_grp the specified service queue belongs to
 * @sq: the throtl_service_queue of interest
 *
 * Return the throtl_grp @sq belongs to.  If @sq is the top-level one
 * embedded in throtl_data, %NULL is returned.
 */
static struct throtl_grp *sq_to_tg(struct throtl_service_queue *sq)
{
	if (sq && sq->parent_sq)
		return container_of(sq, struct throtl_grp, service_queue);
	else
		return NULL;
}

/**
 * sq_to_td - return throtl_data the specified service queue belongs to
 * @sq: the throtl_service_queue of interest
 *
 * A service_queue can be embeded in either a throtl_grp or throtl_data.
 * Determine the associated throtl_data accordingly and return it.
 */
static struct throtl_data *sq_to_td(struct throtl_service_queue *sq)
{
	struct throtl_grp *tg = sq_to_tg(sq);

	if (tg)
		return tg->td;
	else
		return container_of(sq, struct throtl_data, service_queue);
}

/**
 * throtl_log - log debug message via blktrace
 * @sq: the service_queue being reported
 * @fmt: printf format string
 * @args: printf args
 *
 * The messages are prefixed with "throtl BLKG_NAME" if @sq belongs to a
 * throtl_grp; otherwise, just "throtl".
 *
 * TODO: this should be made a function and name formatting should happen
 * after testing whether blktrace is enabled.
 */
#define throtl_log(sq, fmt, args...)	do {				\
	struct throtl_grp *__tg = sq_to_tg((sq));			\
	struct throtl_data *__td = sq_to_td((sq));			\
									\
	(void)__td;							\
	if ((__tg)) {							\
		char __pbuf[128];					\
									\
		blkg_path(tg_to_blkg(__tg), __pbuf, sizeof(__pbuf));	\
		blk_add_trace_msg(__td->queue, "throtl %s " fmt, __pbuf, ##args); \
	} else {							\
		blk_add_trace_msg(__td->queue, "throtl " fmt, ##args);	\
	}								\
} while (0)

/*
 * Worker for allocating per cpu stat for tgs. This is scheduled on the
 * system_wq once there are some groups on the alloc_list waiting for
 * allocation.
 */
static void tg_stats_alloc_fn(struct work_struct *work)
{
	static struct tg_stats_cpu *stats_cpu;	/* this fn is non-reentrant */
	struct delayed_work *dwork = to_delayed_work(work);
	bool empty = false;

alloc_stats:
	if (!stats_cpu) {
		stats_cpu = alloc_percpu(struct tg_stats_cpu);
		if (!stats_cpu) {
			/* allocation failed, try again after some time */
			schedule_delayed_work(dwork, msecs_to_jiffies(10));
			return;
		}
	}

	spin_lock_irq(&tg_stats_alloc_lock);

	if (!list_empty(&tg_stats_alloc_list)) {
		struct throtl_grp *tg = list_first_entry(&tg_stats_alloc_list,
							 struct throtl_grp,
							 stats_alloc_node);
		swap(tg->stats_cpu, stats_cpu);
		list_del_init(&tg->stats_alloc_node);
	}

	empty = list_empty(&tg_stats_alloc_list);
	spin_unlock_irq(&tg_stats_alloc_lock);
	if (!empty)
		goto alloc_stats;
}

/* init a service_queue, assumes the caller zeroed it */
static void throtl_service_queue_init(struct throtl_service_queue *sq)
{
	bio_list_init(&sq->bio_lists[0]);
	bio_list_init(&sq->bio_lists[1]);
	sq->pending_tree = RB_ROOT;
}

static void throtl_pd_init(struct blkcg_gq *blkg)
{
	struct throtl_grp *tg = blkg_to_tg(blkg);
	unsigned long flags;

	throtl_service_queue_init(&tg->service_queue);
	RB_CLEAR_NODE(&tg->rb_node);
	tg->td = blkg->q->td;

	tg->bps[READ] = -1;
	tg->bps[WRITE] = -1;
	tg->iops[READ] = -1;
	tg->iops[WRITE] = -1;

	/*
	 * Ugh... We need to perform per-cpu allocation for tg->stats_cpu
	 * but percpu allocator can't be called from IO path.  Queue tg on
	 * tg_stats_alloc_list and allocate from work item.
	 */
	spin_lock_irqsave(&tg_stats_alloc_lock, flags);
	list_add(&tg->stats_alloc_node, &tg_stats_alloc_list);
	schedule_delayed_work(&tg_stats_alloc_work, 0);
	spin_unlock_irqrestore(&tg_stats_alloc_lock, flags);
}

static void throtl_pd_exit(struct blkcg_gq *blkg)
{
	struct throtl_grp *tg = blkg_to_tg(blkg);
	unsigned long flags;

	spin_lock_irqsave(&tg_stats_alloc_lock, flags);
	list_del_init(&tg->stats_alloc_node);
	spin_unlock_irqrestore(&tg_stats_alloc_lock, flags);

	free_percpu(tg->stats_cpu);
}

static void throtl_pd_reset_stats(struct blkcg_gq *blkg)
{
	struct throtl_grp *tg = blkg_to_tg(blkg);
	int cpu;

	if (tg->stats_cpu == NULL)
		return;

	for_each_possible_cpu(cpu) {
		struct tg_stats_cpu *sc = per_cpu_ptr(tg->stats_cpu, cpu);

		blkg_rwstat_reset(&sc->service_bytes);
		blkg_rwstat_reset(&sc->serviced);
	}
}

static struct throtl_grp *throtl_lookup_tg(struct throtl_data *td,
					   struct blkcg *blkcg)
{
	/*
	 * This is the common case when there are no blkcgs.  Avoid lookup
	 * in this case
	 */
	if (blkcg == &blkcg_root)
		return td_root_tg(td);

	return blkg_to_tg(blkg_lookup(blkcg, td->queue));
}

static struct throtl_grp *throtl_lookup_create_tg(struct throtl_data *td,
						  struct blkcg *blkcg)
{
	struct request_queue *q = td->queue;
	struct throtl_grp *tg = NULL;

	/*
	 * This is the common case when there are no blkcgs.  Avoid lookup
	 * in this case
	 */
	if (blkcg == &blkcg_root) {
		tg = td_root_tg(td);
	} else {
		struct blkcg_gq *blkg;

		blkg = blkg_lookup_create(blkcg, q);

		/* if %NULL and @q is alive, fall back to root_tg */
		if (!IS_ERR(blkg))
			tg = blkg_to_tg(blkg);
		else if (!blk_queue_dying(q))
			tg = td_root_tg(td);
	}

	return tg;
}

static struct throtl_grp *
throtl_rb_first(struct throtl_service_queue *parent_sq)
{
	/* Service tree is empty */
	if (!parent_sq->nr_pending)
		return NULL;

	if (!parent_sq->first_pending)
		parent_sq->first_pending = rb_first(&parent_sq->pending_tree);

	if (parent_sq->first_pending)
		return rb_entry_tg(parent_sq->first_pending);

	return NULL;
}

static void rb_erase_init(struct rb_node *n, struct rb_root *root)
{
	rb_erase(n, root);
	RB_CLEAR_NODE(n);
}

static void throtl_rb_erase(struct rb_node *n,
			    struct throtl_service_queue *parent_sq)
{
	if (parent_sq->first_pending == n)
		parent_sq->first_pending = NULL;
	rb_erase_init(n, &parent_sq->pending_tree);
	--parent_sq->nr_pending;
}

static void update_min_dispatch_time(struct throtl_service_queue *parent_sq)
{
	struct throtl_grp *tg;

	tg = throtl_rb_first(parent_sq);
	if (!tg)
		return;

	parent_sq->first_pending_disptime = tg->disptime;
}

static void tg_service_queue_add(struct throtl_grp *tg,
				 struct throtl_service_queue *parent_sq)
{
	struct rb_node **node = &parent_sq->pending_tree.rb_node;
	struct rb_node *parent = NULL;
	struct throtl_grp *__tg;
	unsigned long key = tg->disptime;
	int left = 1;

	while (*node != NULL) {
		parent = *node;
		__tg = rb_entry_tg(parent);

		if (time_before(key, __tg->disptime))
			node = &parent->rb_left;
		else {
			node = &parent->rb_right;
			left = 0;
		}
	}

	if (left)
		parent_sq->first_pending = &tg->rb_node;

	rb_link_node(&tg->rb_node, parent, node);
	rb_insert_color(&tg->rb_node, &parent_sq->pending_tree);
}

static void __throtl_enqueue_tg(struct throtl_grp *tg,
				struct throtl_service_queue *parent_sq)
{
	tg_service_queue_add(tg, parent_sq);
	tg->flags |= THROTL_TG_PENDING;
	parent_sq->nr_pending++;
}

static void throtl_enqueue_tg(struct throtl_grp *tg,
			      struct throtl_service_queue *parent_sq)
{
	if (!(tg->flags & THROTL_TG_PENDING))
		__throtl_enqueue_tg(tg, parent_sq);
}

static void __throtl_dequeue_tg(struct throtl_grp *tg,
				struct throtl_service_queue *parent_sq)
{
	throtl_rb_erase(&tg->rb_node, parent_sq);
	tg->flags &= ~THROTL_TG_PENDING;
}

static void throtl_dequeue_tg(struct throtl_grp *tg,
			      struct throtl_service_queue *parent_sq)
{
	if (tg->flags & THROTL_TG_PENDING)
		__throtl_dequeue_tg(tg, parent_sq);
}

/* Call with queue lock held */
static void throtl_schedule_delayed_work(struct throtl_data *td,
					 unsigned long delay)
{
	struct delayed_work *dwork = &td->dispatch_work;
	struct throtl_service_queue *sq = &td->service_queue;

	mod_delayed_work(kthrotld_workqueue, dwork, delay);
	throtl_log(sq, "schedule work. delay=%lu jiffies=%lu", delay, jiffies);
}

static void throtl_schedule_next_dispatch(struct throtl_data *td)
{
	struct throtl_service_queue *sq = &td->service_queue;

	/* any pending children left? */
	if (!sq->nr_pending)
		return;

	update_min_dispatch_time(sq);

	if (time_before_eq(sq->first_pending_disptime, jiffies))
		throtl_schedule_delayed_work(td, 0);
	else
		throtl_schedule_delayed_work(td, sq->first_pending_disptime - jiffies);
}

static inline void throtl_start_new_slice(struct throtl_grp *tg, bool rw)
{
	tg->bytes_disp[rw] = 0;
	tg->io_disp[rw] = 0;
	tg->slice_start[rw] = jiffies;
	tg->slice_end[rw] = jiffies + throtl_slice;
	throtl_log(&tg->service_queue,
		   "[%c] new slice start=%lu end=%lu jiffies=%lu",
		   rw == READ ? 'R' : 'W', tg->slice_start[rw],
		   tg->slice_end[rw], jiffies);
}

static inline void throtl_set_slice_end(struct throtl_grp *tg, bool rw,
					unsigned long jiffy_end)
{
	tg->slice_end[rw] = roundup(jiffy_end, throtl_slice);
}

static inline void throtl_extend_slice(struct throtl_grp *tg, bool rw,
				       unsigned long jiffy_end)
{
	tg->slice_end[rw] = roundup(jiffy_end, throtl_slice);
	throtl_log(&tg->service_queue,
		   "[%c] extend slice start=%lu end=%lu jiffies=%lu",
		   rw == READ ? 'R' : 'W', tg->slice_start[rw],
		   tg->slice_end[rw], jiffies);
}

/* Determine if previously allocated or extended slice is complete or not */
static bool throtl_slice_used(struct throtl_grp *tg, bool rw)
{
	if (time_in_range(jiffies, tg->slice_start[rw], tg->slice_end[rw]))
		return 0;

	return 1;
}

/* Trim the used slices and adjust slice start accordingly */
static inline void throtl_trim_slice(struct throtl_grp *tg, bool rw)
{
	unsigned long nr_slices, time_elapsed, io_trim;
	u64 bytes_trim, tmp;

	BUG_ON(time_before(tg->slice_end[rw], tg->slice_start[rw]));

	/*
	 * If bps are unlimited (-1), then time slice don't get
	 * renewed. Don't try to trim the slice if slice is used. A new
	 * slice will start when appropriate.
	 */
	if (throtl_slice_used(tg, rw))
		return;

	/*
	 * A bio has been dispatched. Also adjust slice_end. It might happen
	 * that initially cgroup limit was very low resulting in high
	 * slice_end, but later limit was bumped up and bio was dispached
	 * sooner, then we need to reduce slice_end. A high bogus slice_end
	 * is bad because it does not allow new slice to start.
	 */

	throtl_set_slice_end(tg, rw, jiffies + throtl_slice);

	time_elapsed = jiffies - tg->slice_start[rw];

	nr_slices = time_elapsed / throtl_slice;

	if (!nr_slices)
		return;
	tmp = tg->bps[rw] * throtl_slice * nr_slices;
	do_div(tmp, HZ);
	bytes_trim = tmp;

	io_trim = (tg->iops[rw] * throtl_slice * nr_slices)/HZ;

	if (!bytes_trim && !io_trim)
		return;

	if (tg->bytes_disp[rw] >= bytes_trim)
		tg->bytes_disp[rw] -= bytes_trim;
	else
		tg->bytes_disp[rw] = 0;

	if (tg->io_disp[rw] >= io_trim)
		tg->io_disp[rw] -= io_trim;
	else
		tg->io_disp[rw] = 0;

	tg->slice_start[rw] += nr_slices * throtl_slice;

	throtl_log(&tg->service_queue,
		   "[%c] trim slice nr=%lu bytes=%llu io=%lu start=%lu end=%lu jiffies=%lu",
		   rw == READ ? 'R' : 'W', nr_slices, bytes_trim, io_trim,
		   tg->slice_start[rw], tg->slice_end[rw], jiffies);
}

static bool tg_with_in_iops_limit(struct throtl_grp *tg, struct bio *bio,
				  unsigned long *wait)
{
	bool rw = bio_data_dir(bio);
	unsigned int io_allowed;
	unsigned long jiffy_elapsed, jiffy_wait, jiffy_elapsed_rnd;
	u64 tmp;

	jiffy_elapsed = jiffy_elapsed_rnd = jiffies - tg->slice_start[rw];

	/* Slice has just started. Consider one slice interval */
	if (!jiffy_elapsed)
		jiffy_elapsed_rnd = throtl_slice;

	jiffy_elapsed_rnd = roundup(jiffy_elapsed_rnd, throtl_slice);

	/*
	 * jiffy_elapsed_rnd should not be a big value as minimum iops can be
	 * 1 then at max jiffy elapsed should be equivalent of 1 second as we
	 * will allow dispatch after 1 second and after that slice should
	 * have been trimmed.
	 */

	tmp = (u64)tg->iops[rw] * jiffy_elapsed_rnd;
	do_div(tmp, HZ);

	if (tmp > UINT_MAX)
		io_allowed = UINT_MAX;
	else
		io_allowed = tmp;

	if (tg->io_disp[rw] + 1 <= io_allowed) {
		if (wait)
			*wait = 0;
		return 1;
	}

	/* Calc approx time to dispatch */
	jiffy_wait = ((tg->io_disp[rw] + 1) * HZ)/tg->iops[rw] + 1;

	if (jiffy_wait > jiffy_elapsed)
		jiffy_wait = jiffy_wait - jiffy_elapsed;
	else
		jiffy_wait = 1;

	if (wait)
		*wait = jiffy_wait;
	return 0;
}

static bool tg_with_in_bps_limit(struct throtl_grp *tg, struct bio *bio,
				 unsigned long *wait)
{
	bool rw = bio_data_dir(bio);
	u64 bytes_allowed, extra_bytes, tmp;
	unsigned long jiffy_elapsed, jiffy_wait, jiffy_elapsed_rnd;

	jiffy_elapsed = jiffy_elapsed_rnd = jiffies - tg->slice_start[rw];

	/* Slice has just started. Consider one slice interval */
	if (!jiffy_elapsed)
		jiffy_elapsed_rnd = throtl_slice;

	jiffy_elapsed_rnd = roundup(jiffy_elapsed_rnd, throtl_slice);

	tmp = tg->bps[rw] * jiffy_elapsed_rnd;
	do_div(tmp, HZ);
	bytes_allowed = tmp;

	if (tg->bytes_disp[rw] + bio->bi_size <= bytes_allowed) {
		if (wait)
			*wait = 0;
		return 1;
	}

	/* Calc approx time to dispatch */
	extra_bytes = tg->bytes_disp[rw] + bio->bi_size - bytes_allowed;
	jiffy_wait = div64_u64(extra_bytes * HZ, tg->bps[rw]);

	if (!jiffy_wait)
		jiffy_wait = 1;

	/*
	 * This wait time is without taking into consideration the rounding
	 * up we did. Add that time also.
	 */
	jiffy_wait = jiffy_wait + (jiffy_elapsed_rnd - jiffy_elapsed);
	if (wait)
		*wait = jiffy_wait;
	return 0;
}

static bool tg_no_rule_group(struct throtl_grp *tg, bool rw) {
	if (tg->bps[rw] == -1 && tg->iops[rw] == -1)
		return 1;
	return 0;
}

/*
 * Returns whether one can dispatch a bio or not. Also returns approx number
 * of jiffies to wait before this bio is with-in IO rate and can be dispatched
 */
static bool tg_may_dispatch(struct throtl_grp *tg, struct bio *bio,
			    unsigned long *wait)
{
	bool rw = bio_data_dir(bio);
	unsigned long bps_wait = 0, iops_wait = 0, max_wait = 0;

	/*
 	 * Currently whole state machine of group depends on first bio
	 * queued in the group bio list. So one should not be calling
	 * this function with a different bio if there are other bios
	 * queued.
	 */
	BUG_ON(tg->service_queue.nr_queued[rw] &&
	       bio != bio_list_peek(&tg->service_queue.bio_lists[rw]));

	/* If tg->bps = -1, then BW is unlimited */
	if (tg->bps[rw] == -1 && tg->iops[rw] == -1) {
		if (wait)
			*wait = 0;
		return 1;
	}

	/*
	 * If previous slice expired, start a new one otherwise renew/extend
	 * existing slice to make sure it is at least throtl_slice interval
	 * long since now.
	 */
	if (throtl_slice_used(tg, rw))
		throtl_start_new_slice(tg, rw);
	else {
		if (time_before(tg->slice_end[rw], jiffies + throtl_slice))
			throtl_extend_slice(tg, rw, jiffies + throtl_slice);
	}

	if (tg_with_in_bps_limit(tg, bio, &bps_wait) &&
	    tg_with_in_iops_limit(tg, bio, &iops_wait)) {
		if (wait)
			*wait = 0;
		return 1;
	}

	max_wait = max(bps_wait, iops_wait);

	if (wait)
		*wait = max_wait;

	if (time_before(tg->slice_end[rw], jiffies + max_wait))
		throtl_extend_slice(tg, rw, jiffies + max_wait);

	return 0;
}

static void throtl_update_dispatch_stats(struct blkcg_gq *blkg, u64 bytes,
					 int rw)
{
	struct throtl_grp *tg = blkg_to_tg(blkg);
	struct tg_stats_cpu *stats_cpu;
	unsigned long flags;

	/* If per cpu stats are not allocated yet, don't do any accounting. */
	if (tg->stats_cpu == NULL)
		return;

	/*
	 * Disabling interrupts to provide mutual exclusion between two
	 * writes on same cpu. It probably is not needed for 64bit. Not
	 * optimizing that case yet.
	 */
	local_irq_save(flags);

	stats_cpu = this_cpu_ptr(tg->stats_cpu);

	blkg_rwstat_add(&stats_cpu->serviced, rw, 1);
	blkg_rwstat_add(&stats_cpu->service_bytes, rw, bytes);

	local_irq_restore(flags);
}

static void throtl_charge_bio(struct throtl_grp *tg, struct bio *bio)
{
	bool rw = bio_data_dir(bio);

	/* Charge the bio to the group */
	tg->bytes_disp[rw] += bio->bi_size;
	tg->io_disp[rw]++;

	throtl_update_dispatch_stats(tg_to_blkg(tg), bio->bi_size, bio->bi_rw);
}

static void throtl_add_bio_tg(struct bio *bio, struct throtl_grp *tg,
			      struct throtl_service_queue *parent_sq)
{
	struct throtl_service_queue *sq = &tg->service_queue;
	bool rw = bio_data_dir(bio);

	/*
	 * If @tg doesn't currently have any bios queued in the same
	 * direction, queueing @bio can change when @tg should be
	 * dispatched.  Mark that @tg was empty.  This is automatically
	 * cleaered on the next tg_update_disptime().
	 */
	if (!sq->nr_queued[rw])
		tg->flags |= THROTL_TG_WAS_EMPTY;

	bio_list_add(&sq->bio_lists[rw], bio);
	/* Take a bio reference on tg */
	blkg_get(tg_to_blkg(tg));
	sq->nr_queued[rw]++;
	tg->td->nr_queued[rw]++;
	throtl_enqueue_tg(tg, parent_sq);
}

static void tg_update_disptime(struct throtl_grp *tg,
			       struct throtl_service_queue *parent_sq)
{
	struct throtl_service_queue *sq = &tg->service_queue;
	unsigned long read_wait = -1, write_wait = -1, min_wait = -1, disptime;
	struct bio *bio;

	if ((bio = bio_list_peek(&sq->bio_lists[READ])))
		tg_may_dispatch(tg, bio, &read_wait);

	if ((bio = bio_list_peek(&sq->bio_lists[WRITE])))
		tg_may_dispatch(tg, bio, &write_wait);

	min_wait = min(read_wait, write_wait);
	disptime = jiffies + min_wait;

	/* Update dispatch time */
	throtl_dequeue_tg(tg, parent_sq);
	tg->disptime = disptime;
	throtl_enqueue_tg(tg, parent_sq);

	/* see throtl_add_bio_tg() */
	tg->flags &= ~THROTL_TG_WAS_EMPTY;
}

static void tg_dispatch_one_bio(struct throtl_grp *tg, bool rw,
				struct throtl_service_queue *parent_sq)
{
	struct throtl_service_queue *sq = &tg->service_queue;
	struct bio *bio;

	bio = bio_list_pop(&sq->bio_lists[rw]);
	sq->nr_queued[rw]--;
	/* Drop bio reference on blkg */
	blkg_put(tg_to_blkg(tg));

	BUG_ON(tg->td->nr_queued[rw] <= 0);
	tg->td->nr_queued[rw]--;

	throtl_charge_bio(tg, bio);
	bio_list_add(&parent_sq->bio_lists[rw], bio);
	bio->bi_rw |= REQ_THROTTLED;

	throtl_trim_slice(tg, rw);
}

static int throtl_dispatch_tg(struct throtl_grp *tg,
			      struct throtl_service_queue *parent_sq)
{
	struct throtl_service_queue *sq = &tg->service_queue;
	unsigned int nr_reads = 0, nr_writes = 0;
	unsigned int max_nr_reads = throtl_grp_quantum*3/4;
	unsigned int max_nr_writes = throtl_grp_quantum - max_nr_reads;
	struct bio *bio;

	/* Try to dispatch 75% READS and 25% WRITES */

	while ((bio = bio_list_peek(&sq->bio_lists[READ])) &&
	       tg_may_dispatch(tg, bio, NULL)) {

		tg_dispatch_one_bio(tg, bio_data_dir(bio), parent_sq);
		nr_reads++;

		if (nr_reads >= max_nr_reads)
			break;
	}

	while ((bio = bio_list_peek(&sq->bio_lists[WRITE])) &&
	       tg_may_dispatch(tg, bio, NULL)) {

		tg_dispatch_one_bio(tg, bio_data_dir(bio), parent_sq);
		nr_writes++;

		if (nr_writes >= max_nr_writes)
			break;
	}

	return nr_reads + nr_writes;
}

static int throtl_select_dispatch(struct throtl_service_queue *parent_sq)
{
	unsigned int nr_disp = 0;

	while (1) {
		struct throtl_grp *tg = throtl_rb_first(parent_sq);
		struct throtl_service_queue *sq = &tg->service_queue;

		if (!tg)
			break;

		if (time_before(jiffies, tg->disptime))
			break;

		throtl_dequeue_tg(tg, parent_sq);

		nr_disp += throtl_dispatch_tg(tg, parent_sq);

		if (sq->nr_queued[0] || sq->nr_queued[1])
			tg_update_disptime(tg, parent_sq);

		if (nr_disp >= throtl_quantum)
			break;
	}

	return nr_disp;
}

/* work function to dispatch throttled bios */
void blk_throtl_dispatch_work_fn(struct work_struct *work)
{
	struct throtl_data *td = container_of(to_delayed_work(work),
					      struct throtl_data, dispatch_work);
	struct throtl_service_queue *sq = &td->service_queue;
	struct request_queue *q = td->queue;
	unsigned int nr_disp = 0;
	struct bio_list bio_list_on_stack;
	struct bio *bio;
	struct blk_plug plug;
	int rw;

	spin_lock_irq(q->queue_lock);

	bio_list_init(&bio_list_on_stack);

	throtl_log(sq, "dispatch nr_queued=%u read=%u write=%u",
		   td->nr_queued[READ] + td->nr_queued[WRITE],
		   td->nr_queued[READ], td->nr_queued[WRITE]);

	nr_disp = throtl_select_dispatch(sq);

	if (nr_disp) {
		for (rw = READ; rw <= WRITE; rw++) {
			bio_list_merge(&bio_list_on_stack, &sq->bio_lists[rw]);
			bio_list_init(&sq->bio_lists[rw]);
		}
		throtl_log(sq, "bios disp=%u", nr_disp);
	}

	throtl_schedule_next_dispatch(td);

	spin_unlock_irq(q->queue_lock);

	/*
	 * If we dispatched some requests, unplug the queue to make sure
	 * immediate dispatch
	 */
	if (nr_disp) {
		blk_start_plug(&plug);
		while((bio = bio_list_pop(&bio_list_on_stack)))
			generic_make_request(bio);
		blk_finish_plug(&plug);
	}
}

static u64 tg_prfill_cpu_rwstat(struct seq_file *sf,
				struct blkg_policy_data *pd, int off)
{
	struct throtl_grp *tg = pd_to_tg(pd);
	struct blkg_rwstat rwstat = { }, tmp;
	int i, cpu;

	if (tg->stats_cpu == NULL)
		return 0;

	for_each_possible_cpu(cpu) {
		struct tg_stats_cpu *sc = per_cpu_ptr(tg->stats_cpu, cpu);

		tmp = blkg_rwstat_read((void *)sc + off);
		for (i = 0; i < BLKG_RWSTAT_NR; i++)
			rwstat.cnt[i] += tmp.cnt[i];
	}

	return __blkg_prfill_rwstat(sf, pd, &rwstat);
}

static int tg_print_cpu_rwstat(struct cgroup *cgrp, struct cftype *cft,
			       struct seq_file *sf)
{
	struct blkcg *blkcg = cgroup_to_blkcg(cgrp);

	blkcg_print_blkgs(sf, blkcg, tg_prfill_cpu_rwstat, &blkcg_policy_throtl,
			  cft->private, true);
	return 0;
}

static u64 tg_prfill_conf_u64(struct seq_file *sf, struct blkg_policy_data *pd,
			      int off)
{
	struct throtl_grp *tg = pd_to_tg(pd);
	u64 v = *(u64 *)((void *)tg + off);

	if (v == -1)
		return 0;
	return __blkg_prfill_u64(sf, pd, v);
}

static u64 tg_prfill_conf_uint(struct seq_file *sf, struct blkg_policy_data *pd,
			       int off)
{
	struct throtl_grp *tg = pd_to_tg(pd);
	unsigned int v = *(unsigned int *)((void *)tg + off);

	if (v == -1)
		return 0;
	return __blkg_prfill_u64(sf, pd, v);
}

static int tg_print_conf_u64(struct cgroup *cgrp, struct cftype *cft,
			     struct seq_file *sf)
{
	blkcg_print_blkgs(sf, cgroup_to_blkcg(cgrp), tg_prfill_conf_u64,
			  &blkcg_policy_throtl, cft->private, false);
	return 0;
}

static int tg_print_conf_uint(struct cgroup *cgrp, struct cftype *cft,
			      struct seq_file *sf)
{
	blkcg_print_blkgs(sf, cgroup_to_blkcg(cgrp), tg_prfill_conf_uint,
			  &blkcg_policy_throtl, cft->private, false);
	return 0;
}

static int tg_set_conf(struct cgroup *cgrp, struct cftype *cft, const char *buf,
		       bool is_u64)
{
	struct blkcg *blkcg = cgroup_to_blkcg(cgrp);
	struct blkg_conf_ctx ctx;
	struct throtl_grp *tg;
	struct throtl_data *td;
	int ret;

	ret = blkg_conf_prep(blkcg, &blkcg_policy_throtl, buf, &ctx);
	if (ret)
		return ret;

	tg = blkg_to_tg(ctx.blkg);
	td = ctx.blkg->q->td;

	if (!ctx.v)
		ctx.v = -1;

	if (is_u64)
		*(u64 *)((void *)tg + cft->private) = ctx.v;
	else
		*(unsigned int *)((void *)tg + cft->private) = ctx.v;

	throtl_log(&tg->service_queue,
		   "limit change rbps=%llu wbps=%llu riops=%u wiops=%u",
		   tg->bps[READ], tg->bps[WRITE],
		   tg->iops[READ], tg->iops[WRITE]);

	/*
	 * We're already holding queue_lock and know @tg is valid.  Let's
	 * apply the new config directly.
	 *
	 * Restart the slices for both READ and WRITES. It might happen
	 * that a group's limit are dropped suddenly and we don't want to
	 * account recently dispatched IO with new low rate.
	 */
	throtl_start_new_slice(tg, 0);
	throtl_start_new_slice(tg, 1);

	if (tg->flags & THROTL_TG_PENDING) {
		tg_update_disptime(tg, &td->service_queue);
		throtl_schedule_next_dispatch(td);
	}

	blkg_conf_finish(&ctx);
	return 0;
}

static int tg_set_conf_u64(struct cgroup *cgrp, struct cftype *cft,
			   const char *buf)
{
	return tg_set_conf(cgrp, cft, buf, true);
}

static int tg_set_conf_uint(struct cgroup *cgrp, struct cftype *cft,
			    const char *buf)
{
	return tg_set_conf(cgrp, cft, buf, false);
}

static struct cftype throtl_files[] = {
	{
		.name = "throttle.read_bps_device",
		.private = offsetof(struct throtl_grp, bps[READ]),
		.read_seq_string = tg_print_conf_u64,
		.write_string = tg_set_conf_u64,
		.max_write_len = 256,
	},
	{
		.name = "throttle.write_bps_device",
		.private = offsetof(struct throtl_grp, bps[WRITE]),
		.read_seq_string = tg_print_conf_u64,
		.write_string = tg_set_conf_u64,
		.max_write_len = 256,
	},
	{
		.name = "throttle.read_iops_device",
		.private = offsetof(struct throtl_grp, iops[READ]),
		.read_seq_string = tg_print_conf_uint,
		.write_string = tg_set_conf_uint,
		.max_write_len = 256,
	},
	{
		.name = "throttle.write_iops_device",
		.private = offsetof(struct throtl_grp, iops[WRITE]),
		.read_seq_string = tg_print_conf_uint,
		.write_string = tg_set_conf_uint,
		.max_write_len = 256,
	},
	{
		.name = "throttle.io_service_bytes",
		.private = offsetof(struct tg_stats_cpu, service_bytes),
		.read_seq_string = tg_print_cpu_rwstat,
	},
	{
		.name = "throttle.io_serviced",
		.private = offsetof(struct tg_stats_cpu, serviced),
		.read_seq_string = tg_print_cpu_rwstat,
	},
	{ }	/* terminate */
};

static void throtl_shutdown_wq(struct request_queue *q)
{
	struct throtl_data *td = q->td;

	cancel_delayed_work_sync(&td->dispatch_work);
}

static struct blkcg_policy blkcg_policy_throtl = {
	.pd_size		= sizeof(struct throtl_grp),
	.cftypes		= throtl_files,

	.pd_init_fn		= throtl_pd_init,
	.pd_exit_fn		= throtl_pd_exit,
	.pd_reset_stats_fn	= throtl_pd_reset_stats,
};

bool blk_throtl_bio(struct request_queue *q, struct bio *bio)
{
	struct throtl_data *td = q->td;
	struct throtl_grp *tg;
	struct throtl_service_queue *sq;
	bool rw = bio_data_dir(bio);
	struct blkcg *blkcg;
	bool throttled = false;

	if (bio->bi_rw & REQ_THROTTLED) {
		bio->bi_rw &= ~REQ_THROTTLED;
		goto out;
	}

	/*
	 * A throtl_grp pointer retrieved under rcu can be used to access
	 * basic fields like stats and io rates. If a group has no rules,
	 * just update the dispatch stats in lockless manner and return.
	 */
	rcu_read_lock();
	blkcg = bio_blkcg(bio);
	tg = throtl_lookup_tg(td, blkcg);
	if (tg) {
		if (tg_no_rule_group(tg, rw)) {
			throtl_update_dispatch_stats(tg_to_blkg(tg),
						     bio->bi_size, bio->bi_rw);
			goto out_unlock_rcu;
		}
	}

	/*
	 * Either group has not been allocated yet or it is not an unlimited
	 * IO group
	 */
	spin_lock_irq(q->queue_lock);
	tg = throtl_lookup_create_tg(td, blkcg);
	if (unlikely(!tg))
		goto out_unlock;

	sq = &tg->service_queue;

	/* throtl is FIFO - if other bios are already queued, should queue */
	if (sq->nr_queued[rw])
		goto queue_bio;

	/* Bio is with-in rate limit of group */
	if (tg_may_dispatch(tg, bio, NULL)) {
		throtl_charge_bio(tg, bio);

		/*
		 * We need to trim slice even when bios are not being queued
		 * otherwise it might happen that a bio is not queued for
		 * a long time and slice keeps on extending and trim is not
		 * called for a long time. Now if limits are reduced suddenly
		 * we take into account all the IO dispatched so far at new
		 * low rate and * newly queued IO gets a really long dispatch
		 * time.
		 *
		 * So keep on trimming slice even if bio is not queued.
		 */
		throtl_trim_slice(tg, rw);
		goto out_unlock;
	}

queue_bio:
	throtl_log(sq, "[%c] bio. bdisp=%llu sz=%u bps=%llu iodisp=%u iops=%u queued=%d/%d",
		   rw == READ ? 'R' : 'W',
		   tg->bytes_disp[rw], bio->bi_size, tg->bps[rw],
		   tg->io_disp[rw], tg->iops[rw],
		   sq->nr_queued[READ], sq->nr_queued[WRITE]);

	bio_associate_current(bio);
	throtl_add_bio_tg(bio, tg, &q->td->service_queue);
	throttled = true;

	/* update @tg's dispatch time if @tg was empty before @bio */
	if (tg->flags & THROTL_TG_WAS_EMPTY) {
		tg_update_disptime(tg, &td->service_queue);
		throtl_schedule_next_dispatch(td);
	}

out_unlock:
	spin_unlock_irq(q->queue_lock);
out_unlock_rcu:
	rcu_read_unlock();
out:
	return throttled;
}

/**
 * blk_throtl_drain - drain throttled bios
 * @q: request_queue to drain throttled bios for
 *
 * Dispatch all currently throttled bios on @q through ->make_request_fn().
 */
void blk_throtl_drain(struct request_queue *q)
	__releases(q->queue_lock) __acquires(q->queue_lock)
{
	struct throtl_data *td = q->td;
	struct throtl_service_queue *parent_sq = &td->service_queue;
	struct throtl_grp *tg;
	struct bio *bio;
	int rw;

	queue_lockdep_assert_held(q);

	while ((tg = throtl_rb_first(parent_sq))) {
		struct throtl_service_queue *sq = &tg->service_queue;

		throtl_dequeue_tg(tg, parent_sq);

		while ((bio = bio_list_peek(&sq->bio_lists[READ])))
			tg_dispatch_one_bio(tg, bio_data_dir(bio), parent_sq);
		while ((bio = bio_list_peek(&sq->bio_lists[WRITE])))
			tg_dispatch_one_bio(tg, bio_data_dir(bio), parent_sq);
	}
	spin_unlock_irq(q->queue_lock);

	for (rw = READ; rw <= WRITE; rw++)
		while ((bio = bio_list_pop(&parent_sq->bio_lists[rw])))
			generic_make_request(bio);

	spin_lock_irq(q->queue_lock);
}

int blk_throtl_init(struct request_queue *q)
{
	struct throtl_data *td;
	int ret;

	td = kzalloc_node(sizeof(*td), GFP_KERNEL, q->node);
	if (!td)
		return -ENOMEM;

	INIT_DELAYED_WORK(&td->dispatch_work, blk_throtl_dispatch_work_fn);
	throtl_service_queue_init(&td->service_queue);

	q->td = td;
	td->queue = q;

	/* activate policy */
	ret = blkcg_activate_policy(q, &blkcg_policy_throtl);
	if (ret)
		kfree(td);
	return ret;
}

void blk_throtl_exit(struct request_queue *q)
{
	BUG_ON(!q->td);
	throtl_shutdown_wq(q);
	blkcg_deactivate_policy(q, &blkcg_policy_throtl);
	kfree(q->td);
}

static int __init throtl_init(void)
{
	kthrotld_workqueue = alloc_workqueue("kthrotld", WQ_MEM_RECLAIM, 0);
	if (!kthrotld_workqueue)
		panic("Failed to create kthrotld\n");

	return blkcg_policy_register(&blkcg_policy_throtl);
}

module_init(throtl_init);
