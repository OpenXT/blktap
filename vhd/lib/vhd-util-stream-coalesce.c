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
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>

#include "libvhd.h"
#include "vhd-util.h"

struct vhd_stream_stats {
	char             display;
	uint32_t         total;
	uint32_t         cur;
};

#define ERR(_fmt, _args...) fprintf(stderr, "%d: " _fmt, __LINE__, ##_args)

#define VERR(_v, _fmt, _args...)					\
	do {								\
		char uuid[37];						\
		uuid_unparse((_v)->footer.uuid, uuid);			\
		ERR("%s: " _fmt, uuid, ##_args);			\
	} while (0)							\

#define PROGRESS(_stats)						\
	do {								\
		if ((_stats)->display && (_stats)->total) {		\
			printf("\r%6.2f%%",				\
			       ((float)(_stats)->cur /			\
				(float)(_stats)->total) * 100.0);	\
			fflush(stdout);					\
		}							\
	} while (0)

static void
vhd_util_stream_swap(vhd_context_t **vhds, int i, int j)
{
	vhd_context_t *tmp;

	if (i == j)
		return;

	tmp     = vhds[i];
	vhds[i] = vhds[j];
	vhds[j] = tmp;
}

static int
vhd_util_stream_sort(vhd_context_t **vhds, int num)
{
	int i, err, head;

	err  = -EINVAL;
	head = -1;

	for (i = 0; i < num; i++) {
		int j;
		uuid_t cur;

		uuid_copy(cur, vhds[i]->footer.uuid);

		for (j = 0; j < num; j++) {
			if (vhds[j]->footer.type == HD_TYPE_DIFF) {
				uuid_t tmp;

				uuid_copy(tmp, vhds[j]->header.prt_uuid);

				if (!uuid_compare(cur, tmp))
					break;
			}
		}

		if (j == num) {
			if (head != -1) {
				ERR("multiple children found\n");
				goto out;
			}
			head = i;
		}
	}

	if (head == -1) {
		ERR("child VHD not found\n");
		goto out;
	}

	vhd_util_stream_swap(vhds, head, 0);

	for (i = 0; i < num - 1; i++) {
		int j;
		uuid_t parent;

		if (vhds[i]->footer.type != HD_TYPE_DIFF) {
			VERR(vhds[i], "non-differencing VHD found\n");
			goto out;
		}

		uuid_copy(parent, vhds[i]->header.prt_uuid);

		for (j = i + 1; j < num; j++) {
			uuid_t tmp;

			uuid_copy(tmp, vhds[j]->footer.uuid);

			if (!uuid_compare(tmp, parent)) {
				vhd_util_stream_swap(vhds, i + 1, j);
				break;
			}
		}

		if (j == num) {
			ERR("VHD parent not found\n");
			goto out;
		}
	}

	for (i = 0; i < num - 1; i++) {
		uuid_t parent, next;

		uuid_copy(parent, vhds[i]->header.prt_uuid);
		uuid_copy(next, vhds[i + 1]->footer.uuid);

		if (uuid_compare(parent, next)) {
			VERR(vhds[i], "VHD sort failed\n");
			goto out;
		}
	}

	err = 0;

out:
	return err;
}

static int
vhd_util_stream_transfer_sectors(vhd_context_t *src, vhd_context_t *dst,
				 uint32_t blk, uint32_t sec, uint32_t cnt)
{
	int err;
	char *buf;
	size_t size;
	off64_t off;
	uint64_t sout;

	size = vhd_sectors_to_bytes(cnt);
	sout = (uint64_t)blk * dst->spb + sec;
	off  = src->bat.bat[blk] + src->bm_secs + sec;

	err = posix_memalign((void **)&buf, VHD_SECTOR_SIZE, size);
	if (err) {
		err = -err;
		buf = NULL;
		VERR(src, "error allocating data buffer: %d\n", err);
		goto out;
	}

	err = vhd_pread(src, buf, size, vhd_sectors_to_bytes(off));
	if (err) {
		VERR(src, "error reading from stream\n");
		goto out;
	}

	err = vhd_io_write(dst, buf, sout, cnt);
	if (err) {
		VERR(src, "error writing 0x%x sectors at 0x%llx to output: "
		     "%d\n", cnt, sout, err);
		goto out;
	}

out:
	free(buf);
	return err;
}

static int
vhd_util_stream_copy_block(vhd_context_t *src,
			   vhd_context_t *dst, uint32_t blk)
{
	int err, i;
	char *sbm, *dbm;

	sbm = NULL;
	dbm = NULL;

	if (src->header.block_size != dst->header.block_size) {
		err = -EINVAL;
		VERR(src, "src and dst have different block sizes\n");
		goto out;
	}

	if ((uint64_t)blk * dst->header.block_size > dst->footer.curr_size) {
		err = -EINVAL;
		VERR(src, "block 0x%x beyond end of dst\n", blk);
		goto out;
	}

	if ((uint64_t)blk * src->header.block_size > src->footer.curr_size ||
	    src->bat.bat[blk] == DD_BLK_UNUSED) {
		err = 0;
		goto out;
	}

	if (vhd_has_batmap(dst) && vhd_batmap_test(dst, &dst->batmap, blk)) {
		err = 0;
		goto out;
	}

	if (dst->bat.bat[blk] == DD_BLK_UNUSED) {
		dbm = calloc(1, vhd_sectors_to_bytes(dst->bm_secs));
		if (!dbm) {
			err = -ENOMEM;
			VERR(src, "allocating bitmap");
			goto out;
		}
	} else {
		err = vhd_read_bitmap(dst, blk, &dbm);
		if (err) {
			VERR(src, "error reading dst bitmap for block 0x%x: "
			     "%d\n", blk, err);
			goto out;
		}
	}

	err = vhd_read_bitmap(src, blk, &sbm);
	if (err) {
		ERR("error reading source bitmap for "
		    "block 0x%x: %d\n", blk, err);
		goto out;
	}

	i = 0;
	while (i < src->spb) {
		int cnt, copy;

		cnt  = 1;
		copy = vhd_bitmap_test(src, sbm, i) &&
			!vhd_bitmap_test(dst, dbm, i);

		while (i + cnt < src->spb &&
		       (copy == (vhd_bitmap_test(src, sbm, i + cnt) &&
				 !vhd_bitmap_test(dst, dbm, i + cnt))))
			cnt++;

		if (copy) {
			err = vhd_util_stream_transfer_sectors(src, dst,
							       blk, i, cnt);
			if (err)
				goto out;
		}

		i += cnt;
	}

out:
	free(sbm);
	free(dbm);
	return err;
}

#define p2v_entry(physical, virtual) (((uint64_t)(physical) << 32) | (virtual))
#define p2v_physical(entry)          ((uint32_t)((entry) >> 32))
#define p2v_virtual(entry)           ((uint32_t)((entry) & ((1ULL << 32) - 1)))

static int
p2v_compare(const void *p1, const void *p2)
{
	uint32_t phy1 = p2v_physical(*(uint64_t *)p1);
	uint32_t phy2 = p2v_physical(*(uint64_t *)p2);
	if (phy1 == phy2)
		return 0;
	return (phy1 < phy2 ? -1 : 1);
}

static int
vhd_util_stream_coalesce_one(vhd_context_t *src, vhd_context_t *dst,
			     struct vhd_stream_stats *stats)
{
	int err;
	uint32_t i;
	uint64_t *p2v;

	p2v = malloc(src->bat.entries * sizeof(*p2v));
	if (!p2v) {
		err = -ENOMEM;
		VERR(src, "allocating p2v map");
		goto out;
	}

	for (i = 0; i < src->bat.entries; i++)
		p2v[i] = p2v_entry(src->bat.bat[i], i);

	qsort(p2v, src->bat.entries, sizeof(*p2v), p2v_compare);

	for (i = 0; i < src->bat.entries; i++) {
		uint32_t phys = p2v_physical(p2v[i]);
		uint32_t virt = p2v_virtual(p2v[i]);

		if (phys != DD_BLK_UNUSED) {
			PROGRESS(stats);
			stats->cur++;

			err = vhd_util_stream_copy_block(src, dst, virt);
			if (err)
				goto out;
		}
	}

out:
	free(p2v);
	return err;
}

static int
vhd_util_stream_open_output(vhd_context_t *src, vhd_context_t *tail,
			    vhd_context_t *dst, const char *output)
{
	int err;

	memset(dst, 0, sizeof(*dst));

	if (access(output, F_OK) == 0) {
		err = -EEXIST;
		ERR("%s already exists\n", output);
		goto out;
	}

	err = __vhd_util_clone_metadata_s(tail, output,
					  vhd_cur_capacity(src),
					  vhd_max_capacity(src),
					  1);
	if (err) {
		ERR("error creating %s: %d\n", output, err);
		goto out;
	}

	err = vhd_open(dst, output, VHD_OPEN_RDWR);
	if (err) {
		ERR("error opening %s: %d\n", output, err);
		goto out;
	}

	dst->footer.timestamp = src->footer.timestamp;
	uuid_copy(dst->footer.uuid, src->footer.uuid);

	err = vhd_write_footer(dst, &dst->footer);
	if (err) {
		ERR("error creating %s: %d\n", output, err);
		goto out;
	}

	err = vhd_get_bat(dst);
	if (err) {
		ERR("error reading bat for %s: %d\n", output, err);
		goto out;
	}

	if (vhd_has_batmap(dst)) {
		err = vhd_get_batmap(dst);
		if (err) {
			ERR("error reading batmap for %s: %d\n", output, err);
			goto out;
		}
	}

out:
	if (err) {
		vhd_close(dst);
		memset(dst, 0, sizeof(*dst));
	}
	return err;
}

static int
__vhd_util_stream_coalesce(vhd_context_t **vhds, const int num,
			   const char *output, struct vhd_stream_stats *stats)
{
	int i, err;
	vhd_context_t dst, *src;

	src = vhds[0];
	memset(&dst, 0, sizeof(dst));

	err = vhd_util_stream_open_output(src, vhds[num - 1], &dst, output);
	if (err) {
		if (err == -EEXIST)
			return err;
		else
			goto out;
	}

	for (i = 0; i < num; i++) {
		uint32_t blk;
		src = vhds[i];
		for (blk = 0; blk < src->bat.entries; blk++)
			if (src->bat.bat[blk] != DD_BLK_UNUSED)
				stats->total++;
	}

	if (stats->display) {
		stats->cur = 0;
		PROGRESS(stats);
	}

	for (i = 0; i < num; i++) {
		src = vhds[i];
		err = vhd_util_stream_coalesce_one(src, &dst, stats);
		if (err)
			goto out;
	}

	if (stats->display) {
		stats->cur = stats->total;
		PROGRESS(stats);
		printf("\n");
	}

out:
	vhd_close(&dst);
	if (err)
		unlink(output);
	return err;
}

int
vhd_util_stream_coalesce(int argc, char **argv)
{
	const char *output;
	vhd_context_t **vhds;
	struct vhd_stream_stats stats;
	int c, i, err, cnt, hex, info, ignore_order;

	hex    = 0;
	info   = 0;
	err    = -EINVAL;
	output = NULL;
	ignore_order = 0;

	memset(&stats, 0, sizeof(stats));

	if (!argc || !argv)
		goto usage;

	optind = 0;
	while ((c = getopt(argc, argv, "ixDpo:h")) != -1) {
		switch (c) {
		case 'i':
			info = 1;
			break;
		case 'x':
			hex = 1;
			break;
		case 'p':
			stats.display = 1;
			break;
		case 'D':
			ignore_order = 1;
			break;
		case 'o':
			output = optarg;
			break;
		case 'h':
		default:
			goto usage;
		}
	}

	cnt = argc - optind;
	if (!cnt)
		goto usage;

	if (!info && !output)
		goto usage;

	vhds = calloc(cnt, sizeof(*vhds));
	if (!vhds) {
		err = -ENOMEM;
		ERR("allocating streams");
		goto out;
	}

	for (i = 0; i < cnt; i++) {
		FILE *f;

		f = fopen(argv[optind + i], "r");
		if (!f) {
			err = -errno;
			goto out;
		}

		vhds[i] = vhd_stream_load(f);
		fclose(f);

		if (!vhds[i]) {
			err = -errno;
			goto out;
		}
	}

	if (info) {
		for (i = 0; i < cnt; i++) {
			vhd_print_headers(vhds[i], hex);
			if (i + 1 < cnt)
				printf("\n\n");
		}
	} else {
		err = vhd_util_stream_sort(vhds, cnt);
		if (err) {
			if (ignore_order)
				ERR("WARNING: continuing in spite of "
				    "mal-ordered VHDs.  The output VHD "
				    "may not contain what you expect.\n");
			else
				goto out;
		}

		err = __vhd_util_stream_coalesce(vhds, cnt, output, &stats);
	}

out:
	if (vhds) {
		for (i = 0; i < cnt; i++)
			if (vhds[i])
				vhd_close(vhds[i]);
		free(vhds);
	}
	return err;

usage:
	printf("vhd-util stream-coalesce accepts a chain of VHD streams as\n"
	       "input and produces a single, coalesced version of the chain.\n"
	       "All intput VHDs supplied to this utility should be part of\n"
	       "the same VHD chain, and should be ordered from youngest to\n"
	       "oldest.  VHD UUIDs are checked to verify proper ordering.\n"
	       "Example: vhd-util stream <(cat child.vhd) <(cat parent.vhd) "
	       "-o output.vhd\n"
	       "Options:\n"
	       "-h          Print this help message.\n"
	       "-o NAME     NAME of output VHD to be created.\n"
	       "-D          Disable checking VHD UUIDs for proper ordering.\n"
	       "            Only use this if you know what you are doing.\n"
	       "-p          Display coalesce progress.\n"
	       "-i          Print basic info about the VHDs and exit.\n"
	       "            (No output VHD is created in this case.)\n"
	       "-x          Print in hex.\n");
	return EINVAL;
}
