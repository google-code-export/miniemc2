#
# (C) Copyright 2002
# Gary Jennejohn, DENX Software Engineering, <gj@denx.de>
# David Mueller, ELSOFT AG, <d.mueller@elsoft.ch>
#
# SAMSUNG SMDKC100 board with mDirac3 (ARM-CortexA8) cpu
#
# see http://www.samsung.com/ for more information on SAMSUNG
#

#
# SMDKC100 has a 128 MB SDR SDRAM
#
#
# Linux-Kernel is expected to be at 5000'8000, entry 5000'8000
# optionally with a ramdisk at 5080'0000
#
# we load ourself to 57e0'0000 without MMU
# with MMU, load address is changed to 0xc7e0_0000
#
# download area is 5000'0000
#


ifndef TEXT_BASE

ifdef CONFIG_S5PC100_EVT1
TEXT_BASE = 0x27e00000
else
TEXT_BASE = 0xc7e00000
endif

endif

