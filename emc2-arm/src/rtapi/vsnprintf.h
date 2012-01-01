/********************************************************************
* Description: vsnprintf.h
*   Implementation of vsnprintf for kernel space.
*
*   Derived from the Linux 2.4.18 kernel  (linux/lib/vsprintf.c)
*
* License: GPL Version 2
* System: Linux
*    
* Copyright (c) 2004 All rights reserved.
*
* Last change:
* $Revision: 1.4 $
* $Author: jmkasunich $
* $Date: 2006/01/20 02:31:33 $
********************************************************************/
/** vsnprintf.h

This file implements a vsnprintf for use in kernel space.  It is
shamelessly stolen from the 2.4.18 kernel's linux/lib/vsprintf.c
It is used when compiling rtapi for 2.2 kernels that don't have
vsnprintf, only vsprintf.  To avoid difficulties with 64 bit
math, we take the simple approach - we don't support longlong
values (or floating point).
*/

/* This is free software; you can redistribute it and/or modify it
   under the terms of version 2 of the GNU General Public License
   as published by the Free Software Foundation.  This code is
   distributed in the hope that it will be useful, but WITHOUT ANY
   WARRANTY; without even the implied warranty of MERCHANTABILITY 
   or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
   License for more details.

   You should have received a copy of the GNU General Public License
   along with this library; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111 USA
*/


/* we use this so that we can do without the string library */
static int strn_len(const char *s, int count)
{
    const char *sc;

    for (sc = s; count-- && *sc != '\0'; ++sc);
    return sc - s;
}

static int skip_atoi(const char **s)
{
    int i = 0;

    while (isdigit(**s))
	i = i * 10 + *((*s)++) - '0';
    return i;
}

#define ZEROPAD	1		/* pad with zero */
#define SIGN	2		/* unsigned/signed long */
#define PLUS	4		/* show plus */
#define SPACE	8		/* space if plus */
#define LEFT	16		/* left justified */
#define SPECIAL	32		/* 0x */
#define LARGE	64		/* use 'ABCDEF' instead of 'abcdef' */

static char *number(char *buf, char *end, long long numll, int base,
		    int size, int precision, int type)
{
    unsigned long num;
    char c, sign, tmp[66];
    const char *digits;
    const char small_digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    const char large_digits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    int i;

    digits = (type & LARGE) ? large_digits : small_digits;
    if (type & LEFT) {
	type &= ~ZEROPAD;
    }
    if (base < 2 || base > 36) {
	return 0;
    }
    c = (type & ZEROPAD) ? '0' : ' ';
    sign = 0;
    num = (unsigned long) numll;
    if (type & SIGN) {
	if (numll < 0) {
	    sign = '-';
	    num = -numll;
	    size--;
	} else if (type & PLUS) {
	    sign = '+';
	    size--;
	} else if (type & SPACE) {
	    sign = ' ';
	    size--;
	}
    }
    if (type & SPECIAL) {
	if (base == 16) {
	    size -= 2;
	} else if (base == 8) {
	    size--;
	}
    }
    i = 0;
    if (num == 0) {
	tmp[i++] = '0';
    } else
	while (num != 0) {
	    tmp[i++] = digits[num % base];
	    num /= base;
	}
    if (i > precision) {
	precision = i;
    }
    size -= precision;
    if (!(type & (ZEROPAD + LEFT))) {
	while (size-- > 0) {
	    if (buf <= end) {
		*buf = ' ';
	    }
	    ++buf;
	}
    }
    if (sign) {
	if (buf <= end) {
	    *buf = sign;
	}
	++buf;
    }
    if (type & SPECIAL) {
	if (base == 8) {
	    if (buf <= end) {
		*buf = '0';
	    }
	    ++buf;
	} else if (base == 16) {
	    if (buf <= end) {
		*buf = '0';
	    }
	    ++buf;
	    if (buf <= end) {
		*buf = digits[33];
	    }
	    ++buf;
	}
    }
    if (!(type & LEFT)) {
	while (size-- > 0) {
	    if (buf <= end) {
		*buf = c;
	    }
	    ++buf;
	}
    }
    while (i < precision--) {
	if (buf <= end) {
	    *buf = '0';
	}
	++buf;
    }
    while (i-- > 0) {
	if (buf <= end) {
	    *buf = tmp[i];
	}
	++buf;
    }
    while (size-- > 0) {
	if (buf <= end) {
	    *buf = ' ';
	}
	++buf;
    }
    return buf;
}

