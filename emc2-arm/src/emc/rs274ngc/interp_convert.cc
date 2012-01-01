/********************************************************************
* Description: interp_convert.cc
*
*   Derived from a work by Thomas Kramer
*
* Author:
* License: GPL Version 2
* System: Linux
*    
* Copyright (c) 2004 All rights reserved.
*
* Last change:
* $Revision: 1.96.2.1 $
* $Author: jepler $
* $Date: 2007/11/29 03:22:48 $
********************************************************************/
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "rs274ngc.hh"
#include "rs274ngc_return.hh"
#include "interp_internal.hh"

#include "units.h"

// lathe tools have strange origin points that are not at
// the center of the radius.  This means that the point that
// radius compensation controls (center of radius) is not at
// the tool's origin.  These functions do the necessary
// translation.  Notice tool orientations 0 (mill) and 9, and 
// those with radius 0 (a point) do not need any translation.

static double xtrans(setup_pointer settings, double x) {
    int o = settings->cutter_comp_orientation;
    double r = settings->cutter_comp_radius;

    if(o==2 || o==6 || o==1) x -= r;
    if(o==3 || o==8 || o==4) x += r;
    return x;
}

static double ztrans(setup_pointer settings, double z) {
    int o = settings->cutter_comp_orientation;
    double r = settings->cutter_comp_radius;

    if(o==2 || o==7 || o==3) z -= r;
    if(o==1 || o==5 || o==4) z += r;
    return z;
}

/****************************************************************************/

/*! convert_arc

Returned Value: int
   If one of the following functions returns an error code,
   this returns that error code.
      convert_arc_comp1
      convert_arc_comp2
      convert_arc2
   If any of the following errors occur, this returns the error code shown.
   Otherwise, this returns INTERP_OK.
   1. The block has neither an r value nor any i,j,k values:
      NCE_R_I_J_K_WORDS_ALL_MISSING_FOR_ARC
   2. The block has both an r value and one or more i,j,k values:
      NCE_MIXED_RADIUS_IJK_FORMAT_FOR_ARC
   3. In the ijk format the XY-plane is selected and
      the block has a k value: NCE_K_WORD_GIVEN_FOR_ARC_IN_XY_PLANE
   4. In the ijk format the YZ-plane is selected and
      the block has an i value: NCE_I_WORD_GIVEN_FOR_ARC_IN_YZ_PLANE
   5. In the ijk format the XZ-plane is selected and
      the block has a j value: NCE_J_WORD_GIVEN_FOR_ARC_IN_XZ_PLANE
   6. In either format any of the following occurs.
      a. The XY-plane is selected and the block has no x or y value:
         NCE_X_AND_Y_WORDS_MISSING_FOR_ARC_IN_XY_PLANE
      b. The YZ-plane is selected and the block has no y or z value:
         NCE_Y_AND_Z_WORDS_MISSING_FOR_ARC_IN_YZ_PLANE
      c. The ZX-plane is selected and the block has no z or x value:
         NCE_X_AND_Z_WORDS_MISSING_FOR_ARC_IN_XZ_PLANE
   7. The selected plane is an unknown plane:
      NCE_BUG_PLANE_NOT_XY_YZ__OR_XZ
   8. The feed rate mode is UNITS_PER_MINUTE and feed rate is zero:
      NCE_CANNOT_MAKE_ARC_WITH_ZERO_FEED_RATE
   9. The feed rate mode is INVERSE_TIME and the block has no f word:
      NCE_F_WORD_MISSING_WITH_INVERSE_TIME_ARC_MOVE

Side effects:
   This generates and executes an arc command at feed rate
   (and, possibly a second arc command). It also updates the setting
   of the position of the tool point to the end point of the move.

Called by: convert_motion.

This converts a helical or circular arc.  The function calls:
convert_arc2 (when cutter radius compensation is off) or
convert_arc_comp1 (when cutter comp is on and this is the first move) or
convert_arc_comp2 (when cutter comp is on and this is not the first move).

If the ijk format is used, at least one of the offsets in the current
plane must be given in the block; it is common but not required to
give both offsets. The offsets are always incremental [NCMS, page 21].

*/

int Interp::convert_arc(int move,        //!< either G_2 (cw arc) or G_3 (ccw arc)    
                       block_pointer block,     //!< pointer to a block of RS274 instructions
                       setup_pointer settings)  //!< pointer to machine settings             
{
  static char name[] = "convert_arc";
  int status;
  int first;                    /* flag set ON if this is first move after comp ON */
  int ijk_flag;                 /* flag set ON if any of i,j,k present in NC code  */
  double end_x;
  double end_y;
  double end_z;
  double AA_end;
  double BB_end;
  double CC_end;
  double u_end, v_end, w_end;

  ijk_flag = ((block->i_flag || block->j_flag) || block->k_flag) ? ON : OFF;
  first = settings->cutter_comp_firstmove == ON;

  CHK(((block->r_flag != ON) && (ijk_flag != ON)),
      NCE_R_I_J_K_WORDS_ALL_MISSING_FOR_ARC);
  CHK(((block->r_flag == ON) && (ijk_flag == ON)),
      NCE_MIXED_RADIUS_IJK_FORMAT_FOR_ARC);
  if (settings->feed_mode == UNITS_PER_MINUTE) {
    CHK((settings->feed_rate == 0.0),
        NCE_CANNOT_MAKE_ARC_WITH_ZERO_FEED_RATE);
  } else if(settings->feed_mode == UNITS_PER_REVOLUTION) {
    CHK((settings->feed_rate == 0.0),
        NCE_CANNOT_MAKE_ARC_WITH_ZERO_FEED_RATE);
    CHKS((settings->speed == 0.0),
	"Cannot feed with zero spindle speed in feed per rev mode");
  } else if (settings->feed_mode == INVERSE_TIME) {
    CHK((block->f_number == -1.0),
        NCE_F_WORD_MISSING_WITH_INVERSE_TIME_ARC_MOVE);
  }
  if (ijk_flag) {
    if (settings->plane == CANON_PLANE_XY) {
      CHK((block->k_flag), NCE_K_WORD_GIVEN_FOR_ARC_IN_XY_PLANE);
      if (block->i_flag == OFF) /* i or j flag on to get here */
        block->i_number = 0.0;
      else if (block->j_flag == OFF)
        block->j_number = 0.0;
    } else if (settings->plane == CANON_PLANE_YZ) {
      CHK((block->i_flag), NCE_I_WORD_GIVEN_FOR_ARC_IN_YZ_PLANE);
      if (block->j_flag == OFF) /* j or k flag on to get here */
        block->j_number = 0.0;
      else if (block->k_flag == OFF)
        block->k_number = 0.0;
    } else if (settings->plane == CANON_PLANE_XZ) {
      CHK((block->j_flag), NCE_J_WORD_GIVEN_FOR_ARC_IN_XZ_PLANE);
      if (block->i_flag == OFF) /* i or k flag on to get here */
        block->i_number = 0.0;
      else if (block->k_flag == OFF)
        block->k_number = 0.0;
    } else
      ERM(NCE_BUG_PLANE_NOT_XY_YZ_OR_XZ);
  } else;                       /* r format arc; no other checks needed specific to this format */

  if (settings->plane == CANON_PLANE_XY) {      /* checks for both formats */
    CHK(((block->x_flag == OFF) && (block->y_flag == OFF)),
        NCE_X_AND_Y_WORDS_MISSING_FOR_ARC_IN_XY_PLANE);
  } else if (settings->plane == CANON_PLANE_YZ) {
    CHK(((block->y_flag == OFF) && (block->z_flag == OFF)),
        NCE_Y_AND_Z_WORDS_MISSING_FOR_ARC_IN_YZ_PLANE);
  } else if (settings->plane == CANON_PLANE_XZ) {
    CHK(((block->x_flag == OFF) && (block->z_flag == OFF)),
        NCE_X_AND_Z_WORDS_MISSING_FOR_ARC_IN_XZ_PLANE);
  }

  find_ends(block, settings, &end_x, &end_y, &end_z,
            &AA_end, &BB_end, &CC_end, 
            &u_end, &v_end, &w_end);

  settings->motion_mode = move;

  if (settings->plane == CANON_PLANE_XY) {
    if ((settings->cutter_comp_side == OFF) ||
        (settings->cutter_comp_radius == 0.0)) {
      status =
        convert_arc2(move, block, settings,
                     &(settings->current_x), &(settings->current_y),
                     &(settings->current_z), end_x, end_y, end_z,
                     AA_end, BB_end, CC_end,
                     u_end, v_end, w_end,
                     block->i_number, block->j_number);
      CHP(status);
    } else if (first) {
      status = convert_arc_comp1(move, block, settings, end_x, end_y, end_z,
                                 AA_end, BB_end, CC_end,
                                 u_end, v_end, w_end);
      CHP(status);
    } else {
      status = convert_arc_comp2(move, block, settings, end_x, end_y, end_z,
                                 AA_end, BB_end, CC_end,
                                 u_end, v_end, w_end);
      CHP(status);
    }
  } else if (settings->plane == CANON_PLANE_XZ) {
    if ((settings->cutter_comp_side == OFF) ||
        (settings->cutter_comp_radius == 0.0)) {
      status =
        convert_arc2(move, block, settings,
                     &(settings->current_z), &(settings->current_x),
                     &(settings->current_y), end_z, end_x, end_y,
                     AA_end, BB_end, CC_end,
                     u_end, v_end, w_end,
                     block->k_number, block->i_number);
      CHP(status);
    } else if (first) {
      status = convert_arc_comp1(move, block, settings, end_x, end_y, end_z,
                                 AA_end, BB_end, CC_end,
                                 u_end, v_end, w_end);
      CHP(status);
    } else {
      status = convert_arc_comp2(move, block, settings, end_x, end_y, end_z,
                                 AA_end, BB_end, CC_end,
                                 u_end, v_end, w_end);

      CHP(status);
    }
  } else if (settings->plane == CANON_PLANE_YZ) {
    status =
      convert_arc2(move, block, settings,
                   &(settings->current_y), &(settings->current_z),
                   &(settings->current_x), end_y, end_z, end_x,
                   AA_end, BB_end, CC_end,
                   u_end, v_end, w_end,
                   block->j_number, block->k_number);
    CHP(status);
  } else
    ERM(NCE_BUG_PLANE_NOT_XY_YZ_OR_XZ);
  return INTERP_OK;
}

/****************************************************************************/

/*! convert_arc2

Returned Value: int
   If arc_data_ijk or arc_data_r returns an error code,
   this returns that code.
   Otherwise, it returns INTERP_OK.

Side effects:
   This executes an arc command at feed rate. It also updates the
   setting of the position of the tool point to the end point of the move.
   If inverse time feed rate is in effect, it also resets the feed rate.

Called by: convert_arc.

This converts a helical or circular arc.

*/

int Interp::convert_arc2(int move,       //!< either G_2 (cw arc) or G_3 (ccw arc)    
                        block_pointer block,    //!< pointer to a block of RS274 instructions
                        setup_pointer settings, //!< pointer to machine settings             
                        double *current1,       //!< pointer to current value of coordinate 1
                        double *current2,       //!< pointer to current value of coordinate 2
                        double *current3,       //!< pointer to current value of coordinate 3
                        double end1,    //!< coordinate 1 value at end of arc        
                        double end2,    //!< coordinate 2 value at end of arc        
                        double end3,    //!< coordinate 3 value at end of arc        
                        double AA_end,  //!< a-value at end of arc                   
                        double BB_end,  //!< b-value at end of arc                   
                        double CC_end,  //!< c-value at end of arc                   
                         double u, double v, double w, //!< values at end of arc
                        double offset1, //!< offset of center from current1          
                        double offset2) //!< offset of center from current2          
{
  static char name[] = "convert_arc2";
  double center1;
  double center2;
  int status;                   /* status returned from CHP function call     */
  double tolerance;             /* tolerance for difference of radii          */
  int turn;                     /* number of full or partial turns CCW in arc */
  int plane = settings->plane;

  tolerance = (settings->length_units == CANON_UNITS_INCHES) ?
    TOLERANCE_INCH : TOLERANCE_MM;

  if (block->r_flag) {
      CHP(arc_data_r(move, plane, *current1, *current2, end1, end2,
                   block->r_number, &center1, &center2, &turn, tolerance));
  } else {
      CHP(arc_data_ijk(move, plane, *current1, *current2, end1, end2, offset1,
                     offset2, &center1, &center2, &turn, tolerance));
  }

  if (settings->feed_mode == INVERSE_TIME)
    inverse_time_rate_arc(*current1, *current2, *current3, center1, center2,
                          turn, end1, end2, end3, block, settings);
  ARC_FEED(end1, end2, center1, center2, turn, end3,
           AA_end, BB_end, CC_end, u, v, w);
  *current1 = end1;
  *current2 = end2;
  *current3 = end3;
  settings->AA_current = AA_end;
  settings->BB_current = BB_end;
  settings->CC_current = CC_end;
  settings->u_current = u;
  settings->v_current = v;
  settings->w_current = w;
  
  return INTERP_OK;
}

/****************************************************************************/

/*! convert_arc_comp1

Returned Value: int
   If arc_data_comp_ijk or arc_data_comp_r returns an error code,
   this returns that code.
   Otherwise, it returns INTERP_OK.

Side effects:
   This executes an arc command at
   feed rate. It also updates the setting of the position of
   the tool point to the end point of the move.

Called by: convert_arc.

This function converts a helical or circular arc, generating only one
arc. This is called when cutter radius compensation is on and this is 
the first cut after the turning on.

The arc which is generated is derived from a second arc which passes
through the programmed end point and is tangent to the cutter at its
current location. The generated arc moves the tool so that it stays
tangent to the second arc throughout the move.

*/

int Interp::convert_arc_comp1(int move,  //!< either G_2 (cw arc) or G_3 (ccw arc)            
                             block_pointer block,       //!< pointer to a block of RS274/NGC instructions    
                             setup_pointer settings,    //!< pointer to machine settings                     
                             double end_x,      //!< x-value at end of programmed (then actual) arc  
                             double end_y,      //!< y-value at end of programmed (then actual) arc  
                             double end_z,      //!< z-value at end of arc                           
                             double AA_end,     //!< a-value at end of arc                     
                             double BB_end,     //!< b-value at end of arc                     
                              double CC_end,     //!< c-value at end of arc                     
                              double u_end, double v_end, double w_end) //!< uvw at end of arc
{
  static char name[] = "convert_arc_comp1";
  double center[2];
  double gamma;                 /* direction of perpendicular to arc at end */
  int side;                     /* offset side - right or left              */
  int status;                   /* status returned from CHP function call   */
  double tolerance;             /* tolerance for difference of radii        */
  double tool_radius;
  int turn;                     /* 1 for counterclockwise, -1 for clockwise */
  double end[3], current[3];
  int plane = settings->plane;

  side = settings->cutter_comp_side;
  tool_radius = settings->cutter_comp_radius;   /* always is positive */
  tolerance = (settings->length_units == CANON_UNITS_INCHES) ?
    TOLERANCE_INCH : TOLERANCE_MM;

  if(settings->plane == CANON_PLANE_XZ) {
      end[0] = end_x;
      end[1] = end_z;
      end[2] = end_y;
      current[0] = settings->current_x;
      current[1] = settings->current_z;
      current[2] = settings->current_y;
      if(move == G_2) move = G_3; else move = G_2;
  } else if (settings->plane == CANON_PLANE_XY) {
      end[0] = end_x;
      end[1] = end_y;
      end[2] = end_z;
      current[0] = settings->current_x;
      current[1] = settings->current_y;
      current[2] = settings->current_z;
  } else ERM(NCE_RADIUS_COMP_ONLY_IN_XY_OR_XZ);


  CHK((hypot((end[0] - current[0]),
             (end[1] - current[1])) <= tool_radius),
      NCE_CUTTER_GOUGING_WITH_CUTTER_RADIUS_COMP);

  if (block->r_flag) {
      CHP(arc_data_comp_r(move, plane, side, tool_radius, current[0],
                        current[1], end[0], end[1], block->r_number,
                        &center[0], &center[1], &turn, tolerance));
  } else {
      CHP(arc_data_comp_ijk(move, plane, side, tool_radius, current[0],
                          current[1], end[0], end[1],
                          block->i_number, 
                          settings->plane == CANON_PLANE_XZ? block->k_number: block->j_number,
                          &center[0], &center[1], &turn, tolerance));
  }

  gamma =
    (((side == LEFT) && (move == G_3)) ||
     ((side == RIGHT) && (move == G_2))) ?
    atan2((center[1] - end[1]), (center[0] - end[0])) :
    atan2((end[1] - center[1]), (end[0] - center[0]));

  settings->cutter_comp_firstmove = OFF;
  if(settings->plane == CANON_PLANE_XZ) {
    settings->program_x = end[0];
    settings->program_z = end[1];
    settings->program_y = end[2];
  } else if (settings->plane == CANON_PLANE_XY) {
    settings->program_x = end[0];
    settings->program_y = end[1];
    settings->program_z = end[2];
  }
  end[0] = (end[0] + (tool_radius * cos(gamma))); /* end_x reset actual */
  end[1] = (end[1] + (tool_radius * sin(gamma))); /* end_y reset actual */

  /* imagine a right triangle ABC with A being the endpoint of the
     compensated arc, B being the center of the compensated arc, C being
     the midpoint between start and end of the compensated arc. AB_ang
     is the direction of A->B.  A_ang is the angle of the triangle
     itself.  We need to find a new center for the compensated arc
     (point B). */

  double b_len = hypot(current[1] - end[1], current[0] - end[0]) / 2.0;
  double AB_ang = atan2(center[1] - end[1], center[0] - end[0]);
  double A_ang = atan2(current[1] - end[1], current[0] - end[0]) - AB_ang;

  CHK((fabs(cos(A_ang)) < TOLERANCE_EQUAL), NCE_CUTTER_GOUGING_WITH_CUTTER_RADIUS_COMP);
  
  double c_len = b_len/cos(A_ang);

  center[0] = end[0] + c_len * cos(AB_ang);
  center[1] = end[1] + c_len * sin(AB_ang);

  /* center to endpoint distances matched before - they still should. */

  CHK((fabs(hypot(center[0]-end[0],center[1]-end[1]) - 
            hypot(center[0]-current[0],center[1]-current[1])) > tolerance),
      NCE_BUG_IN_TOOL_RADIUS_COMP);

  if(settings->plane == CANON_PLANE_XZ) {
      if (settings->feed_mode == INVERSE_TIME)
        inverse_time_rate_straight(xtrans(settings, current[0]), current[2], ztrans(settings, current[1]),
                                   AA_end, BB_end, CC_end, u_end, v_end, w_end, block, settings);
      STRAIGHT_FEED(xtrans(settings, current[0]), current[2], ztrans(settings, current[1]),
                    AA_end, BB_end, CC_end, u_end, v_end, w_end);

      if (settings->feed_mode == INVERSE_TIME)
        inverse_time_rate_arc(current[0], current[1],
                              current[2], center[0], center[1], turn,
                              end[0], end[1], end[2], block, settings);
      ARC_FEED(ztrans(settings, end[1]), xtrans(settings, end[0]), 
               ztrans(settings, center[1]), xtrans(settings, center[0]), 
               -turn, end[2], AA_end, BB_end, CC_end, u_end, v_end, w_end);
      settings->current_x = end[0];
      settings->current_z = end[1];
      settings->current_y = end[2];
      settings->AA_current = AA_end;
      settings->BB_current = BB_end;
      settings->CC_current = CC_end;
      settings->u_current = u_end;
      settings->v_current = v_end;
      settings->w_current = w_end;
  } else if (settings->plane == CANON_PLANE_XY) {
      if (settings->feed_mode == INVERSE_TIME)
        inverse_time_rate_arc(current[0], current[1],
                              current[2], center[0], center[1], turn,
                              end[0], end[1], end[2], block, settings);
      ARC_FEED(end[0], end[1], center[0], center[1], turn, end[2],
               AA_end, BB_end, CC_end, u_end, v_end, w_end);
      settings->current_x = end[0];
      settings->current_y = end[1];
      settings->current_z = end[2];
      settings->AA_current = AA_end;
      settings->BB_current = BB_end;
      settings->CC_current = CC_end;
      settings->u_current = u_end;
      settings->v_current = v_end;
      settings->w_current = w_end;
  }

  return INTERP_OK;
}

