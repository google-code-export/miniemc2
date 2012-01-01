/*
 * arch/arm/kernel/fcse.c
 *
 * Helper functions for using the ARM Fast Context Switch Extension with
 * processors supporting it.
 *
 * Copyright (C) 2008 Richard Cochran
 * Copyright (C) 2009-2011 Gilles Chanteperdrix <gch@xenomai.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/bitops.h>
#include <linux/memory.h>
#include <linux/spinlock.h>
#include <linux/mm.h>
#include <linux/kernel_stat.h>
#include <linux/mman.h>
#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/hardirq.h>

#include <asm/fcse.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>

#define NR_PIDS (TASK_SIZE / FCSE_PID_TASK_SIZE)
#define PIDS_LONGS ((NR_PIDS + BITS_PER_LONG - 1) / BITS_PER_LONG)

static IPIPE_DEFINE_SPINLOCK(fcse_lock);
static unsigned long fcse_pids_bits[PIDS_LONGS];
unsigned long fcse_pids_cache_dirty[PIDS_LONGS];
EXPORT_SYMBOL(fcse_pids_cache_dirty);

#ifdef CONFIG_ARM_FCSE_BEST_EFFORT
static unsigned random_pid;
struct fcse_user fcse_pids_user[NR_PIDS];
#endif /* CONFIG_ARM_FCSE_BEST_EFFORT */

static inline void fcse_pid_reference_inner(unsigned pid)
{
#ifdef CONFIG_ARM_FCSE_BEST_EFFORT
	if (++fcse_pids_user[pid].count == 1)
#endif /* CONFIG_ARM_FCSE_BEST_EFFORT */
		__set_bit(pid, fcse_pids_bits);
}

static inline void fcse_pid_dereference(struct mm_struct *mm)
{
	unsigned fcse_pid = mm->context.fcse.pid >> FCSE_PID_SHIFT;

#ifdef CONFIG_ARM_FCSE_BEST_EFFORT
	if (--fcse_pids_user[fcse_pid].count == 0)
		__clear_bit(fcse_pid, fcse_pids_bits);

	/*
	 * The following means we suppose that by the time this
	 * function is called, this mm is out of cache:
	 * - when the caller is destroy_context, exit_mmap is called
	 * by mmput before, which flushes the cache;
	 * - when the caller is fcse_relocate_mm_to_pid from
	 * fcse_switch_mm_inner, we only relocate when the mm is out
	 * of cache;
	 * - when the caller is fcse_relocate_mm_to_pid from
	 * fcse_relocate_mm_to_null_pid, we flush the cache in this
	 * function.
	 */
	if (fcse_pids_user[fcse_pid].mm == mm) {
		fcse_pids_user[fcse_pid].mm = NULL;
		__clear_bit(fcse_pid, fcse_pids_cache_dirty);
	}
#else /* CONFIG_ARM_FCSE_BEST_EFFORT */
	__clear_bit(fcse_pid, fcse_pids_bits);
	__clear_bit(fcse_pid, fcse_pids_cache_dirty);
#endif /* CONFIG_ARM_FCSE_BEST_EFFORT */
}

static inline unsigned find_free_pid(unsigned long bits[])
{
	unsigned fcse_pid;

	fcse_pid = find_next_zero_bit(bits, NR_PIDS, 1);
	if (fcse_pid == NR_PIDS)
		/* Allocate zero pid last, since zero pid is also used by
		   processes with address space larger than 32MB in
		   best-effort mode. */
		if (!test_bit(0, bits))
			fcse_pid = 0;

	return fcse_pid;
}

void fcse_pid_free(struct mm_struct *mm)
{
	unsigned long flags;

	spin_lock_irqsave(&fcse_lock, flags);
	fcse_pid_dereference(mm);
	spin_unlock_irqrestore(&fcse_lock, flags);
}

int fcse_pid_alloc(struct mm_struct *mm)
{
	unsigned long flags;
	unsigned fcse_pid;

	spin_lock_irqsave(&fcse_lock, flags);
	fcse_pid = find_free_pid(fcse_pids_bits);
	if (fcse_pid == NR_PIDS) {
		/* Allocate zero pid last, since zero pid is also used by
		   processes with address space larger than 32MB in
		   best-effort mode. */
#ifdef CONFIG_ARM_FCSE_BEST_EFFORT
		if(++random_pid == NR_PIDS)
			random_pid = 0;
		fcse_pid = random_pid;
#else /* CONFIG_ARM_FCSE_GUARANTEED */
		spin_unlock_irqrestore(&fcse_lock, flags);
#ifdef CONFIG_ARM_FCSE_MESSAGES
		printk(KERN_WARNING "FCSE: %s[%d] would exceed the %lu processes limit.\n",
		       current->comm, current->pid, NR_PIDS);
#endif /* CONFIG_ARM_FCSE_MESSAGES */
		return -EAGAIN;
#endif /* CONFIG_ARM_FCSE_GUARANTEED */
	}
	fcse_pid_reference_inner(fcse_pid);
	spin_unlock_irqrestore(&fcse_lock, flags);

	return fcse_pid;
}

