#############################################################
#
# EMC2 modminiemc
#
#############################################################
EMC2_MODMINIEMC_DIR:=$(TOPDIR)/../modules/modminiemc
EMC2_KERNEL_DIR:=$(TOPDIR)/../kernel/linux-2.6.35.9

$(EMC2_MODMINIEMC_DIR)/.built:
	(cd $(EMC2_MODMINIEMC_DIR); \
	 ARCH=arm \
	 CROSS_COMPILE=$(BR2_TOOLCHAIN_EXTERNAL_PATH)/bin/$(BR2_TOOLCHAIN_EXTERNAL_PREFIX)-  \
	 KERNEL_DIR=$(EMC2_KERNEL_DIR) \
	 make )
	mkdir -p $(TARGET_DIR)/lib/modules
	cp -f $(EMC2_MODMINIEMC_DIR)/modminiemc.ko $(TARGET_DIR)/lib/modules/

modminiemc: emckernel $(EMC2_MODMINIEMC_DIR)/.built


modminiemc-clean:
	$(MAKE) -C $(EMC2_MODMINIEMC_DIR) clean

modminiemc-dirclean:
	rm -rf $(EMC2_MODMINIEMC_DIR)

ifeq ($(BR2_PACKAGE_EMC2_MODMINIEMC),y)
TARGETS+=modminiemc
endif
