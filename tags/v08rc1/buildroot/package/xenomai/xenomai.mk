#############################################################
#
# Xenomai
#
#############################################################
XENOMAI_VERSION:=2.5.6
XENOMAI_SOURCE:=xenomai-$(XENOMAI_VERSION).tar.bz2
2XENOMAI_SITE:=http://download.gna.org/xenomai/stable
XENOMAI_DIR:=$(BUILD_DIR)/xenomai-$(XENOMAI_VERSION)
XENOMAI_INSTALL_DIR:=$(XENOMAI_DIR)/install
#XENOMAI_CONF_OPT = --host=arm-linux-gnueabi --enable-arm-eabi --enable-arm-mach=s3c2410 --disable-arm-tsc

$(DL_DIR)/$(XENOMAI_SOURCE):
	$(call DOWNLOAD,$(XENOMAI_SITE),$(XENOMAI_SOURCE))

$(XENOMAI_DIR)/.source: $(DL_DIR)/$(XENOMAI_SOURCE)
	$(BZCAT) $(DL_DIR)/$(XENOMAI_SOURCE) | tar -C $(BUILD_DIR) $(TAR_OPTIONS) -
	touch $@

$(XENOMAI_DIR)/.configured: $(XENOMAI_DIR)/.source
	(cd $(XENOMAI_DIR); rm -rf config.cache; \
	$(TARGET_CONFIGURE_OPTS) \
	$(TARGET_CONFIGURE_ARGS) \
		./configure \
		--host=$(GNU_TARGET_NAME) \
		--enable-arm-eabi \
		--enable-arm-mach=s3c2410 \
		--disable-arm-tsc \
		--prefix=$(XENOMAI_INSTALL_DIR) \
		--exec-prefix=$(XENOMAI_INSTALL_DIR) \
	)
	touch $@

$(XENOMAI_DIR)/.built: $(XENOMAI_DIR)/.configured
	$(MAKE) CC=$(TARGET_CC) -C $(XENOMAI_DIR)
	$(MAKE) CC=$(TARGET_CC) -C $(XENOMAI_DIR) install
	cp -rf $(XENOMAI_INSTALL_DIR)/lib/* $(STAGING_DIR)/usr/lib/
	cp -f $(XENOMAI_INSTALL_DIR)/lib/libxenomai.so.* $(TARGET_DIR)/usr/lib/
	cp -f $(XENOMAI_INSTALL_DIR)/lib/libnative.so.* $(TARGET_DIR)/usr/lib/
	touch $@

xenomai: $(XENOMAI_DIR)/.built


xenomai-clean:
	$(MAKE) -C $(XENOMAI_DIR) clean

xenomai-dirclean:
	rm -rf $(XENOMAI_DIR)

ifeq ($(BR2_PACKAGE_XENOMAI),y)
TARGETS+=xenomai
endif
