/*
 * (C) Copyright 2002
 * Sysgo Real-Time Solutions, GmbH <www.elinos.com>
 * Marius Groeger <mgroeger@sysgo.de>
 *
 * (C) Copyright 2002
 * David Mueller, ELSOFT AG, <d.mueller@elsoft.ch>
 * 
 * (C) Copyright 2009
 * Michel Pollet <buserror@gmail.com>
 *
 * See file CREDITS for list of people who contributed to this
 * project.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include <common.h>
#include <s3c2440.h>
#include <video_fb.h>

DECLARE_GLOBAL_DATA_PTR;

/* FCLK = 405 MHz, HCLK = 101 MHz, PCLK = 50 MHz, UCLK = 48 MHz */
#define CLKDIVN_VAL	5
#define M_MDIV		0x7f
#define M_PDIV		0x2
#define M_SDIV		0x1

#define U_M_MDIV	0x38
#define U_M_PDIV	0x2
#define U_M_SDIV	0x2

static inline void delay (unsigned long loops)
{
	__asm__ volatile ("1:\n"
	  "subs %0, %1, #1\n"
	  "bne 1b":"=r" (loops):"0" (loops));
}

/*
 * Miscellaneous platform dependent initialisations
 */

int board_init (void)
{
	S3C24X0_CLOCK_POWER * const clk_power = S3C24X0_GetBase_CLOCK_POWER();
	S3C24X0_GPIO * const gpio = S3C24X0_GetBase_GPIO();

	/* to reduce PLL lock time, adjust the LOCKTIME register */
	clk_power->LOCKTIME = 0xFFFFFF;
	clk_power->CLKDIVN = CLKDIVN_VAL;

	/* configure UPLL */
	clk_power->UPLLCON = ((U_M_MDIV << 12) + (U_M_PDIV << 4) + U_M_SDIV);
	/* some delay between MPLL and UPLL */
	delay (10);
	/* configure MPLL */
	clk_power->MPLLCON = ((M_MDIV << 12) + (M_PDIV << 4) + M_SDIV);

	/* some delay between MPLL and UPLL */
	delay (8000);

	gpio->GPACON = 0x007FFFFF;	/* Port A is all "special" */
	// port B outputs reconfigured
	gpio->GPBCON = 	
		(0x1 <<  0) | // GPB0	OUT	TOUT0		PWM Buzzer
		(0x2 <<  2) | // GPB1	OUT	TOUT1		LCD Backlight
		(0x1 <<  4) | // GPB2	OUT	L3MODE
		(0x1 <<  6) | // GBP3	OUT	L3DATA
		(0x1 <<  8) | // GBP4	OUT	L3CLOCK
		(0x1 << 10) | // GBP5	OUT	LED1
		(0x1 << 12) | // GBP6	OUT	LED2
		(0x1 << 14) | // GBP7	OUT	LED3
		(0x1 << 16) | // GBP8	OUT	LED4
		(0x2 << 18) | // GBP9	---	nXDACK0		CON5 EBI
		(0x2 << 20) | // GBP10	---	nXDREQ0		CON5 EBI
		0;
	gpio->GPBUP	= (1 << 10) - 1; // disable pullup on all 10 pins
	gpio->GPBDAT	= 	
		(0 << 5) | /* turn LED 1 on */
		(1 << 6) | /* turn LED 1 off */
		(1 << 7) | /* turn LED 1 off */
		(1 << 8) | /* turn LED 1 off */
		0;

	// lcd signals on C and D
	gpio->GPCCON	= (0xAAAAAAAA &	/* all default IN but ... */
				~(0x3 << 10)) |	/* not pin 5 ... */
				(0x1 << 10);	/* that is output (USBD) */
	gpio->GPCUP	= 0xFFFFFFFF;
	gpio->GPCDAT	= 0;
	
	gpio->GPDCON	= 0xAAAAAAAA;
	gpio->GPDUP	= 0xFFFFFFFF;
	// port E is set for all it's special functions (i2c, spi etc)
    	gpio->GPECON 	= 0xAAAAAAAA;
	gpio->GPEUP	= 0x0000FFFF;

	gpio->GPFCON 	= 
		(0x1 <<  0) | // GPG0	EINT0	OUT
		(0x1 <<  2) | // GPG1	EINT1	OUT
		(0x1 <<  4) | // GPG2	EINT2	OUT
		(0x1 <<  6) | // GPG3	EINT3	OUT
		(0x1 <<  8) | // GPG4	EINT4	OUT
		(0x1 << 10) | // GPG5	EINT5	OUT
		(0x1 << 12) | // GPG6	EINT6	OUT
		(0x0 << 14) | // GPG7	EINT7	IN	DM9000
		0;
	gpio->GPFDAT	= 0;
	gpio->GPFUP	= 
		((1 << 7) - 1) // all disabled
		& ~( 1 << 7 ) // but for the ethernet one, we need it.
		;

	gpio->GPGCON 	=
		(0x0 <<  0) | // GPG0	EINT8	IN	Key1
		(0x1 <<  2) | // GPG1	EINT9	OUT		Con5
		(0x1 <<  4) | // GPG2	EINT10	OUT
		(0x0 <<  6) | // GPG3	EINT11	IN	Key2
		(0x0 <<  8) | // GPG4	EINT12	IN	Smart Screen Interrupt
		(0x0 << 10) | // GPG5	EINT13	IN	Key3
		(0x0 << 12) | // GPG6	EINT14	IN	Key4
		(0x0 << 14) | // GPG7	EINT15	IN	Key5
		(0x1 << 16) | // GPG8	EINT16	OUT	nCD_SD
		(0x1 << 18) | // GPG9	EINT17	OUT
		(0x1 << 20) | // GPG10	EINT18	OUT
		(0x0 << 22) | // GPG11	EINT19	IN	Key6
		(0x0 << 24) | // GPG12	EINT18	IN	// GPG[12..15] need to be inputs
		(0x0 << 26) | // GPG13	EINT18	IN	// hard pullups
		(0x0 << 28) | // GPG14	EINT18	IN
		(0x0 << 30) | // GPG15	EINT18	IN
		0;
	gpio->GPGUP = (1 << 15) -1;	// disable pullups for all pins
	
	gpio->GPHCON = 
		(0x2 <<  0) | // GPH0	nCTS0			---
		(0x2 <<  2) | // GPH1	nRTS0			---
		(0x2 <<  4) | // GPH2	TXD0			---
		(0x2 <<  6) | // GPH3	RXD0			---
		(0x2 <<  8) | // GPH4	TXD1			---
		(0x2 << 10) | // GPH5	RXD1			---
		(0x2 << 12) | // GPH6	[TXD2]	nRTS1
		(0x2 << 14) | // GPH7	[RXD2]	nCTS1
		(0x1 << 16) | // GPH8	UEXTCLK			OUT
		(0x1 << 18) | // GPH9	CLKOUT0			OUT
		(0x1 << 20) | // GPH10	CLKOUT1			OUT
		0;
	gpio->GPHUP = (1 << 10) - 1; // disable pullups for all pins

	gpio->EXTINT0=0x22222222;
	gpio->EXTINT1=0x22222222;
	gpio->EXTINT2=0x22222222;

	/* USB Device Part */
	/* GPC5 is reset for USB Device */

	gpio->GPCDAT |= ( 1 << 5) ; 
	udelay(20000);
	gpio->GPCDAT &= ~( 1 << 5) ; 
	udelay(20000);
	gpio->GPCDAT |= ( 1 << 5) ; 

	/* arch number from kernel post 2.6.28 */
#ifndef MACH_TYPE_MINI2440
#define MACH_TYPE_MINI2440 1999
#endif
	gd->bd->bi_arch_number = MACH_TYPE_MINI2440;

	/* adress of boot parameters */
	gd->bd->bi_boot_params = 0x30000100;

	icache_enable();
	dcache_enable();

	return 0;
}



