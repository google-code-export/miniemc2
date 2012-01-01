/*
 *  linux/arch/arm/plat-versatile/timer-sp.c
 *
 *  Copyright (C) 1999 - 2003 ARM Limited
 *  Copyright (C) 2000 Deep Blue Solutions Ltd
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
#include <linux/clocksource.h>
#include <linux/clockchips.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/ipipe.h>

#include <asm/hardware/arm_timer.h>

#include <plat/timer-sp.h>

/*
 * These timers are currently always setup to be clocked at 1MHz.
 */
#define TIMER_FREQ_KHZ	(1000)
#define TIMER_RELOAD	(TIMER_FREQ_KHZ * 1000 / HZ)

static void __iomem *clksrc_base;

#ifdef CONFIG_IPIPE
int __ipipe_mach_timerint;
EXPORT_SYMBOL(__ipipe_mach_timerint);

int __ipipe_mach_timerstolen;
EXPORT_SYMBOL(__ipipe_mach_timerstolen);

unsigned int __ipipe_mach_ticks_per_jiffy;
EXPORT_SYMBOL(__ipipe_mach_ticks_per_jiffy);

static struct __ipipe_tscinfo tsc_info = {
	.type = IPIPE_TSC_TYPE_FREERUNNING_COUNTDOWN,
	.u = {
		{
			.mask = 0xffffffff,
		},
	},
};
#endif /* CONFIG_IPIPE */

static cycle_t sp804_read(struct clocksource *cs)
{
	return ~readl(clksrc_base + TIMER_VALUE);
}

static struct clocksource clocksource_sp804 = {
	.name		= "timer3",
	.rating		= 200,
	.read		= sp804_read,
	.mask		= CLOCKSOURCE_MASK(32),
	.shift		= 20,
	.flags		= CLOCK_SOURCE_IS_CONTINUOUS,
};

void __init sp804_clocksource_init(void __iomem *base, unsigned long phys)
{
	struct clocksource *cs = &clocksource_sp804;

	clksrc_base = base;

	/* setup timer 0 as free-running clocksource */
	writel(0, clksrc_base + TIMER_CTRL);
	writel(0xffffffff, clksrc_base + TIMER_LOAD);
	writel(0xffffffff, clksrc_base + TIMER_VALUE);
	writel(TIMER_CTRL_32BIT | TIMER_CTRL_ENABLE | TIMER_CTRL_PERIODIC,
		clksrc_base + TIMER_CTRL);

#ifdef CONFIG_IPIPE
	tsc_info.freq = TIMER_FREQ_KHZ * 1000;
	tsc_info.counter_vaddr = (unsigned long)base + TIMER_VALUE;
	tsc_info.u.counter_paddr = phys + TIMER_VALUE;
	__ipipe_tsc_register(&tsc_info);
#endif

	cs->mult = clocksource_khz2mult(TIMER_FREQ_KHZ, cs->shift);
	clocksource_register(cs);
}


static void __iomem *clkevt_base;

/*
 * IRQ handler for the timer
 */
static irqreturn_t sp804_timer_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *evt = dev_id;

#ifndef CONFIG_IPIPE
	/* clear the interrupt */
	writel(1, clkevt_base + TIMER_INTCLR);
#else /* CONFIG_IPIPE */
	__ipipe_tsc_update();
#endif /* CONFIG_IPIPE */

	evt->event_handler(evt);

	return IRQ_HANDLED;
}

static void sp804_set_mode(enum clock_event_mode mode,
	struct clock_event_device *evt)
{
	unsigned long ctrl = TIMER_CTRL_32BIT | TIMER_CTRL_IE;

	writel(ctrl, clkevt_base + TIMER_CTRL);

	switch (mode) {
	case CLOCK_EVT_MODE_PERIODIC:
		writel(TIMER_RELOAD, clkevt_base + TIMER_LOAD);
		ctrl |= TIMER_CTRL_PERIODIC | TIMER_CTRL_ENABLE;
		break;

	case CLOCK_EVT_MODE_ONESHOT:
		/* period set, and timer enabled in 'next_event' hook */
		ctrl |= TIMER_CTRL_ONESHOT;
		break;

	case CLOCK_EVT_MODE_UNUSED:
	case CLOCK_EVT_MODE_SHUTDOWN:
	default:
		break;
	}

	writel(ctrl, clkevt_base + TIMER_CTRL);
}

static int sp804_set_next_event(unsigned long next,
	struct clock_event_device *evt)
{
	unsigned long ctrl = readl(clkevt_base + TIMER_CTRL);

	writel(next, clkevt_base + TIMER_LOAD);
	writel(ctrl | TIMER_CTRL_ENABLE, clkevt_base + TIMER_CTRL);

	return 0;
}

static struct clock_event_device sp804_clockevent = {
	.name		= "timer0",
	.shift		= 32,
	.features       = CLOCK_EVT_FEAT_PERIODIC | CLOCK_EVT_FEAT_ONESHOT,
	.set_mode	= sp804_set_mode,
	.set_next_event	= sp804_set_next_event,
	.rating		= 300,
	.cpumask	= cpu_all_mask,
};

static struct irqaction sp804_timer_irq = {
	.name		= "timer",
	.flags		= IRQF_DISABLED | IRQF_TIMER | IRQF_IRQPOLL,
	.handler	= sp804_timer_interrupt,
	.dev_id		= &sp804_clockevent,
};

void __init sp804_clockevents_init(void __iomem *base, unsigned int timer_irq)
{
	struct clock_event_device *evt = &sp804_clockevent;

	clkevt_base = base;

	evt->irq = timer_irq;
	evt->mult = div_sc(TIMER_FREQ_KHZ, NSEC_PER_MSEC, evt->shift);
	evt->max_delta_ns = clockevent_delta2ns(0xffffffff, evt);
	evt->min_delta_ns = clockevent_delta2ns(0xf, evt);

#ifdef CONFIG_IPIPE
	__ipipe_mach_timerint = timer_irq;
	__ipipe_mach_ticks_per_jiffy = (TIMER_FREQ_KHZ * 1000 + HZ / 2) / HZ;
#endif /* CONFIG_IPIPE */

	setup_irq(timer_irq, &sp804_timer_irq);
	clockevents_register_device(evt);
}

#ifdef CONFIG_IPIPE
int __ipipe_check_tickdev(const char *devname)
{
	/* Keep compatibility with old patches */
	return !strcmp(devname, "TIMER1");
}

void __ipipe_mach_acktimer(void)
{
	writel(1, clkevt_base + TIMER_INTCLR);
}

/*
 * Reprogram the timer
 */

void __ipipe_mach_set_dec(unsigned long delay)
{
	if (delay > 0xf)
		sp804_set_next_event(delay, NULL);
	else
		ipipe_trigger_irq(__ipipe_mach_timerint);
}
EXPORT_SYMBOL(__ipipe_mach_set_dec);

void __ipipe_mach_release_timer(void)
{
	struct clock_event_device *ckdev = &sp804_clockevent;
	ckdev->set_mode(ckdev->mode, ckdev);
	if (ckdev->mode == CLOCK_EVT_MODE_ONESHOT)
		ckdev->set_next_event(__ipipe_mach_ticks_per_jiffy, ckdev);
}
EXPORT_SYMBOL(__ipipe_mach_release_timer);

unsigned long __ipipe_mach_get_dec(void)
{
	return ~readl(clkevt_base + TIMER_VALUE);
}
#endif /* CONFIG_IPIPE */
