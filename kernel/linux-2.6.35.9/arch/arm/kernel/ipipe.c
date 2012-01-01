/* -*- linux-c -*-
 * linux/arch/arm/kernel/ipipe.c
 *
 * Copyright (C) 2002-2005 Philippe Gerum.
 * Copyright (C) 2004 Wolfgang Grandegger (Adeos/arm port over 2.4).
 * Copyright (C) 2005 Heikki Lindholm (PowerPC 970 fixes).
 * Copyright (C) 2005 Stelian Pop.
 * Copyright (C) 2006-2008 Gilles Chanteperdrix.
 * Copyright (C) 2010 Philippe Gerum (SMP port).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, Inc., 675 Mass Ave, Cambridge MA 02139,
 * USA; either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Architecture-dependent I-PIPE support for ARM.
 */

#include <linux/kernel.h>
#include <linux/smp.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/bitops.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/kallsyms.h>
#include <linux/kprobes.h>
#include <linux/ipipe_trace.h>
#include <linux/irq.h>
#include <linux/irqnr.h>
#include <asm/system.h>
#include <asm/atomic.h>
#include <asm/hardirq.h>
#include <asm/io.h>
#include <asm/unistd.h>
#include <asm/mach/irq.h>
#include <asm/mmu_context.h>

/* Next tick date (timebase value). */
DEFINE_PER_CPU(struct pt_regs, __ipipe_tick_regs);
DEFINE_PER_CPU(struct mm_struct *,ipipe_active_mm);
EXPORT_PER_CPU_SYMBOL(ipipe_active_mm);
#ifdef __IPIPE_FEATURE_PIC_MUTE
__ipipe_irqbits_t __ipipe_irqbits;
IPIPE_DEFINE_SPINLOCK(__ipipe_irqbits_lock);
#endif /* __IPIPE_FEATURE_PIC_MUTE */

asmlinkage void asm_do_IRQ(unsigned int irq, struct pt_regs *regs);

static void __ipipe_do_IRQ(unsigned irq, void *cookie);

#ifdef CONFIG_SMP

struct __ipipe_vnmidata {
	void (*fn)(void *);
	void *arg;
	cpumask_t cpumask;
};

static struct __ipipe_vnmislot {
	ipipe_spinlock_t lock;
	struct __ipipe_vnmidata *data;
	ipipe_rwlock_t data_lock;
} __ipipe_vnmi __cacheline_aligned_in_smp = {
	.lock		= IPIPE_SPIN_LOCK_UNLOCKED,
	.data		= NULL,
	.data_lock	= IPIPE_RW_LOCK_UNLOCKED,
};

void __ipipe_init_platform(void)
{
	unsigned int virq, _virq;

	for (virq = IPIPE_FIRST_IPI; virq <= IPIPE_LAST_IPI; virq++) {
		_virq = ipipe_alloc_virq();
		if (virq != _virq)
			panic("I-pipe: cannot reserve virq #%d (got #%d)\n",
			      virq, _virq);
	}

	__ipipe_mach_init_platform();
}

void __ipipe_stall_root(void)
{
	unsigned long flags;

	local_irq_save_hw(flags);
	set_bit(IPIPE_STALL_FLAG, &ipipe_root_cpudom_var(status));
	local_irq_restore_hw(flags);
}
EXPORT_SYMBOL(__ipipe_stall_root);

unsigned long __ipipe_test_and_stall_root(void)
{
	unsigned long flags;
	int x;

	local_irq_save_hw(flags);
	x = test_and_set_bit(IPIPE_STALL_FLAG, &ipipe_root_cpudom_var(status));
	local_irq_restore_hw(flags);

	return x;
}
EXPORT_SYMBOL(__ipipe_test_and_stall_root);

unsigned long __ipipe_test_root(void)
{
	unsigned long flags;
	int x;

	local_irq_save_hw(flags);
	x = test_bit(IPIPE_STALL_FLAG, &ipipe_root_cpudom_var(status));
	local_irq_restore_hw(flags);

	return x;
}
EXPORT_SYMBOL(__ipipe_test_root);