/****************************************************************************/

/*! convert_arc_comp2

Returned Value: int
   If arc_data_ijk or arc_data_r returns an error code,
   this returns that code.
   If any of the following errors occurs, this returns the error code shown.
   Otherwise, it returns INTERP_OK.
   1. A concave corner is found: NCE_CONCAVE_CORNER_WITH_CUTTER_RADIUS_COMP
   2. The tool will not fit inside an arc:
      NCE_TOOL_RADIUS_NOT_LESS_THAN_ARC_RADIUS_WITH_COMP

Side effects:
   This executes an arc command feed rate. If needed, at also generates
   an arc to go around a convex corner. It also updates the setting of
   the position of the tool point to the end point of the move. If
   inverse time feed rate mode is in effect, the feed rate is reset.

Called by: convert_arc.

This function converts a helical or circular arc. The axis must be
parallel to the z-axis. This is called when cutter radius compensation
is on and this is not the first cut after the turning on.

If one or more rotary axes is moved in this block and an extra arc is
required to go around a sharp corner, all the rotary axis motion
occurs on the main arc and none on the extra arc.  An alternative
might be to distribute the rotary axis motion over the extra arc and
the programmed arc in proportion to their lengths.

If the Z-axis is moved in this block and an extra arc is required to
go around a sharp corner, all the Z-axis motion occurs on the main arc
and none on the extra arc.  An alternative might be to distribute the
Z-axis motion over the extra arc and the main arc in proportion to
their lengths.

*/

int Interp::convert_arc_comp2(int move,  //!< either G_2 (cw arc) or G_3 (ccw arc)          
                             block_pointer block,       //!< pointer to a block of RS274/NGC instructions  
                             setup_pointer settings,    //!< pointer to machine settings                   
                             double end_x,      //!< x-value at end of programmed (then actual) arc
                             double end_y,      //!< y-value at end of programmed (then actual) arc
                             double end_z,      //!< z-value at end of arc                         
                             double AA_end,     //!< a-value at end of arc
                             double BB_end,     //!< b-value at end of arc
                              double CC_end,     //!< c-value at end of arc
                              double u, double v, double w) //!< uvw at end of arc
{
  static char name[] = "convert_arc_comp2";
  double alpha;                 /* direction of tangent to start of arc */
  double arc_radius;
  double beta;                  /* angle between two tangents above */
  double center[2];              /* center of arc */
  double delta;                 /* direction of radius from start of arc to center of arc */
  double gamma;                 /* direction of perpendicular to arc at end */
  double mid[2];
  int side;
  double small = TOLERANCE_CONCAVE_CORNER;      /* angle for testing corners */
  double start[2];
  int status;                   /* status returned from CHP function call     */
  double theta;                 /* direction of tangent to last cut */
  double tolerance;
  double tool_radius;
  int turn;                     /* number of full or partial circles CCW */
  double end[3], current[3];
  int plane = settings->plane;

/* find basic arc data: center_x, center_y, and turn */

  if(settings->plane == CANON_PLANE_XZ) {
    start[0] = settings->program_x;
    start[1] = settings->program_z;
    end[0] = end_x;
    end[1] = end_z;
    end[2] = end_y;
    current[0] = settings->current_x;
    current[1] = settings->current_z;
    current[2] = settings->current_y;
    if(move == G_2) move = G_3; else move = G_2;
  } else if (settings->plane == CANON_PLANE_XY) {
    start[0] = settings->program_x;
    start[1] = settings->program_y;
    end[0] = end_x;
    end[1] = end_y;
    end[2] = end_z;
    current[0] = settings->current_x;
    current[1] = settings->current_y;
    current[2] = settings->current_z;
  } else ERM(NCE_RADIUS_COMP_ONLY_IN_XY_OR_XZ);

  tolerance = (settings->length_units == CANON_UNITS_INCHES) ?
    TOLERANCE_INCH : TOLERANCE_MM;

  if (block->r_flag) {
      CHP(arc_data_r(move, plane, start[0], start[1], end[0], end[1],
                   block->r_number, &center[0], &center[1], &turn, tolerance));
  } else {
      CHP(arc_data_ijk(move, plane,
                     start[0], start[1], end[0], end[1],
                     block->i_number,
                     settings->plane == CANON_PLANE_XZ? block->k_number: block->j_number,
                     &center[0], &center[1], &turn, tolerance));
  }

/* compute other data */
  side = settings->cutter_comp_side;
  tool_radius = settings->cutter_comp_radius;   /* always is positive */
  arc_radius = hypot((center[0] - end[0]), (center[1] - end[1]));
  theta = atan2(current[1] - start[1], current[0] - start[0]);
  theta = (side == LEFT) ? (theta - M_PI_2l) : (theta + M_PI_2l);
  delta = atan2(center[1] - start[1], center[0] - start[0]);
  alpha = (move == G_3) ? (delta - M_PI_2l) : (delta + M_PI_2l);
  beta = (side == LEFT) ? (theta - alpha) : (alpha - theta);
  beta = (beta > (1.5 * M_PIl)) ? (beta - (2 * M_PIl)) :
    (beta < -M_PI_2l) ? (beta + (2 * M_PIl)) : beta;

  if (((side == LEFT) && (move == G_3)) || ((side == RIGHT) && (move == G_2))) {
    gamma = atan2((center[1] - end[1]), (center[0] - end[0]));
    CHK((arc_radius <= tool_radius),
        NCE_TOOL_RADIUS_NOT_LESS_THAN_ARC_RADIUS_WITH_COMP);
  } else {
    gamma = atan2((end[1] - center[1]), (end[0] - center[0]));
    delta = (delta + M_PIl);
  }

  if(settings->plane == CANON_PLANE_XZ) {
    settings->program_x = end[0];
    settings->program_z = end[1];
    settings->program_y = end[2];
  } else if (settings->plane == CANON_PLANE_XY) {
    settings->program_x = end[0];
    settings->program_y = end[1];
    settings->program_z = end[2];
  }
  end[0] = (end[0] + (tool_radius * cos(gamma))); /* end_x reset actual */
  end[1] = (end[1] + (tool_radius * sin(gamma))); /* end_y reset actual */

/* check if extra arc needed and insert if so */

  CHK(((beta < -small) || (beta > (M_PIl + small))),
      NCE_CONCAVE_CORNER_WITH_CUTTER_RADIUS_COMP);
  if (beta > small) {           /* two arcs needed */
    mid[0] = (start[0] + (tool_radius * cos(delta)));
    mid[1] = (start[1] + (tool_radius * sin(delta)));
    if (settings->feed_mode == INVERSE_TIME)
      inverse_time_rate_arc2(start[0], start[1], (side == LEFT) ? -1 : 1,
                             mid[0], mid[1], center[0], center[1], turn,
                             end[0], end[1], end[2], block, settings);
    if(settings->plane == CANON_PLANE_XZ) {
      ARC_FEED(ztrans(settings, mid[1]), xtrans(settings, mid[0]), ztrans(settings, start[1]), xtrans(settings, start[0]),
               ((side == LEFT) ? 1 : -1),
               current[2], AA_end, BB_end, CC_end, u, v, w);
      ARC_FEED(ztrans(settings, end[1]), xtrans(settings, end[0]), ztrans(settings, center[1]), xtrans(settings, center[0]),
               -turn, end[2], AA_end, BB_end, CC_end, u, v, w);
    } else if (settings->plane == CANON_PLANE_XY) {
      ARC_FEED(mid[0], mid[1], start[0], start[1], ((side == LEFT) ? -1 : 1),
               current[2],
               AA_end, BB_end, CC_end, u, v, w);
      ARC_FEED(end[0], end[1], center[0], center[1], turn, end[2],
               AA_end, BB_end, CC_end, u, v, w);
    }
  } else {                      /* one arc needed */
    if (settings->feed_mode == INVERSE_TIME)
      inverse_time_rate_arc(current[0], current[1],
                            current[2], center[0], center[1], turn,
                            end[0], end[1], end[2], block, settings);
    if(settings->plane == CANON_PLANE_XZ) {
      ARC_FEED(ztrans(settings, end[1]), xtrans(settings, end[0]), ztrans(settings, center[1]), xtrans(settings, center[0]),
               -turn, end[2], AA_end, BB_end, CC_end, u, v, w);
    } else if (settings->plane == CANON_PLANE_XY) {
      ARC_FEED(end[0], end[1], center[0], center[1], turn, end[2],
               AA_end, BB_end, CC_end, u, v, w);
    }
  }

  if(settings->plane == CANON_PLANE_XZ) {
    settings->current_x = end[0];
    settings->current_z = end[1];
    settings->current_y = end[2];
  } else if (settings->plane == CANON_PLANE_XY) {
    settings->current_x = end[0];
    settings->current_y = end[1];
    settings->current_z = end[2];
  }
  settings->AA_current = AA_end;
  settings->BB_current = BB_end;
  settings->CC_current = CC_end;
  settings->u_current = u;
  settings->v_current = v;
  settings->w_current = w;

  return INTERP_OK;
}

/****************************************************************************/

/*! convert_axis_offsets

Returned Value: int
   If any of the following errors occur, this returns the error code shown.
   Otherwise, it returns INTERP_OK.
   1. The function is called when cutter radius compensation is on:
      NCE_CANNOT_CHANGE_AXIS_OFFSETS_WITH_CUTTER_RADIUS_COMP
   2. The g_code argument is not G_92, G_92_1, G_92_2, or G_92_3
      NCE_BUG_CODE_NOT_IN_G92_SERIES

Side effects:
   SET_PROGRAM_ORIGIN is called, and the coordinate
   values for the axis offsets are reset. The coordinates of the
   current point are reset. Parameters may be set.

Called by: convert_modal_0.

The action of G92 is described in [NCMS, pages 10 - 11] and {Fanuc,
pages 61 - 63]. [NCMS] is ambiguous about the intent, but [Fanuc]
is clear. When G92 is executed, an offset of the origin is calculated
so that the coordinates of the current point with respect to the moved
origin are as specified on the line containing the G92. If an axis
is not mentioned on the line, the coordinates of the current point
are not changed. The execution of G92 results in an axis offset being
calculated and saved for each of the six axes, and the axis offsets
are always used when motion is specified with respect to absolute
distance mode using any of the nine coordinate systems (those designated
by G54 - G59.3). Thus all nine coordinate systems are affected by G92.

Being in incremental distance mode has no effect on the action of G92
in this implementation. [NCMS] is not explicit about this, but it is
implicit in the second sentence of [Fanuc, page 61].

The offset is the amount the origin must be moved so that the
coordinate of the controlled point has the specified value. For
example, if the current point is at X=4 in the currently specified
coordinate system and the current X-axis offset is zero, then "G92 x7"
causes the X-axis offset to be reset to -3.

Since a non-zero offset may be already be in effect when the G92 is
called, that must be taken into account.

In addition to causing the axis offset values in the _setup model to be
set, G92 sets parameters 5211 to 5216 to the x,y,z,a,b,c axis offsets.

The action of G92.2 is described in [NCMS, page 12]. There is no
equivalent command in [Fanuc]. G92.2 resets axis offsets to zero.
G92.1, also included in [NCMS, page 12] (but the usage here differs
slightly from the spec), is like G92.2, except that it also causes
the axis offset parameters to be set to zero, whereas G92.2 does not
zero out the parameters.

G92.3 is not in [NCMS]. It sets the axis offset values to the values
given in the parameters.

*/

int Interp::convert_axis_offsets(int g_code,     //!< g_code being executed (must be in G_92 series)
                                block_pointer block,    //!< pointer to a block of RS274/NGC instructions  
                                setup_pointer settings) //!< pointer to machine settings                   
{
  static char name[] = "convert_axis_offsets";
  double *pars;                 /* short name for settings->parameters            */

  CHK((settings->cutter_comp_side != OFF),      /* not "== ON" */
      NCE_CANNOT_CHANGE_AXIS_OFFSETS_WITH_CUTTER_RADIUS_COMP);
  pars = settings->parameters;
  if (g_code == G_92) {
    if (block->x_flag == ON) {
      settings->axis_offset_x =
        (settings->current_x + settings->axis_offset_x - block->x_number);
      settings->current_x = block->x_number;
    }

    if (block->y_flag == ON) {
      settings->axis_offset_y =
        (settings->current_y + settings->axis_offset_y - block->y_number);
      settings->current_y = block->y_number;
    }

    if (block->z_flag == ON) {
      settings->axis_offset_z =
        (settings->current_z + settings->axis_offset_z - block->z_number);
      settings->current_z = block->z_number;
    }
    if (block->a_flag == ON) {
      settings->AA_axis_offset = (settings->AA_current +
                                  settings->AA_axis_offset - block->a_number);
      settings->AA_current = block->a_number;
    }
    if (block->b_flag == ON) {
      settings->BB_axis_offset = (settings->BB_current +
                                  settings->BB_axis_offset - block->b_number);
      settings->BB_current = block->b_number;
    }
    if (block->c_flag == ON) {
      settings->CC_axis_offset = (settings->CC_current +
                                  settings->CC_axis_offset - block->c_number);
      settings->CC_current = block->c_number;
    }
    if (block->u_flag == ON) {
      settings->u_axis_offset = (settings->u_current +
                                 settings->u_axis_offset - block->u_number);
      settings->u_current = block->u_number;
    }
    if (block->v_flag == ON) {
      settings->v_axis_offset = (settings->v_current +
                                 settings->v_axis_offset - block->v_number);
      settings->v_current = block->v_number;
    }
    if (block->w_flag == ON) {
      settings->w_axis_offset = (settings->w_current +
                                 settings->w_axis_offset - block->w_number);
      settings->w_current = block->w_number;
    }

    SET_ORIGIN_OFFSETS(settings->origin_offset_x + settings->axis_offset_x,
                       settings->origin_offset_y + settings->axis_offset_y,
                       settings->origin_offset_z + settings->axis_offset_z,
                       (settings->AA_origin_offset +
                        settings->AA_axis_offset),
                       (settings->BB_origin_offset +
                        settings->BB_axis_offset),
                       (settings->CC_origin_offset +
                        settings->CC_axis_offset),
                       (settings->u_origin_offset +
                        settings->u_axis_offset),
                       (settings->v_origin_offset +
                        settings->v_axis_offset),
                       (settings->w_origin_offset +
                        settings->w_axis_offset));
    
    pars[5211] = PROGRAM_TO_USER_LEN(settings->axis_offset_x);
    pars[5212] = PROGRAM_TO_USER_LEN(settings->axis_offset_y);
    pars[5213] = PROGRAM_TO_USER_LEN(settings->axis_offset_z);
    pars[5214] = PROGRAM_TO_USER_ANG(settings->AA_axis_offset);
    pars[5215] = PROGRAM_TO_USER_ANG(settings->BB_axis_offset);
    pars[5216] = PROGRAM_TO_USER_ANG(settings->CC_axis_offset);
    pars[5217] = PROGRAM_TO_USER_LEN(settings->u_axis_offset);
    pars[5218] = PROGRAM_TO_USER_LEN(settings->v_axis_offset);
    pars[5219] = PROGRAM_TO_USER_LEN(settings->w_axis_offset);
  } else if ((g_code == G_92_1) || (g_code == G_92_2)) {
    settings->current_x = settings->current_x + settings->axis_offset_x;
    settings->current_y = settings->current_y + settings->axis_offset_y;
    settings->current_z = settings->current_z + settings->axis_offset_z;
    settings->AA_current = (settings->AA_current + settings->AA_axis_offset);
    settings->BB_current = (settings->BB_current + settings->BB_axis_offset);
    settings->CC_current = (settings->CC_current + settings->CC_axis_offset);
    settings->u_current = (settings->u_current + settings->u_axis_offset);
    settings->v_current = (settings->v_current + settings->v_axis_offset);
    settings->w_current = (settings->w_current + settings->w_axis_offset);
    SET_ORIGIN_OFFSETS(settings->origin_offset_x,
                       settings->origin_offset_y, settings->origin_offset_z,
                       settings->AA_origin_offset,
                       settings->BB_origin_offset,
                       settings->CC_origin_offset,
                       settings->u_origin_offset,
                       settings->v_origin_offset,
                       settings->w_origin_offset);
    settings->axis_offset_x = 0.0;
    settings->axis_offset_y = 0.0;
    settings->axis_offset_z = 0.0;
    settings->AA_axis_offset = 0.0;
    settings->BB_axis_offset = 0.0;
    settings->CC_axis_offset = 0.0;
    settings->u_axis_offset = 0.0;
    settings->v_axis_offset = 0.0;
    settings->w_axis_offset = 0.0;
    if (g_code == G_92_1) {
      pars[5211] = 0.0;
      pars[5212] = 0.0;
      pars[5213] = 0.0;
      pars[5214] = 0.0;
      pars[5215] = 0.0;
      pars[5216] = 0.0;
      pars[5217] = 0.0;
      pars[5218] = 0.0;
      pars[5219] = 0.0;
    }
  } else if (g_code == G_92_3) {
    settings->current_x =
      settings->current_x + settings->axis_offset_x - USER_TO_PROGRAM_LEN(pars[5211]);
    settings->current_y =
      settings->current_y + settings->axis_offset_y - USER_TO_PROGRAM_LEN(pars[5212]);
    settings->current_z =
      settings->current_z + settings->axis_offset_z - USER_TO_PROGRAM_LEN(pars[5213]);
    settings->AA_current =
      settings->AA_current + settings->AA_axis_offset - USER_TO_PROGRAM_ANG(pars[5214]);
    settings->BB_current =
      settings->BB_current + settings->BB_axis_offset - USER_TO_PROGRAM_ANG(pars[5215]);
    settings->CC_current =
      settings->CC_current + settings->CC_axis_offset - USER_TO_PROGRAM_ANG(pars[5216]);
    settings->u_current =
      settings->u_current + settings->u_axis_offset - USER_TO_PROGRAM_LEN(pars[5217]);
    settings->v_current =
      settings->v_current + settings->v_axis_offset - USER_TO_PROGRAM_LEN(pars[5218]);
    settings->w_current =
      settings->w_current + settings->w_axis_offset - USER_TO_PROGRAM_LEN(pars[5219]);

    settings->axis_offset_x = USER_TO_PROGRAM_LEN(pars[5211]);
    settings->axis_offset_y = USER_TO_PROGRAM_LEN(pars[5212]);
    settings->axis_offset_z = USER_TO_PROGRAM_LEN(pars[5213]);
    settings->AA_axis_offset = USER_TO_PROGRAM_ANG(pars[5214]);
    settings->BB_axis_offset = USER_TO_PROGRAM_ANG(pars[5215]);
    settings->CC_axis_offset = USER_TO_PROGRAM_ANG(pars[5216]);
    settings->u_axis_offset = USER_TO_PROGRAM_LEN(pars[5217]);
    settings->v_axis_offset = USER_TO_PROGRAM_LEN(pars[5218]);
    settings->w_axis_offset = USER_TO_PROGRAM_LEN(pars[5219]);
    SET_ORIGIN_OFFSETS(settings->origin_offset_x + settings->axis_offset_x,
                       settings->origin_offset_y + settings->axis_offset_y,
                       settings->origin_offset_z + settings->axis_offset_z,
                       (settings->AA_origin_offset +
                        settings->AA_axis_offset),
                       (settings->BB_origin_offset +
                        settings->BB_axis_offset),
                       (settings->CC_origin_offset +
                        settings->CC_axis_offset),
                       (settings->u_origin_offset +
                        settings->u_axis_offset),
                       (settings->v_origin_offset +
                        settings->v_axis_offset),
                       (settings->w_origin_offset +
                        settings->w_axis_offset));

  } else
    ERM(NCE_BUG_CODE_NOT_IN_G92_SERIES);

  return INTERP_OK;
}


