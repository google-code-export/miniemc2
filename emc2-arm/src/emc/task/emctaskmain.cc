/********************************************************************
* Description: emctaskmain.cc
*   Main program for EMC task level
*
*   Derived from a work by Fred Proctor & Will Shackleford
*
* Author:
* License: GPL Version 2
* System: Linux
*    
* Copyright (c) 2004 All rights reserved.
*
********************************************************************/
/*
  Principles of operation:

  1.  The main program calls emcTaskPlan() and emcTaskExecute() cyclically.

  2.  emcTaskPlan() reads the new command, and decides what to do with
  it based on the mode (manual, auto, mdi) or state (estop, on) of the
  machine. Many of the commands just go out immediately to the
  subsystems (motion and IO). In auto mode, the interpreter is called
  and as a result the interp_list is appended with NML commands.

  3.  emcTaskExecute() executes a big switch on execState. If it's done,
  it gets the next item off the interp_list, and sets execState to the
  preconditions for that. These preconditions include waiting for motion,
  waiting for IO, etc. Once they are satisfied, it issues the command, and
  sets execState to the postconditions. Once those are satisfied, it gets
  the next item off the interp_list, and so on.

  4.  preconditions and postconditions are only looked at in conjunction
  with commands on the interp_list. Immediate commands won't have any
  pre- or postconditions associated with them looked at.

  5.  At this point, nothing in this file adds anything to the interp_list.
  This could change, for example, when defining pre- and postconditions for
  jog or home commands. If this is done, make sure that the corresponding
  abort command clears out the interp_list.

  6. Single-stepping is handled in checkPreconditions() as the first
  condition. If we're in single-stepping mode, as indicated by the
  variable 'stepping', we set the state to waiting-for-step. This
  polls on the variable 'steppingWait' which is reset to zero when a
  step command is received, and set to one when the command is
  issued.
  */


#include <stdio.h>		// vsprintf()
#include <string.h>		// strcpy()
#include <stdarg.h>		// va_start()
#include <stdlib.h>		// exit()
#include <signal.h>		// signal(), SIGINT
#include <float.h>		// DBL_MAX
#include <sys/types.h>		// pid_t
#include <unistd.h>		// fork()
#include <sys/wait.h>		// waitpid(), WNOHANG, WIFEXITED
#include <ctype.h>		// isspace()
#include <libintl.h>

#include "rcs.hh"		// NML classes, nmlErrorFormat()
#include "emc.hh"		// EMC NML
#include "emc_nml.hh"
#include "canon.hh"		// CANON_TOOL_TABLE stuff
#include "inifile.hh"		// INIFILE
#include "interpl.hh"		// NML_INTERP_LIST, interp_list
#include "emcglb.h"		// EMC_INIFILE,NMLFILE, EMC_TASK_CYCLE_TIME
#include "interp_return.hh"	// public interpreter return values
#include "interp_internal.hh"	// interpreter private definitions
#include "rcs_print.hh"
#include "timer.hh"
#include "nml_oi.hh"
#include "task.hh"		// emcTaskCommand etc

// command line args-- global so that other modules can access 
int Argc;
char **Argv;

// NML channels
static RCS_CMD_CHANNEL *emcCommandBuffer = 0;
static RCS_STAT_CHANNEL *emcStatusBuffer = 0;
static NML *emcErrorBuffer = 0;

// NML command channel data pointer
static RCS_CMD_MSG *emcCommand = 0;

// global EMC status
EMC_STAT *emcStatus = 0;

// timer stuff
static RCS_TIMER *timer = 0;

// flag signifying that ini file [TASK] CYCLE_TIME is <= 0.0, so
// we should not delay at all between cycles. This means also that
// the EMC_TASK_CYCLE_TIME global will be set to the measured cycle
// time each cycle, in case other code references this.
static int emcTaskNoDelay = 0;
// flag signifying that on the next loop, there should be no delay.
// this is set when transferring trajectory data from userspace to kernel
// space, annd reset otherwise.
static int emcTaskEager = 0;
static double EMC_TASK_CYCLE_TIME_ORIG = 0.0;

// delay counter
static double taskExecDelayTimeout = 0.0;

// emcTaskIssueCommand issues command immediately
static int emcTaskIssueCommand(NMLmsg * cmd);

// pending command to be sent out by emcTaskExecute()
NMLmsg *emcTaskCommand = 0;

// signal handling code to stop main loop
static int done;
static int emctask_shutdown(void);
static int pseudoMdiLineNumber = -1;

static void emctask_quit(int sig)
{
    // set main's done flag
    done = 1;

    // restore signal handler
    signal(sig, emctask_quit);
}

// implementation of EMC error logger
int emcOperatorError(int id, const char *fmt, ...)
{
    EMC_OPERATOR_ERROR error_msg;
    va_list ap;

    // check channel for validity
    if (emcErrorBuffer == NULL)
	return -1;
    if (!emcErrorBuffer->valid())
	return -1;

    if (NULL == fmt) {
	return -1;
    }
    if (0 == *fmt) {
	return -1;
    }
    // prepend error code, leave off 0 ad-hoc code
    error_msg.error[0] = 0;
    if (0 != id) {
	sprintf(error_msg.error, "[%d] ", id);
    }
    // append error string
    va_start(ap, fmt);
    vsprintf(&error_msg.error[strlen(error_msg.error)], fmt, ap);
    va_end(ap);

    // force a NULL at the end for safety
    error_msg.error[LINELEN - 1] = 0;

    // write it
    rcs_print("%s\n", error_msg.error);
    return emcErrorBuffer->write(error_msg);
}

int emcOperatorText(int id, const char *fmt, ...)
{
    EMC_OPERATOR_TEXT text_msg;
    va_list ap;

    // check channel for validity
    if (emcErrorBuffer == NULL)
	return -1;
    if (!emcErrorBuffer->valid())
	return -1;

    // write args to NML message (ignore int text code)
    va_start(ap, fmt);
    vsprintf(text_msg.text, fmt, ap);
    va_end(ap);

    // force a NULL at the end for safety
    text_msg.text[LINELEN - 1] = 0;

    // write it
    return emcErrorBuffer->write(text_msg);
}

int emcOperatorDisplay(int id, const char *fmt, ...)
{
    EMC_OPERATOR_DISPLAY display_msg;
    va_list ap;

    // check channel for validity
    if (emcErrorBuffer == NULL)
	return -1;
    if (!emcErrorBuffer->valid())
	return -1;

    // write args to NML message (ignore int display code)
    va_start(ap, fmt);
    vsprintf(display_msg.display, fmt, ap);
    va_end(ap);

    // force a NULL at the end for safety
    display_msg.display[LINELEN - 1] = 0;

    // write it
    return emcErrorBuffer->write(display_msg);
}

/*
  handling of EMC_SYSTEM_CMD
 */

/* convert string to arg/argv set */

static int argvize(const char *src, char *dst, char *argv[], int len)
{
    char *bufptr;
    int argvix;
    char inquote;
    char looking;

    strncpy(dst, src, len);
    dst[len - 1] = 0;
    bufptr = dst;
    inquote = 0;
    argvix = 0;
    looking = 1;

    while (0 != *bufptr) {
	if (*bufptr == '"') {
	    *bufptr = 0;
	    if (inquote) {
		inquote = 0;
		looking = 1;
	    } else {
		inquote = 1;
	    }
	} else if (isspace(*bufptr) && !inquote) {
	    looking = 1;
	    *bufptr = 0;
	} else if (looking) {
	    looking = 0;
	    argv[argvix] = bufptr;
	    argvix++;
	}
	bufptr++;
    }

    argv[argvix] = 0;		// null-terminate the argv list

    return argvix;
}

static pid_t emcSystemCmdPid = 0;

int emcSystemCmd(char *s)
{
    char buffer[EMC_SYSTEM_CMD_LEN];
    char *argv[EMC_SYSTEM_CMD_LEN / 2 + 1];

    if (0 != emcSystemCmdPid) {
	// something's already running, and we can only handle one
	if (EMC_DEBUG & EMC_DEBUG_TASK_ISSUE) {
	    rcs_print
		("emcSystemCmd: abandoning process %d, running ``%s''\n",
		 emcSystemCmdPid, s);
	}
    }

    emcSystemCmdPid = fork();

    if (-1 == emcSystemCmdPid) {
	// we're still the parent, with no child created
	if (EMC_DEBUG & EMC_DEBUG_TASK_ISSUE) {
	    rcs_print("system command ``%s'' can't be executed\n", s);
	}
	return -1;
    }

    if (0 == emcSystemCmdPid) {
	// we're the child
	// convert string to argc/argv
	argvize(s, buffer, argv, EMC_SYSTEM_CMD_LEN);
	// drop any setuid privileges
	setuid(getuid());
	execvp(argv[0], argv);
	// if we get here, we didn't exec
	if (EMC_DEBUG & EMC_DEBUG_TASK_ISSUE) {
	    rcs_print("emcSystemCmd: can't execute ``%s''\n", s);
	}
	return -1;
    }
    // else we're the parent
    return 0;
}

// shorthand typecasting ptrs
static EMC_AXIS_HALT *axis_halt_msg;
static EMC_AXIS_DISABLE *disable_msg;
static EMC_AXIS_ENABLE *enable_msg;
static EMC_AXIS_HOME *home_msg;
static EMC_AXIS_JOG *jog_msg;
static EMC_AXIS_ABORT *axis_abort_msg;
static EMC_AXIS_INCR_JOG *incr_jog_msg;
static EMC_AXIS_ABS_JOG *abs_jog_msg;
static EMC_AXIS_SET_BACKLASH *set_backlash_msg;
static EMC_AXIS_SET_HOMING_PARAMS *set_homing_params_msg;
static EMC_AXIS_SET_FERROR *set_ferror_msg;
static EMC_AXIS_SET_MIN_FERROR *set_min_ferror_msg;
static EMC_AXIS_SET_MAX_POSITION_LIMIT *set_max_limit_msg;
static EMC_AXIS_SET_MIN_POSITION_LIMIT *set_min_limit_msg;
static EMC_AXIS_OVERRIDE_LIMITS *axis_lim_msg;
//static EMC_AXIS_SET_OUTPUT *axis_output_msg;
static EMC_AXIS_LOAD_COMP *axis_load_comp_msg;
//static EMC_AXIS_SET_STEP_PARAMS *set_step_params_msg;

static EMC_TRAJ_SET_SCALE *emcTrajSetScaleMsg;
static EMC_TRAJ_SET_SPINDLE_SCALE *emcTrajSetSpindleScaleMsg;
static EMC_TRAJ_SET_VELOCITY *emcTrajSetVelocityMsg;
static EMC_TRAJ_SET_ACCELERATION *emcTrajSetAccelerationMsg;
static EMC_TRAJ_LINEAR_MOVE *emcTrajLinearMoveMsg;
static EMC_TRAJ_CIRCULAR_MOVE *emcTrajCircularMoveMsg;
static EMC_TRAJ_DELAY *emcTrajDelayMsg;
static EMC_TRAJ_SET_TERM_COND *emcTrajSetTermCondMsg;
static EMC_TRAJ_SET_SPINDLESYNC *emcTrajSetSpindlesyncMsg;

// These classes are commented out because the compiler
// complains that they are "defined but not used".
//static EMC_MOTION_SET_AOUT *emcMotionSetAoutMsg;
//static EMC_MOTION_SET_DOUT *emcMotionSetDoutMsg;

static EMC_SPINDLE_ON *spindle_on_msg;
static EMC_TOOL_PREPARE *tool_prepare_msg;
static EMC_TOOL_LOAD_TOOL_TABLE *load_tool_table_msg;
static EMC_TOOL_SET_OFFSET *emc_tool_set_offset_msg;
static EMC_TASK_SET_MODE *mode_msg;
static EMC_TASK_SET_STATE *state_msg;
static EMC_TASK_PLAN_RUN *run_msg;
static EMC_TASK_PLAN_EXECUTE *execute_msg;
static EMC_TASK_PLAN_OPEN *open_msg;
static EMC_TASK_PLAN_SET_OPTIONAL_STOP *os_msg;
static EMC_TASK_PLAN_SET_BLOCK_DELETE *bd_msg;

static EMC_AUX_INPUT_WAIT *emcAuxInputWaitMsg;
static int emcAuxInputWaitType = 0;
static int emcAuxInputWaitIndex = -1;

// commands we compose here
static EMC_TASK_PLAN_RUN taskPlanRunCmd;	// 16-Aug-1999 FMP
static EMC_TASK_PLAN_INIT taskPlanInitCmd;
static EMC_TASK_PLAN_SYNCH taskPlanSynchCmd;

static int interpResumeState = EMC_TASK_INTERP_IDLE;
static int programStartLine = 0;	// which line to run program from
// how long the interp list can be
/*! \todo FIXME-- make an ini file global */
#define EMC_TASK_INTERP_MAX_LEN 1000

int stepping = 0;
int steppingWait = 0;
static int steppedLine = 0;

/*
  checkInterpList(NML_INTERP_LIST *il, EMC_STAT *stat) takes a pointer
  to an interpreter list and a pointer to the EMC status, pops each NML
  message off the list, and checks it against limits, resource availability,
  etc. in the status.

  It returns 0 if all messages check out, -1 if any of them fail. If one
  fails, the rest of the list is not checked.
 */
