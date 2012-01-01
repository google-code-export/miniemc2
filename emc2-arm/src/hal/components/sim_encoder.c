/********************************************************************
* Description:  sim_encoder.c
*               A HAL component that generates A, B, and index 
*               signals as an encoder would.
*
* Author: John Kasunich
* License: GPL Version 2
*    
* Copyright (c) 2006 All rights reserved.
*
* Last change: 
# $Revision: 1.15 $
* $Author: cradek $
* $Date: 2007/04/17 00:47:03 $
********************************************************************/
/** This file, 'sim_encoder.c', is a HAL component that simulates 
    a quadrature encoder with an index pulse.  It "rotates" at a
    speed controlled by a HAL pin, and produces A, B, and Z outputs
    on other HAL pins.  A parameter sets the counts/revolution.

    It supports up to 8 simulated encoders. The number is set by
    an insmod command line parameter 'num_chan'.

    The module exports two functions.  'sim-encoder.make-pulses', is
    responsible for actually generating the A, B, and Z signals.  It
    must be executed in a fast thread to reduce pulse jitter.  The 
    other function, 'sim-encoder.update-speed', is is normally called
    from a much slower thread, and sets internal variables used by
    'make-pulses', based on the 'speed' input pin, and the 'ppr'
    parameter.
*/

/** Copyright (C) 2003 John Kasunich
                       <jmkasunich AT users DOT sourceforge DOT net>
*/

/** This program is free software; you can redistribute it and/or
    modify it under the terms of version 2 of the GNU General
    Public License as published by the Free Software Foundation.
    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111 USA

    THE AUTHORS OF THIS LIBRARY ACCEPT ABSOLUTELY NO LIABILITY FOR
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

#ifndef RTAPI
#error This is a realtime component only!
#endif

#include "rtapi.h"		/* RTAPI realtime OS API */
#include "rtapi_app.h"		/* RTAPI realtime module decls */
#include "hal.h"		/* HAL public API decls */

#define MAX_CHAN 8

/* module information */
MODULE_AUTHOR("John Kasunich");
MODULE_DESCRIPTION("Simulated Encoder for EMC HAL");
MODULE_LICENSE("GPL");
int num_chan = 1;
RTAPI_MP_INT(num_chan, "number of 'encoders'");

/***********************************************************************
*                STRUCTURES AND GLOBAL VARIABLES                       *
************************************************************************/

/** These structures contains the runtime data for a single encoder.
    Data is arranged in the structs in the order in which it will be
    accessed, so fetching one item will load the next item(s) into cache.
*/

typedef struct {
    signed long addval;		/* frequency generator add value */
    unsigned long accum;	/* frequency generator accumulator */
    signed char state;		/* current quadrature state */
    long cycle;			/* current cycle */
    hal_bit_t *phaseA;		/* pins for output signals */
    hal_bit_t *phaseB;		/* pins for output signals */
    hal_bit_t *phaseZ;		/* pins for output signals */
    hal_u32_t ppr;		/* param: pulses per revolution */
    hal_float_t scale;		/* param: pulses per revolution */
    hal_float_t *speed;		/* pin: speed in revs/second */
    float old_scale;		/* internal, used to detect changes */
    float scale_mult;		/* internal, reciprocal of scale */
} sim_enc_t;

/* ptr to array of sim_enc_t structs in shared memory, 1 per channel */
static sim_enc_t *sim_enc_array;

/* other globals */
static int comp_id;		/* component ID */
static long periodns;		/* makepulses function period in nanosec */
static long old_periodns;	/* used to detect changes in periodns */
static float periodfp;		/* makepulses function period in seconds */
static float freqscale;		/* conv. factor from Hz to addval counts */
static float maxf;		/* max frequency in Hz */

/***********************************************************************
*                  LOCAL FUNCTION DECLARATIONS                         *
************************************************************************/

static int export_sim_enc(int num, sim_enc_t * addr);
static void make_pulses(void *arg, long period);
static void update_speed(void *arg, long period);

/***********************************************************************
*                       INIT AND EXIT CODE                             *
************************************************************************/

