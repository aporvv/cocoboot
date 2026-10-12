CC = m68k-palmos-gcc
CFLAGS = -Wall -O -s -I../include ${DEFINES}
LDFLAGS = -static 
# PalmOSGlue is normally found via the palmdev specs set up by
# palmdev-prep.  Override LIBS if your SDK lives somewhere else, e.g.
#   make LIBS="-L/path/to/sdk-5r3/lib/m68k-palmos-coff -lPalmOSGlue"
LIBS ?= -lPalmOSGlue
OBJS = cocoboot.o mainform.o mem.o cpu.o imgloader.o mach.o options.o fwutils.o


cocoboot.m68k: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -o cocoboot.m68k ${LIBS}

clean:
	rm -f $(OBJS) cocoboot.m68k
