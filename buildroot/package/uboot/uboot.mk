#############################################################
#
# EMC2
#
#############################################################
ifeq ($(BR2_TARGET_MINIEMC2_MINI2440),y)
EMC2_UBOOT_DIR:=$(TOPDIR)/../bootloaders/mini2440
BOOT_CONFIG:=mini2440_config
else
EMC2_UBOOT_DIR:=$(TOPDIR)/../bootloaders/mini2416
BOOT_CONFIG:=smdk2416_config
endif

$(EMC2_UBOOT_DIR)/.configured:
	( cd $(EMC2_UBOOT_DIR); \
	export CROSS_COMPILE=$(BR2_TOOLCHAIN_EXTERNAL_PATH)/bin/$(BR2_TOOLCHAIN_EXTERNAL_PREFIX)- ; \
	make $(BOOT_CONFIG) \
	)
	touch $@


$(BINARIES_DIR)/u-boot.bin: $(EMC2_UBOOT_DIR)/.configured
	( cd $(EMC2_UBOOT_DIR); \
	export CROSS_COMPILE=$(BR2_TOOLCHAIN_EXTERNAL_PATH)/bin/$(BR2_TOOLCHAIN_EXTERNAL_PREFIX)- ; \
	make; \
	cp -f ./u-boot.bin $(BINARIES_DIR)/ \
	)
ifeq ($(BR2_TARGET_MINIEMC2_MINI2416),y)
	cd $(EMC2_UBOOT_DIR) && ./mkmovi && cp -f ./u-boot-movi.bin $(BINARIES_DIR)/
endif

uboot: busybox $(BINARIES_DIR)/u-boot.bin


uboot-clean:
	$(MAKE) -C $(EMC2_UBOOT_DIR) clean

uboot-dirclean:
	rm -rf $(EMC2_UBOOT_DIR)

ifeq ($(BR2_PACKAGE_EMC2_UBOOT),y)
TARGETS+=uboot
endif
