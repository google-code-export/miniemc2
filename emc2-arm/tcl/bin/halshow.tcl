#!/bin/sh
# the next line restarts using emcsh \
exec $EMC2_EMCSH "$0" "$@"

###############################################################
# Description:  halconfig.tcl
#               This file, shows a running hal configuration
#               and has menu for modifying and tuning
#
#  Author: Raymond E Henry
#  License: GPL Version 2
#
#  Copyright (c) 2006 All rights reserved.
#
#  Last change:
# $Revision: 1.12 $
# $Author: jepler $
# $Date: 2007/03/24 13:56:37 $
###############################################################
# FIXME -- empty mod entry widgets after execute
# FIXME -- please hal param naming conventions aren't
###############################################################

# Load the emc.tcl file, which defines variables for various useful paths
source [file join [file dirname [info script]] .. emc.tcl]

package require BWidget

# add a few default characteristics to the display
foreach class { Button Checkbutton Entry Label Listbox Menu Menubutton \
    Message Radiobutton Scale } {
    option add *$class.borderWidth 1  100
}

#----------start toplevel----------
#

wm title . [msgcat::mc "HAL Configuration"]
wm protocol . WM_DELETE_WINDOW tk_
set masterwidth 700
set masterheight 475
# set fixed size for configuration display and center
set xmax [winfo screenwidth .]
set ymax [winfo screenheight .]
set x [expr ($xmax - $masterwidth )  / 2 ]
set y [expr ($ymax - $masterheight )  / 2]
wm geometry . "${masterwidth}x${masterheight}+$x+$y"

# trap mouse click on window manager delete and ask to save
wm protocol . WM_DELETE_WINDOW askKill
proc askKill {} {
    killHalConfig
}

# clean up a possible problems during shutdown
proc killHalConfig {} {
    global fid
    if {[info exists fid] && $fid != ""} {
        catch flush $fid
        catch close $fid
    }
    destroy .
    exit
}

set main [frame .main -padx 10 -pady 10]
pack $main -fill both -expand yes

# build frames from left side
set tf [frame $main.maint]
set top [NoteBook $main.note]

# Each mode has a unique set of widgets inside tab
set showhal [$top insert 0 ps -text [msgcat::mc " SHOW "] -raisecmd {showMode showhal} ]
set watchhal [$top insert 1 pw -text [msgcat::mc " WATCH "] -raisecmd {showMode watchhal}]

# use place manager to fix locations of frames within top
place configure $tf -in $main -x 0 -y 0 -relheight 1 -relwidth .3
place configure $top -in $main -relx .3 -y 0 -relheight 1 -relwidth .7

# slider process is used for several widgets
proc sSlide {f a b} {
    $f.sc set $a $b
}

# Build menu
# fixme clean up the underlines so they are unique under each set
set menubar [menu $top.menubar -tearoff 0]
set viewmenu [menu $menubar.view -tearoff 0]
    $menubar add cascade -label [msgcat::mc "Tree View"] \
            -menu $viewmenu
        $viewmenu add command -label [msgcat::mc "Expand Tree"] \
            -command {showNode {open}}
        $viewmenu add command -label [msgcat::mc "Collapse Tree"] \
            -command {showNode {close}}
        $viewmenu add separator
        $viewmenu add command -label [msgcat::mc "Expand Pins"] \
            -command {showNode {pin}}
        $viewmenu add command -label [msgcat::mc "Expand Parameters"] \
            -command {showNode {param}}
        $viewmenu add command -label [msgcat::mc "Expand Signals"] \
            -command {showNode {sig}}
        $viewmenu add separator
        $viewmenu add command -label [msgcat::mc "Erase Watch"] \
            -command {watchReset all}
. configure -menu $menubar

# build the tree widgets left side
set treew [Tree $tf.t  -width 10 -yscrollcommand "sSlide $tf" ]
set str $tf.sc
scrollbar $str -orient vert -command "$treew yview"
pack $str -side right -fill y
pack $treew -side right -fill both -expand yes
$treew bindText <Button-1> {workMode   }

#----------tree widget handlers----------
# a global var -- treenodes -- holds the names of existing nodes
# nodenames are the text applied to the toplevel tree nodes
# they could be internationalized here but the international name
# must contain no whitespace.  I'm not certain how to do that.
set nodenames {Components Pins Parameters Signals Functions Threads}

# searchnames is the real name to be used to reference
set searchnames {comp pin param sig funct thread}
set signodes {X Y Z A "Spindle"}

