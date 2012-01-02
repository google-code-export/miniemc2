#############################################################
#
# EMCWEB
#
#############################################################
EMC2_EMCWEB_SRC_DIR:=/home/ksu/projects/miniemc/workspace/projects/emcweb
EMC2_EMCWEB_DIR:=$(BUILD_DIR)/emcweb
EMC2_EMCWEB_DIST:=emcweb.tar.gz

$(DL_DIR)/$(EMC2_EMCWEB_DIST):
	cd $(EMC2_EMCWEB_SRC_DIR); ./mkdist; mv -f ./emcweb.tar.gz $(DL_DIR)

$(EMC2_EMCWEB_DIR)/.unpacked : $(DL_DIR)/$(EMC2_EMCWEB_DIST)
	mkdir -p $(EMC2_EMCWEB_DIR)
	$(ZCAT) $(DL_DIR)/emcweb.tar.gz | tar -C $(EMC2_EMCWEB_DIR) $(TAR_OPTIONS) -
	touch $@
	
$(EMC2_EMCWEB_DIR)/.copied: $(EMC2_EMCWEB_DIR)/.unpacked
	cp -rf $(EMC2_EMCWEB_DIR)/www $(TARGET_DIR)/home/
	cp -f $(EMC2_EMCWEB_DIR)/emcweb $(TARGET_DIR)/bin/
	(cd $(TARGET_DIR)/home/www/html; ln -fs ../../../tmp ./data )
	touch $@
	
emcweb: boost emc2 $(EMC2_EMCWEB_DIR)/.copied

emcweb-clean:
	

emcweb-dirclean:
	rm -rf $EMC2_EMCWEB_DIR

ifeq ($(BR2_PACKAGE_EMC2_EMCWEB),y)
TARGETS+=emcweb
endif
