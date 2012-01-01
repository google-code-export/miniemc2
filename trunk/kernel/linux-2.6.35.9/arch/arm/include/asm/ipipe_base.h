/* -*- linux-c -*-
 * arch/arm/include/asm/ipipe_base.h
 *
 * Copyright (C) 2007 Gilles Chanteperdrix.
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

#ifndef __ARM_IPIPE_BASE_H
#define __ARM_IPIPE_BASE_H

#include <linux/threads.h>
#include <linux/types.h>
#include <asm/ptrace.h>
#include <asm/irq.h>
#include <mach/irqs.h>		/* For __IPIPE_FEATURE_PIC_MUTE */

#define IPIPE_NR_XIRQS		NR_IRQS

/* ARM traps */
#define IPIPE_TRAP_ACCESS	 0	/* Data or instruction access exception */
#define IPIPE_TRAP_SECTION	 1	/* Section fault */
#define IPIPE_TRAP_DABT		 2	/* Generic data abort */
#define IPIPE_TRAP_UNKNOWN	 3	/* Unknown exception */
#define IPIPE_TRAP_BREAK	 4	/* Instruction breakpoint */
#define IPIPE_TRAP_FPU		 5	/* Floating point exception */
#define IPIPE_TRAP_VFP		 6	/* VFP floating point exception */
#define IPIPE_TRAP_UNDEFINSTR	 7	/* Undefined instruction */
#define IPIPE_TRAP_ALIGNMENT	 8	/* Unaligned access exception */
#define IPIPE_NR_FAULTS		 9

/* Pseudo-vectors used for kernel events */
#define IPIPE_FIRST_EVENT	IPIPE_NR_FAULTS
#define IPIPE_EVENT_SYSCALL	(IPIPE_FIRST_EVENT)
#define IPIPE_EVENT_SCHEDULE	(IPIPE_FIRST_EVENT + 1)
#define IPIPE_EVENT_SIGWAKE	(IPIPE_FIRST_EVENT + 2)
#define IPIPE_EVENT_SETSCHED	(IPIPE_FIRST_EVENT + 3)
#define IPIPE_EVENT_INIT	(IPIPE_FIRST_EVENT + 4)
#define IPIPE_EVENT_EXIT	(IPIPE_FIRST_EVENT + 5)
#define IPIPE_EVENT_CLEANUP	(IPIPE_FIRST_EVENT + 6)
#define IPIPE_EVENT_RETURN	(IPIPE_FIRST_EVENT + 7)
#define IPIPE_LAST_EVENT	IPIPE_EVENT_RETURN
#define IPIPE_NR_EVENTS		(IPIPE_LAST_EVENT + 1)

#ifndef __ASSEMBLY__

#include <asm/irqflags.h>

#ifdef CONFIG_SMP

#define IPIPE_FIRST_IPI		IPIPE_VIRQ_BASE

#define IPIPE_CRITICAL_IPI	IPIPE_FIRST_IPI
#define IPIPE_SERVICE_IPI0	(IPIPE_CRITICAL_IPI + 1)
#define IPIPE_SERVICE_IPI1	(IPIPE_CRITICAL_IPI + 2)
#define IPIPE_SERVICE_IPI2	(IPIPE_CRITICAL_IPI + 3)
#define IPIPE_SERVICE_IPI3	(IPIPE_CRITICAL_IPI + 4)
#define IPIPE_SERVICE_VNMI	(IPIPE_CRITICAL_IPI + 5)

#define IPIPE_LAST_IPI		IPIPE_SERVICE_VNMI

#define ipipe_ipi_p(ipi)	\
	((ipi) >= IPIPE_SERVICE_IPI0 && (ipi) <= IPIPE_SERVICE_IPI3)

void __ipipe_stall_root(void);

unsigned long __ipipe_test_and_stall_root(void);

unsigned long __ipipe_test_root(void);

#else /* !CONFIG_SMP */

#if __GNUC__ >= 4
/* Alias to ipipe_root_cpudom_var(status) */
extern unsigned long __ipipe_root_status;
#else
extern unsigned long *const __ipipe_root_status_addr;
#define __ipipe_root_status	(*__ipipe_root_status_addr)
#endif

static inline void __ipipe_stall_root(void)
{
	unsigned long flags;

	local_irq_save_hw(flags);
	__ipipe_root_status |= 1;
	local_irq_restore_hw(flags);
}

static inline unsigned __ipipe_test_root(void)
{
	return __ipipe_root_status & 1;
}

static inline unsigned __ipipe_test_and_stall_root(void)
{
	unsigned long flags, res;

	local_irq_save_hw(flags);
	res = __ipipe_root_status;
	__ipipe_root_status = res | 1;
	local_irq_restore_hw(flags);

	return res & 1;
}

#endif	/* CONFIG_SMP */

#define __IPIPE_FEATURE_PREEMPTIBLE_SWITCH	1
#define __IPIPE_FEATURE_SYSINFO_V2		1

#ifdef CONFIG_VFP
#define __IPIPE_FEATURE_VFP_SAFE		1
#endif

#ifdef CONFIG_IPIPE_ARM_KUSER_TSC
#define __IPIPE_FEATURE_KUSER_TSC
#endif

#endif /* !__ASSEMBLY__ */

#endif /* __ARM_IPIPE_BASE_H */
