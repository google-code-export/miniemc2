/********************************************************************
* Description: command.c
*   emcmotCommandhandler() takes commands passed from user space and
*   performs various functions based on the value in emcmotCommand->command.
*   For the full list, see the EMCMOT_COMMAND enum in motion.h
*
* pc says:
*
*   Most of the configs would be better off being passed via an ioctl
*   implimentation leaving pure realtime data to be handled by
*   emcmotCommmandHandler() - This would provide a small performance
*   increase on slower systems.
*
* jmk says:
*
*   Using commands to set config parameters is "undesireable", because
*   of the large amount of code needed for each parameter.  Today you
*   need to do the following to add a single new parameter called foo:
*
*   1)  Add a member 'foo' to the config or joint structure in motion.h
*   2)  Add a command 'EMCMOT_SET_FOO" to the cmd_code_t enum in motion.h
*   3)  Add a field to the command_t struct for the value used by
*       the set command (if there isn't already one that can be used.)
*   4)  Add a case to the giant switch statement in command.c to
*       handle the 'EMCMOT_SET_FOO' command.
*   5)  Write a function emcSetFoo() in taskintf.cc to issue the command.
*   6)  Add a prototype for emcSetFoo() to emc.hh
*   7)  Add code to iniaxis.cc (or one of the other inixxx.cc files) to
*       get the value from the ini file and call emcSetFoo().  (Note
*       that each parameter has about 16 lines of code, but the code
*       is identical except for variable/parameter names.)
*   8)  Add more code to iniaxis.cc to write the new value back out
*       to the ini file.
*   After all that, you have the abililty to get a number from the
*   ini file to a structure in shared memory where the motion controller
*   can actually use it.  However, if you want to manipulate that number
*   using NML, you have to do more:
*   9)  Add a #define EMC_SET_FOO_TYPE to emc.hh
*   10) Add a class definition for EMC_SET_FOO to emc.hh
*   11) Add a case to a giant switch statement in emctaskmain.cc to
*       call emcSetFoo() when the NML command is received.  (Actually
*       there are about 6 switch statements that need at least a
*       case label added.
*   12) Add cases to two giant switch statements in emc.cc, associated
*       with looking up and formating the command.
*
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
* $Revision: 1.100 $
* $Author: alex_joni $
* $Date: 2007/09/15 21:38:07 $
*
********************************************************************/

#include <linux/types.h>
#include <float.h>
#include "posemath.h"
#include "rtapi.h"
#include "hal.h"
#include "motion.h"
#include "motion_debug.h"
#include "motion_struct.h"
#include "emcmotglb.h"
#include "mot_priv.h"
#include "rtapi_math.h"

/* debugging functions */
extern void print_pose ( EmcPose *pos );
extern void check_stuff(char *msg);


/* value for world home position */
EmcPose worldHome = { {0.0, 0.0, 0.0},
                      0.0, 0.0, 0.0,
                      0.0, 0.0, 0.0
};

/* kinematics flags */
KINEMATICS_FORWARD_FLAGS fflags = 0;
KINEMATICS_INVERSE_FLAGS iflags = 0;

/* loops through the active joints and checks if any are not homed */
int checkAllHomed(void) {
    int joint_num;
    emcmot_joint_t *joint;

    /* bail out if the allHomed flag is already set */
    if (0 != emcmotDebug) {
	if (emcmotDebug->allHomed) return 1;
    }

    for (joint_num = 0; joint_num < num_joints; joint_num++) {
	/* point to joint data */
	joint = &joints[joint_num];
	if (!GET_JOINT_ACTIVE_FLAG(joint)) {
	    /* if joint is not active, don't even look at its limits */
	    continue;
	}
	if (!GET_JOINT_HOMED_FLAG(joint)) {
	    /* if any of the joints is not homed return false */
	    return 0;
	}
    }
    /* set the global flag that all axes are homed */
    if (0 != emcmotDebug) {
	emcmotDebug->allHomed = 1;
    }
    /* return true if all are actives are homed*/
    return 1;
}


/* limits_ok() returns 1 if none of the hard limits are set,
   0 if any are set. Called on a linear and circular move. */
static int limits_ok(void)
{
    int joint_num;
    emcmot_joint_t *joint;

    for (joint_num = 0; joint_num < num_joints; joint_num++) {
	/* point to joint data */
	joint = &joints[joint_num];
	if (!GET_JOINT_ACTIVE_FLAG(joint)) {
	    /* if joint is not active, don't even look at its limits */
	    continue;
	}

	if (GET_JOINT_PHL_FLAG(joint) || GET_JOINT_NHL_FLAG(joint)) {
	    return 0;
	}
    }

    return 1;
}

/* check the value of the axis and velocity against current position,
   returning 1 (okay) if the request is to jog off the limit, 0 (bad)
   if the request is to jog further past a limit. */
static int jog_ok(int joint_num, double vel)
{
    emcmot_joint_t *joint;
    int neg_limit_override, pos_limit_override;

    /* point to joint data */
    joint = &joints[joint_num];
    /* are any limits for this joint overridden? */
    neg_limit_override = emcmotStatus->overrideLimitMask & ( 1 << (joint_num*2));
    pos_limit_override = emcmotStatus->overrideLimitMask & ( 2 << (joint_num*2));
    if ( neg_limit_override && pos_limit_override ) {
	/* both limits have been overridden at the same time.  This
	   happens only when they both share an input, but means it
	   is impossible to know which direction is safe to move.  So
	   we skip the following tests... */
	return 1;
    }
    if (joint_num < 0 || joint_num >= num_joints) {
	reportError("Can't jog invalid joint number %d.", joint_num);
	return 0;
    }
    if (vel > 0.0 && GET_JOINT_PHL_FLAG(joint)) {
	reportError("Can't jog joint %d further past max hard limit.",
	    joint_num);
	return 0;
    }
    if (vel < 0.0 && GET_JOINT_NHL_FLAG(joint)) {
	reportError("Can't jog joint %d further past min hard limit.",
	    joint_num);
	return 0;
    }
    refresh_jog_limits(joint);
    if ( vel > 0.0 && (joint->pos_cmd > joint->max_jog_limit) ) {
	reportError("Can't jog joint %d further past max soft limit.",
	    joint_num);
	return 0;
    }
    if ( vel < 0.0 && (joint->pos_cmd < joint->min_jog_limit) ) {
	reportError("Can't jog joint %d further past min soft limit.",
	    joint_num);
	return 0;
    }
    /* okay to jog */
    return 1;
}

/* Jogs limits change, based on whether the machine is homed or
   or not.  If not homed, the limits are relative to the current
   position by +/- the full range of travel.  Once homed, they
   are absolute.
*/
void refresh_jog_limits(emcmot_joint_t *joint)
{
    double range;
#if 0 //Code disabled by KSU
    if (GET_JOINT_HOMED_FLAG(joint)) {
	/* if homed, set jog limits using soft limits */
	joint->max_jog_limit = joint->max_pos_limit;
	joint->min_jog_limit = joint->min_pos_limit;
    } else {
	/* not homed, set limits based on current position */
	range = joint->max_pos_limit - joint->min_pos_limit;
	joint->max_jog_limit = joint->pos_fb + range;
	joint->min_jog_limit = joint->pos_fb - range;
    }
#else //Maximum jog position always correspond to axes limits
	joint->max_jog_limit = joint->max_pos_limit;
	joint->min_jog_limit = joint->min_pos_limit;
#endif
}

/* inRange() returns non-zero if the position lies within the joint
   limits, or 0 if not */