int rtapi_app_main(void)
{
    int n, retval;

    if ((num_chan <= 0) || (num_chan > MAX_CHAN)) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "SIM_ENCODER: ERROR: 'num_chan' must be 1 to %d\n", MAX_CHAN);
	return -1;
    }
    /* periodns will be set to the proper value when 'make_pulses()' 
       runs for the first time.  We load a default value here to avoid
       glitches at startup, but all these 'constants' are recomputed 
       inside 'update_speed()' using the real period. */
    periodns = 50000;
    /* precompute some constants */
    periodfp = periodns * 0.000000001;
    maxf = 1.0 / periodfp;
    freqscale = ((1L << 30) * 2.0) / maxf;
    old_periodns = periodns;
    /* have good config info, connect to the HAL */
    comp_id = hal_init("sim_encoder");
    if (comp_id < 0) {
	rtapi_print_msg(RTAPI_MSG_ERR, "SIM_ENCODER: ERROR: hal_init() failed\n");
	return -1;
    }
    /* allocate shared memory for encoder data */
    sim_enc_array = hal_malloc(num_chan * sizeof(sim_enc_t));
    if (sim_enc_array == 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "SIM_ENCODER: ERROR: hal_malloc() failed\n");
	hal_exit(comp_id);
	return -1;
    }
    /* export all the variables for each simulated encoder */
    for (n = 0; n < num_chan; n++) {
	/* export all vars */
	retval = export_sim_enc(n, &(sim_enc_array[n]));
	if (retval != 0) {
	    rtapi_print_msg(RTAPI_MSG_ERR,
		"SIM_ENCODER: ERROR: 'encoder' %d var export failed\n", n);
	    hal_exit(comp_id);
	    return -1;
	}
    }
    /* export functions */
    retval = hal_export_funct("sim-encoder.make-pulses", make_pulses,
	sim_enc_array, 0, 0, comp_id);
    if (retval != 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "SIM_ENCODER: ERROR: makepulses funct export failed\n");
	hal_exit(comp_id);
	return -1;
    }
    retval = hal_export_funct("sim-encoder.update-speed", update_speed,
	sim_enc_array, 1, 0, comp_id);
    if (retval != 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "SIM_ENCODER: ERROR: speed update funct export failed\n");
	hal_exit(comp_id);
	return -1;
    }
    rtapi_print_msg(RTAPI_MSG_INFO,
	"SIM_ENCODER: installed %d simulated encoders\n", num_chan);
    hal_ready(comp_id);
    return 0;
}

void rtapi_app_exit(void)
{
    hal_exit(comp_id);
}

/***********************************************************************
*              REALTIME STEP PULSE GENERATION FUNCTIONS                *
************************************************************************/

/** The frequency generator works by adding a signed value proportional
    to frequency to an accumulator.  When the accumulator overflows (or
    underflows), it is time to increment (or decrement) the state of the
    output pins.
    The add value is limited to +/-2^30, and overflows are detected
    at bit 30, not bit 31.  This means that with add_val at it's max
    (or min) value, and overflow (or underflow) occurs on every cycle.
*/

static void make_pulses(void *arg, long period)
{
    sim_enc_t *sim_enc;
    int n, overunder, dir;

    /* store period so scaling constants can be (re)calculated */
    periodns = period;
    /* point to sim_enc data structures */
    sim_enc = arg;
    for (n = 0; n < num_chan; n++) {
	/* get current value of bit 31 */
	overunder = sim_enc->accum >> 31;
	/* update the accumulator */
	sim_enc->accum += sim_enc->addval;
	/* test for overflow/underflow (change in bit 31) */
	overunder ^= sim_enc->accum >> 31;
	if ( overunder ) {
	    /* time to update outputs */
	    /* get direction bit, 1 if negative, 0 if positive */
	    dir = sim_enc->addval >> 31;
	    if ( dir ) {
		/* negative rotation, decrement state, detect underflow */
		if (--(sim_enc->state) < 0) {
		    /* state underflow, roll over */
		    sim_enc->state = 3;
		    /* decrement cycle, detect underflow */
		    if (--(sim_enc->cycle) < 0) {
			/* cycle underflow, roll over */
			sim_enc->cycle += sim_enc->ppr;
		    }
		}
	    } else {
		/* positive rotation, increment state, detect overflow */
		if (++(sim_enc->state) > 3) {
		    /* state overflow, roll over */
		    sim_enc->state = 0;
		    /* increment cycle, detect overflow */
		    if (++(sim_enc->cycle) >= sim_enc->ppr) {
			/* cycle overflow, roll over */
			sim_enc->cycle -= sim_enc->ppr;
		    }
		}
	    }
	}
	/* generate outputs */
	switch (sim_enc->state) {
	case 0:
	    *(sim_enc->phaseA) = 1;
	    *(sim_enc->phaseB) = 0;
	    break;
	case 1:
	    *(sim_enc->phaseA) = 1;
	    *(sim_enc->phaseB) = 1;
	    break;
	case 2:
	    *(sim_enc->phaseA) = 0;
	    *(sim_enc->phaseB) = 1;
	    break;
	case 3:
	    *(sim_enc->phaseA) = 0;
	    *(sim_enc->phaseB) = 0;
	    break;
	default:
	    /* illegal state, reset to legal one */
	    sim_enc->state = 0;
	}
	if ((sim_enc->state == 0) && (sim_enc->cycle == 0)) {
	    *(sim_enc->phaseZ) = 1;
	} else {
	    *(sim_enc->phaseZ) = 0;
	}
	/* move on to next 'encoder' */
	sim_enc++;
    }
    /* done */
}

