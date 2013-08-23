/*
 * evm.c
 *
 * Copyright (C) 2011 Texas Instruments Incorporated - http://www.ti.com/
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <common.h>
#include <asm/cache.h>
#include <asm/omap_common.h>
#include <asm/io.h>
#include <asm/arch/cpu.h>
#include <asm/arch/ddr_defs.h>
#include <asm/arch/hardware.h>
#include <asm/arch/mmc_host_def.h>
#include <asm/arch/sys_proto.h>
#include <asm/arch/mem.h>
#include <asm/arch/nand.h>
#include <asm/arch/clock.h>
#include <linux/mtd/nand.h>
#include <nand.h>
#include <net.h>
#include <miiphy.h>
#include <netdev.h>
#include <spi_flash.h>
#include "common_def.h"
#include "pmic.h"
#include <i2c.h>
#include <serial.h>

DECLARE_GLOBAL_DATA_PTR;

/* UART Defines */
#define UART_SYSCFG_OFFSET	(0x54)
#define UART_SYSSTS_OFFSET	(0x58)

#define UART_RESET		(0x1 << 1)
#define UART_CLK_RUNNING_MASK	0x1
#define UART_SMART_IDLE_EN	(0x1 << 0x3)

/* Timer Defines */
#define TSICR_REG		0x54
#define TIOCP_CFG_REG		0x10
#define TCLR_REG		0x38

/* DDR defines */
#define MDDR_SEL_DDR2		0xefffffff		/* IOs set for DDR2-STL Mode */
#define CKE_NORMAL_OP		0x00000001		/* Normal Op:CKE controlled by EMIF */
#define GATELVL_INIT_MODE_SEL	0x1	/* Selects a starting ratio value based
					on DATA0/1_REG_PHY_GATELVL_INIT_RATIO_0
					value programmed by the user */
#define WRLVL_INIT_MODE_SEL	0x1	/* Selects a starting ratio value based
					on DATA0/1_REG_PHY_WRLVL_INIT_RATIO_0
					value programmed by the user */
#define NO_OF_MAC_ADDR          3
#define ETH_ALEN				6

struct am335x_baseboard_id {
	unsigned int  magic;
	char name[8];
	char version[4];
	char serial[12];
	char config[32];
	char mac_addr[NO_OF_MAC_ADDR][ETH_ALEN];
};

static struct am335x_baseboard_id header;

/*
 * dram_init:
 * At this point we have initialized the i2c bus and can read the
 * EEPROM which will tell us what board and revision we are on.
 */
int dram_init(void)
{
	gd->ram_size = PHYS_DRAM_1_SIZE;
/*	gd->ram_size = get_ram_size(
			(void *)CONFIG_SYS_SDRAM_BASE,
			CONFIG_MAX_RAM_BANK_SIZE);*/

	return 0;
}

void dram_init_banksize (void)
{
	/* Fill up board info */
	gd->bd->bi_dram[0].start = PHYS_DRAM_1;
	gd->bd->bi_dram[0].size = PHYS_DRAM_1_SIZE;
}

#ifdef CONFIG_SPL_BUILD
static void config_vtp(void)
{
	__raw_writel(__raw_readl(VTP0_CTRL_REG) | VTP_CTRL_ENABLE,
			VTP0_CTRL_REG);
	__raw_writel(__raw_readl(VTP0_CTRL_REG) & (~VTP_CTRL_START_EN),
			VTP0_CTRL_REG);
	__raw_writel(__raw_readl(VTP0_CTRL_REG) | VTP_CTRL_START_EN,
			VTP0_CTRL_REG);

	/* Poll for READY */
	while ((__raw_readl(VTP0_CTRL_REG) & VTP_CTRL_READY) != VTP_CTRL_READY);
}

static void phy_config_cmd(void)
{
	writel(DDR3_RATIO, CMD0_CTRL_SLAVE_RATIO_0);
	writel(DDR3_INVERT_CLKOUT, CMD0_INVERT_CLKOUT_0);
	writel(DDR3_RATIO, CMD1_CTRL_SLAVE_RATIO_0);
	writel(DDR3_INVERT_CLKOUT, CMD1_INVERT_CLKOUT_0);
	writel(DDR3_RATIO, CMD2_CTRL_SLAVE_RATIO_0);
	writel(DDR3_INVERT_CLKOUT, CMD2_INVERT_CLKOUT_0);
}