static int inRange(EmcPose pos)
{
    double joint_pos[EMCMOT_MAX_JOINTS];
    int joint_num;
    emcmot_joint_t *joint;

    /* fill in all joints with 0 */
    for (joint_num = 0; joint_num < num_joints; joint_num++) {
	joint_pos[joint_num] = 0.0;
    }

    /* now fill in with real values, for joints that are used */
    kinematicsInverse(&pos, joint_pos, &iflags, &fflags);

    for (joint_num = 0; joint_num < num_joints; joint_num++) {
	/* point to joint data */
	joint = &joints[joint_num];

	if (!GET_JOINT_ACTIVE_FLAG(joint)) {
	    /* if joint is not active, don't even look at its limits */
	    continue;
	}
	if ((joint_pos[joint_num] > joint->max_pos_limit) ||
	    (joint_pos[joint_num] < joint->min_pos_limit)) {
	    return 0;		/* can't move further past limit */
	}
    }

    /* okay to move */
    return 1;
}

/* clearHomes() will clear the homed flags for joints that have moved
   since homing, outside coordinated control, for machines with no
   forward kinematics. This is used in conjunction with the rehomeAll
   flag, which is set for any coordinated move that in general will
   result in all joints moving. The flag is consulted whenever a joint
   is jogged in joint mode, so that either its flag can be cleared if
   no other joints have moved, or all have to be cleared. */
void clearHomes(int joint_num)
{
    int n;
    emcmot_joint_t *joint;

    if (kinType == KINEMATICS_INVERSE_ONLY) {
	if (rehomeAll) {
	    for (n = 0; n < num_joints; n++) {
		/* point at joint data */
		joint = &(joints[n]);
		/* clear flag */
		SET_JOINT_HOMED_FLAG(joint, 0);
	    }
	} else {
	    /* point at joint data */
	    joint = &joints[joint_num];
	    /* clear flag */
	    SET_JOINT_HOMED_FLAG(joint, 0);
	}
    }
    if (0 != emcmotDebug) {
	emcmotDebug->allHomed = 0;
    }
}


/*! \function emcmotDioWrite()

  sets or clears a HAL DIO pin, 
  pins get exported at runtime
  
  index is valid from 0 to EMCMOT_MAX_DIO, defined in emcmotcfg.h
  
*/
void emcmotDioWrite(int index, char value)
{
    if ((index >= EMCMOT_MAX_DIO) || (index < 0)) {
	rtapi_print_msg(RTAPI_MSG_ERR, "ERROR: index out of range, %d not in [0..%d] (increase EMCMOT_MAX_DIO)\n",index,EMCMOT_MAX_DIO);
    } else {
	if (value != 0) {
	    *(emcmot_hal_data->synch_do[index])=1;
	} else {
	    *(emcmot_hal_data->synch_do[index])=0;
	}
    }
}

/*! \function emcmotAioWrite()

  sets or clears a HAL AIO pin, 
  pins get exported at runtime
  
  \todo Implement function, it doesn't do anything right now
  RS274NGC doesn't support it now, only defined/used in emccanon.cc
  
*/
void emcmotAioWrite(int index, double value)
{
    reportError("emcmotAioWrite called, yet not implemented\n");
}

/*
  emcmotCommandHandler() is called each main cycle to read the
  shared memory buffer
  */
