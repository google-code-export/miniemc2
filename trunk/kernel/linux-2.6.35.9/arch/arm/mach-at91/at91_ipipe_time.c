/*
 * linux/arch/arm/mach-at91/at91_ipipe_time.c
 *
 * Copyright (C) 2007 Gilles Chanteperdrix <gilles.chanteperdrix@xenomai.org>
 *
 * Adaptation to AT91SAM926x:
 * Copyright (C) 2007 Gregory CLEMENT, Adeneo
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/clockchips.h>
#include <linux/clk.h>
#include <linux/stringify.h>
#include <linux/err.h>
#include <linux/console.h>
#include <linux/module.h>
#include <linux/ipipe.h>

#include <asm/io.h>
#include <asm/mach/time.h>

#include <mach/hardware.h>
#include <mach/at91_st.h>
#include <mach/at91_tc.h>
#include <mach/at91_pit.h>
#include "clock.h"

#if defined(CONFIG_ARCH_AT91RM9200)
#define AT91_ID_TC0 AT91RM9200_ID_TC0
#define AT91_ID_TC1 AT91RM9200_ID_TC1
#define AT91_ID_TC2 AT91RM9200_ID_TC2
#elif defined(CONFIG_ARCH_AT91SAM9260) || defined(CONFIG_ARCH_AT91SAM9G20)
#define AT91_ID_TC0 AT91SAM9260_ID_TC0
#define AT91_ID_TC1 AT91SAM9260_ID_TC1
#define AT91_ID_TC2 AT91SAM9260_ID_TC2
#elif defined(CONFIG_ARCH_AT91SAM9261)
#define AT91_ID_TC0 AT91SAM9261_ID_TC0
#define AT91_ID_TC1 AT91SAM9261_ID_TC1
#define AT91_ID_TC2 AT91SAM9261_ID_TC2
#elif defined(CONFIG_ARCH_AT91SAM9263)
#define AT91_ID_TC0 AT91SAM9263_ID_TCB
#define AT91_ID_TC1 AT91SAM9263_ID_TCB
#define AT91_ID_TC2 AT91SAM9263_ID_TCB
#elif defined(CONFIG_ARCH_AT91SAM9RL)
#define AT91_ID_TC0 AT91SAM9RL_ID_TC0
#define AT91_ID_TC1 AT91SAM9RL_ID_TC1
#define AT91_ID_TC2 AT91SAM9RL_ID_TC2
#elif defined(CONFIG_ARCH_AT91X40)
#define AT91_ID_TC0 AT91X40_ID_TC0
#define AT91_ID_TC1 AT91X40_ID_TC1
#define AT91_ID_TC2 AT91X40_ID_TC2
#else
#error "AT91 processor unsupported by Adeos"
#endif

#if (CONFIG_IPIPE_AT91_TC==0)
#   define KERNEL_TIMER_IRQ_NUM AT91_ID_TC0
#elif (CONFIG_IPIPE_AT91_TC==1)
#   define KERNEL_TIMER_IRQ_NUM AT91_ID_TC1
#elif (CONFIG_IPIPE_AT91_TC==2)
#   define KERNEL_TIMER_IRQ_NUM AT91_ID_TC2
#else
#error IPIPE_AT91_TC must be 0, 1 or 2.
#endif

#define TCNXCNS(timer,v) ((v) << ((timer)<<1))
#define AT91_TC_REG_MASK (0xffff)

static unsigned long next_match;

static unsigned max_delta_ticks, min_delta_ticks;
static struct clock_event_device clkevt;
static int tc_timer_clock;

static inline unsigned int at91_tc_read(unsigned int reg_offset)
{
	unsigned long addr =
		(AT91_VA_BASE_TCB0 + 0x40 * CONFIG_IPIPE_AT91_TC);

	return readl((void __iomem *)(addr + reg_offset));
}

static inline void at91_tc_write(unsigned int reg_offset, unsigned long value)
{
	unsigned long addr =
		(AT91_VA_BASE_TCB0 + 0x40 * CONFIG_IPIPE_AT91_TC);

	writel(value, (void __iomem *)(addr + reg_offset));
}

#define read_CV() at91_tc_read(AT91_TC_CV)
#define read_RC() at91_tc_read(AT91_TC_RC)
#define write_RC(value) at91_tc_write(AT91_TC_RC, value)

int __ipipe_mach_timerint = KERNEL_TIMER_IRQ_NUM;
EXPORT_SYMBOL(__ipipe_mach_timerint);

int __ipipe_mach_timerstolen = 0;
EXPORT_SYMBOL(__ipipe_mach_timerstolen);

unsigned int __ipipe_mach_ticks_per_jiffy = LATCH;
EXPORT_SYMBOL(__ipipe_mach_ticks_per_jiffy);

/*
 * IRQ handler for the timer.
 */
