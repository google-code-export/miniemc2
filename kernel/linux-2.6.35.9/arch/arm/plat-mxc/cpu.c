#include <linux/module.h>
#include <linux/io.h>
#include <mach/hardware.h>

unsigned int __mxc_cpu_type;
EXPORT_SYMBOL(__mxc_cpu_type);

void mxc_set_cpu_type(unsigned int type)
{
	__mxc_cpu_type = type;
}

#ifdef CONFIG_IPIPE
void ipipe_mach_allow_hwtimer_uaccess(unsigned long aips1, unsigned long aips2)
{
	volatile unsigned long aips_reg;

	if (!cpu_is_mx27()) {
		/*
		 * S/W workaround: Clear the off platform peripheral modules
		 * Supervisor Protect bit for SDMA to access them.
		 */
		__raw_writel(0x0, aips1 + 0x40);
		__raw_writel(0x0, aips1 + 0x44);
		__raw_writel(0x0, aips1 + 0x48);
		__raw_writel(0x0, aips1 + 0x4C);
		aips_reg = __raw_readl(aips1 + 0x50);
		aips_reg &= 0x00FFFFFF;
		__raw_writel(aips_reg, aips1 + 0x50);

		__raw_writel(0x0, aips2 + 0x40);
		__raw_writel(0x0, aips2 + 0x44);
		__raw_writel(0x0, aips2 + 0x48);
		__raw_writel(0x0, aips2 + 0x4C);
		aips_reg = __raw_readl(aips2 + 0x50);
		aips_reg &= 0x00FFFFFF;
		__raw_writel(aips_reg, aips2 + 0x50);
	} else {
		aips_reg = __raw_readl(aips1 + 8);
		aips_reg &= ~(1 << aips2);
		__raw_writel(aips_reg, aips1 + 8);
	}
}
#endif /* CONFIG_IPIPE */