static int checkInterpList(NML_INTERP_LIST * il, EMC_STAT * stat)
{
    NMLmsg *cmd = 0;
    // let's create some shortcuts to casts at compile time
#define operator_error_msg ((EMC_OPERATOR_ERROR *) cmd)
#define linear_move ((EMC_TRAJ_LINEAR_MOVE *) cmd)
#define circular_move ((EMC_TRAJ_CIRCULAR_MOVE *) cmd)

    while (il->len() > 0) {
	cmd = il->get();

	switch (cmd->type) {

	case EMC_OPERATOR_ERROR_TYPE:
	    emcOperatorError(operator_error_msg->id,
			     operator_error_msg->error);
	    break;

	case EMC_TRAJ_LINEAR_MOVE_TYPE:
	    if (linear_move->end.tran.x >
		stat->motion.axis[0].maxPositionLimit) {
		emcOperatorError(0, "%s\n%s", stat->task.command,
				 _("exceeds +X limit"));
		return -1;
	    }
	    if (linear_move->end.tran.y >
		stat->motion.axis[1].maxPositionLimit) {
		emcOperatorError(0, "%s\n%s", stat->task.command,
				 _("exceeds +Y limit"));
		return -1;
	    }
	    if (linear_move->end.tran.z >
		stat->motion.axis[2].maxPositionLimit) {
		emcOperatorError(0, "%s\n%s", stat->task.command,
				 _("exceeds +Z limit"));
		return -1;
	    }
	    if (linear_move->end.tran.x <
		stat->motion.axis[0].minPositionLimit) {
		emcOperatorError(0, "%s\n%s", stat->task.command,
				 _("exceeds -X limit"));
		return -1;
	    }
	    if (linear_move->end.tran.y <
		stat->motion.axis[1].minPositionLimit) {
		emcOperatorError(0, "%s\n%s", stat->task.command,
				 _("exceeds -Y limit"));
		return -1;
	    }
	    if (linear_move->end.tran.z <
		stat->motion.axis[2].minPositionLimit) {
		emcOperatorError(0, "%s\n%s", stat->task.command,
				 _("exceeds -Z limit"));
		return -1;
	    }
	    break;

	case EMC_TRAJ_CIRCULAR_MOVE_TYPE:
	    if (circular_move->end.tran.x >
		stat->motion.axis[0].maxPositionLimit) {
		emcOperatorError(0, "%s\n%s", stat->task.command,
				 _("exceeds +X limit"));
		return -1;
	    }
	    if (circular_move->end.tran.y >
		stat->motion.axis[1].maxPositionLimit) {
		emcOperatorError(0, "%s\n%s", stat->task.command,
				 _("exceeds +Y limit"));
		return -1;
	    }
	    if (circular_move->end.tran.z >
		stat->motion.axis[2].maxPositionLimit) {
		emcOperatorError(0, "%s\n%s", stat->task.command,
				 _("exceeds +Z limit"));
		return -1;
	    }
	    if (circular_move->end.tran.x <
		stat->motion.axis[0].minPositionLimit) {
		emcOperatorError(0, "%s\n%s", stat->task.command,
				 _("exceeds -X limit"));
		return -1;
	    }
	    if (circular_move->end.tran.y <
		stat->motion.axis[1].minPositionLimit) {
		emcOperatorError(0, "%s\n%s", stat->task.command,
				 _("exceeds -Y limit"));
		return -1;
	    }
	    if (circular_move->end.tran.z <
		stat->motion.axis[2].minPositionLimit) {
		emcOperatorError(0, "%s\n%s", stat->task.command,
				 _("exceeds -Z limit"));
		return -1;
	    }
	    break;

	default:
	    break;
	}
    }

    return 0;

    // get rid of the compile-time cast shortcuts
#undef circular_move_msg
#undef linear_move_msg
#undef operator_error_msg
}

/*
  emcTaskPlan()

  Planner for NC code or manual mode operations
  */
