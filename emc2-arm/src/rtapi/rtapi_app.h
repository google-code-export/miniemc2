#ifndef RTAPI_APP_H
#define RTAPI_APP_H

/*
  for Linux kernel modules, exactly one file needs to
  include <linux/module.h>. We put this in this header.
  If we ever support non-Linux platforms, this file will
  get full of ifdefs.
*/

#ifdef SIM
EXPORT_SYMBOL(rtapi_app_main);
EXPORT_SYMBOL(rtapi_app_exit);
#else
#include <linux/module.h>

#define rtapi_app_main(a) init_module(a)
#define rtapi_app_exit(a) cleanup_module(a)
#endif

#endif /* RTAPI_APP_H */