void __ipipe_do_vnmi(unsigned int irq, void *cookie)
{
	int cpu = ipipe_processor_id();
	struct __ipipe_vnmidata *data;

	read_lock(&__ipipe_vnmi.data_lock);

	data = __ipipe_vnmi.data;
	if (likely(data && cpumask_test_cpu(cpu, &data->cpumask))) {
		data->fn(data->arg);
		cpu_clear(cpu, data->cpumask);
	}

	read_unlock(&__ipipe_vnmi.data_lock);
}

static inline void
hook_internal_ipi(struct ipipe_domain *ipd, int virq,
		  void (*handler)(unsigned int irq, void *cookie))
{
	ipd->irqs[virq].acknowledge = NULL;
	ipd->irqs[virq].handler = handler;
	ipd->irqs[virq].cookie = NULL;
	/* Immediately handle in the current domain but *never* pass */
	ipd->irqs[virq].control =
		IPIPE_HANDLE_MASK|IPIPE_STICKY_MASK|IPIPE_SYSTEM_MASK;
}

void __ipipe_hook_critical_ipi(struct ipipe_domain *ipd)
{
	hook_internal_ipi(ipd, IPIPE_CRITICAL_IPI, __ipipe_do_critical_sync);
	hook_internal_ipi(ipd, IPIPE_SERVICE_VNMI, __ipipe_do_vnmi);
}

cpumask_t __ipipe_set_irq_affinity(unsigned irq, cpumask_t cpumask)
{
	cpumask_t oldmask;

	if (irq_to_desc(irq)->chip->set_affinity == NULL)
		return CPU_MASK_NONE;

	if (cpus_empty(cpumask))
		return CPU_MASK_NONE; /* Return mask value -- no change. */

	cpus_and(cpumask, cpumask, cpu_online_map);
	if (cpus_empty(cpumask))
		return CPU_MASK_NONE;	/* Error -- bad mask value or non-routable IRQ. */

	cpumask_copy(&oldmask, irq_to_desc(irq)->affinity);
	irq_to_desc(irq)->chip->set_affinity(irq, &cpumask);

	return oldmask;
}

void __ipipe_send_vnmi(void (*fn)(void *), cpumask_t cpumask, void *arg)
{
	struct __ipipe_vnmidata data;
	unsigned long flags;
	int cpu;

	data.fn = fn;
	data.arg = arg;
	data.cpumask = cpumask;

	while (!spin_trylock_irqsave(&__ipipe_vnmi.lock, flags)) {
		if (irqs_disabled_hw())
			__ipipe_do_vnmi(IPIPE_SERVICE_VNMI, NULL);
		cpu_relax();
	}

	cpu = ipipe_processor_id();
	cpu_clear(cpu, data.cpumask);
	if (cpus_empty(data.cpumask)) {
		spin_unlock_irqrestore(&__ipipe_vnmi.lock, flags);
		return;
	}

	write_lock(&__ipipe_vnmi.data_lock);
	__ipipe_vnmi.data = &data;
	write_unlock(&__ipipe_vnmi.data_lock);

	__ipipe_send_ipi(IPIPE_SERVICE_VNMI, data.cpumask);
	while (!cpus_empty(data.cpumask))
		cpu_relax();

	write_lock(&__ipipe_vnmi.data_lock);
	__ipipe_vnmi.data = NULL;
	write_unlock(&__ipipe_vnmi.data_lock);

	spin_unlock_irqrestore(&__ipipe_vnmi.lock, flags);
}
EXPORT_SYMBOL_GPL(__ipipe_send_vnmi);

static void __ipipe_relay_ext_hrtimer(unsigned int irq, void *cookie)
{
	int cpu = ipipe_processor_id();
	/*
	 * We are running over the root domain here, so hw IRQs are
	 * ON.  Therefore, we have to use the atomic form of
	 * __ipipe_schedule_irq_root(). Don't mess with that, or you
	 * may lose interrupt notifications.
	 */
	ipipe_schedule_irq_root(__ipipe_mach_ext_hrtimer(cpu));
}

