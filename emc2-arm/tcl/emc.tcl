namespace eval emc {
    variable HOME /home/emc2
    variable BIN_DIR /home/emc2/bin
    variable TCL_DIR /home/emc2/tcl
    variable TCL_LIB_DIR 
    variable TCL_BIN_DIR /home/emc2/tcl/bin
    variable TCL_SCRIPT_DIR /home/emc2/tcl/scripts
    variable HELP_DIR /home/emc2/docs/help
    variable RTLIB_DIR /home/emc2/rtlib
    variable CONFIG_PATH {~/emc2/configs:/home/emc2/configs}
    variable NCFILES_DIR /home/emc2/nc_files
    variable LANG_DIR /home/emc2/src/po
    variable IMAGEDIR /home/emc2
    variable REALTIME /home/emc2/scripts/realtime
    variable SIMULATOR yes
    variable _langinit 1
}

if {[string first $::emc::BIN_DIR: $env(PATH)] != 0} {
    set env(PATH) $::emc::BIN_DIR:$env(PATH)
}

proc emc::image_search i {
    set paths "$emc::IMAGEDIR $emc::HOME $emc::HOME/etc/emc2 /etc/emc2 ."
    foreach f $paths {
        if [file exists $f/$i] {
            return [image create photo -file $f/$i]
        }
        if [file exists $f/$i.gif] {
            return [image create photo -file $f/$i.gif]
        }
    }
    error "image $i is not available"
}

# Arrange to load hal.so when the 'hal' command is requested
proc hal {args} {
    load $::emc::TCL_LIB_DIR/hal.so
    eval hal $args
}

# Internationalisation (i18n)
# in order to use i18n, all the strings will be called [msgcat::mc "string-foo"]
# instead of "string-foo".
# Thus msgcat searches for a translation of the string, and in case one isn't 
# found, the original string is used.
# In order to properly use locale's the env variable LANG is queried.
# If LANG is defined, then the folder src/po is searched for files
# called *.msg, (e.g. en_US.msg).
package require msgcat
if {$emc::_langinit && [info exists env(LANG)]} {
    msgcat::mclocale $env(LANG)
    msgcat::mcload $emc::LANG_DIR
    set emc::_langinit 0
}

proc emc::standard_font_size {} {
    if {[info exists ::emc::standard_font]} { return $::emc::standard_font }
    set res1 [catch {exec gconftool -g /desktop/gnome/font_rendering/dpi} gnome_dpi]
    set res2 [catch {exec xlsfonts -fn -adobe-helvetica-medium-r-normal--*-*-*-*-p-*-iso10646-1} fonts]
    if {$res1 == 0 && $res2 == 0} {
        set pixels [expr {$gnome_dpi / 8.}]
        set min_diff [expr .2*$pixels]
        set best_size [expr {int($pixels)}]
        foreach f $fonts {
            regexp -- {-.*?-.*?-.*?-.*?-.*?-.*?-(.*?)-.*} $f _ sz
            set diff [expr {abs($pixels - $sz)}]
            if {$diff < $min_diff} {
                set min_diff $diff
                set best_size $sz
            }
        }
        return -$best_size
        set ::emc::standard_font_size -$best_size
    } else {
        set ::emc::standard_font_size 12
    }
}

proc emc::standard_font_family {} {
    return helvetica
}

proc emc::standard_font {} {
    list [standard_font_family] [standard_font_size]
}

proc emc::standard_fixed_font_family {} {
    return courier
}

proc emc::standard_fixed_font {} {
    set sz [standard_font_size]
    if {$sz >= -12} { return fixed }
    list [standard_fixed_font_family] [standard_font_size]
}
