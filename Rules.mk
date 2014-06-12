#  -*- mode: Makefile; -*-

# `all' is the default target
all:

include $(BLKTAP_ROOT)/Config.mk

ifneq ($(USE_SYSTEM_LIBRARIES),y)
XEN_LIBXC          = $(XEN_ROOT)/tools/libxc
XEN_XENSTORE       = $(XEN_ROOT)/tools/xenstore
endif

# Enable implicit LFS support *and* explicit LFS names.
CFLAGS  += $(shell getconf LFS_CFLAGS)
CFLAGS  += -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE
LDFLAGS += $(shell getconf LFS_LDFLAGS)

# 32-bit x86 does not perform well with -ve segment accesses on Xen.
CFLAGS-$(CONFIG_X86_32) += $(call cc-option,$(CC),-mno-tls-direct-seg-refs)
CFLAGS += $(CFLAGS-y)

%.opic: %.c
	$(CC) $(CPPFLAGS) -DPIC $(CFLAGS) -fPIC -c -o $@ $<

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

%.o: %.cc
	$(CC) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

