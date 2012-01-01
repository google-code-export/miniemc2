/* linux/arch/arm/plat-samsung/time.c
 *
 * Copyright (C) 2003-2005 Simtec Electronics
 *	Ben Dooks, <ben@simtec.co.uk>
 *
 * Copyright (C) 2006, 2007 Sebastian Smolorz <ssmolorz@emlix.com>, emlix GmbH
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/module.h>

#include <asm/system.h>
#include <asm/leds.h>
#include <asm/mach-types.h>

#include <asm/irq.h>
#include <mach/map.h>
#include <plat/regs-timer.h>
#include <mach/regs-irq.h>
#include <asm/mach/time.h>
#include <mach/tick.h>

#include <plat/clock.h>
#include <plat/cpu.h>

static unsigned long timer_usec_ticks;

#ifndef TICK_MAX
#define TICK_MAX (0xffff)
#endif

#define TIMER_USEC_SHIFT 16

/* we use the shifted arithmetic to work out the ratio of timer ticks
 * to usecs, as often the peripheral clock is not a nice even multiple
 * of 1MHz.
 *
 * shift of 14 and 15 are too low for the 12MHz, 16 seems to be ok
 * for the current HZ value of 200 without producing overflows.
 *
 * Original patch by Dimitry Andric, updated by Ben Dooks
*/

static unsigned long last_free_running_tcnt = 0;
static unsigned long free_running_tcon = 0;
static unsigned long timer_lxlost = 0;

#ifdef CONFIG_IPIPE
unsigned int __ipipe_mach_ticks_per_jiffy;
EXPORT_SYMBOL(__ipipe_mach_ticks_per_jiffy);

int __ipipe_mach_timerint = IRQ_TIMER4;
EXPORT_SYMBOL(__ipipe_mach_timerint);

static unsigned long timer_ackval = 1UL << (IRQ_TIMER4 - IRQ_EINT0);

static struct __ipipe_tscinfo tsc_info = {
	.type = IPIPE_TSC_TYPE_DECREMENTER,
	.counter_vaddr = (unsigned long)S3C2410_TCNTO(3),
	.u = {
		{
			.counter_paddr = 0x51000038UL,
			.mask = 0xffff,
		},
	},
};

static IPIPE_DEFINE_SPINLOCK(timer_lock);

int __ipipe_mach_timerstolen = 0;
EXPORT_SYMBOL(__ipipe_mach_timerstolen);
#endif /* CONFIG_IPIPE */

/* timer_mask_usec_ticks
 *
 * given a clock and divisor, make the value to pass into timer_ticks_to_usec
 * to scale the ticks into usecs
*/

static inline unsigned long
timer_mask_usec_ticks(unsigned long scaler, unsigned long pclk)
{
	unsigned long den = pclk / 1000;

	return ((1000 << TIMER_USEC_SHIFT) * scaler + (den >> 1)) / den;
}

/* timer_ticks_to_usec
 *
 * convert timer ticks to usec.
*/

static inline unsigned long timer_ticks_to_usec(unsigned long ticks)
{
	unsigned long res;

	res = ticks * timer_usec_ticks;
	res += 1 << (TIMER_USEC_SHIFT - 4);	/* round up slightly */

	return res >> TIMER_USEC_SHIFT;
}

static inline unsigned long timer_freerunning_getvalue(void)
{
	return __raw_readl(S3C2410_TCNTO(3));
}

static inline unsigned long timer_freerunning_getticksoffset(unsigned long tval)
{
	long tdone;

	tdone =  last_free_running_tcnt - tval;
	if (tdone < 0)
		tdone += 0x10000;

	return tdone;
}

static inline unsigned long getticksoffset(void)
{
	return timer_freerunning_getticksoffset(timer_freerunning_getvalue());
}

#ifdef CONFIG_IPIPE
static inline unsigned long getticksoffset_tscupdate(void)
{
	unsigned long tval;
	unsigned long ticks;

	tval = timer_freerunning_getvalue();
	ticks = timer_freerunning_getticksoffset(tval);
	last_free_running_tcnt = tval;
	__ipipe_tsc_update();
	return ticks;
}
#else
static unsigned long s3c2410_gettimeoffset (void)
{
	return timer_ticks_to_usec(timer_lxlost + getticksoffset());
}
#endif /* CONFIG_IPIPE */