#endif	/* CONFIG_SMP */

/*
 * ipipe_trigger_irq() -- Push the interrupt at front of the pipeline
 * just like if it has been actually received from a hw source. Also
 * works for virtual interrupts.
 */
int ipipe_trigger_irq(unsigned irq)
{
	unsigned long flags;

#ifdef CONFIG_IPIPE_DEBUG
	if (irq >= IPIPE_NR_IRQS)
		return -EINVAL;
	if (ipipe_virtual_irq_p(irq)) {
		if (!test_bit(irq - IPIPE_VIRQ_BASE,
			      &__ipipe_virtual_irq_map))
			return -EINVAL;
	} else if (irq_to_desc(irq) == NULL)
		return -EINVAL;
#endif
	local_irq_save_hw(flags);
	__ipipe_handle_irq(irq, NULL);
	local_irq_restore_hw(flags);

	return 1;
}

int ipipe_get_sysinfo(struct ipipe_sysinfo *info)
{
	info->sys_nr_cpus = num_online_cpus();
	info->sys_cpu_freq = __ipipe_cpu_freq;
	info->sys_hrtimer_irq = __ipipe_mach_hrtimer_irq;
	info->sys_hrtimer_freq = __ipipe_mach_hrtimer_freq;
	info->sys_hrclock_freq = __ipipe_mach_hrclock_freq;
	__ipipe_mach_get_tscinfo(&info->arch_tsc);

	return 0;
}

static void __ipipe_ack_irq(unsigned irq, struct irq_desc *desc)
{
	desc->ipipe_ack(irq, desc);
}

static void __maybe_unused __ipipe_ack_timerirq(unsigned int irq, struct irq_desc *desc)
{
	desc->ipipe_ack(irq, desc);
	__ipipe_mach_acktimer();
	desc->ipipe_end(irq, desc);
}

void __ipipe_enable_irqdesc(struct ipipe_domain *ipd, unsigned irq)
{
	unsigned long flags;
	irq_to_desc(irq)->status &= ~IRQ_DISABLED;

#ifdef __IPIPE_FEATURE_PIC_MUTE
	spin_lock_irqsave(&__ipipe_irqbits_lock, flags);
	if (ipd == &ipipe_root)
		__ipipe_irqbits[irq / BITS_PER_LONG]
			|= (1 << (irq % BITS_PER_LONG));
	else
		__ipipe_irqbits[irq / BITS_PER_LONG]
			&= ~(1 << (irq % BITS_PER_LONG));
	spin_unlock_irqrestore(&__ipipe_irqbits_lock, flags);

	__ipipe_mach_enable_irqdesc(ipd, irq);
#else
	(void) flags;
#endif /* __IPIPE_FEATURE_PIC_MUTE */
}
EXPORT_SYMBOL(__ipipe_enable_irqdesc);

#ifdef __IPIPE_FEATURE_PIC_MUTE
void __ipipe_disable_irqdesc(struct ipipe_domain *ipd, unsigned irq)
{
	unsigned long flags;

	__ipipe_mach_disable_irqdesc(ipd, irq);

	spin_lock_irqsave(&__ipipe_irqbits_lock, flags);
	if (ipd == &ipipe_root || !ipipe_root.irqs[irq].handler)
		__ipipe_irqbits[irq / BITS_PER_LONG]
			&= ~(1 << (irq % BITS_PER_LONG));
	spin_unlock_irqrestore(&__ipipe_irqbits_lock, flags);

}
EXPORT_SYMBOL(__ipipe_disable_irqdesc);
EXPORT_SYMBOL(ipipe_mute_pic);
EXPORT_SYMBOL(ipipe_unmute_pic);
#endif /* __IPIPE_FEATURE_PIC_MUTE */

/*
 * __ipipe_enable_pipeline() -- We are running on the boot CPU, hw
 * interrupts are off, and secondary CPUs are still lost in space.
 */
