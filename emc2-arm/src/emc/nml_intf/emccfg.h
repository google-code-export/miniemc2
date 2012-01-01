/********************************************************************
* Description: emccfg.h
*   Compile-time defaults for EMC application. Defaults are used to
*   initialize globals in emcglb.c. Include emcglb.h to access these
*   globals.
*
*   Derived from a work by Fred Proctor & Will Shackleford
*
* Author:
* License: GPL Version 2
* System: Linux
*    
* Copyright (c) 2004 All rights reserved.
*
* Last change:
* $Revision: 1.6 $
* $Author: alex_joni $
* $Date: 2006/09/28 20:44:20 $
********************************************************************/
#ifndef EMCCFG_H
#define EMCCFG_H

#ifdef __cplusplus
extern "C" {
#endif

/* default name of EMC ini file */
#define DEFAULT_EMC_INIFILE "emc.ini"

/* default name of EMC NML file */
#define DEFAULT_EMC_NMLFILE "emc.nml"

/* cycle time for emctask, in seconds */
#define DEFAULT_EMC_TASK_CYCLE_TIME 0.100

/* cycle time for emctio, in seconds */
#define DEFAULT_EMC_IO_CYCLE_TIME 0.100

/* default name of EMC_TOOL tool table file */
#define DEFAULT_TOOL_TABLE_FILE "tool.tbl"

/* default feed rate, in user units per second */
#define DEFAULT_TRAJ_DEFAULT_VELOCITY 1.0

/* default traverse rate, in user units per second */
#define DEFAULT_TRAJ_MAX_VELOCITY 10.0

/* default axis traverse rate, in user units per second */
#define DEFAULT_AXIS_MAX_VELOCITY 1.0

/* default axis acceleration, in user units per second per second */
#define DEFAULT_AXIS_MAX_ACCELERATION 1.0

/* seconds after speed off to apply brake */
#define DEFAULT_SPINDLE_OFF_WAIT 2.0

/* seconds after brake off for spindle on */
#define DEFAULT_SPINDLE_ON_WAIT 2.0

/* point locations for analog outputs */
#define DEFAULT_SPINDLE_ON_INDEX           0
#define DEFAULT_MIN_VOLTS_PER_RPM         -0.01
#define DEFAULT_MAX_VOLTS_PER_RPM          0.01

#ifdef __cplusplus
}				/* matches extern "C" at top */
#endif
#endif
