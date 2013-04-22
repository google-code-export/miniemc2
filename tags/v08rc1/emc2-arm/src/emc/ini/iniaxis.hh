/********************************************************************
* Description: iniaxis.hh
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
* $Author: yabosukz $
* $Date: 2005/07/08 14:11:04 $
********************************************************************/
#ifndef INIAXIS_HH
#define INIAXIS_HH

#include "emc.hh"		// EMC_AXIS_STAT

/* initializes axis modules from ini file */
extern int iniAxis(int axis, const char *filename);

/* dump axis stat to ini file */
extern int dumpAxis(int axis, const char *filename,
		    EMC_AXIS_STAT * status);
#endif
