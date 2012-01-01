#    This is a component of AXIS, a front-end for emc
#    Copyright 2004, 2005, 2006 Jeff Epler <jepler@unpythonic.net>
#
#    This program is free software; you can redistribute it and/or modify
#    it under the terms of the GNU General Public License as published by
#    the Free Software Foundation; either version 2 of the License, or
#    (at your option) any later version.
#
#    This program is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    GNU General Public License for more details.
#
#    You should have received a copy of the GNU General Public License
#    along with this program; if not, write to the Free Software
#    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
import math

class Translated:
    offset_x = offset_y = offset_z = offset_a = offset_b = offset_c = 0
    def translate(self, x,y,z,a,b,c):
        return [x+self.offset_x, y+self.offset_y, z+self.offset_z,
            a+self.offset_a, b+self.offset_b, c+self.offset_c]

    def straight_traverse(self, *args):
        self.straight_traverse_translated(*self.translate(*args))
    def straight_feed(self, *args):
        self.straight_feed_translated(*self.translate(*args))
    def set_origin_offsets(self, offset_x, offset_y, offset_z, offset_a, offset_b, offset_c):
        self.offset_x = offset_x #- (self.ox - self.offset_x)
        self.offset_y = offset_y #- (self.oy - self.offset_y)
        self.offset_z = offset_z #- (self.oz - self.offset_z)

class ArcsToSegmentsMixin:
    plane = 1

    def set_plane(self, plane):
        self.plane = plane

    def arc_feed(self, x1, y1, cx, cy, rot, z1, a, b, c, u, v, w):
        if self.plane == 1:
            f = n = [x1+self.offset_x,y1+self.offset_y,z1+self.offset_z, a, b, c, 0, 0, 0]
            cx=cx+self.offset_x
            cy=cy+self.offset_y
            xyz = [0,1,2]
        elif self.plane == 3:
            f = n = [y1+self.offset_x,z1+self.offset_y,x1+self.offset_z, a, b, c, 0, 0, 0]
            cx=cx+self.offset_x
            cy=cy+self.offset_z
            xyz = [2,0,1]
        else:
            f = n = [z1+self.offset_x,x1+self.offset_y,y1+self.offset_z, a, b, c, 0, 0, 0]
            cx=cx+self.offset_y
            cy=cy+self.offset_z
            xyz = [1,2,0]
        ox, oy, oz = self.lo
        o = [ox, oy, oz, 0, 0, 0, 0, 0, 0]
        theta1 = math.atan2(o[xyz[1]]-cy, o[xyz[0]]-cx)
        theta2 = math.atan2(n[xyz[1]]-cy, n[xyz[0]]-cx)
        rad = math.hypot(o[xyz[0]]-cx, o[xyz[1]]-cy)

        if rot < 0:
            if theta2 >= theta1: theta2 -= math.pi * 2
        else:
            if theta2 <= theta1: theta2 += math.pi * 2

        def interp(low, high):
            return low + (high-low) * i / steps

        steps = max(8, int(128 * abs(theta1 - theta2) / math.pi))
        p = [0] * 9
        for i in range(1, steps):
            theta = interp(theta1, theta2)
            p[xyz[0]] = math.cos(theta) * rad + cx
            p[xyz[1]] = math.sin(theta) * rad + cy
            p[xyz[2]] = interp(o[xyz[2]], n[xyz[2]])
            p[3] = interp(o[3], n[3])
            p[4] = interp(o[4], n[4])
            p[5] = interp(o[5], n[5])
            p[6] = interp(o[6], n[6])
            p[7] = interp(o[7], n[7])
            p[8] = interp(o[8], n[8])
            self.straight_arcsegment(*p)
        self.straight_arcsegment(*n)

class PrintCanon:
    def set_origin_offsets(self, *args):
        print "set_origin_offsets", args

    def next_line(self, state):
        print "next_line", state.sequence_number
        self.state = state

    def set_plane(self, plane):
        print "set plane", plane

    def set_feed_rate(self, arg):
        print "set feed rate", arg

    def comment(self, arg):
        print "#", arg

    def straight_traverse(self, *args):
        print "straight_traverse %.4g %.4g %.4g  %.4g %.4g %.4g" % args

    def straight_feed(self, *args):
        print "straight_feed %.4g %.4g %.4g  %.4g %.4g %.4g" % args

    def dwell(self, arg):
        if arg < .1:
            print "dwell %f ms" % (1000 * arg)
        else:
            print "dwell %f seconds" % arg

    def arc_feed(self, *args):
        print "arc_feed %.4g %.4g  %.4g %.4g %.4g  %.4g  %.4g %.4g %.4g" % args

# vim:ts=8:sts=4:et:
