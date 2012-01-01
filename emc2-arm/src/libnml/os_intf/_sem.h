/********************************************************************
* Description: _sem.h
*
*   Derived from a work by Fred Proctor & Will Shackleford
*
* Author:
* License: LGPL Version 2
* System: Linux
*    
* Copyright (c) 2004 All rights reserved.
*
* Last change: 
* $Revision: 1.6 $
* $Author: paul_c $
* $Date: 2005/05/23 16:34:16 $
********************************************************************/

#ifndef _RCS_SEMAPHORE_H
#define _RCS_SEMAPHORE_H

#ifndef rcs_sem_t_defined
typedef void rcs_sem_t;
#define rcs_sem_t_defined
#endif

#ifdef __cplusplus
extern "C" {
#endif

    int rcs_sem_destroy(rcs_sem_t * sem);
    rcs_sem_t *rcs_sem_open(const char *name, int oflag,	/* , int mode 
								 */ ...);
    int rcs_sem_close(rcs_sem_t * sem);
    int rcs_sem_unlink(const char *name);
    int rcs_sem_wait(rcs_sem_t * sem, double timeout);
    int rcs_sem_trywait(rcs_sem_t * sem);
    int rcs_sem_flush(rcs_sem_t * sem);
    int rcs_sem_post(rcs_sem_t * sem);
    int rcs_sem_clear(rcs_sem_t * sem);

/* additions */
    rcs_sem_t *rcs_sem_create(unsigned long int id, int mode, int state);
#ifdef __cplusplus
}
#endif
#endif
