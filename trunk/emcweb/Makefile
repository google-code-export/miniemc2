CFLAGS += -O2 -I./include -I./ -I../emc2-arm/include -I../emc2-arm/src/emc/usr_intf -s
LDFLAGS += -L../emc2-arm/lib -lboost_serialization-mt -lboost_thread-mt -lpthread -lemc -lnml -lemcini -ldl
EXEC = emcweb
OBJS =	emccontroller.cpp main.cpp mongoose_wrapper.cpp ../emc2-arm/src/emc/usr_intf/shcom.cc

all: $(EXEC)

$(EXEC): $(OBJS) mongoose.o
	echo Compiler $(CXX)
	echo CFLAGS $(CFLAGS)
	$(CXX)  $(CFLAGS) -o $@ $(OBJS) mongoose.o $(LDFLAGS)

mogoose.o: mogoose.c
	$(CC) $(CFLAGS) -o $@ -c ./mongoose.c

clean:
	-rm -f $(EXEC) *.o