#define MVAL		(0)
#define MVAL_USED 	(0)		//0=each frame   1=rate by MVAL
#define INVVDEN		(1)		//0=normal       1=inverted
#define BSWP		(0)		//Byte swap control
#define HWSWP		(1)		//Half word swap control


//TFT 240320
#define LCD_XSIZE_TFT_240320 	(240)	
#define LCD_YSIZE_TFT_240320 	(320)

//TFT240320
#define HOZVAL_TFT_240320	(LCD_XSIZE_TFT_240320-1)
#define LINEVAL_TFT_240320	(LCD_YSIZE_TFT_240320-1)

//Timing parameter for NEC3.5"
#define VBPD_240320		(3)		
#define VFPD_240320		(10)
#define VSPW_240320		(1)

#define HBPD_240320		(5)
#define HFPD_240320		(2)
#define HSPW_240320		(36)

#define CLKVAL_TFT_240320	(3) 	
//FCLK=101.25MHz,HCLK=50.625MHz,VCLK=6.33MHz


void board_video_init(GraphicDevice *pGD) 
{ 
    S3C24X0_LCD * const lcd = S3C24X0_GetBase_LCD(); 
	 
    /* FIXME: select LCM type by env variable */ 
	 
	/* Configuration for GTA01 LCM on QT2410 */ 
	lcd->LCDCON1 = 0x00000378; /* CLKVAL=4, BPPMODE=16bpp, TFT, ENVID=0 */ 
	
//    lcd->LCDCON2 = 0x014fc141; 
//	lcd->LCDCON3 = 0x0098ef13; 
//	lcd->LCDCON4 = 0x00000d05; 
	lcd->LCDCON5 = 0x00000f09; 

	lcd->LCDCON2 = (VBPD_240320<<24)|(LINEVAL_TFT_240320<<14)|(VFPD_240320<<6)|(VSPW_240320); 
	lcd->LCDCON3 = (HBPD_240320<<19)|(HOZVAL_TFT_240320<<8)|(HFPD_240320); 
	lcd->LCDCON4 = (MVAL<<8)|(HSPW_240320); 
	
   
    lcd->LPCSEL  = 0x00000000; 
} 

int dram_init (void)
{
	S3C24X0_MEMCTL * const mem = S3C24X0_GetBase_MEMCTL();
	gd->bd->bi_dram[0].start = PHYS_SDRAM_1;
	/* if the early bootloader found 128MB, lets tell the kernel */
	if ((mem->BANKCON[6] & 0x3) == 0x2)
		gd->bd->bi_dram[0].size = 128*1024*1024;
	else
		gd->bd->bi_dram[0].size = 64*1024*1024;

	return 0;
}

/* The sum of all part_size[]s must equal to the NAND size, i.e., 0x4000000 */

unsigned int dynpart_size[] = {
    CFG_UBOOT_SIZE, 0x20000, 0x500000, 0xffffffff, 0 };
char *dynpart_names[] = {
    "u-boot", "u-boot_env", "kernel", "rootfs", NULL };