int Interp::convert_param_comment(char *comment, char *expanded, int len)
{
    static char name[] = "convert_param_comment";
    int i;
    char param[LINELEN+1];
    int paramNumber;
    int stat;
    double value;
    char valbuf[30]; // max double length + room
    char *v;
    int found;

    while(*comment)
    {
        if(*comment == '#')
        {
            found = 0;
            logDebug("a parameter");

            // skip over the '#'
            comment++;
            CHK((0 == *comment), NCE_NAMED_PARAMETER_NOT_TERMINATED);

            if(isdigit(*comment))  // is this numeric param?
            {
                logDebug("numeric parameter");
                for(i=0; isdigit(*comment)&& (i<LINELEN); i++)
                {
                    param[i] = *comment++;
                }
                param[i] = 0;
                paramNumber = atoi(param);
                if((paramNumber >= 0) &&
                   (paramNumber < RS274NGC_MAX_PARAMETERS))
                {
                    value = _setup.parameters[paramNumber];
                    found = 1;
                }
            }
            else if(*comment == '<')
            {
                logDebug("name parameter");
                // this is a name parameter
                // skip over the '<'
                comment++;
                CHK((0 == *comment), NCE_NAMED_PARAMETER_NOT_TERMINATED);

                for(i=0; (')' != *comment) &&
                        (i<LINELEN) && (0 != *comment);)
                {
                    if('>' == *comment)
                    {
                        break;     // done
                    }
                    if(isspace(*comment)) // skip space inside the param
                    {
                        comment++;
                        continue;
                    }
                    else
                    {
                        param[i] = *comment++;
                        i++;
                    }
                }
                if('>' != *comment)
                {
                    ERM(NCE_NAMED_PARAMETER_NOT_TERMINATED);
                    logDebug("parameter not terminated");
                }
                else
                {
                    comment++;
                }

                // terminate the name
                param[i] = 0;

                // now lookup the name
                find_named_param(param, &stat, &value);
                if(stat)
                {
                    found = 1;
                }
            }
            else
            {
                // neither numeric or name
                logDebug("neither numeric nor name");
                // just store the '#'
                *expanded++ = '#';

                CHK((*comment == 0), NCE_NAMED_PARAMETER_NOT_TERMINATED);
                continue;
            }

            // we have a parameter -- now insert it
            // we have the value
            if(found)
            {
                sprintf(valbuf, "%lf", value);
            }
            else
            {
                strcpy(valbuf, "######");
            }
            logDebug("found:%d value:|%s|", found, valbuf);

            v = valbuf;
            while(*v)
            {
                *expanded++ = *v++;
            }
        }
        else  // not a '#'
        {
            *expanded++ = *comment++;
        }
    }
    *expanded = 0; // the final nul
    
    return INTERP_OK;
}

/****************************************************************************/

/*! convert_comment

Returned Value: int (INTERP_OK)

Side effects:
   The message function is called if the string starts with "MSG,".
   Otherwise, the comment function is called.

Called by: execute_block

To be a message, the first four characters of the comment after the
opening left parenthesis must be "MSG,", ignoring the case of the
letters and allowing spaces or tabs anywhere before the comma (to make
the treatment of case and white space consistent with how it is
handled elsewhere).

Messages are not provided for in [NCMS]. They are implemented here as a
subtype of comment. This is an extension to the rs274NGC language.

*/

int Interp::convert_comment(char *comment)       //!< string with comment
{
  enum
  { LC_SIZE = 256 };            // 256 from comment[256] in rs274ngc.hh
  char lc[LC_SIZE];
  char expanded[2*LC_SIZE+1];
  char MSG_STR[] = "msg,";
  char SYSTEM_STR[] = "system,";

  //!!!KL add two -- debug => same as msg
  //!!!KL         -- print => goes to stderr
  char DEBUG_STR[] = "debug,";
  char PRINT_STR[] = "print,";
  int m, n, start;

  // step over leading white space in comment
  m = 0;
  while (isspace(comment[m]))
    m++;
  start = m;
  // copy lowercase comment to lc[]
  for (n = 0; n < LC_SIZE && comment[m] != 0; m++, n++) {
    lc[n] = tolower(comment[m]);
  }
  lc[n] = 0;                    // null terminate

  // compare with MSG, SYSTEM, DEBUG, PRINT
  if (!strncmp(lc, MSG_STR, strlen(MSG_STR))) {
    MESSAGE(comment + start + strlen(MSG_STR));
    return INTERP_OK;
  }
  else if (!strncmp(lc, DEBUG_STR, strlen(DEBUG_STR)))
  {
      convert_param_comment(comment+start+strlen(DEBUG_STR), expanded,
                            2*LC_SIZE);
      MESSAGE(expanded);
      return INTERP_OK;
  }
  else if (!strncmp(lc, PRINT_STR, strlen(PRINT_STR)))
  {
      convert_param_comment(comment+start+strlen(PRINT_STR), expanded,
                            2*LC_SIZE);
      fprintf(stdout, "%s\n", expanded);
      return INTERP_OK;
  }
  else if (!strncmp(lc, SYSTEM_STR, strlen(SYSTEM_STR))) {
/*! \todo Implement SYSTEM commands in the task controller */
/*! \todo Another #if 0 */
#if 0
/*! \todo FIX-ME Impliment these at a later stage... */
    SYSTEM(comment + start + strlen(SYSTEM_STR));
    return INTERP_EXECUTE_FINISH;     // inhibit read-ahead until this is done
#endif
  }
  // else it's a real comment
  COMMENT(comment + start);
  return INTERP_OK;
}

/****************************************************************************/

/*! convert_control_mode

Returned Value: int
   If any of the following errors occur, this returns the error code shown.
   Otherwise, it returns INTERP_OK.
   1. g_code isn't G_61, G_61_1, G_64 : NCE_BUG_CODE_NOT_G61_G61_1_OR_G64

Side effects: See below

Called by: convert_g.

The interpreter switches the machine settings to indicate the
control mode (CANON_EXACT_STOP, CANON_EXACT_PATH or CANON_CONTINUOUS)

A call is made to SET_MOTION_CONTROL_MODE(CANON_XXX), where CANON_XXX is
CANON_EXACT_PATH if g_code is G_61, CANON_EXACT_STOP if g_code is G_61_1,
and CANON_CONTINUOUS if g_code is G_64.

Setting the control mode to CANON_EXACT_STOP on G_61 would correspond
more closely to the meaning of G_61 as given in [NCMS, page 40], but
CANON_EXACT_PATH has the advantage that the tool does not stop if it
does not have to, and no evident disadvantage compared to
CANON_EXACT_STOP, so it is being used for G_61. G_61_1 is not defined
in [NCMS], so it is available and is used here for setting the control
mode to CANON_EXACT_STOP.

It is OK to call SET_MOTION_CONTROL_MODE(CANON_XXX) when CANON_XXX is
already in force.

*/

int Interp::convert_control_mode(int g_code,     //!< g_code being executed (G_61, G61_1, || G_64)
				double tolerance,    //tolerance for the path following in G64
                                setup_pointer settings) //!< pointer to machine settings                 
{
  static char name[] = "convert_control_mode";
  if (g_code == G_61) {
    SET_MOTION_CONTROL_MODE(CANON_EXACT_PATH, 0);
    settings->control_mode = CANON_EXACT_PATH;
  } else if (g_code == G_61_1) {
    SET_MOTION_CONTROL_MODE(CANON_EXACT_STOP, 0);
    settings->control_mode = CANON_EXACT_STOP;
  } else if (g_code == G_64) {
	if (tolerance >= 0) {
	    SET_MOTION_CONTROL_MODE(CANON_CONTINUOUS, tolerance);
	} else {
	    SET_MOTION_CONTROL_MODE(CANON_CONTINUOUS, 0);
	}
    settings->control_mode = CANON_CONTINUOUS;
  } else 
    ERM(NCE_BUG_CODE_NOT_G61_G61_1_OR_G64);
  return INTERP_OK;
}

/****************************************************************************/

/*! convert_coordinate_system

Returned Value: int
   If any of the following errors occur, this returns the error code shown.
   Otherwise, it returns INTERP_OK.
   1. The value of the g_code argument is not 540, 550, 560, 570, 580, 590
      591, 592, or 593:
      NCE_BUG_CODE_NOT_IN_RANGE_G54_TO_G593

Side effects:
   If the coordinate system selected by the g_code is not already in
   use, the canonical program coordinate system axis offset values are
   reset and the coordinate values of the current point are reset.

Called by: convert_g.

COORDINATE SYSTEMS (involves g10, g53, g54 - g59.3, g92)

The canonical machining functions view of coordinate systems is:
1. There are two coordinate systems: absolute and program.
2. All coordinate values are given in terms of the program coordinate system.
3. The offsets of the program coordinate system may be reset.

The RS274/NGC view of coordinate systems, as given in section 3.2
of [NCMS] is:
1. there are ten coordinate systems: absolute and 9 program. The
   program coordinate systems are numbered 1 to 9.
2. you can switch among the 9 but not to the absolute one. G54
   selects coordinate system 1, G55 selects 2, and so on through
   G56, G57, G58, G59, G59.1, G59.2, and G59.3.
3. you can set the offsets of the 9 program coordinate systems
   using G10 L2 Pn (n is the number of the coordinate system) with
   values for the axes in terms of the absolute coordinate system.
4. the first one of the 9 program coordinate systems is the default.
5. data for coordinate systems is stored in parameters [NCMS, pages 59 - 60].
6. g53 means to interpret coordinate values in terms of the absolute
   coordinate system for the one block in which g53 appears.
7. You can offset the current coordinate system using g92. This offset
   will then apply to all nine program coordinate systems.

The approach used in the interpreter mates the canonical and NGC views
of coordinate systems as follows:

During initialization, data from the parameters for the first NGC
coordinate system is used in a SET_ORIGIN_OFFSETS function call and
origin_index in the machine model is set to 1.

If a g_code in the range g54 - g59.3 is encountered in an NC program,
the data from the appropriate NGC coordinate system is copied into the
origin offsets used by the interpreter, a SET_ORIGIN_OFFSETS function
call is made, and the current position is reset.

If a g10 is encountered, the convert_setup function is called to reset
the offsets of the program coordinate system indicated by the P number
given in the same block.

If a g53 is encountered, the axis values given in that block are used
to calculate what the coordinates are of that point in the current
coordinate system, and a STRAIGHT_TRAVERSE or STRAIGHT_FEED function
call to that point using the calculated values is made. No offset
values are changed.

If a g92 is encountered, that is handled by the convert_axis_offsets
function. A g92 results in an axis offset for each axis being calculated
and stored in the machine model. The axis offsets are applied to all
nine coordinate systems. Axis offsets are initialized to zero.

*/

int Interp::convert_coordinate_system(int g_code,        //!< g_code called (must be one listed above)     
                                     setup_pointer settings)    //!< pointer to machine settings                  
{
  static char name[] = "convert_coordinate_system";
  int origin;
  double x;
  double y;
  double z;
  double a;
  double b;
  double c;
  double u, v, w;
  double *parameters;

  parameters = settings->parameters;
  switch (g_code) {
  case 540:
    origin = 1;
    break;
  case 550:
    origin = 2;
    break;
  case 560:
    origin = 3;
    break;
  case 570:
    origin = 4;
    break;
  case 580:
    origin = 5;
    break;
  case 590:
    origin = 6;
    break;
  case 591:
    origin = 7;
    break;
  case 592:
    origin = 8;
    break;
  case 593:
    origin = 9;
    break;
  default:
    ERM(NCE_BUG_CODE_NOT_IN_RANGE_G54_TO_G593);
  }

  if (origin == settings->origin_index) {       /* already using this origin */
#ifdef DEBUG_EMC
    COMMENT("interpreter: continuing to use same coordinate system");
#endif
    return INTERP_OK;
  }

  settings->origin_index = origin;
  parameters[5220] = (double) origin;

/* axis offsets could be included in the two set of calculations for
   current_x, current_y, etc., but do not need to be because the results
   would be the same. They would be added in then subtracted out. */
  settings->current_x = (settings->current_x + settings->origin_offset_x);
  settings->current_y = (settings->current_y + settings->origin_offset_y);
  settings->current_z = (settings->current_z + settings->origin_offset_z);
  settings->AA_current = (settings->AA_current + settings->AA_origin_offset);
  settings->BB_current = (settings->BB_current + settings->BB_origin_offset);
  settings->CC_current = (settings->CC_current + settings->CC_origin_offset);
  settings->u_current = (settings->u_current + settings->u_origin_offset);
  settings->v_current = (settings->v_current + settings->v_origin_offset);
  settings->w_current = (settings->w_current + settings->w_origin_offset);

  x = USER_TO_PROGRAM_LEN(parameters[5201 + (origin * 20)]);
  y = USER_TO_PROGRAM_LEN(parameters[5202 + (origin * 20)]);
  z = USER_TO_PROGRAM_LEN(parameters[5203 + (origin * 20)]);
  a = USER_TO_PROGRAM_ANG(parameters[5204 + (origin * 20)]);
  b = USER_TO_PROGRAM_ANG(parameters[5205 + (origin * 20)]);
  c = USER_TO_PROGRAM_ANG(parameters[5206 + (origin * 20)]);
  u = USER_TO_PROGRAM_LEN(parameters[5207 + (origin * 20)]);
  v = USER_TO_PROGRAM_LEN(parameters[5208 + (origin * 20)]);
  w = USER_TO_PROGRAM_LEN(parameters[5209 + (origin * 20)]);

  settings->origin_offset_x = x;
  settings->origin_offset_y = y;
  settings->origin_offset_z = z;
  settings->AA_origin_offset = a;
  settings->BB_origin_offset = b;
  settings->CC_origin_offset = c;
  settings->u_origin_offset = u;
  settings->v_origin_offset = v;
  settings->w_origin_offset = w;

  settings->current_x = (settings->current_x - x);
  settings->current_y = (settings->current_y - y);
  settings->current_z = (settings->current_z - z);
  settings->AA_current = (settings->AA_current - a);
  settings->BB_current = (settings->BB_current - b);
  settings->CC_current = (settings->CC_current - c);
  settings->u_current = (settings->u_current - u);
  settings->v_current = (settings->v_current - v);
  settings->w_current = (settings->w_current - w);

  SET_ORIGIN_OFFSETS(x + settings->axis_offset_x,
                     y + settings->axis_offset_y,
                     z + settings->axis_offset_z,
                     a + settings->AA_axis_offset,
                     b + settings->BB_axis_offset,
                     c + settings->CC_axis_offset,
                     u + settings->u_axis_offset,
                     v + settings->v_axis_offset,
                     w + settings->w_axis_offset);
  return INTERP_OK;
}

/****************************************************************************/

/*! convert_cutter_compensation

Returned Value: int
   If convert_cutter_compensation_on or convert_cutter_compensation_off
      is called and returns an error code, this returns that code.
   If any of the following errors occur, this returns the error shown.
   Otherwise, it returns INTERP_OK.
   1. g_code is not G_40, G_41, or G_42:
      NCE_BUG_CODE_NOT_G40_G41_OR_G42

Side effects:
   The value of cutter_comp_side in the machine model mode is
   set to RIGHT, LEFT, or OFF. The currently active tool table index in
   the machine model (which is the index of the slot whose diameter
   value is used in cutter radius compensation) is updated.

Since cutter radius compensation is performed in the interpreter, no
call is made to any canonical function regarding cutter radius compensation.

Called by: convert_g

*/

int Interp::convert_cutter_compensation(int g_code,      //!< must be G_40, G_41, or G_42             
                                       block_pointer block,     //!< pointer to a block of RS274 instructions
                                       setup_pointer settings)  //!< pointer to machine settings             
{
  static char name[] = "convert_cutter_compensation";
  int status;

  if (g_code == G_40) {
    CHP(convert_cutter_compensation_off(settings));
  } else if (g_code == G_41) {
    CHP(convert_cutter_compensation_on(LEFT, block, settings));
  } else if (g_code == G_42) {
    CHP(convert_cutter_compensation_on(RIGHT, block, settings));
  } else if (g_code == G_41_1) {
    CHP(convert_cutter_compensation_on(LEFT, block, settings));
  } else if (g_code == G_42_1) {
    CHP(convert_cutter_compensation_on(RIGHT, block, settings));
  } else
    ERS("BUG: Code not G40, G41, G41.1, G42, G42.2");

  return INTERP_OK;
}

