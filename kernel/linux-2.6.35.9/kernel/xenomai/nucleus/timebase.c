/**
 * @file
 * @note Copyright (C) 2006,2007 Philippe Gerum <rpm@xenomai.org>.
 *
 * Xenomai is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Xenomai is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Xenomai; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * \ingroup timebase
 */

/*!
 * \ingroup nucleus
 * \defgroup timebase Time base services.
 *
 * Xenomai implements the notion of time base, by which software
 * timers that belong to different skins may be clocked separately
 * according to distinct frequencies, or aperiodically. In the
 * periodic case, delays and timeouts are given in counts of ticks;
 * the duration of a tick is specified by the time base. In the
 * aperiodic case, timings are directly specified in nanoseconds.
 *
 * Only a single aperiodic (i.e. tick-less) time base may exist in the
 * system, and the nucleus provides for it through the nktbase
 * object. All skins depending on aperiodic timings should bind to the
 * latter (see xntbase_alloc()), also known as the master time
 * base.
 *
 * Skins depending on periodic timings may create and bind to their
 * own time base. Such a periodic time base is managed as a timed
 * slave object of the master time base.  A cascading software timer
 * fired by the master time base according to the appropriate
 * frequency, triggers in turn the update process of the associated
 * timed slave, which eventually fires the elapsed software timers
 * controlled by the periodic time base. In other words, Xenomai
 * emulates periodic timing over an aperiodic policy.
 *
 * Xenomai always controls the underlying timer hardware in a
 * tick-less fashion, also known as the oneshot mode.
 *
 *@{*/

#include <nucleus/pod.h>
#include <nucleus/timer.h>
#include <nucleus/module.h>

DEFINE_XNQUEUE(nktimebaseq);

#ifdef CONFIG_XENO_OPT_TIMING_PERIODIC

/*!
 * \fn int xntbase_alloc(const char *name,u_long period,u_long flags,xntbase_t **basep)
 * \brief Allocate a time base.
 *
 * A time base is an abstraction used to provide private clocking
 * information to real-time skins, by which they may operate either in
 * aperiodic or periodic mode, possibly according to distinct clock
 * frequencies in the latter case. This abstraction is required in
 * order to support several RTOS emulators running concurrently, which
 * may exhibit different clocking policies and/or period.
 *
 * Once allocated, a time base may be attached to all software timers
 * created directly or indirectly by a given skin, and influences all
 * timed services accordingly.
 *
 * The xntbase_alloc() service allocates a new time base to the
 * caller, and returns the address of its descriptor. The new time
 * base is left in a disabled state (unless @a period equals
 * XN_APERIODIC_TICK), calling xntbase_start() is needed to enable it.
 *
 * @param name The symbolic name of the new time base. This
 * information is used to report status information when reading from
 * /proc/xenomai/timebases; it has currently no other usage.
 *
 * @param period The duration of the clock tick for the new time base,
 * given as a count of nanoseconds. The special @a XN_APERIODIC_TICK
 * value may be used to retrieve the master - aperiodic - time base,
 * which is always up and running when a real-time skin has called the
 * xnpod_init() service. All other values are meant to define the
 * clock rate of a periodic time base. For instance, passing 1000000
 * (ns) in the @a period parameter will create a periodic time base
 * clocked at a frequency of 1Khz.
 *
 * @param flags A bitmask composed as follows:
 *
 *        - XNTBISO causes the target timebase to be isolated from
 *        global wallclock offset updates as performed by
 *        xntbase_adjust_time().
 *
 * @param basep A pointer to a memory location which will be written
 * upon success with the address of the allocated time base. If @a
 * period equals XN_APERIODIC_TICK, the address of the built-in master
 * time base descriptor will be copied back to this location.
 *
 * @return 0 is returned on success. Otherwise:
 *
 * - -ENOMEM is returned if no system memory is available to allocate
 * a new time base descriptor.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization code
 * - User-space task in secondary mode
 *
 * Rescheduling: never.
 *
 * @note Any periodic time base allocated by a real-time skin must be
 * released by a call to xntbase_free() before the kernel module
 * implementing the skin may be unloaded.
 */

