/** RTAPI is a library providing a uniform API for several real time
    operating systems.  As of ver 2.0, RTLinux and RTAI are supported.
*/
/********************************************************************
* Description:  rtl_ulapi.c
*               This file, 'rtl_ulapi.c', implements the nonrealtime 
*               portion of the API for the RTlinux platform.
*
* Author: John Kasunich, Paul Corner
* License: LGPL Version 2
*    
* Copyright (c) 2004 All rights reserved.
*
* Last change: 
# $Revision: 1.12 $
* $Author: jmkasunich $
* $Date: 2006/05/29 18:36:59 $
********************************************************************/

/** This file, 'rtl_ulapi.c', implements the non-realtime portion of
    the API for the RTLinux platform.  The API is defined in rtapi.h,
    which includes documentation for all the API functions.  The
    realtime portion of the API is implemented in rtl_rtapi.c
    (for the RTLinux platform).
*/

/** Copyright (C) 2003 John Kasunich
                       <jmkasunich AT users DOT sourceforge DOT net>
    Copyright (C) 2003 Paul Corner
                       <paul_c AT users DOT sourceforge DOT net>
    This library is based on version 1.0, which was released into
    the public domain by its author, Fred Proctor.  Thanks Fred!
*/

/* This library is free software; you can redistribute it and/or
   modify it under the terms of version 2.1 of the GNU Lesser General
   Public License as published by the Free Software Foundation.
   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.

   You should have received a copy of the GNU General Lesser Public
   License along with this library; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111 USA
*/

/** THE AUTHORS OF THIS LIBRARY ACCEPT ABSOLUTELY NO LIABILITY FOR
    ANY HARM OR LOSS RESULTING FROM ITS USE.  IT IS _EXTREMELY_ UNWISE
    TO RELY ON SOFTWARE ALONE FOR SAFETY.  Any machinery capable of
    harming persons must have provisions for completely removing power
    from all motors, etc, before persons enter any danger area.  All
    machinery must be designed to comply with local and national safety
    codes, and the authors of this software can not, and do not, take
    any responsibility for such compliance.

    This code was written as part of the EMC HAL project.  For more
    information, go to www.linuxcnc.org.
*/

#ifndef ULAPI
#error ULAPI must be defined to compile rtl_ulapi.c!
#endif

#include <stdio.h>		/* sprintf() */
#include <string.h>		/* strcpy, etc. */
#include <stdarg.h>		/* va_arg, etc. */
//#include <stddef.h>           /* NULL, needed for rtai_shm.h */
#include <unistd.h>		/* open(), close() */
//#include <sys/mman.h>         /* PROT_READ, needed for rtai_shm.h */
//#include <sys/types.h>                /* off_t, needed for rtai_shm.h */
//#include <sys/fcntl.h>                /* O_RDWR, needed for rtai_shm.h */
#include <malloc.h>		/* malloc(), free() */
/*! \todo  FIXME - probably need an include for inb() and outb() */
#include <asm/io.h>
#include <mbuff.h>

#include "rtapi.h"		/* public RTAPI decls */
#include "rtapi_common.h"	/* shared realtime/nonrealtime stuff */

/* the following are internal functions that do the real work associated
   with deleting resources.  They do not check the mutex that protects
   the internal data structures.  When someone calls a rtapi_xxx_delete()
   function, the rtapi funct gets the mutex before calling one of these
   internal functions.  When internal code that already has the mutex
   needs to delete something, it calls these functions directly.
*/

static int shmem_delete(int shmem_id, int module_id);
static int fifo_delete(int fifo_id, int module_id);

/* internal function for mapping int keys to ascii keystrings */
static void genstr(unsigned int i, char *str);
#define KEYSTR_LEN 16		/* length of ascii keystrings */

/* resource data unique to this process */
static void *shmem_addr_array[RTAPI_MAX_SHMEMS + 1];
static int fifo_fd_array[RTAPI_MAX_FIFOS + 1];

static int msg_level = RTAPI_MSG_INFO;	/* message printing level */

/***********************************************************************
*                      GENERAL PURPOSE FUNCTIONS                       *
************************************************************************/

/* NOTE:  /dev/mbuff must have read/write permissions for any users
   that will be running rtapi processes.
*/