/*
 * IRQ handler for the timer
 */
static irqreturn_t
s3c2410_timer_interrupt(int irq, void *dev_id)
{
#ifdef CONFIG_IPIPE
	timer_lxlost = 0;

	if (!__ipipe_mach_timerstolen) {
		spin_lock(&timer_lock);
		getticksoffset_tscupdate();
		spin_unlock(&timer_lock);
	}
#endif /* CONFIG_IPIPE */

	timer_tick();
	return IRQ_HANDLED;
}

static struct irqaction s3c2410_timer_irq = {
	.name		= "S3C2410 Timer Tick",
	.flags		= IRQF_DISABLED | IRQF_TIMER | IRQF_IRQPOLL,
	.handler	= s3c2410_timer_interrupt,
};

#define use_tclk1_12() ( \
	machine_is_bast()	|| \
	machine_is_vr1000()	|| \
	machine_is_anubis()	|| \
	machine_is_osiris())

static struct clk *tin;
static struct clk *tdiv;
static struct clk *timerclk;

/*
 * Set up timer interrupt.
 *
 * Currently we use timer4 as event timer and timer3 as tick counter which
 * permanently counts ticks without interrupt generation.
 */
static void s3c2410_timer_setup (void)
{
	unsigned long tcon;
	unsigned long tcnt;
	unsigned long tcfg1;
	unsigned long tcfg0;
	unsigned long intmask;

	tcnt = TICK_MAX;  /* default value for tcnt */

	/* configure the system for whichever machine is in use */

	if (use_tclk1_12()) {
		/* timer is at 12MHz, scaler is 1 */
		timer_usec_ticks = timer_mask_usec_ticks(1, 12000000);
		tcnt = 12000000 / HZ;

		tcfg1 = __raw_readl(S3C2410_TCFG1);
		tcfg1 &= ~(S3C2410_TCFG1_MUX4_MASK | S3C2410_TCFG1_MUX3_MASK);
		tcfg1 |= (S3C2410_TCFG1_MUX4_TCLK1 | S3C2410_TCFG1_MUX3_TCLK1);
		__raw_writel(tcfg1, S3C2410_TCFG1);
	} else {
		unsigned long pclk;
		struct clk *tscaler;

		/* for the h1940 (and others), we use the pclk from the core
		 * to generate the timer values. since values around 50 to
		 * 70MHz are not values we can directly generate the timer
		 * value from, we need to pre-scale and divide before using it.
		 *
		 * for instance, using 50.7MHz and dividing by 6 gives 8.45MHz
		 * (8.45 ticks per usec)
		 */

		pclk = clk_get_rate(timerclk);

		/* configure clock tick */

		timer_usec_ticks = timer_mask_usec_ticks(6, pclk);

		tscaler = clk_get_parent(tdiv);

		clk_set_rate(tscaler, pclk / 3);
		clk_set_rate(tdiv, pclk / 6);
		clk_set_parent(tin, tdiv);

		tcnt = clk_get_rate(tin) / HZ;
	}

	tcon = __raw_readl(S3C2410_TCON);
	tcfg0 = __raw_readl(S3C2410_TCFG0);
	tcfg1 = __raw_readl(S3C2410_TCFG1);

#ifdef CONFIG_IPIPE
	__ipipe_mach_ticks_per_jiffy = tcnt;
	tsc_info.freq = tcnt * HZ;
	__ipipe_tsc_register(&tsc_info);
#endif /* CONFIG_IPIPE */

	/* timers reload after counting zero, so reduce the count by 1 */

	tcnt--;

	printk(KERN_DEBUG "timer tcon=%08lx, tcnt %04lx, tcfg %08lx,%08lx, usec %08lx\n",
	       tcon, tcnt, tcfg0, tcfg1, timer_usec_ticks);

	/* check to see if timer is within 16bit range... */
	if (tcnt > TICK_MAX) {
		panic("setup_timer: HZ is too small, cannot configure timer!");
		return;
	}

	__raw_writel(tcfg1, S3C2410_TCFG1);
	__raw_writel(tcfg0, S3C2410_TCFG0);

	/* ensure timers are stopped... */
	tcon &= ~(0x3f<<17);
	__raw_writel(tcon, S3C2410_TCON);

	/* Mask timer3 interrupt. */
	intmask = __raw_readl(S3C2410_INTMSK);
	intmask |= 1UL << (IRQ_TIMER3 - IRQ_EINT0);
	__raw_writel(intmask, S3C2410_INTMSK);

	/* Set timer values */
	__raw_writel(tcnt, S3C2410_TCNTB(4));
	__raw_writel(tcnt, S3C2410_TCMPB(4));
	__raw_writel(0xffff, S3C2410_TCNTB(3));
	__raw_writel(0xffff, S3C2410_TCMPB(3));

	/* Set base tcon value for later programming of timer 4 by Xenomai. */
	free_running_tcon = tcon |  S3C2410_TCON_T3RELOAD | S3C2410_TCON_T3START;

	/* Set auto reloads for both timers. */
	tcon |= S3C2410_TCON_T3RELOAD | S3C2410_TCON_T4RELOAD;

	/* Manual update */
	__raw_writel(tcon | S3C2410_TCON_T3MANUALUPD
			  | S3C2410_TCON_T4MANUALUPD, S3C2410_TCON);

	tcon |= S3C2410_TCON_T3START | S3C2410_TCON_T4START;
	/* Start timers.*/
	__raw_writel(tcon, S3C2410_TCON);

	/* Save start value of timer 3 as begining of first period. */
	last_free_running_tcnt = 0xffff;
}

