obj-m	:= modminiemc.o
INCLUDE	:= -I$(KERNEL_DIR)/include
PWD		:= $(shell pwd)

all: Release

Release:
	$(MAKE) -C $(KERNEL_DIR) $(INCLUDE) SUBDIRS=$(PWD) modules

clean: cleanRelease

cleanRelease::
	$(RM) .modminiemc.* *.cmd *.o *.ko *.mod.c modtest
	$(RM) -R .tmp* *.symvers *.order