int rtapi_init(char *modname)
{
    int n, module_id;
    module_data *module;
    char keystr[KEYSTR_LEN];

    /* say hello */
    rtapi_print_msg(RTAPI_MSG_DBG, "RTAPI: Init\n");
    /* setup revision string and code, and print opening message */
    setup_revision_info();
    /* convert main RTAPI key to a string */
    genstr((unsigned int) RTAPI_KEY, keystr);
    /* get main shared memory block from OS and save its address */
    rtapi_data = mbuff_alloc(keystr, sizeof(rtapi_data_t));
    if (rtapi_data == NULL) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "RTAPI: ERROR: Could not open shared memory area\n");
	return RTAPI_NOMEM;
    }
    /* perform a global init if needed */
    init_rtapi_data(rtapi_data);
    /* check revision code */
    if (rtapi_data->rev_code != rev_code) {
	/* mismatch - release master shared memory block */
	mbuff_free(keystr, rtapi_data);
	rtapi_print_msg(RTAPI_MSG_ERR, "RTAPI: ERROR: Version mismatch\n");
	return RTAPI_FAIL;
    }
    /* set up local pointers to global data */
    module_array = rtapi_data->module_array;
    task_array = rtapi_data->task_array;
    shmem_array = rtapi_data->shmem_array;
    sem_array = rtapi_data->sem_array;
    fifo_array = rtapi_data->fifo_array;
    irq_array = rtapi_data->irq_array;
    /* perform local init */
    for (n = 0; n <= RTAPI_MAX_SHMEMS; n++) {
	shmem_addr_array[n] = NULL;
    }
    /* get the mutex */
    rtapi_mutex_get(&(rtapi_data->mutex));
    /* find empty spot in module array */
    n = 1;
    while ((n <= RTAPI_MAX_MODULES) && (module_array[n].state != NO_MODULE)) {
	n++;
    }
    if (n > RTAPI_MAX_MODULES) {
	/* no room */
	rtapi_mutex_give(&(rtapi_data->mutex));
	return RTAPI_LIMIT;
    }
    /* we have space for the module */
    module_id = n;
    module = &(module_array[n]);
    /* update module data */
    module->state = USERSPACE;
    if (modname != NULL) {
	/* use name supplied by caller, truncating if needed */
	snprintf(module->name, RTAPI_NAME_LEN, "%s", modname);
    } else {
	/* make up a name */
	snprintf(module->name, RTAPI_NAME_LEN, "ULMOD%03d", module_id);
    }
    rtapi_data->ul_module_count++;
    rtapi_mutex_give(&(rtapi_data->mutex));
    return module_id;
}

int rtapi_exit(int module_id)
{
    module_data *module;
    int n;
    char keystr[KEYSTR_LEN];

    if (rtapi_data == NULL) {
	/* not inited */
	return RTAPI_INVAL;
    }
    /* validate module ID */
    if ((module_id < 1) || (module_id > RTAPI_MAX_MODULES)) {
	return RTAPI_BADID;
    }
    /* get mutex */
    rtapi_mutex_get(&(rtapi_data->mutex));
    /* point to the module's data */
    module = &(module_array[module_id]);
    /* check module status */
    if (module->state != USERSPACE) {
	/* not an active user space module */
	rtapi_mutex_give(&(rtapi_data->mutex));
	return RTAPI_INVAL;
    }
    /* clean up any mess left behind by the module */
    for (n = 1; n <= RTAPI_MAX_SHMEMS; n++) {
	if (test_bit(module_id, shmem_array[n].bitmap)) {
	    fprintf(stderr,
		"RTAPI: WARNING: module '%s' failed to delete shmem %02d\n",
		module->name, n);
	    shmem_delete(n, module_id);
	}
    }
    for (n = 1; n <= RTAPI_MAX_FIFOS; n++) {
	if ((fifo_array[n].reader == module_id) ||
	    (fifo_array[n].writer == module_id)) {
	    fprintf(stderr,
		"RTAPI: WARNING: module '%s' failed to delete fifo %02d\n",
		module->name, n);
	    fifo_delete(n, module_id);
	}
    }
    /* update module data */
    module->state = NO_MODULE;
    module->name[0] = '\0';
    rtapi_data->ul_module_count--;
    /* unmap shared memory block */
    rtapi_mutex_give(&(rtapi_data->mutex));
    genstr((unsigned int) RTAPI_KEY, keystr);
    mbuff_free(keystr, rtapi_data);
    return RTAPI_SUCCESS;
}

