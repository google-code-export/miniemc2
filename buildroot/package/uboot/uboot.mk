#############################################################
#
# EMC2
#
#############################################################
EMC2_UBOOT_SVN:=file:///home/ksu/repository/miniemc/uboot/u-boot-1.3.2
EMC2_UBOOT_DIR:=$(TOPDIR)/../uboot


$(EMC2_UBOOT_DIR)/.built:
	( cd $(EMC2_UBOOT_DIR); \
	export CROSS_COMPILE=$(BR2_TOOLCHAIN_EXTERNAL_PATH)/bin/$(BR2_TOOLCHAIN_EXTERNAL_PREFIX)- ; \
	make mini2440_config; \
	make; \
	cp -f ./u-boot.bin $(BINARIES_DIR)/ \
	)

uboot: busybox $(EMC2_UBOOT_DIR)/.built


uboot-clean:
	$(MAKE) -C $(EMC2_UBOOT_DIR) clean

uboot-dirclean:
	rm -rf $(EMC2_UBOOT_DIR)

ifeq ($(BR2_PACKAGE_EMC2_UBOOT),y)
TARGETS+=uboot
endif