/**
* vsn_printf - Format a string and place it in a buffer
* @buf: The buffer to place the result into
* @size: The size of the buffer, including the trailing null space
* @fmt: The format string to use
* @args: Arguments for the format string
*/
static int vsn_printf(char *buf, int size, const char *fmt, va_list args)
{
    int len;
    unsigned long long num;
    int i, base;
    char *str, *end, c;
    const char *s;

    int flags;			/* flags to number() */

    int field_width;		/* width of output field */
    int precision;		/* min. # of digits for integers; max number
				   of chars for from string */
    int qualifier;		/* 'h', 'l', or 'L' for integer fields */

    if (size < 0) {
	size = 0;
    }
    str = buf;
    end = buf + size - 1;
    if (end < buf - 1) {
	end = ((void *) -1);
	size = end - buf + 1;
    }
    for (; *fmt; ++fmt) {
	if (*fmt != '%') {
	    if (str <= end) {
		*str = *fmt;
	    }
	    ++str;
	    continue;
	}
	/* process flags */
	flags = 0;
      repeat:
	++fmt;			/* this also skips first '%' */
	switch (*fmt) {
	case '-':
	    flags |= LEFT;
	    goto repeat;
	case '+':
	    flags |= PLUS;
	    goto repeat;
	case ' ':
	    flags |= SPACE;
	    goto repeat;
	case '#':
	    flags |= SPECIAL;
	    goto repeat;
	case '0':
	    flags |= ZEROPAD;
	    goto repeat;
	}
	/* get field width */
	field_width = -1;
	if (isdigit(*fmt)) {
	    field_width = skip_atoi(&fmt);
	} else if (*fmt == '*') {
	    ++fmt;
	    /* it's the next argument */
	    field_width = va_arg(args, int);
	    if (field_width < 0) {
		field_width = -field_width;
		flags |= LEFT;
	    }
	}
	/* get the precision */
	precision = -1;
	if (*fmt == '.') {
	    ++fmt;
	    if (isdigit(*fmt)) {
		precision = skip_atoi(&fmt);
	    } else if (*fmt == '*') {
		++fmt;
		/* it's the next argument */
		precision = va_arg(args, int);
	    }
	    if (precision < 0) {
		precision = 0;
	    }
	}
	/* get the conversion qualifier */
	qualifier = -1;
	if (*fmt == 'h' || *fmt == 'l') {
	    qualifier = *fmt;
	    ++fmt;
	}
	/* default base */
	base = 10;
	switch (*fmt) {
	case 'c':
	    if (!(flags & LEFT)) {
		while (--field_width > 0) {
		    if (str <= end) {
			*str = ' ';
		    }
		    ++str;
		}
	    }
	    c = (unsigned char) va_arg(args, int);
	    if (str <= end) {
		*str = c;
	    }
	    ++str;
	    while (--field_width > 0) {
		if (str <= end) {
		    *str = ' ';
		}
		++str;
	    }
	    continue;
	case 's':
	    s = va_arg(args, char *);
	    if (!s) {
		s = "<NULL>";
	    }
	    len = strn_len(s, precision);
	    if (!(flags & LEFT)) {
		while (len < field_width--) {
		    if (str <= end) {
			*str = ' ';
		    }
		    ++str;
		}
	    }
	    for (i = 0; i < len; ++i) {
		if (str <= end) {
		    *str = *s;
		}
		++str;
		++s;
	    }
	    while (len < field_width--) {
		if (str <= end) {
		    *str = ' ';
		}
		++str;
	    }
	    continue;
	case 'p':
	    if (field_width == -1) {
		field_width = 2 * sizeof(void *);
		flags |= ZEROPAD;
	    }
	    str = number(str, end, (unsigned long) va_arg(args, void *),
			 16, field_width, precision, flags);
	    continue;
	case '%':
	    if (str <= end) {
		*str = '%';
	    }
	    ++str;
	    continue;
	    /* integer number formats - set up the flags and "break" */
	case 'o':
	    base = 8;
	    break;
	case 'X':
	    flags |= LARGE;
	case 'x':
	    base = 16;
	    break;
	case 'd':
	case 'i':
	    flags |= SIGN;
	case 'u':
	    break;
	default:
	    if (str <= end) {
		*str = '%';
	    }
	    ++str;
	    if (*fmt) {
		if (str <= end) {
		    *str = *fmt;
		}
		++str;
	    } else {
		--fmt;
	    }
	    continue;
	}
	if (qualifier == 'l') {
	    num = va_arg(args, unsigned long);
	    if (flags & SIGN) {
		num = (signed long) num;
	    }
	} else if (qualifier == 'h') {
	    num = (unsigned short) va_arg(args, int);
	    if (flags & SIGN) {
		num = (signed short) num;
	    }
	} else {
	    num = va_arg(args, unsigned int);
	    if (flags & SIGN) {
		num = (signed int) num;
	    }
	}
	str = number(str, end, num, base, field_width, precision, flags);
    }
    if (str <= end) {
	*str = '\0';
    } else if (size > 0) {
	/* don't write out a null byte if the buf size is zero */
	*end = '\0';
    }
    /* the trailing null byte doesn't count towards the total * ++str; */
    return str - buf;
}

/**
 * strsep - Split a string into tokens
 * @s: The string to be searched
 * @ct: The characters to search for
 *
 * strsep() updates @s to point after the token, ready for the next call.
 *
 * It returns empty tokens, too, behaving exactly like the libc function
 * of that name. It is reentrant and should be faster) than strtok.
 * Use only strsep() in new code, please.
 * Taken from 2.4 kernel file by Ingo Oeser <ioe@informatik.tu-chemnitz.de>
 */
char *strsep(char **s, const char *ct)
{
    char *sbegin = *s, *end;

    if (sbegin == NULL)
	return NULL;

    end = strpbrk(sbegin, ct);
    if (end)
	*end++ = '\0';
    *s = end;

    return sbegin;
}