int rtapi_vsnprintf(char *buf, unsigned long int size, const char *fmt, va_list ap) {
    return vsnprintf(buf, size, fmt, ap);
}

int rtapi_snprintf(char *buf, unsigned long int size, const char *fmt, ...)
{
    va_list args;
    int i;

    va_start(args, fmt);
    /* call the normal library vnsprintf() */
    i = vsnprintf(buf, size, fmt, args);
    va_end(args);
    return i;
}

/* this can be 'stdout' or 'stderr', whichever you prefer... */
#define PRINT_DEST stdout
#define BUFFERLEN 1024

void rtapi_print(const char *fmt, ...)
{
    char buffer[BUFFERLEN + 1];
    va_list args;

    va_start(args, fmt);
    /* call the normal library vnsprintf() */
    vsnprintf(buffer, BUFFERLEN, fmt, args);
    fputs(buffer, PRINT_DEST);
    va_end(args);
}

void rtapi_print_msg(int level, const char *fmt, ...)
{
    char buffer[BUFFERLEN + 1];
    va_list args;

    if ((level <= msg_level) && (msg_level != RTAPI_MSG_NONE)) {
	va_start(args, fmt);
	/* call the normal library vnsprintf() */
	vsnprintf(buffer, BUFFERLEN, fmt, args);
	fputs(buffer, PRINT_DEST);
	va_end(args);
    }
}

int rtapi_set_msg_level(int level)
{
    if ((level < RTAPI_MSG_NONE) || (level > RTAPI_MSG_ALL)) {
	return RTAPI_INVAL;
    }
    msg_level = level;
    return RTAPI_SUCCESS;
}

int rtapi_get_msg_level(void)
{
    return msg_level;
}

void rtapi_printall(void)
{
    module_data *modules;
    task_data *tasks;
    shmem_data *shmems;
    sem_data *sems;
    fifo_data *fifos;
    irq_data *irqs;
    int n, m;

    if (rtapi_data == NULL) {
	printf("rtapi_data = NULL, not initialized\n");
	return;
    }
    printf("rtapi_data = %p\n", rtapi_data);
    printf("  magic = %d\n", rtapi_data->magic);
    printf("  rev_code = %08x\n", rtapi_data->rev_code);
    printf("  mutex = %lu\n", rtapi_data->mutex);
    printf("  rt_module_count = %d\n", rtapi_data->rt_module_count);
    printf("  ul_module_count = %d\n", rtapi_data->ul_module_count);
    printf("  task_count  = %d\n", rtapi_data->task_count);
    printf("  shmem_count = %d\n", rtapi_data->shmem_count);
    printf("  sem_count   = %d\n", rtapi_data->sem_count);
    printf("  fifo_count  = %d\n", rtapi_data->fifo_count);
    printf("  irq_countc  = %d\n", rtapi_data->irq_count);
    printf("  timer_running = %d\n", rtapi_data->timer_running);
    printf("  timer_period  = %ld\n", rtapi_data->timer_period);
    modules = &(rtapi_data->module_array[0]);
    tasks = &(rtapi_data->task_array[0]);
    shmems = &(rtapi_data->shmem_array[0]);
    sems = &(rtapi_data->sem_array[0]);
    fifos = &(rtapi_data->fifo_array[0]);
    irqs = &(rtapi_data->irq_array[0]);
    printf("  module array = %p\n", modules);
    printf("  task array   = %p\n", tasks);
    printf("  shmem array  = %p\n", shmems);
    printf("  sem array    = %p\n", sems);
    printf("  fifo array   = %p\n", fifos);
    printf("  irq array    = %p\n", irqs);
    for (n = 0; n <= RTAPI_MAX_MODULES; n++) {
	if (modules[n].state != NO_MODULE) {
	    printf("  module %02d\n", n);
	    printf("    state = %d\n", modules[n].state);
	    printf("    name = %p\n", modules[n].name);
	    printf("    name = '%s'\n", modules[n].name);
	}
    }
    for (n = 0; n <= RTAPI_MAX_TASKS; n++) {
	if (tasks[n].state != EMPTY) {
	    printf("  task %02d\n", n);
	    printf("    state = %d\n", tasks[n].state);
	    printf("    prio  = %d\n", tasks[n].prio);
	    printf("    owner = %d\n", tasks[n].owner);
	    printf("    code  = %p\n", tasks[n].taskcode);
	}
    }
    for (n = 0; n <= RTAPI_MAX_SHMEMS; n++) {
	if (shmems[n].key != 0) {
	    printf("  shmem %02d\n", n);
	    printf("    key     = %d\n", shmems[n].key);
	    printf("    rtusers = %d\n", shmems[n].rtusers);
	    printf("    ulusers = %d\n", shmems[n].ulusers);
	    printf("    size    = %ld\n", shmems[n].size);
	    printf("    bitmap  = ");
	    for (m = 0; m <= RTAPI_MAX_MODULES; m++) {
		if (test_bit(m, shmems[n].bitmap)) {
		    putchar('1');
		} else {
		    putchar('0');
		}
	    }
	    putchar('\n');
	}
    }
    for (n = 0; n <= RTAPI_MAX_SEMS; n++) {
	if (sems[n].key != 0) {
	    printf("  sem %02d\n", n);
	    printf("    key     = %d\n", sems[n].key);
	    printf("    users   = %d\n", sems[n].users);
	    printf("    bitmap  = ");
	    for (m = 0; m <= RTAPI_MAX_MODULES; m++) {
		if (test_bit(m, sems[n].bitmap)) {
		    putchar('1');
		} else {
		    putchar('0');
		}
	    }
	    putchar('\n');
	}
    }
    for (n = 0; n <= RTAPI_MAX_FIFOS; n++) {
	if (fifos[n].state != UNUSED) {
	    printf("  fifo %02d\n", n);
	    printf("    state  = %d\n", fifos[n].state);
	    printf("    key    = %d\n", fifos[n].key);
	    printf("    reader = %d\n", fifos[n].reader);
	    printf("    writer = %d\n", fifos[n].writer);
	    printf("    size   = %ld\n", fifos[n].size);
	}
    }
    for (n = 0; n <= RTAPI_MAX_IRQS; n++) {
	if (irqs[n].irq_num != 0) {
	    printf("  irq %02d\n", n);
	    printf("    irq_num = %d\n", irqs[n].irq_num);
	    printf("    owner   = %d\n", irqs[n].owner);
	    printf("    handler = %p\n", irqs[n].handler);
	}
    }
}

