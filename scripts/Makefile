#
# Copyright (c) 2010 Citrix Systems, Inc.
#

BLKTAP_ROOT := ../
include $(BLKTAP_ROOT)/Rules.mk

IBIN               = vhd-dm-create vhd-dm-remove
INST_DIR           = /usr/sbin

all: build

build: $(IBIN)

install: all
	$(INSTALL_DIR) -p $(DESTDIR)$(INST_DIR)
	$(INSTALL_PROG) $(IBIN) $(DESTDIR)$(INST_DIR)

clean:
	rm -rf *~

.PHONY: all build clean install
