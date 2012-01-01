#ifndef __ASM_ARM_IRQFLAGS_H
#define __ASM_ARM_IRQFLAGS_H

#ifdef __KERNEL__

#include <asm/ptrace.h>

/*
 * CPU interrupt mask handling.
 */
#if __LINUX_ARM_ARCH__ >= 6

#define local_irq_save_hw_notrace(x)					\
	({							\
	__asm__ __volatile__(					\
	"mrs	%0, cpsr		@ local_irq_save_hw\n"	\
	"cpsid	i"						\
	: "=r" (x) : : "memory", "cc");				\
	})

#define local_irq_enable_hw_notrace()  __asm__("cpsie i	@ __sti" : : : "memory", "cc")
#define local_irq_disable_hw_notrace() __asm__("cpsid i	@ __cli" : : : "memory", "cc")
#define local_fiq_enable_hw_notrace()  __asm__("cpsie f	@ __stf" : : : "memory", "cc")
#define local_fiq_disable_hw_notrace() __asm__("cpsid f	@ __clf" : : : "memory", "cc")

#else

/*
 * Save the current interrupt enable state & disable IRQs
 */
#define local_irq_save_hw_notrace(x)					\
	({							\
		unsigned long temp;				\
		(void) (&temp == &x);				\
	__asm__ __volatile__(					\
	"mrs	%0, cpsr		@ local_irq_save_hw\n"	\
"	orr	%1, %0, #128\n"					\
"	msr	cpsr_c, %1"					\
	: "=r" (x), "=r" (temp)					\
	:							\
	: "memory", "cc");					\
	})
	
/*
 * Enable IRQs
 */
#define local_irq_enable_hw_notrace()				\
	({							\
		unsigned long temp;				\
	__asm__ __volatile__(					\
	"mrs	%0, cpsr		@ local_irq_enable_hw\n"	\
"	bic	%0, %0, #128\n"					\
"	msr	cpsr_c, %0"					\
	: "=r" (temp)						\
	:							\
	: "memory", "cc");					\
	})

/*
 * Disable IRQs
 */
#define local_irq_disable_hw_notrace()				\
	({							\
		unsigned long temp;				\
	__asm__ __volatile__(					\
	"mrs	%0, cpsr		@ local_irq_disable_hw\n"	\
"	orr	%0, %0, #128\n"					\
"	msr	cpsr_c, %0"					\
	: "=r" (temp)						\
	:							\
	: "memory", "cc");					\
	})

/*
 * Enable FIQs
 */
#define local_fiq_enable_hw_notrace()				\
	({							\
		unsigned long temp;				\
	__asm__ __volatile__(					\
	"mrs	%0, cpsr		@ stf\n"		\
"	bic	%0, %0, #64\n"					\
"	msr	cpsr_c, %0"					\
	: "=r" (temp)						\
	:							\
	: "memory", "cc");					\
	})

/*
 * Disable FIQs
 */
#define local_fiq_disable_hw_notrace()				\
	({							\
		unsigned long temp;				\
	__asm__ __volatile__(					\
	"mrs	%0, cpsr		@ clf\n"		\
"	orr	%0, %0, #64\n"					\
"	msr	cpsr_c, %0"					\
	: "=r" (temp)						\
	:							\
	: "memory", "cc");					\
	})

#endif

/*
 * Save the current interrupt enable state.
 */
#define local_save_flags_hw(x)					\
	({							\
	__asm__ __volatile__(					\
	"mrs	%0, cpsr		@ local_save_flags_hw"	\
	: "=r" (x) : : "memory", "cc");				\
	})

/*
 * restore saved IRQ & FIQ state
 */
#define local_irq_restore_hw_notrace(x)				\
	__asm__ __volatile__(					\
	"msr	cpsr_c, %0		@ local_irq_restore_hw\n"	\
	:							\
	: "r" (x)						\
	: "memory", "cc")

#define raw_irqs_disabled_flags(flags)	\
({					\
	(int)((flags) & PSR_I_BIT);	\
})

#define irqs_disabled_hw()			\
({						\
	unsigned long flags;			\
	local_save_flags_hw(flags);		\
	raw_irqs_disabled_flags(flags);		\
})