static int emcTaskPlan(void)
{
    NMLTYPE type;
    static char errstring[200];
    int retval = 0;
    int readRetval;
    int execRetval;

    // check for new command
    if (emcCommand->serial_number != emcStatus->echo_serial_number) {
	// flag it here locally as a new command
	type = emcCommand->type;
    } else {
	// no new command-- reset local flag
	type = 0;
    }

    // handle any new command
    switch (emcStatus->task.state) {
    case EMC_TASK_STATE_OFF:
    case EMC_TASK_STATE_ESTOP:
    case EMC_TASK_STATE_ESTOP_RESET:

	// now switch on the mode
	switch (emcStatus->task.mode) {
	case EMC_TASK_MODE_MANUAL:
	case EMC_TASK_MODE_AUTO:
	case EMC_TASK_MODE_MDI:

	    // now switch on the command
	    switch (type) {
	    case 0:
	    case EMC_NULL_TYPE:
		// no command
		break;

		// immediate commands
	    case EMC_AXIS_SET_BACKLASH_TYPE:
	    case EMC_AXIS_SET_HOMING_PARAMS_TYPE:
	    case EMC_AXIS_DISABLE_TYPE:
	    case EMC_AXIS_ENABLE_TYPE:
	    case EMC_AXIS_SET_FERROR_TYPE:
	    case EMC_AXIS_SET_MIN_FERROR_TYPE:
	    case EMC_AXIS_ABORT_TYPE:
	    case EMC_AXIS_SET_OUTPUT_TYPE:
	    case EMC_AXIS_LOAD_COMP_TYPE:
	    case EMC_AXIS_SET_STEP_PARAMS_TYPE:
	    case EMC_TRAJ_SET_SCALE_TYPE:
	    case EMC_TRAJ_SET_SPINDLE_SCALE_TYPE:
	    case EMC_TRAJ_SET_FO_ENABLE_TYPE:
	    case EMC_TRAJ_SET_FH_ENABLE_TYPE:
	    case EMC_TRAJ_SET_SO_ENABLE_TYPE:
	    case EMC_TRAJ_SET_VELOCITY_TYPE:
	    case EMC_TRAJ_SET_ACCELERATION_TYPE:
	    case EMC_TASK_INIT_TYPE:
	    case EMC_TASK_SET_MODE_TYPE:
	    case EMC_TASK_SET_STATE_TYPE:
	    case EMC_TASK_PLAN_INIT_TYPE:
	    case EMC_TASK_PLAN_OPEN_TYPE:
	    case EMC_TASK_PLAN_SET_OPTIONAL_STOP_TYPE:
	    case EMC_TASK_PLAN_SET_BLOCK_DELETE_TYPE:
	    case EMC_TASK_ABORT_TYPE:
	    case EMC_TRAJ_CLEAR_PROBE_TRIPPED_FLAG_TYPE:
	    case EMC_TRAJ_PROBE_TYPE:
	    case EMC_AUX_INPUT_WAIT_TYPE:
	    case EMC_TRAJ_RIGID_TAP_TYPE:
	    case EMC_TRAJ_SET_TELEOP_ENABLE_TYPE:
	    case EMC_SET_DEBUG_TYPE:
		retval = emcTaskIssueCommand(emcCommand);
		break;

		// one case where we need to be in manual mode
	    case EMC_AXIS_OVERRIDE_LIMITS_TYPE:
		retval = 0;
		if (emcStatus->task.mode == EMC_TASK_MODE_MANUAL) {
		    retval = emcTaskIssueCommand(emcCommand);
		}
		break;

	    case EMC_TOOL_LOAD_TOOL_TABLE_TYPE:
	    case EMC_TOOL_SET_OFFSET_TYPE:
		// send to IO
		emcTaskQueueCommand(emcCommand);
		// signify no more reading
		emcTaskPlanSetWait();
		if (EMC_DEBUG & EMC_DEBUG_INTERP) {
		    rcs_print("emcTaskPlanSetWait() called\n");
		}
		// then resynch interpreter
		emcTaskQueueCommand(&taskPlanSynchCmd);
		break;

	    default:
		emcOperatorError(0,
				 _
				 ("command (%s) cannot be executed until the machine is out of E-stop and turned on"),
				 emc_symbol_lookup(type));
		retval = -1;
		break;

	    }			// switch (type)

	default:
	    // invalid mode
	    break;

	}			// switch (mode)

	break;			// case EMC_TASK_STATE_OFF,ESTOP,ESTOP_RESET

    case EMC_TASK_STATE_ON:
	/* we can do everything (almost) when the machine is on, so let's
	   switch on the execution mode */
	switch (emcStatus->task.mode) {
	case EMC_TASK_MODE_MANUAL:	// ON, MANUAL
	    switch (type) {
	    case 0:
	    case EMC_NULL_TYPE:
		// no command
		break;

		// immediate commands

	    case EMC_AXIS_DISABLE_TYPE:
	    case EMC_AXIS_ENABLE_TYPE:
	    case EMC_AXIS_SET_BACKLASH_TYPE:
	    case EMC_AXIS_SET_HOMING_PARAMS_TYPE:
	    case EMC_AXIS_SET_FERROR_TYPE:
	    case EMC_AXIS_SET_MIN_FERROR_TYPE:
	    case EMC_AXIS_SET_MAX_POSITION_LIMIT_TYPE:
	    case EMC_AXIS_SET_MIN_POSITION_LIMIT_TYPE:
	    case EMC_AXIS_SET_STEP_PARAMS_TYPE:
	    case EMC_AXIS_ABORT_TYPE:
	    case EMC_AXIS_HALT_TYPE:
	    case EMC_AXIS_HOME_TYPE:
	    case EMC_AXIS_JOG_TYPE:
	    case EMC_AXIS_INCR_JOG_TYPE:
	    case EMC_AXIS_ABS_JOG_TYPE:
	    case EMC_AXIS_OVERRIDE_LIMITS_TYPE:
	    case EMC_AXIS_SET_OUTPUT_TYPE:
	    case EMC_TRAJ_PAUSE_TYPE:
	    case EMC_TRAJ_RESUME_TYPE:
	    case EMC_TRAJ_ABORT_TYPE:
	    case EMC_TRAJ_SET_SCALE_TYPE:
	    case EMC_TRAJ_SET_SPINDLE_SCALE_TYPE:
	    case EMC_TRAJ_SET_FO_ENABLE_TYPE:
	    case EMC_TRAJ_SET_FH_ENABLE_TYPE:
	    case EMC_TRAJ_SET_SO_ENABLE_TYPE:
	    case EMC_SPINDLE_ON_TYPE:
	    case EMC_SPINDLE_OFF_TYPE:
	    case EMC_SPINDLE_BRAKE_RELEASE_TYPE:
	    case EMC_SPINDLE_BRAKE_ENGAGE_TYPE:
	    case EMC_SPINDLE_INCREASE_TYPE:
	    case EMC_SPINDLE_DECREASE_TYPE:
	    case EMC_SPINDLE_CONSTANT_TYPE:
	    case EMC_COOLANT_MIST_ON_TYPE:
	    case EMC_COOLANT_MIST_OFF_TYPE:
	    case EMC_COOLANT_FLOOD_ON_TYPE:
	    case EMC_COOLANT_FLOOD_OFF_TYPE:
	    case EMC_LUBE_ON_TYPE:
	    case EMC_LUBE_OFF_TYPE:
	    case EMC_TASK_SET_MODE_TYPE:
	    case EMC_TASK_SET_STATE_TYPE:
	    case EMC_TASK_ABORT_TYPE:
	    case EMC_TASK_PLAN_PAUSE_TYPE:
	    case EMC_TASK_PLAN_RESUME_TYPE:
	    case EMC_TASK_PLAN_INIT_TYPE:
	    case EMC_TASK_PLAN_SYNCH_TYPE:
	    case EMC_TASK_PLAN_SET_OPTIONAL_STOP_TYPE:
	    case EMC_TASK_PLAN_SET_BLOCK_DELETE_TYPE:
	    case EMC_TASK_PLAN_OPTIONAL_STOP_TYPE:
	    case EMC_TRAJ_CLEAR_PROBE_TRIPPED_FLAG_TYPE:
	    case EMC_TRAJ_PROBE_TYPE:
	    case EMC_AUX_INPUT_WAIT_TYPE:
	    case EMC_TRAJ_RIGID_TAP_TYPE:
	    case EMC_TRAJ_SET_TELEOP_ENABLE_TYPE:
	    case EMC_TRAJ_SET_TELEOP_VECTOR_TYPE:
	    case EMC_SET_DEBUG_TYPE:
		retval = emcTaskIssueCommand(emcCommand);
		break;

		// queued commands

	    case EMC_TASK_PLAN_EXECUTE_TYPE:
		// resynch the interpreter, since we may have moved
		// externally
		emcTaskIssueCommand(&taskPlanSynchCmd);
		// and now call for interpreter execute
		retval = emcTaskIssueCommand(emcCommand);
		break;

	    case EMC_TOOL_LOAD_TOOL_TABLE_TYPE:
	    case EMC_TOOL_SET_OFFSET_TYPE:
		// send to IO
		emcTaskQueueCommand(emcCommand);
		// signify no more reading
		emcTaskPlanSetWait();
		if (EMC_DEBUG & EMC_DEBUG_INTERP) {
		    rcs_print("emcTaskPlanSetWait() called\n");
		}
		// then resynch interpreter
		emcTaskQueueCommand(&taskPlanSynchCmd);
		break;

		// otherwise we can't handle it
	    default:
		sprintf(errstring, _("can't do that (%s) in manual mode"),
			emc_symbol_lookup(type));
		emcOperatorError(0, errstring);
		retval = -1;
		break;

	    }			// switch (type) in ON, MANUAL

	    break;		// case EMC_TASK_MODE_MANUAL

	case EMC_TASK_MODE_AUTO:	// ON, AUTO
	    switch (emcStatus->task.interpState) {
	    case EMC_TASK_INTERP_IDLE:	// ON, AUTO, IDLE
		switch (type) {
		case 0:
		case EMC_NULL_TYPE:
		    // no command
		    break;

		    // immediate commands

		case EMC_AXIS_SET_BACKLASH_TYPE:
		case EMC_AXIS_SET_HOMING_PARAMS_TYPE:
		case EMC_AXIS_SET_FERROR_TYPE:
		case EMC_AXIS_SET_MIN_FERROR_TYPE:
		case EMC_AXIS_SET_OUTPUT_TYPE:
		case EMC_AXIS_SET_STEP_PARAMS_TYPE:
		case EMC_TRAJ_PAUSE_TYPE:
		case EMC_TRAJ_RESUME_TYPE:
		case EMC_TRAJ_ABORT_TYPE:
		case EMC_TRAJ_SET_SCALE_TYPE:
		case EMC_TRAJ_SET_SPINDLE_SCALE_TYPE:
		case EMC_TRAJ_SET_FO_ENABLE_TYPE:
	        case EMC_TRAJ_SET_FH_ENABLE_TYPE:
		case EMC_TRAJ_SET_SO_ENABLE_TYPE:
		case EMC_SPINDLE_ON_TYPE:
		case EMC_SPINDLE_OFF_TYPE:
		case EMC_SPINDLE_BRAKE_RELEASE_TYPE:
		case EMC_SPINDLE_BRAKE_ENGAGE_TYPE:
		case EMC_SPINDLE_INCREASE_TYPE:
		case EMC_SPINDLE_DECREASE_TYPE:
		case EMC_SPINDLE_CONSTANT_TYPE:
		case EMC_COOLANT_MIST_ON_TYPE:
		case EMC_COOLANT_MIST_OFF_TYPE:
		case EMC_COOLANT_FLOOD_ON_TYPE:
		case EMC_COOLANT_FLOOD_OFF_TYPE:
		case EMC_LUBE_ON_TYPE:
		case EMC_LUBE_OFF_TYPE:
		case EMC_TASK_SET_MODE_TYPE:
		case EMC_TASK_SET_STATE_TYPE:
		case EMC_TASK_ABORT_TYPE:
		case EMC_TASK_PLAN_INIT_TYPE:
		case EMC_TASK_PLAN_OPEN_TYPE:
		case EMC_TASK_PLAN_RUN_TYPE:
		case EMC_TASK_PLAN_EXECUTE_TYPE:
		case EMC_TASK_PLAN_PAUSE_TYPE:
		case EMC_TASK_PLAN_RESUME_TYPE:
		case EMC_TASK_PLAN_SET_OPTIONAL_STOP_TYPE:
		case EMC_TASK_PLAN_SET_BLOCK_DELETE_TYPE:
		case EMC_TASK_PLAN_OPTIONAL_STOP_TYPE:
		case EMC_TRAJ_CLEAR_PROBE_TRIPPED_FLAG_TYPE:
		case EMC_TRAJ_PROBE_TYPE:
		case EMC_AUX_INPUT_WAIT_TYPE:
		case EMC_TRAJ_RIGID_TAP_TYPE:
		case EMC_SET_DEBUG_TYPE:
		    retval = emcTaskIssueCommand(emcCommand);
		    break;

		case EMC_TASK_PLAN_STEP_TYPE:
		    // handles case where first action is to step the program
		    taskPlanRunCmd.line = 1;	// run from start
		    /*! \todo FIXME-- can have GUI set this; send a run instead of a 
		       step */
		    retval = emcTaskIssueCommand(&taskPlanRunCmd);
		    // issuing an EMC_TASK_PLAN_RUN message clears the
		    // stepping
		    // flag-- reset it here
		    stepping = 1;	// set step flag
		    steppingWait = 0;	// don't wait for first one
		    break;

		case EMC_TOOL_LOAD_TOOL_TABLE_TYPE:
		case EMC_TOOL_SET_OFFSET_TYPE:
		    // send to IO
		    emcTaskQueueCommand(emcCommand);
		    // signify no more reading
		    emcTaskPlanSetWait();
		    if (EMC_DEBUG & EMC_DEBUG_INTERP) {
			rcs_print("emcTaskPlanSetWait() called\n");
		    }
		    // then resynch interpreter
		    emcTaskQueueCommand(&taskPlanSynchCmd);
		    break;

		    // otherwise we can't handle it
		default:
		    sprintf(errstring,
			    _
			    ("can't do that (%s) in auto mode with the interpreter idle"),
			    emc_symbol_lookup(type));
		    emcOperatorError(0, errstring);
		    retval = -1;
		    break;

		}		// switch (type) in ON, AUTO, IDLE

		break;		// EMC_TASK_INTERP_IDLE

	    case EMC_TASK_INTERP_READING:	// ON, AUTO, READING
		switch (type) {
		case 0:
		case EMC_NULL_TYPE:
		    // no command
		    break;

		    // immediate commands

		case EMC_AXIS_SET_BACKLASH_TYPE:
		case EMC_AXIS_SET_HOMING_PARAMS_TYPE:
		case EMC_AXIS_SET_FERROR_TYPE:
		case EMC_AXIS_SET_MIN_FERROR_TYPE:
		case EMC_AXIS_SET_OUTPUT_TYPE:
		case EMC_AXIS_SET_STEP_PARAMS_TYPE:
		case EMC_TRAJ_PAUSE_TYPE:
		case EMC_TRAJ_RESUME_TYPE:
		case EMC_TRAJ_ABORT_TYPE:
		case EMC_TRAJ_SET_SCALE_TYPE:
		case EMC_TRAJ_SET_SPINDLE_SCALE_TYPE:
		case EMC_TRAJ_SET_FO_ENABLE_TYPE:
		case EMC_TRAJ_SET_FH_ENABLE_TYPE:
		case EMC_TRAJ_SET_SO_ENABLE_TYPE:
		case EMC_SPINDLE_INCREASE_TYPE:
		case EMC_SPINDLE_DECREASE_TYPE:
		case EMC_SPINDLE_CONSTANT_TYPE:
		case EMC_TASK_PLAN_PAUSE_TYPE:
		case EMC_TASK_PLAN_RESUME_TYPE:
		case EMC_TASK_PLAN_SET_OPTIONAL_STOP_TYPE:
		case EMC_TASK_PLAN_SET_BLOCK_DELETE_TYPE:
		case EMC_TASK_PLAN_OPTIONAL_STOP_TYPE:
		case EMC_TASK_SET_MODE_TYPE:
		case EMC_TASK_SET_STATE_TYPE:
		case EMC_TASK_ABORT_TYPE:
		case EMC_TRAJ_CLEAR_PROBE_TRIPPED_FLAG_TYPE:
		case EMC_TRAJ_PROBE_TYPE:
		case EMC_AUX_INPUT_WAIT_TYPE:
		case EMC_TRAJ_RIGID_TAP_TYPE:
		case EMC_SET_DEBUG_TYPE:
		    retval = emcTaskIssueCommand(emcCommand);
		    return retval;
		    break;

		case EMC_TASK_PLAN_STEP_TYPE:
		    stepping = 1;	// set stepping mode in case it's not
		    steppingWait = 0;	// clear the wait
		    break;

		    // otherwise we can't handle it
		default:
		    sprintf(errstring,
			    _
			    ("can't do that (%s) in auto mode with the interpreter reading"),
			    emc_symbol_lookup(type));
		    emcOperatorError(0, errstring);
		    retval = -1;
		    break;

		}		// switch (type) in ON, AUTO, READING

		// now handle interpreter call logic
		if (interp_list.len() <= EMC_TASK_INTERP_MAX_LEN) {
                    int count = 0;
interpret_again:
		    if (emcTaskPlanIsWait()) {
			// delay reading of next line until all is done
			if (interp_list.len() == 0 &&
			    emcTaskCommand == 0 &&
			    emcStatus->task.execState ==
			    EMC_TASK_EXEC_DONE) {
			    emcTaskPlanClearWait();
			    if (EMC_DEBUG & EMC_DEBUG_INTERP) {
				rcs_print
				    ("emcTaskPlanClearWait() called\n");
			    }
			}
		    } else {
			readRetval = emcTaskPlanRead();
			if (EMC_DEBUG & EMC_DEBUG_INTERP) {
			    rcs_print("emcTaskPlanRead() returned %d\n",
				      readRetval);
			}
			/*! \todo MGS FIXME
			   This next bit of code is goofy for the following reasons:
			   1. It uses numbers when these values are #defined in interp_return.hh...
			   2. This if() actually evaluates to if (readRetval != INTERP_OK)...
			   3. The "end of file" comment is inaccurate...
			   *** Need to look at all calls to things that return INTERP_xxx values! ***
			   MGS */
			if (readRetval > INTERP_MIN_ERROR || readRetval == 3	/* INTERP_ENDFILE 
										 */  ||
			    readRetval == 1 /* INTERP_EXIT */  ||
			    readRetval == 2	/* INTERP_ENDFILE,
						   INTERP_EXECUTE_FINISH */ ) {
			    /* emcTaskPlanRead retval != INTERP_OK
			       Signal to the rest of the system that that the interp
			       is now in a paused state. */
			    /*! \todo FIXME The above test *should* be reduced to:
			       readRetVal != INTERP_OK
			       (N.B. Watch for negative error codes.) */
			    emcStatus->task.interpState =
				EMC_TASK_INTERP_WAITING;
			} else {
			    // got a good line
			    // record the line number and command
			    emcStatus->task.readLine = emcTaskPlanLine();
			    if (EMC_DEBUG & EMC_DEBUG_INTERP) {
				rcs_print
				    ("emcTaskPlanLine() returned %d\n",
				     emcStatus->task.readLine);
			    }

			    interp_list.set_line_number(emcStatus->task.
							readLine);
			    emcTaskPlanCommand((char *) &emcStatus->task.
					       command);
			    if (EMC_DEBUG & EMC_DEBUG_INTERP) {
				rcs_print
				    ("emcTaskPlanCommand(%s) called. (line_number=%d)\n",
				     ((char *) &emcStatus->task.command),
				     emcStatus->task.readLine);
			    }
			    // and execute it
			    execRetval = emcTaskPlanExecute(0);
			    if (EMC_DEBUG & EMC_DEBUG_INTERP) {
				rcs_print
				    ("emcTaskPlanExecute(0) return %d\n",
				     execRetval);
			    }
			    if (execRetval == -1 /* INTERP_ERROR */  ||
				execRetval > INTERP_MIN_ERROR || execRetval == 1	/* INTERP_EXIT
											 */ ) {
				// end of file
				emcStatus->task.interpState =
				    EMC_TASK_INTERP_WAITING;
			    } else if (execRetval == 2	/* INTERP_EXECUTE_FINISH
							 */ ) {
				// INTERP_EXECUTE_FINISH signifies
				// that no more reading should be done until
				// everything
				// outstanding is completed
				emcTaskPlanSetWait();
				if (EMC_DEBUG & EMC_DEBUG_INTERP) {
				    rcs_print
					("emcTaskPlanSetWait() called\n");
				}
				// and resynch interp WM
				emcTaskQueueCommand(&taskPlanSynchCmd);
			    } else if (execRetval != 0) {
				// end of file
				emcStatus->task.interpState =
				    EMC_TASK_INTERP_WAITING;
			    } else {

				// executed a good line
			    }

			    // throw the results away if we're supposed to
			    // read
			    // through it
			    if (programStartLine < 0 ||
				emcStatus->task.readLine <
				programStartLine) {
				// we're stepping over lines, so check them
				// for
				// limits, etc. and clear then out
				if (0 != checkInterpList(&interp_list,
							 emcStatus)) {
				    // problem with actions, so do same as we
				    // did
				    // for a bad read from emcTaskPlanRead()
				    // above
				    emcStatus->task.interpState =
					EMC_TASK_INTERP_WAITING;
				}
				// and clear it regardless
				interp_list.clear();
			    }

			    if (emcStatus->task.readLine < programStartLine) {
			    
				//update the position with our current position, as the other positions are only skipped through
				CANON_UPDATE_END_POINT(emcStatus->motion.traj.actualPosition.tran.x,
						       emcStatus->motion.traj.actualPosition.tran.y,
						       emcStatus->motion.traj.actualPosition.tran.z,
						       emcStatus->motion.traj.actualPosition.a,
						       emcStatus->motion.traj.actualPosition.b,
						       emcStatus->motion.traj.actualPosition.c,
						       emcStatus->motion.traj.actualPosition.u,
						       emcStatus->motion.traj.actualPosition.v,
						       emcStatus->motion.traj.actualPosition.w);
			    }

                            if (count++ < 1000
                                    && emcStatus->task.interpState == EMC_TASK_INTERP_READING
                                    && interp_list.len() <= EMC_TASK_INTERP_MAX_LEN * 2/3) {
                                goto interpret_again;
                            }

			}	// else read was OK, so execute
		    }		// else not emcTaskPlanIsWait
		}		// if interp len is less than max

		break;		// EMC_TASK_INTERP_READING

	    case EMC_TASK_INTERP_PAUSED:	// ON, AUTO, PAUSED
		switch (type) {
		case 0:
		case EMC_NULL_TYPE:
		    // no command
		    break;

		    // immediate commands

		case EMC_AXIS_SET_BACKLASH_TYPE:
		case EMC_AXIS_SET_HOMING_PARAMS_TYPE:
		case EMC_AXIS_SET_FERROR_TYPE:
		case EMC_AXIS_SET_MIN_FERROR_TYPE:
		case EMC_AXIS_SET_OUTPUT_TYPE:
		case EMC_AXIS_SET_STEP_PARAMS_TYPE:
		case EMC_TRAJ_PAUSE_TYPE:
		case EMC_TRAJ_RESUME_TYPE:
		case EMC_TRAJ_ABORT_TYPE:
		case EMC_TRAJ_SET_SCALE_TYPE:
		case EMC_TRAJ_SET_SPINDLE_SCALE_TYPE:
		case EMC_TRAJ_SET_FO_ENABLE_TYPE:
	        case EMC_TRAJ_SET_FH_ENABLE_TYPE:
		case EMC_TRAJ_SET_SO_ENABLE_TYPE:
		case EMC_SPINDLE_ON_TYPE:
		case EMC_SPINDLE_OFF_TYPE:
		case EMC_SPINDLE_BRAKE_RELEASE_TYPE:
		case EMC_SPINDLE_BRAKE_ENGAGE_TYPE:
		case EMC_SPINDLE_INCREASE_TYPE:
		case EMC_SPINDLE_DECREASE_TYPE:
		case EMC_SPINDLE_CONSTANT_TYPE:
		case EMC_COOLANT_MIST_ON_TYPE:
		case EMC_COOLANT_MIST_OFF_TYPE:
		case EMC_COOLANT_FLOOD_ON_TYPE:
		case EMC_COOLANT_FLOOD_OFF_TYPE:
		case EMC_LUBE_ON_TYPE:
		case EMC_LUBE_OFF_TYPE:
		case EMC_TASK_SET_MODE_TYPE:
		case EMC_TASK_SET_STATE_TYPE:
		case EMC_TASK_ABORT_TYPE:
		case EMC_TASK_PLAN_EXECUTE_TYPE:
		case EMC_TASK_PLAN_PAUSE_TYPE:
		case EMC_TASK_PLAN_RESUME_TYPE:
		case EMC_TASK_PLAN_SET_OPTIONAL_STOP_TYPE:
		case EMC_TASK_PLAN_SET_BLOCK_DELETE_TYPE:
		case EMC_TASK_PLAN_OPTIONAL_STOP_TYPE:
		case EMC_TRAJ_CLEAR_PROBE_TRIPPED_FLAG_TYPE:
		case EMC_TRAJ_PROBE_TYPE:
		case EMC_AUX_INPUT_WAIT_TYPE:
		case EMC_TRAJ_RIGID_TAP_TYPE:
		case EMC_SET_DEBUG_TYPE:
		    retval = emcTaskIssueCommand(emcCommand);
		    break;

		case EMC_TASK_PLAN_STEP_TYPE:
		    stepping = 1;
		    steppingWait = 0;
		    if (emcStatus->motion.traj.paused &&
			emcStatus->motion.traj.queue > 0) {
			// there are pending motions paused; step them
			emcTrajStep();
		    } else {
			emcStatus->task.interpState = (enum EMC_TASK_INTERP_ENUM) interpResumeState;
		    }
		    break;

		    // otherwise we can't handle it
		default:
		    sprintf(errstring,
			    _
			    ("can't do that (%s) in auto mode with the interpreter paused"),
			    emc_symbol_lookup(type));
		    emcOperatorError(0, errstring);
		    retval = -1;
		    break;

		}		// switch (type) in ON, AUTO, PAUSED

		break;		// EMC_TASK_INTERP_PAUSED

	    case EMC_TASK_INTERP_WAITING:
		// interpreter ran to end
		// handle input commands
		switch (type) {
		case 0:
		case EMC_NULL_TYPE:
		    // no command
		    break;

		    // immediate commands

		case EMC_AXIS_SET_BACKLASH_TYPE:
		case EMC_AXIS_SET_HOMING_PARAMS_TYPE:
		case EMC_AXIS_SET_FERROR_TYPE:
		case EMC_AXIS_SET_MIN_FERROR_TYPE:
		case EMC_AXIS_SET_OUTPUT_TYPE:
		case EMC_AXIS_SET_STEP_PARAMS_TYPE:
		case EMC_TRAJ_PAUSE_TYPE:
		case EMC_TRAJ_RESUME_TYPE:
		case EMC_TRAJ_ABORT_TYPE:
		case EMC_TRAJ_SET_SCALE_TYPE:
		case EMC_TRAJ_SET_SPINDLE_SCALE_TYPE:
		case EMC_TRAJ_SET_FO_ENABLE_TYPE:
	        case EMC_TRAJ_SET_FH_ENABLE_TYPE:
		case EMC_TRAJ_SET_SO_ENABLE_TYPE:
		case EMC_SPINDLE_INCREASE_TYPE:
		case EMC_SPINDLE_DECREASE_TYPE:
		case EMC_SPINDLE_CONSTANT_TYPE:
		case EMC_TASK_PLAN_EXECUTE_TYPE:
		case EMC_TASK_PLAN_PAUSE_TYPE:
		case EMC_TASK_PLAN_RESUME_TYPE:
		case EMC_TASK_PLAN_SET_OPTIONAL_STOP_TYPE:
		case EMC_TASK_PLAN_SET_BLOCK_DELETE_TYPE:
		case EMC_TASK_PLAN_OPTIONAL_STOP_TYPE:
		case EMC_TASK_SET_MODE_TYPE:
		case EMC_TASK_SET_STATE_TYPE:
		case EMC_TASK_ABORT_TYPE:
		case EMC_TRAJ_CLEAR_PROBE_TRIPPED_FLAG_TYPE:
		case EMC_TRAJ_PROBE_TYPE:
		case EMC_AUX_INPUT_WAIT_TYPE:
	        case EMC_TRAJ_RIGID_TAP_TYPE:
		case EMC_SET_DEBUG_TYPE:
		    retval = emcTaskIssueCommand(emcCommand);
		    break;

		case EMC_TASK_PLAN_STEP_TYPE:
		    stepping = 1;	// set stepping mode in case it's not
		    steppingWait = 0;	// clear the wait
		    break;

		    // otherwise we can't handle it
		default:
		    sprintf(errstring,
			    _
			    ("can't do that (%s) in auto mode with the interpreter waiting"),
			    emc_symbol_lookup(type));
		    emcOperatorError(0, errstring);
		    retval = -1;
		    break;

		}		// switch (type) in ON, AUTO, WAITING

		// now handle call logic
		// check for subsystems done
		if (interp_list.len() == 0 &&
		    emcTaskCommand == 0 &&
		    emcStatus->motion.traj.queue == 0 &&
		    emcStatus->io.status == RCS_DONE)
		    // finished
		{
		    int was_open = taskplanopen;
		    if (was_open) {
			emcTaskPlanClose();
			if (EMC_DEBUG & EMC_DEBUG_INTERP && was_open) {
			    rcs_print
				("emcTaskPlanClose() called at %s:%d\n",
				 __FILE__, __LINE__);
			}
			// then resynch interpreter
			emcTaskQueueCommand(&taskPlanSynchCmd);
		    } else {
			emcStatus->task.interpState = EMC_TASK_INTERP_IDLE;
		    }
		    emcStatus->task.readLine = 0;
		    interp_list.set_line_number(0);
		} else {
		    // still executing
		}

		break;		// end of case EMC_TASK_INTERP_WAITING

	    default:
		// coding error
		rcs_print_error("invalid mode(%d)", emcStatus->task.mode);
		retval = -1;
		break;

	    }			// switch (mode) in ON, AUTO

	    break;		// case EMC_TASK_MODE_AUTO

	case EMC_TASK_MODE_MDI:	// ON, MDI
	    switch (type) {
	    case 0:
	    case EMC_NULL_TYPE:
		// no command
		break;

		// immediate commands

	    case EMC_AXIS_SET_BACKLASH_TYPE:
	    case EMC_AXIS_SET_HOMING_PARAMS_TYPE:
	    case EMC_AXIS_SET_FERROR_TYPE:
	    case EMC_AXIS_SET_MIN_FERROR_TYPE:
	    case EMC_AXIS_SET_OUTPUT_TYPE:
	    case EMC_AXIS_SET_STEP_PARAMS_TYPE:
	    case EMC_TRAJ_SET_SCALE_TYPE:
	    case EMC_TRAJ_SET_SPINDLE_SCALE_TYPE:
	    case EMC_TRAJ_SET_FO_ENABLE_TYPE:
	    case EMC_TRAJ_SET_FH_ENABLE_TYPE:
	    case EMC_TRAJ_SET_SO_ENABLE_TYPE:
	    case EMC_SPINDLE_ON_TYPE:
	    case EMC_SPINDLE_OFF_TYPE:
	    case EMC_SPINDLE_BRAKE_RELEASE_TYPE:
	    case EMC_SPINDLE_BRAKE_ENGAGE_TYPE:
	    case EMC_SPINDLE_INCREASE_TYPE:
	    case EMC_SPINDLE_DECREASE_TYPE:
	    case EMC_SPINDLE_CONSTANT_TYPE:
	    case EMC_COOLANT_MIST_ON_TYPE:
	    case EMC_COOLANT_MIST_OFF_TYPE:
	    case EMC_COOLANT_FLOOD_ON_TYPE:
	    case EMC_COOLANT_FLOOD_OFF_TYPE:
	    case EMC_LUBE_ON_TYPE:
	    case EMC_LUBE_OFF_TYPE:
	    case EMC_TASK_SET_MODE_TYPE:
	    case EMC_TASK_SET_STATE_TYPE:
	    case EMC_TASK_PLAN_INIT_TYPE:
	    case EMC_TASK_PLAN_OPEN_TYPE:
	    case EMC_TASK_PLAN_EXECUTE_TYPE:
	    case EMC_TASK_PLAN_PAUSE_TYPE:
	    case EMC_TASK_PLAN_SET_OPTIONAL_STOP_TYPE:
	    case EMC_TASK_PLAN_SET_BLOCK_DELETE_TYPE:
	    case EMC_TASK_PLAN_RESUME_TYPE:
	    case EMC_TASK_PLAN_OPTIONAL_STOP_TYPE:
	    case EMC_TASK_ABORT_TYPE:
	    case EMC_TRAJ_CLEAR_PROBE_TRIPPED_FLAG_TYPE:
	    case EMC_TRAJ_PROBE_TYPE:
	    case EMC_AUX_INPUT_WAIT_TYPE:
	    case EMC_TRAJ_RIGID_TAP_TYPE:
	    case EMC_SET_DEBUG_TYPE:
		retval = emcTaskIssueCommand(emcCommand);
		break;

	    case EMC_TOOL_LOAD_TOOL_TABLE_TYPE:
	    case EMC_TOOL_SET_OFFSET_TYPE:
		// send to IO
		emcTaskQueueCommand(emcCommand);
		// signify no more reading
		emcTaskPlanSetWait();
		if (EMC_DEBUG & EMC_DEBUG_INTERP) {
		    rcs_print("emcTaskPlanSetWait() called\n");
		}
		// then resynch interpreter
		emcTaskQueueCommand(&taskPlanSynchCmd);
		break;

		// otherwise we can't handle it
	    default:

		sprintf(errstring, _("can't do that (%s) in MDI mode"),
			emc_symbol_lookup(type));
		emcOperatorError(0, errstring);
		retval = -1;
		break;

	    }			// switch (type) in ON, MDI

	    break;		// case EMC_TASK_MODE_MDI

	default:
	    break;

	}			// switch (mode)

	break;			// case EMC_TASK_STATE_ON

    default:
	break;

    }				// switch (task.state)

    return retval;
}

