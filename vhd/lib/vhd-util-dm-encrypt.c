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
#include <sys/stat.h>

#include "libvhd.h"
#include "vhd-util.h"

#define ERR(_fmt, _args...) fprintf(stderr, "%d: " _fmt, __LINE__, ##_args)

#define VERR(_v, _fmt, _args...)					\
	do {								\
		char uuid[37];						\
		uuid_unparse((_v)->footer.uuid, uuid);			\
		ERR("%s: " _fmt, uuid, ##_args);			\
	} while (0)							\

static int
vhd_util_stream_transfer_sectors(vhd_context_t *src, int fd,
				 uint32_t blk, uint32_t sec, uint32_t cnt)
{
	int err;
	char *buf;
	off64_t off;
	size_t size;
	ssize_t ret;
	uint64_t sout;

	size = vhd_sectors_to_bytes(cnt);
	sout = vhd_sectors_to_bytes((uint64_t)blk * src->spb + sec);
	off  = src->bat.bat[blk] + src->bm_secs + sec;

	err = posix_memalign((void **)&buf, VHD_SECTOR_SIZE, size);
	if (err) {
		err = -err;
		buf = NULL;
		goto out;
	}

	err = vhd_pread(src, buf, size, vhd_sectors_to_bytes(off));
	if (err) {
		VERR(src, "error reading from stream\n");
		goto out;
	}

	errno = 0;
	ret = pwrite(fd, buf, size, sout);
	if (ret != size) {
		err = (errno ? -errno : -EIO);
		VERR(src, "error writing 0x%x sectors at 0x%llx to output: "
		     "%d\n", cnt, sout, err);
		goto out;
	}

out:
	free(buf);
	return err;
}

static int
vhd_util_stream_allocate_block(vhd_context_t *src, int fd, uint32_t blk)
{
	int err;
	char *buf;
	off64_t off;
	ssize_t ret;

	off = vhd_sectors_to_bytes((uint64_t)blk * src->spb);
	err = posix_memalign((void **)&buf, VHD_SECTOR_SIZE, VHD_SECTOR_SIZE);
	if (err) {
		err = -err;
		buf = NULL;
		goto out;
	}

	memset(buf, 0, VHD_SECTOR_SIZE);

	errno = 0;
	ret = pwrite(fd, buf, VHD_SECTOR_SIZE, off);
	if (ret != VHD_SECTOR_SIZE) {
		err = (errno ? -errno : -EIO);
		VERR(src, "error allocating block 0x%x: %d\n", blk, err);
	}

out:
	free(buf);
	return err;
}

static int
vhd_util_stream_copy_block(vhd_context_t *src, int fd, uint32_t blk)
{
	char *bm;
	int err, i, allocated;

	bm = NULL;
	allocated = 0;

	if (src->bat.bat[blk] == DD_BLK_UNUSED) {
		err = 0;
		goto out;
	}

	err = vhd_read_bitmap(src, blk, &bm);
	if (err) {
		ERR("error reading source bitmap for "
		    "block 0x%x: %d\n", blk, err);
		goto out;
	}

	i = 0;
	while (i < src->spb) {
		int cnt, copy;

		cnt  = 1;
		copy = vhd_bitmap_test(src, bm, i);

		while (i + cnt < src->spb &&
		       copy == vhd_bitmap_test(src, bm, i + cnt))
			cnt++;

		if (copy) {
			err = vhd_util_stream_transfer_sectors(src, fd,
							       blk, i, cnt);
			if (err)
				goto out;

			allocated = 1;
		}

		i += cnt;
	}

	if (!allocated) {
		/*
		 * The BAT says this block is allocated, but it has an empty
		 * bitmap. In general we are safe not writing any data, but to
		 * force the output VHD size to match the original VHD size,
		 * we'll write one sector of zeros here to allocate the block.
		 */
		err = vhd_util_stream_allocate_block(src, fd, blk);
	}

out:
	free(bm);
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
__vhd_util_dm_encrypt(vhd_context_t *src, const char *output, int progress)
{
	int fd, err;
	uint32_t i;
	uint64_t *p2v, cur, total;

	cur   = 0;
	total = 0;

	fd = open(output, O_WRONLY | O_LARGEFILE | O_DIRECT);
	if (fd == -1) {
		err = -errno;
		goto out;
	}

	p2v = malloc(src->bat.entries * sizeof(*p2v));
	if (!p2v) {
		err = -ENOMEM;
		goto out;
	}

	for (i = 0; i < src->bat.entries; i++) {
		if (progress && src->bat.bat[i] != DD_BLK_UNUSED)
			total++;
		p2v[i] = p2v_entry(src->bat.bat[i], i);
	}

	qsort(p2v, src->bat.entries, sizeof(*p2v), p2v_compare);

	for (i = 0; i < src->bat.entries; i++) {
		uint32_t phys = p2v_physical(p2v[i]);
		uint32_t virt = p2v_virtual(p2v[i]);

		if (phys != DD_BLK_UNUSED) {
			if (progress && total) {
				printf("\r%6.2f%%",
				       ((float)cur / (float)total) * 100.0);
				fflush(stdout);
				cur++;
			}

			err = vhd_util_stream_copy_block(src, fd, virt);
			if (err)
				goto out;
		}
	}

	if (progress) {
		printf("\r%6.2f%%\n", 100.0);
		fflush(stdout);
	}

out:
	free(p2v);
	close(fd);
	return err;
}

static int
vhd_util_drain_fifo(FILE *fifo)
{
	ssize_t ret;
	char buf[4096];

	while ((ret = fread(buf, sizeof(buf), 1, fifo)))
		if (ret == -1 && errno != EAGAIN)
			return -errno;

	return 0;
}

static int
vhd_util_instantiate_output(const char *command)
{
	FILE *cmd;
	int err, ret;

	cmd = popen(command, "r");
	if (!cmd) {
		err = -errno;
		goto out;
	}

	err = vhd_util_drain_fifo(cmd);

	ret = pclose(cmd);
	if (ret)
		err = ret;

out:
	return err;
}

int
vhd_util_dm_encrypt(int argc, char **argv)
{
	FILE *file;
	vhd_context_t *vhd;
	int c, fd, err, progress;
	const char *input, *raw_out, *vhd_out, *command;

	err      = 0;
	progress = 0;
	vhd      = NULL;
	file     = NULL;
	input    = NULL;
	raw_out  = NULL;
	vhd_out  = NULL;
	command  = NULL;

	if (!argc || !argv)
		goto usage;

	optind = 0;
	while ((c = getopt(argc, argv, "i:o:c:C:ph")) != -1) {
		switch (c) {
		case 'i':
			input = optarg;
			break;
		case 'o':
			raw_out = optarg;
			break;
		case 'c':
			vhd_out = optarg;
			break;
		case 'C':
			command = optarg;
			break;
		case 'p':
			progress = 1;
			break;
		case 'h':
		default:
			goto usage;
		}
	}

	if (optind != argc)
		goto usage;

	if (!input || !raw_out)
		goto usage;

	if ((!!vhd_out) ^ (!!command))
		goto usage;

	if (!strcmp(input, "-"))
		file = stdin;
	else {
		file = fopen(input, "r");
		if (!file) {
			err = -errno;
			goto out;
		}
	}

	vhd = vhd_stream_load(file);
	if (!vhd) {
		err = -errno;
		ERR("error loading vhd from %s: %d\n", input, err);
		goto out;
	}

	if (vhd_out) {
		err = __vhd_util_clone_metadata(vhd, vhd_out, 1);
		if (err) {
			ERR("error creating %s: %d\n", vhd_out, err);
			goto out;
		}

		err = vhd_util_instantiate_output(command);
		if (err) {
			ERR("error running %s: %d\n", command, err);
			goto cleanup;
		}
	}

	err = __vhd_util_dm_encrypt(vhd, raw_out, progress);
	if (err) {
		ERR("error encrypting data: %d\n", err);
		goto cleanup;
	}

	fd = fileno(file);
	if (fd != -1) {
		struct stat st;

		if (!fstat(fd, &st) && S_ISFIFO(st.st_mode))
			vhd_util_drain_fifo(file);
 	}

cleanup:
	if (err && vhd_out)
		unlink(vhd_out);
out:
	if (vhd)
		vhd_close(vhd);
	if (file)
		fclose(file);
	return err;

usage:
	printf("vhd-util dm-encrypt writes the allocated data of a given vhd "
	       "to a given file/device.\n"
	       "\n"
	       "Optionally, dm-encrypt can create the vhd it will write to.\n"
	       "In this case, the -c switch designates the name of the vhd "
	       "to be created,\n"
	       "and the -C switch designates a command to be used (via popen) "
	       "to instantiate\n"
	       "the vhd as the device with the name specified by the -o "
	       "switch.\n\n"
	       "Example: cat clear.vhd |\n"
	       "    vhd-util dm-encrypt -i - -o /dev/mapper/encrypt-dev -c "
	       "encrypt.vhd \\\n"
	       "             -C 'command to instantiate encrypt.vhd as "
	       "encrypt-dev'\n"
	       "\nThis will create encrypt.vhd with metadata cloned from "
	       "clear.vhd,\n"
	       "instantiate encrypt-dev over encrypt.vhd via the -C command,\n"
	       "and write the data from clear.vhd to encrypt-dev.\n"
	       "\n"
	       "Options:\n"
	       "-h          Print this help message.\n"
	       "-p          Display progress.\n"
	       "-o NAME     NAME of file/device to write to.\n"
	       "-i NAME     NAME of input VHD to copy ('-' for stdin).\n"
	       "-c NAME     NAME of vhd to create (requires -C option).\n"
	       "-C COMMAND  COMMAND to instantiate created vhd.\n");
	return EINVAL;
}
