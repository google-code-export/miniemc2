/********************************************************************
* Description: IoControl.cc
*           Simply accepts NML messages sent to the IO controller
*           outputs those to a HAL pin,
*           and sends back a "Done" message.
*
*
*  ENABLE logic:  this module exports three HAL pins related to ENABLE.
*  The first is emc-enable-in.  It is an input from the HAL, when FALSE,
*  EMC will go into the STOPPED state (regardless of the state of
*  the other two pins).  When it goes TRUE, EMC will go into the
*  ESTOP_RESET state (also known as READY).
*
*  The second HAL pin is an output to the HAL.  It is controlled by
*  the NML messages ESTOP_ON and ESTOP_OFF, which normally result from
*  user actions at the GUI.  For the simplest system, loop user-enable-out 
*  back to emc-enable-in in the HAL.  The GUI controls user-enable-out, and EMC
*  responds to that once it is looped back.
*
*  If external ESTOP inputs are desired, they can be
*  used in a classicladder rung, in series with user-enable-out.
*  It will look like this:
*
*  -----|UEO|-----|EEST|--+--|EEI|--+--(EEI)----
*                         |         |
*                         +--|URE|--+
*  UEO=user-enable-out
*  EEST=external ESTOP circuitry
*  EEI=machine is enabled
*  URE=user request enable
*
*  This will work like this: EMC will be enabled (by EEI, emc-enabled-in),
*  only if UEO, EEST and EEI are closed. 
*  If any of UEO (user requested stop) or EEST (external estop) have been
*  opened, then EEI will open aswell.
*  After restoring normal condition (UEO and EEST closed), an aditional
*  URE (user-request-enable) is needed, this is either sent by the GUI
*  (using the EMC_AUX_ESTOP_RESET NML message), or by a hardware button
*  connected to the ladder driving URE.
*
*  NML messages are sent usually from the user hitting F1 on the GUI.
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
* $Revision: 1.45 $
* $Author: cradek $
* $Date: 2007/06/16 16:04:02 $
********************************************************************/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>

#include "hal.h"		/* access to HAL functions/definitions */
#include "rtapi.h"		/* rtapi_print_msg */
#include "rcs.hh"		/* RCS_CMD_CHANNEL */
#include "emc.hh"		/* EMC NML */
#include "emc_nml.hh"
#include "emcglb.h"		/* EMC_NMLFILE, EMC_INIFILE, TOOL_TABLE_FILE */
#include "inifile.hh"		/* INIFILE */
#include "initool.hh"		/* iniTool() */
#include "nml_oi.hh"
#include "timer.hh"
#include "rcs_print.hh"

static RCS_CMD_CHANNEL *emcioCommandBuffer = 0;
static RCS_CMD_MSG *emcioCommand = 0;
static RCS_STAT_CHANNEL *emcioStatusBuffer = 0;
static EMC_IO_STAT emcioStatus;
static NML *emcErrorBuffer = 0;

struct iocontrol_str {
    hal_bit_t *user_enable_out;	/* output, TRUE when EMC wants stop */
    hal_bit_t *emc_enable_in;	/* input, TRUE on any external stop */
    hal_bit_t *user_request_enable;	/* output, used to reset ENABLE latch */
    hal_bit_t *coolant_mist;	/* coolant mist output pin */
    hal_bit_t *coolant_flood;	/* coolant flood output pin */
    hal_bit_t *lube;		/* lube output pin */
    hal_bit_t *lube_level;	/* lube level input pin */


    // the following pins are needed for toolchanging
    //tool-prepare
    hal_bit_t *tool_prepare;	/* output, pin that notifies HAL it needs to prepare a tool */
    hal_s32_t *tool_prep_number;/* output, pin that holds the tool number to be prepared, only valid when tool-prepare=TRUE */
    hal_s32_t *tool_number;     /* output, pin that holds the tool number currently in the spindle */
    hal_bit_t *tool_prepared;	/* input, pin that notifies that the tool has been prepared */
    //tool-change
    hal_bit_t *tool_change;	/* output, notifies a tool-change should happen (emc should be in the tool-change position) */
    hal_bit_t *tool_changed;	/* input, notifies tool has been changed */

    // note: spindle control has been moved to motion
} * iocontrol_data;			//pointer to the HAL-struct

//static iocontrol_struct *iocontrol_data;	
static int comp_id;				/* component ID */

