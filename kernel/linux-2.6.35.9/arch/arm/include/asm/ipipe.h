/* -*- linux-c -*-
 * arch/arm/include/asm/ipipe.h
 *
 * Copyright (C) 2002-2005 Philippe Gerum.
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
 */

#ifndef __ARM_IPIPE_H
#define __ARM_IPIPE_H

#ifdef CONFIG_IPIPE

#include <linux/ipipe_percpu.h>
#include <linux/ipipe_trace.h>

#define IPIPE_ARCH_STRING	"1.18-01"
#define IPIPE_MAJOR_NUMBER	1
#define IPIPE_MINOR_NUMBER	18
#define IPIPE_PATCH_NUMBER	1

#ifdef CONFIG_SMP
#define ipipe_processor_id()	hard_smp_processor_id()
#else /* !CONFIG_SMP */
#define ipipe_processor_id()	0
#endif	/* CONFIG_SMP */

#define smp_processor_id_hw() ipipe_processor_id()

#if defined(CONFIG_IPIPE_WANT_PREEMPTIBLE_SWITCH) || defined(CONFIG_SMP)

#define prepare_arch_switch(next)			\
	do {						\
		local_irq_enable_hw();			\
		ipipe_schedule_notify(current, next);	\
	} while(0)

#define task_hijacked(p)				\
	({						\
		int __x__ = ipipe_root_domain_p;	\
		!__x__;					\
	})

#define ipipe_mm_switch_protect(flags)		\
	do {					\
		(void)(flags);			\
	} while(0)

#define ipipe_mm_switch_unprotect(flags)	\
	do {					\
		(void)(flags);			\
	} while(0)

#else /* !CONFIG_IPIPE_WANT_PREEMPTIBLE_SWITCH && !SMP */

#define prepare_arch_switch(next)			\
	do {                                            \
		ipipe_schedule_notify(current ,next);   \
		local_irq_disable_hw();                 \
	} while(0)

#define task_hijacked(p)				\
	({						\
		int __x__ = __ipipe_root_domain_p;	\
		if (__x__) local_irq_enable_hw();	\
		!__x__;					\
	})

#define ipipe_mm_switch_protect(flags) \
	local_irq_save_hw_cond(flags)

#define ipipe_mm_switch_unprotect(flags) \
	local_irq_restore_hw_cond(flags)

#endif /* !CONFIG_IPIPE_WANT_PREEMPTIBLE_SWITCH && !SMP */

extern unsigned long arm_return_addr(int level);

#define BROKEN_BUILTIN_RETURN_ADDRESS
#define __BUILTIN_RETURN_ADDRESS0 arm_return_addr(0)
#define __BUILTIN_RETURN_ADDRESS1 arm_return_addr(1)

struct ipipe_domain;

#define IPIPE_TSC_TYPE_NONE	   		0
#define IPIPE_TSC_TYPE_FREERUNNING 		1
#define IPIPE_TSC_TYPE_DECREMENTER 		2
#define IPIPE_TSC_TYPE_FREERUNNING_COUNTDOWN	3

/* tscinfo, exported to user-space */
struct __ipipe_tscinfo {
	unsigned type;
	unsigned freq;
	unsigned long counter_vaddr;
	union {
		struct {
			unsigned long counter_paddr;
			unsigned mask;
		};
		struct {
			unsigned *counter; /* Hw counter physical address */
			unsigned mask; /* Significant bits in the hw counter. */
			unsigned long long *tsc; /* 64 bits tsc value. */
		} fr;
		struct {
			unsigned *counter; /* Hw counter physical address */
			unsigned mask; /* Significant bits in the hw counter. */
			unsigned *last_cnt; /* Counter value when updating
						tsc value. */
			unsigned long long *tsc; /* 64 bits tsc value. */
		} dec;
	} u;
};

struct ipipe_sysinfo {
       int sys_nr_cpus;        /* Number of CPUs on board */
       int sys_hrtimer_irq;    /* hrtimer device IRQ */
       u64 sys_hrtimer_freq;   /* hrtimer device frequency */
       u64 sys_hrclock_freq;   /* hrclock device frequency */
       u64 sys_cpu_freq;       /* CPU frequency (Hz) */
       struct __ipipe_tscinfo arch_tsc; /* exported data for u.s. tsc */
};

