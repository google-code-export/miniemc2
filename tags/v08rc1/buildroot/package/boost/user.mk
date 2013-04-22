#############################################################
#
#  Widgets for Technical Applications
#
#############################################################
BOOST_VERSION:=1_42_0
BOOST_SOURCE:=boost_$(BOOST_VERSION).tar.bz2
BOOST_SITE:=http://kent.dl.sourceforge.net/project/boost/boost/1.42.0/boost_1_42_0.tar.bz2
BOOST_DIR:=$(BUILD_DIR)/boost_$(BOOST_VERSION)
BOOST_LIBRARY:=boost_$(BOOST_VERSION)
BOOST_TARGET_LIBRARY:=usr/lib
BOOST_DIR=$(BUILD_DIR)/boost_$(BOOST_VERSION)
BOOST_TOOLSET:='using gcc : arm : $(TARGET_CC) : <compileflags>-march=armv4t -mtune=arm920t <linkflags>-march=armv4t -mtune=arm920t ;'

BOOST_LIBS:= --with-thread

ifeq ($(BR2_BOOST_TEST),y)
BOOST_LIBS+= --with-test
endif
ifeq ($(BR2_BOOST_MATH),y)
BOOST_LIBS+= --with-math
endif
ifeq ($(BR2_BOOST_system),y)
BOOST_LIBS+= --with-system
endif
ifeq ($(BR2_BOOST_FILESYSTEM),y)
BOOST_LIBS+= --with-filesystem
endif
ifeq ($(BR2_BOOST_SERIALIZATION),y)
BOOST_LIBS+= --with-serialization
endif


$(DL_DIR)/$(BOOST_SOURCE):
	$(call DOWNLOAD,$(BOOST_SITE),$(BOOST_SOURCE))

$(BOOST_DIR)/.source: $(DL_DIR)/$(BOOST_SOURCE)
	$(BZCAT) $(DL_DIR)/$(BOOST_SOURCE) | tar -C $(BUILD_DIR) $(TAR_OPTIONS) -
	echo $(BOOST_TOOLSET) >>  $(BOOST_DIR)/tools/build/v2/user-config.jam
	touch $@

$(BOOST_DIR)/.configured: $(BOOST_DIR)/.source
	( cd $(BOOST_DIR); $(BOOST_DIR)/bootstrap.sh )
	touch $@

$(BOOST_DIR)/$(BOOST_LIBRARY): $(BOOST_DIR)/.configured
	(cd $(BOOST_DIR); \
	$(BOOST_DIR)/bjam install $(BOOST_LIBS) \
	--libdir=$(STAGING_DIR)/usr/lib \
	--includedir=$(STAGING_DIR)/usr/include \
	--layout=tagged \
	--toolset=gcc-arm \
	)
	touch $@




$(TARGET_DIR)/$(BOOST_TARGET_LIBRARY): $(BOOST_DIR)/$(BOOST_LIBRARY)
	mkdir -p $(TARGET_DIR)/usr/lib
	cp -dpf $(STAGING_DIR)/usr/lib/libboost_*.so* $(TARGET_DIR)/usr/lib






boost: busybox $(TARGET_DIR)/$(BOOST_TARGET_LIBRARY)

boost-source: $(DL_DIR)/$(BOOST_SOURCE)
boost-clean:

boost-dirclean:
	rm -rf $(BOOST_DIR)

#############################################################
#
# Toplevel Makefile options
#
#############################################################
ifeq ($(BR2_PACKAGE_BOOST),y)
TARGETS+=boost
endif