/*
   emcTaskCheckPreconditions() is called for commands on the interp_list.
   Immediate commands, i.e., commands sent from calls to emcTaskIssueCommand()
   in emcTaskPlan() directly, are not handled here.

   The return value is a state for emcTaskExecute() to wait on, e.g.,
   EMC_TASK_EXEC_WAITING_FOR_MOTION, before the command can be sent out.
   */
static int emcTaskCheckPreconditions(NMLmsg * cmd)
{
    if (0 == cmd) {
	return EMC_TASK_EXEC_DONE;
    }

    switch (cmd->type) {
	// operator messages, if queued, will go out when everything before
	// them is done
    case EMC_OPERATOR_ERROR_TYPE:
    case EMC_OPERATOR_TEXT_TYPE:
    case EMC_OPERATOR_DISPLAY_TYPE:
    case EMC_SYSTEM_CMD_TYPE:
    case EMC_TRAJ_PROBE_TYPE:	// prevent blending of this
    case EMC_TRAJ_RIGID_TAP_TYPE: //and this
    case EMC_TRAJ_CLEAR_PROBE_TRIPPED_FLAG_TYPE:	// and this
    case EMC_AUX_INPUT_WAIT_TYPE:
	return EMC_TASK_EXEC_WAITING_FOR_MOTION_AND_IO;
	break;

    case EMC_TRAJ_LINEAR_MOVE_TYPE:
    case EMC_TRAJ_CIRCULAR_MOVE_TYPE:
    case EMC_TRAJ_SET_VELOCITY_TYPE:
    case EMC_TRAJ_SET_ACCELERATION_TYPE:
    case EMC_TRAJ_SET_TERM_COND_TYPE:
    case EMC_TRAJ_SET_SPINDLESYNC_TYPE:
    case EMC_TRAJ_SET_FO_ENABLE_TYPE:
    case EMC_TRAJ_SET_FH_ENABLE_TYPE:
    case EMC_TRAJ_SET_SO_ENABLE_TYPE:
	return EMC_TASK_EXEC_WAITING_FOR_IO;
	break;

    case EMC_TRAJ_SET_OFFSET_TYPE:
	// this applies the tool length offset variable after previous
	// motions
    case EMC_TRAJ_SET_ORIGIN_TYPE:
	// this applies the program origin after previous motions
	return EMC_TASK_EXEC_WAITING_FOR_MOTION;
	break;

    case EMC_TOOL_LOAD_TYPE:
    case EMC_TOOL_UNLOAD_TYPE:
    case EMC_COOLANT_MIST_ON_TYPE:
    case EMC_COOLANT_MIST_OFF_TYPE:
    case EMC_COOLANT_FLOOD_ON_TYPE:
    case EMC_COOLANT_FLOOD_OFF_TYPE:
    case EMC_SPINDLE_ON_TYPE:
    case EMC_SPINDLE_OFF_TYPE:
	return EMC_TASK_EXEC_WAITING_FOR_MOTION_AND_IO;
	break;

    case EMC_TOOL_PREPARE_TYPE:
    case EMC_LUBE_ON_TYPE:
    case EMC_LUBE_OFF_TYPE:
	return EMC_TASK_EXEC_WAITING_FOR_IO;
	break;

    case EMC_TOOL_LOAD_TOOL_TABLE_TYPE:
    case EMC_TOOL_SET_OFFSET_TYPE:
	return EMC_TASK_EXEC_WAITING_FOR_MOTION_AND_IO;
	break;

    case EMC_TASK_PLAN_PAUSE_TYPE:
    case EMC_TASK_PLAN_OPTIONAL_STOP_TYPE:
	/* pause on the interp list is queued, so wait until all are done */
	return EMC_TASK_EXEC_WAITING_FOR_MOTION_AND_IO;
	break;

    case EMC_TASK_PLAN_END_TYPE:
	return EMC_TASK_EXEC_WAITING_FOR_MOTION_AND_IO;
	break;

    case EMC_TASK_PLAN_INIT_TYPE:
    case EMC_TASK_PLAN_RUN_TYPE:
    case EMC_TASK_PLAN_SYNCH_TYPE:
    case EMC_TASK_PLAN_EXECUTE_TYPE:
	return EMC_TASK_EXEC_WAITING_FOR_MOTION_AND_IO;
	break;

    case EMC_TRAJ_DELAY_TYPE:
	return EMC_TASK_EXEC_WAITING_FOR_MOTION_AND_IO;
	break;

    case EMC_MOTION_SET_AOUT_TYPE:
	if (((EMC_MOTION_SET_AOUT *) cmd)->now) {
    	    return EMC_TASK_EXEC_WAITING_FOR_MOTION;
	}
	return EMC_TASK_EXEC_DONE;
	break;

    case EMC_MOTION_SET_DOUT_TYPE:
	if (((EMC_MOTION_SET_DOUT *) cmd)->now) {
    	    return EMC_TASK_EXEC_WAITING_FOR_MOTION;
	}
	return EMC_TASK_EXEC_DONE;
	break;

    case EMC_MOTION_ADAPTIVE_TYPE:
	return EMC_TASK_EXEC_WAITING_FOR_MOTION;
	break;


    default:
	// unrecognized command
	if (EMC_DEBUG & EMC_DEBUG_TASK_ISSUE) {
	    rcs_print_error("preconditions: unrecognized command %d:%s\n",
			    cmd->type, emc_symbol_lookup(cmd->type));
	}
	return EMC_TASK_EXEC_ERROR;
	break;
    }

    return EMC_TASK_EXEC_DONE;
}

// puts command on interp list
int emcTaskQueueCommand(NMLmsg * cmd)
{
    if (0 == cmd) {
	return 0;
    }

    interp_list.append(cmd);

    return 0;
}

