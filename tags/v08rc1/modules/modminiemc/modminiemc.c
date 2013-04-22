// Kernel Module

#include <linux/module.h>
#include <linux/init.h>
#include <linux/version.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/sched.h>
#include <asm/uaccess.h>
#include <asm/io.h>
#include <linux/io.h>
#include <linux/vmalloc.h>
#include <linux/mman.h>
#include <linux/slab.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
#include <linux/wrapper.h>
#endif

#include <linux/kernel.h>

#include <linux/device.h>
#include <mach/hardware.h>
#include <plat/regs-timer.h>
#include <mach/regs-irq.h>
#include <mach/regs-gpio.h>
#include <plat/map.h>
#include <linux/gpio.h>
#include <mach/fiq_ipc_mini2440.h>


#define MINIEMC_MAJOR 240
#define MINIEMC_NAME "miniemc"

static dma_addr_t fiq_bus_addr;

static void init_fiq_data( void )
{
    int i=0;
    if( pfiq_ipc_shared )
        memset(pfiq_ipc_shared, 0x00 ,sizeof(struct fiq_ipc_shared));
    // Set all axis to unconfigured state
    fiq_ipc_static.cycle_per_ms = 100;
    fiq_ipc_static.cycle_counter = 0;
    fiq_ipc_static.rb_size = RINGBUFF_SIZE;

    for(i=0; i < MAX_AXIS; i++)
    {
        fiq_ipc_static.axis[i].configured = 0;
    }
    fiq_ipc_static.scan_pin_num = -1;
}

static int miniemc_open (struct inode *inode, struct file *file) {
    printk("modminiemc open\n");
    init_fiq_data();
    return 0;
}

static int miniemc_release (struct inode *inode, struct file *file) {
    printk("%s\n", __func__ );
    return 0;
}

// ioctl - I/O control
struct fiq_ipc_static fst;

#ifndef CONFIG_MACH_MINI2416
static int miniemc_ioctl(struct inode *inode, struct file *file,
		unsigned int cmd, unsigned long arg)
#else
static long miniemc_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
#endif
{
	int retval = 0, i=0;
	switch ( cmd ) {
		case AXIS_SET_IOCTL:/* for configuring data */
			if (copy_from_user(&fst, (struct fiq_ipc_static *)arg, sizeof(struct fiq_ipc_static)))
                return -EFAULT;
    	    {
                fiq_ipc_static.rb_size = fst.rb_size;
                for( i = 0; i < MAX_PWM; i++ )
                {
                    fiq_ipc_static.pwm_pin_addr[i] = fst.pwm_pin_addr[i];
                    fiq_ipc_static.pwm_pin_mask[i] = fst.pwm_pin_mask[i];
                }

                for(i = 0; i < MAX_AXIS; i++)
                {
                    if( fst.axis[i].configured )
                    {
                        fiq_ipc_static.axis[i].step_pin_addr = fst.axis[i].step_pin_addr;
                        fiq_ipc_static.axis[i].dir_pin_addr = fst.axis[i].dir_pin_addr;
                        fiq_ipc_static.axis[i].step_pin_mask = fst.axis[i].step_pin_mask;
                        fiq_ipc_static.axis[i].dir_pin_mask = fst.axis[i].dir_pin_mask;
                        fiq_ipc_static.axis[i].dir_pin_pol = fst.axis[i].dir_pin_pol;
                        fiq_ipc_static.axis[i].configured = fst.axis[i].configured;
                    }
                }
                fiq_ipc_static.scan_pin_num = fst.scan_pin_num;
            }
	break;

        case SCAN_PIN_SETUP_IOCTL:
	    printk("fiq_static addr=%p\n", &fiq_ipc_static );
            printk("fifo size=%d\n", fiq_ipc_static.rb_size );
            for(i = 0; i < MAX_AXIS; i++)
            {
                printk("axis[%d].configured=%x\n", i, fiq_ipc_static.axis[i].configured );
                printk("axis[%d].step_pin_addr=%x\n", i, fiq_ipc_static.axis[i].step_pin_addr );
                printk("axis[%d].step_pin_mask=%x\n", i, fiq_ipc_static.axis[i].step_pin_mask );
                printk("axis[%d].dir_pin_addr=%x\n", i, fiq_ipc_static.axis[i].dir_pin_addr );
                printk("axis[%d].dir_pin_mask=%x\n", i, fiq_ipc_static.axis[i].dir_pin_mask );
                printk("axis[%d].dir_pin_pol=%x\n", i, fiq_ipc_static.axis[i].dir_pin_pol );
            }
            for(i = 0; i < MAX_PWM; i++)
            {
                 printk("pwm[%d].pin_addr=%x\n", i, fiq_ipc_static.pwm_pin_addr[i] );
                 printk("pwm[%d].pin_mask=%x\n", i, fiq_ipc_static.pwm_pin_mask[i] );
            }
            break;
	default:
		retval = -EINVAL;
	}
	return retval;
}

static int miniemc_mmap(struct file * filp, struct vm_area_struct * vma) {

    return dma_mmap_writecombine(NULL, vma, pfiq_ipc_shared, fiq_bus_addr,  sizeof(struct fiq_ipc_shared)+ 2 * PAGE_SIZE);
}

// define which file operations are supported
struct file_operations skeleton_fops = {
	.owner	    =	THIS_MODULE,
	.llseek	    =	NULL,
	.read       =   NULL,
	.write	    =	NULL,
	.readdir	=	NULL,
	.poll		=	NULL,
	.flush	    =	NULL,
	.fsync	    =	NULL,
	.fasync	    =	NULL,
	.lock		=	NULL,
#ifndef CONFIG_MACH_MINI2416
	.ioctl	    =	miniemc_ioctl,
#else
	.compat_ioctl	    =	miniemc_ioctl,
	.unlocked_ioctl	    =	miniemc_ioctl,
#endif
	.mmap		=	miniemc_mmap,
	.open		=	miniemc_open,
	.release	=	miniemc_release,

};

// initialize module
static int __init miniemc_init_module (void) {
    int i, msk, ack_mask = 1L << 12;

    printk("initializing module\n");
    i = register_chrdev (MINIEMC_MAJOR, MINIEMC_NAME, &skeleton_fops);
    if (i != 0) 
	return - EIO;
    pfiq_ipc_shared = dma_alloc_writecombine(NULL, sizeof(struct fiq_ipc_shared)+ 2 * PAGE_SIZE, &fiq_bus_addr, GFP_KERNEL);
    if (!pfiq_ipc_shared)
    {
	printk("kmalloc failed\n");
	return - ENOMEM;
    }
    init_fiq_data();
    // Starting timer FIQ's
    msk = __raw_readl(S3C2410_INTMSK);
    __raw_writel(msk & ~ack_mask, S3C2410_INTMSK );

    return 0;
}

// close and cleanup module
static void __exit miniemc_cleanup_module (void) {
    int msk;
    printk("%s: cleaning up\n", __func__ );
    // Stopping timer FIQ's
    msk = __raw_readl(S3C2410_INTMSK);
    __raw_writel(msk | (1L << 12), S3C2410_INTMSK );
    dma_free_writecombine(NULL, sizeof(struct fiq_ipc_shared)+ 2 * PAGE_SIZE, pfiq_ipc_shared, fiq_bus_addr);
    unregister_chrdev (MINIEMC_MAJOR, MINIEMC_NAME);
}

module_init(miniemc_init_module);
module_exit(miniemc_cleanup_module);
MODULE_AUTHOR("KSU");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Linux MiniEMC helper driver");