/****************************************************************************/

/*! convert_cutter_compensation_off

Returned Value: int (INTERP_OK)

Side effects:
   A comment is made that cutter radius compensation is turned off.
   The machine model of the cutter radius compensation mode is set to OFF.
   The value of cutter_comp_firstmove in the machine model is set to ON.
     This serves as a flag when cutter radius compensation is
     turned on again.

Called by: convert_cutter_compensation

*/

int Interp::convert_cutter_compensation_off(setup_pointer settings)      //!< pointer to machine settings
{
#ifdef DEBUG_EMC
  COMMENT("interpreter: cutter radius compensation off");
#endif
  if(settings->cutter_comp_side != OFF && settings->cutter_comp_radius > 0.0) {
      settings->current_x = settings->program_x;
      settings->current_y = settings->program_y;
      settings->current_z = settings->program_z;
  }
  settings->cutter_comp_side = OFF;
  settings->cutter_comp_firstmove = ON;
  return INTERP_OK;
}

/****************************************************************************/

/*! convert_cutter_compensation_on

Returned Value: int
   If any of the following errors occur, this returns the error code shown.
   Otherwise, it returns INTERP_OK.
   1. The selected plane is not the XY plane:
      NCE_CANNOT_TURN_CUTTER_RADIUS_COMP_ON_OUT_OF_XY_PLANE
   2. Cutter radius compensation is already on:
      NCE_CANNOT_TURN_CUTTER_RADIUS_COMP_ON_WHEN_ON

Side effects:
   A COMMENT function call is made (conditionally) saying that the
   interpreter is switching mode so that cutter radius compensation is on.
   The value of cutter_comp_radius in the machine model mode is
   set to the absolute value of the radius given in the tool table.
   The value of cutter_comp_side in the machine model mode is
   set to RIGHT or LEFT. The currently active tool table index in
   the machine model is updated.

Called by: convert_cutter_compensation

check_other_codes checks that a d word occurs only in a block with g41
or g42.

Cutter radius compensation is carried out in the interpreter, so no
call is made to a canonical function (although there is a canonical
function, START_CUTTER_RADIUS_COMPENSATION, that could be called if
the primitive level could execute it).

This version uses a D word if there is one in the block, but it does
not require a D word, since the sample programs which the interpreter
is supposed to handle do not have them.  Logically, the D word is
optional, since the D word is always (except in cases we have never
heard of) the slot number of the tool in the spindle. Not requiring a
D word is contrary to [Fanuc, page 116] and [NCMS, page 79], however.
Both manuals require the use of the D-word with G41 and G42.

This version handles a negative offset radius, which may be
encountered if the programmed tool path is a center line path for
cutting a profile and the path was constructed using a nominal tool
diameter. Then the value in the tool table for the diameter is set to
be the difference between the actual diameter and the nominal
diameter. If the actual diameter is less than the nominal, the value
in the table is negative. The method of handling a negative radius is
to switch the side of the offset and use a positive radius. This
requires that the profile use arcs (not straight lines) to go around
convex corners.

*/

/* Set *result to the integer nearest to value; return TRUE if value is
 * within .0001 of an integer
 */
static int is_near_int(int *result, double value) {
    *result = (int)(value + .5);
    return fabs(*result - value) < .0001;
}

int Interp::convert_cutter_compensation_on(int side,     //!< side of path cutter is on (LEFT or RIGHT)
                                          block_pointer block,  //!< pointer to a block of RS274 instructions 
                                          setup_pointer settings)       //!< pointer to machine settings              
{
  static char name[] = "convert_cutter_compensation_on";
  double radius;
  int index, orientation;

  CHK((settings->plane != CANON_PLANE_XY && settings->plane != CANON_PLANE_XZ),
      NCE_RADIUS_COMP_ONLY_IN_XY_OR_XZ);
  CHK((settings->cutter_comp_side != OFF),
      NCE_CANNOT_TURN_CUTTER_RADIUS_COMP_ON_WHEN_ON);
  if(block->g_modes[7] == G_41_1 || block->g_modes[7] == G_42_1) {
      CHKF((block->d_flag != ON),
              (_("G%d.1 with no D word"), block->g_modes[7]/10 ));
      radius = block->d_number_float / 2;
      if(block->l_number != -1) {
          orientation = block->l_number;
      } else {
          orientation = 0;
      }
  } else {
      if(block->d_flag == OFF) {
          index = settings->current_slot;
      } else {
          int tool;
          CHKF(!is_near_int(&tool, block->d_number_float),
                  (_("G%d requires D word to be a whole number"),
                   block->g_modes[7]/10));
          CHK((tool < 0), NCE_NEGATIVE_D_WORD_TOOL_RADIUS_INDEX_USED);
          CHK((tool > _setup.tool_max), NCE_TOOL_RADIUS_INDEX_TOO_BIG);
          index = tool;
      }
      radius = USER_TO_PROGRAM_LEN(settings->tool_table[index].diameter) / 2.0;
      orientation = settings->tool_table[index].orientation;
  }
  if (radius < 0.0) { /* switch side & make radius positive if radius negative */
    radius = -radius;
    if (side == RIGHT)
      side = LEFT;
    else
      side = RIGHT;
  }
#ifdef DEBUG_EMC
  if (side == RIGHT)
    COMMENT("interpreter: cutter radius compensation on right");
  else
    COMMENT("interpreter: cutter radius compensation on left");
#endif

  settings->cutter_comp_radius = radius;
  settings->cutter_comp_orientation = orientation;
  settings->cutter_comp_side = side;
  return INTERP_OK;
}

/****************************************************************************/

/*! convert_distance_mode

Returned Value: int
   If any of the following errors occur, this returns the error shown.
   Otherwise, it returns INTERP_OK.
   1. g_code isn't G_90 or G_91: NCE_BUG_CODE_NOT_G90_OR_G91

Side effects:
   The interpreter switches the machine settings to indicate the current
   distance mode (absolute or incremental).

   The canonical machine to which commands are being sent does not have
   an incremental mode, so no command setting the distance mode is
   generated in this function. A comment function call explaining the
   change of mode is made (conditionally), however, if there is a change.

Called by: convert_g.

*/

int Interp::convert_distance_mode(int g_code,    //!< g_code being executed (must be G_90 or G_91)
                                 setup_pointer settings)        //!< pointer to machine settings                 
{
  static char name[] = "convert_distance_mode";
  if (g_code == G_90) {
    if (settings->distance_mode != MODE_ABSOLUTE) {
#ifdef DEBUG_EMC
      COMMENT("interpreter: distance mode changed to absolute");
#endif
      settings->distance_mode = MODE_ABSOLUTE;
    }
  } else if (g_code == G_91) {
    if (settings->distance_mode != MODE_INCREMENTAL) {
#ifdef DEBUG_EMC
      COMMENT("interpreter: distance mode changed to incremental");
#endif
      settings->distance_mode = MODE_INCREMENTAL;
    }
  } else
    ERM(NCE_BUG_CODE_NOT_G90_OR_G91);
  return INTERP_OK;
}

/****************************************************************************/

/*! convert_dwell

Returned Value: int (INTERP_OK)

Side effects:
   A dwell command is executed.

Called by: convert_g.

*/

int Interp::convert_dwell(double time)   //!< time in seconds to dwell  */
{
  DWELL(time);
  return INTERP_OK;
}

/****************************************************************************/

/*! convert_feed_mode

Returned Value: int
   If any of the following errors occur, this returns an error code.
   Otherwise, it returns INTERP_OK.
   1.  g_code isn't G_93, G_94 or G_95

Side effects:
   The interpreter switches the machine settings to indicate the current
   feed mode (UNITS_PER_MINUTE or INVERSE_TIME).

   The canonical machine to which commands are being sent does not have
   a feed mode, so no command setting the distance mode is generated in
   this function. A comment function call is made (conditionally)
   explaining the change in mode, however.

Called by: execute_block.

*/

int Interp::convert_feed_mode(int g_code,        //!< g_code being executed (must be G_93, G_94 or G_95)
                             setup_pointer settings)    //!< pointer to machine settings                 
{
  static char name[] = "convert_feed_mode";
  if (g_code == G_93) {
#ifdef DEBUG_EMC
    COMMENT("interpreter: feed mode set to inverse time");
#endif
    settings->feed_mode = INVERSE_TIME;
    SET_FEED_MODE(0);
  } else if (g_code == G_94) {
#ifdef DEBUG_EMC
    COMMENT("interpreter: feed mode set to units per minute");
#endif
    settings->feed_mode = UNITS_PER_MINUTE;
    SET_FEED_MODE(0);
    SET_FEED_RATE(0);
  } else if(g_code == G_95) {
#ifdef DEBUG_EMC
    COMMENT("interpreter: feed mode set to units per revolution");
#endif
    settings->feed_mode = UNITS_PER_REVOLUTION;
    SET_FEED_MODE(1);
    SET_FEED_RATE(0);
  } else
    ERS("BUG: Code not G93, G94, or G95");
  return INTERP_OK;
}

/****************************************************************************/

/*! convert_feed_rate

Returned Value: int (INTERP_OK)

Side effects:
   The machine feed_rate is set to the value of f_number in the
     block by function call.
   The machine model feed_rate is set to that value.

Called by: execute_block

This is called only if the feed mode is UNITS_PER_MINUTE or UNITS_PER_REVOLUTION.

*/

int Interp::convert_feed_rate(block_pointer block,       //!< pointer to a block of RS274 instructions
                             setup_pointer settings)    //!< pointer to machine settings             
{
  SET_FEED_RATE(block->f_number);
  settings->feed_rate = block->f_number;
  return INTERP_OK;
}

/****************************************************************************/

/*! convert_g

Returned Value: int
   If one of the following functions is called and returns an error code,
   this returns that code.
      convert_control_mode
      convert_coordinate_system
      convert_cutter_compensation
      convert_distance_mode
      convert_dwell
      convert_length_units
      convert_modal_0
      convert_motion
      convert_retract_mode
      convert_set_plane
      convert_tool_length_offset
   Otherwise, it returns INTERP_OK.

Side effects:
   Any g_codes in the block (excluding g93 and 94) and any implicit
   motion g_code are executed.

Called by: execute_block.

This takes a pointer to a block of RS274/NGC instructions (already
read in) and creates the appropriate output commands corresponding to
any "g" codes in the block.

Codes g93 and g94, which set the feed mode, are executed earlier by
execute_block before reading the feed rate.

G codes are are executed in the following order.
1.  mode 0, G4 only - dwell. Left here from earlier versions.
2.  mode 2, one of (G17, G18, G19) - plane selection.
3.  mode 6, one of (G20, G21) - length units.
4.  mode 7, one of (G40, G41, G42) - cutter radius compensation.
5.  mode 8, one of (G43, G49) - tool length offset
6.  mode 12, one of (G54, G55, G56, G57, G58, G59, G59.1, G59.2, G59.3)
    - coordinate system selection.
7.  mode 13, one of (G61, G61.1, G64, G50, G51) - control mode
8.  mode 3, one of (G90, G91) - distance mode.
9.  mode 10, one of (G98, G99) - retract mode.
10. mode 0, one of (G10, G28, G30, G92, G92.1, G92.2, G92.3) -
    setting coordinate system locations, return to reference point 1,
    return to reference point 2, setting or cancelling axis offsets.
11. mode 1, one of (G0, G1, G2, G3, G38.2, G80, G81 to G89, G33, G33.1, G76) - motion or cancel.
    G53 from mode 0 is also handled here, if present.

Some mode 0 and most mode 1 G codes must be executed after the length units
are set, since they use coordinate values. Mode 1 codes also must wait
until most of the other modes are set.

*/

int Interp::convert_g(block_pointer block,       //!< pointer to a block of RS274/NGC instructions
                     setup_pointer settings)    //!< pointer to machine settings                 
{
  static char name[] = "convert_g";
  int status;

  if (block->g_modes[0] == G_4) {
    CHP(convert_dwell(block->p_number));
  }
  if (block->g_modes[2] != -1) {
    CHP(convert_set_plane(block->g_modes[2], settings));
  }
  if (block->g_modes[6] != -1) {
    CHP(convert_length_units(block->g_modes[6], settings));
  }
  if (block->g_modes[7] != -1) {
    CHP(convert_cutter_compensation(block->g_modes[7], block, settings));
  }
  if (block->g_modes[8] != -1) {
    CHP(convert_tool_length_offset(block->g_modes[8], block, settings));
  }
  if (block->g_modes[12] != -1) {
    CHP(convert_coordinate_system(block->g_modes[12], settings));
  }
  if (block->g_modes[13] != -1) {
    CHP(convert_control_mode(block->g_modes[13], block->p_number, settings));
  }
  if (block->g_modes[3] != -1) {
    CHP(convert_distance_mode(block->g_modes[3], settings));
  }
  if (block->g_modes[10] != -1) {
    CHP(convert_retract_mode(block->g_modes[10], settings));
  }
  if (block->g_modes[0] != -1) {
    CHP(convert_modal_0(block->g_modes[0], block, settings));
  }
  if (block->motion_to_be != -1) {
    CHP(convert_motion(block->motion_to_be, block, settings));
  }
  return INTERP_OK;
}

/****************************************************************************/

/*! convert_home

Returned Value: int
   If any of the following errors occur, this returns the error code shown.
   Otherwise, it returns INTERP_OK.
   1. cutter radius compensation is on:
      NCE_CANNOT_USE_G28_OR_G30_WITH_CUTTER_RADIUS_COMP
   2. The code is not G28 or G30: NCE_BUG_CODE_NOT_G28_OR_G30

Side effects: 
   This executes a straight traverse to the programmed point, using
   the current coordinate system, tool length offset, and motion mode
   to interpret the coordinate values. Then it executes a straight
   traverse to move one or more axes to the location of reference
   point 1 (if G28) or reference point 2 (if G30).  If any axis words
   are specified in this block, only those axes are moved to the
   reference point.  If none are specified, all axes are moved.  It
   also updates the setting of the position of the tool point to the
   end point of the move.

   N.B. Many gcode programmers call the reference point a home
   position, and that is exactly what it is if the parameters are
   zero.  Do not confuse this with homing the axis (searching for
   a switch or index pulse).

Called by: convert_modal_0.

*/

int Interp::convert_home(int move,       //!< G code, must be G_28 or G_30            
                        block_pointer block,    //!< pointer to a block of RS274 instructions
                        setup_pointer settings) //!< pointer to machine settings             
{
  static char name[] = "convert_home";
  double end_x;
  double end_y;
  double end_z;
  double AA_end;
  double BB_end;
  double CC_end;
  double u_end;
  double v_end;
  double w_end;
  double end_x_home;
  double end_y_home;
  double end_z_home;
  double AA_end_home;
  double BB_end_home;
  double CC_end_home;
  double u_end_home;
  double v_end_home;
  double w_end_home;
  double *parameters;

  parameters = settings->parameters;
  find_ends(block, settings, &end_x, &end_y, &end_z,
            &AA_end, &BB_end, &CC_end, 
            &u_end, &v_end, &w_end);

  CHK((settings->cutter_comp_side != OFF),
      NCE_CANNOT_USE_G28_OR_G30_WITH_CUTTER_RADIUS_COMP);

  // waypoint is in currently active coordinate system

  STRAIGHT_TRAVERSE(end_x, end_y, end_z,
                    AA_end, BB_end, CC_end,
                    u_end, v_end, w_end);

  if (move == G_28) {
      find_relative(USER_TO_PROGRAM_LEN(parameters[5161]),
                    USER_TO_PROGRAM_LEN(parameters[5162]),
                    USER_TO_PROGRAM_LEN(parameters[5163]),
                    USER_TO_PROGRAM_ANG(parameters[5164]),
                    USER_TO_PROGRAM_ANG(parameters[5165]),
                    USER_TO_PROGRAM_ANG(parameters[5166]),
                    USER_TO_PROGRAM_LEN(parameters[5167]),
                    USER_TO_PROGRAM_LEN(parameters[5168]),
                    USER_TO_PROGRAM_LEN(parameters[5169]),
                    &end_x_home, &end_y_home, &end_z_home,
                    &AA_end_home, &BB_end_home, &CC_end_home, 
                    &u_end_home, &v_end_home, &w_end_home, settings);
  } else if (move == G_30) {
      find_relative(USER_TO_PROGRAM_LEN(parameters[5181]),
                    USER_TO_PROGRAM_LEN(parameters[5182]),
                    USER_TO_PROGRAM_LEN(parameters[5183]),
                    USER_TO_PROGRAM_ANG(parameters[5184]),
                    USER_TO_PROGRAM_ANG(parameters[5185]),
                    USER_TO_PROGRAM_ANG(parameters[5186]),
                    USER_TO_PROGRAM_LEN(parameters[5187]),
                    USER_TO_PROGRAM_LEN(parameters[5188]),
                    USER_TO_PROGRAM_LEN(parameters[5189]),
                    &end_x_home, &end_y_home, &end_z_home,
                    &AA_end_home, &BB_end_home, &CC_end_home, 
                    &u_end_home, &v_end_home, &w_end_home, settings);
  } else
    ERM(NCE_BUG_CODE_NOT_G28_OR_G30);
  
  // if any axes are specified, home only those axes after the waypoint 
  // (both fanuc & haas, contrary to emc historical operation)

  if (block->x_flag == ON) end_x = end_x_home;  
  if (block->y_flag == ON) end_y = end_y_home;  
  if (block->z_flag == ON) end_z = end_z_home;  
  if (block->a_flag == ON) AA_end = AA_end_home;
  if (block->b_flag == ON) BB_end = BB_end_home;
  if (block->c_flag == ON) CC_end = CC_end_home;
  if (block->u_flag == ON) u_end = u_end_home;  
  if (block->v_flag == ON) v_end = v_end_home;  
  if (block->w_flag == ON) w_end = w_end_home;  

  // but, if no axes are specified, home all of them 
  // (haas does this, emc historical did, throws an error in fanuc)

  if (block->x_flag == OFF && block->y_flag == OFF && block->z_flag == OFF &&
      block->a_flag == OFF && block->b_flag == OFF && block->c_flag == OFF &&
      block->u_flag == OFF && block->v_flag == OFF && block->w_flag == OFF) {
      end_x = end_x_home;  
      end_y = end_y_home;  
      end_z = end_z_home;  
      AA_end = AA_end_home;
      BB_end = BB_end_home;
      CC_end = CC_end_home;
      u_end = u_end_home;  
      v_end = v_end_home;  
      w_end = w_end_home;  
  }

  STRAIGHT_TRAVERSE(end_x, end_y, end_z,
                    AA_end, BB_end, CC_end,
                    u_end, v_end, w_end);
  settings->current_x = end_x;
  settings->current_y = end_y;
  settings->current_z = end_z;
  settings->AA_current = AA_end;
  settings->BB_current = BB_end;
  settings->CC_current = CC_end;
  settings->u_current = u_end;
  settings->v_current = v_end;
  settings->w_current = w_end;
  
  return INTERP_OK;
}

