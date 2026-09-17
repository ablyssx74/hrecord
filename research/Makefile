# Standalone build for directwindow_probe -- see directwindow_probe.cpp's own
# top comment for what this is and how to read its output. Not part of
# hrecord's own build; run `make` from inside this research/ directory.

TARGET = directwindow_probe
SRCS = directwindow_probe.cpp

UNAME_M := $(shell uname -m)
ifeq ($(UNAME_M), BePC)
    CXX = g++-x86
else
    CXX = g++
endif

CXXFLAGS = -std=c++17 -O2 -Wall
INCLUDES = -I/boot/system/develop/headers
LIB_PATH = -L/boot/system/lib -L/boot/system/develop/lib
# If linking fails with undefined references to BDirectWindow's own symbols,
# uncomment the next line -- some Haiku versions keep it in a separate Game
# Kit library instead of libbe itself.
# LIBS_EXTRA = -lgame
LIBS = $(LIB_PATH) -lbe $(LIBS_EXTRA)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $(TARGET) $(SRCS) $(LIBS)

clean:
	rm -f $(TARGET)