/********************************************************************
*
* Description: emcIoNmlGet()
*		Attempts to connect to NML buffers and set the relevant
*		pointers.
*
* Return Value: Zero on success or -1 if can not connect to a buffer.
*
* Side Effects: None.
*
* Called By: main()
*
********************************************************************/
static int emcIoNmlGet()
{
    int retval = 0;

    /* Try to connect to EMC IO command buffer */
    if (emcioCommandBuffer == 0) {
	emcioCommandBuffer =
	    new RCS_CMD_CHANNEL(emcFormat, "toolCmd", "tool", EMC_NMLFILE);
	if (!emcioCommandBuffer->valid()) {
	    rtapi_print_msg(RTAPI_MSG_ERR,
			    "emcToolCmd buffer not available\n");
	    delete emcioCommandBuffer;
	    emcioCommandBuffer = 0;
	    retval = -1;
	} else {
	    /* Get our command data structure */
	    emcioCommand = emcioCommandBuffer->get_address();
	}
    }

    /* try to connect to EMC IO status buffer */
    if (emcioStatusBuffer == 0) {
	emcioStatusBuffer =
	    new RCS_STAT_CHANNEL(emcFormat, "toolSts", "tool",
				 EMC_NMLFILE);
	if (!emcioStatusBuffer->valid()) {
	    rtapi_print_msg(RTAPI_MSG_ERR,
			    "toolSts buffer not available\n");
	    delete emcioStatusBuffer;
	    emcioStatusBuffer = 0;
	    retval = -1;
	} else {
	    /* initialize and write status */
	    emcioStatus.heartbeat = 0;
	    emcioStatus.command_type = 0;
	    emcioStatus.echo_serial_number = 0;
	    emcioStatus.status = RCS_DONE;
	    emcioStatusBuffer->write(&emcioStatus);
	}
    }

    /* try to connect to EMC error buffer */
    if (emcErrorBuffer == 0) {
	emcErrorBuffer =
	    new NML(nmlErrorFormat, "emcError", "tool", EMC_NMLFILE);
	if (!emcErrorBuffer->valid()) {
	    rtapi_print_msg(RTAPI_MSG_ERR,
			    "emcError buffer not available\n");
	    delete emcErrorBuffer;
	    emcErrorBuffer = 0;
	    retval = -1;
	}
    }

    return retval;
}

static int iniLoad(const char *filename)
{
    IniFile inifile;
    const char *inistring;
    char version[LINELEN], machine[LINELEN];

    /* Open the ini file */
    if (inifile.Open(filename) == false) {
	return -1;
    }

    if (NULL != (inistring = inifile.Find("DEBUG", "EMC"))) {
	/* copy to global */
	if (1 != sscanf(inistring, "%i", &EMC_DEBUG)) {
	    EMC_DEBUG = 0;
	}
    } else {
	/* not found, use default */
	EMC_DEBUG = 0;
    }

    if (EMC_DEBUG & EMC_DEBUG_VERSIONS) {
	if (NULL != (inistring = inifile.Find("VERSION", "EMC"))) {
	    if(sscanf(inistring, "$Revision: %s", version) != 1) {
		strncpy(version, "unknown", LINELEN-1);
	    }
	} else {
	    strncpy(version, "unknown", LINELEN-1);
	}

	if (NULL != (inistring = inifile.Find("MACHINE", "EMC"))) {
	    strncpy(machine, inistring, LINELEN-1);
	} else {
	    strncpy(machine, "unknown", LINELEN-1);
	}
	rtapi_print("iocontrol: machine: '%s'  version '%s'\n", machine, version);
    }

    if (NULL != (inistring = inifile.Find("NML_FILE", "EMC"))) {
	strcpy(EMC_NMLFILE, inistring);
    } else {
	// not found, use default
    }

    double temp;
    temp = EMC_IO_CYCLE_TIME;
    if (NULL != (inistring = inifile.Find("CYCLE_TIME", "EMCIO"))) {
	if (1 == sscanf(inistring, "%lf", &EMC_IO_CYCLE_TIME)) {
	    // found it
	} else {
	    // found, but invalid
	    EMC_IO_CYCLE_TIME = temp;
	    rtapi_print
		("invalid [EMCIO] CYCLE_TIME in %s (%s); using default %f\n",
		 filename, inistring, EMC_IO_CYCLE_TIME);
	}
    } else {
	// not found, using default
	rtapi_print
	    ("[EMCIO] CYCLE_TIME not found in %s; using default %f\n",
	     filename, EMC_IO_CYCLE_TIME);
    }

    // close it
    inifile.Close();

    return 0;
}

