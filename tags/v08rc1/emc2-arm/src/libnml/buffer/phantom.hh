/********************************************************************
* Description: phantom.hh
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
* $Revision: 1.4 $
* $Author: paul_c $
* $Date: 2005/05/23 16:34:10 $
********************************************************************/

#ifndef PHANTOM_HH
#define PHANTOM_HH

#include "cms.hh"

class PHANTOMMEM:public CMS {
  public:
    PHANTOMMEM(char *bufline, char *procline);
      virtual ~ PHANTOMMEM();
    virtual CMS_STATUS main_access(void *_local);
};

#endif
