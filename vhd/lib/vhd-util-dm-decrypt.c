/*
 * Copyright (c) 2010, Citrix Systems, Inc.
 *
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

#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>

#include "libvhd.h"

struct vhd_decrypt_progress {
	char                          display;
	uint32_t                      total;
	uint32_t                      cur;
};

struct vhd_decrypt_context {
	int                           src_raw;
	vhd_context_t                *src_vhd;
	vhd_context_t                *dst_vhd;
	struct vhd_decrypt_progress   progress;
};

#define ERR(_fmt, _args...) fprintf(stderr, "%d: " _fmt, __LINE__, ##_args)

#define VERR(_v, _fmt, _args...)					\
	do {								\
		char uuid[37];						\
		uuid_unparse((_v)->footer.uuid, uuid);			\
		ERR("%s: " _fmt, uuid, ##_args);			\
	} while (0)							\

#define PROGRESS(_ctx)							\
	do {								\
		if ((_ctx)->progress.display &&				\
		    (_ctx)->progress.total) {				\
			float cur = (float)(_ctx)->progress.cur;	\
			float total = (float)(_ctx)->progress.total;	\
			float pct = (cur / total) * 100.0;		\
			fprintf(stderr, "\r%6.2f%%", pct);		\
			fflush(stderr);					\
		}							\
	} while (0)

static int
vhd_util_pread_data(int fd, char *buf, size_t size, off64_t off)
{
	int err;
	ssize_t ret;

	err = 0;

	if (lseek64(fd, off, SEEK_SET) == (off64_t)-1) {
		err = -errno;
		goto out;
	}

	while (size) {
		ret = read(fd, buf, size);
		if (ret == -1) {
			if (errno == EAGAIN)
				continue;
			err = -errno;
			goto out;
		}

		buf  += ret;
		size -= ret;
	}

out:
	return err;
}

static int
vhd_util_stream_copy_block(struct vhd_decrypt_context *ctx, uint32_t blk)
{
	int err, i;
	off64_t off;
	char *bm, *data;
	vhd_context_t *src, *dst;

	bm    = NULL;
	data  = NULL;
	src   = ctx->src_vhd;
	dst   = ctx->dst_vhd;

	if (src->bat.bat[blk] == DD_BLK_UNUSED) {
		if (dst->bat.bat[blk] == DD_BLK_UNUSED)
			err = 0;
		else {
			err = -EIO;
			ERR("skipping allocated block 0x%x\n", blk);
		}
		goto out;
	}

	PROGRESS(ctx);

	err = posix_memalign((void **)&data,
			     VHD_SECTOR_SIZE, src->header.block_size);
	if (err) {
		err  = -err;
		data = NULL;
		ERR("allocating block 0x%x\n", blk);
		goto out;
	}

	memset(data, 0, src->header.block_size);
	off = ((uint64_t)blk * src->header.block_size) >> VHD_SECTOR_SHIFT;

	err = vhd_read_bitmap(src, blk, &bm);
	if (err) {
		ERR("error reading source bitmap for "
		    "block 0x%x: %d\n", blk, err);
		goto out;
	}

	i = 0;
	while (i < src->spb) {
		char *buf;
		off64_t pos;
		int cnt, copy;

		cnt  = 1;
		pos  = off + i;
		buf  = data + vhd_sectors_to_bytes(i);
		copy = vhd_bitmap_test(src, bm, i);

		while (i + cnt < src->spb &&
		       copy == vhd_bitmap_test(src, bm, i + cnt))
			cnt++;

		if (copy) {
			err = vhd_util_pread_data(ctx->src_raw, buf,
						  vhd_sectors_to_bytes(cnt),
						  vhd_sectors_to_bytes(pos));
			if (err) {
				ERR("reading dev block 0x%x: %d\n", blk, err);
				goto out;
			}
		}

		i += cnt;
	}

	err = vhd_write_bitmap(dst, blk, bm);
	if (err) {
		ERR("writing bitmap 0x%x: %d\n", blk, err);
		goto out;
	}

	err = vhd_write_block(dst, blk, data);
	if (err) {
		ERR("writing data 0x%x: %d\n", blk, err);
		goto out;
	}

out:
	free(bm);
	free(data);
	return err;
}

static int
__vhd_util_dm_decrypt(struct vhd_decrypt_context *ctx)
{
	char *buf;
	uint32_t i;
	int err, spp;
	vhd_bat_t bat;
	off64_t off, eoh;
	vhd_context_t *src;
	vhd_context_t *dst;

	buf = NULL;
	src = ctx->src_vhd;
	dst = ctx->dst_vhd;
	spp = getpagesize() >> VHD_SECTOR_SHIFT;

	/*
	 * We copy all the source metadata unmodified,
	 * with the exception of the BAT, which must be sorted
	 * by physical block address to enable serial output to a fifo.
	 */
	err = vhd_end_of_headers(src, &eoh);
	if (err) {
		ERR("finding end of source headers: %d\n", err);
		goto out;
	}

	err = posix_memalign((void **)&buf, VHD_SECTOR_SIZE, eoh);
	if (err) {
		err = -err;
		buf = NULL;
		ERR("allocating metadata\n");
		goto out;
	}

	err = vhd_pread(src, buf, eoh, 0);
	if (err) {
		ERR("reading vhd headers: %d\n", err);
		goto out;
	}

	bat.spb     = src->bat.spb;
	bat.entries = src->bat.entries;
	bat.bat     = (uint32_t *)(buf + src->header.table_offset);

	/*
	 * sort output BAT
	 */
	memset(dst->bat.bat, DD_BLK_UNUSED,
	       dst->bat.entries * sizeof(uint32_t));
	for (i = 0; i < src->bat.entries; i++) {
		if (src->bat.bat[i] != DD_BLK_UNUSED) {
			int gap;
	
			gap = 0;
			err = vhd_end_of_data(dst, &off);
			if (err) {
				ERR("finding end of data: %d\n", err);
				goto out;
			}

			off >>= VHD_SECTOR_SHIFT;

			/* data region of block should be page aligned */
			if ((off + dst->bm_secs) % spp) {
				gap  = (spp - ((off + dst->bm_secs) % spp));
				off += gap;
			}

			dst->bat.bat[i] = off;
			bat.bat[i] = off;

			if (ctx->progress.display)
				ctx->progress.total++;
		}
	}

	vhd_bat_out(&bat);

	err = vhd_pwrite(dst, buf, eoh, 0);
	if (err) {
		ERR("copying vhd headers\n");
		goto out;
	}

	for (i = 0; i < src->bat.entries; i++) {
		err = vhd_util_stream_copy_block(ctx, i);
		if (err)
			goto out;
	}

	err = vhd_end_of_data(dst, &off);
	if (err) {
		ERR("finding end of data: %d\n", err);
		goto out;
	}

	err = vhd_write_footer_at(dst, &dst->footer, off);
	if (err) {
		ERR("writing primary footer: %d\n", err);
		goto out;
	}

	PROGRESS(ctx);