/********************************************************************
*
* Description: loadToolTable(const char *filename, CANON_TOOL_TABLE toolTable[])
*		Loads the tool table from file filename into toolTable[] array.
*		  Array is CANON_TOOL_MAX + 1 entries, since 0 is included.
*
* Return Value: Zero on success or -1 if file not found.
*
* Side Effects: Default setting used if the parameter not found in
*		the ini file.
*
* Called By: main()
*
********************************************************************/
static int loadToolTable(const char *filename,
			 CANON_TOOL_TABLE toolTable[])
{
    int t;
    FILE *fp;
    char buffer[CANON_TOOL_ENTRY_LEN];
    const char *name;

    // check filename
    if (filename[0] == 0) {
	name = TOOL_TABLE_FILE;
    } else {
	// point to name provided
	name = filename;
    }

    //AJ: for debug reasons
    //rtapi_print("loadToolTable called with %s\n", filename);

    // open tool table file
    if (NULL == (fp = fopen(name, "r"))) {
	// can't open file
	return -1;
    }
    // clear out tool table
    for (t = 0; t <= CANON_TOOL_MAX; t++) {
	// unused tools are 0, 0.0, 0.0
	toolTable[t].id = 0;
	toolTable[t].zoffset = 0.0;
	toolTable[t].diameter = 0.0;
        toolTable[t].xoffset = 0.0;
        toolTable[t].frontangle = 0.0;
        toolTable[t].backangle = 0.0;
        toolTable[t].orientation = 0;
    }

    /*
       Override 0's with codes from tool file
       File format is:

       <header>
       <pocket # 0..CANON_TOOL_MAX> <FMS id> <length> <diameter>
       ...

     */

    // read and discard header
    if (NULL == fgets(buffer, 256, fp)) {
	// nothing in file at all
	rtapi_print("IO: toolfile exists, but is empty\n");
	fclose(fp);
	return -1;
    }

    while (!feof(fp)) {
	int pocket;
	int id;
	double zoffset;  // AKA length
        double xoffset;
	double diameter;
        double frontangle, backangle;
        int orientation;

	// just read pocket, ID, and length offset
	if (NULL == fgets(buffer, CANON_TOOL_ENTRY_LEN, fp)) {
	    break;
	}
        if (sscanf(buffer, "%d %d %lf %lf %lf %lf %lf %d",
                   &pocket, &id, &zoffset, &xoffset, &diameter,
                   &frontangle, &backangle, &orientation) == 8) {
            if (pocket < 0 || pocket > CANON_TOOL_MAX) {
                printf("skipping tool: bad pocket number %d\n", pocket);
                continue;
            } else {
                /* lathe tool */
                toolTable[pocket].id = id;
                toolTable[pocket].zoffset = zoffset;
                toolTable[pocket].xoffset = xoffset;
                toolTable[pocket].diameter = diameter;

                toolTable[pocket].frontangle = frontangle;
                toolTable[pocket].backangle = backangle;
                toolTable[pocket].orientation = orientation;
            }
        } else if (sscanf(buffer, "%d %d %lf %lf",
                   &pocket, &id, &zoffset, &diameter) == 4) {
            if (pocket < 0 || pocket > CANON_TOOL_MAX) {
                printf("skipping tool: bad pocket number %d\n", pocket);
                continue;
            } else {
                /* mill tool */
                toolTable[pocket].id = id;
                toolTable[pocket].zoffset = zoffset;
                toolTable[pocket].diameter = diameter;

                // these aren't used on a mill
                toolTable[pocket].frontangle = toolTable[pocket].backangle = 0.0;
                toolTable[pocket].xoffset = 0.0;
                toolTable[pocket].orientation = 0;
            }
        } else {
            /* invalid line. skip it silently */
            continue;
        }
    }

    // close the file
    fclose(fp);

    return 0;
}

/********************************************************************
*
* Description: saveToolTable(const char *filename, CANON_TOOL_TABLE toolTable[])
*		Saves the tool table from toolTable[] array into file filename.
*		  Array is CANON_TOOL_MAX + 1 entries, since 0 is included.
*
* Return Value: Zero on success or -1 if file not found.
*
* Side Effects: Default setting used if the parameter not found in
*		the ini file.
*
* Called By: main()
*
********************************************************************/
static int saveToolTable(const char *filename,
			 CANON_TOOL_TABLE toolTable[])
{
    int pocket;
    FILE *fp;
    const char *name;

    fprintf(stderr,"I thought saveToolTable wasn't used.  Please report.\n");
    return 0;

    // check filename
    if (filename[0] == 0) {
	name = TOOL_TABLE_FILE;
    } else {
	// point to name provided
	name = filename;
    }

    // open tool table file
    if (NULL == (fp = fopen(name, "w"))) {
	// can't open file
	return -1;
    }
    // write header
    fprintf(fp, "POC\tFMS\tLEN\t\tDIAM\n");

    for (pocket = 1; pocket <= CANON_TOOL_MAX; pocket++) {
	fprintf(fp, "%d\t%d\t%f\t%f\n",
		pocket,
		toolTable[pocket].id,
		toolTable[pocket].zoffset, toolTable[pocket].diameter);
    }

    // close the file
    fclose(fp);

    return 0;
}

static int done = 0;

/********************************************************************
*
* Description: quit(int sig)
*		Signal handler for SIGINT - Usually generated by a
*		Ctrl C sequence from the keyboard.
*
* Return Value: None.
*
* Side Effects: Sets the termination condition of the main while loop.
*
* Called By: Operating system.
*
********************************************************************/
static void quit(int sig)
{
    done = 1;
}

