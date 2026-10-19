# hrecord Build Script (Native Haiku OS Conversion)
SHELL := /bin/bash
GUI_TARGET = hrecord
VERSION = 1.14.2
PACKAGE_DIR := build/package
DUMMY_PC_PATH := $(shell pwd)/build/pkgconfig

# Source mapping parameters
GUI_SRCS = hrecord.cpp stream/hrecord_stream.cpp
GUI_OBJS = $(GUI_SRCS:.cpp=.o)

# FIXED: Standardized target matching. Checking for hrecord.rdef instead of hTV.rdef
HAS_RDEF := $(shell [ -f hrecord.rdef ] && echo yes || echo no)
ifeq ($(HAS_RDEF), yes)
    GUI_RSRCS = hrecord.rsrc
else
    GUI_RSRCS = 
endif

# --- Architecture & Path Setup ---
UNAME_M := $(shell uname -m)
ifeq ($(UNAME_M), BePC)
    CXX = g++-x86
    ARCH = x86_gcc2
    LIB_ARCH_DIR = /x86
    DEFINES += -DIS_HAIKU_32BIT
    PKG_CONFIG_CMD = pkg-config-x86
    # the 32-bit libraries come from the secondary-architecture packages (ffmpeg8_x86, curl_x86)
    DEP_SUFFIX = _x86
else
    CXX = g++
    ARCH = x86_64
    LIB_ARCH_DIR = 
    PKG_CONFIG_CMD = pkg-config
    DEP_SUFFIX =
endif

export PKG_CONFIG_PATH := $(DUMMY_PC_PATH):/boot/home/config/non-packaged/lib/pkgconfig:/boot/home/config/non-packaged/lib$(LIB_ARCH_DIR)/pkgconfig:/boot/system/develop/lib$(LIB_ARCH_DIR)/pkgconfig:

CXXFLAGS = -std=c++17 -O3 -Wall -rdynamic
# private/interface and private/app hold <WindowInfo.h> (get_window_info(),
# BPrivate::get_window_order()) -- used by --experimental-screen-capture's
# window tracking, the same private Window Kit API hDesktop uses.
INCLUDES = -I/boot/home/config/non-packaged/include -I/boot/system/develop/headers \
           -I/boot/system/develop/headers/private/interface -I/boot/system/develop/headers/private/app
LIB_PATH = -L/boot/system/lib$(LIB_ARCH_DIR) -L/boot/system/develop/lib$(LIB_ARCH_DIR) -L/boot/home/config/non-packaged/lib$(LIB_ARCH_DIR) 

EXTRA_LIBS = $(shell $(PKG_CONFIG_CMD) --libs libavformat libavcodec libavutil libswscale libswresample) \
             -lcurl -lnetwork

# -lgame: BDirectWindow's own symbols live in Haiku's separate Game Kit lib,
# not libbe itself -- confirmed by research/directwindow_probe.cpp's own
# Makefile, needed here too for --direct-tiled-capture.
HAIKU_LIBS = -lbe -lgame -lmedia -ltranslation -ltracker -lshared -lroot -lpthread

CXXFLAGS += $(shell $(PKG_CONFIG_CMD) --cflags libavformat libavcodec libavutil libswscale libswresample)

LIBS = $(LIB_PATH) $(EXTRA_LIBS) $(HAIKU_LIBS)

.PHONY: all clean gui

# The mixer window: runs hrecord as its engine
gui: hrecord_gui.cpp hrecord_mixer.rdef
	$(CXX) -std=c++17 -O2 -Wall $(INCLUDES) hrecord_gui.cpp -o hrecord_mixer -lbe -lroot -lnetwork
	rc -o hrecord_mixer.rsrc hrecord_mixer.rdef
	xres -o hrecord_mixer hrecord_mixer.rsrc
	mimeset -f hrecord_mixer

all: $(GUI_TARGET)

# Link the graphical desktop client binary
$(GUI_TARGET): $(GUI_OBJS) $(GUI_RSRCS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $(GUI_TARGET) $(GUI_OBJS) $(LIBS) 
ifeq ($(HAS_RDEF), yes)
	xres -o $(GUI_TARGET) $(GUI_RSRCS)
endif
	mimeset -f $(GUI_TARGET)

# FIXED: Explicitly mapped resource target to guarantee rc runs cleanly
$(GUI_RSRCS): hrecord.rdef
	rc -o $@ $<

# General object file compilation hooks
%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Deep system cleaning target
clean:
	rm -f *.o stream/*.o *.rsrc *.hpkg $(GUI_TARGET) hrecord_mixer stream/hrecord_x264 
	rm -rf build




release: all gui stream-release
	@[ -n "$(PACKAGE_DIR)" ] || { echo "PACKAGE_DIR is undefined"; exit 1; }
	rm -rf "./$(PACKAGE_DIR)"
	mkdir -p $(PACKAGE_DIR)
	sed -e 's/$$(GUI_TARGET)/$(GUI_TARGET)/g' -e 's/$$(VERSION)/$(VERSION)/g' -e 's/$$(ARCH)/$(ARCH)/' -e 's/$$(DEP_SUFFIX)/$(DEP_SUFFIX)/g' -e 's/$$(YEAR)/$(shell date +%Y)/' $(GUI_TARGET).tpl > $(PACKAGE_DIR)/.PackageInfo
	mkdir -p $(PACKAGE_DIR)/bin
	cp $(GUI_TARGET) $(PACKAGE_DIR)/bin/$(GUI_TARGET)
	mkdir -p $(PACKAGE_DIR)/apps $(PACKAGE_DIR)/data/deskbar/menu/Applications
	cp hrecord_mixer $(PACKAGE_DIR)/apps/hrecord_mixer
	ln -s ../../../../apps/hrecord_mixer "$(PACKAGE_DIR)/data/deskbar/menu/Applications/hRecord Mixer"
	package create -C $(PACKAGE_DIR) $(GUI_TARGET)-$(VERSION)-1-$(ARCH).hpkg	





# The H.264 add-on of hrecord's live streaming, as a package of its own (it is GPL, because it is built on x264)
STREAM_PACKAGE_DIR := build/stream_package
stream-release:
	$(MAKE) -C stream
	rm -rf "./$(STREAM_PACKAGE_DIR)"
	mkdir -p $(STREAM_PACKAGE_DIR)/add-ons/hrecord
	sed -e 's/$$(VERSION)/$(VERSION)/g' -e 's/$$(ARCH)/$(ARCH)/' -e 's/$$(DEP_SUFFIX)/$(DEP_SUFFIX)/g' -e 's/$$(YEAR)/$(shell date +%Y)/' stream/hrecord_stream.tpl > $(STREAM_PACKAGE_DIR)/.PackageInfo
	cp stream/hrecord_x264 $(STREAM_PACKAGE_DIR)/add-ons/hrecord/hrecord_x264
	package create -C $(STREAM_PACKAGE_DIR) hrecord_stream-$(VERSION)-1-$(ARCH).hpkg

.PHONY: stream-release
