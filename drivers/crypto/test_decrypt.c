/*
 * Copyright (c) 2010, XenSource Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of XenSource Inc. nor the names of its contributors
 *       may be used to endorse or promote products derived from this software
 *       without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
 * OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Copyright (c) 2014 Citrix Systems, Inc.
 */


// #define PERF

#include <err.h>
#include <stdio.h>
#include <stdint.h>
#include "compat-crypto-openssl.h"
#include "xts_aes.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
 
#define BUFSIZE (512)
uint8_t dst_buf[BUFSIZE];
uint8_t src_buf[BUFSIZE];

uint8_t key[32];

int
main(int argc, char **argv, char **envp)
{
    struct crypto_blkcipher *xts_tfm;
    uint8_t *p_buf;
    int fd, i, ret;
    char *keyfile = "test.key";

    fd = open(keyfile, O_RDONLY);
    if (fd == -1)
      err(1, "open");

    ret = read(fd, key, sizeof(key));
    if (ret != sizeof(key))
      err(1, "read");

    xts_tfm = xts_aes_setup();

    xts_setkey(crypto_blkcipher_tfm(xts_tfm), key, sizeof(key));

#ifdef PERF
  for (i = 0; i < 1000000; i++) {
#endif

#ifndef PERF
    printf("buffers: src %p dst %p\n", src_buf, dst_buf);
#endif
    ret = xts_aes_plain_encrypt(xts_tfm, 0, src_buf, src_buf, BUFSIZE);
#ifndef PERF
    printf("return %d\n", ret);
#endif

#ifndef PERF
    p_buf = src_buf;
    for (i = 0; i < BUFSIZE; i += 8)
	printf("%03d/%p: %02x%02x%02x%02x%02x%02x%02x%02x\n", i, &p_buf[i],
	       p_buf[i], p_buf[i+1], p_buf[i+2], p_buf[i+3],
	       p_buf[i+4], p_buf[i+5], p_buf[i+6], p_buf[i+7]);
#endif

    ret = xts_aes_plain_decrypt(xts_tfm, 0, src_buf, src_buf, BUFSIZE);
#ifndef PERF
    printf("return %d\n", ret);
#endif

#ifndef PERF
    p_buf = src_buf;
    for (i = 0; i < BUFSIZE; i += 8)
	printf("%03d/%p: %02x%02x%02x%02x%02x%02x%02x%02x\n", i, &p_buf[i],
	       p_buf[i], p_buf[i+1], p_buf[i+2], p_buf[i+3],
	       p_buf[i+4], p_buf[i+5], p_buf[i+6], p_buf[i+7]);
#endif
#ifdef PERF
  }
#endif

    return 0;
}
