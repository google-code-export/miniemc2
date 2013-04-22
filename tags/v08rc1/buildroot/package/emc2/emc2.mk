#############################################################
#
# EMC2
#
#############################################################
EMC2_SVN:=file:///home/ksu/repository/miniemc/emc-arm-xen
EMC2_DIR:=$(TOPDIR)/../emc2-arm
EMC2_SRC:=emc-arm.tar.gz
ifeq ($(BR2_TARGET_MINIEMC2_MINI2416),y)
EMC2_ARCHOPTS="-I$(EMC2_KERNEL_DIR) -DCONFIG_MARCH_MINI2416"
else
EMC2_ARCHOPTS=-I$(EMC2_KERNEL_DIR)
endif

ARCHOPTS=-I$(EMC2_KERNEL_DIR) \



$(EMC2_DIR)/.configure: 
	( cd $(EMC2_DIR)/src; \
	$(TARGET_CONFIGURE_OPTS) \
	$(TARGET_CONFIGURE_ARGS) \
	PTH_CONFIG=$(BUILD_DIR)/xenomai-2.5.6/install/bin/xeno-config \
	ARCHOPTS=$(EMC2_ARCHOPTS) \
	EMC2_HOME=/home/emc2 \
	./configure \
		--without-x  --enable-simulator --enable-run-in-place \
		--host=arm-linux --disable-python  --prefix=/home/emc2 \
	)
	touch $@
	
$(EMC2_DIR)/.built: $(EMC2_DIR)/.configure
	(cd $(EMC2_DIR)/src; export PATH=$(PATH):$(BR2_TOOLCHAIN_EXTERNAL_PATH)/bin/; make || echo Done )


$(EMC2_DIR)/.copied: $(EMC2_DIR)/.built
	mkdir -p $(TARGET_DIR)/home/emc2 $(TARGET_DIR)/home/emc2/configs $(TARGET_DIR)/home/emc2/nc_files $(TARGET_DIR)/home/emc2/lib
	mkdir -p $(TARGET_DIR)/home/emc2/scripts $(TARGET_DIR)/home/emc2/bin $(TARGET_DIR)/home/emc2/rtlib
	cp -f $(EMC2_DIR)/nc_files/* $(TARGET_DIR)/home/emc2/nc_files
	cp -f $(EMC2_DIR)/rtlib/* $(TARGET_DIR)/home/emc2/rtlib
	cp -f $(EMC2_DIR)/scripts/emc_* $(TARGET_DIR)/home/emc2/scripts/
	cp -f $(EMC2_DIR)/bin/* $(TARGET_DIR)/home/emc2/bin/
	cp -f $(EMC2_DIR)/lib/*.so* $(TARGET_DIR)/home/emc2/lib/
	cp -f $(EMC2_DIR)/configs/miniemc2/* $(TARGET_DIR)/home/emc2/configs
ifeq ($(BR2_TARGET_MINIEMC2_MINI2416),y)
	rm -f $(TARGET_DIR)/home/emc2/configs/miniemc.hal.2440
	mv -f $(TARGET_DIR)/home/emc2/configs/miniemc.hal.2416 $(TARGET_DIR)/home/emc2/configs/miniemc.hal
else
	rm -f $(TARGET_DIR)/home/emc2/configs/miniemc.hal.2416
	mv -f $(TARGET_DIR)/home/emc2/configs/miniemc.hal.2440 $(TARGET_DIR)/home/emc2/configs/miniemc.hal
endif
	cp -rf $(EMC2_DIR)/lib/*  $(STAGING_DIR)/usr/lib/
	touch $@

emc2: emckernel xenomai $(EMC2_DIR)/.copied


emc2-clean:
	$(MAKE) -C $(EMC2_DIR) clean

emc2-dirclean:
	rm -rf $(EMC2_DIR)

ifeq ($(BR2_PACKAGE_EMC2),y)
TARGETS+=emc2
endif
