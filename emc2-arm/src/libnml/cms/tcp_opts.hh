/********************************************************************
* Description: tcp_opts.hh
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
* $Date: 2005/05/23 16:34:12 $
********************************************************************/
#ifndef TCP_OPTS_HH
#define TCP_OPTS_HH

/* Function shared by client and server to set desired options. */
int set_tcp_socket_options(int socket_fd);
int make_tcp_socket_nonblocking(int socket_fd);
int make_tcp_socket_blocking(int socket_fd);

#endif /* TCP_OPTS_HH */
