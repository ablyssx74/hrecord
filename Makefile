# hTV Build Script (Native Haiku OS Conversion)
SHELL := /bin/bash
GUI_TARGET = hrecord
VERSION = 1.0.2
PACKAGE_DIR := build/package
DUMMY_PC_PATH := $(shell pwd)/build/pkgconfig

# Source mapping parameters
GUI_SRCS = hrecord.cpp
GUI_OBJS = $(GUI_SRCS:.cpp=.o)

# FIXED: Standardized target matching. Checking for hrecord.rdef instead of hTV.rdef
HAS_RDEF := $(shell [ -f hrecord.rdef ] && echo yes || echo no)
ifeq ($(HAS_RDEF), yes)
    GUI_RSRCS = hrecord.rsrc
else
    GUI_RSRCS = 
endif

# --- Architecture & Path Setup ---
UNAME_M := $(shell uname -p)
ifeq ($(UNAME_M), x86)
    CXX = g++-x86
    ARCH = x86_gcc2
    LIB_ARCH_DIR = /x86
    DEFINES += -DIS_HAIKU_32BIT
    PKG_CONFIG_CMD = x86-pkg-config
else
    CXX = g++
    ARCH = x86_64
    LIB_ARCH_DIR = 
    PKG_CONFIG_CMD = pkg-config
endif

export PKG_CONFIG_PATH := $(DUMMY_PC_PATH):/boot/home/config/non-packaged/lib/pkgconfig:/boot/home/config/non-packaged/lib$(LIB_ARCH_DIR)/pkgconfig:/boot/system/develop/lib$(LIB_ARCH_DIR)/pkgconfig:

CXXFLAGS = -std=c++17 -O3 -Wall -rdynamic 
INCLUDES = -I/boot/home/config/non-packaged/include -I/boot/system/develop/headers
LIB_PATH = -L/boot/system/lib$(LIB_ARCH_DIR) -L/boot/system/develop/lib$(LIB_ARCH_DIR) -L/boot/home/config/non-packaged/lib$(LIB_ARCH_DIR) 

EXTRA_LIBS = $(shell $(PKG_CONFIG_CMD) --libs libavformat libavcodec libavutil libswscale libswresample) \
             -lcurl -lnetwork

HAIKU_LIBS = -lbe -ltranslation -ltracker -lshared -lroot -lpthread

CXXFLAGS += $(shell $(PKG_CONFIG_CMD) --cflags libavformat libavcodec libavutil libswscale libswresample)

LIBS = $(LIB_PATH) $(EXTRA_LIBS) $(HAIKU_LIBS)

.PHONY: all clean 

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
	rm -f *.o *.rsrc *.hpkg $(GUI_TARGET) 
	rm -rf build




release: all
	@[ -n "$(PACKAGE_DIR)" ] || { echo "PACKAGE_DIR is undefined"; exit 1; }
	rm -rf "./$(PACKAGE_DIR)"
	mkdir -p $(PACKAGE_DIR)
	sed -e 's/$$(GUI_TARGET)/$(GUI_TARGET)/g' -e 's/$$(VERSION)/$(VERSION)/g' -e 's/$$(ARCH)/$(ARCH)/' -e 's/$$(YEAR)/$(shell date +%Y)/' $(GUI_TARGET).tpl > $(PACKAGE_DIR)/.PackageInfo
	mkdir -p $(PACKAGE_DIR)/bin
	cp $(GUI_TARGET) $(PACKAGE_DIR)/bin/$(GUI_TARGET)
	package create -C $(PACKAGE_DIR) $(GUI_TARGET)-$(VERSION)-1-$(ARCH).hpkg	