DECLARE_PER_CPU(struct mm_struct *,ipipe_active_mm);
/* arch specific stuff */
extern char __ipipe_tsc_area[];
extern int __ipipe_mach_timerstolen;
extern unsigned int __ipipe_mach_ticks_per_jiffy;
extern void __ipipe_mach_acktimer(void);
extern void __ipipe_mach_set_dec(unsigned long);
extern void __ipipe_mach_release_timer(void);
extern unsigned long __ipipe_mach_get_dec(void);
void __ipipe_mach_get_tscinfo(struct __ipipe_tscinfo *info);
int __ipipe_check_tickdev(const char *devname);

#ifdef CONFIG_IPIPE_ARM_KUSER_TSC
unsigned long long __ipipe_tsc_get(void) __attribute__((long_call));
void __ipipe_tsc_register(struct __ipipe_tscinfo *info);
void __ipipe_tsc_update(void);
#else /* ! generic tsc */
unsigned long long __ipipe_mach_get_tsc(void);
#define __ipipe_tsc_get() __ipipe_mach_get_tsc()
#endif /* ! generic tsc */

#ifndef __ipipe_cpu_freq
#define __ipipe_cpu_freq		(HZ * __ipipe_mach_ticks_per_jiffy)
#endif
#ifndef __ipipe_mach_hrtimer_freq
#define __ipipe_mach_hrtimer_freq	__ipipe_cpu_freq
#endif
#ifndef __ipipe_mach_hrclock_freq
#define	__ipipe_mach_hrclock_freq	__ipipe_mach_hrtimer_freq
#endif
#ifndef __ipipe_mach_hrtimer_irq
/*
 * hrtimer IRQ advertised to domains. Defaults to the contents of the
 * __ipipe_mach_timerint variable found in legacy platform supports.
 * May be overridden on SMP platforms with distinct per-CPU timer
 * interrupts.
 */
extern int __ipipe_mach_timerint;
#define __ipipe_mach_hrtimer_irq	__ipipe_mach_timerint
#endif
#ifndef __ipipe_mach_ext_hrtimer
/*
 * hrtimer external IRQ number for the given CPU. Same as the internal
 * hrtimer IRQ number by default.
 */
#define __ipipe_mach_ext_hrtimer(cpu)				\
	({							\
		(void)(cpu);					\
		__ipipe_mach_hrtimer_irq;			\
	})
#define __ipipe_mach_ext_hrtimer_p(irq)				\
	((irq) == __ipipe_mach_hrtimer_irq)
#endif
#ifndef __ipipe_mach_localtimer
/*
 * Some SMP platforms may use different IRQ numbers depending on the
 * CPU, remapping them to a single virq to advertise a common local
 * timer interrupt to domains. The macro below provides the hrtimer
 * IRQ number after remapping, if any. By default, there is no
 * remapping.
 */
#define __ipipe_mach_localtimer(ext_irq)  (ext_irq)
#endif
#ifndef __ipipe_mach_doirq
#define __ipipe_mach_doirq(irq)		__ipipe_do_IRQ
#endif
#ifndef __ipipe_mach_ackirq
#define __ipipe_mach_ackirq(irq)			\
	({						\
		__ipipe_mach_ext_hrtimer_p(irq)		\
			? __ipipe_ack_timerirq		\
			: __ipipe_ack_irq;		\
	})
#endif
#ifndef __ipipe_mach_hrtimer_debug
#define __ipipe_mach_hrtimer_debug(irq)	do { } while (0)
#endif

#define ipipe_read_tsc(t)	do { t = __ipipe_tsc_get(); } while (0)
#define __ipipe_read_timebase()	__ipipe_tsc_get()

#define ipipe_tsc2ns(t) \
({ \
	unsigned long long delta = (t)*1000; \
	do_div(delta, __ipipe_cpu_freq / 1000000 + 1); \
	(unsigned long)delta; \
})
#define ipipe_tsc2us(t) \
({ \
	unsigned long long delta = (t); \
	do_div(delta, __ipipe_cpu_freq / 1000000 + 1); \
	(unsigned long)delta; \
})

/* Private interface -- Internal use only */

