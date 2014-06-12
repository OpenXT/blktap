#
# Copyright (c) 2011 Citrix Systems, Inc.
#

BLKTAP_ROOT := ../../../
include $(BLKTAP_ROOT)/Rules.mk

CFLAGS            += -Werror
CFLAGS            += -Wno-unused
CFLAGS            += -I../include
CFLAGS            += -D_GNU_SOURCE

# Get gcc to generate the dependencies for us.
CFLAGS            += -Wp,-MD,.$(@F).d
DEPS               = .*.d

BINS              := random-copy test-snapshot aio-test

all: $(BINS)

random-copy: random-copy.c
	$(CC) $(CFLAGS) -o $@ $^

test-snapshot: test-snapshot.c
	$(CC) $(CFLAGS) -o $@ $^

aio-test: aio-test.c
	$(CC) $(CFLAGS) -o $@ $^ -laio

clean:
	rm -rf *.o *~ $(DEPS) $(BINS)

.PHONY: all clean

-include $(DEPS)