/********************************************************************
*
* Description: iocontrol_hal_init(void)
*
* Side Effects: Exports HAL pins.
*
* Called By: main
********************************************************************/
int iocontrol_hal_init(void)
{
    char name[HAL_NAME_LEN + 2];	//name of the pin to be registered
    int n = 0, retval;		//n - number of the hal component (only one for iocotrol)

    /* STEP 1: initialise the hal component */
    comp_id = hal_init("iocontrol");
    if (comp_id < 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
			"IOCONTROL: ERROR: hal_init() failed\n");
	return -1;
    }

    /* STEP 2: allocate shared memory for iocontrol data */
    iocontrol_data = (iocontrol_str *) hal_malloc(sizeof(iocontrol_str));
    if (iocontrol_data == 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
			"IOCONTROL: ERROR: hal_malloc() failed\n");
	hal_exit(comp_id);
	return -1;
    }

    /* STEP 3a: export the out-pin(s) */

    // user-enable-out
    rtapi_snprintf(name, HAL_NAME_LEN, "iocontrol.%d.user-enable-out", n);
    retval =
	hal_pin_bit_new(name, HAL_OUT, &(iocontrol_data->user_enable_out), comp_id);
    if (retval != HAL_SUCCESS) {
	rtapi_print_msg(RTAPI_MSG_ERR,
			"IOCONTROL: ERROR: iocontrol %d pin user-enable-out export failed with err=%i\n",
			n, retval);
	hal_exit(comp_id);
	return -1;
    }
    // user-request-enable
    rtapi_snprintf(name, HAL_NAME_LEN, "iocontrol.%d.user-request-enable", n);
    retval =
	hal_pin_bit_new(name, HAL_OUT, &(iocontrol_data->user_request_enable), comp_id);
    if (retval != HAL_SUCCESS) {
	rtapi_print_msg(RTAPI_MSG_ERR,
			"IOCONTROL: ERROR: iocontrol %d pin user-request-enable export failed with err=%i\n",
			n, retval);
	hal_exit(comp_id);
	return -1;
    }
    // coolant-flood
    rtapi_snprintf(name, HAL_NAME_LEN, "iocontrol.%d.coolant-flood", n);
    retval =
	hal_pin_bit_new(name, HAL_OUT, &(iocontrol_data->coolant_flood),	comp_id);
    if (retval != HAL_SUCCESS) {
	rtapi_print_msg(RTAPI_MSG_ERR,
			"IOCONTROL: ERROR: iocontrol %d pin coolant-flood export failed with err=%i\n",
			n, retval);
	hal_exit(comp_id);
	return -1;
    }
    // coolant-mist
    rtapi_snprintf(name, HAL_NAME_LEN, "iocontrol.%d.coolant-mist", n);
    retval =
	hal_pin_bit_new(name, HAL_OUT, &(iocontrol_data->coolant_mist),
			comp_id);
    if (retval != HAL_SUCCESS) {
	rtapi_print_msg(RTAPI_MSG_ERR,
			"IOCONTROL: ERROR: iocontrol %d pin coolant-mist export failed with err=%i\n",
			n, retval);
	hal_exit(comp_id);
	return -1;
    }
    // lube
    rtapi_snprintf(name, HAL_NAME_LEN, "iocontrol.%d.lube", n);
    retval =
	hal_pin_bit_new(name, HAL_OUT, &(iocontrol_data->lube), comp_id);
    if (retval != HAL_SUCCESS) {
	rtapi_print_msg(RTAPI_MSG_ERR,
			"IOCONTROL: ERROR: iocontrol %d pin lube export failed with err=%i\n",
			n, retval);
	hal_exit(comp_id);
	return -1;
    }
    // tool-prepare
    rtapi_snprintf(name, HAL_NAME_LEN, "iocontrol.%d.tool-prepare", n);
    retval =
	hal_pin_bit_new(name, HAL_OUT, &(iocontrol_data->tool_prepare), comp_id);
    if (retval != HAL_SUCCESS) {
	rtapi_print_msg(RTAPI_MSG_ERR,
			"IOCONTROL: ERROR: iocontrol %d pin tool-prepare export failed with err=%i\n",
			n, retval);
	hal_exit(comp_id);
	return -1;
    }
    // tool-number
    rtapi_snprintf(name, HAL_NAME_LEN, "iocontrol.%d.tool-number", n);
    retval =
	hal_pin_s32_new(name, HAL_OUT, &(iocontrol_data->tool_number), comp_id);
    if (retval != HAL_SUCCESS) {
	rtapi_print_msg(RTAPI_MSG_ERR,
			"IOCONTROL: ERROR: iocontrol %d pin tool-number export failed with err=%i\n",
			n, retval);
	hal_exit(comp_id);
	return -1;
    }
    // tool-prep-number
    rtapi_snprintf(name, HAL_NAME_LEN, "iocontrol.%d.tool-prep-number", n);
    retval =
	hal_pin_s32_new(name, HAL_OUT, &(iocontrol_data->tool_prep_number), comp_id);
    if (retval != HAL_SUCCESS) {
	rtapi_print_msg(RTAPI_MSG_ERR,
			"IOCONTROL: ERROR: iocontrol %d pin tool-prep-number export failed with err=%i\n",
			n, retval);
	hal_exit(comp_id);
	return -1;
    }
    // tool-prepared
    rtapi_snprintf(name, HAL_NAME_LEN, "iocontrol.%d.tool-prepared", n);
    retval =
	hal_pin_bit_new(name, HAL_IN, &(iocontrol_data->tool_prepared), comp_id);
    if (retval != HAL_SUCCESS) {
	rtapi_print_msg(RTAPI_MSG_ERR,
			"IOCONTROL: ERROR: iocontrol %d pin tool-prepared export failed with err=%i\n",
			n, retval);
	hal_exit(comp_id);
	return -1;
    }
    // tool-change
    rtapi_snprintf(name, HAL_NAME_LEN, "iocontrol.%d.tool-change", n);
    retval =
	hal_pin_bit_new(name, HAL_OUT, &(iocontrol_data->tool_change), comp_id);
    if (retval != HAL_SUCCESS) {
	rtapi_print_msg(RTAPI_MSG_ERR,
			"IOCONTROL: ERROR: iocontrol %d pin tool-change export failed with err=%i\n",
			n, retval);
	hal_exit(comp_id);
	return -1;
    }
    // tool-changed
    rtapi_snprintf(name, HAL_NAME_LEN, "iocontrol.%d.tool-changed", n);
    retval =
	hal_pin_bit_new(name, HAL_IN, &(iocontrol_data->tool_changed), comp_id);
    if (retval != HAL_SUCCESS) {
	rtapi_print_msg(RTAPI_MSG_ERR,
			"IOCONTROL: ERROR: iocontrol %d pin tool-changed export failed with err=%i\n",
			n, retval);
	hal_exit(comp_id);
	return -1;
    }
    /* STEP 3b: export the in-pin(s) */

    // emc-enable-in
    rtapi_snprintf(name, HAL_NAME_LEN, "iocontrol.%d.emc-enable-in", n);
    retval =
	hal_pin_bit_new(name, HAL_IN, &(iocontrol_data->emc_enable_in), comp_id);
    if (retval != HAL_SUCCESS) {
	rtapi_print_msg(RTAPI_MSG_ERR,
			"IOCONTROL: ERROR: iocontrol %d pin emc-enable-in export failed with err=%i\n",
			n, retval);
	hal_exit(comp_id);
	return -1;
    }
    // lube_level
    rtapi_snprintf(name, HAL_NAME_LEN, "iocontrol.%d.lube_level", n);
    retval =
	hal_pin_bit_new(name, HAL_IN, &(iocontrol_data->lube_level), comp_id);
    if (retval != HAL_SUCCESS) {
	rtapi_print_msg(RTAPI_MSG_ERR,
			"IOCONTROL: ERROR: iocontrol %d pin lube_level export failed with err=%i\n",
			n, retval);
	hal_exit(comp_id);
	return -1;
    }

    hal_ready(comp_id);

    return 0;
}

