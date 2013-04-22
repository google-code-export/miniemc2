/********************************************************************
* Description: nmldiag.hh
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
* $Revision: 1.3 $
* $Author: paul_c $
* $Date: 2005/05/23 16:34:15 $
********************************************************************/

#ifndef NMLDIAG_HH
#define NMLDIAG_HH

#include "cmsdiag.hh"

class NML_DIAGNOSTICS_INFO:public CMS_DIAGNOSTICS_INFO {
  public:
    void print();
};

extern "C" int nml_print_diag_list();

#endif