void __ipipe_enable_pipeline(void)
{
	unsigned long flags;
	unsigned int irq;

#ifdef CONFIG_CPU_ARM926T
	/*
	 * We do not want "wfi" to be called in arm926ejs based
	 * processor, as this causes Linux to disable the I-cache
	 * when idle.
	 */
	disable_hlt();
#endif
	flags = ipipe_critical_enter(NULL);

	/* First, virtualize all interrupts from the root domain. */

	for (irq = 0; irq < NR_IRQS; irq++)
		ipipe_virtualize_irq(ipipe_root_domain,
				     irq,
				     (ipipe_irq_handler_t)__ipipe_mach_doirq(irq),
				     NULL, __ipipe_mach_ackirq(irq),
				     IPIPE_HANDLE_MASK | IPIPE_PASS_MASK);
#ifdef CONFIG_SMP
	/*
	 * If the external hrtimer IRQ is remapped internally to a
	 * common localtimer IRQ number, we have to relay the virtual
	 * localtimer interrupt reaching the root stage to the proper
	 * external IRQ handlers defined by the kernel.
	 *
	 * NOTE: we don't make any assumption regarding whether the
	 * virtual localtimer interrupt should be acked or not, so we
	 * set the ack handler as the platform-specific
	 * __ipipe_mach_ackirq() tells us, like for any other
	 * interrupt.
	 */
	if (__ipipe_mach_ext_hrtimer(0) != __ipipe_mach_hrtimer_irq)
		ipipe_virtualize_irq(ipipe_root_domain,
				     __ipipe_mach_hrtimer_irq,
				     __ipipe_relay_ext_hrtimer,
				     NULL, __ipipe_mach_ackirq(__ipipe_mach_hrtimer_irq),
				     IPIPE_HANDLE_MASK);
#endif

	ipipe_critical_exit(flags);
}

#ifdef CONFIG_SMP
asmlinkage int __ipipe_check_root(void)
{
	return ipipe_root_domain_p;
}

asmlinkage int __ipipe_check_root_interruptible(void)
{
	return __ipipe_root_domain_p && !irqs_disabled();
}
#endif

__kprobes int
__ipipe_switch_to_notifier_call_chain(struct atomic_notifier_head *nh,
				      unsigned long val, void *v)
{
	unsigned long flags;
	int ret;

	local_irq_save(flags);
	ret = atomic_notifier_call_chain(nh, val, v);
	__local_irq_restore_nosync(flags);

	return ret;
}

asmlinkage int __ipipe_syscall_root(unsigned long scno, struct pt_regs *regs)
{
	struct ipipe_percpu_domain_data *p;
	unsigned long orig_r7;
	int ret = 0;

	WARN_ON_ONCE(irqs_disabled_hw());

	/*
	 * We use r7 to pass the syscall number to the other domains.
	 */
	orig_r7 = regs->ARM_r7;
	regs->ARM_r7 = __NR_SYSCALL_BASE + scno;

	/*
	 * This routine either returns:
	 * 0 -- if the syscall is to be passed to Linux;
	 * >0 -- if the syscall should not be passed to Linux, and no
	 * tail work should be performed;
	 * <0 -- if the syscall should not be passed to Linux but the
	 * tail work has to be performed (for handling signals etc).
	 */

	if (!__ipipe_syscall_watched_p(current, regs->ARM_r7) ||
	    !__ipipe_event_monitored_p(IPIPE_EVENT_SYSCALL))
		goto out;

	ret = __ipipe_dispatch_event(IPIPE_EVENT_SYSCALL, regs);

	local_irq_disable_hw();

	/*
	 * This is the end of the syscall path, so we may
	 * safely assume a valid Linux task stack here.
	 */
	if (current->ipipe_flags & PF_EVTRET) {
		current->ipipe_flags &= ~PF_EVTRET;
		__ipipe_dispatch_event(IPIPE_EVENT_RETURN, regs);
	}

	if (!__ipipe_root_domain_p)
		ret = -1;
	else {
		p = ipipe_root_cpudom_ptr();
		if (__ipipe_ipending_p(p))
			__ipipe_sync_pipeline();
	}

	local_irq_enable_hw();
out:
	regs->ARM_r7 = orig_r7;

	return -ret;
}