set treenodes ""
proc refreshHAL {} {
    global treew treenodes oldvar
    set tmpnodes ""
    # look through tree for nodes that are displayed
    foreach node $treenodes {
        if {[$treew itemcget $node -open]} {
            lappend tmpnodes $node
        }
    }
    # clean out the old tree
    $treew delete [$treew nodes root]
    # reread hal and make new nodes
    listHAL
    # read opennodes and set tree state if they still exist
    foreach node $tmpnodes {
        if {[$treew exists $node]} {
            $treew opentree $node no
        }
    }
    showHAL $oldvar
}

# listhal gets $searchname stuff
# and calls makeNodeX with list of stuff found.
proc listHAL {} {
    global searchnames nodenames
    set i 0
    foreach node $searchnames {
        writeNode "$i root $node [lindex $nodenames $i] 1"
        set ${node}str [hal list $node]
        switch -- $node {
            pin {-}
            param {
                makeNodeP $node [set ${node}str]
            }
            sig {
                makeNodeSig $sigstr
            }
            comp {-}
            funct {-}
            thread {
                makeNodeOther $node [set ${node}str]
            }
            default {}
        }
    incr i
    }
}

proc makeNodeP {which pstring} {
    global treew 
    # make an array to hold position counts
    array set pcounts {1 1 2 1 3 1 4 1 5 1}
    # consider each listed element
    foreach p $pstring {
        set elementlist [split $p "." ]
        set lastnode [llength $elementlist]
        set i 1
        foreach element $elementlist {
            switch $i {
                1 {
                    set snode "$which+$element"
                    if {! [$treew exists "$snode"] } {
                        set leaf [expr $lastnode - 1]
                        set j [lindex [array get pcounts 1] end]
                        writeNode "$j $which $snode $element $leaf"
                        array set pcounts "1 [incr j] 2 1 3 1 4 1 5 1"
                        set j 0
                    }
                    set i 2
                }
                2 {
                    set ssnode "$snode.$element"
                    if {! [$treew exists "$ssnode"] } {
                        set leaf [expr $lastnode - 2]
                        set j [lindex [array get pcounts 2] end]
                        writeNode "$j $snode $ssnode $element $leaf"
                        array set pcounts "2 [incr j] 3 1 4 1 5 1"
                        set j 0
                    }
                    set i 3
                }
                3 {
                    set sssnode "$ssnode.$element"
                    if {! [$treew exists "$sssnode"] } {
                        set leaf [expr $lastnode - 3]
                        set j [lindex [array get pcounts 3] end]
                        writeNode "$j $ssnode $sssnode $element $leaf"
                        array set pcounts "3 [incr j] 4 1 5 1"
                        set j 0
                    }
                    set i 4
                }
                4 {
                    set ssssnode "$sssnode.$element"
                    if {! [$treew exists "$ssssnode"] } {
                        set leaf [expr $lastnode - 4]
                        set j [lindex [array get pcounts 4] end]
                        writeNode "$j $sssnode $ssssnode $element $leaf"
                        array set pcounts "4 [incr j] 5 1"
                        set j 0
                    }
                    set i 5
                }
                5 {
                    set sssssnode "$ssssnode.$element"
                    if {! [$treew exists "$sssssnode"] } {
                        set leaf [expr $lastnode - 5]
                        set j [lindex [array get pcounts 5] end]
                        writeNode "$j $ssssnode $sssssnode $element $leaf"
                        array set pcounts "5 [incr j]"
                        set j 0
                    }
                }
              # end of node making switch
            }
           # end of element foreach
         }
        # end of param foreach
    }
    # empty the counts array in preparation for next proc call
    array unset pcounts {}
}