/****************************************************************************/

/*! convert_length_units

Returned Value: int
   If any of the following errors occur, this returns the error shown.
   Otherwise, it returns INTERP_OK.
   1. The g_code argument isnt G_20 or G_21:
      NCE_BUG_CODE_NOT_G20_OR_G21
   2. Cutter radius compensation is on:
      NCE_CANNOT_CHANGE_UNITS_WITH_CUTTER_RADIUS_COMP

Side effects:
   A command setting the length units is executed. The machine
   settings are reset regarding length units and current position.

Called by: convert_g.

Tool length offset and diameter, work coordinate systems, feed rate,
g28/g30 home positions, and g53 motion in absolute coordinates all work
properly after switching units.  Historically these had problems but the
intention is that they work properly now.

The tool table in settings is not converted here; it is always in
inifile units and the conversion happens when reading an entry.  Tool
offsets and feed rate that are in effect are converted by rereading them
from the canon level.

Cutter diameter is not converted because radius comp is not in effect
when we are changing units.  

XXX Other distance items in the settings (such as the various parameters
for cycles) need testing.

*/

int Interp::convert_length_units(int g_code,     //!< g_code being executed (must be G_20 or G_21)
                                setup_pointer settings) //!< pointer to machine settings                 
{
  static char name[] = "convert_length_units";
  CHK((settings->cutter_comp_side != OFF),
      NCE_CANNOT_CHANGE_UNITS_WITH_CUTTER_RADIUS_COMP);
  if (g_code == G_20) {
    USE_LENGTH_UNITS(CANON_UNITS_INCHES);
    if (settings->length_units != CANON_UNITS_INCHES) {
      settings->length_units = CANON_UNITS_INCHES;
      settings->current_x = (settings->current_x * INCH_PER_MM);
      settings->current_y = (settings->current_y * INCH_PER_MM);
      settings->current_z = (settings->current_z * INCH_PER_MM);
      settings->axis_offset_x = (settings->axis_offset_x * INCH_PER_MM);
      settings->axis_offset_y = (settings->axis_offset_y * INCH_PER_MM);
      settings->axis_offset_z = (settings->axis_offset_z * INCH_PER_MM);
      settings->origin_offset_x = (settings->origin_offset_x * INCH_PER_MM);
      settings->origin_offset_y = (settings->origin_offset_y * INCH_PER_MM);
      settings->origin_offset_z = (settings->origin_offset_z * INCH_PER_MM);

      settings->u_current = (settings->u_current * INCH_PER_MM);
      settings->v_current = (settings->v_current * INCH_PER_MM);
      settings->w_current = (settings->w_current * INCH_PER_MM);
      settings->u_axis_offset = (settings->u_axis_offset * INCH_PER_MM);
      settings->v_axis_offset = (settings->v_axis_offset * INCH_PER_MM);
      settings->w_axis_offset = (settings->w_axis_offset * INCH_PER_MM);
      settings->u_origin_offset = (settings->u_origin_offset * INCH_PER_MM);
      settings->v_origin_offset = (settings->v_origin_offset * INCH_PER_MM);
      settings->w_origin_offset = (settings->w_origin_offset * INCH_PER_MM);

      settings->tool_zoffset = GET_EXTERNAL_TOOL_LENGTH_ZOFFSET();
      settings->tool_xoffset = GET_EXTERNAL_TOOL_LENGTH_XOFFSET();
      settings->feed_rate = GET_EXTERNAL_FEED_RATE();
    }
  } else if (g_code == G_21) {
    USE_LENGTH_UNITS(CANON_UNITS_MM);
    if (settings->length_units != CANON_UNITS_MM) {
      settings->length_units = CANON_UNITS_MM;
      settings->current_x = (settings->current_x * MM_PER_INCH);
      settings->current_y = (settings->current_y * MM_PER_INCH);
      settings->current_z = (settings->current_z * MM_PER_INCH);
      settings->axis_offset_x = (settings->axis_offset_x * MM_PER_INCH);
      settings->axis_offset_y = (settings->axis_offset_y * MM_PER_INCH);
      settings->axis_offset_z = (settings->axis_offset_z * MM_PER_INCH);
      settings->origin_offset_x = (settings->origin_offset_x * MM_PER_INCH);
      settings->origin_offset_y = (settings->origin_offset_y * MM_PER_INCH);
      settings->origin_offset_z = (settings->origin_offset_z * MM_PER_INCH);

      settings->u_current = (settings->u_current * MM_PER_INCH);
      settings->v_current = (settings->v_current * MM_PER_INCH);
      settings->w_current = (settings->w_current * MM_PER_INCH);
      settings->u_axis_offset = (settings->u_axis_offset * MM_PER_INCH);
      settings->v_axis_offset = (settings->v_axis_offset * MM_PER_INCH);
      settings->w_axis_offset = (settings->w_axis_offset * MM_PER_INCH);
      settings->u_origin_offset = (settings->u_origin_offset * MM_PER_INCH);
      settings->v_origin_offset = (settings->v_origin_offset * MM_PER_INCH);
      settings->w_origin_offset = (settings->w_origin_offset * MM_PER_INCH);

      settings->tool_zoffset = GET_EXTERNAL_TOOL_LENGTH_ZOFFSET();
      settings->tool_xoffset = GET_EXTERNAL_TOOL_LENGTH_XOFFSET();
      settings->feed_rate = GET_EXTERNAL_FEED_RATE();
    }
  } else
    ERM(NCE_BUG_CODE_NOT_G20_OR_G21);
  return INTERP_OK;
}

/****************************************************************************/

/*! convert_m

Returned Value: int
   If convert_tool_change returns an error code, this returns that code.
   If input-related stuff is needed, it sets the flag input_flag = ON.
   Otherwise, it returns INTERP_OK.

Side effects:
   m_codes in the block are executed. For each m_code
   this consists of making a function call(s) to a canonical machining
   function(s) and setting the machine model.

Called by: execute_block.

This handles four separate types of activity in order:
1. changing the tool (m6) - which also retracts and stops the spindle.
2. Turning the spindle on or off (m3, m4, and m5)
3. Turning coolant on and off (m7, m8, and m9)
4. turning a-axis clamping on and off (m26, m27) - commented out.
5. enabling or disabling feed and speed overrides (m49, m49).
Within each group, only the first code encountered will be executed.

This does nothing with m0, m1, m2, m30, or m60 (which are handled in
convert_stop).

*/

int Interp::convert_m(block_pointer block,       //!< pointer to a block of RS274/NGC instructions
                     setup_pointer settings)    //!< pointer to machine settings                 
{
  static char name[] = "convert_m";
  int status;
  int type, timeout;
  double *pars;                 /* short name for settings->parameters            */

  pars = settings->parameters;

  /* The M62-65 commands are used for DIO */
  /* M62 sets a DIO synched with motion
     M63 clears a DIO synched with motion
     M64 sets a DIO imediately
     M65 clears a DIO imediately 
     M66 waits for an input
     M67 reads a digital input
     M68 reads an analog input*/

  if (block->m_modes[5] == 62) {
    SET_MOTION_OUTPUT_BIT(round_to_int(block->p_number));
  } else if (block->m_modes[5] == 63) {
    CLEAR_MOTION_OUTPUT_BIT(round_to_int(block->p_number));
  } else if (block->m_modes[5] == 64) {
    SET_AUX_OUTPUT_BIT(round_to_int(block->p_number));
  } else if (block->m_modes[5] == 65) {
    CLEAR_AUX_OUTPUT_BIT(round_to_int(block->p_number));
  } else if (block->m_modes[5] == 66) {
    //P-word = digital channel
    //E-word = analog channel
    //L-word = wait type (immediate, rise, fall, high, low)
    //Q-word = timeout
    // it is an error if:

    // P and E word are specified together
    CHK(((block->p_flag == ON) && (block->e_flag == ON)),
	NCE_BOTH_DIGITAL_AND_ANALOG_INPUT_SELECTED);

    // L-word not 0, and timeout <= 0 
    CHK(((round_to_int(block->q_number) <= 0) && (block->l_flag == ON) && (round_to_int(block->l_number) >= 0)),
	NCE_ZERO_TIMEOUT_WITH_WAIT_NOT_IMMEDIATE);
	
    // E-word specified (analog input) and wait type not immediate
    CHK(((block->e_flag == ON) && (block->l_flag == ON) && (round_to_int(block->l_number) != 0)),
	NCE_ANALOG_INPUT_WITH_WAIT_NOT_IMMEDIATE);

    // missing P or E (or invalid = negative)
    CHK( ((block->p_flag == ON) && (round_to_int(block->p_number) < 0)) || 
         ((block->e_flag == ON) && (round_to_int(block->e_number) < 0)) ||
	 ((block->p_flag == OFF) && (block->e_flag == OFF)) ,
	NCE_INVALID_OR_MISSING_P_AND_E_WORDS_FOR_WAIT_INPUT);

    if (block->p_flag == ON) { // got a digital input
	if (round_to_int(block->p_number) < 0) // safety check for negative words
	    ERS("invalid P-word with M66");
	    
	if (block->l_flag == ON)
	    type = round_to_int(block->l_number);
	else 
	    type = WAIT_MODE_IMMEDIATE;
	    
	if (round_to_int(block->q_number) >= 0)
	    timeout = round_to_int(block->q_number);
	else
	    timeout = 0;

	WAIT(round_to_int(block->p_number), DIGITAL_INPUT, type, timeout); 
	settings->input_flag = ON;
	settings->input_index = round_to_int(block->p_number);
	settings->input_digital = ON;
    } else if (round_to_int(block->e_number) >= 0) { // got an analog input
	WAIT(round_to_int(block->e_number), ANALOG_INPUT, 0, 0);
	settings->input_flag = ON;
	settings->input_index = round_to_int(block->e_number);
	settings->input_digital = OFF;
    } 
  }    

  if (block->m_modes[6] != -1) {
    CHP(convert_tool_change(settings));
#ifdef DEBATABLE
    // I would like this, but it's a big change.  It changes the
    // operation of legal ngc programs, but it could be argued that
    // those programs are buggy or likely to be not what the author
    // intended.

    // It would allow you to turn on G43 after loading the first tool,
    // and then not worry about it through the program.  When you
    // finally unload the last tool, G43 mode is canceled.

    if(settings->active_g_codes[9] == G_43) {
        if(settings->selected_tool_slot > 0) {
            struct block_struct g43;
            init_block(&g43);
            block->g_modes[_gees[G_43]] = G_43;
            CHP(convert_tool_length_offset(G_43, &g43, settings));
        } else {
            struct block_struct g49;
            init_block(&g49);
            block->g_modes[_gees[G_49]] = G_49;
            CHP(convert_tool_length_offset(G_49, &g49, settings));
        }
    }
#endif
  }

  if (block->m_modes[7] == 3) {
    START_SPINDLE_CLOCKWISE();
    settings->spindle_turning = CANON_CLOCKWISE;
  } else if (block->m_modes[7] == 4) {
    START_SPINDLE_COUNTERCLOCKWISE();
    settings->spindle_turning = CANON_COUNTERCLOCKWISE;
  } else if (block->m_modes[7] == 5) {
    STOP_SPINDLE_TURNING();
    settings->spindle_turning = CANON_STOPPED;
  }

  if (block->m_modes[8] == 7) {
    MIST_ON();
    settings->mist = ON;
  } else if (block->m_modes[8] == 8) {
    FLOOD_ON();
    settings->flood = ON;
  } else if (block->m_modes[8] == 9) {
    MIST_OFF();
    settings->mist = OFF;
    FLOOD_OFF();
    settings->flood = OFF;
  }

/* No axis clamps in this version
  if (block->m_modes[2] == 26)
    {
#ifdef DEBUG_EMC
      COMMENT("interpreter: automatic A-axis clamping turned on");
#endif
      settings->a_axis_clamping = ON;
    }
  else if (block->m_modes[2] == 27)
    {
#ifdef DEBUG_EMC
      COMMENT("interpreter: automatic A-axis clamping turned off");
#endif
      settings->a_axis_clamping = OFF;
    }
*/

  if (block->m_modes[9] == 48) {
    ENABLE_FEED_OVERRIDE();
    ENABLE_SPEED_OVERRIDE();
    settings->feed_override = ON;
    settings->speed_override = ON;
  } else if (block->m_modes[9] == 49) {
    DISABLE_FEED_OVERRIDE();
    DISABLE_SPEED_OVERRIDE();
    settings->feed_override = OFF;
    settings->speed_override = OFF;
  }

  if (block->m_modes[9] == 50) {
    if (block->p_number != 0) {
	ENABLE_FEED_OVERRIDE();
	settings->feed_override = ON;
    } else {
	DISABLE_FEED_OVERRIDE();
	settings->feed_override = OFF;
    }
  }

  if (block->m_modes[9] == 51) {
    if (block->p_number != 0) {
	ENABLE_SPEED_OVERRIDE();
	settings->speed_override = ON;
    } else {
	DISABLE_SPEED_OVERRIDE();
	settings->speed_override = OFF;
    }
  }
  
  if (block->m_modes[9] == 52) {
    if (block->p_number != 0) {
	ENABLE_ADAPTIVE_FEED();
	settings->adaptive_feed = ON;
    } else {
	DISABLE_ADAPTIVE_FEED();
	settings->adaptive_feed = OFF;
    }
  }
  
  if (block->m_modes[9] == 53) {
    if (block->p_number != 0) {
	ENABLE_FEED_HOLD();
	settings->feed_hold = ON;
    } else {
	DISABLE_FEED_HOLD();
	settings->feed_hold = OFF;
    }
  }

  /* user-defined M codes */
  if (block->m_modes[10] != -1) {
    int index = block->m_modes[10];
    if (USER_DEFINED_FUNCTION[index - 100] != 0) {
      (*(USER_DEFINED_FUNCTION[index - 100])) (index - 100,
                                               block->p_number,
                                               block->q_number);
    } else {
      CHK(1, NCE_UNKNOWN_M_CODE_USED);
    }
  }
  return INTERP_OK;
}

/****************************************************************************/

/*! convert_modal_0

Returned Value: int
   If one of the following functions is called and returns an error code,
   this returns that code.
      convert_axis_offsets
      convert_home
      convert_setup
   If any of the following errors occur, this returns the error code shown.
   Otherwise, it returns INTERP_OK.
   1. code is not G_4, G_10, G_28, G_30, G_53, G92, G_92_1, G_92_2, or G_92_3:
      NCE_BUG_CODE_NOT_G4_G10_G28_G30_G53_OR_G92_SERIES

Side effects: See below

Called by: convert_g

If the g_code is g10, g28, g30, g92, g92.1, g92.2, or g92.3 (all are in
modal group 0), it is executed. The other two in modal group 0 (G4 and
G53) are executed elsewhere.

*/

int Interp::convert_modal_0(int code,    //!< G code, must be from group 0                
                           block_pointer block, //!< pointer to a block of RS274/NGC instructions
                           setup_pointer settings)      //!< pointer to machine settings                 
{
  static char name[] = "convert_modal_0";
  int status;

  if (code == G_10) {
    CHP(convert_setup(block, settings));
  } else if ((code == G_28) || (code == G_30)) {
    CHP(convert_home(code, block, settings));
  } else if ((code == G_92) || (code == G_92_1) ||
             (code == G_92_2) || (code == G_92_3)) {
    CHP(convert_axis_offsets(code, block, settings));
  } else if ((code == G_4) || (code == G_53));  /* handled elsewhere */
  else
    ERM(NCE_BUG_CODE_NOT_G4_G10_G28_G30_G53_OR_G92_SERIES);
  return INTERP_OK;
}

/****************************************************************************/

/*! convert_motion

Returned Value: int
   If one of the following functions is called and returns an error code,
   this returns that code.
      convert_arc
      convert_cycle
      convert_probe
      convert_straight
   If any of the following errors occur, this returns the error shown.
   Otherwise, it returns INTERP_OK.
   1. The motion code is not 0,1,2,3,38.2,80,81,82,83,84,85,86,87, 88, or 89:
      NCE_BUG_UNKNOWN_MOTION_CODE

Side effects:
   A g_code from the group causing motion (mode 1) is executed.

Called by: convert_g.

*/

int Interp::convert_motion(int motion,   //!< g_code for a line, arc, canned cycle     
                          block_pointer block,  //!< pointer to a block of RS274 instructions 
                          setup_pointer settings)       //!< pointer to machine settings              
{
  static char name[] = "convert_motion";
  int status;

  if ((motion == G_0) || (motion == G_1) || (motion == G_33) || (motion == G_33_1) || (motion == G_76)) {
    CHP(convert_straight(motion, block, settings));
  } else if ((motion == G_3) || (motion == G_2)) {
    CHP(convert_arc(motion, block, settings));
  } else if (motion == G_38_2) {
    CHP(convert_probe(block, settings));
  } else if (motion == G_80) {
#ifdef DEBUG_EMC
    COMMENT("interpreter: motion mode set to none");
#endif
    settings->motion_mode = G_80;
  } else if ((motion > G_80) && (motion < G_90)) {
    CHP(convert_cycle(motion, block, settings));
  } else
    ERM(NCE_BUG_UNKNOWN_MOTION_CODE);

  return INTERP_OK;
}

/****************************************************************************/

/*! convert_probe

Returned Value: int
   If any of the following errors occur, this returns the error code shown.
   Otherwise, it returns INTERP_OK.
   1. No value is given in the block for any of X, Y, or Z:
      NCE_X_Y_AND_Z_WORDS_ALL_MISSING_WITH_G38_2
   2. feed mode is inverse time: NCE_CANNOT_PROBE_IN_INVERSE_TIME_FEED_MODE
   3. cutter radius comp is on: NCE_CANNOT_PROBE_WITH_CUTTER_RADIUS_COMP_ON
   4. Feed rate is zero: NCE_CANNOT_PROBE_WITH_ZERO_FEED_RATE
   5. The move is degenerate (already at the specified point)
      NCE_START_POINT_TOO_CLOSE_TO_PROBE_POINT

Side effects:
   This executes a straight_probe command.
   The probe_flag in the settings is set to ON.
   The motion mode in the settings is set to G_38_2.

Called by: convert_motion.

The approach to operating in incremental distance mode (g91) is to
put the the absolute position values into the block before using the
block to generate a move.

After probing is performed, the location of the probe cannot be
predicted. This differs from every other command, all of which have
predictable results. The next call to the interpreter (with either
Interp::read or Interp::execute) will result in updating the
current position by calls to get_external_position_x, etc.

*/