static inline void fcse_clear_dirty_all(void)
{
	switch(ARRAY_SIZE(fcse_pids_cache_dirty)) {
	case 4:
		fcse_pids_cache_dirty[3] = 0UL;
	case 3:
		fcse_pids_cache_dirty[2] = 0UL;
	case 2:
		fcse_pids_cache_dirty[1] = 0UL;
	case 1:
		fcse_pids_cache_dirty[0] = 0UL;
	}
}

unsigned fcse_flush_all_start(void)
{
	if (!cache_is_vivt())
		return 0;

#ifndef CONFIG_ARM_FCSE_PREEMPT_FLUSH
	preempt_disable();
#endif /* CONFIG_ARM_FCSE_PREEMPT_FLUSH */

#if defined(CONFIG_IPIPE)
	clear_ti_thread_flag(current_thread_info(), TIF_SWITCHED);
#elif defined(CONFIG_ARM_FCSE_PREEMPT_FLUSH)
	return nr_context_switches();
#endif /* CONFIG_ARM_FCSE_PREEMPT_FLUSH */

	return 0;
}

noinline void
fcse_flush_all_done(unsigned seq, unsigned dirty)
{
	unsigned long flags;

	if (!cache_is_vivt())
		return;

	spin_lock_irqsave(&fcse_lock, flags);
#if defined(CONFIG_IPIPE)
	if (!test_ti_thread_flag(current_thread_info(), TIF_SWITCHED))
#elif defined(CONFIG_ARM_FCSE_PREEMPT_FLUSH)
	if (seq == nr_context_switches())
#endif /* CONFIG_ARM_FCSE_PREEMPT_FLUSH */
		fcse_clear_dirty_all();

	if (dirty && current->mm != &init_mm && current->mm) {
		unsigned fcse_pid =
			current->mm->context.fcse.pid >> FCSE_PID_SHIFT;
		__set_bit(fcse_pid, fcse_pids_cache_dirty);
	}
	spin_unlock_irqrestore(&fcse_lock, flags);
#ifndef CONFIG_ARM_FCSE_PREEMPT_FLUSH
	preempt_enable();
#endif /* CONFIG_ARM_FCSE_PREEMPT_FLUSH */
}

#ifdef CONFIG_ARM_FCSE_BEST_EFFORT
/* Called with preemption disabled, mm->mmap_sem being held for writing. */
static noinline int fcse_relocate_mm_to_pid(struct mm_struct *mm, int fcse_pid)
{
	const unsigned len = pgd_index(FCSE_TASK_SIZE) * sizeof(pgd_t);
	unsigned long flags;
	pgd_t *from, *to;

	spin_lock_irqsave(&fcse_lock, flags);
#if defined(CONFIG_ARM_FCSE_DYNPID)
	/* pid == -1 means find a free pid. */
	if (fcse_pid == -1) {
		fcse_pid = find_free_pid(fcse_pids_bits);
		if (fcse_pid == NR_PIDS) {
			fcse_pid = find_free_pid(fcse_pids_cache_dirty);
			if (unlikely(fcse_pid == NR_PIDS)) {
				spin_unlock_irqrestore(&fcse_lock, flags);
				return -ENOENT;
			}
		}
	}
#endif /* CONFIG_ARM_FCSE_DYNPID */
	fcse_pid_dereference(mm);
	fcse_pid_reference_inner(fcse_pid);
	fcse_pids_user[fcse_pid].mm = mm;
	__set_bit(fcse_pid, fcse_pids_cache_dirty);
	spin_unlock_irqrestore(&fcse_lock, flags);

	from = pgd_offset(mm, 0);
	mm->context.fcse.pid = fcse_pid << FCSE_PID_SHIFT;
	to = pgd_offset(mm, 0);

	memcpy(to, from, len);
	memset(from, '\0', len);
	barrier();
	clean_dcache_area(from, len);
	clean_dcache_area(to, len);

	return fcse_pid;
}

