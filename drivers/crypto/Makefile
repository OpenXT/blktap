#
# Copyright (c) 2014 Citrix Systems, Inc.
#

# ASFLAGS += -g -I.
# CFLAGS += -g -I.
# LDFLAGS += -g
RANLIB ?= ranlib

LIBXTS-AES-OBJS := xts_aes.o

libxts-aes.a: $(LIBXTS-AES-OBJS)
	$(AR) r $@ $(LIBXTS-AES-OBJS)
	$(RANLIB) $@

test_decrypt: test_decrypt.o libxts-aes.a
	$(CC) $(CFLAGS) -o $@ -lcrypto -L. -lxts-aes $+

clean:
	rm -f $(LIBXTS-AES-OBJS)
	rm -f libxts-aes.a
	rm -f test_decrypt test_decrypt.o