int Interp::convert_probe(block_pointer block,   //!< pointer to a block of RS274 instructions
                         setup_pointer settings)        //!< pointer to machine settings             
{
  static char name[] = "convert_probe";
  double end_x;
  double end_y;
  double end_z;
  double AA_end;
  double BB_end;
  double CC_end;
  double u_end;
  double v_end;
  double w_end;
  
  CHK((block->x_flag == OFF && block->y_flag == OFF &&
       block->z_flag == OFF && block->a_flag == OFF &&
       block->b_flag == OFF && block->c_flag == OFF &&
       block->u_flag == OFF && block->v_flag == OFF &&
       block->w_flag == OFF),
       NCE_X_Y_Z_A_B_C_U_V_AND_W_WORDS_ALL_MISSING_WITH_G38_2);
  CHK((settings->cutter_comp_side != OFF),
      NCE_CANNOT_PROBE_WITH_CUTTER_RADIUS_COMP_ON);
  CHK((settings->feed_rate == 0.0), NCE_CANNOT_PROBE_WITH_ZERO_FEED_RATE);
  CHKS(settings->feed_mode == UNITS_PER_REVOLUTION,
	  "Cannot probe with feed per rev mode");
  CHK((settings->feed_rate == 0.0), NCE_CANNOT_PROBE_WITH_ZERO_FEED_RATE);
  find_ends(block, settings, &end_x, &end_y, &end_z,
            &AA_end, &BB_end, &CC_end,
            &u_end, &v_end, &w_end);
  CHK((settings->current_x == end_x && settings->current_y == end_y &&
       settings->current_z == end_z && settings->AA_current == AA_end &&
       settings->BB_current == BB_end && settings->CC_current == CC_end &&
       settings->u_current == u_end && settings->v_current == v_end &&
       settings->w_current == w_end),
       NCE_START_POINT_TOO_CLOSE_TO_PROBE_POINT);
       
  TURN_PROBE_ON();
  STRAIGHT_PROBE(end_x, end_y, end_z,
                 AA_end, BB_end, CC_end,
                 u_end, v_end, w_end);

  TURN_PROBE_OFF();
  settings->motion_mode = G_38_2;
  settings->probe_flag = ON;
  return INTERP_OK;
}

/****************************************************************************/

/*! convert_retract_mode

Returned Value: int
   If any of the following errors occur, this returns the error code shown.
   Otherwise, it returns INTERP_OK.
   1. g_code isn't G_98 or G_99: NCE_BUG_CODE_NOT_G98_OR_G99

Side effects:
   The interpreter switches the machine settings to indicate the current
   retract mode for canned cycles (OLD_Z or R_PLANE).

Called by: convert_g.

The canonical machine to which commands are being sent does not have a
retract mode, so no command setting the retract mode is generated in
this function.

*/

int Interp::convert_retract_mode(int g_code,     //!< g_code being executed (must be G_98 or G_99)
                                setup_pointer settings) //!< pointer to machine settings                 
{
  static char name[] = "convert_retract_mode";
  if (g_code == G_98) {
#ifdef DEBUG_EMC
    COMMENT("interpreter: retract mode set to old_z");
#endif
    settings->retract_mode = OLD_Z;
  } else if (g_code == G_99) {
#ifdef DEBUG_EMC
    COMMENT("interpreter: retract mode set to r_plane");
#endif
    settings->retract_mode = R_PLANE;
  } else
    ERM(NCE_BUG_CODE_NOT_G98_OR_G99);
  return INTERP_OK;
}

/****************************************************************************/

/*! convert_setup

Returned Value: int (INTERP_OK)

Side effects:
   SET_PROGRAM_ORIGIN is called, and the coordinate
   values for the program origin are reset.
   If the program origin is currently in use, the values of the
   the coordinates of the current point are updated.

Called by: convert_modal_0.

This is called only if g10 is called. g10 L2 may be used to alter the
location of coordinate systems as described in [NCMS, pages 9 - 10] and
[Fanuc, page 65]. [Fanuc] has only six coordinate systems, while
[NCMS] has nine (the first six of which are the same as the six [Fanuc]
has). All nine are implemented here.

Being in incremental distance mode has no effect on the action of G10
in this implementation. The manual is not explicit about what is
intended.

See documentation of convert_coordinate_system for more information.

*/

int Interp::convert_setup(block_pointer block,   //!< pointer to a block of RS274/NGC instructions
                         setup_pointer settings)        //!< pointer to machine settings                 
{
  double x;
  double y;
  double z;
  double a;
  double b;
  double c;
  double u, v, w;
  double *parameters;
  int p_int;

  parameters = settings->parameters;
  p_int = (int) (block->p_number + 0.0001);

  if (block->x_flag == ON) {
    x = block->x_number;
    parameters[5201 + (p_int * 20)] = PROGRAM_TO_USER_LEN(x);
  } else
    x = USER_TO_PROGRAM_LEN(parameters[5201 + (p_int * 20)]);

  if (block->y_flag == ON) {
    y = block->y_number;
    parameters[5202 + (p_int * 20)] = PROGRAM_TO_USER_LEN(y);
  } else
    y = USER_TO_PROGRAM_LEN(parameters[5202 + (p_int * 20)]);

  if (block->z_flag == ON) {
    z = block->z_number;
    parameters[5203 + (p_int * 20)] = PROGRAM_TO_USER_LEN(z);
  } else
    z = USER_TO_PROGRAM_LEN(parameters[5203 + (p_int * 20)]);

  if (block->a_flag == ON) {
    a = block->a_number;
    parameters[5204 + (p_int * 20)] = PROGRAM_TO_USER_ANG(a);
  } else
    a = USER_TO_PROGRAM_ANG(parameters[5204 + (p_int * 20)]);

  if (block->b_flag == ON) {
    b = block->b_number;
    parameters[5205 + (p_int * 20)] = PROGRAM_TO_USER_ANG(b);
  } else
    b = USER_TO_PROGRAM_ANG(parameters[5205 + (p_int * 20)]);

  if (block->c_flag == ON) {
    c = block->c_number;
    parameters[5206 + (p_int * 20)] = PROGRAM_TO_USER_ANG(c);
  } else
    c = USER_TO_PROGRAM_ANG(parameters[5206 + (p_int * 20)]);

  if (block->u_flag == ON) {
    u = block->u_number;
    parameters[5207 + (p_int * 20)] = PROGRAM_TO_USER_LEN(u);
  } else
    u = USER_TO_PROGRAM_LEN(parameters[5207 + (p_int * 20)]);

  if (block->v_flag == ON) {
    v = block->v_number;
    parameters[5208 + (p_int * 20)] = PROGRAM_TO_USER_LEN(v);
  } else
    v = USER_TO_PROGRAM_LEN(parameters[5208 + (p_int * 20)]);

  if (block->w_flag == ON) {
    w = block->w_number;
    parameters[5209 + (p_int * 20)] = PROGRAM_TO_USER_LEN(w);
  } else
    w = USER_TO_PROGRAM_LEN(parameters[5209 + (p_int * 20)]);

/* axis offsets could be included in the two sets of calculations for
   current_x, current_y, etc., but do not need to be because the results
   would be the same. They would be added in then subtracted out. */
  if (p_int == settings->origin_index) {        /* system is currently used */
    settings->current_x = (settings->current_x + settings->origin_offset_x);
    settings->current_y = (settings->current_y + settings->origin_offset_y);
    settings->current_z = (settings->current_z + settings->origin_offset_z);
    settings->AA_current =
      (settings->AA_current + settings->AA_origin_offset);
    settings->BB_current =
      (settings->BB_current + settings->BB_origin_offset);
    settings->CC_current =
      (settings->CC_current + settings->CC_origin_offset);
    settings->u_current =
      (settings->u_current + settings->u_origin_offset);
    settings->v_current =
      (settings->v_current + settings->v_origin_offset);
    settings->w_current =
      (settings->w_current + settings->w_origin_offset);

    settings->origin_offset_x = x;
    settings->origin_offset_y = y;
    settings->origin_offset_z = z;
    settings->AA_origin_offset = a;
    settings->BB_origin_offset = b;
    settings->CC_origin_offset = c;
    settings->u_origin_offset = u;
    settings->v_origin_offset = v;
    settings->w_origin_offset = w;

    settings->current_x = (settings->current_x - x);
    settings->current_y = (settings->current_y - y);
    settings->current_z = (settings->current_z - z);
    settings->AA_current = (settings->AA_current - a);
    settings->BB_current = (settings->BB_current - b);
    settings->CC_current = (settings->CC_current - c);
    settings->u_current = (settings->u_current - u);
    settings->v_current = (settings->v_current - v);
    settings->w_current = (settings->w_current - w);

    SET_ORIGIN_OFFSETS(x + settings->axis_offset_x,
                       y + settings->axis_offset_y,
                       z + settings->axis_offset_z,
                       a + settings->AA_axis_offset,
                       b + settings->BB_axis_offset,
                       c + settings->CC_axis_offset,
                       u + settings->u_axis_offset,
                       v + settings->v_axis_offset,
                       w + settings->w_axis_offset);
  }
#ifdef DEBUG_EMC
  else
    COMMENT("interpreter: setting coordinate system origin");
#endif
  return INTERP_OK;
}

/****************************************************************************/

/*! convert_set_plane

Returned Value: int
   If any of the following errors occur, this returns the error code shown.
   Otherwise, it returns INTERP_OK.
   1. The user tries to change to a different plane while comp is on:
      NCE_CANNOT_CHANGE_PLANES_WITH_CUTTER_RADIUS_COMP_ON);
   2. The g_code is not G_17, G_18, or G_19:
      NCE_BUG_CODE_NOT_G17_G18_OR_G19

Side effects:
   A canonical command setting the current plane is executed.

Called by: convert_g.

*/

int Interp::convert_set_plane(int g_code,        //!< must be G_17, G_18, or G_19 
                             setup_pointer settings)    //!< pointer to machine settings 
{
  static char name[] = "convert_set_plane";
  CHK((settings->cutter_comp_side != OFF && g_code == G_17 && settings->plane != CANON_PLANE_XY),
        NCE_CANNOT_CHANGE_PLANES_WITH_CUTTER_RADIUS_COMP_ON);
  CHK((settings->cutter_comp_side != OFF && g_code == G_18 && settings->plane != CANON_PLANE_XZ),
        NCE_CANNOT_CHANGE_PLANES_WITH_CUTTER_RADIUS_COMP_ON);
  CHK((settings->cutter_comp_side != OFF && g_code == G_19 && settings->plane != CANON_PLANE_YZ),
        NCE_CANNOT_CHANGE_PLANES_WITH_CUTTER_RADIUS_COMP_ON);

  CHK((settings->cutter_comp_side != OFF && g_code == G_19), 
          NCE_RADIUS_COMP_ONLY_IN_XY_OR_XZ);

  if (g_code == G_17) {
    SELECT_PLANE(CANON_PLANE_XY);
    settings->plane = CANON_PLANE_XY;
  } else if (g_code == G_18) {
    SELECT_PLANE(CANON_PLANE_XZ);
    settings->plane = CANON_PLANE_XZ;
  } else if (g_code == G_19) {
    SELECT_PLANE(CANON_PLANE_YZ);
    settings->plane = CANON_PLANE_YZ;
  } else
    ERM(NCE_BUG_CODE_NOT_G17_G18_OR_G19);
  return INTERP_OK;
}

/****************************************************************************/

/*! convert_speed

Returned Value: int (INTERP_OK)

Side effects:
  The machine spindle speed is set to the value of s_number in the
  block by a call to SET_SPINDLE_SPEED.
  The machine model for spindle speed is set to that value.

Called by: execute_block.

*/

int Interp::convert_speed(block_pointer block,   //!< pointer to a block of RS274 instructions
                         setup_pointer settings)        //!< pointer to machine settings             
{
  SET_SPINDLE_SPEED(block->s_number);
  settings->speed = block->s_number;
  return INTERP_OK;
}

int Interp::convert_spindle_mode(block_pointer block, setup_pointer settings)
{
    if(block->g_modes[14] == G_97) {
	SET_SPINDLE_MODE(0);
    } else { /* G_96 */
	if(block->d_flag)
	    SET_SPINDLE_MODE(block->d_number_float);
	else
	    SET_SPINDLE_MODE(1e30);
    }
    return INTERP_OK;
}
/****************************************************************************/

/*! convert_stop

Returned Value: int
   When an m2 or m30 (program_end) is encountered, this returns INTERP_EXIT.
   If the code is not m0, m1, m2, m30, or m60, this returns
   NCE_BUG_CODE_NOT_M0_M1_M2_M30_M60
   Otherwise, it returns INTERP_OK.

Side effects:
   An m0, m1, m2, m30, or m60 in the block is executed.

   For m0, m1, and m60, this makes a function call to the PROGRAM_STOP
   canonical machining function (which stops program execution).
   In addition, m60 calls PALLET_SHUTTLE.

   For m2 and m30, this resets the machine and then calls PROGRAM_END.
   In addition, m30 calls PALLET_SHUTTLE.

Called by: execute_block.

This handles stopping or ending the program (m0, m1, m2, m30, m60)

[NCMS] specifies how the following modes should be reset at m2 or
m30. The descriptions are not collected in one place, so this list
may be incomplete.

G52 offsetting coordinate zero points [NCMS, page 10]
G92 coordinate offset using tool position [NCMS, page 10]

The following should have reset values, but no description of reset
behavior could be found in [NCMS].
G17, G18, G19 selected plane [NCMS, pages 14, 20]
G90, G91 distance mode [NCMS, page 15]
G93, G94 feed mode [NCMS, pages 35 - 37]
M48, M49 overrides enabled, disabled [NCMS, pages 37 - 38]
M3, M4, M5 spindle turning [NCMS, page 7]

The following should be set to some value at machine start-up but
not automatically reset by any of the stopping codes.
1. G20, G21 length units [NCMS, page 15]. This is up to the installer.
2. motion_control_mode. This is set in Interp::init but not reset here.
   Might add it here.

The following resets have been added by calling the appropriate
canonical machining command and/or by resetting interpreter
settings. They occur on M2 or M30.

1. Axis offsets are set to zero (like g92.2) and      - SET_ORIGIN_OFFSETS
   origin offsets are set to the default (like G54)
2. Selected plane is set to CANON_PLANE_XY (like G17) - SELECT_PLANE
3. Distance mode is set to MODE_ABSOLUTE (like G90)   - no canonical call
4. Feed mode is set to UNITS_PER_MINUTE (like G94)    - no canonical call
5. Feed and speed overrides are set to ON (like M48)  - ENABLE_FEED_OVERRIDE
                                                      - ENABLE_SPEED_OVERRIDE
6. Cutter compensation is turned off (like G40)       - no canonical call
7. The spindle is stopped (like M5)                   - STOP_SPINDLE_TURNING
8. The motion mode is set to G_1 (like G1)            - no canonical call
9. Coolant is turned off (like M9)                    - FLOOD_OFF & MIST_OFF

*/

int Interp::convert_stop(block_pointer block,    //!< pointer to a block of RS274/NGC instructions
                        setup_pointer settings) //!< pointer to machine settings                 
{
  static char name[] = "convert_stop";
  int index;
  char *line;
  int length;

  if (block->m_modes[4] == 0) {
    PROGRAM_STOP();
  } else if (block->m_modes[4] == 60) {
    PALLET_SHUTTLE();
    PROGRAM_STOP();
  } else if (block->m_modes[4] == 1) {
    OPTIONAL_PROGRAM_STOP();
  } else if ((block->m_modes[4] == 2) || (block->m_modes[4] == 30)) {   /* reset stuff here */
/*1*/
    settings->current_x = settings->current_x
      + settings->origin_offset_x + settings->axis_offset_x;
    settings->current_y = settings->current_y
      + settings->origin_offset_y + settings->axis_offset_y;
    settings->current_z = settings->current_z
      + settings->origin_offset_z + settings->axis_offset_z;
    settings->AA_current = settings->AA_current
      + settings->AA_origin_offset + settings->AA_axis_offset;
    settings->BB_current = settings->BB_current
      + settings->BB_origin_offset + settings->BB_axis_offset;
    settings->CC_current = settings->CC_current
      + settings->CC_origin_offset + settings->CC_axis_offset;
    settings->u_current = settings->u_current
      + settings->u_origin_offset + settings->u_axis_offset;
    settings->v_current = settings->v_current
      + settings->v_origin_offset + settings->v_axis_offset;
    settings->w_current = settings->w_current
      + settings->w_origin_offset + settings->w_axis_offset;
    settings->origin_index = 1;
    settings->parameters[5220] = 1.0;
    settings->origin_offset_x = USER_TO_PROGRAM_LEN(settings->parameters[5221]);
    settings->origin_offset_y = USER_TO_PROGRAM_LEN(settings->parameters[5222]);
    settings->origin_offset_z = USER_TO_PROGRAM_LEN(settings->parameters[5223]);
    settings->AA_origin_offset = USER_TO_PROGRAM_ANG(settings->parameters[5224]);
    settings->BB_origin_offset = USER_TO_PROGRAM_ANG(settings->parameters[5225]);
    settings->CC_origin_offset = USER_TO_PROGRAM_ANG(settings->parameters[5226]);
    settings->u_origin_offset = USER_TO_PROGRAM_LEN(settings->parameters[5227]);
    settings->v_origin_offset = USER_TO_PROGRAM_LEN(settings->parameters[5228]);
    settings->w_origin_offset = USER_TO_PROGRAM_LEN(settings->parameters[5229]);

    settings->axis_offset_x = 0;
    settings->axis_offset_y = 0;
    settings->axis_offset_z = 0;
    settings->AA_axis_offset = 0;
    settings->BB_axis_offset = 0;
    settings->CC_axis_offset = 0;
    settings->u_axis_offset = 0;
    settings->v_axis_offset = 0;
    settings->w_axis_offset = 0;

    settings->current_x = settings->current_x - settings->origin_offset_x;
    settings->current_y = settings->current_y - settings->origin_offset_y;
    settings->current_z = settings->current_z - settings->origin_offset_z;
    settings->AA_current = settings->AA_current - settings->AA_origin_offset;
    settings->BB_current = settings->BB_current - settings->BB_origin_offset;
    settings->CC_current = settings->CC_current - settings->CC_origin_offset;
    settings->u_current = settings->u_current - settings->u_origin_offset;
    settings->v_current = settings->v_current - settings->v_origin_offset;
    settings->w_current = settings->w_current - settings->w_origin_offset;

    SET_ORIGIN_OFFSETS(settings->origin_offset_x,
                       settings->origin_offset_y, settings->origin_offset_z,
                       settings->AA_origin_offset,
                       settings->BB_origin_offset,
                       settings->CC_origin_offset,
                       settings->u_origin_offset,
                       settings->v_origin_offset,
                       settings->w_origin_offset);

/*2*/ if (settings->plane != CANON_PLANE_XY) {
      SELECT_PLANE(CANON_PLANE_XY);
      settings->plane = CANON_PLANE_XY;
    }

/*3*/
    settings->distance_mode = MODE_ABSOLUTE;

/*4*/ settings->feed_mode = UNITS_PER_MINUTE;
    SET_FEED_MODE(0);
    SET_FEED_RATE(0);

/*5*/ if (settings->feed_override != ON) {
      ENABLE_FEED_OVERRIDE();
      settings->feed_override = ON;
    }
    if (settings->speed_override != ON) {
      ENABLE_SPEED_OVERRIDE();
      settings->speed_override = ON;
    }

/*6*/
    settings->cutter_comp_side = OFF;
    settings->cutter_comp_firstmove = ON;

/*7*/ STOP_SPINDLE_TURNING();
    settings->spindle_turning = CANON_STOPPED;

    /* turn off FPR */
    SET_SPINDLE_MODE(0);

/*8*/ settings->motion_mode = G_1;

/*9*/ if (settings->mist == ON) {
      MIST_OFF();
      settings->mist = OFF;
    }
    if (settings->flood == ON) {
      FLOOD_OFF();
      settings->flood = OFF;
    }

    if (block->m_modes[4] == 30)
      PALLET_SHUTTLE();
    PROGRAM_END();
    if (_setup.percent_flag == ON) {
      CHK((_setup.file_pointer == NULL), NCE_UNABLE_TO_OPEN_FILE);
      line = _setup.linetext;
      for (;;) {                /* check for ending percent sign and comment if missing */
        if (fgets(line, LINELEN, _setup.file_pointer) == NULL) {
          COMMENT("interpreter: percent sign missing from end of file");
          break;
        }
        length = strlen(line);
        if (length == (LINELEN - 1)) {       // line is too long. need to finish reading the line
          for (; fgetc(_setup.file_pointer) != '\n';);
          continue;
        }
        for (index = (length - 1);      // index set on last char
             (index >= 0) && (isspace(line[index])); index--);
        if (line[index] == '%') // found line with % at end
        {
          for (index--; (index >= 0) && (isspace(line[index])); index--);
          if (index == -1)      // found line with only percent sign
            break;
        }
      }
    }
    return INTERP_EXIT;
  } else
    ERM(NCE_BUG_CODE_NOT_M0_M1_M2_M30_M60);
  return INTERP_OK;
}