int xntbase_alloc(const char *name, u_long period, u_long flags,
		  xntbase_t **basep)
{
	xntslave_t *slave;
	xntbase_t *base;
	spl_t s;

	if (flags & ~XNTBISO)
		return -EINVAL;

	if (period == XN_APERIODIC_TICK) {
		*basep = &nktbase;
		xnarch_declare_tbase(&nktbase);
		return 0;
	}

	slave = (xntslave_t *)xnarch_alloc_host_mem(sizeof(*slave));

	if (!slave)
		return -ENOMEM;

	base = &slave->base;
	base->tickvalue = period;
	base->ticks2sec = 1000000000UL / period;
	base->wallclock_offset = 0;
	base->jiffies = 0;
	base->hook = NULL;
	base->ops = &nktimer_ops_periodic;
	base->name = name;
	inith(&base->link);
	xntslave_init(slave);

	/* Set initial status:
	   Not running, no time set, unlocked, isolated if requested. */
	base->status = flags;

	*basep = base;
#ifdef CONFIG_XENO_OPT_STATS
	initq(&base->timerq);
#endif /* CONFIG_XENO_OPT_TIMING_PERIODIC */
	xntbase_declare_proc(base);
	xnlock_get_irqsave(&nklock, s);
	appendq(&nktimebaseq, &base->link);
	xnlock_put_irqrestore(&nklock, s);

	xnarch_declare_tbase(base);

	return 0;
}
EXPORT_SYMBOL_GPL(xntbase_alloc);

/*!
 * \fn void xntbase_free(xntbase_t *base)
 * \brief Free a time base.
 *
 * This service disarms all outstanding timers from the affected
 * periodic time base, destroys the aperiodic cascading timer,
 * then releases the time base descriptor.
 *
 * @param base The address of the time base descriptor to release.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - User-space task in secondary mode
 *
 * Rescheduling: never.
 *
 * @note Requests to free the master time base are silently caught and
 * discarded; in such a case, outstanding aperiodic timers are left
 * untouched.
 */

void xntbase_free(xntbase_t *base)
{
	spl_t s;

	if (base == &nktbase)
		return;

	xntslave_destroy(base2slave(base));
	xntbase_remove_proc(base);

	xnlock_get_irqsave(&nklock, s);
	removeq(&nktimebaseq, &base->link);
	xnlock_put_irqrestore(&nklock, s);

	xnarch_free_host_mem(base, sizeof(*base));
}
EXPORT_SYMBOL_GPL(xntbase_free);

/*!
 * \fn int xntbase_update(xntbase_t *base, u_long period)
 * \brief Change the period of a time base.
 *
 * @param base The address of the time base descriptor to update.
 *
 * @param period The duration of the clock tick for the time base,
 * given as a count of nanoseconds. This value is meant to define the
 * new clock rate of the affected periodic time base (i.e. 1e9 /
 * period).
 *
 * @return 0 is returned on success. Otherwise:
 *
 * - -EINVAL is returned if an attempt is made to set a null
 * period.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization code
 * - Kernel-based task
 * - User-space task
 *
 * Rescheduling: never.
 *
 * @note Requests to update the master time base are silently caught
 * and discarded. The master time base has a fixed aperiodic policy
 * which may not be changed.
 */

int xntbase_update(xntbase_t *base, u_long period)
{
	spl_t s;

	if (base == &nktbase || base->tickvalue == period)
		return 0;

	if (period == XN_APERIODIC_TICK)
		return -EINVAL;

	xnlock_get_irqsave(&nklock, s);
	base->tickvalue = period;
	base->ticks2sec = 1000000000UL / period;
	xntslave_update(base2slave(base), period);
	xnlock_put_irqrestore(&nklock, s);

	return 0;
}
EXPORT_SYMBOL_GPL(xntbase_update);