static void update_speed(void *arg, long period)
{
    sim_enc_t *sim_enc;
    int n;
    float rev_sec, freq;

    /* this periodns stuff is a little convoluted because we need to
       calculate some constants here in this relatively slow thread but the
       constants are based on the period of the much faster 'make_pulses()'
       thread. */
    if (periodns != old_periodns) {
	/* recompute various constants that depend on periodns */
	periodfp = periodns * 0.000000001;
	maxf = 1.0 / periodfp;
	freqscale = ((1L << 30) * 2.0) / maxf;
	old_periodns = periodns;
    }
    /* update the 'encoders' */
    sim_enc = arg;
    for (n = 0; n < num_chan; n++) {
	/* check for change in scale value */
	if ( sim_enc->scale != sim_enc->old_scale ) {
	    /* save new scale to detect future changes */
	    sim_enc->old_scale = sim_enc->scale;
	    /* scale value has changed, test and update it */
	    if ((sim_enc->scale < 1e-20) && (sim_enc->scale > -1e-20)) {
		/* value too small, divide by zero is a bad thing */
		sim_enc->scale = 1.0;
	    }
	    /* we actually want the reciprocal */
	    sim_enc->scale_mult = 1.0 / sim_enc->scale;
	}
	/* convert speed command (user units) to revs/sec */
	rev_sec = *(sim_enc->speed) * sim_enc->scale_mult;
	/* convert speed command (revs per sec) to counts/sec */
	freq = rev_sec * sim_enc->ppr * 4.0;
	/* limit the commanded frequency */
	if (freq > maxf) {
	    freq = maxf;
	} else if (freq < -maxf) {
	    freq = -maxf;
	}
	/* calculate new addval */
	sim_enc->addval = freq * freqscale;
	sim_enc++;
    }
    /* done */
}

/***********************************************************************
*                   LOCAL FUNCTION DEFINITIONS                         *
************************************************************************/

static int export_sim_enc(int num, sim_enc_t * addr)
{
    int retval, msg;
    char buf[HAL_NAME_LEN + 2];

    /* This function exports a lot of stuff, which results in a lot of
       logging if msg_level is at INFO or ALL. So we save the current value
       of msg_level and restore it later.  If you actually need to log this
       function's actions, change the second line below */
    msg = rtapi_get_msg_level();
    rtapi_set_msg_level(RTAPI_MSG_WARN);
    /* export param variable for pulses per rev */
    rtapi_snprintf(buf, HAL_NAME_LEN, "sim-encoder.%d.ppr", num);
    retval = hal_param_u32_new(buf, HAL_RW, &(addr->ppr), comp_id);
    if (retval != 0) {
	return retval;
    }
    /* export param variable for scaling */
    rtapi_snprintf(buf, HAL_NAME_LEN, "sim-encoder.%d.scale", num);
    retval = hal_param_float_new(buf, HAL_RW, &(addr->scale), comp_id);
    if (retval != 0) {
	return retval;
    }
    /* export pin for speed command */
    rtapi_snprintf(buf, HAL_NAME_LEN, "sim-encoder.%d.speed", num);
    retval = hal_pin_float_new(buf, HAL_IN, &(addr->speed), comp_id);
    if (retval != 0) {
	return retval;
    }
    /* export pins for output phases */
    rtapi_snprintf(buf, HAL_NAME_LEN, "sim-encoder.%d.phase-A", num);
    retval = hal_pin_bit_new(buf, HAL_OUT, &(addr->phaseA), comp_id);
    if (retval != 0) {
	return retval;
    }
    rtapi_snprintf(buf, HAL_NAME_LEN, "sim-encoder.%d.phase-B", num);
    retval = hal_pin_bit_new(buf, HAL_OUT, &(addr->phaseB), comp_id);
    if (retval != 0) {
	return retval;
    }
    rtapi_snprintf(buf, HAL_NAME_LEN, "sim-encoder.%d.phase-Z", num);
    retval = hal_pin_bit_new(buf, HAL_OUT, &(addr->phaseZ), comp_id);
    if (retval != 0) {
	return retval;
    }
    /* init parameters */
    addr->ppr = 100;
    addr->scale = 1.0;
    /* init internal vars */
    addr->old_scale = 0.0;
    addr->scale_mult = 1.0;
    /* init the state variables */
    addr->accum = 0;
    addr->addval = 0;
    addr->state = 0;
    addr->cycle = 0;
    /* init the outputs */
    *(addr->phaseA) = 0;
    *(addr->phaseB) = 0;
    *(addr->phaseZ) = 0;
    /* restore saved message level */
    rtapi_set_msg_level(msg);
    return 0;
}