/***********************************************************************
*                  SHARED MEMORY RELATED FUNCTIONS                     *
************************************************************************/

/*
  RTAPI uses integers as keys, since these can be mapped onto either
  integers or strings easily, whereas the reverse is not true: you can't
  map an arbitrary string to an integer uniquely. Since mbuff takes
  string keys, we need to convert them to some unique string using genstr().

  genstr() generates a string 'str' unique for unsigned integers 'i',
  as the reverse, e.g., 120 -> "012", -1 -> "5927694924"
*/

#define KEYSTR_LEN 16		/* larger than number of digits in MAX_INT */

static void genstr(unsigned int i, char *str)
{
    unsigned int x, d;

    if (i == 0) {
	*str++ = '0';
	*str = 0;
	return;
    }

    x = i;
    while (x > 0) {
	i = x / 10;
	d = x - (i * 10);
	*str++ = d + '0';
	x = i;
    }
    *str = 0;

    return;
}

int rtapi_shmem_new(int key, int module_id, unsigned long int size)
{
    int n;
    int shmem_id;
    shmem_data *shmem;
    char keystr[KEYSTR_LEN];

    /* key must be non-zero, and also cannot match the key that RTAPI uses */
    if ((key == 0) || (key == RTAPI_KEY)) {
	return RTAPI_INVAL;
    }
    /* get the mutex */
    rtapi_mutex_get(&(rtapi_data->mutex));
    /* validate module_id */
    if ((module_id < 1) || (module_id > RTAPI_MAX_MODULES)) {
	rtapi_mutex_give(&(rtapi_data->mutex));
	return RTAPI_INVAL;
    }
    if (module_array[module_id].state != USERSPACE) {
	rtapi_mutex_give(&(rtapi_data->mutex));
	return RTAPI_INVAL;
    }
    /* check if a block is already open for this key */
    for (n = 1; n <= RTAPI_MAX_SHMEMS; n++) {
	if (shmem_array[n].key == key) {
	    /* found a match */
	    shmem_id = n;
	    shmem = &(shmem_array[n]);
	    /* is it big enough? */
	    if (shmem->size < size) {
		rtapi_mutex_give(&(rtapi_data->mutex));
		return RTAPI_INVAL;
	    }
	    /* is this module already using it? */
	    if (test_bit(module_id, shmem->bitmap)) {
		rtapi_mutex_give(&(rtapi_data->mutex));
		return RTAPI_INVAL;
	    }

	    /* no, map it */
	    /* convert key to a string */
	    genstr((unsigned int) key, keystr);
	    /* map shared memory block */
	    shmem_addr_array[shmem_id] = mbuff_alloc(keystr, shmem->size);
	    if (shmem_addr_array[shmem_id] == NULL) {
		/* map failed */
		rtapi_mutex_give(&(rtapi_data->mutex));
		return RTAPI_NOMEM;
	    }
	    /* update usage data */
	    set_bit(module_id, shmem->bitmap);
	    shmem->ulusers++;
	    /* done */
	    rtapi_mutex_give(&(rtapi_data->mutex));
	    return shmem_id;
	}
    }
    /* find empty spot in shmem array */
    n = 1;
    while ((n <= RTAPI_MAX_SHMEMS) && (shmem_array[n].key != 0)) {
	n++;
    }
    if (n > RTAPI_MAX_SHMEMS) {
	/* no room */
	rtapi_mutex_give(&(rtapi_data->mutex));
	return RTAPI_LIMIT;
    }
    /* we have space for the block data */
    shmem_id = n;
    shmem = &(shmem_array[n]);
    /* convert key to a string */
    genstr((unsigned int) key, keystr);
    /* now get shared memory block from OS */
    shmem_addr_array[shmem_id] = mbuff_alloc(keystr, size);
    if (shmem_addr_array[shmem_id] == NULL) {
	rtapi_mutex_give(&(rtapi_data->mutex));
	return RTAPI_NOMEM;
    }
    /* the block has been created, update data */
    set_bit(module_id, shmem->bitmap);
    shmem->key = key;
    shmem->rtusers = 0;
    shmem->ulusers = 1;
    shmem->size = size;
    rtapi_data->shmem_count++;
    /* zero the first word of the shmem area */
    *((long int *)(shmem_addr_array[shmem_id])) = 0;
    /* done */
    rtapi_mutex_give(&(rtapi_data->mutex));
    return shmem_id;
}