/********************************************************************
*
* Description: hal_init_pins(void)
*
* Side Effects: Sets HAL pins default values.
*
* Called By: main
********************************************************************/
void hal_init_pins(void)
{
    *(iocontrol_data->user_enable_out)=0;	/* output, FALSE when EMC wants stop */
    *(iocontrol_data->user_request_enable)=0;	/* output, used to reset HAL latch */
    *(iocontrol_data->coolant_mist)=0;		/* coolant mist output pin */
    *(iocontrol_data->coolant_flood)=0;		/* coolant flood output pin */
    *(iocontrol_data->lube)=0;			/* lube output pin */
    *(iocontrol_data->tool_prepare)=0;		/* output, pin that notifies HAL it needs to prepare a tool */
    *(iocontrol_data->tool_prep_number)=0;	/* output, pin that holds the tool number to be prepared, only valid when tool-prepare=TRUE */
    *(iocontrol_data->tool_change)=0;		/* output, notifies a tool-change should happen (emc should be in the tool-change position) */
}


/********************************************************************
*
* Description: read_hal_inputs(void)
*			Reads the pin values from HAL 
*			this function gets called once per cycle
*			It sets the values for the emcioStatus.aux.*
*
* Returns:	returns > 0 if any of the status has changed
*		we then need to update through NML
*
* Side Effects: updates values
*
* Called By: main every CYCLE
********************************************************************/
int read_hal_inputs(void)
{
    int oldval, retval = 0;

    oldval = emcioStatus.aux.estop;

    if ( *(iocontrol_data->emc_enable_in)==0) //check for estop from HW
	emcioStatus.aux.estop = 1;
    else
	emcioStatus.aux.estop = 0;
    
    if (oldval != emcioStatus.aux.estop) {
	retval = 1;
    }
    
    
    oldval = emcioStatus.lube.level;
    emcioStatus.lube.level = *(iocontrol_data->lube_level);	//check for lube_level from HW
    if (oldval != emcioStatus.lube.level) {
	retval = 1;
    }
    return retval;
}


/********************************************************************
*
* Description: read_tool_inputs(void)
*			Reads the tool-pin values from HAL 
*			this function gets called once per cycle
*			It sets the values for the emcioStatus.aux.*
*
* Returns:	returns which of the status has changed
*		we then need to update through NML (a bit different as read_hal_inputs)
*
* Side Effects: updates values
*
* Called By: main every CYCLE
********************************************************************/
int read_tool_inputs(void)
{
    if (*iocontrol_data->tool_prepare && *iocontrol_data->tool_prepared) {
	emcioStatus.tool.toolPrepped = *(iocontrol_data->tool_prep_number); //check if tool has been prepared
	*(iocontrol_data->tool_prepare) = 0;
	emcioStatus.status = RCS_DONE;  // we finally finished to do tool-changing, signal task with RCS_DONE
	return 10; //prepped finished
    }
    
    if (*iocontrol_data->tool_change && *iocontrol_data->tool_changed) {
	emcioStatus.tool.toolInSpindle = emcioStatus.tool.toolPrepped; //the tool now in the spindle is the one that was prepared
	*(iocontrol_data->tool_number) = emcioStatus.tool.toolInSpindle; //likewise in HAL
	emcioStatus.tool.toolPrepped = -1; //reset the tool preped number, -1 to permit tool 0 to be loaded
	*(iocontrol_data->tool_prep_number) = 0; //likewise in HAL
	*(iocontrol_data->tool_change) = 0; //also reset the tool change signal
	emcioStatus.status = RCS_DONE;	// we finally finished to do tool-changing, signal task with RCS_DONE
	return 11; //change finished
    }
    return 0;
}

static void do_hal_exit(void) {
    hal_exit(comp_id);
}

