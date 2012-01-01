#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/clocksource.h>
#include <linux/sched.h>

#include <linux/ipipe.h>

#include <asm/cacheflush.h>

typedef unsigned long long __ipipe_tsc_t(void);

extern __ipipe_tsc_t __ipipe_freerunning_32,
	__ipipe_freerunning_16,
	__ipipe_freerunning_countdown,
	__ipipe_decrementer_16;
extern unsigned long __ipipe_tsc_addr;

static struct __ipipe_tscinfo tsc_info;

static struct clocksource clksrc = {
	.name = "ipipe_tsc",
	.rating = 0x7fffffff,
	.read = (typeof(clksrc.read))__ipipe_tsc_get,
	.mask = CLOCKSOURCE_MASK(64),
	.flags = CLOCK_SOURCE_IS_CONTINUOUS,
};

struct ipipe_tsc_value_t {
	unsigned long long last_tsc;
	unsigned last_cnt;
};

struct ipipe_tsc_value_t *const ipipe_tsc_value =
	(struct ipipe_tsc_value_t *)&__ipipe_tsc_area[0];

void __ipipe_tsc_register(struct __ipipe_tscinfo *info)
{
	__ipipe_tsc_t *implem;
	unsigned long flags;

	switch(info->type) {
	case IPIPE_TSC_TYPE_FREERUNNING:
		switch(info->u.mask) {
		case 0xffff:
			implem = &__ipipe_freerunning_16;
			break;
		case 0xffffffff:
			implem = &__ipipe_freerunning_32;
			break;
		default:
			goto unimplemented;
		}
		break;

	case IPIPE_TSC_TYPE_DECREMENTER:
		if (info->u.mask != 0xffff)
			goto unimplemented;
		implem = &__ipipe_decrementer_16;
		break;

	case IPIPE_TSC_TYPE_FREERUNNING_COUNTDOWN:
		if (info->u.mask != 0xffffffff)
			goto unimplemented;
		implem = &__ipipe_freerunning_countdown;
		break;

	default:
	unimplemented:
		printk("I-pipel: Unimplemented tsc configuration, "
		       "type: %d, mask: 0x%08x\n", info->type, info->u.mask);
		BUG();
	}

	tsc_info = *info;
	__ipipe_tsc_addr = tsc_info.counter_vaddr;
	if (tsc_info.type == IPIPE_TSC_TYPE_DECREMENTER) {
		tsc_info.u.dec.last_cnt = &ipipe_tsc_value->last_cnt;
		tsc_info.u.dec.tsc = &ipipe_tsc_value->last_tsc;
	} else
		tsc_info.u.fr.tsc = &ipipe_tsc_value->last_tsc;

	local_irq_save_hw(flags);
	memcpy(__ipipe_tsc_area + 0x20, implem, 0x60);
	flush_icache_range((unsigned long)(__ipipe_tsc_area),
			   (unsigned long)(__ipipe_tsc_area + 0x80));
	local_irq_restore_hw(flags);

	clksrc.shift = fls(tsc_info.freq) - 1;
	clksrc.mult = clocksource_hz2mult(tsc_info.freq, clksrc.shift);
	printk(KERN_INFO "I-pipe, %u.%03u MHz clocksource\n",
	       tsc_info.freq / 1000000, (tsc_info.freq % 1000000) / 1000);
	clocksource_register(&clksrc);
}

void __ipipe_mach_get_tscinfo(struct __ipipe_tscinfo *info)
{
	*info = tsc_info;
}

void __ipipe_tsc_update(void)
{
	if (tsc_info.type == IPIPE_TSC_TYPE_DECREMENTER) {
		unsigned cnt = *(unsigned *)tsc_info.counter_vaddr;
		int offset = ipipe_tsc_value->last_cnt - cnt;
		if (offset < 0)
			offset += 0x10000;
		ipipe_tsc_value->last_tsc += offset;
		ipipe_tsc_value->last_cnt = cnt;
		return;
	}
	ipipe_tsc_value->last_tsc = __ipipe_tsc_get() - 1;
}
EXPORT_SYMBOL(__ipipe_tsc_get);
