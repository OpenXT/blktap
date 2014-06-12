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

#define ERR(_fmt, _args...) fprintf(stderr, _fmt, ##_args)

int
__vhd_util_clone_metadata_s(vhd_context_t *vhd, const char *output,
			    uint64_t bytes, uint64_t mbytes, int quick)
{
	int err;
	vhd_context_t vout;

	memset(&vout, 0, sizeof(vout));

	if (!access(output, F_OK)) {
		ERR("%s already exists\n", output);
		return -EEXIST;
	}

	switch (vhd->footer.type) {
	case HD_TYPE_DYNAMIC:
		err = vhd_create(output, bytes, HD_TYPE_DYNAMIC, mbytes, 0);
		break;
	case HD_TYPE_DIFF: {
		int fd;
		char *tmp;

		if (asprintf(&tmp, "%s.XXXXXX", output) == -1) {
			err = -ENOMEM;
			goto out;
		}

		fd = mkstemp(tmp);
		if (fd == -1) {
			err = -errno;
			free(tmp);
			goto out;
		}

		err = vhd_snapshot(output, bytes, tmp,
				   mbytes, VHD_FLAG_CREAT_PARENT_RAW);

		close(fd);
		unlink(tmp);
		free(tmp);
		break;
	}
	default:
		err = -EINVAL;
		break;
	}

	if (err) {
		ERR("error creating %s: %d\n", output, err);
		goto out;
	}

	err = vhd_open(&vout, output, VHD_OPEN_RDWR);
	if (err) {
		ERR("error opening %s: %d\n", output, err);
		goto out;
	}

	/*
	 * if the source vhd doesn't have a batmap, remove the batmap
	 * in the destination vhd to keep the sizes consistent.
	 */
	if (!vhd_has_batmap(vhd)) {
		off64_t off, eob;

		err = vhd_batmap_header_offset(&vout, &off);
		if (err) {
			ERR("error finding batmap: %d\n", err);
			goto out;
		}

		eob = vout.header.table_offset +
			vhd_bytes_padded(vout.header.max_bat_size *
					 sizeof(uint32_t));

		/*
		 * this won't work if the batmap is not located
		 * directly after the bat.
		 */
		if (off != eob) {
			err = -EINVAL;
			ERR("unexpected batmap location\n");
			goto out;
		}

		err = ftruncate(vout.fd, eob);
		if (err) {
			ERR("error removing batmap: %d\n", err);
			goto out;
		}

		/*
		 * update the in-memory footer so library calls
		 * will realize that vout has no batmap.
		 */
		memcpy(&vout.footer, &vhd->footer, sizeof(vout.footer));
	}

	err = vhd_write_footer_at(&vout, &vhd->footer, 0);
	if (err) {
		ERR("error copying backup footer: %d\n", err);
		goto out;
	}

	err = vhd_write_header(&vout, &vhd->header);
	if (err) {
		ERR("error copying header: %d\n", err);
		goto out;
	}

	if (vhd->footer.type == HD_TYPE_DIFF) {
		int i;

		for (i = 0; i < vhd_parent_locator_count(vhd); i++) {
			char *buf;
			off64_t off;
			size_t size;
			vhd_parent_locator_t *loc;

			buf  = NULL;
			loc  = vhd->header.loc + i;
			off  = loc->data_offset;
			size = vhd_parent_locator_size(loc);

			if (!size)
				continue;

			err = posix_memalign((void **)&buf, VHD_SECTOR_SIZE, size);
			if (err) {
				err = -err;
				goto out;
			}

			err = vhd_pread(vhd, buf, size, off);
			if (err) {
				ERR("error reading parent locator: %d\n", err);
				free(buf);
				goto out;
			}

			loc = vout.header.loc + i;
			off = loc->data_offset;

			if (size != vhd_parent_locator_size(loc)) {
				free(buf);
				ERR("parent locator mismatch\n");
				err = -ENOSYS;
				goto out;
			}

			err = vhd_pwrite(&vout, buf, size, loc->data_offset);
			if (err) {
				ERR("error writing parent locator: %d\n", err);
				free(buf);
				goto out;
			}
		}
	}

	if (quick) {
		err = vhd_write_footer(&vout, &vhd->footer);
		if (err) {
			ERR("error writing footer: %d\n", err);
			goto out;
		}
	} else {
		vhd_footer_t footer;

		err = vhd_read_footer(vhd, &footer);
		if (err) {
			ERR("error reading footer: %d\n", err);
			goto out;
		}

		err = vhd_write_footer(&vout, &footer);
		if (err) {
			ERR("error writing footer: %d\n", err);
			goto out;
		}
	}

out:
	vhd_close(&vout);
	if (err)
		unlink(output);
	return err;
}

int
__vhd_util_clone_metadata(vhd_context_t *vhd, const char *output, int quick)
{
	return __vhd_util_clone_metadata_s(vhd, output,
					   vhd_cur_capacity(vhd),
					   vhd_max_capacity(vhd),
					   quick);
}

int
vhd_util_clone_metadata(int argc, char **argv)
{
	int c, quick, err;
	vhd_context_t *vhd;
	const char *input, *output;

	err    = 0;
	quick  = 0;
	vhd    = NULL;
	input  = NULL;
	output = NULL;

	if (!argc || !argv)
		goto usage;

	optind = 0;
	while ((c = getopt(argc, argv, "qi:o:h")) != -1) {
		switch (c) {
		case 'i':
			input = optarg;
			break;
		case 'o':
			output = optarg;
			break;
		case 'q':
			quick = 1;
			break;
		case 'h':
		default:
			goto usage;
		}
	}

	if (optind != argc)
		goto usage;

	if (!input || !output)
		goto usage;

	{
		FILE *f;

		if (!strncmp(input, "-", strlen("-")))
			f = stdin;
		else {
			f = fopen(input, "r");
			if (!f) {
				err = -errno;
				goto out;
			}
		}

		vhd = vhd_stream_load(f);
		if (f != stdin)
			fclose(f);

		if (!vhd) {
			err = -errno;
			goto out;
		}
	}

	err = __vhd_util_clone_metadata(vhd, output, quick);

out:
	if (vhd)
		vhd_close(vhd);
	return err;

usage:
	printf("vhd-util clone-metadata creates an empty vhd with metadata "
	       "identical to the input vhd.\n"
	       "Options:\n"
	       "-h          Print this help message.\n"
	       "-o NAME     NAME of output VHD to be created.\n"
	       "-i NAME     NAME of input VHD to clone ('-' for stdin).\n"
	       "-q          Quick clone -- use input backup footer for both\n"
	       "            output primary and backup footers.\n");
	return EINVAL;
}