/*!
 * \fn int xntbase_switch(const char *name,u_long period,xntbase_t **basep)
 * \brief Replace a time base.
 *
 * This service is useful for switching the current time base of a
 * real-time skin between aperiodic and periodic modes, by providing a
 * new time base descriptor as needed. The original time base
 * descriptor is freed as a result of this operation (unless it refers
 * to the master time base). The new time base is automatically
 * started by a call to xntbase_start() if the original time base was
 * enabled at the time of the call, or left in a disabled state
 * otherwise.
 *
 * This call handles all mode transitions and configuration changes
 * carefully, i.e. periodic <-> periodic, aperiodic <-> aperiodic,
 * periodic <-> aperiodic.
 *
 * @param name The symbolic name of the new time base. This
 * information is used to report status information when reading from
 * /proc/xenomai/timebases; it has currently no other usage.
 *
 * @param period The duration of the clock tick for the time base,
 * given as a count of nanoseconds. This value is meant to define the
 * new clock rate of the new periodic time base (i.e. 1e9 / period).
 *
 * @param basep A pointer to a memory location which will be first
 * read to pick the address of the original time base to be replaced,
 * then written back upon success with the address of the new time
 * base.  A null pointer is allowed on input in @a basep, in which
 * case the new time base will be created as if xntbase_alloc() had
 * been called directly.
 *
 * @return 0 is returned on success. Otherwise:
 *
 * - -ENOMEM is returned if no system memory is available to allocate
 * a new time base descriptor.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization code
 * - User-space task in secondary mode
 *
 * Rescheduling: never.
 */

int xntbase_switch(const char *name, u_long period, xntbase_t **basep)
{
	xntbase_t *oldbase = *basep, *newbase;
	int err = 0;

	if (!*basep)
		/* Switching from no time base to a valid one is ok,
		 we only need to assume that the old time base was the
		 master one. */
		oldbase = &nktbase;

	if (period == XN_APERIODIC_TICK) {
		if (xntbase_periodic_p(oldbase)) {
			/* The following call cannot fail. */
			xntbase_alloc(name, XN_APERIODIC_TICK, 0, basep);
			xntbase_free(oldbase);
		}
	} else {
		if (xntbase_periodic_p(oldbase))
			xntbase_update(oldbase, period);
		else {
			err = xntbase_alloc(name, period, 0, &newbase);
			if (!err) {
				int enabled = xntbase_enabled_p(oldbase);
				*basep = newbase;
				xntbase_free(oldbase);
				if (enabled)
					xntbase_start(newbase);
			}
		}
	}

	return err;
}
EXPORT_SYMBOL_GPL(xntbase_switch);

/*!
 * \fn void xntbase_start(xntbase_t *base)
 * \brief Start a time base.
 *
 * This service enables a time base, using a cascading timer running
 * in the master time base as the source of periodic clock
 * ticks. The time base is synchronised on the Xenomai system clock.
 * Timers attached to the started time base are immediated armed.
 *
 * @param base The address of the time base descriptor to start.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization code
 * - Kernel-based task
 * - User-space task
 *
 * Rescheduling: never.
 *
 * @note Requests to enable the master time base are silently caught
 * and discarded; only the internal service xnpod_enable_timesource()
 * is allowed to start the latter. The master time base remains
 * enabled until no real-time skin remains attached to the nucleus.
 */

void xntbase_start(xntbase_t *base)
{
	xnticks_t start_date;
	spl_t s;

	if (base == &nktbase || xntbase_enabled_p(base))
		return;

	trace_mark(xn_nucleus, tbase_start, "base %s", base->name);

	xnlock_get_irqsave(&nklock, s);

	start_date = xnarch_get_cpu_time();

	/* Only synchronise non-isolated time bases on the master base. */
	if (!xntbase_isolated_p(base)) {
		base->wallclock_offset = xntbase_ns2ticks(base,
			start_date + nktbase.wallclock_offset);
		__setbits(base->status, XNTBSET);
	}

	start_date += base->tickvalue;
	__setbits(base->status, XNTBRUN);

	xnlock_put_irqrestore(&nklock, s);

	xntslave_start(base2slave(base), start_date, base->tickvalue);
}
EXPORT_SYMBOL_GPL(xntbase_start);

