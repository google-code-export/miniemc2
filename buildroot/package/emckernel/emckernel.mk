#############################################################
#
# EMC2
#
#############################################################
EMC2_KERNEL_DIR:=$(TOPDIR)/../kernel/linux-2.6.35.9

$(EMC2_KERNEL_DIR)/.configured:
	(   cd $(EMC2_KERNEL_DIR); \
	    export ARCH=arm; \
	    export CROSS_COMPILE=$(BR2_TOOLCHAIN_EXTERNAL_PATH)/bin/$(BR2_TOOLCHAIN_EXTERNAL_PREFIX)- ; \
	    make miniemc_defconfig; \
	)
	touch $@

$(EMC2_KERNEL_DIR)/.built: $(EMC2_KERNEL_DIR)/.configured
	(   cd $(EMC2_KERNEL_DIR); \
	    export ARCH=arm; \
	    export CROSS_COMPILE=$(BR2_TOOLCHAIN_EXTERNAL_PATH)/bin/$(BR2_TOOLCHAIN_EXTERNAL_PREFIX)- ; \
	    make zImage; \
	$(TOPDIR)/../uboot/tools/mkimage -A arm -O linux -T kernel -C none -a 0x30008000 -e 0x30008000 -d ./arch/arm/boot/zImage  $(BINARIES_DIR)/uImage \
	)

emckernel: uboot $(EMC2_KERNEL_DIR)/.built


emckernel-clean:
	$(MAKE) -C $(EMC2_KERNEL_DIR) clean

emckernel-dirclean:
	rm -rf $(EMC2_KERNEL_DIR)

ifeq ($(BR2_PACKAGE_EMC2_KERNEL),y)
TARGETS+=emckernel
endif