/*
 * __ipipe_handle_irq() -- IPIPE's generic IRQ handler. An optimistic
 * interrupt protection log is maintained here for each domain. Hw
 * interrupts are off on entry.
 */
void __ipipe_handle_irq(int irq, struct pt_regs *regs)
{
	struct ipipe_domain *this_domain, *next_domain;
	struct list_head *head, *pos;
	struct irq_desc *desc;
	int m_ack;

	/* Software-triggered IRQs do not need any ack. */
	m_ack = (regs == NULL);

#ifdef CONFIG_IPIPE_DEBUG
	if (unlikely(irq >= IPIPE_NR_IRQS) ||
	    (!ipipe_virtual_irq_p(irq) && irq_to_desc(irq) == NULL)) {
		printk(KERN_ERR "I-pipe: spurious interrupt %d\n", irq);
		return;
	}
#endif
	this_domain = ipipe_current_domain;
	if (unlikely(test_bit(IPIPE_STICKY_FLAG, &this_domain->irqs[irq].control)))
		head = &this_domain->p_link;
	else {
		head = __ipipe_pipeline.next;
		next_domain = list_entry(head, struct ipipe_domain, p_link);
		if (likely(test_bit(IPIPE_WIRED_FLAG, &next_domain->irqs[irq].control))) {
			if (!m_ack && next_domain->irqs[irq].acknowledge) {
				desc = ipipe_virtual_irq_p(irq) ? NULL : irq_to_desc(irq);
				next_domain->irqs[irq].acknowledge(irq, desc);
			}
			__ipipe_dispatch_wired(next_domain, irq);
			return;
		}
	}

	/* Ack the interrupt. */

	pos = head;
	while (pos != &__ipipe_pipeline) {
		next_domain = list_entry(pos, struct ipipe_domain, p_link);
		prefetch(next_domain);
		/*
		 * For each domain handling the incoming IRQ, mark it as
		 * pending in its log.
		 */
		if (test_bit(IPIPE_HANDLE_FLAG, &next_domain->irqs[irq].control)) {
			/*
			 * Domains that handle this IRQ are polled for
			 * acknowledging it by decreasing priority order. The
			 * interrupt must be made pending _first_ in the
			 * domain's status flags before the PIC is unlocked.
			 */
			__ipipe_set_irq_pending(next_domain, irq);

			if (!m_ack && next_domain->irqs[irq].acknowledge) {
				desc = ipipe_virtual_irq_p(irq) ? NULL : irq_to_desc(irq);
				next_domain->irqs[irq].acknowledge(irq, desc);
				m_ack = 1;
			}
		}

		/*
		 * If the domain does not want the IRQ to be passed down the
		 * interrupt pipe, exit the loop now.
		 */
		if (!test_bit(IPIPE_PASS_FLAG, &next_domain->irqs[irq].control))
			break;

		pos = next_domain->p_link.next;
	}

	/*
	 * If the interrupt preempted the head domain, then do not
	 * even try to walk the pipeline, unless an interrupt is
	 * pending for it.
	 */
	if (test_bit(IPIPE_AHEAD_FLAG, &this_domain->flags) &&
	    !__ipipe_ipending_p(ipipe_head_cpudom_ptr()))
		return;

	/*
	 * Now walk the pipeline, yielding control to the highest
	 * priority domain that has pending interrupt(s) or
	 * immediately to the current domain if the interrupt has been
	 * marked as 'sticky'. This search does not go beyond the
	 * current domain in the pipeline.
	 */
	__ipipe_walk_pipeline(head);
}

void __ipipe_exit_irq(struct pt_regs *regs)
{
	if (user_mode(regs) &&
	    (current->ipipe_flags & PF_EVTRET) != 0) {
		/*
		 * Testing for user_regs() eliminates foreign stack
		 * contexts, including from careless domains which did
		 * not set the foreign stack bit (foreign stacks are
		 * always kernel-based).
		 */
		current->ipipe_flags &= ~PF_EVTRET;
		__ipipe_dispatch_event(IPIPE_EVENT_RETURN, regs);
	}
}