/*!
 * \fn void xntbase_stop(xntbase_t *base)
 * \brief Stop a time base.
 *
 * This service disables a time base, stopping the cascading timer
 * running in the master time base which is used to clock it.
 * Outstanding timers attached to the stopped time base are immediated
 * disarmed.
 *
 * Stopping a time base also invalidates its clock setting.
 *
 * @param base The address of the time base descriptor to stop.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization code
 * - Kernel-based task
 * - User-space task
 *
 * @note Requests to disable the master time base are silently caught
 * and discarded; only the internal service xnpod_disable_timesource()
 * is allowed to stop the latter. The master time base remains enabled
 * until no real-time skin remains attached to the nucleus.
 */

void xntbase_stop(xntbase_t *base)
{
	if (base == &nktbase || !xntbase_enabled_p(base))
		return;

	xntslave_stop(base2slave(base));
	__clrbits(base->status, XNTBRUN | XNTBSET);

	trace_mark(xn_nucleus, tbase_stop, "base %s", base->name);
}
EXPORT_SYMBOL_GPL(xntbase_stop);

/*!
 * \fn void xntbase_tick(xntbase_t *base)
 * \brief Announce a clock tick to a time base.
 *
 * This service announces a new clock tick to a time base. Normally,
 * only specialized nucleus code would announce clock ticks. However,
 * under certain circumstances, it may be useful to allow client code
 * to send such notifications on their own.
 *
 * Notifying a clock tick to a time base causes the timer management
 * code to check for outstanding timers, which may in turn fire off
 * elapsed timeout handlers. Additionally, periodic time bases
 * (i.e. all but the master time base) would also update their count
 * of elapsed jiffies, in case the current processor has been defined
 * as the internal time keeper (i.e. CPU# == XNTIMER_KEEPER_ID).
 *
 * @param base The address of the time base descriptor to announce a
 * tick to.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Interrupt context only.
 *
 * Rescheduling: never.
 */

void xntbase_tick(xntbase_t *base)
{
	spl_t s;

	xnlock_get_irqsave(&nklock, s);

	trace_mark(xn_nucleus, tbase_tick, "base %s", base->name);

	if (base == &nktbase)
		xntimer_tick_aperiodic();
	else {
		xntslave_t *slave = base2slave(base);
		xntimer_tick_periodic_inner(slave);
	}

	xnlock_put_irqrestore(&nklock, s);
}
EXPORT_SYMBOL_GPL(xntbase_tick);

xnticks_t xntbase_ns2ticks_ceil(xntbase_t *base, xntime_t t)
{
	return xnarch_ulldiv(t + xntbase_get_tickval(base) - 1,
			     xntbase_get_tickval(base), NULL);
}
EXPORT_SYMBOL_GPL(xntbase_ns2ticks_ceil);

/*!
 * \fn xnticks_t xntbase_convert(xntbase_t *srcbase,xnticks_t ticks,xntbase_t *dstbase)
 * \brief Convert a clock value into another time base.
 *
 * @param srcbase The descriptor address of the source time base.

 * @param ticks The clock value expressed in the source time base to
 * convert to the destination time base.
 *
 * @param dstbase The descriptor address of the destination time base.

 * @return The converted count of ticks in the destination time base
 * is returned.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization code
 * - Kernel-based task
 * - User-space task
 *
 * Rescheduling: never.
 */