static inline unsigned long raw_mangle_irq_bits(int virt, unsigned long real)
{
	/* Merge virtual and real interrupt mask bits into a single
	   32bit word. */
	return (real & ~(1L << 8)) | ((virt != 0) << 8);
}

static inline int raw_demangle_irq_bits(unsigned long *x)
{
	int virt = (*x & (1 << 8)) != 0;
	*x &= ~(1L << 8);
	return virt;
}

#ifdef CONFIG_IPIPE

void __ipipe_unstall_root(void);
void __ipipe_restore_root(unsigned long flags);

/* PSR_I_BIT is bit no. 7 and is set if interrupts are _disabled_ */
#define raw_local_irq_save(flags)		((flags) = __ipipe_test_and_stall_root() << 7)
#define raw_local_irq_enable()		__ipipe_unstall_root()
#define raw_local_irq_disable()		__ipipe_stall_root()
//#define local_fiq_enable()		__ipipe_unstall_root()
//#define local_fiq_disable()		__ipipe_stall_root()
#define local_fiq_enable()		local_fiq_enable_hw_notrace()
#define local_fiq_disable()		local_fiq_disable_hw_notrace()
#define raw_local_save_flags(flags)	((flags) = __ipipe_test_root() << 7)
#define raw_local_irq_restore(flags)	__ipipe_restore_root(flags & (1 << 7))

#ifdef CONFIG_IPIPE_TRACE_IRQSOFF

#include <linux/ipipe_trace.h>

#define local_irq_disable_hw() do { \
	if (!irqs_disabled_hw()) { \
		local_irq_disable_hw_notrace(); \
		ipipe_trace_begin(0x80000000); \
	} \
} while (0)
#define local_irq_enable_hw() do { \
	if (irqs_disabled_hw()) { \
		ipipe_trace_end(0x80000000); \
		local_irq_enable_hw_notrace(); \
	} \
} while (0)
#define local_irq_save_hw(x) do { \
	local_save_flags_hw(x); \
	if (!raw_irqs_disabled_flags(x)) { \
		local_irq_disable_hw_notrace(); \
		ipipe_trace_begin(0x80000001); \
	} \
} while (0)
#define local_irq_restore_hw(x) do { \
	if (!raw_irqs_disabled_flags(x)) \
		ipipe_trace_end(0x80000001); \
	local_irq_restore_hw_notrace(x); \
} while (0)

#else /* !CONFIG_IPIPE_TRACE_IRQSOFF */

#define local_irq_save_hw(flags)	local_irq_save_hw_notrace(flags)
#define local_irq_enable_hw()		local_irq_enable_hw_notrace()
#define local_irq_disable_hw()		local_irq_disable_hw_notrace()
#define local_fiq_enable_hw()		local_fiq_enable_hw_notrace()
#define local_fiq_disable_hw()		local_fiq_disable_hw_notrace()
#define local_irq_restore_hw(flags)	local_irq_restore_hw_notrace(flags)

#endif /* CONFIG_IPIPE_TRACE_IRQSOFF */

#else /* !CONFIG_IPIPE */

#define raw_local_irq_save(flags)	local_irq_save_hw_notrace(flags)
#define raw_local_irq_enable()		local_irq_enable_hw_notrace()
#define raw_local_irq_disable()		local_irq_disable_hw_notrace()
#define local_fiq_enable()		local_fiq_enable_hw_notrace()
#define local_fiq_disable()		local_fiq_disable_hw_notrace()
#define raw_local_save_flags(flags)	local_save_flags_hw(flags)
#define raw_local_irq_restore(flags)	local_irq_restore_hw_notrace(flags)

#define local_irq_save_hw(flags)	local_irq_save_hw_notrace(flags)
#define local_irq_enable_hw()		local_irq_enable_hw_notrace()
#define local_irq_disable_hw()		local_irq_disable_hw_notrace()
#define local_fiq_enable_hw()		local_fiq_enable_hw_notrace()
#define local_fiq_disable_hw()		local_fiq_disable_hw_notrace()
#define local_irq_restore_hw(flags)	local_irq_restore_hw_notrace(flags)

#endif /* CONFIG_IPIPE */

#endif
#endif