// issues command immediately
static int emcTaskIssueCommand(NMLmsg * cmd)
{
    int retval = 0;
    int execRetval = 0;

    if (0 == cmd) {
        if (EMC_DEBUG & EMC_DEBUG_TASK_ISSUE) {
            printf("emcTaskIssueCommand() null command\n");
        }
	return 0;
    }
    if (EMC_DEBUG & EMC_DEBUG_TASK_ISSUE) {
	rcs_print("Issuing %s -- \t (%s)\n", emcSymbolLookup(cmd->type),
		  emcCommandBuffer->msg2str(cmd));
    }
    switch (cmd->type) {
	// general commands

    case EMC_OPERATOR_ERROR_TYPE:
	retval = emcOperatorError(((EMC_OPERATOR_ERROR *) cmd)->id,
				  ((EMC_OPERATOR_ERROR *) cmd)->error);
	break;

    case EMC_OPERATOR_TEXT_TYPE:
	retval = emcOperatorText(((EMC_OPERATOR_TEXT *) cmd)->id,
				 ((EMC_OPERATOR_TEXT *) cmd)->text);
	break;

    case EMC_OPERATOR_DISPLAY_TYPE:
	retval = emcOperatorDisplay(((EMC_OPERATOR_DISPLAY *) cmd)->id,
				    ((EMC_OPERATOR_DISPLAY *) cmd)->
				    display);
	break;

    case EMC_SYSTEM_CMD_TYPE:
	retval = emcSystemCmd(((EMC_SYSTEM_CMD *) cmd)->string);
	break;

	// axis commands

    case EMC_AXIS_DISABLE_TYPE:
	disable_msg = (EMC_AXIS_DISABLE *) cmd;
	retval = emcAxisDisable(disable_msg->axis);
	break;

    case EMC_AXIS_ENABLE_TYPE:
	enable_msg = (EMC_AXIS_ENABLE *) cmd;
	retval = emcAxisEnable(enable_msg->axis);
	break;

    case EMC_AXIS_HOME_TYPE:
	home_msg = (EMC_AXIS_HOME *) cmd;
	retval = emcAxisHome(home_msg->axis);
	break;

    case EMC_AXIS_JOG_TYPE:
	jog_msg = (EMC_AXIS_JOG *) cmd;
	retval = emcAxisJog(jog_msg->axis, jog_msg->vel);
	break;

    case EMC_AXIS_ABORT_TYPE:
	axis_abort_msg = (EMC_AXIS_ABORT *) cmd;
	retval = emcAxisAbort(axis_abort_msg->axis);
	break;

    case EMC_AXIS_INCR_JOG_TYPE:
	incr_jog_msg = (EMC_AXIS_INCR_JOG *) cmd;
	retval = emcAxisIncrJog(incr_jog_msg->axis,
				incr_jog_msg->incr, incr_jog_msg->vel);
	break;

    case EMC_AXIS_ABS_JOG_TYPE:
	abs_jog_msg = (EMC_AXIS_ABS_JOG *) cmd;
	retval = emcAxisAbsJog(abs_jog_msg->axis,
			       abs_jog_msg->pos, abs_jog_msg->vel);
	break;

    case EMC_AXIS_SET_BACKLASH_TYPE:
	set_backlash_msg = (EMC_AXIS_SET_BACKLASH *) cmd;
	retval =
	    emcAxisSetBacklash(set_backlash_msg->axis,
			       set_backlash_msg->backlash);
	break;

    case EMC_AXIS_SET_HOMING_PARAMS_TYPE:
	set_homing_params_msg = (EMC_AXIS_SET_HOMING_PARAMS *) cmd;
	retval = emcAxisSetHomingParams(set_homing_params_msg->axis,
					set_homing_params_msg->home,
					set_homing_params_msg->offset,
					set_homing_params_msg->search_vel,
					set_homing_params_msg->latch_vel,
					set_homing_params_msg->use_index,
					set_homing_params_msg->ignore_limits,
					set_homing_params_msg->is_shared,
					set_homing_params_msg->home_sequence);
	break;

    case EMC_AXIS_SET_FERROR_TYPE:
	set_ferror_msg = (EMC_AXIS_SET_FERROR *) cmd;
	retval = emcAxisSetFerror(set_ferror_msg->axis,
				  set_ferror_msg->ferror);
	break;

    case EMC_AXIS_SET_MIN_FERROR_TYPE:
	set_min_ferror_msg = (EMC_AXIS_SET_MIN_FERROR *) cmd;
	retval = emcAxisSetMinFerror(set_min_ferror_msg->axis,
				     set_min_ferror_msg->ferror);
	break;

    case EMC_AXIS_SET_MAX_POSITION_LIMIT_TYPE:
	set_max_limit_msg = (EMC_AXIS_SET_MAX_POSITION_LIMIT *) cmd;
	retval = emcAxisSetMaxPositionLimit(set_max_limit_msg->axis,
					    set_max_limit_msg->limit);
	break;

    case EMC_AXIS_SET_MIN_POSITION_LIMIT_TYPE:
	set_min_limit_msg = (EMC_AXIS_SET_MIN_POSITION_LIMIT *) cmd;
	retval = emcAxisSetMinPositionLimit(set_min_limit_msg->axis,
					    set_min_limit_msg->limit);
	break;

    case EMC_AXIS_HALT_TYPE:
	axis_halt_msg = (EMC_AXIS_HALT *) cmd;
	retval = emcAxisHalt(axis_halt_msg->axis);
	break;

    case EMC_AXIS_OVERRIDE_LIMITS_TYPE:
	axis_lim_msg = (EMC_AXIS_OVERRIDE_LIMITS *) cmd;
	retval = emcAxisOverrideLimits(axis_lim_msg->axis);
	break;

    case EMC_AXIS_LOAD_COMP_TYPE:
	axis_load_comp_msg = (EMC_AXIS_LOAD_COMP *) cmd;
	retval = emcAxisLoadComp(axis_load_comp_msg->axis,
				 axis_load_comp_msg->file,
				 axis_load_comp_msg->type);
	break;

	// traj commands

    case EMC_TRAJ_SET_SCALE_TYPE:
	emcTrajSetScaleMsg = (EMC_TRAJ_SET_SCALE *) cmd;
	retval = emcTrajSetScale(emcTrajSetScaleMsg->scale);
	break;

    case EMC_TRAJ_SET_SPINDLE_SCALE_TYPE:
	emcTrajSetSpindleScaleMsg = (EMC_TRAJ_SET_SPINDLE_SCALE *) cmd;
	retval = emcTrajSetSpindleScale(emcTrajSetSpindleScaleMsg->scale);
	break;

    case EMC_TRAJ_SET_FO_ENABLE_TYPE:
	retval = emcTrajSetFOEnable(((EMC_TRAJ_SET_FO_ENABLE *) cmd)->mode);  // feed override enable/disable
	break;

    case EMC_TRAJ_SET_FH_ENABLE_TYPE:
	retval = emcTrajSetFHEnable(((EMC_TRAJ_SET_FH_ENABLE *) cmd)->mode); //feed hold enable/disable
	break;

    case EMC_TRAJ_SET_SO_ENABLE_TYPE:
	retval = emcTrajSetSOEnable(((EMC_TRAJ_SET_SO_ENABLE *) cmd)->mode); //spindle speed override enable/disable
	break;

    case EMC_TRAJ_SET_VELOCITY_TYPE:
	emcTrajSetVelocityMsg = (EMC_TRAJ_SET_VELOCITY *) cmd;
	retval = emcTrajSetVelocity(emcTrajSetVelocityMsg->velocity,
			emcTrajSetVelocityMsg->ini_maxvel);
	break;

    case EMC_TRAJ_SET_ACCELERATION_TYPE:
	emcTrajSetAccelerationMsg = (EMC_TRAJ_SET_ACCELERATION *) cmd;
	retval = emcTrajSetAcceleration(emcTrajSetAccelerationMsg->acceleration);
	break;

    case EMC_TRAJ_LINEAR_MOVE_TYPE:
	emcTrajLinearMoveMsg = (EMC_TRAJ_LINEAR_MOVE *) cmd;
        retval = emcTrajLinearMove(emcTrajLinearMoveMsg->end,
                emcTrajLinearMoveMsg->type, emcTrajLinearMoveMsg->vel,
                emcTrajLinearMoveMsg->ini_maxvel, emcTrajLinearMoveMsg->acc);
	break;

    case EMC_TRAJ_CIRCULAR_MOVE_TYPE:
	emcTrajCircularMoveMsg = (EMC_TRAJ_CIRCULAR_MOVE *) cmd;
        retval = emcTrajCircularMove(emcTrajCircularMoveMsg->end,
                emcTrajCircularMoveMsg->center, emcTrajCircularMoveMsg->normal,
                emcTrajCircularMoveMsg->turn, emcTrajCircularMoveMsg->type,
                emcTrajCircularMoveMsg->vel,
                emcTrajCircularMoveMsg->ini_maxvel,
                emcTrajCircularMoveMsg->acc);
	break;

    case EMC_TRAJ_PAUSE_TYPE:
	retval = emcTrajPause();
	break;

    case EMC_TRAJ_RESUME_TYPE:
	retval = emcTrajResume();
	break;

    case EMC_TRAJ_ABORT_TYPE:
	retval = emcTrajAbort();
	break;

    case EMC_TRAJ_DELAY_TYPE:
	emcTrajDelayMsg = (EMC_TRAJ_DELAY *) cmd;
	// set the timeout clock to expire at 'now' + delay time
	taskExecDelayTimeout = etime() + emcTrajDelayMsg->delay;
	retval = 0;
	break;

    case EMC_TRAJ_SET_TERM_COND_TYPE:
	emcTrajSetTermCondMsg = (EMC_TRAJ_SET_TERM_COND *) cmd;
	retval = emcTrajSetTermCond(emcTrajSetTermCondMsg->cond, emcTrajSetTermCondMsg->tolerance);
	break;

    case EMC_TRAJ_SET_SPINDLESYNC_TYPE:
        emcTrajSetSpindlesyncMsg = (EMC_TRAJ_SET_SPINDLESYNC *) cmd;
        retval = emcTrajSetSpindleSync(emcTrajSetSpindlesyncMsg->feed_per_revolution, emcTrajSetSpindlesyncMsg->velocity_mode);
        break;

    case EMC_TRAJ_SET_OFFSET_TYPE:
	// update tool offset
	emcStatus->task.toolOffset.tran.z =
	    ((EMC_TRAJ_SET_OFFSET *) cmd)->offset.tran.z;
	emcStatus->task.toolOffset.tran.x =
	    ((EMC_TRAJ_SET_OFFSET *) cmd)->offset.tran.x;
	retval = 0;
	break;

    case EMC_TRAJ_SET_ORIGIN_TYPE:
	// struct-copy program origin
	emcStatus->task.origin = ((EMC_TRAJ_SET_ORIGIN *) cmd)->origin;
	retval = 0;
	break;
    case EMC_TRAJ_CLEAR_PROBE_TRIPPED_FLAG_TYPE:
	retval = emcTrajClearProbeTrippedFlag();
	break;

    case EMC_TRAJ_PROBE_TYPE:
	retval = emcTrajProbe(
	    ((EMC_TRAJ_PROBE *) cmd)->pos, 
	    ((EMC_TRAJ_PROBE *) cmd)->type,
	    ((EMC_TRAJ_PROBE *) cmd)->vel,
            ((EMC_TRAJ_PROBE *) cmd)->ini_maxvel,  
	    ((EMC_TRAJ_PROBE *) cmd)->acc);
	break;

    case EMC_AUX_INPUT_WAIT_TYPE:
	emcAuxInputWaitMsg = (EMC_AUX_INPUT_WAIT *) cmd;
	if (emcAuxInputWaitMsg->timeout == WAIT_MODE_IMMEDIATE) { //nothing to do, CANON will get the needed value when asked by the interp
	    emcStatus->task.input_timeout = 0; // no timeout can occur
	    emcAuxInputWaitIndex = -1;
	} else {
	    emcAuxInputWaitType = emcAuxInputWaitMsg->wait_type; // remember what we are waiting for 
	    emcAuxInputWaitIndex = emcAuxInputWaitMsg->index; // remember the input to look at
	    emcStatus->task.input_timeout = 2; // set timeout flag, gets cleared if input changes before timeout happens
	    // set the timeout clock to expire at 'now' + delay time
	    taskExecDelayTimeout = etime() + emcAuxInputWaitMsg->timeout;
	}
	break;

    case EMC_TRAJ_RIGID_TAP_TYPE:
	retval = emcTrajRigidTap(((EMC_TRAJ_RIGID_TAP *) cmd)->pos,
	        ((EMC_TRAJ_RIGID_TAP *) cmd)->vel,
        	((EMC_TRAJ_RIGID_TAP *) cmd)->ini_maxvel,  
		((EMC_TRAJ_RIGID_TAP *) cmd)->acc);
	break;

    case EMC_TRAJ_SET_TELEOP_ENABLE_TYPE:
	if (((EMC_TRAJ_SET_TELEOP_ENABLE *) cmd)->enable) {
	    retval = emcTrajSetMode(EMC_TRAJ_MODE_TELEOP);
	} else {
	    retval = emcTrajSetMode(EMC_TRAJ_MODE_FREE);
	}
	break;

    case EMC_TRAJ_SET_TELEOP_VECTOR_TYPE:
	retval =
	    emcTrajSetTeleopVector(((EMC_TRAJ_SET_TELEOP_VECTOR *) cmd)->
				   vector);
	break;

    case EMC_MOTION_SET_AOUT_TYPE:
	retval = emcMotionSetAout(((EMC_MOTION_SET_AOUT *) cmd)->index,
				  ((EMC_MOTION_SET_AOUT *) cmd)->start,
				  ((EMC_MOTION_SET_AOUT *) cmd)->end,
				  ((EMC_MOTION_SET_AOUT *) cmd)->now);
	break;

    case EMC_MOTION_SET_DOUT_TYPE:
	retval = emcMotionSetDout(((EMC_MOTION_SET_DOUT *) cmd)->index,
				  ((EMC_MOTION_SET_DOUT *) cmd)->start,
				  ((EMC_MOTION_SET_DOUT *) cmd)->end,
				  ((EMC_MOTION_SET_DOUT *) cmd)->now);
	break;

    case EMC_MOTION_ADAPTIVE_TYPE:
	retval = emcTrajSetAFEnable(((EMC_MOTION_ADAPTIVE *) cmd)->status);
	break;

    case EMC_SET_DEBUG_TYPE:
	/* set the debug level here */
	EMC_DEBUG = ((EMC_SET_DEBUG *) cmd)->debug;
	/* and in IO and motion */
	emcIoSetDebug(EMC_DEBUG);
	emcMotionSetDebug(EMC_DEBUG);
	/* and reflect it in the status-- this isn't updated continually */
	emcStatus->debug = EMC_DEBUG;
	break;

	// unimplemented ones

	// IO commands

    case EMC_SPINDLE_ON_TYPE:
	spindle_on_msg = (EMC_SPINDLE_ON *) cmd;
	retval = emcSpindleOn(spindle_on_msg->speed, spindle_on_msg->factor, spindle_on_msg->xoffset);
	break;

    case EMC_SPINDLE_OFF_TYPE:
	retval = emcSpindleOff();
	break;

    case EMC_SPINDLE_BRAKE_RELEASE_TYPE:
	retval = emcSpindleBrakeRelease();
	break;

    case EMC_SPINDLE_INCREASE_TYPE:
	retval = emcSpindleIncrease();
	break;

    case EMC_SPINDLE_DECREASE_TYPE:
	retval = emcSpindleDecrease();
	break;

    case EMC_SPINDLE_CONSTANT_TYPE:
	retval = emcSpindleConstant();
	break;

    case EMC_SPINDLE_BRAKE_ENGAGE_TYPE:
	retval = emcSpindleBrakeEngage();
	break;

    case EMC_COOLANT_MIST_ON_TYPE:
	retval = emcCoolantMistOn();
	break;

    case EMC_COOLANT_MIST_OFF_TYPE:
	retval = emcCoolantMistOff();
	break;

    case EMC_COOLANT_FLOOD_ON_TYPE:
	retval = emcCoolantFloodOn();
	break;

    case EMC_COOLANT_FLOOD_OFF_TYPE:
	retval = emcCoolantFloodOff();
	break;

    case EMC_LUBE_ON_TYPE:
	retval = emcLubeOn();
	break;

    case EMC_LUBE_OFF_TYPE:
	retval = emcLubeOff();
	break;

    case EMC_TOOL_PREPARE_TYPE:
	tool_prepare_msg = (EMC_TOOL_PREPARE *) cmd;
	retval = emcToolPrepare(tool_prepare_msg->tool);
	break;

    case EMC_TOOL_LOAD_TYPE:
	retval = emcToolLoad();
	break;

    case EMC_TOOL_UNLOAD_TYPE:
	retval = emcToolUnload();
	break;

    case EMC_TOOL_LOAD_TOOL_TABLE_TYPE:
	load_tool_table_msg = (EMC_TOOL_LOAD_TOOL_TABLE *) cmd;
	retval = emcToolLoadToolTable(load_tool_table_msg->file);
	break;

    case EMC_TOOL_SET_OFFSET_TYPE:
	emc_tool_set_offset_msg = (EMC_TOOL_SET_OFFSET *) cmd;
	retval = emcToolSetOffset(emc_tool_set_offset_msg->tool,
				  emc_tool_set_offset_msg->length,
				  emc_tool_set_offset_msg->diameter);
	break;

	// task commands

    case EMC_TASK_INIT_TYPE:
	retval = emcTaskInit();
	break;

    case EMC_TASK_ABORT_TYPE:
	// abort everything
	emcTaskAbort();
	retval = 0;
	break;

	// mode and state commands

    case EMC_TASK_SET_MODE_TYPE:
	mode_msg = (EMC_TASK_SET_MODE *) cmd;
	if (emcStatus->task.mode == EMC_TASK_MODE_AUTO &&
	    emcStatus->task.interpState != EMC_TASK_INTERP_IDLE &&
	    mode_msg->mode != EMC_TASK_MODE_AUTO) {
	    emcOperatorError(0, "Can't switch mode while mode is AUTO and interpreter is not IDLE\n");
	} else { // we can honour the modeswitch
	    if (mode_msg->mode == EMC_TASK_MODE_MANUAL &&
		emcStatus->task.mode != EMC_TASK_MODE_MANUAL) {
		// leaving auto or mdi mode for manual

		/*! \todo FIXME-- duplicate code for abort,
	        also near end of main, when aborting on subordinate errors,
	        and in emcTaskExecute() */

		// abort everything
		emcTaskAbort();

		// without emcTaskPlanClose(), a new run command resumes at
		// aborted line-- feature that may be considered later
		{
		    int was_open = taskplanopen;
		    emcTaskPlanClose();
		    if (EMC_DEBUG & EMC_DEBUG_INTERP && was_open) {
			rcs_print("emcTaskPlanClose() called at %s:%d\n",
			      __FILE__, __LINE__);
		    }
		}

		// clear out the pending command
		emcTaskCommand = 0;
		interp_list.clear();

		// clear out the interpreter state
		emcStatus->task.interpState = EMC_TASK_INTERP_IDLE;
		emcStatus->task.execState = EMC_TASK_EXEC_DONE;
		stepping = 0;
		steppingWait = 0;

		// now queue up command to resynch interpreter
		emcTaskQueueCommand(&taskPlanSynchCmd);
		retval = 0;
	    }
	    retval = emcTaskSetMode(mode_msg->mode);
	}
	break;

    case EMC_TASK_SET_STATE_TYPE:
	state_msg = (EMC_TASK_SET_STATE *) cmd;
	retval = emcTaskSetState(state_msg->state);
	break;

	// interpreter commands

    case EMC_TASK_PLAN_OPEN_TYPE:
	open_msg = (EMC_TASK_PLAN_OPEN *) cmd;
	retval = emcTaskPlanOpen(open_msg->file);
	if (EMC_DEBUG & EMC_DEBUG_INTERP) {
	    rcs_print("emcTaskPlanOpen(%s) returned %d\n", open_msg->file,
		      retval);
	}
	if (retval > INTERP_MIN_ERROR) {
	    retval = -1;
	}
	if (-1 == retval) {
	    emcOperatorError(0, _("can't open %s"), open_msg->file);
	} else {
	    strcpy(emcStatus->task.file, open_msg->file);
	    retval = 0;
	}
	break;

    case EMC_TASK_PLAN_EXECUTE_TYPE:
	stepping = 0;
	steppingWait = 0;
	execute_msg = (EMC_TASK_PLAN_EXECUTE *) cmd;
	if (execute_msg->command[0] != 0) {
	    if (emcStatus->task.mode == EMC_TASK_MODE_MDI) {
		interp_list.set_line_number(--pseudoMdiLineNumber);
	    }
	    execRetval = emcTaskPlanExecute(execute_msg->command);
	    if (EMC_DEBUG & EMC_DEBUG_INTERP) {
		rcs_print("emcTaskPlanExecute(%s) returned %d\n",
			  execute_msg->command, execRetval);
	    }
	    if (execRetval == 2 /* INTERP_ENDFILE */ ) {
		// this is an end-of-file
		// need to flush execution, so signify no more reading
		// until all is done
		emcTaskPlanSetWait();
		if (EMC_DEBUG & EMC_DEBUG_INTERP) {
		    rcs_print("emcTaskPlanSetWait() called\n");
		}
		// and resynch the interpreter WM
		emcTaskQueueCommand(&taskPlanSynchCmd);
		// it's success, so retval really is 0
		retval = 0;
	    } else if (execRetval != 0) {
		retval = -1;
	    } else {
		// other codes are OK
		retval = 0;
	    }
	}
	break;

    case EMC_TASK_PLAN_RUN_TYPE:
	stepping = 0;
	steppingWait = 0;
	if (!taskplanopen && emcStatus->task.file[0] != 0) {
	    emcTaskPlanOpen(emcStatus->task.file);
	}
	run_msg = (EMC_TASK_PLAN_RUN *) cmd;
	programStartLine = run_msg->line;
	emcStatus->task.interpState = EMC_TASK_INTERP_READING;
	retval = 0;
	break;

    case EMC_TASK_PLAN_PAUSE_TYPE:
	emcTrajPause();
	if (emcStatus->task.interpState != EMC_TASK_INTERP_PAUSED) {
	    interpResumeState = emcStatus->task.interpState;
	}
	emcStatus->task.interpState = EMC_TASK_INTERP_PAUSED;
	retval = 0;
	break;

    case EMC_TASK_PLAN_OPTIONAL_STOP_TYPE:
	if (GET_OPTIONAL_PROGRAM_STOP() == ON) {
	    emcTrajPause();
	    if (emcStatus->task.interpState != EMC_TASK_INTERP_PAUSED) {
		interpResumeState = emcStatus->task.interpState;
	    }
	    emcStatus->task.interpState = EMC_TASK_INTERP_PAUSED;
	}
	retval = 0;
	break;

    case EMC_TASK_PLAN_RESUME_TYPE:
	emcTrajResume();
	emcStatus->task.interpState =
	    (enum EMC_TASK_INTERP_ENUM) interpResumeState;
	stepping = 0;
	steppingWait = 0;
	retval = 0;
	break;

    case EMC_TASK_PLAN_END_TYPE:
	retval = 0;
	break;

    case EMC_TASK_PLAN_INIT_TYPE:
	retval = emcTaskPlanInit();
	if (EMC_DEBUG & EMC_DEBUG_INTERP) {
	    rcs_print("emcTaskPlanInit() returned %d\n", retval);
	}
	if (retval > INTERP_MIN_ERROR) {
	    retval = -1;
	}
	break;

    case EMC_TASK_PLAN_SYNCH_TYPE:
	retval = emcTaskPlanSynch();
	if (EMC_DEBUG & EMC_DEBUG_INTERP) {
	    rcs_print("emcTaskPlanSynch() returned %d\n", retval);
	}
	if (retval > INTERP_MIN_ERROR) {
	    retval = -1;
	}
	break;

    case EMC_TASK_PLAN_SET_OPTIONAL_STOP_TYPE:
	os_msg = (EMC_TASK_PLAN_SET_OPTIONAL_STOP *) cmd;
	emcTaskPlanSetOptionalStop(os_msg->state);
	retval = 0;
	break;

    case EMC_TASK_PLAN_SET_BLOCK_DELETE_TYPE:
	bd_msg = (EMC_TASK_PLAN_SET_BLOCK_DELETE *) cmd;
	emcTaskPlanSetBlockDelete(bd_msg->state);
	retval = 0;
	break;

     default:
	// unrecognized command
	if (EMC_DEBUG & EMC_DEBUG_TASK_ISSUE) {
	    rcs_print_error("ignoring issue of unknown command %d:%s\n",
			    cmd->type, emc_symbol_lookup(cmd->type));
	}
	retval = 0;		// don't consider this an error
	break;
    }

    if (retval == -1) {
	if (EMC_DEBUG & EMC_DEBUG_TASK_ISSUE) {
	    rcs_print_error("error executing command %d:%s\n", cmd->type,
			    emc_symbol_lookup(cmd->type));
	}
    }
/* debug */
    if ((EMC_DEBUG & EMC_DEBUG_TASK_ISSUE) && retval) {
    	printf("emcTaskIssueCommand() returning: %d\n", retval);
    }
    return retval;
}