static void phy_config_data(void)
{

	writel(DDR3_RD_DQS, DATA0_RD_DQS_SLAVE_RATIO_0);
	writel(DDR3_WR_DQS, DATA0_WR_DQS_SLAVE_RATIO_0);
	writel(DDR3_PHY_FIFO_WE, DATA0_FIFO_WE_SLAVE_RATIO_0);
	writel(DDR3_PHY_WR_DATA, DATA0_WR_DATA_SLAVE_RATIO_0);

	writel(DDR3_RD_DQS, DATA1_RD_DQS_SLAVE_RATIO_0);
	writel(DDR3_WR_DQS, DATA1_WR_DQS_SLAVE_RATIO_0);
	writel(DDR3_PHY_FIFO_WE, DATA1_FIFO_WE_SLAVE_RATIO_0);
	writel(DDR3_PHY_WR_DATA, DATA1_WR_DATA_SLAVE_RATIO_0);
}

static void config_emif_ddr3(void)
{
	/*Program EMIF0 CFG Registers*/
	writel(DDR3_EMIF_READ_LATENCY, EMIF4_0_DDR_PHY_CTRL_1);
	writel(DDR3_EMIF_READ_LATENCY, EMIF4_0_DDR_PHY_CTRL_1_SHADOW);
	writel(DDR3_EMIF_READ_LATENCY, EMIF4_0_DDR_PHY_CTRL_2);
	writel(DDR3_EMIF_TIM1, EMIF4_0_SDRAM_TIM_1);
	writel(DDR3_EMIF_TIM1, EMIF4_0_SDRAM_TIM_1_SHADOW);
	writel(DDR3_EMIF_TIM2, EMIF4_0_SDRAM_TIM_2);
	writel(DDR3_EMIF_TIM2, EMIF4_0_SDRAM_TIM_2_SHADOW);
	writel(DDR3_EMIF_TIM3, EMIF4_0_SDRAM_TIM_3);
	writel(DDR3_EMIF_TIM3, EMIF4_0_SDRAM_TIM_3_SHADOW);

	writel(DDR3_EMIF_SDREF, EMIF4_0_SDRAM_REF_CTRL);
	writel(DDR3_EMIF_SDREF, EMIF4_0_SDRAM_REF_CTRL_SHADOW);
	writel(DDR3_ZQ_CFG, EMIF0_0_ZQ_CONFIG);

	writel(DDR3_EMIF_SDCFG, EMIF4_0_SDRAM_CONFIG);

	/*
	 * Write contents of SDRAM_CONFIG to SECURE_EMIF_SDRAM_CONFIG.
	 * SDRAM_CONFIG will be reconfigured with this value during resume
	 */
	writel(DDR3_EMIF_SDCFG, SECURE_EMIF_SDRAM_CONFIG);

}

static void config_am335x_ddr3(void)
{
	enable_ddr3_clocks();

	config_vtp();

	phy_config_cmd();
	phy_config_data();

	/* set IO control registers */
	writel(DDR3_IOCTRL_VALUE, DDR_CMD0_IOCTRL);
	writel(DDR3_IOCTRL_VALUE, DDR_CMD1_IOCTRL);
	writel(DDR3_IOCTRL_VALUE, DDR_CMD2_IOCTRL);
	writel(DDR3_IOCTRL_VALUE, DDR_DATA0_IOCTRL);
	writel(DDR3_IOCTRL_VALUE, DDR_DATA1_IOCTRL);

	/* IOs set for DDR3 */
	writel(readl(DDR_IO_CTRL) & MDDR_SEL_DDR2, DDR_IO_CTRL);
	/* CKE controlled by EMIF/DDR_PHY */
	writel(readl(DDR_CKE_CTRL) | CKE_NORMAL_OP, DDR_CKE_CTRL);

	config_emif_ddr3();
}

static void init_timer(void)
{
	/* Reset the Timer */
	__raw_writel(0x2, (DM_TIMER2_BASE + TSICR_REG));

	/* Wait until the reset is done */
	while (__raw_readl(DM_TIMER2_BASE + TIOCP_CFG_REG) & 1);

	/* Start the Timer */
	__raw_writel(0x1, (DM_TIMER2_BASE + TCLR_REG));
}
#endif

