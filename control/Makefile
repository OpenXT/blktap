#
# Copyright (c) 2012 Citrix Systems, Inc.
#

BLKTAP_ROOT := ../
include $(BLKTAP_ROOT)/Rules.mk

IBIN               = tap-ctl

#CFLAGS            += -Werror
CFLAGS            += -Wno-unused
CFLAGS            += -I../include -I../drivers
CFLAGS            += -D_GNU_SOURCE
CFLAGS            += -DTAPCTL

# Get gcc to generate the dependencies for us.
CFLAGS            += -Wp,-MD,.$(@F).d
DEPS               = .*.d

CTL_OBJS  := tap-ctl-ipc.o
CTL_OBJS  += tap-ctl-list.o
CTL_OBJS  += tap-ctl-allocate.o
CTL_OBJS  += tap-ctl-free.o
CTL_OBJS  += tap-ctl-create.o
CTL_OBJS  += tap-ctl-destroy.o
CTL_OBJS  += tap-ctl-spawn.o
CTL_OBJS  += tap-ctl-attach.o
CTL_OBJS  += tap-ctl-detach.o
CTL_OBJS  += tap-ctl-open.o
CTL_OBJS  += tap-ctl-close.o
CTL_OBJS  += tap-ctl-pause.o
CTL_OBJS  += tap-ctl-unpause.o
CTL_OBJS  += tap-ctl-major.o
CTL_OBJS  += tap-ctl-check.o
CTL_OBJS  += tap-ctl-stats.o

CTL_PICS  = $(patsubst %.o,%.opic,$(CTL_OBJS))

OBJS = $(CTL_OBJS) tap-ctl.o
PICS = $(CTL_PICS)

LIBTAPCTL_MAJOR = 0.1
LIBTAPCTL_MINOR = 0

LIBS = libblktapctl.a libblktapctl.so.$(LIBTAPCTL_MAJOR).$(LIBTAPCTL_MINOR)
IBIN = tap-ctl

all: build

build: $(IBIN) $(LIBS)

tap-ctl: tap-ctl.o libblktapctl.so.$(LIBTAPCTL_MAJOR).$(LIBTAPCTL_MINOR)
	$(CC) $(CFLAGS) -o $@ $^

libblktapctl.a: $(CTL_OBJS)
	$(AR) r $@ $^

libblktapctl.so.$(LIBTAPCTL_MAJOR).$(LIBTAPCTL_MINOR): $(CTL_PICS)
	$(CC) $(CFLAGS) -fPIC -shared -rdynamic $^ -o $@

install: $(IBIN) $(LIBS)
	$(INSTALL_DIR) -p $(DESTDIR)$(SBINDIR)
	$(INSTALL_PROG) $(IBIN) $(DESTDIR)$(SBINDIR)
	$(INSTALL_DIR) -p $(DESTDIR)$(LIBDIR)
	$(INSTALL_DATA) $(LIBS) $(DESTDIR)$(LIBDIR)
	ln -sf libblktapctl.so.$(LIBTAPCTL_MAJOR).$(LIBTAPCTL_MINOR) libblktapctl.so
#	$(INSTALL_DIR) -p $(DESTDIR)$(SYSCONFDIR)/udev/rules.d
#	$(INSTALL_DATA) blktap.rules $(DESTDIR)$(SYSCONFDIR)/udev/rules.d/

clean:
	rm -f $(OBJS) $(PICS) $(DEPS) $(IBIN) $(LIBS) libblktapctl.so*
	rm -f *~

.PHONY: all build clean install

-include $(DEPS)
