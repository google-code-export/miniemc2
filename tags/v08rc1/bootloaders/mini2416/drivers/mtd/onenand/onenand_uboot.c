/*
 *  drivers/mtd/onenand/onenand_uboot.c
 *
 *  Copyright (C) 2005-2008 Samsung Electronics
 *  Kyungmin Park <kyungmin.park@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

/*
 * OneNAND initialization at U-Boot
 */

#include <common.h>

/*#ifdef CONFIG_CMD_ONENAND*/

#if defined(CONFIG_S3C64XX) || defined(CONFIG_S5PC1XX)

#include <s3c_onenand.h>

#ifndef CFG_ONENAND_BASE_LIST
#define CFG_ONENAND_BASE_LIST { CFG_ONENAND_BASE }
#endif

int onenand_curr_device = -1;
onenand_info_t onenand_info[CFG_MAX_ONENAND_DEVICE];

static struct onenand_chip onenand_chip[CFG_MAX_ONENAND_DEVICE];
static ulong base_address[CFG_MAX_ONENAND_DEVICE] = CFG_ONENAND_BASE_LIST;

static const char default_onenand_name[] = "OneNAND";

extern int board_onenand_init(struct onenand_chip *onenand);

static void onenand_init_chip(struct mtd_info *mtd, struct onenand_chip *onenand,
			   ulong base_addr)
{
	mtd->priv = onenand;

	if (board_onenand_init(onenand))
		return;

	if (onenand_scan(mtd, CFG_MAX_ONENAND_DEVICE) == 0) {
		if (!mtd->name)
			mtd->name = (char *)default_onenand_name;
	} else
		mtd->name = NULL;
}

void onenand_init(void)
{
	int i;
	unsigned int size = 0;
	for (i = 0; i < CFG_MAX_ONENAND_DEVICE; i++) {
		onenand_init_chip(&onenand_info[i], &onenand_chip[i], base_address[i]);
		size += onenand_info[i].size;
		if (onenand_curr_device == -1)
			onenand_curr_device = i;
	}
	printf("%lu MB\n", size / (1024 * 1024));
}

#else

#include <linux/mtd/compat.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/onenand.h>

struct mtd_info onenand_mtd;
struct onenand_chip onenand_chip;

void onenand_init(void)
{
	memset(&onenand_mtd, 0, sizeof(struct mtd_info));
	memset(&onenand_chip, 0, sizeof(struct onenand_chip));

	onenand_chip.base = (void *) CFG_ONENAND_BASE;
	onenand_mtd.priv = &onenand_chip;

	onenand_scan(&onenand_mtd, 1);

	puts("OneNAND: ");
	print_size(onenand_mtd.size, "\n");
}

#endif

//#endif	/* CONFIG_CMD_ONENAND */
