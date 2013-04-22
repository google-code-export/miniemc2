#############################################################
#
# EMCWEB
#
#############################################################
EMC2_EMCWEB_SRC_DIR:=$(TOPDIR)/../emcweb

	
$(TARGET_DIR)/bin/emcweb: 
	$(TARGET_CONFIGURE_OPTS) make -C $(EMC2_EMCWEB_SRC_DIR)
	mkdir -p $(TARGET_DIR)/home/www/html/res $(TARGET_DIR)/home/www/html/js $(TARGET_DIR)/home/www/html/css
	cp -r $(EMC2_EMCWEB_SRC_DIR)/res $(TARGET_DIR)/home/www/html/
	cp -r $(EMC2_EMCWEB_SRC_DIR)/css $(TARGET_DIR)/home/www/html/
	cp -r $(EMC2_EMCWEB_SRC_DIR)/js $(TARGET_DIR)/home/www/html/
	cp -r $(EMC2_EMCWEB_SRC_DIR)/html/* $(TARGET_DIR)/home/www/html/
	cp -f $(EMC2_EMCWEB_SRC_DIR)/emcweb $(TARGET_DIR)/bin/
	
emcweb: boost emc2 $(TARGET_DIR)/bin/emcweb

emcweb-clean:
	make -C $(EMC2_EMCWEB_SRC_DIR) clean

ifeq ($(BR2_PACKAGE_EMC2_EMCWEB),y)
TARGETS+=emcweb
endif