xnticks_t xntbase_convert(xntbase_t *srcbase, xnticks_t ticks, xntbase_t *dstbase)
{
	/* Twisted, but tries hard not to rescale to nanoseconds *
	   before converting, so that we could save a 64bit multiply
	   in the common cases (i.e. converting to/from master). */

	if (dstbase->tickvalue == srcbase->tickvalue)
		return ticks;

	if (likely(xntbase_master_p(dstbase)))
		return xntbase_ticks2ns(srcbase, ticks); /* Periodic to master base. */

	if (xntbase_master_p(srcbase))
		return xntbase_ns2ticks(dstbase, ticks); /* Master base to periodic. */

	/* Periodic to periodic. */

	return xntbase_ns2ticks(dstbase, xntbase_ticks2ns(srcbase, ticks));
}
EXPORT_SYMBOL_GPL(xntbase_convert);

#endif /* CONFIG_XENO_OPT_TIMING_PERIODIC */

/*!
 * \fn void xntbase_adjust_time(xntbase_t *base, xnsticks_t delta)
 * \brief Adjust the clock time for the system.
 *
 * Xenomai tracks the current time as a monotonously increasing count
 * of ticks since the epoch. The epoch is initially the same as the
 * underlying machine time, and it is always synchronised across all
 * active time bases.
 *
 * This service changes the epoch for the system by applying the
 * specified tick delta on the master's wallclock offset and
 * resynchronizing all other time bases.
 *
 * @param base The address of the initiating time base.
 *
 * @param delta The adjustment of the system time expressed in ticks
 * of the specified time base.
 *
 * @note This routine must be entered nklock locked, interrupts off.
 *
 * Environments:
 *
 * This service can be called from:
 *
 * - Kernel module initialization/cleanup code
 * - Interrupt service routine
 * - Kernel-based task
 * - User-space task
 *
 * Rescheduling: never.
 */

void xntbase_adjust_time(xntbase_t *base, xnsticks_t delta)
{
	xnticks_t now;

#ifdef CONFIG_XENO_OPT_TIMING_PERIODIC
	if (xntbase_isolated_p(base)) {
		/* Only update the specified isolated base. */
		base->wallclock_offset += delta;
		__setbits(base->status, XNTBSET);
		xntslave_adjust(base2slave(base), delta);

	} else {
		xnholder_t *holder;
#endif /* CONFIG_XENO_OPT_TIMING_PERIODIC */
		/* Update all non-isolated bases in the system. */
		nktbase.wallclock_offset += xntbase_ticks2ns(base, delta);
		now = xnarch_get_cpu_time() + nktbase.wallclock_offset;
		xntimer_adjust_all_aperiodic(xntbase_ticks2ns(base, delta));

#ifdef CONFIG_XENO_OPT_TIMING_PERIODIC
		for (holder = getheadq(&nktimebaseq);
		     holder != NULL; holder = nextq(&nktimebaseq, holder)) {
			xntbase_t *tbase = link2tbase(holder);
			if (tbase == &nktbase || xntbase_isolated_p(tbase))
				continue;

			tbase->wallclock_offset =
				xntbase_ns2ticks(tbase, now) -
				xntbase_get_jiffies(tbase);
			xntslave_adjust(base2slave(tbase), delta);
		}
	}
#endif /* CONFIG_XENO_OPT_TIMING_PERIODIC */

	trace_mark(xn_nucleus, tbase_adjust, "base %s delta %Lu",
		   base->name, delta);
}
EXPORT_SYMBOL_GPL(xntbase_adjust_time);

#ifdef CONFIG_PROC_FS

#include <linux/proc_fs.h>

#ifdef CONFIG_XENO_OPT_STATS

#include <linux/seq_file.h>

static struct proc_dir_entry *tmstat_proc_root;

struct tmstat_seq_iterator {
	int nentries;
	struct tmstat_seq_info {
		int cpu;
		unsigned int scheduled;
		unsigned int fired;
		xnticks_t timeout;
		xnticks_t interval;
		xnflags_t status;
		char handler[12];
		char name[XNOBJECT_NAME_LEN];
	} stat_info[1];
};