static irqreturn_t at91_timer_interrupt(int irq, void *dev_id)
{
	clkevt.event_handler(&clkevt);
	return IRQ_HANDLED;
}

static struct irqaction at91_timer_irq = {
	.name		= "at91_tick",
	.flags		= IRQF_DISABLED | IRQF_TIMER,
	.handler	= &at91_timer_interrupt
};

void __ipipe_mach_acktimer(void)
{
	at91_tc_read(AT91_TC_SR);

	if (unlikely(!__ipipe_mach_timerstolen)) {
		__ipipe_tsc_update();
		next_match = (next_match + __ipipe_mach_ticks_per_jiffy)
			& AT91_TC_REG_MASK;
		write_RC(next_match);
	}
}

static void
at91_tc_set_mode(enum clock_event_mode mode, struct clock_event_device *dev)
{
	/* Disable the channel */
	at91_tc_write(AT91_TC_CCR, AT91_TC_CLKDIS);

	/* Disable all interrupts. */
	at91_tc_write(AT91_TC_IDR, ~0ul);

	if (mode == CLOCK_EVT_MODE_PERIODIC) {
		unsigned long v;

#ifndef CONFIG_ARCH_AT91SAM9263
		clk_enable(clk_get(NULL, "tc"
				   __stringify(CONFIG_IPIPE_AT91_TC) "_clk"));
#else /* AT91SAM9263 */
		clk_enable(clk_get(NULL, "tcb_clk"));
#endif /* AT91SAM9263 */

		/* No Sync. */
		at91_tc_write(AT91_TC_BCR, 0);

		/* program NO signal on XCN */
		v = readl((void __iomem *) (AT91_VA_BASE_TCB0 + AT91_TC_BMR));
		v &= ~TCNXCNS(CONFIG_IPIPE_AT91_TC, 3);
		v |= TCNXCNS(CONFIG_IPIPE_AT91_TC, 1); /* AT91_TC_TCNXCNS_NONE */
		writel(v, (void __iomem *) (AT91_VA_BASE_TCB0 + AT91_TC_BMR));

		/* Use the clock selected by at91_timer_init as input clock. */
		at91_tc_write(AT91_TC_CMR, tc_timer_clock);

		/* Load the TC register C. */
		next_match = __ipipe_mach_ticks_per_jiffy;
		write_RC(next_match);

		/* Enable CPCS interrupt. */
		at91_tc_write(AT91_TC_IER, AT91_TC_CPCS);

		/* Enable the channel. */
		at91_tc_write(AT91_TC_CCR, AT91_TC_CLKEN | AT91_TC_SWTRG);
	}
}

/*
 * Reprogram the timer
 */
void __ipipe_mach_set_dec(unsigned long delay)
{
	if (delay > max_delta_ticks)
		delay = max_delta_ticks;

	if (likely(delay > min_delta_ticks)) {
		write_RC((read_CV() + delay) & AT91_TC_REG_MASK);
		__ipipe_tsc_update();
	} else
		ipipe_trigger_irq(KERNEL_TIMER_IRQ_NUM);
}
EXPORT_SYMBOL(__ipipe_mach_set_dec);

int __ipipe_check_tickdev(const char *devname)
{
	return !strcmp(devname, clkevt.name);
}

static struct clock_event_device clkevt = {
	.name		= "at91_tc" __stringify(CONFIG_IPIPE_AT91_TC),
	.features	= CLOCK_EVT_FEAT_PERIODIC,
	.shift		= 20,
	.rating		= 250,
	.set_mode	= at91_tc_set_mode,
};

static struct __ipipe_tscinfo tsc_info = {
	.type = IPIPE_TSC_TYPE_FREERUNNING,
	.counter_vaddr = (AT91_VA_BASE_TCB0 +
			  0x40 * CONFIG_IPIPE_AT91_TC + AT91_TC_CV),
	.u = {
		{
			.counter_paddr = (AT91_BASE_TCB0 +
					  0x40 * CONFIG_IPIPE_AT91_TC +
					  AT91_TC_CV),
			.mask = AT91_TC_REG_MASK,
		},
	},
};

