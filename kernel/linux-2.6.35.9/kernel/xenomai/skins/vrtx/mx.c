/*
 * Copyright (C) 2001,2002 IDEALX (http://www.idealx.com/).
 * Written by Julien Pinon <jpinon@idealx.com>.
 * Copyright (C) 2003,2006 Philippe Gerum <rpm@xenomai.org>.
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
 * along with Xenomai; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <vrtx/task.h>
#include <vrtx/mx.h>

static xnmap_t *vrtx_mx_idmap;

static xnqueue_t vrtx_mx_q;

#ifdef CONFIG_PROC_FS

static int __mutex_read_proc(char *page,
			     char **start,
			     off_t off, int count, int *eof, void *data)
{
	vrtxmx_t *mx = (vrtxmx_t *)data;
	xnthread_t *owner;
	char *p = page;
	int len;
	spl_t s;

	xnlock_get_irqsave(&nklock, s);

	owner = xnsynch_owner(&mx->synchbase);
	if (owner) {
		xnpholder_t *holder;

		/* Locked mx -- dump owner and waiters, if any. */

		p += sprintf(p, "=locked by %s\n",
			     xnthread_name(owner));

		holder = getheadpq(xnsynch_wait_queue(&mx->synchbase));

		while (holder) {
			xnthread_t *sleeper = link2thread(holder, plink);
			p += sprintf(p, "+%s\n", xnthread_name(sleeper));
			holder =
			    nextpq(xnsynch_wait_queue(&mx->synchbase),
				   holder);
		}
	} else
		/* Mutex unlocked. */
		p += sprintf(p, "=unlocked\n");

	xnlock_put_irqrestore(&nklock, s);

	len = (p - page) - off;
	if (len <= off + count)
		*eof = 1;
	*start = page + off;
	if (len > count)
		len = count;
	if (len < 0)
		len = 0;

	return len;
}

extern xnptree_t __vrtx_ptree;

static xnpnode_t __mutex_pnode = {

	.dir = NULL,
	.type = "mutexes",
	.entries = 0,
	.read_proc = &__mutex_read_proc,
	.write_proc = NULL,
	.root = &__vrtx_ptree,
};

#else /* !CONFIG_PROC_FS */

static xnpnode_t __mutex_pnode = {

	.type = "mutexes"
};

#endif /* !CONFIG_PROC_FS */

int mx_destroy_internal(vrtxmx_t *mx)
{
	int s = xnsynch_destroy(&mx->synchbase);
	xnmap_remove(vrtx_mx_idmap, mx->mid);
	removeq(&vrtx_mx_q, &mx->link);
	xnregistry_remove(mx->handle);
	xnfree(mx);
	return s;
}

int vrtxmx_init(void)
{
	initq(&vrtx_mx_q);
	vrtx_mx_idmap = xnmap_create(VRTX_MAX_MUTEXES, 0, 0);
	return vrtx_mx_idmap ? 0 : -ENOMEM;
}

void vrtxmx_cleanup(void)
{
	xnholder_t *holder;

	while ((holder = getheadq(&vrtx_mx_q)) != NULL)
		mx_destroy_internal(link2vrtxmx(holder));

	xnmap_delete(vrtx_mx_idmap);
}

int sc_mcreate(unsigned int opt, int *errp)
{
	int bflags, mid;
	vrtxmx_t *mx;
	spl_t s;

	switch (opt) {
	case 0:
		bflags = XNSYNCH_PRIO;
		break;
	case 1:
		bflags = XNSYNCH_FIFO;
		break;
	case 2:
		bflags = XNSYNCH_PRIO | XNSYNCH_PIP;
		break;
	default:
		*errp = ER_IIP;
		return 0;
	}

	mx = xnmalloc(sizeof(*mx));
	if (mx == NULL) {
		*errp = ER_NOCB;
		return -1;
	}

	mid = xnmap_enter(vrtx_mx_idmap, -1, mx);
	if (mid < 0) {
		xnfree(mx);
		return -1;
	}

	inith(&mx->link);
	mx->mid = mid;
	xnsynch_init(&mx->synchbase, bflags | XNSYNCH_DREORD | XNSYNCH_OWNER,
		     NULL);

	xnlock_get_irqsave(&nklock, s);
	appendq(&vrtx_mx_q, &mx->link);
	xnlock_put_irqrestore(&nklock, s);

	sprintf(mx->name, "mx%d", mid);
	xnregistry_enter(mx->name, mx, &mx->handle, &__mutex_pnode);

	*errp = RET_OK;

	return mid;
}