static void *tmstat_seq_start(struct seq_file *seq, loff_t *pos)
{
	struct tmstat_seq_iterator *iter = seq->private;

	if (*pos > iter->nentries)
		return NULL;

	if (*pos == 0)
		return SEQ_START_TOKEN;

	return iter->stat_info + *pos - 1;
}

static void *tmstat_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	struct tmstat_seq_iterator *iter = seq->private;

	++*pos;

	if (*pos > iter->nentries)
		return NULL;

	return iter->stat_info + *pos - 1;
}

static int tmstat_seq_show(struct seq_file *seq, void *v)
{
	if (v == SEQ_START_TOKEN)
		seq_printf(seq,
			   "%-3s  %-10s  %-10s  %-10s  %-10s  %-11s  %-15s\n",
			   "CPU", "SCHEDULED", "FIRED", "TIMEOUT",
			   "INTERVAL", "HANDLER", "NAME");
	else {
		struct tmstat_seq_info *p = v;
		char timeout_buf[21]  = "-         ";
		char interval_buf[21] = "-         ";

		if (!testbits(p->status, XNTIMER_DEQUEUED))
			snprintf(timeout_buf, sizeof(timeout_buf), "%-10llu",
				 p->timeout);
		if (testbits(p->status, XNTIMER_PERIODIC))
			snprintf(interval_buf, sizeof(interval_buf), "%-10llu",
				 p->interval);
		seq_printf(seq,
			   "%-3u  %-10u  %-10u  %s  %s  %-11s  %-15s\n",
			   p->cpu, p->scheduled, p->fired, timeout_buf,
			   interval_buf, p->handler, p->name);
	}

	return 0;
}

static void tmstat_seq_stop(struct seq_file *seq, void *v)
{
}

static struct seq_operations tmstat_op = {
	.start = &tmstat_seq_start,
	.next = &tmstat_seq_next,
	.stop = &tmstat_seq_stop,
	.show = &tmstat_seq_show
};

static int tmstat_seq_open(struct inode *inode, struct file *file)
{
	xntbase_t *base = PDE(inode)->data;
	struct tmstat_seq_iterator *iter = NULL;
	struct seq_file *seq;
	xnholder_t *holder;
	struct tmstat_seq_info *stat_info;
	int err, count, tmq_rev;
	spl_t s;

	if (!xnpod_active_p())
		return -ESRCH;

	xnlock_get_irqsave(&nklock, s);

      restart:
	count = countq(&base->timerq);
	holder = getheadq(&base->timerq);
	tmq_rev = base->timerq_rev;

	xnlock_put_irqrestore(&nklock, s);

	if (iter)
		kfree(iter);
	iter = kmalloc(sizeof(*iter)
		       + (count - 1) * sizeof(struct tmstat_seq_info),
		       GFP_KERNEL);
	if (!iter)
		return -ENOMEM;

	err = seq_open(file, &tmstat_op);

	if (err) {
		kfree(iter);
		return err;
	}

	iter->nentries = 0;

	/* Take a snapshot element-wise, restart if something changes
	   underneath us. */

	while (holder) {
		xntimer_t *timer;

		xnlock_get_irqsave(&nklock, s);

		if (base->timerq_rev != tmq_rev)
			goto restart;

		timer = tblink2timer(holder);
		/* Skip inactive timers */
		if (xnstat_counter_get(&timer->scheduled) == 0)
			goto skip;

		stat_info = &iter->stat_info[iter->nentries++];

		stat_info->cpu = xnsched_cpu(xntimer_sched(timer));
		stat_info->scheduled = xnstat_counter_get(&timer->scheduled);
		stat_info->fired = xnstat_counter_get(&timer->fired);
		stat_info->timeout = xntimer_get_timeout(timer);
		stat_info->interval = xntimer_get_interval(timer);
		stat_info->status = timer->status;
		memcpy(stat_info->handler, timer->handler_name,
		       sizeof(stat_info->handler)-1);
		stat_info->handler[sizeof(stat_info->handler)-1] = 0;
		xnobject_copy_name(stat_info->name, timer->name);

	      skip:
		holder = nextq(&base->timerq, holder);

		xnlock_put_irqrestore(&nklock, s);
	}

	seq = file->private_data;
	seq->private = iter;

	return 0;
}