void __ipipe_mach_release_timer(void)
{
	unsigned long flags;
	local_irq_save_hw(flags);
	__ipipe_mach_set_dec(__ipipe_mach_ticks_per_jiffy);
	local_irq_restore_hw(flags);
}
EXPORT_SYMBOL(__ipipe_mach_release_timer);

unsigned long __ipipe_mach_get_dec(void)
{
	return (read_RC() - read_CV()) & AT91_TC_REG_MASK;
}

void __init at91_timer_init(void)
{
	unsigned char tc_divisors[] = { 2, 8, 32, 128, 0, };
	unsigned master_freq, divisor = 0, divided_freq = 0;
	unsigned long long wrap_ns;

	/* Disable (boot loader) timer interrupts. */
#if defined(CONFIG_ARCH_AT91RM9200)
	at91_sys_write(AT91_ST_IDR, AT91_ST_PITS | AT91_ST_WDOVF | AT91_ST_RTTINC | AT91_ST_ALMS);
	(void) at91_sys_read(AT91_ST_SR);	/* Clear any pending interrupts */
#elif defined(CONFIG_ARCH_AT91SAM9260) || defined(CONFIG_ARCH_AT91SAM9261) \
	|| defined(CONFIG_ARCH_AT91SAM9263) || defined(CONFIG_ARCH_AT91SAM9RL) \
	|| defined(CONFIG_ARCH_AT91SAM9G20)
	at91_sys_write(AT91_PIT_MR, 0);

	/* Clear any pending interrupts */
	(void) at91_sys_read(AT91_PIT_PIVR);
#endif /* CONFIG_ARCH_AT91SAM926x */

	master_freq = clk_get_rate(clk_get(NULL, "mck"));
	/* Find the first frequency above 1 MHz */
	for (tc_timer_clock = ARRAY_SIZE(tc_divisors) - 1;
	     tc_timer_clock >= 0; tc_timer_clock--) {
		divisor = tc_divisors[tc_timer_clock];
		divided_freq = (divisor
				? master_freq / divisor : AT91_SLOW_CLOCK);
		if (divided_freq > 1000000)
			break;
	}

	wrap_ns = (unsigned long long) (AT91_TC_REG_MASK + 1) * NSEC_PER_SEC;
	do_div(wrap_ns, divided_freq);

	if (divided_freq < 1000000)
		printk(KERN_INFO "AT91 I-pipe warning: could not find a"
		       " frequency greater than 1MHz\n");

	printk(KERN_INFO "AT91 I-pipe timer: div: %u, freq: %u.%06u MHz, wrap: "
	       "%u.%06u ms\n", divisor,
	       divided_freq / 1000000, divided_freq % 1000000,
	       (unsigned) wrap_ns / 1000000, (unsigned) wrap_ns % 1000000);

	/* Add a 1ms margin. It means that when an interrupt occurs, update_tsc
	   must be called within 1ms. update_tsc is called by acktimer when no
	   higher domain handles the timer, and called through set_dec when a
	   higher domain handles the timer. */
	wrap_ns -= 1000000;
	/* Set up the interrupt. */
	setup_irq(KERNEL_TIMER_IRQ_NUM, &at91_timer_irq);

	clkevt.mult = div_sc(divided_freq, NSEC_PER_SEC, clkevt.shift);
	clkevt.max_delta_ns = wrap_ns;
	clkevt.min_delta_ns = 2000;
	clkevt.cpumask = cpumask_of(0);
	clockevents_register_device(&clkevt);

	tsc_info.freq = divided_freq;
	__ipipe_tsc_register(&tsc_info);

	__ipipe_mach_ticks_per_jiffy = (divided_freq + HZ/2) / HZ;
	max_delta_ticks = (wrap_ns * clkevt.mult) >> clkevt.shift;
	min_delta_ticks = ((unsigned long long) clkevt.min_delta_ns
			   * clkevt.mult) >> clkevt.shift;
}

#ifdef CONFIG_ARCH_AT91RM9200
struct sys_timer at91rm9200_timer = {
#elif defined(CONFIG_ARCH_AT91SAM9260) || defined(CONFIG_ARCH_AT91SAM9261) \
	|| defined(CONFIG_ARCH_AT91SAM9263) || defined(CONFIG_ARCH_AT91SAM9RL) \
	|| defined(CONFIG_ARCH_AT91SAM9G20)
struct sys_timer at91sam926x_timer = {
#elif defined(CONFIG_ARCH_AT91X40)
struct sys_timer at91x40_timer = {
#else
#error "Unknown machine"
#endif
	.init		= at91_timer_init,
	.suspend	= NULL,
	.resume		= NULL,
};