void sc_mpost(int mid, int *errp)
{
	xnthread_t *cur = xnpod_current_thread();
	vrtxmx_t *mx;
	spl_t s;

	xnlock_get_irqsave(&nklock, s);

	mx = xnmap_fetch(vrtx_mx_idmap, mid);
	/* Return ER_ID if the poster does not own the mutex. */
	if (mx == NULL || xnsynch_owner(&mx->synchbase) != cur) {
		*errp = ER_ID;
		goto unlock_and_exit;
	}

	*errp = RET_OK;

	if (xnsynch_release(&mx->synchbase))
		xnpod_schedule();

      unlock_and_exit:

	xnlock_put_irqrestore(&nklock, s);
}

void sc_mdelete(int mid, int opt, int *errp)
{
	xnthread_t *owner;
	vrtxmx_t *mx;
	spl_t s;

	if (opt & ~1) {
		*errp = ER_IIP;
		return;
	}

	xnlock_get_irqsave(&nklock, s);

	mx = xnmap_fetch(vrtx_mx_idmap, mid);
	if (mx == NULL) {
		*errp = ER_ID;
		goto unlock_and_exit;
	}

	owner = xnsynch_owner(&mx->synchbase);
	if (owner && (opt == 0 || xnpod_current_thread() != owner)) {
		*errp = ER_PND;
		goto unlock_and_exit;
	}

	*errp = RET_OK;

	if (mx_destroy_internal(mx) == XNSYNCH_RESCHED)
		xnpod_schedule();

unlock_and_exit:

	xnlock_put_irqrestore(&nklock, s);
}

void sc_mpend(int mid, unsigned long timeout, int *errp)
{
	xnthread_t *cur = xnpod_current_thread();
	vrtxtask_t *task;
	vrtxmx_t *mx;
	spl_t s;

	xnlock_get_irqsave(&nklock, s);

	if (xnpod_unblockable_p()) {
		*errp = -EPERM;
		goto unlock_and_exit;
	}

	mx = xnmap_fetch(vrtx_mx_idmap, mid);
	if (mx == NULL) {
		*errp = ER_ID;
		goto unlock_and_exit;
	}

	*errp = RET_OK;

	if (xnsynch_owner(&mx->synchbase) == NULL) {
		xnsynch_set_owner(&mx->synchbase, cur);
		goto unlock_and_exit;
	}

	if (xnsynch_owner(&mx->synchbase) == cur)
		goto unlock_and_exit;

	task = thread2vrtxtask(cur);
	task->vrtxtcb.TCBSTAT = TBSMUTEX;

	if (timeout)
		task->vrtxtcb.TCBSTAT |= TBSDELAY;

	xnsynch_acquire(&mx->synchbase, timeout, XN_RELATIVE);

	if (xnthread_test_info(cur, XNBREAK))
		*errp = -EINTR;
	else if (xnthread_test_info(cur, XNRMID))
		*errp = ER_DEL;	/* Mutex deleted while pending. */
	else if (xnthread_test_info(cur, XNTIMEO))
		*errp = ER_TMO;	/* Timeout. */

      unlock_and_exit:

	xnlock_put_irqrestore(&nklock, s);
}

void sc_maccept(int mid, int *errp)
{
	vrtxmx_t *mx;
	spl_t s;

	xnlock_get_irqsave(&nklock, s);

	if (xnpod_unblockable_p()) {
		*errp = -EPERM;
		goto unlock_and_exit;
	}

	mx = xnmap_fetch(vrtx_mx_idmap, mid);
	if (mx == NULL) {
		*errp = ER_ID;
		goto unlock_and_exit;
	}

	if (xnsynch_owner(&mx->synchbase) == NULL) {
		xnsynch_set_owner(&mx->synchbase, xnpod_current_thread());
		*errp = RET_OK;
	} else
		*errp = ER_PND;

unlock_and_exit:

	xnlock_put_irqrestore(&nklock, s);
}

int sc_minquiry(int mid, int *errp)
{
	vrtxmx_t *mx;
	spl_t s;
	int rc;

	xnlock_get_irqsave(&nklock, s);

	mx = xnmap_fetch(vrtx_mx_idmap, mid);
	if (mx == NULL) {
		rc = 0;
		*errp = ER_ID;
		goto unlock_and_exit;
	}

	rc = xnsynch_owner(&mx->synchbase) == NULL;

unlock_and_exit:

	xnlock_put_irqrestore(&nklock, s);

	return rc;
}