/********************************************************************
*
* Description: main(int argc, char * argv[])
*		Connects to NML buffers and enters an endless loop
*		processing NML IO commands. Print statements are
*		sent to the console indicating which IO command was
*		executed if debug level is set to RTAPI_MSG_DBG.
*
* Return Value: Zero or -1 if ini file not found or failure to connect
*		to NML buffers.
*
* Side Effects: None.
*
* Called By:
*
********************************************************************/
int main(int argc, char *argv[])
{
    int t, tool_status;
    NMLTYPE type;

    for (t = 1; t < argc; t++) {
	if (!strcmp(argv[t], "-ini")) {
	    if (t == argc - 1) {
		return -1;
	    } else {
		strcpy(EMC_INIFILE, argv[t + 1]);
		t++;
	    }
	    continue;
	}
	/* do other args similarly here */
    }

    /* Register the routine that catches the SIGINT signal */
    signal(SIGINT, quit);
    /* catch SIGTERM too - the run script uses it to shut things down */
    signal(SIGTERM, quit);

    if (iocontrol_hal_init() != 0) {
	rtapi_print_msg(RTAPI_MSG_ERR, "can't initialize the HAL\n");
	return -1;
    }

    atexit(do_hal_exit);

    if (0 != iniLoad(EMC_INIFILE)) {
	rtapi_print_msg(RTAPI_MSG_ERR, "can't open ini file %s\n",
			EMC_INIFILE);
	return -1;
    }

    if (0 != emcIoNmlGet()) {
	rtapi_print_msg(RTAPI_MSG_ERR,
			"can't connect to NML buffers in %s\n",
			EMC_NMLFILE);
	return -1;
    }
    // used only for getting TOOL_TABLE_FILE out of the ini file
    if (0 != iniTool(EMC_INIFILE)) {
	rcs_print_error("iniTool failed.\n");
	return -1;
    }

    if (0 != loadToolTable(TOOL_TABLE_FILE, emcioStatus.tool.toolTable)) {
	rcs_print_error("can't load tool table.\n");
    }

    done = 0;

    /* set status values to 'normal' */
    emcioStatus.aux.estop = 1; //estop=1 means to emc that ESTOP condition is met
    emcioStatus.tool.toolPrepped = -1;
    emcioStatus.tool.toolInSpindle = 0;
    emcioStatus.coolant.mist = 0;
    emcioStatus.coolant.flood = 0;
    emcioStatus.lube.on = 0;
    emcioStatus.lube.level = 1;

    while (!done) {
	// check for inputs from HAL (updates emcioStatus)
	// returns 1 if any of the HAL pins changed from the last time we checked
	/* if an external ESTOP is activated (or another hal-pin has changed)
	   a NML message has to be pushed to EMC.
	   the way it was done status was only checked at the end of a command */
	if (read_hal_inputs() > 0) {
	    emcioStatus.command_type = EMC_IO_STAT_TYPE;
	    emcioStatus.echo_serial_number =
		emcioCommand->serial_number+1; //need for different serial number, because we are pushing a new message
	    emcioStatus.heartbeat++;
	    emcioStatusBuffer->write(&emcioStatus);
	}
	;
	if ( (tool_status = read_tool_inputs() ) > 0) { // in case of tool prep (or change) update, we only need to change the state (from RCS_EXEC
	    emcioStatus.command_type = EMC_IO_STAT_TYPE; // to RCS_DONE, no need for different serial_number
	    emcioStatus.echo_serial_number =
		emcioCommand->serial_number;
	    emcioStatus.heartbeat++;
	    emcioStatusBuffer->write(&emcioStatus);
	}

	/* read NML, run commands */
	if (-1 == emcioCommandBuffer->read()) {
	    /* bad command, wait until next cycle */
	    esleep(EMC_IO_CYCLE_TIME);
	    /* and repeat */
	    continue;
	}

	if (0 == emcioCommand ||	// bad command pointer
	    0 == emcioCommand->type ||	// bad command type
	    emcioCommand->serial_number == emcioStatus.echo_serial_number) {	// command already finished
	    /* wait until next cycle */
	    esleep(EMC_IO_CYCLE_TIME);
	    /* and repeat */
	    continue;
	}

	type = emcioCommand->type;
	emcioStatus.status = RCS_DONE;

	switch (type) {
	case 0:
	    break;

	case EMC_IO_INIT_TYPE:
	    rtapi_print_msg(RTAPI_MSG_DBG, "EMC_IO_INIT\n");
	    hal_init_pins();
	    break;

	case EMC_TOOL_INIT_TYPE:
	    rtapi_print_msg(RTAPI_MSG_DBG, "EMC_TOOL_INIT\n");
	    loadToolTable(TOOL_TABLE_FILE, emcioStatus.tool.toolTable);
	    break;

	case EMC_TOOL_HALT_TYPE:
	    rtapi_print_msg(RTAPI_MSG_DBG, "EMC_TOOL_HALT\n");
	    break;

	case EMC_TOOL_ABORT_TYPE:
	    // this gets sent on any Task Abort, so it might be safer to stop
	    // the spindle  and coolant
	    rtapi_print_msg(RTAPI_MSG_DBG, "EMC_TOOL_ABORT\n");

	    emcioStatus.coolant.mist = 0;
	    emcioStatus.coolant.flood = 0;
	    *(iocontrol_data->coolant_mist)=0;		/* coolant mist output pin */
    	    *(iocontrol_data->coolant_flood)=0;		/* coolant flood output pin */
	    break;

	case EMC_TOOL_PREPARE_TYPE:
	    rtapi_print_msg(RTAPI_MSG_DBG, "EMC_TOOL_PREPARE\n");
	    /* set tool number first */
	    *(iocontrol_data->tool_prep_number) = ((EMC_TOOL_PREPARE *) emcioCommand)->tool;
	    /* then set the prepare pin to tell external logic to get started */
	    *(iocontrol_data->tool_prepare) = 1;
	    // the feedback logic is done inside read_hal_inputs()
	    // we only need to set RCS_EXEC if RCS_DONE is not already set by the above logic
	    if (tool_status != 10) //set above to 10 in case PREP already finished (HAL loopback machine)
		emcioStatus.status = RCS_EXEC;
	    break;

	case EMC_TOOL_LOAD_TYPE:
	    rtapi_print_msg(RTAPI_MSG_DBG, "EMC_TOOL_LOAD loaded=%d prepped=%d\n", emcioStatus.tool.toolInSpindle, emcioStatus.tool.toolPrepped);
	    if ( emcioStatus.tool.toolInSpindle != emcioStatus.tool.toolPrepped
		    && emcioStatus.tool.toolPrepped != -1) {
		//notify HW for toolchange
		*(iocontrol_data->tool_change) = 1;
		// the feedback logic is done inside read_hal_inputs() we only
		// need to set RCS_EXEC if RCS_DONE is not already set by the
		// above logic
		if (tool_status != 11)
		    // set above to 11 in case LOAD already finished (HAL
		    // loopback machine)
		    emcioStatus.status = RCS_EXEC;
	    }
	    break;

	case EMC_TOOL_UNLOAD_TYPE:
	    rtapi_print_msg(RTAPI_MSG_DBG, "EMC_TOOL_UNLOAD\n");
	    emcioStatus.tool.toolInSpindle = 0;
	    break;

	case EMC_TOOL_LOAD_TOOL_TABLE_TYPE:
	    rtapi_print_msg(RTAPI_MSG_DBG, "EMC_TOOL_LOAD_TOOL_TABLE\n");
	    if (0 != loadToolTable(((EMC_TOOL_LOAD_TOOL_TABLE *) emcioCommand)->
			      file, emcioStatus.tool.toolTable))
		emcioStatus.status = RCS_ERROR;
	    break;

	case EMC_TOOL_SET_OFFSET_TYPE:
	    rtapi_print_msg(RTAPI_MSG_DBG,
			    "EMC_TOOL_SET_OFFSET length=%lf diameter=%lf\n",
			    ((EMC_TOOL_SET_OFFSET *) emcioCommand)->length,
			    ((EMC_TOOL_SET_OFFSET *) emcioCommand)->diameter);
	    emcioStatus.tool.
		toolTable[((EMC_TOOL_SET_OFFSET *) emcioCommand)->tool].
		zoffset = ((EMC_TOOL_SET_OFFSET *) emcioCommand)->length;
	    emcioStatus.tool.
		toolTable[((EMC_TOOL_SET_OFFSET *) emcioCommand)->tool].
		diameter = ((EMC_TOOL_SET_OFFSET *) emcioCommand)->diameter;
	    if (0 != saveToolTable(TOOL_TABLE_FILE, emcioStatus.tool.toolTable))
		emcioStatus.status = RCS_ERROR;
	    break;

	case EMC_COOLANT_INIT_TYPE:
	    rtapi_print_msg(RTAPI_MSG_DBG, "EMC_COOLANT_INIT\n");
	    emcioStatus.coolant.mist = 0;
	    emcioStatus.coolant.flood = 0;
	    *(iocontrol_data->coolant_mist) = 0;
	    *(iocontrol_data->coolant_flood) = 0;
	    break;

	case EMC_COOLANT_HALT_TYPE:
	    rtapi_print_msg(RTAPI_MSG_DBG, "EMC_COOLANT_HALT\n");
	    emcioStatus.coolant.mist = 0;
	    emcioStatus.coolant.flood = 0;
	    *(iocontrol_data->coolant_mist) = 0;
	    *(iocontrol_data->coolant_flood) = 0;
	    break;

	case EMC_COOLANT_ABORT_TYPE:
	    rtapi_print_msg(RTAPI_MSG_DBG, "EMC_COOLANT_ABORT\n");
	    emcioStatus.coolant.mist = 0;
	    emcioStatus.coolant.flood = 0;
	    *(iocontrol_data->coolant_mist) = 0;
	    *(iocontrol_data->coolant_flood) = 0;
	    break;

	case EMC_COOLANT_MIST_ON_TYPE:
	    rtapi_print_msg(RTAPI_MSG_DBG, "EMC_COOLANT_MIST_ON\n");
	    emcioStatus.coolant.mist = 1;
	    *(iocontrol_data->coolant_mist) = 1;
	    break;

	case EMC_COOLANT_MIST_OFF_TYPE:
	    rtapi_print_msg(RTAPI_MSG_DBG, "EMC_COOLANT_MIST_OFF\n");
	    emcioStatus.coolant.mist = 0;
	    *(iocontrol_data->coolant_mist) = 0;
	    break;

	case EMC_COOLANT_FLOOD_ON_TYPE:
	    rtapi_print_msg(RTAPI_MSG_DBG, "EMC_COOLANT_FLOOD_ON\n");
	    emcioStatus.coolant.flood = 1;
	    *(iocontrol_data->coolant_flood) = 1;
	    break;

	case EMC_COOLANT_FLOOD_OFF_TYPE:
	    rtapi_print_msg(RTAPI_MSG_DBG, "EMC_COOLANT_FLOOD_OFF\n");
	    emcioStatus.coolant.flood = 0;
	    *(iocontrol_data->coolant_flood) = 0;
	    break;

	case EMC_AUX_INIT_TYPE:
	    rtapi_print_msg(RTAPI_MSG_DBG, "EMC_AUX_INIT\n");
	    hal_init_pins(); //init default (safe) pin values
	    emcioStatus.aux.estop = 1;	// this should get modified by the loopback
	    *(iocontrol_data->user_enable_out) = 0; //don't enable on AUX_INIT
	    break;

	case EMC_AUX_HALT_TYPE:
	    rtapi_print_msg(RTAPI_MSG_DBG, "EMC_AUX_HALT\n");
	    emcioStatus.aux.estop = 1;  // this should get modified by the loopback
	    *(iocontrol_data->user_enable_out) = 0; //disable on AUX_HALT
	    break;

	case EMC_AUX_ABORT_TYPE:
	    rtapi_print_msg(RTAPI_MSG_DBG, "EMC_AUX_ABORT\n");
	    emcioStatus.aux.estop = 1;  // this should get modified by the loopback
	    *(iocontrol_data->user_enable_out) = 0; //disable on AUX_ABORT
	    break;

	case EMC_AUX_ESTOP_ON_TYPE:
	    rtapi_print_msg(RTAPI_MSG_DBG, "EMC_AUX_ESTOP_ON\n");
	    /* assert an ESTOP to the outside world (thru HAL) */
	    *(iocontrol_data->user_enable_out) = 0; //disable on ESTOP_ON
	    hal_init_pins(); //resets all HAL pins to safe value
	    break;

	case EMC_AUX_ESTOP_OFF_TYPE:
	    rtapi_print_msg(RTAPI_MSG_DBG, "EMC_AUX_ESTOP_OFF\n");
	    /* remove ESTOP */
	    *(iocontrol_data->user_enable_out) = 1; //we're good to enable on ESTOP_OFF
	    /* generate a rising edge to reset optional HAL latch */
	    *(iocontrol_data->user_request_enable) = 1;
	    break;
	    
	case EMC_AUX_ESTOP_RESET_TYPE:
	    rtapi_print_msg(RTAPI_MSG_DBG, "EMC_AUX_ESTOP_RESET\n");
	    // doesn't do anything right now, this will need to come from GUI
	    // but that means task needs to be rewritten/rethinked
	    break;
	    
	case EMC_LUBE_INIT_TYPE:
	    rtapi_print_msg(RTAPI_MSG_DBG, "EMC_LUBE_INIT\n");
	    emcioStatus.lube.on = 0;
	    //get the lube-level from hal
	    emcioStatus.lube.level = *(iocontrol_data->lube_level);
	    *(iocontrol_data->lube) = 0;
	    break;

	case EMC_LUBE_HALT_TYPE:
	    rtapi_print_msg(RTAPI_MSG_DBG, "EMC_LUBE_HALT\n");
	    emcioStatus.lube.on = 0;
	    //get the lube-level from hal
	    emcioStatus.lube.level = *(iocontrol_data->lube_level);
	    *(iocontrol_data->lube) = 0;
	    break;

	case EMC_LUBE_ABORT_TYPE:
	    rtapi_print_msg(RTAPI_MSG_DBG, "EMC_LUBE_ABORT\n");
	    emcioStatus.lube.on = 0;
	    emcioStatus.lube.level = 1;
	    //get the lube-level from hal
	    emcioStatus.lube.level = *(iocontrol_data->lube_level);
	    *(iocontrol_data->lube) = 0;
	    break;

	case EMC_LUBE_ON_TYPE:
	    rtapi_print_msg(RTAPI_MSG_DBG, "EMC_LUBE_ON\n");
	    emcioStatus.lube.on = 1;
	    *(iocontrol_data->lube) = 1;
	    break;

	case EMC_LUBE_OFF_TYPE:
	    rtapi_print_msg(RTAPI_MSG_DBG, "EMC_LUBE_OFF\n");
	    emcioStatus.lube.on = 0;
	    *(iocontrol_data->lube) = 0;
	    break;

	case EMC_SET_DEBUG_TYPE:
	    rtapi_print_msg(RTAPI_MSG_DBG, "EMC_SET_DEBUG\n");
	    EMC_DEBUG = ((EMC_SET_DEBUG *) emcioCommand)->debug;
	    break;

	default:
	    rtapi_print("IO: unknown command %s\n", emcSymbolLookup(type));
	    break;
	}			/* switch (type) */

	// ack for the received command
	emcioStatus.command_type = type;
	emcioStatus.echo_serial_number = emcioCommand->serial_number;
	//set above, to allow some commands to fail this
	//emcioStatus.status = RCS_DONE;
	emcioStatus.heartbeat++;
	emcioStatusBuffer->write(&emcioStatus);

	esleep(EMC_IO_CYCLE_TIME);
	/* clear reset line to allow for a later rising edge */
	*(iocontrol_data->user_request_enable) = 0;
	
    }	// end of "while (! done)" loop

    if (emcErrorBuffer != 0) {
	delete emcErrorBuffer;
	emcErrorBuffer = 0;
    }

    if (emcioStatusBuffer != 0) {
	delete emcioStatusBuffer;
	emcioStatusBuffer = 0;
    }

    if (emcioCommandBuffer != 0) {
	delete emcioCommandBuffer;
	emcioCommandBuffer = 0;
    }

    return 0;
}