/*************************************************************************** */

/*! convert_straight

Returned Value: int
   If convert_straight_comp1 or convert_straight_comp2 is called
   and returns an error code, this returns that code.
   If any of the following errors occur, this returns the error shown.
   Otherwise, it returns INTERP_OK.
   1. The value of move is not G_0 or G_1:
      NCE_BUG_CODE_NOT_G0_OR_G1
   2. A straight feed (g1) move is called with feed rate set to 0:
      NCE_CANNOT_DO_G1_WITH_ZERO_FEED_RATE
   3. A straight feed (g1) move is called with inverse time feed in effect
      but no f word (feed time) is provided:
      NCE_F_WORD_MISSING_WITH_INVERSE_TIME_G1_MOVE
   4. A move is called with G53 and cutter radius compensation on:
      NCE_CANNOT_USE_G53_WITH_CUTTER_RADIUS_COMP
   5. A G33 move is called without the necessary support compiled in:
      NCE_G33_NOT_SUPPORTED

Side effects:
   This executes a STRAIGHT_FEED command at cutting feed rate
   (if move is G_1) or a STRAIGHT_TRAVERSE command (if move is G_0).
   It also updates the setting of the position of the tool point to the
   end point of the move. If cutter radius compensation is on, it may
   also generate an arc before the straight move. Also, in INVERSE_TIME
   feed mode, SET_FEED_RATE will be called the feed rate setting changed.

Called by: convert_motion.

The approach to operating in incremental distance mode (g91) is to
put the the absolute position values into the block before using the
block to generate a move.

In inverse time feed mode, a lower bound of 0.1 is placed on the feed
rate so that the feed rate is never set to zero. If the destination
point is the same as the current point, the feed rate would be
calculated as zero otherwise.

*/


int Interp::convert_straight(int move,   //!< either G_0 or G_1                       
                            block_pointer block,        //!< pointer to a block of RS274 instructions
                            setup_pointer settings)     //!< pointer to machine settings             
{
  static char name[] = "convert_straight";
  double end_x;
  double end_y;
  double end_z;
  double AA_end;
  double BB_end;
  double CC_end;
  double u_end, v_end, w_end;
  int status;

  if (move == G_1) {
    if (settings->feed_mode == UNITS_PER_MINUTE) {
      CHK((settings->feed_rate == 0.0), NCE_CANNOT_DO_G1_WITH_ZERO_FEED_RATE);
    } else if (settings->feed_mode == UNITS_PER_REVOLUTION) {
      CHK((settings->feed_rate == 0.0), NCE_CANNOT_DO_G1_WITH_ZERO_FEED_RATE);
      CHKS((settings->speed == 0.0), "Cannot feed with zero spindle speed in feed per rev mode");
    } else if (settings->feed_mode == INVERSE_TIME) {
      CHK((block->f_number == -1.0),
          NCE_F_WORD_MISSING_WITH_INVERSE_TIME_G1_MOVE);
    }
  }

  settings->motion_mode = move;
  find_ends(block, settings, &end_x, &end_y, &end_z,
            &AA_end, &BB_end, &CC_end, &u_end, &v_end, &w_end);

  if ((settings->cutter_comp_side != OFF) &&    /* ! "== ON" */
      (settings->cutter_comp_radius > 0.0)) {   /* radius always is >= 0 */
    CHK((block->g_modes[0] == G_53),
        NCE_CANNOT_USE_G53_WITH_CUTTER_RADIUS_COMP);
    if (settings->cutter_comp_firstmove == ON) {
      status =
        convert_straight_comp1(move, block, settings, end_x, end_y, end_z,
                               AA_end, BB_end, CC_end, u_end, v_end, w_end);

      CHP(status);
    } else {
      status =
        convert_straight_comp2(move, block, settings, end_x, end_y, end_z,
                               AA_end, BB_end, CC_end, u_end, v_end, w_end);
      CHP(status);
    }
  } else if (move == G_0) {
    STRAIGHT_TRAVERSE(end_x, end_y, end_z,
                      AA_end, BB_end, CC_end,
                      u_end, v_end, w_end);
    settings->current_x = end_x;
    settings->current_y = end_y;
    settings->current_z = end_z;
  } else if (move == G_1) {
    if (settings->feed_mode == INVERSE_TIME)
      inverse_time_rate_straight(end_x, end_y, end_z,
                                 AA_end, BB_end, CC_end,
                                 u_end, v_end, w_end,
                                 block, settings);
    STRAIGHT_FEED(end_x, end_y, end_z,
                  AA_end, BB_end, CC_end,
                  u_end, v_end, w_end);
    settings->current_x = end_x;
    settings->current_y = end_y;
    settings->current_z = end_z;
  } else if (move == G_33) {
    CHKS(((settings->spindle_turning != CANON_CLOCKWISE) &&
           (settings->spindle_turning != CANON_COUNTERCLOCKWISE)),
          "Spindle not turning in G33");
    START_SPEED_FEED_SYNCH(block->k_number, 0);
    STRAIGHT_FEED(end_x, end_y, end_z, AA_end, BB_end, CC_end, u_end, v_end, w_end);
    STOP_SPEED_FEED_SYNCH();
    settings->current_x = end_x;
    settings->current_y = end_y;
    settings->current_z = end_z;
  } else if (move == G_33_1) {
    CHKS(((settings->spindle_turning != CANON_CLOCKWISE) &&
           (settings->spindle_turning != CANON_COUNTERCLOCKWISE)),
          "Spindle not turning in G33.1");
    START_SPEED_FEED_SYNCH(block->k_number, 0);
    RIGID_TAP(end_x, end_y, end_z);
    STOP_SPEED_FEED_SYNCH();
    // after the RIGID_TAP cycle we'll be in the same spot
  } else if (move == G_76) {
    CHK((settings->AA_current != AA_end || 
         settings->BB_current != BB_end || 
         settings->CC_current != CC_end ||
         settings->u_current != u_end ||
         settings->v_current != v_end ||
         settings->w_current != w_end), NCE_CANNOT_MOVE_ROTARY_AXES_WITH_G76);
    convert_threading_cycle(block, settings, end_x, end_y, end_z);
  } else
    ERM(NCE_BUG_CODE_NOT_G0_OR_G1);

  settings->AA_current = AA_end;
  settings->BB_current = BB_end;
  settings->CC_current = CC_end;
  settings->u_current = u_end;
  settings->v_current = v_end;
  settings->w_current = w_end;
  return INTERP_OK;
}

#define AABBCC settings->AA_current, settings->BB_current, settings->CC_current, settings->u_current, settings->v_current, settings->w_current

// make one threading pass.  only called from convert_threading_cycle.
static void 
threading_pass(setup_pointer settings,
	       int boring, double safe_x, double depth, double end_depth, 
	       double start_y, double start_z, double zoff, double taper_dist,
	       int entry_taper, int exit_taper, double taper_pitch, 
	       double pitch, double full_threadheight, double target_z) {
    STRAIGHT_TRAVERSE(boring?
		      safe_x + depth - end_depth:
		      safe_x - depth + end_depth,
		      start_y, start_z - zoff, AABBCC); //back
    if(taper_dist && entry_taper) {
	DISABLE_FEED_OVERRIDE();
	START_SPEED_FEED_SYNCH(taper_pitch, 0);
	STRAIGHT_FEED(boring? 
		      safe_x + depth - full_threadheight: 
		      safe_x - depth + full_threadheight,
		      start_y, start_z - zoff, AABBCC); //in
	STRAIGHT_FEED(boring? safe_x + depth: safe_x - depth, //angled in
		      start_y, start_z - zoff - taper_dist, AABBCC);
	START_SPEED_FEED_SYNCH(pitch, 0);
    } else {
	STRAIGHT_TRAVERSE(boring? safe_x + depth: safe_x - depth, 
			  start_y, start_z - zoff, AABBCC); //in
	DISABLE_FEED_OVERRIDE();
	START_SPEED_FEED_SYNCH(pitch, 0);
    }
        
    if(taper_dist && exit_taper) {
	STRAIGHT_FEED(boring? safe_x + depth: safe_x - depth,  //over
		      start_y, target_z - zoff + taper_dist, AABBCC);
	START_SPEED_FEED_SYNCH(taper_pitch, 0);
	STRAIGHT_FEED(boring? 
		      safe_x + depth - full_threadheight: 
		      safe_x - depth + full_threadheight, 
		      start_y, target_z - zoff, AABBCC); //angled out
    } else {
	STRAIGHT_FEED(boring? safe_x + depth: safe_x - depth, 
		      start_y, target_z - zoff, AABBCC); //over
    }
    STOP_SPEED_FEED_SYNCH();
    STRAIGHT_TRAVERSE(boring? 
		      safe_x + depth - end_depth:
		      safe_x - depth + end_depth,
		      start_y, target_z - zoff, AABBCC); //out
    ENABLE_FEED_OVERRIDE();
}

int Interp::convert_threading_cycle(block_pointer block, 
				    setup_pointer settings,
				    double end_x, double end_y, double end_z) {
    double start_x = settings->current_x;
    double start_y = settings->current_y;
    double start_z = settings->current_z;

    int boring = 0;

    if (block->i_number > 0.0)
	boring = 1;

    double safe_x = start_x;
    double full_dia_depth = fabs(block->i_number);
    double start_depth = fabs(block->i_number) + fabs(block->j_number);
    double cut_increment = fabs(block->j_number);
    double full_threadheight = fabs(block->k_number);
    double end_depth = fabs(block->k_number) + fabs(block->i_number);

    double pitch = block->p_number;
    double compound_angle = block->q_number;
    if(compound_angle == -1) compound_angle = 0;
    compound_angle *= M_PIl/180.0;
    if(end_z > start_z) compound_angle = -compound_angle;

    int spring_cuts = block->h_flag == ON ? block->h_number: 0;

    double degression = block->r_number;
    if(degression < 1.0 || !block->r_flag) degression = 1.0;

    double taper_dist = block->e_flag? block->e_number: 0.0;
    if(taper_dist < 0.0) taper_dist = 0.0;
    double taper_pitch = taper_dist > 0.0? 
	pitch * hypot(taper_dist, full_threadheight)/taper_dist: pitch;

    if(end_z > start_z) taper_dist = -taper_dist;

    int taper_flags = block->l_number;
    if(taper_flags < 0) taper_flags = 0;

    int entry_taper = taper_flags & 1;
    int exit_taper = taper_flags & 2;

    double depth, zoff;
    int pass = 1;

    double target_z = end_z + fabs(block->k_number) * tan(compound_angle);

    depth = start_depth;
    zoff = (depth - full_dia_depth) * tan(compound_angle);
    while (depth < end_depth) {
	threading_pass(settings, boring, safe_x, depth, end_depth, start_y, 
		       start_z, zoff, taper_dist, entry_taper, exit_taper, 
		       taper_pitch, pitch, full_threadheight, target_z);
        depth = full_dia_depth + cut_increment * pow(++pass, 1.0/degression);
        zoff = (depth - full_dia_depth) * tan(compound_angle);
    } 
    // full specified depth now
    depth = end_depth;
    zoff = (depth - full_dia_depth) * tan(compound_angle);
    // cut at least once -- more if spring cuts.
    for(int i = 0; i<spring_cuts+1; i++) {
	threading_pass(settings, boring, safe_x, depth, end_depth, start_y, 
		       start_z, zoff, taper_dist, entry_taper, exit_taper, 
		       taper_pitch, pitch, full_threadheight, target_z);
    } 
    STRAIGHT_TRAVERSE(end_x, end_y, end_z, AABBCC);
    settings->current_x = end_x;
    settings->current_y = end_y;
    settings->current_z = end_z;
#undef AABBC
    return INTERP_OK;
}


/****************************************************************************/

/*! convert_straight_comp1

Returned Value: int
   If any of the following errors occur, this returns the error shown.
   Otherwise, it returns INTERP_OK.
   1. The side is not RIGHT or LEFT:
      NCE_BUG_SIDE_NOT_RIGHT_OR_LEFT
   2. The destination tangent point is not more than a tool radius
      away (indicating gouging): NCE_CUTTER_GOUGING_WITH_CUTTER_RADIUS_COMP
   3. The value of move is not G_0 or G_1
      NCE_BUG_CODE_NOT_G0_OR_G1

Side effects:
   This executes a STRAIGHT_MOVE command at cutting feed rate
   or a STRAIGHT_TRAVERSE command.
   It also updates the setting of the position of the tool point
   to the end point of the move and updates the programmed point.
   If INVERSE_TIME feed rate mode is in effect, it resets the feed rate.

Called by: convert_straight.

This is called if cutter radius compensation is on and
settings->cutter_comp_firstmove is ON, indicating that this is the
first move after cutter radius compensation is turned on.

The algorithm used here for determining the path is to draw a straight
line from the destination point which is tangent to a circle whose
center is at the current point and whose radius is the radius of the
cutter. The destination point of the cutter tip is then found as the
center of a circle of the same radius tangent to the tangent line at
the destination point.

*/

int Interp::convert_straight_comp1(int move,     //!< either G_0 or G_1                        
                                   block_pointer block,  //!< pointer to a block of RS274 instructions 
                                   setup_pointer settings,       //!< pointer to machine settings              
                                   double px,    //!< X coordinate of end point                
                                   double py,    //!< Y coordinate of end point                
                                   double pz,    //!< Z coordinate of end point                
                                   double AA_end,        //!< A coordinate of end point          
                                   double BB_end,        //!< B coordinate of end point          
                                   double CC_end,        //!< C coordinate of end point          
                                   double u_end, double v_end, double w_end)
{
  static char name[] = "convert_straight_comp1";
  double alpha;
  double distance;
  double radius;
  int side;
  double theta;
  double p[3], c[2], tp[2];

  if(settings->plane == CANON_PLANE_XZ) {
      p[0] = px;
      p[1] = pz;
      p[2] = py;
      tp[0] = xtrans(settings, px);
      tp[1] = ztrans(settings, pz);
      c[0] = settings->current_x;
      c[1] = settings->current_z;
  } else if (settings->plane == CANON_PLANE_XY) {
      tp[0] = p[0] = px;
      tp[1] = p[1] = py;
      p[2] = pz;
      c[0] = settings->current_x;
      c[1] = settings->current_y;
  } else ERM(NCE_RADIUS_COMP_ONLY_IN_XY_OR_XZ);

  side = settings->cutter_comp_side;

  radius = settings->cutter_comp_radius;        /* always will be positive */
  distance = hypot((tp[0] - c[0]), (tp[1] - c[1]));

  CHK(((side != LEFT) && (side != RIGHT)), NCE_BUG_SIDE_NOT_RIGHT_OR_LEFT);
  CHK((distance <= radius), NCE_CUTTER_GOUGING_WITH_CUTTER_RADIUS_COMP);

  theta = acos(radius / distance);
  alpha = (side == LEFT) ? (atan2((c[1] - tp[1]), (c[0] - tp[0])) - theta) :
                           (atan2((c[1] - tp[1]), (c[0] - tp[0])) + theta);
  c[0] = (p[0] + (radius * cos(alpha)));    /* reset to end location */
  c[1] = (p[1] + (radius * sin(alpha)));

  if (move == G_0) {
     if(settings->plane == CANON_PLANE_XZ) {
         STRAIGHT_TRAVERSE(xtrans(settings, c[0]), p[2], ztrans(settings, c[1]),
                           AA_end, BB_end, CC_end, u_end, v_end, w_end);
     } else if(settings->plane == CANON_PLANE_XY) {
         STRAIGHT_TRAVERSE(c[0], c[1], p[2],
                           AA_end, BB_end, CC_end, u_end, v_end, w_end);
     }
  }
  else if (move == G_1) {
    if(settings->plane == CANON_PLANE_XZ) {
       if (settings->feed_mode == INVERSE_TIME)
         inverse_time_rate_straight(c[0], p[2], c[1],
                                    AA_end, BB_end, CC_end,
                                    u_end, v_end, w_end,
                                    block, settings);
       STRAIGHT_FEED(xtrans(settings, c[0]), p[2], ztrans(settings, c[1]),
                     AA_end, BB_end, CC_end, u_end, v_end, w_end);
    } else if(settings->plane == CANON_PLANE_XY) {
       if (settings->feed_mode == INVERSE_TIME)
         inverse_time_rate_straight(c[0], c[1], p[2],
                                    AA_end, BB_end, CC_end,
                                    u_end, v_end, w_end,
                                    block, settings);
       STRAIGHT_FEED(c[0], c[1], p[2],
                     AA_end, BB_end, CC_end, u_end, v_end, w_end);
    }
  } else
    ERM(NCE_BUG_CODE_NOT_G0_OR_G1);

  settings->cutter_comp_firstmove = OFF;
  if(settings->plane == CANON_PLANE_XZ) {
      settings->current_x = c[0];
      settings->current_y = p[2];
      settings->current_z = c[1];
      settings->program_x = p[0];
      settings->program_z = p[1];
      settings->program_y = p[2];
  } else if(settings->plane == CANON_PLANE_XY) {
      settings->current_x = c[0];
      settings->current_y = c[1];
      settings->current_z = p[2];
      settings->program_x = p[0];
      settings->program_y = p[1];
      settings->program_z = p[2];
  }
  return INTERP_OK;
}