static struct file_operations tmstat_seq_operations = {
	.owner = THIS_MODULE,
	.open = tmstat_seq_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release_private,
};

void xntbase_declare_proc(xntbase_t *base)
{
	struct proc_dir_entry *entry;

	entry = rthal_add_proc_seq(base->name, &tmstat_seq_operations, 0,
				   tmstat_proc_root);
	if (entry)
		entry->data = base;
}

void xntbase_remove_proc(xntbase_t *base)
{
	remove_proc_entry(base->name, tmstat_proc_root);
}

#endif /* CONFIG_XENO_OPT_STATS */

static int timebase_read_proc(char *page,
			      char **start,
			      off_t off, int count, int *eof, void *data)
{
	xnholder_t *holder;
	xntbase_t *tbase;
	char *p = page;
	int len = 0;

	p += sprintf(p, "%-10s %10s  %10s   %s\n",
		     "NAME", "RESOLUTION", "JIFFIES", "STATUS");

	for (holder = getheadq(&nktimebaseq);
	     holder != NULL; holder = nextq(&nktimebaseq, holder)) {
		tbase = link2tbase(holder);
		if (xntbase_periodic_p(tbase))
			p += sprintf(p, "%-10s %10lu  %10Lu   %s%s%s\n",
				     tbase->name,
				     tbase->tickvalue,
				     tbase->jiffies,
				     xntbase_enabled_p(tbase) ? "enabled" : "disabled",
				     xntbase_timeset_p(tbase) ? ",set" : ",unset",
				     xntbase_isolated_p(tbase) ? ",isolated" : "");
		else
			p += sprintf(p, "%-10s %10s  %10s   %s\n",
				     tbase->name,
				     "1",
				     "n/a",
				     "enabled,set");
	}

	len = p - page - off;
	if (len <= off + count)
		*eof = 1;
	*start = page + off;
	if (len > count)
		len = count;
	if (len < 0)
		len = 0;

	return len;
}

void xntbase_init_proc(void)
{
#ifdef CONFIG_XENO_OPT_STATS
	tmstat_proc_root =
		create_proc_entry("timerstat", S_IFDIR, rthal_proc_root);
#endif /* CONFIG_XENO_OPT_STATS */
	rthal_add_proc_leaf("timebases", &timebase_read_proc, NULL, NULL,
			    rthal_proc_root);
}

void xntbase_cleanup_proc(void)
{
	remove_proc_entry("timebases", rthal_proc_root);
#ifdef CONFIG_XENO_OPT_STATS
	/* All timebases must have been deregistered now. */
	XENO_ASSERT(NUCLEUS, !getheadq(&nktimebaseq), ;);
	remove_proc_entry("timerstat", rthal_proc_root);
#endif /* CONFIG_XENO_OPT_STATS */
}

#endif /* CONFIG_PROC_FS */

/* The master time base - the most precise one, aperiodic, always valid. */

xntbase_t nktbase = {
	.ops = &nktimer_ops_aperiodic,
	.jiffies = 0,	/* Unused. */
	.hook = NULL,
	.wallclock_offset = 0,
	.tickvalue = 1,
	.ticks2sec = 1000000000UL,
	.status = XNTBRUN|XNTBSET,
	.name = "master",
#ifdef CONFIG_XENO_OPT_STATS
	.timerq = XNQUEUE_INITIALIZER(nktbase.timerq),
#endif /* CONFIG_XENO_OPT_STATS */
};
EXPORT_SYMBOL_GPL(nktbase);

/*@}*/
