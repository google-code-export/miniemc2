#ifdef USE_FOR_ARM
EMCRSHSRCS := emc/usr_intf/emcrsh.cc \
              emc/usr_intf/shcom.cc
IOSHSRCS := emc/usr_intf/iosh.cc \
	emc/motion/usrmotintf.cc \
	emc/motion/emcmotglb.c \
	emc/motion/emcmotutil.c

USRMOTSRCS := \
	emc/usr_intf/usrmot.c \
	emc/motion/usrmotintf.cc \
	emc/motion/emcmotglb.c \
	emc/motion/emcmotutil.c

HALUISRCS := emc/usr_intf/halui.cc

USERSRCS += $(EMCRSHSRCS) $(IOSHSRCS) $(USRMOTSRCS) $(HALUISRCS)

$(call TOOBJSDEPS, $(TCLSRCS)) : EXTRAFLAGS = $(ULFLAGS) $(TCL_CFLAGS)


../bin/emcrsh: $(call TOOBJS, $(EMCRSHSRCS)) ../lib/libemc.a ../lib/libnml.so.0 ../lib/libemcini.so.0
	$(ECHO) Linking $(notdir $@)
	$(CXX) $(LDFLAGS) -o $@ $(ULFLAGS) $^ -lpthread
TARGETS += ../bin/emcrsh

../bin/iosh: $(call TOOBJS, $(IOSHSRCS)) ../lib/libemc.a ../lib/libnml.so.0 ../lib/libemchal.so.0 ../lib/libemcini.so.0
	$(ECHO) Linking $(notdir $@)
	$(CXX) $(LDFLAGS) -o $@ $(ULFLAGS) $(TCL_CFLAGS) $^ $(TCL_LIBS)
TARGETS += ../bin/iosh

../bin/usrmot: $(call TOOBJS, $(USRMOTSRCS)) ../lib/libemc.a ../lib/libnml.so.0 ../lib/libemchal.so.0 ../lib/libemcini.so.0
	$(ECHO) Linking $(notdir $@)
	$(CXX) $(LDFLAGS) -o $@ $(ULFLAGS) $^
TARGETS += ../bin/usrmot

../bin/usrmot: $(call TOOBJS, $(USRMOTSRCS)) ../lib/libemc.a ../lib/libnml.so.0 ../lib/libemchal.so.0 ../lib/libemcini.so.0
	$(ECHO) Linking $(notdir $@)
	$(CXX) $(LDFLAGS) -o $@ $(ULFLAGS) $^
TARGETS += ../bin/usrmot

../bin/halui: $(call TOOBJS, $(HALUISRCS)) ../lib/libemc.a ../lib/libemcini.so.0 ../lib/libnml.so.0 ../lib/libemchal.so.0
	$(ECHO) Linking $(notdir $@)
	$(CXX) $(LDFLAGS) -o $@ $(ULFLAGS) $^
TARGETS += ../bin/halui


#endif