/****************************************************************************/

/*! convert_straight_comp2

Returned Value: int
   If any of the following errors occur, this returns the error shown.
   Otherwise, it returns INTERP_OK.
   1. The compensation side is not RIGHT or LEFT:
      NCE_BUG_SIDE_NOT_RIGHT_OR_LEFT
   2. A concave corner is found:
      NCE_CONCAVE_CORNER_WITH_CUTTER_RADIUS_COMP

Side effects:
   This executes a STRAIGHT_FEED command at cutting feed rate
   or a STRAIGHT_TRAVERSE command.
   It also generates an ARC_FEED to go around a corner, if necessary.
   It also updates the setting of the position of the tool point to
   the end point of the move and updates the programmed point.
   If INVERSE_TIME feed mode is in effect, it also calls SET_FEED_RATE
   and resets the feed rate in the machine model.

Called by: convert_straight.

This is called if cutter radius compensation is on and
settings->cutter_comp_firstmove is not ON, indicating that this is not
the first move after cutter radius compensation is turned on.

The algorithm used here is:
1. Determine the direction of the last motion. This is done by finding
   the direction of the line from the last programmed point to the
   current tool tip location. This line is a radius of the tool and is
   perpendicular to the direction of motion since the cutter is tangent
   to that direction.
2. Determine the direction of the programmed motion.
3. If there is a convex corner, insert an arc to go around the corner.
4. Find the destination point for the tool tip. The tool will be
   tangent to the line from the last programmed point to the present
   programmed point at the present programmed point.
5. Go in a straight line from the current tool tip location to the
   destination tool tip location.

This uses an angle tolerance of TOLERANCE_CONCAVE_CORNER (0.01 radian)
to determine if:
1) an illegal concave corner exists (tool will not fit into corner),
2) no arc is required to go around the corner (i.e. the current line
   is in the same direction as the end of the previous move), or
3) an arc is required to go around a convex corner and start off in
   a new direction.

If a rotary axis is moved in this block and an extra arc is required
to go around a sharp corner, all the rotary axis motion occurs on the
arc.  An alternative might be to distribute the rotary axis motion
over the arc and the straight move in proportion to their lengths.

If the Z-axis is moved in this block and an extra arc is required to
go around a sharp corner, all the Z-axis motion occurs on the straight
line and none on the extra arc.  An alternative might be to distribute
the Z-axis motion over the extra arc and the straight line in
proportion to their lengths.

This handles inverse time feed rates by computing the length of the
compensated path.

This handles the case of there being no XY motion.

This handles G0 moves. Where an arc is inserted to round a corner in a
G1 move, no arc is inserted for a G0 move; a STRAIGHT_TRAVERSE is made
from the current point to the end point. The end point for a G0
move is the same as the end point for a G1 move, however.

*/

int Interp::convert_straight_comp2(int move,     //!< either G_0 or G_1                        
                                   block_pointer block,  //!< pointer to a block of RS274 instructions 
                                   setup_pointer settings,       //!< pointer to machine settings              
                                   double px,    //!< X coordinate of programmed end point     
                                   double py,    //!< Y coordinate of programmed end point     
                                   double pz,    //!< Z coordinate of end point                
                                   double AA_end,        //!< A coordinate of end point
                                   double BB_end,        //!< B coordinate of end point
                                   double CC_end,        //!< C coordinate of end point
                                   double u_end, double v_end, double w_end)
{
  static char name[] = "convert_straight_comp2";
  double alpha;
  double beta;
  double end[2];                 /* x-coordinate of actual end point */
  double gamma;
  double mid[2];                 /* x-coordinate of end of added arc, if needed */
  double radius;
  int side;
  double small = TOLERANCE_CONCAVE_CORNER;      /* radians, testing corners */
  double start[2];      /* programmed beginning point */
  double theta;
  double p[3];  /* programmed endpoint */
  double c[2];  /* current */

  if(settings->plane == CANON_PLANE_XZ) {
      p[0] = px;
      p[1] = pz;
      p[2] = py;
      c[0] = settings->current_x;
      c[1] = settings->current_z;
      start[0] = settings->program_x;
      start[1] = settings->program_z;
      end[0] = settings->current_x;
      end[1] = settings->current_z;
  } else if (settings->plane == CANON_PLANE_XY) {
      p[0] = px;
      p[1] = py;
      p[2] = pz;
      c[0] = settings->current_x;
      c[1] = settings->current_y;
      start[0] = settings->program_x;
      start[1] = settings->program_y;
      end[0] = settings->current_x;
      end[1] = settings->current_y;
  } else ERM(NCE_RADIUS_COMP_ONLY_IN_XY_OR_XZ);


  if ((p[1] == start[1]) && (p[0] == start[0])) {     /* no XY motion */
    if (move == G_0) {
      if(settings->plane == CANON_PLANE_XZ) {
          STRAIGHT_TRAVERSE(xtrans(settings, end[0]), py, ztrans(settings, end[1]),
                            AA_end, BB_end, CC_end, u_end, v_end, w_end);
      } else if(settings->plane == CANON_PLANE_XY) {
          STRAIGHT_TRAVERSE(end[0], end[1], pz,
                            AA_end, BB_end, CC_end, u_end, v_end, w_end);
      }
    } else if (move == G_1) {
      if(settings->plane == CANON_PLANE_XZ) {
          if (settings->feed_mode == INVERSE_TIME)
            inverse_time_rate_straight(end[0], py, end[1],
                                       AA_end, BB_end, CC_end,
                                       u_end, v_end, w_end,
                                       block, settings);
          STRAIGHT_FEED(xtrans(settings, end[0]), py, ztrans(settings, end[1]),
                        AA_end, BB_end, CC_end, u_end, v_end, w_end);
      } else if(settings->plane == CANON_PLANE_XY) {
          if (settings->feed_mode == INVERSE_TIME)
            inverse_time_rate_straight(end[0], end[1], pz,
                                       AA_end, BB_end, CC_end,
                                       u_end, v_end, w_end,
                                       block, settings);
          STRAIGHT_FEED(end[0], end[1], pz,
                        AA_end, BB_end, CC_end, u_end, v_end, w_end);
      }
    } else
      ERM(NCE_BUG_CODE_NOT_G0_OR_G1);
  } else {
    side = settings->cutter_comp_side;
    radius = settings->cutter_comp_radius;      /* will always be positive */
    theta = atan2(end[1] - start[1],
                  end[0] - start[0]);
    alpha = atan2(p[1] - start[1], p[0] - start[0]);

    if (side == LEFT) {
      if (theta < alpha)
        theta = (theta + (2 * M_PIl));
      beta = ((theta - alpha) - M_PI_2l);
      gamma = M_PI_2l;
    } else if (side == RIGHT) {
      if (alpha < theta)
        alpha = (alpha + (2 * M_PIl));
      beta = ((alpha - theta) - M_PI_2l);
      gamma = -M_PI_2l;
    } else
      ERM(NCE_BUG_SIDE_NOT_RIGHT_OR_LEFT);
    end[0] = (p[0] + (radius * cos(alpha + gamma)));
    end[1] = (p[1] + (radius * sin(alpha + gamma)));
    mid[0] = (start[0] + (radius * cos(alpha + gamma)));
    mid[1] = (start[1] + (radius * sin(alpha + gamma)));

    CHK(((beta < -small) || (beta > (M_PIl + small))),
        NCE_CONCAVE_CORNER_WITH_CUTTER_RADIUS_COMP);
    if (move == G_0) {
      if(settings->plane == CANON_PLANE_XZ) {
          STRAIGHT_TRAVERSE(xtrans(settings, end[0]), py, ztrans(settings, end[1]),
                            AA_end, BB_end, CC_end, u_end, v_end, w_end);
      }
      else if(settings->plane == CANON_PLANE_XY) {
          STRAIGHT_TRAVERSE(end[0], end[1], pz,
                            AA_end, BB_end, CC_end, u_end, v_end, w_end);
      }
    }
    else if (move == G_1) {
      if (beta > small) {       /* ARC NEEDED */
        if(settings->plane == CANON_PLANE_XZ) {
            if (settings->feed_mode == INVERSE_TIME)
              inverse_time_rate_as(start[0], start[1],
                                   (side == LEFT) ? -1 : 1, mid[0],
                                   mid[1], end[0], p[2], end[1],
                                   AA_end, BB_end, CC_end,
                                   u_end, v_end, w_end,
                                   block, settings);
            ARC_FEED(ztrans(settings, mid[1]), xtrans(settings, mid[0]), ztrans(settings, start[1]), xtrans(settings, start[0]),
                     ((side == LEFT) ? 1 : -1), settings->current_y,
                     AA_end, BB_end, CC_end, u_end, v_end, w_end);
            STRAIGHT_FEED(xtrans(settings, end[0]), p[2], ztrans(settings, end[1]),
                          AA_end, BB_end, CC_end, u_end, v_end, w_end);
        }
        else if(settings->plane == CANON_PLANE_XY) {
            if (settings->feed_mode == INVERSE_TIME)
              inverse_time_rate_as(start[0], start[1],
                                   (side == LEFT) ? -1 : 1, mid[0],
                                   mid[1], end[0], end[1], p[2],
                                   AA_end, BB_end, CC_end,
                                   u_end, v_end, w_end,
                                   block, settings);
            ARC_FEED(mid[0], mid[1], start[0], start[1],
                     ((side == LEFT) ? -1 : 1), settings->current_z,
                     AA_end, BB_end, CC_end, u_end, v_end, w_end);
            STRAIGHT_FEED(end[0], end[1], p[2],
                          AA_end, BB_end, CC_end, u_end, v_end, w_end);
        }
      } else {
         if(settings->plane == CANON_PLANE_XZ) {
            if (settings->feed_mode == INVERSE_TIME)
              inverse_time_rate_straight(end[0], p[2], end[1],
                                         AA_end, BB_end, CC_end,
                                         u_end, v_end, w_end,
                                         block, settings);
            STRAIGHT_FEED(xtrans(settings, end[0]), p[2], ztrans(settings, end[1]),
                          AA_end, BB_end, CC_end, u_end, v_end, w_end);
         } else if(settings->plane == CANON_PLANE_XY) {
            if (settings->feed_mode == INVERSE_TIME)
              inverse_time_rate_straight(end[0], end[1], p[2],
                                         AA_end, BB_end, CC_end,
                                         u_end, v_end, w_end,
                                         block, settings);
            STRAIGHT_FEED(end[0], end[1], p[2],
                          AA_end, BB_end, CC_end, u_end, v_end, w_end);
         }
      }
    } else
      ERM(NCE_BUG_CODE_NOT_G0_OR_G1);
  }

  if(settings->plane == CANON_PLANE_XZ) {
      settings->current_x = end[0];
      settings->current_z = end[1];
      settings->current_y = p[2];
      settings->program_x = p[0];
      settings->program_z = p[1];
      settings->program_y = p[2];
  } else if(settings->plane == CANON_PLANE_XY) {
      settings->current_x = end[0];
      settings->current_y = end[1];
      settings->current_z = p[2];
      settings->program_x = p[0];
      settings->program_y = p[1];
      settings->program_z = p[2];
  }
  return INTERP_OK;
}

/****************************************************************************/

/*! convert_tool_change

Returned Value: int (INTERP_OK)

Side effects:
   This makes function calls to canonical machining functions, and sets
   the machine model as described below.

Called by: convert_m

This function carries out an m6 command, which changes the tool in the
spindle. The only function call this makes is to the CHANGE_TOOL
function. The semantics of this function call is that when it is
completely carried out, the tool that was selected is in the spindle,
the tool that was in the spindle (if any) is returned to its changer
slot, the spindle will be stopped (but the spindle speed setting will
not have changed) and the x, y, z, a, b, and c positions will be the same
as they were before (although they may have moved around during the
change).

It would be nice to add more flexibility to this function by allowing
more changes to occur (position changes, for example) as a result of
the tool change. There are at least two ways of doing this:

1. Require that certain machine settings always have a given fixed
value after a tool change (which may be different from what the value
was before the change), and record the fixed values somewhere (in the
world model that is read at initialization, perhaps) so that this
function can retrieve them and reset any settings that have changed.
Fixed values could even be hard coded in this function.

2. Allow the executor of the CHANGE_TOOL function to change the state
of the world however it pleases, and have the interpreter read the
executor's world model after the CHANGE_TOOL function is carried out.
Implementing this would require a change in other parts of the EMC
system, since calls to the interpreter would then have to be
interleaved with execution of the function calls output by the
interpreter.

There may be other commands in the block that includes the tool change.
They will be executed in the order described in execute_block.

This implements the "Next tool in T word" approach to tool selection.
The tool is selected when the T word is read (and the carousel may
move at that time) but is changed when M6 is read.

Note that if a different tool is put into the spindle, the current_z
location setting may be incorrect for a time. It is assumed the
program will contain an appropriate USE_TOOL_LENGTH_OFFSET command
near the CHANGE_TOOL command, so that the incorrect setting is only
temporary.

In [NCMS, page 73, 74] there are three other legal approaches in addition
to this one.

*/

int Interp::convert_tool_change(setup_pointer settings)  //!< pointer to machine settings
{
  static char name[] = "convert_tool_change";
  if (settings->selected_tool_slot < 0) 
    ERM(NCE_TXX_MISSING_FOR_M6);
  STOP_SPINDLE_TURNING();
  CHANGE_TOOL(settings->selected_tool_slot);
  settings->current_slot = settings->selected_tool_slot;
  settings->spindle_turning = CANON_STOPPED;
  // tool change can move the controlled point.  reread it:
  settings->toolchange_flag = ON; 
  return INTERP_OK;
}

/****************************************************************************/

/*! convert_tool_length_offset

Returned Value: int
   If any of the following errors occur, this returns the error code shown.
   Otherwise, it returns INTERP_OK.
   1. The block has no offset index (h number): NCE_OFFSET_INDEX_MISSING
   2. The g_code argument is not G_43 or G_49:
      NCE_BUG_CODE_NOT_G43_OR_G49

Side effects:
   A USE_TOOL_LENGTH_OFFSET function call is made. Current_z,
   tool_length_offset, and length_offset_index are reset.

Called by: convert_g

This is called to execute g43 or g49.

The g49 RS274/NGC command translates into a USE_TOOL_LENGTH_OFFSET(0.0)
function call.

The g43 RS274/NGC command translates into a USE_TOOL_LENGTH_OFFSET(length)
function call, where length is the value of the entry in the tool length
offset table whose index is the H number in the block.

The H number in the block (if present) was checked for being a non-negative
integer when it was read, so that check does not need to be repeated.

*/

int Interp::convert_tool_length_offset(int g_code,       //!< g_code being executed (must be G_43 or G_49)
                                      block_pointer block,      //!< pointer to a block of RS274/NGC instructions
                                      setup_pointer settings)   //!< pointer to machine settings                 
{
  static char name[] = "convert_tool_length_offset";
  int index;
  double xoffset, zoffset;

  if (g_code == G_49) {
    xoffset = 0;
    zoffset = 0;
    index = 0;
  } else if (g_code == G_43) {
    CHK((block->h_flag == OFF && !settings->current_slot), 
        NCE_OFFSET_INDEX_MISSING);
    index = block->h_flag == ON? block->h_number: settings->current_slot;
    xoffset = USER_TO_PROGRAM_LEN(settings->tool_table[index].xoffset);
    zoffset = USER_TO_PROGRAM_LEN(settings->tool_table[index].zoffset);
  } else if (g_code == G_43_1) {
    CHK((block->x_flag == ON) ||
        (block->y_flag == ON) ||
        (block->z_flag == ON) ||
        (block->a_flag == ON) ||
        (block->b_flag == ON) ||
        (block->c_flag == ON) ||
        (block->j_flag == ON),
        NCE_XYZABCJ_WORDS_NOT_ALLOWED_WITH_G43H_1_G41R_OR_G42R);
    xoffset = settings->tool_xoffset;
    zoffset = settings->tool_zoffset;
    index = -1;
    if(block->i_flag == ON) xoffset = block->i_number;
    if(block->k_flag == ON) zoffset = block->k_number;
  } else {
    ERS("BUG: Code not G43, G43.1, or G49");
  }
  USE_TOOL_LENGTH_OFFSET(xoffset, zoffset);
  settings->current_x += settings->tool_xoffset - xoffset;
  settings->current_z += settings->tool_zoffset - zoffset;
  settings->tool_xoffset = xoffset;
  settings->tool_zoffset = zoffset;
  settings->tool_offset_index = index;
  return INTERP_OK;
}

/****************************************************************************/

/*! convert_tool_select

Returned Value: int
   If the tool slot given in the block is larger than allowed,
   this returns NCE_SELECTED_TOOL_SLOT_NUMBER_TOO_LARGE.
   Otherwise, it returns INTERP_OK.

Side effects: See below

Called by: execute_block

A select tool command is given, which causes the changer chain to move
so that the slot with the t_number given in the block is next to the
tool changer, ready for a tool change.  The
settings->selected_tool_slot is set to the given slot.

An alternative in this function is to select by tool id. This was used
in the K&T and VGER interpreters. It is easy to code.

A check that the t_number is not negative has already been made in read_t.
A zero t_number is allowed and means no tool should be selected.

*/

int Interp::convert_tool_select(block_pointer block,     //!< pointer to a block of RS274 instructions
                               setup_pointer settings)  //!< pointer to machine settings             
{
  static char name[] = "convert_tool_select";

  CHK((block->t_number > settings->tool_max),
      NCE_SELECTED_TOOL_SLOT_NUMBER_TOO_LARGE);
  SELECT_TOOL(block->t_number);
  settings->selected_tool_slot = block->t_number;
  return INTERP_OK;
}