#if defined(CONFIG_SPL_BUILD) && defined(CONFIG_SPL_BOARD_INIT)

void spl_board_init(void)
{
	/* Configure the i2c0 pin mux */
	enable_i2c0_pin_mux();
	mpu_pll_config(MPUPLL_M_720);
}

#endif

/*
 * early system init of muxing and clocks.
 */
void s_init(void)
{
	/* Can be removed as A8 comes up with L2 enabled */
	l2_cache_enable();

	/* WDT1 is already running when the bootloader gets control
	 * Disable it to avoid "random" resets
	 */
	__raw_writel(0xAAAA, WDT_WSPR);
	while(__raw_readl(WDT_WWPS) != 0x0);
	__raw_writel(0x5555, WDT_WSPR);
	while(__raw_readl(WDT_WWPS) != 0x0);
#ifdef CONFIG_SPL_BUILD
	/* Setup the PLLs and the clocks for the peripherals */
	pll_init();

	/* UART softreset */
	u32 regVal;
	u32 uart_base = DEFAULT_UART_BASE;

	enable_uart0_pin_mux();
	enable_nand_pin_mux();

	regVal = __raw_readl(uart_base + UART_SYSCFG_OFFSET);
	regVal |= UART_RESET;
	__raw_writel(regVal, (uart_base + UART_SYSCFG_OFFSET) );
	while ((__raw_readl(uart_base + UART_SYSSTS_OFFSET) &
			UART_CLK_RUNNING_MASK) != UART_CLK_RUNNING_MASK);

	/* Disable smart idle */
	regVal = __raw_readl((uart_base + UART_SYSCFG_OFFSET));
	regVal |= UART_SMART_IDLE_EN;
	__raw_writel(regVal, (uart_base + UART_SYSCFG_OFFSET));

	/* Initialize the Timer */
	init_timer();

	preloader_console_init();

	ddr_pll_config(303);
	config_am335x_ddr3();

#endif
}


/*
 * Basic board specific setup
 */
#ifndef CONFIG_SPL_BUILD
int board_evm_init(void)
{
	/* mach type passed to kernel */

	gd->bd->bi_arch_number = MACH_TYPE_TIAM335EVM;

	/* address of boot parameters */
	gd->bd->bi_boot_params = PHYS_DRAM_1 + 0x100;
#ifdef CONFIG_SMC911X
	enable_smscnet_pin_mux();
#endif
	return 0;
}
#endif

int board_init(void)
{
	/* Configure the i2c0 pin mux */
	enable_i2c0_pin_mux();

	i2c_init(CONFIG_SYS_I2C_SPEED, CONFIG_SYS_I2C_SLAVE);

#ifndef CONFIG_SPL_BUILD
	board_evm_init();
#endif
	gpmc_init();

	return 0;
}

int misc_init_r(void)
{
#ifdef DEBUG
	unsigned int cntr;
	unsigned char *valPtr;

	debug("EVM Configuration - ");
	debug("\tBoard id %x, profile %x, db %d\n", board_id, profile,
						daughter_board_connected);
	debug("Base Board EEPROM Data\n");
	valPtr = (unsigned char *)&header;
	for(cntr = 0; cntr < sizeof(header); cntr++) {
		if(cntr % 16 == 0)
			debug("\n0x%02x :", cntr);
		debug(" 0x%02x", (unsigned int)valPtr[cntr]);
	}
	debug("\n\n");

	debug("Board identification from EEPROM contents:\n");
	debug("\tBoard name   : %.8s\n", header.name);
	debug("\tBoard version: %.4s\n", header.version);
	debug("\tBoard serial : %.12s\n", header.serial);
	debug("\tBoard config : %.6s\n\n", header.config);
#endif
	return 0;
}

#ifdef BOARD_LATE_INIT
int board_late_init(void)
{
	return 0;
}
#endif

#if defined(CONFIG_SMC911X) || \
	(defined(CONFIG_USB_ETHER) && defined(CONFIG_MUSB_GADGET) && \
	(!defined(CONFIG_SPL_BUILD) || defined(CONFIG_SPL_USB_ETH_SUPPORT)))