/*
   emcTaskCheckPostconditions() is called for commands on the interp_list.
   Immediate commands, i.e., commands sent from calls to emcTaskIssueCommand()
   in emcTaskPlan() directly, are not handled here.

   The return value is a state for emcTaskExecute() to wait on, e.g.,
   EMC_TASK_EXEC_WAITING_FOR_MOTION, after the command has finished and
   before any other commands can be sent out.
   */
static int emcTaskCheckPostconditions(NMLmsg * cmd)
{
    if (0 == cmd) {
	return EMC_TASK_EXEC_DONE;
    }

    switch (cmd->type) {
    case EMC_OPERATOR_ERROR_TYPE:
    case EMC_OPERATOR_TEXT_TYPE:
    case EMC_OPERATOR_DISPLAY_TYPE:
	return EMC_TASK_EXEC_DONE;
	break;

    case EMC_SYSTEM_CMD_TYPE:
	return EMC_TASK_EXEC_WAITING_FOR_SYSTEM_CMD;
	break;

    case EMC_TRAJ_LINEAR_MOVE_TYPE:
    case EMC_TRAJ_CIRCULAR_MOVE_TYPE:
    case EMC_TRAJ_SET_VELOCITY_TYPE:
    case EMC_TRAJ_SET_ACCELERATION_TYPE:
    case EMC_TRAJ_SET_TERM_COND_TYPE:
    case EMC_TRAJ_SET_SPINDLESYNC_TYPE:
    case EMC_TRAJ_SET_OFFSET_TYPE:
    case EMC_TRAJ_SET_ORIGIN_TYPE:
    case EMC_TRAJ_PROBE_TYPE:
    case EMC_TRAJ_RIGID_TAP_TYPE:
    case EMC_TRAJ_CLEAR_PROBE_TRIPPED_FLAG_TYPE:
    case EMC_TRAJ_SET_TELEOP_ENABLE_TYPE:
    case EMC_TRAJ_SET_TELEOP_VECTOR_TYPE:
    case EMC_TRAJ_SET_FO_ENABLE_TYPE:
    case EMC_TRAJ_SET_FH_ENABLE_TYPE:
    case EMC_TRAJ_SET_SO_ENABLE_TYPE:
	return EMC_TASK_EXEC_DONE;
	break;

    case EMC_TOOL_PREPARE_TYPE:
    case EMC_TOOL_LOAD_TYPE:
    case EMC_TOOL_UNLOAD_TYPE:
    case EMC_TOOL_LOAD_TOOL_TABLE_TYPE:
    case EMC_TOOL_SET_OFFSET_TYPE:
    case EMC_SPINDLE_ON_TYPE:
    case EMC_SPINDLE_OFF_TYPE:
    case EMC_COOLANT_MIST_ON_TYPE:
    case EMC_COOLANT_MIST_OFF_TYPE:
    case EMC_COOLANT_FLOOD_ON_TYPE:
    case EMC_COOLANT_FLOOD_OFF_TYPE:
    case EMC_LUBE_ON_TYPE:
    case EMC_LUBE_OFF_TYPE:
	return EMC_TASK_EXEC_DONE;
	break;

    case EMC_TASK_PLAN_RUN_TYPE:
    case EMC_TASK_PLAN_PAUSE_TYPE:
    case EMC_TASK_PLAN_END_TYPE:
    case EMC_TASK_PLAN_INIT_TYPE:
    case EMC_TASK_PLAN_SYNCH_TYPE:
    case EMC_TASK_PLAN_EXECUTE_TYPE:
    case EMC_TASK_PLAN_OPTIONAL_STOP_TYPE:
	return EMC_TASK_EXEC_DONE;
	break;

    case EMC_TRAJ_DELAY_TYPE:
    case EMC_AUX_INPUT_WAIT_TYPE:
	return EMC_TASK_EXEC_WAITING_FOR_DELAY;
	break;

    case EMC_MOTION_SET_AOUT_TYPE:
    case EMC_MOTION_SET_DOUT_TYPE:
    case EMC_MOTION_ADAPTIVE_TYPE:
	return EMC_TASK_EXEC_DONE;
	break;

    default:
	// unrecognized command
	if (EMC_DEBUG & EMC_DEBUG_TASK_ISSUE) {
	    rcs_print_error("postconditions: unrecognized command %d:%s\n",
			    cmd->type, emc_symbol_lookup(cmd->type));
	}
	return EMC_TASK_EXEC_DONE;
	break;
    }
    return EMC_TASK_EXEC_DONE; // unreached
}