static void __init s3c2410_timer_resources(void)
{
	struct platform_device tmpdev;

	tmpdev.dev.bus = &platform_bus_type;
	tmpdev.id = 4;

	timerclk = clk_get(NULL, "timers");
	if (IS_ERR(timerclk))
		panic("failed to get clock for system timer");

	clk_enable(timerclk);

	if (!use_tclk1_12()) {
		tin = clk_get(&tmpdev.dev, "pwm-tin");
		if (IS_ERR(tin))
			panic("failed to get pwm-tin clock for system timer");

		tdiv = clk_get(&tmpdev.dev, "pwm-tdiv");
		if (IS_ERR(tdiv))
			panic("failed to get pwm-tdiv clock for system timer");
	}

	clk_enable(tin);
}

static void __init s3c2410_timer_init(void)
{
	s3c2410_timer_resources();
	s3c2410_timer_setup();
	setup_irq(IRQ_TIMER4, &s3c2410_timer_irq);
}

struct sys_timer s3c24xx_timer = {
	.init		= s3c2410_timer_init,
#ifndef CONFIG_IPIPE
	.offset		= s3c2410_gettimeoffset,
#endif
	.resume		= s3c2410_timer_setup
};

#ifdef CONFIG_IPIPE
void __ipipe_mach_acktimer(void)
{
	__raw_writel(timer_ackval, S3C2410_SRCPND);
	__raw_writel(timer_ackval, S3C2410_INTPND);
}

static inline void set_dec(unsigned long reload)
{
	/* Don't modify T0-T2 settings */
	int tcon = __raw_readl(S3C2410_TCON) & 0xFFFF;
	__raw_writel(reload, S3C2410_TCNTB(4));
	/* Manual update */
	__raw_writel(tcon | free_running_tcon | S3C2410_TCON_T4MANUALUPD, S3C2410_TCON);
	/* Start timer */
	__raw_writel(tcon | free_running_tcon | S3C2410_TCON_T4START, S3C2410_TCON);
}

void __ipipe_mach_set_dec(unsigned long reload)
{
	unsigned long flags;

	spin_lock_irqsave(&timer_lock, flags);
	timer_lxlost += getticksoffset_tscupdate();
	set_dec(reload);
	spin_unlock_irqrestore(&timer_lock, flags);
}
EXPORT_SYMBOL(__ipipe_mach_set_dec);

void __ipipe_mach_release_timer(void)
{
	free_running_tcon |= S3C2410_TCON_T4RELOAD;
	__ipipe_mach_set_dec(__ipipe_mach_ticks_per_jiffy - 1);
	free_running_tcon &= ~S3C2410_TCON_T4RELOAD;
}
EXPORT_SYMBOL(__ipipe_mach_release_timer);

unsigned long __ipipe_mach_get_dec(void)
{
	return __raw_readl(S3C2410_TCNTO(4));
}
#endif /* CONFIG_IPIPE */
