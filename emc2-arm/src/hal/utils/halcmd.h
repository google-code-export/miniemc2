/* Copyright (C) 2007 Jeff Epler <jepler@unpythonic.net>
 * Copyright (C) 2003 John Kasunich
 *                     <jmkasunich AT users DOT sourceforge DOT net>
 *
 *  Other contributers:
 *                     Martin Kuhnle
 *                     <mkuhnle AT users DOT sourceforge DOT net>
 *                     Alex Joni
 *                     <alex_joni AT users DOT sourceforge DOT net>
 *                     Benn Lipkowitz
 *                     <fenn AT users DOT sourceforge DOT net>
 *                     Stephen Wille Padnos
 *                     <swpadnos AT users DOT sourceforge DOT net>
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of version 2 of the GNU General
 *  Public License as published by the Free Software Foundation.
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111 USA
 *
 *  THE AUTHORS OF THIS LIBRARY ACCEPT ABSOLUTELY NO LIABILITY FOR
 *  ANY HARM OR LOSS RESULTING FROM ITS USE.  IT IS _EXTREMELY_ UNWISE
 *  TO RELY ON SOFTWARE ALONE FOR SAFETY.  Any machinery capable of
 *  harming persons must have provisions for completely removing power
 *  from all motors, etc, before persons enter any danger area.  All
 *  machinery must be designed to comply with local and national safety
 *  codes, and the authors of this software can not, and do not, take
 *  any responsibility for such compliance.
 *
 *  This code was written as part of the EMC HAL project.  For more
 *  information, go to www.linuxcnc.org.
 */

#ifndef HALCMD_H
#define HALCMD_H
#include <stdio.h>
#include "hal.h"
extern int halcmd_startup();
extern void halcmd_shutdown();
extern int halcmd_parse_cmd(char * tokens[]);
extern int halcmd_parse_line(char * line);
extern int halcmd_startup(void);
extern void halcmd_shutdown(void);
extern int prompt_mode, errorcount, halcmd_done;
extern int halcmd_preprocess_line ( char *line, char **tokens);

void halcmd_info(const char *format,...) __attribute__((format(printf,1,2)));
void halcmd_output(const char *format,...) __attribute__((format(printf,1,2)));
void halcmd_warning(const char *format,...) __attribute__((format(printf,1,2)));
void halcmd_error(const char *format,...) __attribute__((format(printf,1,2)));

void halcmd_set_filename(const char *new_filename);
const char *halcmd_get_filename(void);

void halcmd_set_linenumber(int new_linenumber);
int halcmd_get_linenumber(void);


extern FILE *halcmd_inifile;

#define MAX_TOK 20
#define MAX_CMD_LEN 1024
#define MAX_EXPECTED_SIGS 999

#endif
