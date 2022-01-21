# Requirements: 
# i2cdriver binary: libfuse3-dev
# i2cdriver manpage: go-md2man

# If we're called from inside the linux/ directory, call again from ..
ifeq ($(notdir $(CURDIR)),linux)
all:
	cd .. && $(MAKE) -f linux/Makefile all

.DEFAULT:
	cd .. && $(MAKE) -f linux/Makefile $(MAKECMDGOALS)
else

INCLUDES := -Icommon -Ilinux $(shell pkg-config fuse3 --cflags)
FUSELIB := -lfuse3

DESTDIR := /usr/local

CXX := g++
CC:= gcc
INSTALL := install -D

OPTIMIZE := -O2
WARNFLAGS := -Wall -Wextra -Wno-unused-parameter 
CXXFLAGS := $(OPTIMIZE) $(WARNFLAGS) $(INCLUDES) -D_GNU_SOURCE -std=gnu++2a -fno-rtti

CFLAGS += -I common -Wall -Wpointer-sign # -Werror

all: build/i2ccl build/i2cdriver linux/i2cdriver.1

install: all
	$(INSTALL) build/i2ccl       $(DESTDIR)/bin/i2ccl
	$(INSTALL) build/i2cdriver   $(DESTDIR)/bin/i2cdriver
	$(INSTALL) linux/i2cdriver.1 $(DESTDIR)/share/man/man1/i2cdriver.1

build/i2ccl: linux/i2c.c common/i2cdriver.c
	mkdir -p build/
	$(CC) -o $@ $(CPPFLAGS) $(CFLAGS) $^

build/i2cdriver: linux/i2cdriver.cpp
	mkdir -p build/
	$(CXX) -o $@ $(CXXFLAGS) $^ $(FUSELIB)

linux/i2cdriver.1: linux/README.md
	go-md2man -in=$< -out=$@

clean:
	rm -f build/i2ccl build/i2cdriver
	rmdir build

distclean: clean
	rm -f linux/i2cdriver.1

endif