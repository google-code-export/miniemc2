#############################################################
#
#  Widgets for Technical Applications
#
#############################################################
QT2_VERSION:=2.3.10
QT2_SOURCE:=qt-$(QT2_VERSION).tar.gz
QT2_SITE:=ftp://ftp.trolltech.com/qt/source/qt-embedded-$(QT2_VERSION)-free.tar.gz
QT2_DIR:=$(BUILD_DIR)/qt-$(QT2_VERSION)
#BOOST_LIBRARY:=boost_$(BOOST_VERSION)
QT2_TARGET_LIBRARY:=usr/lib/libqte.so.2.3.10
#BOOST_DIR=$(BUILD_DIR)/boost_$(BOOST_VERSION)
#BOOST_TOOLSET:='using gcc : arm : $(TARGET_CC) : <compileflags>-march=armv4t -mtune=arm920t <linkflags>-march=armv4t -mtune=arm920t ;'


$(DL_DIR)/$(QT2_SOURCE):
	$(call DOWNLOAD,$(QT2_SITE),$(QT2_SOURCE))

$(QT2_DIR)/.source: $(DL_DIR)/$(QT2_SOURCE)
	$(ZCAT) $(DL_DIR)/$(QT2_SOURCE) | tar -C $(BUILD_DIR) $(TAR_OPTIONS) -
	touch $@

$(QT2_DIR)/.configured: $(QT2_DIR)/.source
	( cd $(QT2_DIR); export QTDIR=`pwd`; export SYSROOT=$(STAGING_DIR); \
	./configure -xplatform linux-arm-g++ -shared -depths 16  -no-xkb -no-sm -no-xft -vnc -thread -tslib; \
	make \
	)
	touch $@



$(TARGET_DIR)/$(QT2_TARGET_LIBRARY): $(QT2_DIR)/.configured
	cp -dpfR  $(QT2_DIR)/lib/* $(STAGING_DIR)/usr/lib/
	mkdir -p $(TARGET_DIR)/usr/lib/
	cp -dpfR  $(QT2_DIR)/lib/* $(TARGET_DIR)/usr/lib/

QT2_DEP=tslib
QT2_DEP+=busybox

qt2: $(QT2_DEP) $(TARGET_DIR)/$(QT2_TARGET_LIBRARY)

qt2-source: $(DL_DIR)/$(QT2_SOURCE)
qt2-clean:

qt2-dirclean:
	rm -rf $(QT2_DIR)

#############################################################
#
# Toplevel Makefile options
#
#############################################################
ifeq ($(BR2_PACKAGE_QT2),y)
TARGETS+=qt2
endif