# signal node assumes more about HAL than pins or params.
# For this reason the hard coded variable signodes
proc makeNodeSig {sigstring} {
    global treew signodes
    # build sublists dotstring, each signode element, and remainder
    foreach nodename $signodes {
        set nodesig$nodename ""
    }
    set dotsig ""
    set remainder ""
    foreach tmp $sigstring {
        set i 0
        if {[string match *.* $tmp]} {
            lappend dotsig $tmp
            set i 1
        }
   
        foreach nodename $signodes {
            if {$i == 0 && [string match *$nodename* $tmp]} {
                lappend nodesig$nodename $tmp
                set i 1
            }
        }
        if {$i == 0} {
            lappend remainder $tmp
        }
    }
    set i 0
    # build the signode named nodes and leaves
    foreach nodename $signodes {
        set tmpstring [set nodesig$nodename]
        if {$tmpstring != ""} {
            set snode "sig+$nodename"
            writeNode "$i sig $snode $nodename 1"
            incr i
            set j 0
            foreach tmp [set nodesig$nodename] {
                set ssnode sig+$tmp
                writeNode "$j $snode $ssnode $tmp 0"
                incr j
            }
        }
    }
    set j 0
    # build the linkpp based signals just below signode
    foreach tmp $dotsig {
        set tmplist [split $tmp "."]
        set tmpmain [lindex $tmplist 0]
        set tmpname [lindex $tmplist end] 
        set snode sig+$tmpmain
        if {! [$treew exists "$snode"] } {
            writeNode "$i sig $snode $tmpmain 1"
            incr i
        }
        set ssnode sig+$tmp
        writeNode "$j $snode $ssnode $tmp 0"
        incr j
    }
    # build the remaining leaves at the bottom of list
    foreach tmp $remainder {
        set snode sig+$tmp
        writeNode "$i sig $snode $tmp 0"
        incr i
    }

}

proc makeNodeOther {which otherstring} {
    global treew
    set i 0
    foreach element $otherstring {
        set snode "$which+$element"
        if {! [$treew exists "$snode"] } {
            set leaf 0
            writeNode "$i $which $snode $element $leaf"
        }
        incr i
    }
}

# writeNode handles tree node insertion for makeNodeX
# builds a global list -- treenodes -- but not leaves
proc writeNode {arg} {
    global treew treenodes
    scan $arg {%i %s %s %s %i} j base node name leaf
    $treew insert $j  $base  $node -text $name
    if {$leaf > 0} {
        lappend treenodes $node
    }
}

proc showNode {which} {
    global treew
    switch -- $which {
        open {-}
        close {
            foreach type {pin param sig} {
                $treew ${which}tree $type
            }
        }
        pin {-}
        param {-}
        sig {
            foreach type {pin param sig} {
                $treew closetree $type
            }
            $treew opentree $which
            $treew see $which
        }
        default {}
    }
    focus -force $treew
}

#
#----------end of tree building processes----------

set oldvar "zzz"
# build show mode right side
proc makeShow {} {
    global showhal disp showtext
    set what full
    if {$what == "full"} {
        set stf [frame $showhal.t]
        set disp [text $stf.tx  -wrap word -takefocus 0 -state disabled \
            -relief flat -borderwidth 0 -height 26 -yscrollcommand "sSlide $stf"]
        set stt $stf.sc
        scrollbar $stt -orient vert -command "$disp yview"
        pack $disp -side left -fill both -expand yes
        pack $stt -side left -fill y
        set seps [frame $showhal.sep -bg black -borderwidth 2]
        set cfent [frame $showhal.b]
        set lab [label $cfent.label -text [msgcat::mc "Test HAL command :"] ]
        set com [entry $cfent.entry -textvariable halcommand]
        bind $com <KeyPress-Return> {showEx $halcommand}
        set ex [button $cfent.execute -text [msgcat::mc "Execute"] \
            -command {showEx $halcommand} ]
        set showtext [text $showhal.txt -height 2  -borderwidth 2 -relief groove ]
        pack $lab -side left -padx 5 -pady 3 
        pack $com -side left -fill x -expand 1 -pady 3
        pack $ex -side left -padx 5 -pady 3
        pack $stf -side top -fill both -expand 1
        pack $cfent -side top -fill x -anchor w
        pack $seps -side top -fill x -anchor w
        pack $showtext -side top -fill both -anchor w
    }
}

proc makeWatch {} {
    global cisp watchhal
    set cisp [canvas $watchhal.c -yscrollcommand [list $watchhal.s set]]
    scrollbar $watchhal.s -command [list $cisp yview] -orient v
    pack $cisp -side left -fill both -expand yes
    pack $watchhal.s -side left -fill y -expand no
}

# showmode handles the tab selection of mode
proc showMode {mode} {
    global workmode
    set workmode $mode
    if {$mode=="watchhal"} {
        watchLoop
    }
}

# all clicks on tree node names go into workMode
# oldvar keeps the last HAL variable for refresh
proc workMode {which} {
    global workmode oldvar thisvar newmodvar
    set thisvar $which
    switch -- $workmode {
        showhal {
            showHAL $which
        }
        watchhal {
            watchHAL $which
        }
        default {
            showMode showhal
            displayThis "Mode went way wrong."
        }
    }
    set oldvar $which
}