int rtapi_shmem_delete(int shmem_id, int module_id)
{
    int retval;

    rtapi_mutex_get(&(rtapi_data->mutex));
    retval = shmem_delete(shmem_id, module_id);
    rtapi_mutex_give(&(rtapi_data->mutex));
    return retval;
}

int shmem_delete(int shmem_id, int module_id)
{
    shmem_data *shmem;
    char keystr[KEYSTR_LEN];

    /* validate shmem ID */
    if ((shmem_id < 1) || (shmem_id > RTAPI_MAX_SHMEMS)) {
	return RTAPI_BADID;
    }
    /* point to the shmem's data */
    shmem = &(shmem_array[shmem_id]);
    /* is the block valid? */
    if (shmem->key == 0) {
	return RTAPI_BADID;
    }
    /* validate module_id */
    if ((module_id < 1) || (module_id > RTAPI_MAX_MODULES)) {
	return RTAPI_INVAL;
    }
    if (module_array[module_id].state != USERSPACE) {
	return RTAPI_INVAL;
    }
    /* is this module using the block? */
    if (test_bit(module_id, shmem->bitmap) == 0) {
	return RTAPI_INVAL;
    }
    /* OK, we're no longer using it */
    clear_bit(module_id, shmem->bitmap);
    shmem->ulusers--;
    /* unmap the block */
    /* convert key to a string */
    genstr((unsigned int) shmem->key, keystr);
    /* free the shared memory */
    mbuff_free(keystr, shmem_addr_array[shmem_id]);
    shmem_addr_array[shmem_id] = NULL;
    /* is somebody else still using the block? */
    if ((shmem->ulusers > 0) || (shmem->rtusers > 0)) {
	/* yes, we're done for now */
	return RTAPI_SUCCESS;
    }
    /* update the data array and usage count */
    shmem->key = 0;
    shmem->size = 0;
    rtapi_data->shmem_count--;
    return RTAPI_SUCCESS;
}

