CC=icc
CXX=icpc
CFLAGS=-g -I.  -std=c99 -O
CXXFLAGS=-g -I. -tbb -O
LDFLAGS=
LIBS=-lpthread -lrt
TARGET ?= intel64

SRCS=main.c #server.c

PROG=overhead.bin

ifeq ($(TARGET),mic)
    CFLAGS += -mmic -DKNC
    CXXFLAGS += -mmic -DKNC
    LDFLAGS += -mmic
endif
ifeq ($(TARGET),intel64)
    CFLAGS += -DINTEL64
    CXXFLAGS += -DINTEL64
endif

TARGETDIR=$(TARGET)

TARGETSRCS=events.cpp output.cpp

OBJS=$(SRCS:%.c=$(TARGETDIR)/%.o) $(TARGETSRCS:%.cpp=$(TARGETDIR)/%.o)

default: $(TARGETDIR)/$(PROG)

# Build rules
$(TARGETDIR)/%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $(DEFINES) $(INCLUDES) $<

$(TARGETDIR)/%.o: $(TARGETDIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $(DEFINES) $(INCLUDES) $<

$(TARGETDIR)/%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $(DEFINES) $(INCLUDES) $<

$(TARGETDIR)/%.o: %.S
	$(CC) $(CFLAGS) -c -o $@ $(DEFINES) $(INCLUDES) $<

$(TARGETDIR)/%.s: %.S
	$(CC) $(CFLAGS) -E -dD $(DEFINES) $(INCLUDES) $< >  $@ 

$(OBJS): Makefile $(wildcard *.h)

.PRECIOUS: $(TARGETDIR)/%.o


$(TARGETDIR)/$(PROG): $(OBJS)
	$(CC) -o $@ $(LDFLAGS) $(OBJS) $(LIBS)


clean:
	rm -rf $(TARGETDIR)/$(PROG) $(TARGETDIR)/*.o