void emcmotCommandHandler(void *arg, long period)
{
    int joint_num;
    emcmot_joint_t *joint;
    double tmp1;
    emcmot_comp_entry_t *comp_entry;
    
check_stuff ( "before command_handler()" );

    /* check for split read */
    if (emcmotCommand->head != emcmotCommand->tail) {
	emcmotDebug->split++;
	return;			/* not really an error */
    }
    if (emcmotCommand->commandNum != emcmotStatus->commandNumEcho) {
	/* increment head count-- we'll be modifying emcmotStatus */
	emcmotStatus->head++;
	emcmotDebug->head++;

	/* got a new command-- echo command and number... */
	emcmotStatus->commandEcho = emcmotCommand->command;
	emcmotStatus->commandNumEcho = emcmotCommand->commandNum;

	/* clear status value by default */
	emcmotStatus->commandStatus = EMCMOT_COMMAND_OK;
	
	/* ...and process command */

	/* Many commands uses "command->axis" to indicate which joint they
	   wish to operate on.  This code eliminates the need to copy
	   command->axis to "joint_num", limit check it, and then set "joint"
	   to point to the joint data.  All the individual commands need to do
	   is verify that "joint" is non-zero. */
	joint_num = emcmotCommand->axis;
	if (joint_num >= 0 && joint_num < num_joints) {
	    /* valid joint, point to it's data */
	    joint = &joints[joint_num];
	} else {
	    /* bad joint number */
	    joint = 0;
	}

/* printing of commands for troubleshooting */
	rtapi_print_msg(RTAPI_MSG_DBG, "%d: CMD %d, code %3d ", emcmotStatus->heartbeat,
	    emcmotCommand->commandNum, emcmotCommand->command);

	switch (emcmotCommand->command) {
	case EMCMOT_ABORT:
	    /* abort motion */
	    /* can happen at any time */
	    /* this command attempts to stop all machine motion. it looks at
	       the current mode and acts accordingly, if in teleop mode, it
	       sets the desired velocities to zero, if in coordinated mode,
	       it calls the traj planner abort function (don't know what that
	       does yet), and if in free mode, it disables the free mode traj
	       planners which stops axis motion */
	    rtapi_print_msg(RTAPI_MSG_DBG, "ABORT");
	    rtapi_print_msg(RTAPI_MSG_DBG, " %d", emcmotCommand->axis);
	    /* check for coord or free space motion active */
	    if (GET_MOTION_TELEOP_FLAG()) {
		emcmotDebug->teleop_data.desiredVel.tran.x = 0.0;
		emcmotDebug->teleop_data.desiredVel.tran.y = 0.0;
		emcmotDebug->teleop_data.desiredVel.tran.z = 0.0;
		emcmotDebug->teleop_data.desiredVel.a = 0.0;
		emcmotDebug->teleop_data.desiredVel.b = 0.0;
		emcmotDebug->teleop_data.desiredVel.c = 0.0;
	    } else if (GET_MOTION_COORD_FLAG()) {
		tpAbort(&emcmotDebug->queue);
		SET_MOTION_ERROR_FLAG(0);
	    } else {
		for (joint_num = 0; joint_num < num_joints; joint_num++) {
		    /* point to joint struct */
		    joint = &joints[joint_num];
		    /* tell joint planner to stop */
		    joint->free_tp_enable = 0;
		    /* stop homing if in progress */
		    if ( joint->home_state != HOME_IDLE ) {
			joint->home_state = HOME_ABORT;
		    }
		}
	    }
	    /* clear axis errors (regardless of mode */	    
	    for (joint_num = 0; joint_num < num_joints; joint_num++) {
		/* point to joint struct */
		joint = &joints[joint_num];
		/* update status flags */
		SET_JOINT_ERROR_FLAG(joint, 0);
		SET_JOINT_FAULT_FLAG(joint, 0);
	    }
	    break;

	case EMCMOT_AXIS_ABORT:
	    /* abort one axis */
	    /* can happen at any time */
	    /* this command stops a single axis.  It is only usefull
	       in free mode, so in coord or teleop mode it does
	       nothing. */
	    rtapi_print_msg(RTAPI_MSG_DBG, "AXIS_ABORT");
	    rtapi_print_msg(RTAPI_MSG_DBG, " %d", emcmotCommand->axis);
	    if (GET_MOTION_TELEOP_FLAG()) {
		/* do nothing in teleop mode */
	    } else if (GET_MOTION_COORD_FLAG()) {
		/* do nothing in coord mode */
	    } else {
		/* validate joint */
		if (joint == 0) {
		    break;
		}
		/* tell joint planner to stop */
		joint->free_tp_enable = 0;
		/* stop homing if in progress */
		if ( joint->home_state != HOME_IDLE ) {
		    joint->home_state = HOME_ABORT;
		}
		/* update status flags */
		SET_JOINT_ERROR_FLAG(joint, 0);
	    }
	    break;

	case EMCMOT_FREE:
	    /* change the mode to free axis motion */
	    /* can be done at any time */
	    /* this code doesn't actually make the transition, it merely
	       requests the transition by clearing a couple of flags */
	    /* reset the emcmotDebug->coordinating flag to defer transition
	       to controller cycle */
	    rtapi_print_msg(RTAPI_MSG_DBG, "FREE");
	    emcmotDebug->coordinating = 0;
	    emcmotDebug->teleoperating = 0;
	    break;

	case EMCMOT_COORD:
	    /* change the mode to coordinated axis motion */
	    /* can be done at any time */
	    /* this code doesn't actually make the transition, it merely
	       tests a condition and then sets a flag requesting the
	       transition */
	    /* set the emcmotDebug->coordinating flag to defer transition to
	       controller cycle */
	    rtapi_print_msg(RTAPI_MSG_DBG, "COORD");
	    emcmotDebug->coordinating = 1;
	    emcmotDebug->teleoperating = 0;
	    if (kinType != KINEMATICS_IDENTITY) {
		if (!checkAllHomed()) {
		    reportError
			("all axes must be homed before going into coordinated mode");
		    emcmotDebug->coordinating = 0;
		    break;
		}
	    }
	    break;

	case EMCMOT_TELEOP:
	    /* change the mode to teleop motion */
	    /* can be done at any time */
	    /* this code doesn't actually make the transition, it merely
	       tests a condition and then sets a flag requesting the
	       transition */
	    /* set the emcmotDebug->teleoperating flag to defer transition to
	       controller cycle */
	    rtapi_print_msg(RTAPI_MSG_DBG, "TELEOP");
	    emcmotDebug->teleoperating = 1;
	    if (kinType != KINEMATICS_IDENTITY) {
		
		if (!checkAllHomed()) {
		    reportError
			("all axes must be homed before going into teleop mode");
		    emcmotDebug->teleoperating = 0;
		    break;
		}

	    }
	    break;

	case EMCMOT_SET_NUM_AXES:
	    /* set the global NUM_AXES, which must be between 1 and
	       EMCMOT_MAX_AXIS, inclusive */
	    /* this sets a global - I hate globals - hopefully this can be
	       moved into the config structure, or dispensed with completely */
	    rtapi_print_msg(RTAPI_MSG_DBG, "SET_NUM_AXES");
	    rtapi_print_msg(RTAPI_MSG_DBG, " %d", emcmotCommand->axis);
	    if (( emcmotCommand->axis <= 0 ) ||
		( emcmotCommand->axis > EMCMOT_MAX_AXIS )) {
		break;
	    }
	    num_axes = emcmotCommand->axis;
	    emcmotConfig->numAxes = num_axes;
	    break;

	case EMCMOT_SET_WORLD_HOME:
	    rtapi_print_msg(RTAPI_MSG_DBG, "SET_WORLD_HOME");
	    emcmotStatus->world_home = emcmotCommand->pos;
	    break;

	case EMCMOT_SET_HOMING_PARAMS:
	    rtapi_print_msg(RTAPI_MSG_DBG, "SET_HOMING_PARAMS");
	    rtapi_print_msg(RTAPI_MSG_DBG, " %d", joint_num);
	    emcmot_config_change();
	    if (joint == 0) {
		break;
	    }
	    joint->home_offset = emcmotCommand->offset;
	    joint->home = emcmotCommand->home;
	    joint->home_search_vel = emcmotCommand->search_vel;
	    joint->home_latch_vel = emcmotCommand->latch_vel;
	    joint->home_flags = emcmotCommand->flags;
	    joint->home_sequence = emcmotCommand->home_sequence;
	    break;

	case EMCMOT_OVERRIDE_LIMITS:
	    /* this command can be issued with axix < 0 to re-enable
	       limits, but they are automatically re-enabled at the
	       end of the next jog */
	    rtapi_print_msg(RTAPI_MSG_DBG, "OVERRIDE_LIMITS");
	    rtapi_print_msg(RTAPI_MSG_DBG, " %d", emcmotCommand->axis);
	    if (emcmotCommand->axis < 0) {
		/* don't override limits */
		rtapi_print_msg(RTAPI_MSG_DBG, "override off");
		emcmotStatus->overrideLimitMask = 0;
	    } else {
		rtapi_print_msg(RTAPI_MSG_DBG, "override on");
		emcmotStatus->overrideLimitMask = 0;
		for (joint_num = 0; joint_num < num_joints; joint_num++) {
		    /* point at joint data */
		    joint = &joints[joint_num];
#if 0		// Original code commented by KSU to allow tripped axis move
		    /* only override limits that are currently tripped */
		    if ( GET_JOINT_NHL_FLAG(joint) ) {
			emcmotStatus->overrideLimitMask |= (1 << (joint_num*2));
		    }
		    if ( GET_JOINT_PHL_FLAG(joint) ) {
			emcmotStatus->overrideLimitMask |= (2 << (joint_num*2));
		    }
#else
		    emcmotStatus->overrideLimitMask |= (1 << (joint_num*2));
		    emcmotStatus->overrideLimitMask |= (2 << (joint_num*2));
#endif
		}
	    }
	    emcmotDebug->overriding = 0;
	    for (joint_num = 0; joint_num < num_joints; joint_num++) {
		/* point at joint data */
		joint = &joints[joint_num];
		/* clear joint errors */
		SET_JOINT_ERROR_FLAG(joint, 0);
	    }
	    break;

	case EMCMOT_SET_MOTOR_OFFSET:
	    rtapi_print_msg(RTAPI_MSG_DBG, "SET_MOTOR_OFFSET");
	    rtapi_print_msg(RTAPI_MSG_DBG, " %d", emcmotCommand->axis);
	    if(joint == 0) {
		break;
	    }
	    joint->motor_offset = emcmotCommand->motor_offset;
	    break;

	case EMCMOT_SET_POSITION_LIMITS:
	    /* sets soft limits for an axis */
	    rtapi_print_msg(RTAPI_MSG_DBG, "SET_POSITION_LIMITS");
	    rtapi_print_msg(RTAPI_MSG_DBG, " %d", joint_num);
	    emcmot_config_change();
	    /* set the position limits for the axis */
	    /* can be done at any time */
	    if (joint == 0) {
		break;
	    }
	    joint->min_pos_limit = emcmotCommand->minLimit;
	    joint->max_pos_limit = emcmotCommand->maxLimit;
	    break;

	case EMCMOT_SET_BACKLASH:
	    /* sets backlash for an axis */
	    rtapi_print_msg(RTAPI_MSG_DBG, "SET_BACKLASH");
	    rtapi_print_msg(RTAPI_MSG_DBG, " %d", joint_num);
	    emcmot_config_change();
	    /* set the backlash for the axis */
	    /* can be done at any time */
	    if (joint == 0) {
		break;
	    }
	    joint->backlash = emcmotCommand->backlash;
	    break;

	    /*
	       Max and min ferror work like this: limiting ferror is
	       determined by slope of ferror line, = maxFerror/limitVel ->
	       limiting ferror = maxFerror/limitVel * vel. If ferror <
	       minFerror then OK else if ferror < limiting ferror then OK
	       else ERROR */
	case EMCMOT_SET_MAX_FERROR:
	    rtapi_print_msg(RTAPI_MSG_DBG, "SET_MAX_FERROR");
	    rtapi_print_msg(RTAPI_MSG_DBG, " %d", joint_num);
	    emcmot_config_change();
	    if (joint == 0 || emcmotCommand->maxFerror < 0.0) {
		break;
	    }
	    joint->max_ferror = emcmotCommand->maxFerror;
	    break;

	case EMCMOT_SET_MIN_FERROR:
	    rtapi_print_msg(RTAPI_MSG_DBG, "SET_MIN_FERROR");
	    rtapi_print_msg(RTAPI_MSG_DBG, " %d", joint_num);
	    emcmot_config_change();
	    if (joint == 0 || emcmotCommand->minFerror < 0.0) {
		break;
	    }
	    joint->min_ferror = emcmotCommand->minFerror;
	    break;

	case EMCMOT_JOG_CONT:
	    /* do a continuous jog, implemented as an incremental jog to the
	       limit.  When the user lets go of the button an abort will
	       stop the jog. */
	    rtapi_print_msg(RTAPI_MSG_DBG, "JOG_CONT");
	    rtapi_print_msg(RTAPI_MSG_DBG, " %d", joint_num);
	    /* check axis range */
	    if (joint == 0) {
		break;
	    }

	    /* must be in free mode and enabled */
	    if (GET_MOTION_COORD_FLAG()) {
		reportError("Can't jog axis in coordinated mode.");
		SET_JOINT_ERROR_FLAG(joint, 1);
		break;
	    }
	    if (!GET_MOTION_ENABLE_FLAG()) {
		reportError("Can't jog axis when not enabled.");
		SET_JOINT_ERROR_FLAG(joint, 1);
		break;
	    }
	    if (emcmotStatus->homing_active) {
		reportError("Can't jog any axis while homing.");
		SET_JOINT_ERROR_FLAG(joint, 1);
		break;
	    }
	    if (joint->wheel_jog_active) {
		/* can't do two kinds of jog at once */
		break;
	    }
	    if (emcmotStatus->net_feed_scale < 0.0001 ) {
		/* don't jog if feedhold is on or if feed override is zero */
		break;
	    }
	    /* don't jog further onto limits */
	    if (!jog_ok(joint_num, emcmotCommand->vel)) {
		SET_JOINT_ERROR_FLAG(joint, 1);
		break;
	    }
	    /* set destination of jog */
	    refresh_jog_limits(joint);
	    if (emcmotCommand->vel > 0.0) {
		joint->free_pos_cmd = joint->max_jog_limit;
	    } else {
		joint->free_pos_cmd = joint->min_jog_limit;
	    }
	    /* set velocity of jog */
	    joint->free_vel_lim = fabs(emcmotCommand->vel);
	    /* lock out other jog sources */
	    joint->kb_jog_active = 1;
	    /* and let it go */
	    joint->free_tp_enable = 1;
	    /*! \todo FIXME - should we really be clearing errors here? */
	    SET_JOINT_ERROR_FLAG(joint, 0);
	    /* clear axis homed flag(s) if we don't have forward kins.
	       Otherwise, a transition into coordinated mode will incorrectly
	       assume the homed position. Do all if they've all been moved
	       since homing, otherwise just do this one */
	    clearHomes(joint_num);
	    break;

	case EMCMOT_JOG_INCR:
	    /* do an incremental jog */

	    /* check axis range */
	    rtapi_print_msg(RTAPI_MSG_DBG, "JOG_INCR");
	    rtapi_print_msg(RTAPI_MSG_DBG, " %d", joint_num);
	    if (joint == 0) {
		break;
	    }

	    /* must be in free mode and enabled */
	    if (GET_MOTION_COORD_FLAG()) {
		reportError("Can't jog axis in coordinated mode.");
		SET_JOINT_ERROR_FLAG(joint, 1);
		break;
	    }
	    if (!GET_MOTION_ENABLE_FLAG()) {
		reportError("Can't jog axis when not enabled.");
		SET_JOINT_ERROR_FLAG(joint, 1);
		break;
	    }
	    if (emcmotStatus->homing_active) {
		reportError("Can't jog any axis while homing.");
		SET_JOINT_ERROR_FLAG(joint, 1);
		break;
	    }
	    if (joint->wheel_jog_active) {
		/* can't do two kinds of jog at once */
		break;
	    }
	    if (emcmotStatus->net_feed_scale < 0.0001 ) {
		/* don't jog if feedhold is on or if feed override is zero */
		break;
	    }
	    /* don't jog further onto limits */
	    if (!jog_ok(joint_num, emcmotCommand->vel)) {
		SET_JOINT_ERROR_FLAG(joint, 1);
		break;
	    }
	    /* set target position for jog */
	    if (emcmotCommand->vel > 0.0) {
		tmp1 = joint->free_pos_cmd + emcmotCommand->offset;
	    } else {
		tmp1 = joint->free_pos_cmd - emcmotCommand->offset;
	    }
	    /* don't jog past limits */
	    refresh_jog_limits(joint);
	    if (tmp1 > joint->max_jog_limit) {
		break;
	    }
	    if (tmp1 < joint->min_jog_limit) {
		break;
	    }
	    /* set target position */
	    joint->free_pos_cmd = tmp1;
	    /* set velocity of jog */
	    joint->free_vel_lim = fabs(emcmotCommand->vel);
	    /* lock out other jog sources */
	    joint->kb_jog_active = 1;
	    /* and let it go */
	    joint->free_tp_enable = 1;
	    SET_JOINT_ERROR_FLAG(joint, 0);
	    /* clear axis homed flag(s) if we don't have forward kins.
	       Otherwise, a transition into coordinated mode will incorrectly
	       assume the homed position. Do all if they've all been moved
	       since homing, otherwise just do this one */
	    clearHomes(joint_num);
	    break;

	case EMCMOT_JOG_ABS:
	    /* do an absolute jog */

	    /* check axis range */
	    rtapi_print_msg(RTAPI_MSG_DBG, "JOG_ABS");
	    rtapi_print_msg(RTAPI_MSG_DBG, " %d", joint_num);
	    if (joint == 0) {
		break;
	    }
	    /* must be in free mode and enabled */
	    if (GET_MOTION_COORD_FLAG()) {
		reportError("Can't jog axis in coordinated mode.");
		SET_JOINT_ERROR_FLAG(joint, 1);
		break;
	    }
	    if (!GET_MOTION_ENABLE_FLAG()) {
		reportError("Can't jog axis when not enabled.");
		SET_JOINT_ERROR_FLAG(joint, 1);
		break;
	    }
	    if (emcmotStatus->homing_active) {
		reportError("Can't jog any axis while homing.");
		SET_JOINT_ERROR_FLAG(joint, 1);
		break;
	    }
	    if (joint->wheel_jog_active) {
		/* can't do two kinds of jog at once */
		break;
	    }
	    if (emcmotStatus->net_feed_scale < 0.0001 ) {
		/* don't jog if feedhold is on or if feed override is zero */
		break;
	    }
	    /* don't jog further onto limits */
	    if (!jog_ok(joint_num, emcmotCommand->vel)) {
		SET_JOINT_ERROR_FLAG(joint, 1);
		break;
	    }
	    /*! \todo FIXME-- use 'goal' instead */
	    joint->free_pos_cmd = emcmotCommand->offset;
	    /* don't jog past limits */
	    refresh_jog_limits(joint);
	    if (joint->free_pos_cmd > joint->max_jog_limit) {
		joint->free_pos_cmd = joint->max_jog_limit;
	    }
	    if (joint->free_pos_cmd < joint->min_jog_limit) {
		joint->free_pos_cmd = joint->min_jog_limit;
	    }
	    /* set velocity of jog */
	    joint->free_vel_lim = fabs(emcmotCommand->vel);
	    /* lock out other jog sources */
	    joint->kb_jog_active = 1;
	    /* and let it go */
	    joint->free_tp_enable = 1;
	    SET_JOINT_ERROR_FLAG(joint, 0);
	    /* clear axis homed flag(s) if we don't have forward kins.
	       Otherwise, a transition into coordinated mode will incorrectly
	       assume the homed position. Do all if they've all been moved
	       since homing, otherwise just do this one */
	    clearHomes(joint_num);
	    break;

	case EMCMOT_SET_TERM_COND:
	    /* sets termination condition for motion emcmotDebug->queue */
	    rtapi_print_msg(RTAPI_MSG_DBG, "SET_TERM_COND");
	    tpSetTermCond(&emcmotDebug->queue, emcmotCommand->termCond, emcmotCommand->tolerance);
	    break;

        case EMCMOT_SET_SPINDLESYNC:
            tpSetSpindleSync(&emcmotDebug->queue, emcmotCommand->spindlesync, emcmotCommand->flags);
            break;

	case EMCMOT_SET_LINE:
	    /* emcmotDebug->queue up a linear move */
	    /* requires coordinated mode, enable off, not on limits */
	    rtapi_print_msg(RTAPI_MSG_DBG, "SET_LINE");
	    if (!GET_MOTION_COORD_FLAG() || !GET_MOTION_ENABLE_FLAG()) {
		reportError
		    ("need to be enabled, in coord mode for linear move");
		emcmotStatus->commandStatus = EMCMOT_COMMAND_INVALID_COMMAND;
		SET_MOTION_ERROR_FLAG(1);
		break;
	    } else if (!inRange(emcmotCommand->pos)) {
		if(emcmotCommand->id > 0)
		    reportError("linear move on line %d would exceed limits",
			    emcmotCommand->id);
		else
		    reportError("linear move in MDI would exceed limits");
		emcmotStatus->commandStatus = EMCMOT_COMMAND_INVALID_PARAMS;
		tpAbort(&emcmotDebug->queue);
		SET_MOTION_ERROR_FLAG(1);
		break;
	    } else if (!limits_ok()) {
		reportError("can't do linear move with limits exceeded");
		emcmotStatus->commandStatus = EMCMOT_COMMAND_INVALID_PARAMS;
		tpAbort(&emcmotDebug->queue);
		SET_MOTION_ERROR_FLAG(1);
		break;
	    }

	    /* append it to the emcmotDebug->queue */
	    tpSetId(&emcmotDebug->queue, emcmotCommand->id);
	    if (-1 == tpAddLine(&emcmotDebug->queue, emcmotCommand->pos, emcmotCommand->motion_type, emcmotCommand->vel, emcmotCommand->ini_maxvel, emcmotCommand->acc, emcmotStatus->enables_new)) {
		reportError("can't add linear move");
		emcmotStatus->commandStatus = EMCMOT_COMMAND_BAD_EXEC;
		tpAbort(&emcmotDebug->queue);
		SET_MOTION_ERROR_FLAG(1);
		break;
	    } else {
		SET_MOTION_ERROR_FLAG(0);
		/* set flag that indicates all axes need rehoming, if any
		   axis is moved in joint mode, for machines with no forward
		   kins */
		rehomeAll = 1;
	    }
	    break;

	case EMCMOT_SET_CIRCLE:
	    /* emcmotDebug->queue up a circular move */
	    /* requires coordinated mode, enable on, not on limits */
	    rtapi_print_msg(RTAPI_MSG_DBG, "SET_CIRCLE");
	    if (!GET_MOTION_COORD_FLAG() || !GET_MOTION_ENABLE_FLAG()) {
		reportError
		    ("need to be enabled, in coord mode for circular move");
		emcmotStatus->commandStatus = EMCMOT_COMMAND_INVALID_COMMAND;
		SET_MOTION_ERROR_FLAG(1);
		break;
	    } else if (!inRange(emcmotCommand->pos)) {
		if(emcmotCommand->id > 0)
		    reportError("circular move on line %d would exceed limits",
			emcmotCommand->id);
		else
		    reportError("circular move in MDI would exceed limits");
		emcmotStatus->commandStatus = EMCMOT_COMMAND_INVALID_PARAMS;
		tpAbort(&emcmotDebug->queue);
		SET_MOTION_ERROR_FLAG(1);
		break;
	    } else if (!limits_ok()) {
		reportError("can't do circular move with limits exceeded");
		emcmotStatus->commandStatus = EMCMOT_COMMAND_INVALID_PARAMS;
		tpAbort(&emcmotDebug->queue);
		SET_MOTION_ERROR_FLAG(1);
		break;
	    }

	    /* append it to the emcmotDebug->queue */
	    tpSetId(&emcmotDebug->queue, emcmotCommand->id);
	    if (-1 ==
		tpAddCircle(&emcmotDebug->queue, emcmotCommand->pos,
		    emcmotCommand->center, emcmotCommand->normal,
		    emcmotCommand->turn, emcmotCommand->motion_type,
                    emcmotCommand->vel, emcmotCommand->ini_maxvel,
                    emcmotCommand->acc, emcmotStatus->enables_new)) {
		reportError("can't add circular move");
		emcmotStatus->commandStatus = EMCMOT_COMMAND_BAD_EXEC;
		tpAbort(&emcmotDebug->queue);
		SET_MOTION_ERROR_FLAG(1);
		break;
	    } else {
		SET_MOTION_ERROR_FLAG(0);
		/* set flag that indicates all axes need rehoming, if any
		   axis is moved in joint mode, for machines with no forward
		   kins */
		rehomeAll = 1;
	    }
	    break;

	case EMCMOT_SET_VEL:
	    /* set the velocity for subsequent moves */
	    /* can do it at any time */
	    rtapi_print_msg(RTAPI_MSG_DBG, "SET_VEL");
	    emcmotStatus->vel = emcmotCommand->vel;
	    tpSetVmax(&emcmotDebug->queue, emcmotStatus->vel, 
			    emcmotCommand->ini_maxvel);
	    break;

	case EMCMOT_SET_VEL_LIMIT:
	    rtapi_print_msg(RTAPI_MSG_DBG, "SET_VEL_LIMIT");
	    emcmot_config_change();
	    /* set the absolute max velocity for all subsequent moves */
	    /* can do it at any time */
	    emcmotConfig->limitVel = emcmotCommand->vel;
	    tpSetVlimit(&emcmotDebug->queue, emcmotConfig->limitVel);
	    break;

	case EMCMOT_SET_JOINT_VEL_LIMIT:
	    rtapi_print_msg(RTAPI_MSG_DBG, "SET_JOINT_VEL_LIMIT");
	    rtapi_print_msg(RTAPI_MSG_DBG, " %d", joint_num);
	    emcmot_config_change();
	    /* check axis range */
	    if (joint == 0) {
		break;
	    }
	    joint->vel_limit = emcmotCommand->vel;
	    joint->big_vel = 10 * emcmotCommand->vel;
	    break;

	case EMCMOT_SET_JOINT_ACC_LIMIT:
	    rtapi_print_msg(RTAPI_MSG_DBG, "SET_JOINT_ACC_LIMIT");
	    rtapi_print_msg(RTAPI_MSG_DBG, " %d", joint_num);
	    emcmot_config_change();
	    /* check axis range */
	    if (joint == 0) {
		break;
	    }
	    joint->acc_limit = emcmotCommand->acc;
	    break;

	case EMCMOT_SET_ACC:
	    /* set the max acceleration */
	    /* can do it at any time */
	    rtapi_print_msg(RTAPI_MSG_DBG, "SET_ACCEL");
	    emcmotStatus->acc = emcmotCommand->acc;
	    tpSetAmax(&emcmotDebug->queue, emcmotStatus->acc);
	    break;

	case EMCMOT_PAUSE:
	    /* pause the motion */
	    /* can happen at any time */
	    rtapi_print_msg(RTAPI_MSG_DBG, "PAUSE");
	    tpPause(&emcmotDebug->queue);
	    emcmotStatus->paused = 1;
	    break;

	case EMCMOT_RESUME:
	    /* resume paused motion */
	    /* can happen at any time */
	    rtapi_print_msg(RTAPI_MSG_DBG, "RESUME");
	    emcmotDebug->stepping = 0;
	    tpResume(&emcmotDebug->queue);
	    emcmotStatus->paused = 0;
	    break;

	case EMCMOT_STEP:
	    /* resume paused motion until id changes */
	    /* can happen at any time */
            rtapi_print_msg(RTAPI_MSG_DBG, "STEP");
            if(emcmotStatus->paused) {
                emcmotDebug->idForStep = emcmotStatus->id;
                emcmotDebug->stepping = 1;
                tpResume(&emcmotDebug->queue);
                emcmotStatus->paused = 1;
            } else {
		reportError("MOTION: can't STEP while already executing");
	    }
	    break;

	case EMCMOT_FEED_SCALE:
	    /* override speed */
	    /* can happen at any time */
	    rtapi_print_msg(RTAPI_MSG_DBG, "FEED SCALE");
	    if (emcmotCommand->scale < 0.0) {
		emcmotCommand->scale = 0.0;	/* clamp it */
	    }
	    emcmotStatus->feed_scale = emcmotCommand->scale;
	    break;

	case EMCMOT_FS_ENABLE:
	    /* enable/disable overriding speed */
	    /* can happen at any time */
	    if ( emcmotCommand->mode != 0 ) {
		rtapi_print_msg(RTAPI_MSG_DBG, "FEED SCALE: ON");
		emcmotStatus->enables_new |= FS_ENABLED;
            } else {
		rtapi_print_msg(RTAPI_MSG_DBG, "FEED SCALE: OFF");
		emcmotStatus->enables_new &= ~FS_ENABLED;
	    }
	    break;

	case EMCMOT_FH_ENABLE:
	    /* enable/disable feed hold */
	    /* can happen at any time */
	    if ( emcmotCommand->mode != 0 ) {
		rtapi_print_msg(RTAPI_MSG_DBG, "FEED HOLD: ENABLED");
		emcmotStatus->enables_new |= FH_ENABLED;
            } else {
		rtapi_print_msg(RTAPI_MSG_DBG, "FEED HOLD: DISABLED");
		emcmotStatus->enables_new &= ~FH_ENABLED;
	    }
	    break;

	case EMCMOT_SPINDLE_SCALE:
	    /* override spindle speed */
	    /* can happen at any time */
	    rtapi_print_msg(RTAPI_MSG_DBG, "SPINDLE SCALE");
	    if (emcmotCommand->scale < 0.0) {
		emcmotCommand->scale = 0.0;	/* clamp it */
	    }
	    emcmotStatus->spindle_scale = emcmotCommand->scale;
	    break;

	case EMCMOT_SS_ENABLE:
	    /* enable/disable overriding spindle speed */
	    /* can happen at any time */
	    if ( emcmotCommand->mode != 0 ) {
		rtapi_print_msg(RTAPI_MSG_DBG, "SPINDLE SCALE: ON");
		emcmotStatus->enables_new |= SS_ENABLED;
            } else {
		rtapi_print_msg(RTAPI_MSG_DBG, "SPINDLE SCALE: OFF");
		emcmotStatus->enables_new &= ~SS_ENABLED;
	    }
	    break;

	case EMCMOT_AF_ENABLE:
	    /* enable/disable adaptive feedrate override from HAL pin */
	    /* can happen at any time */
	    if ( emcmotCommand->flags != 0 ) {
		rtapi_print_msg(RTAPI_MSG_DBG, "ADAPTIVE FEED: ON");
		emcmotStatus->enables_new |= AF_ENABLED;
            } else {
		rtapi_print_msg(RTAPI_MSG_DBG, "ADAPTIVE FEED: OFF");
		emcmotStatus->enables_new &= ~AF_ENABLED;
	    }
	    break;

	case EMCMOT_DISABLE:
	    /* go into disable */
	    /* can happen at any time */
	    /* reset the emcmotDebug->enabling flag to defer disable until
	       controller cycle (it *will* be honored) */
	    rtapi_print_msg(RTAPI_MSG_DBG, "DISABLE");
	    emcmotDebug->enabling = 0;
	    if (kinType == KINEMATICS_INVERSE_ONLY) {
		emcmotDebug->teleoperating = 0;
		emcmotDebug->coordinating = 0;
	    }
	    break;

	case EMCMOT_ENABLE:
	    /* come out of disable */
	    /* can happen at any time */
	    /* set the emcmotDebug->enabling flag to defer enable until
	       controller cycle */
	    rtapi_print_msg(RTAPI_MSG_DBG, "ENABLE");
	    if ( *(emcmot_hal_data->enable) == 0 ) {
		reportError("can't enable motion, enable input is false");
	    } else {
		emcmotDebug->enabling = 1;
		if (kinType == KINEMATICS_INVERSE_ONLY) {
		    emcmotDebug->teleoperating = 0;
		    emcmotDebug->coordinating = 0;
		}
	    }
	    break;

	case EMCMOT_ACTIVATE_JOINT:
	    /* make axis active, so that amps will be enabled when system is
	       enabled or disabled */
	    /* can be done at any time */
	    rtapi_print_msg(RTAPI_MSG_DBG, "ACTIVATE_JOINT");
	    rtapi_print_msg(RTAPI_MSG_DBG, " %d", joint_num);
	    if (joint == 0) {
		break;
	    }
	    SET_JOINT_ACTIVE_FLAG(joint, 1);
	    break;

	case EMCMOT_DEACTIVATE_JOINT:
	    /* make axis inactive, so that amps won't be affected when system
	       is enabled or disabled */
	    /* can be done at any time */
	    rtapi_print_msg(RTAPI_MSG_DBG, "DEACTIVATE_AXIS");
	    rtapi_print_msg(RTAPI_MSG_DBG, " %d", joint_num);
	    if (joint == 0) {
		break;
	    }
	    SET_JOINT_ACTIVE_FLAG(joint, 0);
	    break;
/*! \todo FIXME - need to replace the ext function */
	case EMCMOT_ENABLE_AMPLIFIER:
	    /* enable the amplifier directly, but don't enable calculations */
	    /* can be done at any time */
	    rtapi_print_msg(RTAPI_MSG_DBG, "ENABLE_AMP");
	    rtapi_print_msg(RTAPI_MSG_DBG, " %d", joint_num);
	    if (joint == 0) {
		break;
	    }
/*! \todo Another #if 0 */
#if 0
	    extAmpEnable(axis, 1);
#endif
	    break;

	case EMCMOT_DISABLE_AMPLIFIER:
	    /* disable the axis calculations and amplifier, but don't disable
	       calculations */
	    /* can be done at any time */
	    rtapi_print_msg(RTAPI_MSG_DBG, "DISABLE_AMP");
	    rtapi_print_msg(RTAPI_MSG_DBG, " %d", joint_num);
	    if (joint == 0) {
		break;
	    }
/*! \todo Another #if 0 */
#if 0
	    extAmpEnable(axis, 0);
#endif
	    break;

	case EMCMOT_HOME:
	    /* home the specified axis */
	    /* need to be in free mode, enable on */
	    /* this just sets the initial state, then the state machine in
	       control.c does the rest */
	    rtapi_print_msg(RTAPI_MSG_DBG, "HOME");
	    rtapi_print_msg(RTAPI_MSG_DBG, " %d", joint_num);

	    if (emcmotStatus->motion_state != EMCMOT_MOTION_FREE) {
		/* can't home unless in free mode */
		reportError("must be in joint mode to home");
		return;
	    }
	    if (!GET_MOTION_ENABLE_FLAG()) {
		break;
	    }

	    if(joint_num == -1) {
                if(emcmotStatus->homingSequenceState == HOME_SEQUENCE_IDLE)
                    emcmotStatus->homingSequenceState = HOME_SEQUENCE_START;
                else
                    reportError("homing sequence already in progress");
		break;
	    }

	    if (joint == NULL) {
		break;
	    }

            if(joint->home_state != HOME_IDLE) {
                reportError("homing already in progress");
            } else if(emcmotStatus->homingSequenceState != HOME_SEQUENCE_IDLE) {
                reportError("homing sequence already in progress");
            } else {
                /* abort any movement (jog, etc) that is in progress */
                joint->free_tp_enable = 0;
                
                /* prime the homing state machine */
                joint->home_state = HOME_START;
            }
	    break;

	case EMCMOT_ENABLE_WATCHDOG:
	    rtapi_print_msg(RTAPI_MSG_DBG, "ENABLE_WATCHDOG");
/*! \todo Another #if 0 */
#if 0
	    emcmotDebug->wdEnabling = 1;
	    emcmotDebug->wdWait = emcmotCommand->wdWait;
	    if (emcmotDebug->wdWait < 0) {
		emcmotDebug->wdWait = 0;
	    }
#endif
	    break;

	case EMCMOT_DISABLE_WATCHDOG:
	    rtapi_print_msg(RTAPI_MSG_DBG, "DISABLE_WATCHDOG");
/*! \todo Another #if 0 */
#if 0
	    emcmotDebug->wdEnabling = 0;
#endif
	    break;

	case EMCMOT_CLEAR_PROBE_FLAGS:
	    rtapi_print_msg(RTAPI_MSG_DBG, "CLEAR_PROBE_FLAGS");
	    emcmotStatus->probing = 0;
	    break;

	case EMCMOT_PROBE:
	    /* most of this is taken from EMCMOT_SET_LINE */
	    /* emcmotDebug->queue up a linear move */
	    /* requires coordinated mode, enable off, not on limits */
	    rtapi_print_msg(RTAPI_MSG_DBG, "PROBE");
	    if (!GET_MOTION_COORD_FLAG() || !GET_MOTION_ENABLE_FLAG()) {
		reportError
		    ("need to be enabled, in coord mode for probe move");
		emcmotStatus->commandStatus = EMCMOT_COMMAND_INVALID_COMMAND;
		SET_MOTION_ERROR_FLAG(1);
		break;
	    } else if (!inRange(emcmotCommand->pos)) {
		if(emcmotCommand->id > 0)
		    reportError("probe move on line %d would exceed limits",
			    emcmotCommand->id);
		else
		    reportError("probe move in MDI would exceed limits");
		emcmotStatus->commandStatus = EMCMOT_COMMAND_INVALID_PARAMS;
		tpAbort(&emcmotDebug->queue);
		SET_MOTION_ERROR_FLAG(1);
		break;
	    } else if (!limits_ok()) {
		reportError("can't do probe move with limits exceeded");
		emcmotStatus->commandStatus = EMCMOT_COMMAND_INVALID_PARAMS;
		tpAbort(&emcmotDebug->queue);
		SET_MOTION_ERROR_FLAG(1);
		break;
	    }

	    /* append it to the emcmotDebug->queue */
	    tpSetId(&emcmotDebug->queue, emcmotCommand->id);
	    if (-1 == tpAddLine(&emcmotDebug->queue, emcmotCommand->pos, emcmotCommand->motion_type, emcmotCommand->vel, emcmotCommand->ini_maxvel, emcmotCommand->acc, emcmotStatus->enables_new)) {
		reportError("can't add probe move");
		emcmotStatus->commandStatus = EMCMOT_COMMAND_BAD_EXEC;
		tpAbort(&emcmotDebug->queue);
		SET_MOTION_ERROR_FLAG(1);
		break;
	    } else {
		emcmotStatus->probing = 1;
		SET_MOTION_ERROR_FLAG(0);
		/* set flag that indicates all axes need rehoming, if any
		   axis is moved in joint mode, for machines with no forward
		   kins */
		rehomeAll = 1;
	    }
	    break;


	case EMCMOT_RIGID_TAP:
	    /* most of this is taken from EMCMOT_SET_LINE */
	    /* emcmotDebug->queue up a linear move */
	    /* requires coordinated mode, enable off, not on limits */
	    rtapi_print_msg(RTAPI_MSG_DBG, "RIGID_TAP");
	    if (!GET_MOTION_COORD_FLAG() || !GET_MOTION_ENABLE_FLAG()) {
		reportError
		    ("need to be enabled, in coord mode for rigid tap move");
		emcmotStatus->commandStatus = EMCMOT_COMMAND_INVALID_COMMAND;
		SET_MOTION_ERROR_FLAG(1);
		break;
	    } else if (!inRange(emcmotCommand->pos)) {
		if(emcmotCommand->id > 0)
		    reportError("rigid tap move on line %d would exceed limits",
			    emcmotCommand->id);
		else
		    reportError("rigid tap move in MDI would exceed limits");
		emcmotStatus->commandStatus = EMCMOT_COMMAND_INVALID_PARAMS;
		tpAbort(&emcmotDebug->queue);
		SET_MOTION_ERROR_FLAG(1);
		break;
	    } else if (!limits_ok()) {
		reportError("can't do rigid tap move with limits exceeded");
		emcmotStatus->commandStatus = EMCMOT_COMMAND_INVALID_PARAMS;
		tpAbort(&emcmotDebug->queue);
		SET_MOTION_ERROR_FLAG(1);
		break;
	    }

	    /* append it to the emcmotDebug->queue */
	    tpSetId(&emcmotDebug->queue, emcmotCommand->id);
	    if (-1 == tpAddRigidTap(&emcmotDebug->queue, emcmotCommand->pos, emcmotCommand->vel, emcmotCommand->ini_maxvel, emcmotCommand->acc, emcmotStatus->enables_new)) {
		reportError("can't add rigid tap move");
		emcmotStatus->commandStatus = EMCMOT_COMMAND_BAD_EXEC;
		tpAbort(&emcmotDebug->queue);
		SET_MOTION_ERROR_FLAG(1);
		break;
	    } else {
		SET_MOTION_ERROR_FLAG(0);
	    }
	    break;

	case EMCMOT_SET_TELEOP_VECTOR:
	    rtapi_print_msg(RTAPI_MSG_DBG, "SET_TELEOP_VECTOR");
	    if (!GET_MOTION_TELEOP_FLAG() || !GET_MOTION_ENABLE_FLAG()) {
		reportError
		    ("need to be enabled, in teleop mode for teleop move");
	    } else {
		double velmag;
		emcmotDebug->teleop_data.desiredVel = emcmotCommand->pos;
		pmCartMag(emcmotDebug->teleop_data.desiredVel.tran, &velmag);
		if (emcmotDebug->teleop_data.desiredVel.a > velmag) {
		    velmag = emcmotDebug->teleop_data.desiredVel.a;
		}
		if (emcmotDebug->teleop_data.desiredVel.b > velmag) {
		    velmag = emcmotDebug->teleop_data.desiredVel.b;
		}
		if (emcmotDebug->teleop_data.desiredVel.c > velmag) {
		    velmag = emcmotDebug->teleop_data.desiredVel.c;
		}
		if (velmag > emcmotConfig->limitVel) {
		    pmCartScalMult(emcmotDebug->teleop_data.desiredVel.tran,
			emcmotConfig->limitVel / velmag,
			&emcmotDebug->teleop_data.desiredVel.tran);
		    emcmotDebug->teleop_data.desiredVel.a *=
			emcmotConfig->limitVel / velmag;
		    emcmotDebug->teleop_data.desiredVel.b *=
			emcmotConfig->limitVel / velmag;
		    emcmotDebug->teleop_data.desiredVel.c *=
			emcmotConfig->limitVel / velmag;
		}
		/* flag that all joints need to be homed, if any joint is
		   jogged individually later */
		rehomeAll = 1;
	    }
	    break;

	case EMCMOT_SET_DEBUG:
	    rtapi_print_msg(RTAPI_MSG_DBG, "SET_DEBUG");
	    emcmotConfig->debug = emcmotCommand->debug;
	    emcmot_config_change();
	    break;

	/* needed for synchronous I/O */
	case EMCMOT_SET_AOUT:
	    if (emcmotCommand->now) { //we set it right away
		emcmotAioWrite(emcmotCommand->out, emcmotCommand->minLimit);
	    } else { // we put it on the TP queue, warning: only room for one in there, any new ones will overwrite
		tpSetAout(&emcmotDebug->queue, emcmotCommand->out,
		    emcmotCommand->start, emcmotCommand->end);
	    }
	    break;

	case EMCMOT_SET_DOUT:
	    rtapi_print_msg(RTAPI_MSG_DBG, "SET_DOUT");
	    if (emcmotCommand->now) { //we set it right away
		emcmotDioWrite(emcmotCommand->out, emcmotCommand->start);
	    } else { // we put it on the TP queue, warning: only room for one in there, any new ones will overwrite
		tpSetDout(&emcmotDebug->queue, emcmotCommand->out,
		    emcmotCommand->start, emcmotCommand->end);
	    }
	    break;

	case EMCMOT_SET_SPINDLE_VEL:
	    rtapi_print_msg(RTAPI_MSG_DBG, "SET_SPINDLE_VEL");
	    emcmotStatus->spindle.speed = emcmotCommand->vel;
	    break;
	    
	case EMCMOT_SPINDLE_ON:
	    rtapi_print_msg(RTAPI_MSG_DBG, "SPINDLE_ON");
	    emcmotStatus->spindle.speed = emcmotCommand->vel;
	    emcmotStatus->spindle.css_factor = emcmotCommand->ini_maxvel;
	    emcmotStatus->spindle.xoffset = emcmotCommand->acc;
	    if (emcmotCommand->vel >= 0) {
		emcmotStatus->spindle.direction = 1;
	    } else {
		emcmotStatus->spindle.direction = -1;
	    }
	    emcmotStatus->spindle.brake = 0; //disengage brake
	    break;

	case EMCMOT_SPINDLE_OFF:
	    rtapi_print_msg(RTAPI_MSG_DBG, "SPINDLE_OFF");
	    emcmotStatus->spindle.speed = 0;
	    emcmotStatus->spindle.direction = 0;
	    emcmotStatus->spindle.brake = 1; // engage brake
	    break;

	case EMCMOT_SPINDLE_INCREASE:
	    rtapi_print_msg(RTAPI_MSG_DBG, "SPINDLE_INCREASE");
	    if (emcmotStatus->spindle.speed > 0) {
		emcmotStatus->spindle.speed += 100; //FIXME - make the step a HAL parameter
	    } else if (emcmotStatus->spindle.speed < 0) {
		emcmotStatus->spindle.speed -= 100;
	    }
	    break;

	case EMCMOT_SPINDLE_DECREASE:
	    rtapi_print_msg(RTAPI_MSG_DBG, "SPINDLE_DECREASE");
	    if (emcmotStatus->spindle.speed > 100) {
		emcmotStatus->spindle.speed -= 100; //FIXME - make the step a HAL parameter
	    } else if (emcmotStatus->spindle.speed < -100) {
		emcmotStatus->spindle.speed += 100;
	    }
	    break;

	case EMCMOT_SPINDLE_BRAKE_ENGAGE:
	    rtapi_print_msg(RTAPI_MSG_DBG, "SPINDLE_BRAKE_ENGAGE");
	    emcmotStatus->spindle.speed = 0;
	    emcmotStatus->spindle.direction = 0;
	    emcmotStatus->spindle.brake = 1;
	    break;

	case EMCMOT_SPINDLE_BRAKE_RELEASE:
	    rtapi_print_msg(RTAPI_MSG_DBG, "SPINDLE_BRAKE_RELEASE");
	    emcmotStatus->spindle.brake = 0;
	    break;

	case EMCMOT_SET_JOINT_COMP:
	    rtapi_print_msg(RTAPI_MSG_DBG, "SET_JOINT_COMP for joint %d", joint_num);
	    if (joint == 0) {
		break;
	    }
	    if (joint->comp.entries >= EMCMOT_COMP_SIZE) {
		reportError("joint %d: too many compensation entries", joint_num);
		break;
	    }
	    /* point to last entry */
	    comp_entry = &(joint->comp.array[joint->comp.entries]);
	    if (emcmotCommand->comp_nominal <= comp_entry[0].nominal) {
		reportError("joint %d: compensation values must increase", joint_num);
		break;
	    }
	    /* store data to new entry */
	    comp_entry[1].nominal = emcmotCommand->comp_nominal;
	    comp_entry[1].fwd_trim = emcmotCommand->comp_forward;
	    comp_entry[1].rev_trim = emcmotCommand->comp_reverse;
	    /* calculate slopes from previous entry to the new one */
	    if ( comp_entry[0].nominal != -HUGE_VAL ) {
		/* but only if the previous entry is "real" */
		tmp1 = comp_entry[1].nominal - comp_entry[0].nominal;
		comp_entry[0].fwd_slope =
		    (comp_entry[1].fwd_trim - comp_entry[0].fwd_trim) / tmp1;
		comp_entry[0].rev_slope =
		    (comp_entry[1].rev_trim - comp_entry[0].rev_trim) / tmp1;
	    } else {
		/* previous entry is at minus infinity, slopes are zero */
		comp_entry[0].fwd_trim = comp_entry[1].fwd_trim;
		comp_entry[0].rev_trim = comp_entry[1].rev_trim;
	    }
	    joint->comp.entries++;
	    break;

	default:
	    rtapi_print_msg(RTAPI_MSG_DBG, "UNKNOWN");
	    reportError("unrecognized command %d", emcmotCommand->command);
	    emcmotStatus->commandStatus = EMCMOT_COMMAND_UNKNOWN_COMMAND;
	    break;

	}			/* end of: command switch */
	if (emcmotStatus->commandStatus != EMCMOT_COMMAND_OK) {
	    rtapi_print_msg(RTAPI_MSG_DBG, "ERRROR: %d",
		emcmotStatus->commandStatus);
	}
	rtapi_print_msg(RTAPI_MSG_DBG, "\n");
	/* synch tail count */
	emcmotStatus->tail = emcmotStatus->head;
	emcmotConfig->tail = emcmotConfig->head;
	emcmotDebug->tail = emcmotDebug->head;

    }
    /* end of: if-new-command */
check_stuff ( "after command_handler()" );

    return;
}