int rtapi_shmem_getptr(int shmem_id, void **ptr)
{
    /* validate shmem ID */
    if ((shmem_id < 1) || (shmem_id > RTAPI_MAX_SHMEMS)) {
	return RTAPI_BADID;
    }
    /* is the block mapped? */
    if (shmem_addr_array[shmem_id] == NULL) {
	return RTAPI_BADID;
    }
    /* pass memory address back to caller */
    *ptr = shmem_addr_array[shmem_id];
    return RTAPI_SUCCESS;
}

/***********************************************************************
*                       FIFO RELATED FUNCTIONS                         *
************************************************************************/

int rtapi_fifo_new(int key, int module_id, unsigned long int size, char mode)
{
    enum
    { DEVSTR_LEN = 256 };
    char devstr[DEVSTR_LEN];
    int n, flags;
    int fifo_id;
    fifo_data *fifo;

    /* key must be non-zero */
    if (key == 0) {
	return RTAPI_INVAL;
    }
    /* mode must be "R" or "W" */
    if ((mode != 'R') && (mode != 'W')) {
	return RTAPI_INVAL;
    }
    /* determine mode for fifo */
    if (mode == 'R') {
	flags = O_RDONLY;
    } else {			/* mode == 'W' */

	flags = O_WRONLY;
    }
    /* get the mutex */
    rtapi_mutex_get(&(rtapi_data->mutex));
    /* validate module_id */
    if ((module_id < 1) || (module_id > RTAPI_MAX_MODULES)) {
	rtapi_mutex_give(&(rtapi_data->mutex));
	return RTAPI_INVAL;
    }
    if (module_array[module_id].state != USERSPACE) {
	rtapi_mutex_give(&(rtapi_data->mutex));
	return RTAPI_INVAL;
    }
    /* check if a fifo already exists for this key */
    for (n = 1; n <= RTAPI_MAX_FIFOS; n++) {
	if ((fifo_array[n].state != UNUSED) && (fifo_array[n].key == key)) {
	    /* found a match */
	    fifo_id = n;
	    fifo = &(fifo_array[n]);
	    /* is the desired mode available */
	    if (mode == 'R') {
		if (fifo->state & HAS_READER) {
		    rtapi_mutex_give(&(rtapi_data->mutex));
		    return RTAPI_BUSY;
		}
		/* determine system name for fifo */
		sprintf(devstr, "/dev/rtf%d", fifo_id);
		/* open the fifo */
		fifo_fd_array[fifo_id] = open(devstr, flags);
		if (fifo_fd_array[fifo_id] < 0) {
		    /* open failed */
		    rtapi_mutex_give(&(rtapi_data->mutex));
		    return RTAPI_NOTFND;
		}
		/* fifo opened, update status */
		fifo->state |= HAS_READER;
		fifo->reader = module_id;
		rtapi_mutex_give(&(rtapi_data->mutex));
		return fifo_id;
	    } else {		/* mode == 'W' */

		if (fifo->state & HAS_WRITER) {
		    rtapi_mutex_give(&(rtapi_data->mutex));
		    return RTAPI_BUSY;
		}
		/* determine system name for fifo */
		sprintf(devstr, "/dev/rtf%d", fifo_id);
		/* open the fifo */
		fifo_fd_array[fifo_id] = open(devstr, flags);
		if (fifo_fd_array[fifo_id] < 0) {
		    /* open failed */
		    rtapi_mutex_give(&(rtapi_data->mutex));
		    return RTAPI_NOTFND;
		}
		/* fifo opened, update status */
		fifo->state |= HAS_WRITER;
		fifo->writer = module_id;
		rtapi_mutex_give(&(rtapi_data->mutex));
		return fifo_id;
	    }
	}
    }
    /* find empty spot in fifo array */
    n = 1;
    while ((n <= RTAPI_MAX_FIFOS) && (fifo_array[n].state != UNUSED)) {
	n++;
    }
    if (n > RTAPI_MAX_FIFOS) {
	/* no room */
	rtapi_mutex_give(&(rtapi_data->mutex));
	return RTAPI_LIMIT;
    }
    /* we have a free ID for the fifo */
    fifo_id = n;
    fifo = &(fifo_array[n]);
    /* determine system name for fifo */
    sprintf(devstr, "/dev/rtf%d", fifo_id);
    /* open the fifo */
    fifo_fd_array[fifo_id] = open(devstr, flags);
    if (fifo_fd_array[fifo_id] < 0) {
	/* open failed */
	rtapi_mutex_give(&(rtapi_data->mutex));
	return RTAPI_NOTFND;
    }
    /* the fifo has been created, update data */
    if (mode == 'R') {
	fifo->state = HAS_READER;
	fifo->reader = module_id;
    } else {			/* mode == 'W' */

	fifo->state = HAS_WRITER;
	fifo->writer = module_id;
    }
    fifo->key = key;
    fifo->size = size;
    rtapi_data->fifo_count++;
    /* done */
    rtapi_mutex_give(&(rtapi_data->mutex));
    return fifo_id;
}