# process uses it's own halcmd show so that displayed
# info looks like what is in the Hal_Introduction.pdf
proc showHAL {which} {
    global disp
    if {![info exists disp]} {return}
    if {$which == "zzz"} {
        displayThis [msgcat::mc "Select a node to show."]
        return
    }
    set thisnode $which
    set thislist [split $which "+"]
    set searchbase [lindex $thislist 0]
    set searchstring [lindex $thislist 1]
    set thisret [hal show $searchbase $searchstring]
    displayThis $thisret
}

proc showEx {what} {
    global showtext
    set str [eval hal $what]
    $showtext delete 1.0 end
    $showtext insert end $str
    refreshHAL
}

set watchlist ""
set watchstring ""
proc watchHAL {which} {
    global watchlist watchstring watching cisp
    if {$which == "zzz"} {
        $cisp create text 40 [expr 1 * 20 + 12] -anchor w -tag firstmessage\
            -text [msgcat::mc "<-- Select a Leaf.  Click on its name."]
        set watchlist ""
        set watchstring ""
        return
    } else {
        $cisp delete firstmessage
    }
    # return if variable is already used.
    if {[lsearch $watchlist $which] != -1} {
        return
    }
    lappend watchlist $which
    set i [llength $watchlist]
    set label [lindex [split $which +] end]
    set tmplist [split $which +]
    set vartype [lindex $tmplist 0]
    if {$vartype != "pin" && $vartype != "param" && $vartype != "sig"} {
	# cannot watch components, functions, or threads
	return
    }
    set varname [lindex $tmplist end]
    if {$vartype == "sig"} {
	# stype (and gets) fail when the item clicked is not a leaf
	# e.g., clicking "Signals / X"
	if {[catch {hal stype $varname} type]} { return }
    } else {
	# ptype (and getp) fail when the item clicked is not a leaf
	# e.g., clicking "Pins / axis / 0"
	if {[catch {hal ptype $varname} type]} { return }
    }
    if {$type == "bit"} {
        $cisp create oval 20 [expr $i * 20 + 5] 35 [expr $i * 20 + 20] \
            -fill firebrick4 -tag oval$i
        $cisp create text 80 [expr $i * 20 + 12] -text $label \
            -anchor w -tag $label
    } else {
        # other gets a text display for value
        $cisp create text 10 [expr $i * 20 + 12] -text "" \
            -anchor w -tag text$i
        $cisp create text 80 [expr $i * 20 + 12] -text $label \
            -anchor w -tag $label
    }
    $cisp configure -scrollregion [$cisp bbox all]
    $cisp yview moveto 1.0
    set tmplist [split $which +]
    set vartype [lindex $tmplist 0]
    set varname [lindex $tmplist end]
    lappend watchstring "$i $vartype $varname "
    if {$watching == 0} {watchLoop} 
}

# watchHAL prepares a string of {i HALtype name} sets
# watchLoop submits these to halcmd and sets canvas
# color or value based on reply
set watching 0
proc watchLoop {} {
    global cisp watchstring watching workmode
    set watching 1
    set which $watchstring
    foreach var $which {
        scan $var {%i %s %s} cnum vartype varname
        if {$vartype == "sig" } {
            set ret [hal gets $varname]
        } else {
            set ret [hal getp $varname]
        }
        if {$ret == "TRUE"} {
            $cisp itemconfigure oval$cnum -fill yellow
        } elseif {$ret == "FALSE"} {
            $cisp itemconfigure oval$cnum -fill firebrick4
        } else {
            set value [expr $ret]
            $cisp itemconfigure text$cnum -text $value
        }
    }
    if {$workmode == "watchhal"} {
        after 250 watchLoop
    } else {
        set watching 0
    }
}

proc watchReset {del} {
    global watchlist cisp
    $cisp delete all
    switch -- $del {
        all {
            watchHAL zzz
            return
        }
        default {
            set place [lsearch $watchlist $del]
            if {$place != -1 } {
            set watchlist [lreplace $watchlist $place]
                foreach var $watchlist {
                    watchHAL $var
                }
            } else {            
                watchHAL zzz
            }
        }
    }
}

# proc switches the insert and removal of upper right text
# This also removes any modify array variables
proc displayThis {str} {
    global disp
    $disp configure -state normal 
    $disp delete 1.0 end
    $disp insert end $str
    $disp configure -state disabled
}

#----------start up the displays----------
makeShow
makeWatch
refreshHAL
$top raise ps

set firststr [msgcat::mc "Commands may be tested here but they will NOT be saved"]

$showtext delete 1.0 end
$showtext insert end $firststr