int fcse_switch_mm_inner(struct mm_struct *prev, struct mm_struct *next)
{
	unsigned fcse_pid = next->context.fcse.pid >> FCSE_PID_SHIFT;
	unsigned flush_needed, reused_pid = 0;
	unsigned long flags;

	if (unlikely(next == &init_mm)) {
		spin_lock_irqsave(&fcse_lock, flags);
		goto is_flush_needed;
	}

#ifdef CONFIG_ARM_FCSE_DYNPID
	/*
	 * If the next mm's pid is currently in use, and not by that
	 * mm, try and find a new, free, pid.
	 */
	if (unlikely(fcse_pids_user[fcse_pid].mm != next)
	    && test_bit(fcse_pid, fcse_pids_cache_dirty)
	    && fcse_pids_user[fcse_pid].mm
	    && !rwsem_is_locked(&next->mmap_sem)
	    && !next->context.fcse.large
	    && !next->core_state) {
		int new_fcse_pid = fcse_relocate_mm_to_pid(next, -1);
		if (new_fcse_pid >= 0)
			fcse_pid = new_fcse_pid;
	}
#endif /* CONFIG_ARM_FCSE_DYNPID */

	spin_lock_irqsave(&fcse_lock, flags);
	if (fcse_pids_user[fcse_pid].mm != next) {
		if (fcse_pids_user[fcse_pid].mm)
			reused_pid = test_bit(fcse_pid, fcse_pids_cache_dirty);
		fcse_pids_user[fcse_pid].mm = next;
	}

  is_flush_needed:
	flush_needed = reused_pid
		|| next->context.fcse.high_pages
		|| !prev
		|| prev->context.fcse.shared_dirty_pages
		|| prev->context.fcse.high_pages;

	fcse_pid_set(fcse_pid << FCSE_PID_SHIFT);
	if (flush_needed)
		fcse_clear_dirty_all();
	if (next != &init_mm)
		__set_bit(fcse_pid, fcse_pids_cache_dirty);
	spin_unlock_irqrestore(&fcse_lock, flags);

	return flush_needed;
}
EXPORT_SYMBOL_GPL(fcse_switch_mm_inner);

void fcse_pid_reference(unsigned fcse_pid)
{
	unsigned long flags;

	spin_lock_irqsave(&fcse_lock, flags);
	fcse_pid_reference_inner(fcse_pid);
	spin_unlock_irqrestore(&fcse_lock, flags);
}

/* Called with mm->mmap_sem write-locked. */
static noinline void fcse_relocate_mm_to_null_pid(struct mm_struct *mm)
{
	if (!cache_is_vivt())
		return;

	preempt_disable();
	while (fcse_mm_in_cache(mm)) {
		unsigned seq;

		preempt_enable();

		seq = fcse_flush_all_start();
		flush_cache_all();

		preempt_disable();
		fcse_flush_all_done(seq, 0);
	}

	fcse_relocate_mm_to_pid(mm, 0);
	barrier();
	flush_tlb_mm(mm);
	fcse_pid_set(0);

	preempt_enable();
}
#endif /* CONFIG_ARM_FCSE_BEST_EFFORT */

unsigned long
fcse_check_mmap_inner(struct mm_struct *mm, unsigned long start_addr,
		      unsigned long addr, unsigned long len, unsigned long fl)
{
#ifdef CONFIG_ARM_FCSE_BEST_EFFORT
	unsigned long stack_reserved =
		current->signal->rlim[RLIMIT_STACK].rlim_cur;
	unsigned long stack_base = PAGE_ALIGN(mm->start_stack) - stack_reserved;

	/* We enfore the RLIMIT_STACK stack size, and here, the return
	   address would fall in that reserved stack area */
	if ((unsigned long)(addr + len - stack_base) < stack_reserved) {
		/* Restart to try and find a hole, once. */
		if (start_addr != TASK_UNMAPPED_BASE && !(fl & MAP_FIXED)
		    && !mm->context.fcse.large)
			return TASK_UNMAPPED_BASE;

		/* Forcibly restart from above the stack */
		if (!(fl & MAP_FIXED))
			return PAGE_ALIGN(mm->start_stack);

		/* If MAP_FIXED is set, we encroach upon the reserved
		   stack area. No choice. */
	}

	/* Address above 32MB */
	if (addr + len > FCSE_TASK_SIZE && !mm->context.fcse.high_pages) {
		/* Restart to try and find a hole, once. */
		if (start_addr != TASK_UNMAPPED_BASE && !(fl & MAP_FIXED))
			return TASK_UNMAPPED_BASE;

		if (!mm->context.fcse.large) {
			/* Ok, the process is going to be larger than 32MB */
#ifdef CONFIG_ARM_FCSE_MSSAGES
			printk(KERN_INFO "FCSE: process %u(%s) VM exceeds 32MB.\n",
			       current->pid, current->comm);
#endif /* CONFIG_ARM_FCSE_MESSAGES */
			mm->context.fcse.large = 1;
		}
		if (mm->context.fcse.pid)
			fcse_relocate_mm_to_null_pid(mm);
	}

	return addr;

#else /* CONFIG_ARM_FCSE_GUARANTEED */
	/* Address above 32MB */
	/* Restart to try and find a hole, once. */
	if (start_addr != TASK_UNMAPPED_BASE && !(fl & MAP_FIXED))
		return TASK_UNMAPPED_BASE;

	/* Fail, no 32MB processes in guaranteed mode. */
#ifdef CONFIG_ARM_FCSE_MESSAGES
	printk(KERN_WARNING "FCSE: process %u(%s) VM would exceed the 32MB limit.\n",
	       current->pid, current->comm);
#endif /* CONFIG_ARM_FCSE_MESSAGES */
	return -ENOMEM;
#endif /* CONFIG_ARM_FCSE_GUARANTEED */
}