int board_eth_init(bd_t *bis)
{
	int rv, n = 0;
#ifdef CONFIG_SMC911X
#define STR_ENV_ETHADDR	"ethaddr"

	struct eth_device *dev;
	uchar eth_addr[6];
	uint8_t mac_addr[6];
	uint32_t mac_hi, mac_lo;
	u_int32_t i;

	if (!eth_getenv_enetaddr("ethaddr", mac_addr)) {
		debug("<ethaddr> not set. Reading from E-fuse\n");
		/* try reading mac address from efuse */
		mac_lo = readl(MAC_ID0_LO);
		mac_hi = readl(MAC_ID0_HI);
		mac_addr[0] = mac_hi & 0xFF;
		mac_addr[1] = (mac_hi & 0xFF00) >> 8;
		mac_addr[2] = (mac_hi & 0xFF0000) >> 16;
		mac_addr[3] = (mac_hi & 0xFF000000) >> 24;
		mac_addr[4] = mac_lo & 0xFF;
		mac_addr[5] = (mac_lo & 0xFF00) >> 8;

		if (!is_valid_ether_addr(mac_addr)) {
			debug("Did not find a valid mac address in e-fuse. "
					"Trying the one present in EEPROM\n");

			for (i = 0; i < ETH_ALEN; i++)
				mac_addr[i] = header.mac_addr[0][i];
		}

		if (is_valid_ether_addr(mac_addr))
			eth_setenv_enetaddr("ethaddr", mac_addr);
		else {
			printf("Caution: Using hardcoded mac address. "
				"Set <ethaddr> variable to overcome this.\n");
		}
	}

	n = smc911x_initialize(0, CONFIG_SMC911X_BASE);

	if (!eth_getenv_enetaddr(STR_ENV_ETHADDR, eth_addr)) {
		dev = eth_get_dev_by_index(0);
		if (dev) {
			eth_setenv_enetaddr(STR_ENV_ETHADDR, dev->enetaddr);
		} else {
			printf("omap3evm: Couldn't get eth device\n");
			n = -1;
		}
	}
#endif
#if defined(CONFIG_USB_ETHER) && \
	(!defined(CONFIG_SPL_BUILD) || defined(CONFIG_SPL_USB_ETH_SUPPORT))
	rv = musb_register(&musb_plat, &musb_board_data, OTG_REGS_BASE);
	if (rv < 0) {
		printf("Error %d registering MUSB device\n", rv);
	} else {
		rv = usb_eth_initialize(bis);
		if (rv < 0)
			printf("Error %d registering USB_ETHER\n", rv);
		else
			n += rv;
	}
#endif
	return n;
}
#endif

#ifndef CONFIG_SPL_BUILD
#ifdef CONFIG_GENERIC_MMC
int board_mmc_init(bd_t *bis)
{
	enable_mmc0_pin_mux();
	omap_mmc_init(0);
	return 0;
}
#endif

#ifdef CONFIG_NAND_TI81XX
/******************************************************************************
 * Command to switch between NAND HW and SW ecc
 *****************************************************************************/
extern void ti81xx_nand_switch_ecc(nand_ecc_modes_t hardware, int32_t mode);
static int do_switch_ecc(cmd_tbl_t * cmdtp, int flag, int argc, char * const argv[])
{
	int type = 0;
	if (argc < 2)
		goto usage;

	if (strncmp(argv[1], "hw", 2) == 0) {
		if (argc == 3)
			type = simple_strtoul(argv[2], NULL, 10);
		ti81xx_nand_switch_ecc(NAND_ECC_HW, type);
	}
	else if (strncmp(argv[1], "sw", 2) == 0)
		ti81xx_nand_switch_ecc(NAND_ECC_SOFT, 0);
	else
		goto usage;

	return 0;

usage:
	printf("Usage: nandecc %s\n", cmdtp->usage);
	return 1;
}

U_BOOT_CMD(
	nandecc, 3, 1,	do_switch_ecc,
	"Switch NAND ECC calculation algorithm b/w hardware and software",
	"[sw|hw <hw_type>] \n"
	"   [sw|hw]- Switch b/w hardware(hw) & software(sw) ecc algorithm\n"
	"   hw_type- 0 for Hamming code\n"
	"            1 for bch4\n"
	"            2 for bch8\n"
	"            3 for bch16\n"
);

#endif /* CONFIG_NAND_TI81XX */
#endif /* CONFIG_SPL_BUILD */