asmlinkage void __ipipe_grab_irq(int irq, struct pt_regs *regs)
{
	int cpu = ipipe_processor_id();
#ifdef irq_finish
	/* AT91 specific workaround */
	irq_finish(irq);
#endif /* irq_finish */
	/*
	 * Some SMP platforms may remap different per-CPU local timer
	 * interrupts onto a single (usually virtual) IRQ, so that the
	 * I-pipe advertises the same local timer IRQ number to the
	 * client domains, regardless of the CPU. So we first check
	 * whether we received an external hrtimer IRQ for the current
	 * CPU, then remap it to its local timer number if so - most
	 * platforms won't change this value though.
	 */
	if (likely(__ipipe_mach_ext_hrtimer(cpu) == irq)) {
		__ipipe_mach_hrtimer_debug(irq);
		irq = __ipipe_mach_localtimer(irq);
		/*
		 * Given our deferred dispatching model for regular IRQs, we
		 * only record CPU regs for the last timer interrupt, so that
		 * the timer handler charges CPU times properly. It is assumed
		 * that other interrupt handlers don't actually care for such
		 * information.
		 */
		__raw_get_cpu_var(__ipipe_tick_regs).ARM_cpsr =
			(ipipe_root_domain_p
			 ? regs->ARM_cpsr
			 : regs->ARM_cpsr | PSR_I_BIT);
		__raw_get_cpu_var(__ipipe_tick_regs).ARM_pc = regs->ARM_pc;
	}

#ifdef CONFIG_IPIPE_TRACE_IRQSOFF
	ipipe_trace_begin(regs->ARM_ORIG_r0);
#endif

	__ipipe_handle_irq(irq, regs);

#ifdef CONFIG_IPIPE_TRACE_IRQSOFF
	ipipe_trace_end(regs->ARM_ORIG_r0);
#endif
	__ipipe_exit_irq(regs);
}

static void __ipipe_do_IRQ(unsigned irq, void *cookie)
{
	asm_do_IRQ(irq, &__raw_get_cpu_var(__ipipe_tick_regs));
}

asmlinkage int __ipipe_dispatch_trap(int trap, struct pt_regs *regs)
{
	return ipipe_trap_notify(trap, regs);
}

#if defined(CONFIG_DEBUG_LL) && defined(CONFIG_IPIPE_DEBUG)

void printascii(const char *s);

static IPIPE_DEFINE_SPINLOCK(serial_debug_lock);

void __ipipe_serial_debug(const char *fmt, ...)
{
	unsigned long flags;
	char buf[128];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
	va_end(ap);

	if (n > 0 && buf[n - 1] == '\n') {
		buf[n] = '\r';
		buf[n+1] = '\0';
	}

	spin_lock_irqsave(&serial_debug_lock, flags);
	printascii(buf);
	spin_unlock_irqrestore(&serial_debug_lock, flags);
}

#ifndef CONFIG_SERIAL_8250_CONSOLE
EXPORT_SYMBOL_GPL(__ipipe_serial_debug);
#endif

#endif

EXPORT_SYMBOL_GPL(show_stack);
EXPORT_SYMBOL_GPL(init_mm);
#ifndef MULTI_CPU
EXPORT_SYMBOL_GPL(cpu_do_switch_mm);
#endif
EXPORT_SYMBOL_GPL(__check_kvm_seq);
#if defined(CONFIG_SMP) || defined(CONFIG_DEBUG_SPINLOCK)
EXPORT_SYMBOL_GPL(tasklist_lock);
#endif /* CONFIG_SMP || CONFIG_DEBUG_SPINLOCK */

#ifdef CONFIG_SPARSE_IRQ
EXPORT_SYMBOL_GPL(irq_to_desc);
#else
EXPORT_SYMBOL_GPL(irq_desc);
#endif

#ifdef CONFIG_CPU_HAS_ASID
EXPORT_SYMBOL_GPL(__new_context);
EXPORT_SYMBOL_GPL(cpu_last_asid);
#endif /* CONFIG_CPU_HAS_ASID */