int rtapi_fifo_delete(int fifo_id, int module_id)
{
    int retval;

    rtapi_mutex_get(&(rtapi_data->mutex));
    retval = fifo_delete(fifo_id, module_id);
    rtapi_mutex_give(&(rtapi_data->mutex));
    return retval;
}

static int fifo_delete(int fifo_id, int module_id)
{
    fifo_data *fifo;

    /* validate fifo ID */
    if ((fifo_id < 1) || (fifo_id > RTAPI_MAX_FIFOS)) {
	return RTAPI_BADID;
    }
    /* point to the fifo's data */
    fifo = &(fifo_array[fifo_id]);
    /* is the fifo valid? */
    if (fifo->state == UNUSED) {
	return RTAPI_BADID;
    }
    /* validate module_id */
    if ((module_id < 1) || (module_id > RTAPI_MAX_MODULES)) {
	return RTAPI_INVAL;
    }
    if (module_array[module_id].state != USERSPACE) {
	return RTAPI_INVAL;
    }
    /* is this module using the fifo? */
    if ((fifo->reader != module_id) && (fifo->writer != module_id)) {
	return RTAPI_INVAL;
    }
    /* update fifo state */
    if (fifo->reader == module_id) {
	fifo->state &= ~HAS_READER;
	fifo->reader = 0;
    }
    if (fifo->writer == module_id) {
	fifo->state &= ~HAS_WRITER;
	fifo->writer = 0;
    }
    /* close the fifo */
    if (close(fifo_id) < 0) {
	return RTAPI_NOTFND;
    }
    /* is somebody else still using the fifo */
    if (fifo->state != UNUSED) {
	/* yes, done for now */
	return RTAPI_SUCCESS;
    }
    /* no other users, update the data array and usage count */
    fifo->state = UNUSED;
    fifo->key = 0;
    fifo->size = 0;
    rtapi_data->fifo_count--;
    return RTAPI_SUCCESS;
}

int rtapi_fifo_read(int fifo_id, char *buf, unsigned long int size)
{
    int retval;

    fifo_data *fifo;

    /* validate fifo ID */
    if ((fifo_id < 1) || (fifo_id > RTAPI_MAX_FIFOS)) {
	return RTAPI_BADID;
    }
    /* point to the fifo's data */
    fifo = &(fifo_array[fifo_id]);
    /* is the fifo valid? */
    if ((fifo->state & HAS_READER) == 0) {
	return RTAPI_BADID;
    }
    /* get whatever data is available */
    retval = read(fifo_fd_array[fifo_id], buf, size);
    if (retval <= 0) {
	return RTAPI_FAIL;
    }
    return retval;

}

int rtapi_fifo_write(int fifo_id, char *buf, unsigned long int size)
{
    int retval;
    fifo_data *fifo;

    /* validate fifo ID */
    if ((fifo_id < 1) || (fifo_id > RTAPI_MAX_FIFOS)) {
	return RTAPI_BADID;
    }
    /* point to the fifo's data */
    fifo = &(fifo_array[fifo_id]);
    /* is the fifo valid? */
    if ((fifo->state & HAS_WRITER) == 0) {
	return RTAPI_BADID;
    }
    /* put whatever data will fit */
    retval = write(fifo_fd_array[fifo_id], buf, size);
    if (retval < 0) {
	return RTAPI_FAIL;
    }
    return retval;
}

/***********************************************************************
*                        I/O RELATED FUNCTIONS                         *
************************************************************************/

void rtapi_outb(unsigned char byte, unsigned int port)
{
    outb(byte, port);
}

unsigned char rtapi_inb(unsigned int port)
{
    return inb(port);
}