/*
  STEPPING_CHECK() is a macro that prefaces a switch-case with a check
  for stepping. If stepping is active, it waits until the step has been
  given, then falls through to the rest of the case statement.
*/

#define STEPPING_CHECK()                                                   \
if (stepping) {                                                            \
  if (! steppingWait) {                                                    \
    steppingWait = 1;                                                      \
    steppedLine = emcStatus->task.currentLine;                             \
  }                                                                        \
  else {                                                                   \
    if (emcStatus->task.currentLine != steppedLine) {                      \
      break;                                                               \
    }                                                                      \
  }                                                                        \
}

// executor function
static int emcTaskExecute(void)
{
    int retval = 0;
    int status;			// status of child from EMC_SYSTEM_CMD
    pid_t pid;			// pid returned from waitpid()

    // first check for an abandoned system command and abort it
    if (emcSystemCmdPid != 0 &&
	emcStatus->task.execState !=
	EMC_TASK_EXEC_WAITING_FOR_SYSTEM_CMD) {
	if (EMC_DEBUG & EMC_DEBUG_TASK_ISSUE) {
	    rcs_print("emcSystemCmd: abandoning process %d\n",
		      emcSystemCmdPid);
	}
	kill(emcSystemCmdPid, SIGINT);
	emcSystemCmdPid = 0;
    }

    switch (emcStatus->task.execState) {
    case EMC_TASK_EXEC_ERROR:

	/*! \todo FIXME-- duplicate code for abort,
	   also near end of main, when aborting on subordinate errors,
	   and in emcTaskIssueCommand() */

	// abort everything
	emcTaskAbort();

	// without emcTaskPlanClose(), a new run command resumes at
	// aborted line-- feature that may be considered later
	{
	    int was_open = taskplanopen;
	    emcTaskPlanClose();
	    if (EMC_DEBUG & EMC_DEBUG_INTERP && was_open) {
		rcs_print("emcTaskPlanClose() called at %s:%d\n", __FILE__,
			  __LINE__);
	    }
	}

	// clear out pending command
	emcTaskCommand = 0;
	interp_list.clear();

	// clear out the interpreter state
	emcStatus->task.interpState = EMC_TASK_INTERP_IDLE;
	emcStatus->task.execState = EMC_TASK_EXEC_DONE;
	stepping = 0;
	steppingWait = 0;

	// now queue up command to resynch interpreter
	emcTaskQueueCommand(&taskPlanSynchCmd);

	retval = -1;
	break;

    case EMC_TASK_EXEC_DONE:
	STEPPING_CHECK();
	if (!emcStatus->motion.traj.queueFull &&
	    emcStatus->task.interpState != EMC_TASK_INTERP_PAUSED) {
	    if (0 == emcTaskCommand) {
		// need a new command
		emcTaskCommand = interp_list.get();
		// interp_list now has line number associated with this-- get
		// it
		if (0 != emcTaskCommand) {
		    emcTaskEager = 1;
		    emcStatus->task.currentLine =
			interp_list.get_line_number();
		    // and set it for all subsystems which use queued ids
		    emcTrajSetMotionId(emcStatus->task.currentLine);
		    if (emcStatus->motion.traj.queueFull) {
			emcStatus->task.execState =
			    EMC_TASK_EXEC_WAITING_FOR_MOTION_QUEUE;
		    } else {
			emcStatus->task.execState =
			    (enum EMC_TASK_EXEC_ENUM)
			    emcTaskCheckPreconditions(emcTaskCommand);
		    }
		}
	    } else {
		// have an outstanding command
		if (0 != emcTaskIssueCommand(emcTaskCommand)) {
		    emcStatus->task.execState = EMC_TASK_EXEC_ERROR;
		    retval = -1;
		} else {
		    emcStatus->task.execState = (enum EMC_TASK_EXEC_ENUM)
			emcTaskCheckPostconditions(emcTaskCommand);
		    emcTaskEager = 1;
		}
		emcTaskCommand = 0;	// reset it
	    }
	}
	break;

    case EMC_TASK_EXEC_WAITING_FOR_MOTION_QUEUE:
	STEPPING_CHECK();
	if (!emcStatus->motion.traj.queueFull) {
	    if (0 != emcTaskCommand) {
		emcStatus->task.execState = (enum EMC_TASK_EXEC_ENUM)
		    emcTaskCheckPreconditions(emcTaskCommand);
		emcTaskEager = 1;
	    } else {
		emcStatus->task.execState = EMC_TASK_EXEC_DONE;
		emcTaskEager = 1;
	    }
	}
	break;

    case EMC_TASK_EXEC_WAITING_FOR_PAUSE:
	STEPPING_CHECK();
	if (emcStatus->task.interpState != EMC_TASK_INTERP_PAUSED) {
	    if (0 != emcTaskCommand) {
		if (emcStatus->motion.traj.queue > 0) {
		    emcStatus->task.execState =
			EMC_TASK_EXEC_WAITING_FOR_MOTION_QUEUE;
		} else {
		    emcStatus->task.execState = (enum EMC_TASK_EXEC_ENUM)
			emcTaskCheckPreconditions(emcTaskCommand);
		    emcTaskEager = 1;
		}
	    } else {
		emcStatus->task.execState = EMC_TASK_EXEC_DONE;
		emcTaskEager = 1;
	    }
	}
	break;

    case EMC_TASK_EXEC_WAITING_FOR_MOTION:
	STEPPING_CHECK();
	if (emcStatus->motion.status == RCS_ERROR) {
	    // emcOperatorError(0, "error in motion controller");
	    emcStatus->task.execState = EMC_TASK_EXEC_ERROR;
	} else if (emcStatus->motion.status == RCS_DONE) {
	    emcStatus->task.execState = EMC_TASK_EXEC_DONE;
	    emcTaskEager = 1;
	}
	break;

    case EMC_TASK_EXEC_WAITING_FOR_IO:
	STEPPING_CHECK();
	if (emcStatus->io.status == RCS_ERROR) {
	    // emcOperatorError(0, "error in IO controller");
	    emcStatus->task.execState = EMC_TASK_EXEC_ERROR;
	} else if (emcStatus->io.status == RCS_DONE) {
	    emcStatus->task.execState = EMC_TASK_EXEC_DONE;
	    emcTaskEager = 1;
	}
	break;

    case EMC_TASK_EXEC_WAITING_FOR_MOTION_AND_IO:
	STEPPING_CHECK();
	if (emcStatus->motion.status == RCS_ERROR) {
	    // emcOperatorError(0, "error in motion controller");
	    emcStatus->task.execState = EMC_TASK_EXEC_ERROR;
	} else if (emcStatus->io.status == RCS_ERROR) {
	    // emcOperatorError(0, "error in IO controller");
	    emcStatus->task.execState = EMC_TASK_EXEC_ERROR;
	} else if (emcStatus->motion.status == RCS_DONE &&
		   emcStatus->io.status == RCS_DONE) {
	    emcStatus->task.execState = EMC_TASK_EXEC_DONE;
	    emcTaskEager = 1;
	}
	break;

    case EMC_TASK_EXEC_WAITING_FOR_DELAY:
	STEPPING_CHECK();
	// check if delay has passed
	if (etime() >= taskExecDelayTimeout) {
	    emcStatus->task.execState = EMC_TASK_EXEC_DONE;
	    if (emcStatus->task.input_timeout != 0)
		emcStatus->task.input_timeout = 1; // timeout occured
	    emcTaskEager = 1;
	}
	// delay can be also be because we wait for an input
	// if the index is set (not -1)
	if (emcAuxInputWaitIndex >= 0) { 
	    switch (emcAuxInputWaitType) {
		case WAIT_MODE_HIGH:
    		case WAIT_MODE_RISE: //FIXME: implement different rise mode if needed
		    if (emcStatus->motion.synch_di[emcAuxInputWaitIndex] != 0) {
			emcStatus->task.input_timeout = 0; // clear timeout flag
			emcAuxInputWaitIndex = -1;
			emcStatus->task.execState = EMC_TASK_EXEC_DONE;
		    }
		    break;
		    
		case WAIT_MODE_LOW:
		case WAIT_MODE_FALL: //FIXME: implement different fall mode if needed
		    if (emcStatus->motion.synch_di[emcAuxInputWaitIndex] == 0) {
			emcStatus->task.input_timeout = 0; // clear timeout flag
			emcAuxInputWaitIndex = -1;
			emcStatus->task.execState = EMC_TASK_EXEC_DONE;
		    }
		    break;

		case WAIT_MODE_IMMEDIATE:
		    emcStatus->task.input_timeout = 0; // clear timeout flag
		    emcAuxInputWaitIndex = -1;
		    emcStatus->task.execState = EMC_TASK_EXEC_DONE;
		    break;
		
		default:
		    emcOperatorError(0, "Unknown Wait Mode");
	    }
	}
	break;

    case EMC_TASK_EXEC_WAITING_FOR_SYSTEM_CMD:
	STEPPING_CHECK();

	// if we got here without a system command pending, say we're done
	if (0 == emcSystemCmdPid) {
	    emcStatus->task.execState = EMC_TASK_EXEC_DONE;
	    break;
	}
	// check the status of the system command
	pid = waitpid(emcSystemCmdPid, &status, WNOHANG);

	if (0 == pid) {
	    // child is still executing
	    break;
	}

	if (-1 == pid) {
	    // execution error
	    if (EMC_DEBUG & EMC_DEBUG_TASK_ISSUE) {
		rcs_print("emcSystemCmd: error waiting for %d\n",
			  emcSystemCmdPid);
	    }
	    emcSystemCmdPid = 0;
	    emcStatus->task.execState = EMC_TASK_EXEC_ERROR;
	    break;
	}

	if (emcSystemCmdPid != pid) {
	    // somehow some other child finished, which is a coding error
	    if (EMC_DEBUG & EMC_DEBUG_TASK_ISSUE) {
		rcs_print
		    ("emcSystemCmd: error waiting for system command %d, we got %d\n",
		     emcSystemCmdPid, pid);
	    }
	    emcSystemCmdPid = 0;
	    emcStatus->task.execState = EMC_TASK_EXEC_ERROR;
	    break;
	}
	// else child has finished
	if (WIFEXITED(status)) {
	    if (0 == WEXITSTATUS(status)) {
		// child exited normally
		emcSystemCmdPid = 0;
		emcStatus->task.execState = EMC_TASK_EXEC_DONE;
		emcTaskEager = 1;
	    } else {
		// child exited with non-zero status
		if (EMC_DEBUG & EMC_DEBUG_TASK_ISSUE) {
		    rcs_print
			("emcSystemCmd: system command %d exited abnormally with value %d\n",
			 emcSystemCmdPid, WEXITSTATUS(status));
		}
		emcSystemCmdPid = 0;
		emcStatus->task.execState = EMC_TASK_EXEC_ERROR;
	    }
	} else if (WIFSIGNALED(status)) {
	    // child exited with an uncaught signal
	    if (EMC_DEBUG & EMC_DEBUG_TASK_ISSUE) {
		rcs_print("system command %d terminated with signal %d\n",
			  emcSystemCmdPid, WTERMSIG(status));
	    }
	    emcSystemCmdPid = 0;
	    emcStatus->task.execState = EMC_TASK_EXEC_ERROR;
	} else if (WIFSTOPPED(status)) {
	    // child is currently being traced, so keep waiting
	} else {
	    // some other status, we'll call this an error
	    emcSystemCmdPid = 0;
	    emcStatus->task.execState = EMC_TASK_EXEC_ERROR;
	}
	break;

    default:
	// coding error
	if (EMC_DEBUG & EMC_DEBUG_TASK_ISSUE) {
	    rcs_print_error("invalid execState");
	}
	retval = -1;
	break;
    }
    return retval;
}