#ifdef CONFIG_ARM_FCSE_MESSAGES
#define addr_in_vma(vma, addr)						\
	({								\
		struct vm_area_struct *_vma = (vma);			\
		((unsigned long)((addr) - _vma->vm_start)		\
		 < (unsigned long)((_vma->vm_end - _vma->vm_start)));	\
	})

#ifdef CONFIG_DEBUG_USER
static noinline void
dump_vmas(struct mm_struct *mm, unsigned long addr, struct pt_regs *regs)
{
	struct vm_area_struct *vma;
	char path[128];
	int locked = 0;

	printk("mappings:\n");
	if (!in_atomic())
		locked = down_read_trylock(&mm->mmap_sem);
	for(vma = mm->mmap; vma; vma = vma->vm_next) {
		struct file *file = vma->vm_file;
		int flags = vma->vm_flags;
		const char *name;

		printk("0x%08lx-0x%08lx %c%c%c%c 0x%08llx ",
		       vma->vm_start,
		       vma->vm_end,
		       flags & VM_READ ? 'r' : '-',
		       flags & VM_WRITE ? 'w' : '-',
		       flags & VM_EXEC ? 'x' : '-',
		       flags & VM_MAYSHARE ? 's' : 'p',
		       ((loff_t)vma->vm_pgoff) << PAGE_SHIFT);

		if (file)
			name = d_path(&file->f_path, path, sizeof(path));
		else if ((name = arch_vma_name(vma)))
			;
		else if (!vma->vm_mm)
			name = "[vdso]";
		else if (vma->vm_start <= mm->start_brk
			 && vma->vm_end >= mm->brk)
			name = "[heap]";
		else if (vma->vm_start <= mm->start_stack &&
			 vma->vm_end >= mm->start_stack)
			name = "[stack]";
		else
			name = "";
		printk("%s", name);
		if (addr_in_vma(vma, regs->ARM_pc))
			printk(" <- PC");
		if (addr_in_vma(vma, regs->ARM_sp))
			printk(" <- SP");
		if (addr_in_vma(vma, addr))
			printk("%s fault",
			       (addr_in_vma(vma, regs->ARM_pc)
				|| addr_in_vma(vma, regs->ARM_sp)
				? "," : " <-"));
		printk("\n");
	}
	if (locked)
		up_read(&mm->mmap_sem);
}
#endif /* CONFIG_DEBUG_USER */

void fcse_notify_segv(struct mm_struct *mm,
		       unsigned long addr, struct pt_regs *regs)
{
	int locked = 0;

#if defined(CONFIG_DEBUG_USER)
	if (user_debug & UDBG_SEGV)
		dump_vmas(mm, addr, regs);
#endif /* CONFIG_DEBUG_USER */

	if (!in_atomic())
		locked = down_read_trylock(&mm->mmap_sem);
	if (find_vma(mm, addr) == find_vma(mm, regs->ARM_sp))
		printk(KERN_INFO "FCSE: process %u(%s) probably overflowed stack at 0x%08lx.\n",
		       current->pid, current->comm, regs->ARM_pc);
	if (locked)
		up_read(&mm->mmap_sem);
}
#endif /* CONFIG_ARM_FCSE_MESSAGES */