out:
	free(buf);
	return err;
}

static int
vhd_util_dm_decrypt_open_output(struct vhd_decrypt_context *ctx,
				const char *vhd_out)
{
	int err;
	FILE *file;

	if (!strcmp(vhd_out, "-")) {
		file = stdout;
	} else {
		if (!access(vhd_out, F_OK)) {
			err = -EEXIST;
			ERR("%s already exists\n", vhd_out);
			goto out;
		}

		file = fopen(vhd_out, "w");
		if (!file) {
			err = -errno;
			ERR("error opening %s: %d\n", vhd_out, err);
			goto out;
		}
	}

	ctx->dst_vhd = vhd_stream_initialize(file, ctx->src_vhd);
	if (!ctx->dst_vhd)
		goto out;

out:
	return err;
}

int
vhd_util_dm_decrypt(int argc, char **argv)
{
	int c, err;
	vhd_context_t src;
	struct vhd_decrypt_context ctx;
	const char *raw_in, *vhd_in, *vhd_out;

	err      = 0;
	raw_in   = NULL;
	vhd_in   = NULL;
	vhd_out  = NULL;

	memset(&src, 0, sizeof(src));
	memset(&ctx, 0, sizeof(ctx));

	ctx.src_raw = -1;

	if (!argc || !argv)
		goto usage;

	optind = 0;
	while ((c = getopt(argc, argv, "i:I:o:ph")) != -1) {
		switch (c) {
		case 'i':
			raw_in = optarg;
			break;
		case 'I':
			vhd_in = optarg;
			break;
		case 'o':
			vhd_out = optarg;
			break;
		case 'p':
			ctx.progress.display = 1;
			break;
		case 'h':
		default:
			goto usage;
		}
	}

	if (optind != argc)
		goto usage;

	if (!raw_in || !vhd_in || !vhd_out)
		goto usage;

	ctx.src_raw = open(raw_in, O_RDONLY | O_LARGEFILE | O_DIRECT);
	if (ctx.src_raw == -1) {
		err = -errno;
		fprintf(stderr, "error opening %s: %d\n", raw_in, err);
		goto out;
	}

	err = vhd_open(&src, vhd_in, VHD_OPEN_RDONLY);
	if (err) {
		fprintf(stderr, "error opening %s: %d\n", vhd_in, err);
		goto out;
	}

	ctx.src_vhd = &src;

	err = vhd_util_dm_decrypt_open_output(&ctx, vhd_out);
	if (err)
		goto out;

	err = __vhd_util_dm_decrypt(&ctx);
	if (err)
		goto out;

out:
	vhd_close(&src);
	if (ctx.src_raw != -1)
		close(ctx.src_raw);
	if (ctx.dst_vhd)
		vhd_close(ctx.dst_vhd);

	if (err && vhd_out && strcmp(vhd_out, "-"))
		unlink(vhd_out);

	return err;

usage:
	printf("vhd-util dm-decrypt reads the allocated data of a given vhd "
	       "dm target and writes it to a new vhd.\n"
	       "Options:\n"
	       "-h          Print this help message.\n"
	       "-p          Display progress.\n"
	       "-o NAME     NAME of output VHD to create ('-' for stdout).\n"
	       "-i NAME     NAME of input device to read.\n"
	       "-I NAME     NAME of input vhd to read.\n");
	return EINVAL;
}