// called to allocate and init resources
static int emctask_startup()
{
    double end;
    int good;

#define RETRY_TIME 10.0		// seconds to wait for subsystems to come up
#define RETRY_INTERVAL 1.0	// seconds between wait tries for a subsystem

    // get our status data structure
    emcStatus = new EMC_STAT;

    // get the NML command buffer
    if (!(EMC_DEBUG & EMC_DEBUG_NML)) {
	set_rcs_print_destination(RCS_PRINT_TO_NULL);	// inhibit diag
	// messages
    }
    end = RETRY_TIME;
    good = 0;
    do {
	if (NULL != emcCommandBuffer) {
	    delete emcCommandBuffer;
	}
	emcCommandBuffer =
	    new RCS_CMD_CHANNEL(emcFormat, "emcCommand", "emc",
				EMC_NMLFILE);
	if (emcCommandBuffer->valid()) {
	    good = 1;
	    break;
	}
	esleep(RETRY_INTERVAL);
	end -= RETRY_INTERVAL;
	if (done) {
	    emctask_shutdown();
	    exit(1);
	}
    } while (end > 0.0);
    set_rcs_print_destination(RCS_PRINT_TO_STDOUT);	// restore diag
    // messages
    if (!good) {
	rcs_print_error("can't get emcCommand buffer\n");
	return -1;
    }
    // get our command data structure
    emcCommand = emcCommandBuffer->get_address();

    // get the NML status buffer
    if (!(EMC_DEBUG & EMC_DEBUG_NML)) {
	set_rcs_print_destination(RCS_PRINT_TO_NULL);	// inhibit diag
	// messages
    }
    end = RETRY_TIME;
    good = 0;
    do {
	if (NULL != emcStatusBuffer) {
	    delete emcStatusBuffer;
	}
	emcStatusBuffer =
	    new RCS_STAT_CHANNEL(emcFormat, "emcStatus", "emc",
				 EMC_NMLFILE);
	if (emcStatusBuffer->valid()) {
	    good = 1;
	    break;
	}
	esleep(RETRY_INTERVAL);
	end -= RETRY_INTERVAL;
	if (done) {
	    emctask_shutdown();
	    exit(1);
	}
    } while (end > 0.0);
    set_rcs_print_destination(RCS_PRINT_TO_STDOUT);	// restore diag
    // messages
    if (!good) {
	rcs_print_error("can't get emcStatus buffer\n");
	return -1;
    }

    if (!(EMC_DEBUG & EMC_DEBUG_NML)) {
	set_rcs_print_destination(RCS_PRINT_TO_NULL);	// inhibit diag
	// messages
    }
    end = RETRY_TIME;
    good = 0;
    do {
	if (NULL != emcErrorBuffer) {
	    delete emcErrorBuffer;
	}
	emcErrorBuffer =
	    new NML(nmlErrorFormat, "emcError", "emc", EMC_NMLFILE);
	if (emcErrorBuffer->valid()) {
	    good = 1;
	    break;
	}
	esleep(RETRY_INTERVAL);
	end -= RETRY_INTERVAL;
	if (done) {
	    emctask_shutdown();
	    exit(1);
	}
    } while (end > 0.0);
    set_rcs_print_destination(RCS_PRINT_TO_STDOUT);	// restore diag
    // messages
    if (!good) {
	rcs_print_error("can't get emcError buffer\n");
	return -1;
    }
    // get the timer
    if (!emcTaskNoDelay) {
	timer = new RCS_TIMER(EMC_TASK_CYCLE_TIME, "", "");
    }
    // initialize the subsystems

    // IO first

    if (!(EMC_DEBUG & EMC_DEBUG_NML)) {
	set_rcs_print_destination(RCS_PRINT_TO_NULL);	// inhibit diag
	// messages
    }
    end = RETRY_TIME;
    good = 0;
    do {
	if (0 == emcIoInit()) {
	    good = 1;
	    break;
	}
	esleep(RETRY_INTERVAL);
	end -= RETRY_INTERVAL;
	if (done) {
	    emctask_shutdown();
	    exit(1);
	}
    } while (end > 0.0);
    set_rcs_print_destination(RCS_PRINT_TO_STDOUT);	// restore diag
    // messages
    if (!good) {
	rcs_print_error("can't initialize IO\n");
	return -1;
    }

    end = RETRY_TIME;
    good = 0;
    do {
	if (0 == emcIoUpdate(&emcStatus->io)) {
	    good = 1;
	    break;
	}
	esleep(RETRY_INTERVAL);
	end -= RETRY_INTERVAL;
	if (done) {
	    emctask_shutdown();
	    exit(1);
	}
    } while (end > 0.0);
    if (!good) {
	rcs_print_error("can't read IO status\n");
	return -1;
    }
    // now motion

    end = RETRY_TIME;
    good = 0;
    do {
	if (0 == emcMotionInit()) {
	    good = 1;
	    break;
	}
	esleep(RETRY_INTERVAL);
	end -= RETRY_INTERVAL;
	if (done) {
	    emctask_shutdown();
	    exit(1);
	}
    } while (end > 0.0);
    if (!good) {
	rcs_print_error("can't initialize motion\n");
	return -1;
    }

    end = RETRY_TIME;
    good = 0;
    do {
	if (0 == emcMotionUpdate(&emcStatus->motion)) {
	    good = 1;
	    break;
	}
	esleep(RETRY_INTERVAL);
	end -= RETRY_INTERVAL;
	if (done) {
	    emctask_shutdown();
	    exit(1);
	}
    } while (end > 0.0);
    if (!good) {
	rcs_print_error("can't read motion status\n");
	return -1;
    }
    // now the interpreter
    if (0 != emcTaskPlanInit()) {
	rcs_print_error("can't initialize interpreter\n");
	return -1;
    }

    if (done) {
	emctask_shutdown();
	exit(1);
    }
    // now task
    if (0 != emcTaskInit()) {
	rcs_print_error("can't initialize task\n");
	return -1;
    }
    emcTaskUpdate(&emcStatus->task);

    return 0;
}

// called to deallocate resources
static int emctask_shutdown(void)
{
    // shut down the subsystems
    if (0 != emcStatus) {
	emcTaskHalt();
	emcTaskPlanExit();
	emcMotionHalt();
	emcIoHalt();
    }
    // delete the timer
    if (0 != timer) {
	delete timer;
	timer = 0;
    }
    // delete the NML channels

    if (0 != emcErrorBuffer) {
	delete emcErrorBuffer;
	emcErrorBuffer = 0;
    }

    if (0 != emcStatusBuffer) {
	delete emcStatusBuffer;
	emcStatusBuffer = 0;
	emcStatus = 0;
    }

    if (0 != emcCommandBuffer) {
	delete emcCommandBuffer;
	emcCommandBuffer = 0;
	emcCommand = 0;
    }

    if (0 != emcStatus) {
	delete emcStatus;
	emcStatus = 0;
    }
    return 0;
}

static int iniLoad(const char *filename)
{
    IniFile inifile;
    const char *inistring;
    char version[LINELEN], machine[LINELEN];
    double saveDouble;

    // open it
    if (inifile.Open(filename) == false) {
	return -1;
    }

    if (NULL != (inistring = inifile.Find("DEBUG", "EMC"))) {
	// copy to global
	if (1 != sscanf(inistring, "%i", &EMC_DEBUG)) {
	    EMC_DEBUG = 0;
	}
    } else {
	// not found, use default
	EMC_DEBUG = 0;
    }
    if (EMC_DEBUG & EMC_DEBUG_RCS) {
	// set_rcs_print_flag(PRINT_EVERYTHING);
	max_rcs_errors_to_print = -1;
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
	rcs_print("task: machine: '%s'  version '%s'\n", machine, version);
    }

    if (NULL != (inistring = inifile.Find("NML_FILE", "EMC"))) {
	// copy to global
	strcpy(EMC_NMLFILE, inistring);
    } else {
	// not found, use default
    }

    if (NULL != (inistring = inifile.Find("RS274NGC_STARTUP_CODE", "EMC"))) {
	// copy to global
	strcpy(RS274NGC_STARTUP_CODE, inistring);
    } else {
	// not found, use default
    }

    saveDouble = EMC_TASK_CYCLE_TIME;
    EMC_TASK_CYCLE_TIME_ORIG = EMC_TASK_CYCLE_TIME;
    emcTaskNoDelay = 0;
    if (NULL != (inistring = inifile.Find("CYCLE_TIME", "TASK"))) {
	if (1 == sscanf(inistring, "%lf", &EMC_TASK_CYCLE_TIME)) {
	    // found it
	    // if it's <= 0.0, then flag that we don't want to
	    // wait at all, which will set the EMC_TASK_CYCLE_TIME
	    // global to the actual time deltas
	    if (EMC_TASK_CYCLE_TIME <= 0.0) {
		emcTaskNoDelay = 1;
	    }
	} else {
	    // found, but invalid
	    EMC_TASK_CYCLE_TIME = saveDouble;
	    rcs_print
		("invalid [TASK] CYCLE_TIME in %s (%s); using default %f\n",
		 filename, inistring, EMC_TASK_CYCLE_TIME);
	}
    } else {
	// not found, using default
	rcs_print("[TASK] CYCLE_TIME not found in %s; using default %f\n",
		  filename, EMC_TASK_CYCLE_TIME);
    }

    // close it
    inifile.Close();

    return 0;
}

static EMC_STAT last_emc_status;

/*
  syntax: a.out {-d -ini <inifile>} {-nml <nmlfile>} {-shm <key>}
  */
int main(int argc, char *argv[])
{
    int taskAborted = 0;	// flag to prevent flurry of task aborts
    int taskPlanError = 0;
    int taskExecuteError = 0;
    double startTime;

    double minTime, maxTime;

    // copy command line args
    Argc = argc;
    Argv = argv;

    // loop until done
    done = 0;
    // trap ^C
    signal(SIGINT, emctask_quit);
    // and SIGTERM (used by runs script to shut down
    signal(SIGTERM, emctask_quit);

    // set print destination to stdout, for console apps
    set_rcs_print_destination(RCS_PRINT_TO_STDOUT);
    // process command line args
    if (0 != emcGetArgs(argc, argv)) {
	rcs_print_error("error in argument list\n");
	exit(1);
    }

    if (done) {
	emctask_shutdown();
	exit(1);
    }
    // initialize globals
    emcInitGlobals();

    if (done) {
	emctask_shutdown();
	exit(1);
    }
    // get configuration information
    iniLoad(EMC_INIFILE);

    if (done) {
	emctask_shutdown();
	exit(1);
    }
    // initialize everything
    if (0 != emctask_startup()) {
	emctask_shutdown();
	exit(1);
    }
    // set the default startup modes
    emcTaskSetState(EMC_TASK_STATE_ESTOP);
    emcTaskSetMode(EMC_TASK_MODE_MANUAL);

    // cause the interpreter's starting offset to be reflected
    emcTaskPlanInit();

    // reflect the initial value of EMC_DEBUG in emcStatus->debug
    emcStatus->debug = EMC_DEBUG;

    startTime = etime();	// set start time before entering loop;
    // it will be set at end of loop from now on
    minTime = DBL_MAX;		// set to value that can never be exceeded
    maxTime = 0.0;		// set to value that can never be underset
    while (!done) {
	// read command
	if (0 != emcCommandBuffer->peek()) {
	    // got a new command, so clear out errors
	    taskPlanError = 0;
	    taskExecuteError = 0;
	}
	// run control cycle
	if (0 != emcTaskPlan()) {
	    taskPlanError = 1;
	}
	if (0 != emcTaskExecute()) {
	    taskExecuteError = 1;
	}
	// update subordinate status

	emcIoUpdate(&emcStatus->io);
	emcMotionUpdate(&emcStatus->motion);
	// synchronize subordinate states
	if (emcStatus->io.aux.estop) {
	    if (emcStatus->motion.traj.enabled) {
		if (EMC_DEBUG & EMC_DEBUG_IO_POINTS) {
		    rcs_print("emcStatus->io.aux.estop=%d\n",
			      emcStatus->io.aux.estop);
		}
		emcTrajDisable();
		emcTaskAbort();
		emcTaskPlanSynch();
	    }
	    if (emcStatus->io.coolant.mist) {
		emcCoolantMistOff();
	    }
	    if (emcStatus->io.coolant.flood) {
		emcCoolantFloodOff();
	    }
	    if (emcStatus->io.lube.on) {
		emcLubeOff();
	    }
	    if (emcStatus->motion.spindle.enabled) {
		emcSpindleOff();
	    }
	}

	// check for subordinate errors, and halt task if so
	if (emcStatus->motion.status == RCS_ERROR ||
	    emcStatus->io.status == RCS_ERROR) {

	    /*! \todo FIXME-- duplicate code for abort,
	       also in emcTaskExecute()
	       and in emcTaskIssueCommand() */

	    if (!taskAborted) {
		// abort everything
		emcTaskAbort();
		// without emcTaskPlanClose(), a new run command resumes at
		// aborted line-- feature that may be considered later
		{
		    int was_open = taskplanopen;
		    emcTaskPlanClose();
		    if (EMC_DEBUG & EMC_DEBUG_INTERP && was_open) {
			rcs_print("emcTaskPlanClose() called at %s:%d\n",
				  __FILE__, __LINE__);
		    }
		}

		// clear out the pending command
		emcTaskCommand = 0;
		interp_list.clear();

		// clear out the interpreter state
		emcStatus->task.interpState = EMC_TASK_INTERP_IDLE;
		emcStatus->task.execState = EMC_TASK_EXEC_DONE;
		stepping = 0;
		steppingWait = 0;

		// now queue up command to resynch interpreter
		emcTaskQueueCommand(&taskPlanSynchCmd);
		taskAborted = 1;
	    }
	} else {
	    taskAborted = 0;
	}

	// update task-specific status
	emcTaskUpdate(&emcStatus->task);

	// handle RCS_STAT_MSG base class members explicitly, since this
	// is not an NML_MODULE and they won't be set automatically

	// do task
	emcStatus->task.command_type = emcCommand->type;
	emcStatus->task.echo_serial_number = emcCommand->serial_number;

	// do top level
	emcStatus->command_type = emcCommand->type;
	emcStatus->echo_serial_number = emcCommand->serial_number;

	if (taskPlanError || taskExecuteError ||
	    emcStatus->task.execState == EMC_TASK_EXEC_ERROR ||
	    emcStatus->motion.status == RCS_ERROR ||
	    emcStatus->io.status == RCS_ERROR) {
	    emcStatus->status = RCS_ERROR;
	    emcStatus->task.status = RCS_ERROR;
	} else if (!taskPlanError && !taskExecuteError &&
		   emcStatus->task.execState == EMC_TASK_EXEC_DONE &&
		   emcStatus->motion.status == RCS_DONE &&
		   emcStatus->io.status == RCS_DONE &&
		   interp_list.len() == 0 &&
		   emcTaskCommand == 0 &&
		   emcStatus->task.interpState == EMC_TASK_INTERP_IDLE) {
	    emcStatus->status = RCS_DONE;
	    emcStatus->task.status = RCS_DONE;
	} else {
	    emcStatus->status = RCS_EXEC;
	    emcStatus->task.status = RCS_EXEC;
	}
	// ignore state, line, source_line, and source_file[] since they're
	// N/A

	// Check for some error/warning conditions and warn the operator.
	// The GUI would be a better place to check these but we will have
	// lot's
	// of gui's and some will neglect to check these flags.
	for (int i = 0; i < EMC_AXIS_MAX; i++) {
	    if (last_emc_status.motion.axis[i].minSoftLimit == 0
		&& emcStatus->motion.axis[i].minSoftLimit == 1) {
		emcOperatorError(0,
				 _
				 ("Minimum Software Limit on axis %d exceeded."),
				 i);
	    }
	    last_emc_status.motion.axis[i].minSoftLimit =
		emcStatus->motion.axis[i].minSoftLimit;
	    if (last_emc_status.motion.axis[i].maxSoftLimit == 0
		&& emcStatus->motion.axis[i].maxSoftLimit == 1) {
		emcOperatorError(0,
				 _
				 ("Maximum Software Limit on axis %d exceeded."),
				 i);
	    }
	    last_emc_status.motion.axis[i].maxSoftLimit =
		emcStatus->motion.axis[i].maxSoftLimit;
	}
	// write it
	// since emcStatus was passed to the WM init functions, it
	// will be updated in the _update() functions above. There's
	// no need to call the individual functions on all WM items.
	emcStatusBuffer->write(emcStatus);

	// wait on timer cycle, if specified, or calculate actual
	// interval if ini file says to run full out via
	// [TASK] CYCLE_TIME <= 0.0
	// emcTaskEager = 0;
	if ((emcTaskNoDelay) || (emcTaskEager)) {
	    emcTaskEager = 0;
	} else {
	    timer->wait();
	}
    }				// end of while (! done)

    // clean up everything
    emctask_shutdown();
    /* debugging */
    if (emcTaskNoDelay) {
	if (EMC_DEBUG & EMC_DEBUG_INTERP) {
	    printf("cycle times (seconds): %f min, %f max\n", minTime,
	       maxTime);
	}
    }
    // and leave
    exit(0);
}
