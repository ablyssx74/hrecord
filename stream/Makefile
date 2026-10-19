# hrecord_x264: the H.264 add-on of hrecord (GPL, because it is built on x264)
ADDON = hrecord_x264
ifeq ($(shell uname -m), BePC)
	CC = gcc-x86
	LDFLAGS = -L/boot/system/lib/x86 -L/boot/system/develop/lib/x86
else
	CC = gcc
	LDFLAGS =
endif

all: $(ADDON)

$(ADDON): hrecord_x264.c hrecord_x264.h
	$(CC) -O2 -Wall -fPIC -shared $(LDFLAGS) -o $@ hrecord_x264.c -lx264 -lroot

clean:
	rm -f $(ADDON)