#define __ipipe_check_platform()	do { } while(0)

#define __ipipe_enable_irq(irq)	irq_desc[irq].chip->enable(irq)

#define __ipipe_disable_irq(irq)	irq_desc[irq].chip->disable(irq)

#ifdef CONFIG_SMP
void __ipipe_init_platform(void);
void __ipipe_hook_critical_ipi(struct ipipe_domain *ipd);
void __ipipe_root_ipi(unsigned int irq, struct pt_regs *regs);
void __ipipe_root_localtimer(unsigned int irq, struct pt_regs *regs);
void __ipipe_send_vnmi(void (*fn)(void *), cpumask_t cpumask, void *arg);
void __ipipe_do_vnmi(unsigned int irq, void *cookie);
#else /* !CONFIG_SMP */
#define __ipipe_init_platform()		do { } while(0)
#define __ipipe_hook_critical_ipi(ipd)	do { } while(0)
#endif /* !CONFIG_SMP */
#ifndef __ipipe_mach_init_platform
#define __ipipe_mach_init_platform()	do { } while(0)
#endif

void __ipipe_enable_irqdesc(struct ipipe_domain *ipd, unsigned irq);

#ifndef __IPIPE_FEATURE_PIC_MUTE
#define __ipipe_disable_irqdesc(ipd, irq) do { } while (0)
#else /* __IPIPE_FEATURE_PIC_MUTE */

typedef unsigned long
__ipipe_irqbits_t[(NR_IRQS + BITS_PER_LONG - 1) / BITS_PER_LONG];
extern __ipipe_irqbits_t __ipipe_irqbits;

void __ipipe_mach_enable_irqdesc(struct ipipe_domain *ipd, unsigned irq);

void __ipipe_mach_disable_irqdesc(struct ipipe_domain *ipd, unsigned irq);

void __ipipe_disable_irqdesc(struct ipipe_domain *ipd, unsigned irq);

void ipipe_mute_pic(void);

void ipipe_unmute_pic(void);
#endif /* __IPIPE_FEATURE_PIC_MUTE */

void __ipipe_enable_pipeline(void);

void __ipipe_do_critical_sync(unsigned irq, void *cookie);

DECLARE_PER_CPU(struct pt_regs, __ipipe_tick_regs);

void __ipipe_grab_irq(int irq, struct pt_regs *regs);

void __ipipe_exit_irq(struct pt_regs *regs);

void __ipipe_handle_irq(int irq, struct pt_regs *regs);

static inline void ipipe_handle_chained_irq(unsigned int irq)
{
	struct pt_regs regs;    /* dummy */

	ipipe_trace_irq_entry(irq);
	__ipipe_handle_irq(irq, &regs);
	ipipe_trace_irq_exit(irq);
}

#define ipipe_update_tick_evtdev(evtdev) do { } while (0)

static inline unsigned long __ipipe_ffnz(unsigned long ul)
{
	return ffs(ul) - 1;
}

#define __ipipe_syscall_watched_p(p, sc)				\
	(ipipe_notifier_enabled_p(p) || (unsigned long)sc >= __ARM_NR_BASE + 64)

#define __ipipe_root_tick_p(regs) (!raw_irqs_disabled_flags(regs->ARM_cpsr))

#else /* !CONFIG_IPIPE */

#define task_hijacked(p)		0

#define ipipe_update_tick_evtdev(evtdev)	do { } while (0)

#define smp_processor_id_hw()		smp_processor_id()

#define ipipe_handle_chained_irq(irq)   generic_handle_irq(irq)

#define ipipe_mm_switch_protect(flags) \
	do {					\
		(void) (flags);			\
	} while (0)

#define ipipe_mm_switch_unprotect(flags)	\
	do {					\
		(void) (flags);			\
	} while (0)

#endif /* CONFIG_IPIPE */

#if defined (CONFIG_IPIPE_DEBUG) &&		\
	(defined(CONFIG_DEBUG_LL) || defined(CONFIG_SERIAL_8250_CONSOLE))
void __ipipe_serial_debug(const char *fmt, ...);
#else
#define __ipipe_serial_debug(fmt, args...)	do { } while (0)
#endif

#endif	/* !__ARM_IPIPE_H */
