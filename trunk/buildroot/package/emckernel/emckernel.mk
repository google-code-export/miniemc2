#############################################################
#
# EMC2
#
#############################################################
ifeq ($(BR2_TARGET_MINIEMC2_MINI2440),y)
EMC2_KERNEL_DIR:=$(TOPDIR)/../kernel/linux-2.6.35.9
DEFCONFIG:=miniemc_defconfig
UIMAGE_CMD:=$(TOPDIR)/../bootloaders/mini2440/tools/mkimage -A arm -O linux -T kernel -C none -a 0x30008000 -e 0x30008000 -d ./arch/arm/boot/zImage  $(BINARIES_DIR)/uImage 
endif

ifeq ($(BR2_TARGET_MINIEMC2_MINI2416),y)
EMC2_KERNEL_DIR:=$(TOPDIR)/../kernel/linux-2.6.37.6
DEFCONFIG:=mini2416_defconfig
UIMAGE_CMD:=cp $(EMC2_KERNEL_DIR)/arch/arm/boot/zImage $(BINARIES_DIR)/
endif

$(EMC2_KERNEL_DIR)/.configured:
	(   cd $(EMC2_KERNEL_DIR); \
	    export ARCH=arm; \
	    export CROSS_COMPILE=$(BR2_TOOLCHAIN_EXTERNAL_PATH)/bin/$(BR2_TOOLCHAIN_EXTERNAL_PREFIX)- ; \
	    make $(DEFCONFIG); \
	)
	touch $@

$(EMC2_KERNEL_DIR)/.built: $(EMC2_KERNEL_DIR)/.configured
	(   cd $(EMC2_KERNEL_DIR); \
	    export ARCH=arm; \
	    export CROSS_COMPILE=$(BR2_TOOLCHAIN_EXTERNAL_PATH)/bin/$(BR2_TOOLCHAIN_EXTERNAL_PREFIX)- ; \
	    make zImage; \
	    $(UIMAGE_CMD) \
	)

emckernel: uboot $(EMC2_KERNEL_DIR)/.built


emckernel-clean:
	$(MAKE) -C $(EMC2_KERNEL_DIR) clean

emckernel-dirclean:
	rm -rf $(EMC2_KERNEL_DIR)

ifeq ($(BR2_PACKAGE_EMC2_KERNEL),y)
TARGETS+=emckernel
endif
