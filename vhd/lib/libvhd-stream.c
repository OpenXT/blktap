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

struct vhd_stream {
	FILE            *stream;
	off64_t          pos;
	vhd_context_t    vhd;
};

#define ERR(_fmt, _args...) fprintf(stderr, "%d: " _fmt, __LINE__, ##_args)

#define VERR(_v, _fmt, _args...)					\
	do {								\
		char uuid[37];						\
		uuid_unparse((_v)->vhd.footer.uuid, uuid);		\
		ERR("%s: " _fmt, uuid, ##_args);			\
	} while (0)							\

#define vhd_to_stream(_vhd)						\
	((struct vhd_stream *)((unsigned long)(_vhd) -			\
			       offsetof(struct vhd_stream, vhd)))

static off64_t
vhd_stream_position(struct vhd_stream *vstream)
{
	if (!vstream->stream)
		return (off64_t) -EBADF;
	return vstream->pos;
}

static int
vhd_stream_read(struct vhd_stream *vstream, void *buf, size_t count)
{
	int err;

	if (!vstream->stream) {
		err = -EBADF;
		goto out;
	}

	if (fread(buf, count, 1, vstream->stream) == 1) {
		err = 0;
		vstream->pos += count;
	} else {
		err = ferror(vstream->stream);
		if (err)
			err = (errno ? -errno : -EIO);
		else
			err = -EIO;
	}

out:
	if (err)
		VERR(vstream, "error reading 0x%x bytes: %d\n", count, err);
	return err;
}

static int
vhd_stream_write(struct vhd_stream *vstream, const void *buf, size_t count)
{
	int err;

	if (!vstream->stream) {
		err = -EBADF;
		goto out;
	}

	if (fwrite(buf, count, 1, vstream->stream) == 1 &&
	    !fflush(vstream->stream)) {
		err = 0;
		vstream->pos += count;
	} else {
		err = ferror(vstream->stream);
		if (err)
			err = (errno ? -errno : -EIO);
		else
			err = -EIO;
	}

out:
	if (err)
		VERR(vstream, "error writing 0x%x bytes: %d\n", count, err);
	return err;
}

static int
vhd_stream_seek(struct vhd_stream *vstream, off64_t off, int whence)
{
	int err, rw;
	off64_t pos;
	char buf[4096];

	rw = vhd_flag_test(vstream->vhd.oflags, VHD_OPEN_RDWR);

	if (!vstream->stream) {
		err = -EBADF;
		goto out;
	}


	err = fseeko(vstream->stream, off, whence);
	if (!err) {
		vstream->pos = off;
		goto out;
	} else if (errno != ESPIPE) {
		err = -errno;
		goto out;
	}

	switch (whence) {
	case SEEK_SET:
		pos = off;
		break;
	case SEEK_CUR:
		pos = vstream->pos + off;
		break;
	case SEEK_END:
		err = -ESPIPE;
		goto out;
	default:
		err = -EINVAL;
		goto out;
	}

	if (pos < 0) {
		err = -EINVAL;
		goto out;
	}

	if (pos < vstream->pos) {
		err = -ESPIPE;
		goto out;
	}

	err = 0;

	if (rw && vstream->pos < pos)
		memset(buf, 0, sizeof(buf));

	while (vstream->pos < pos) {
		size_t size = sizeof(buf);

		if (size > (pos - vstream->pos))
			size = (pos - vstream->pos);

		if (rw)
			err = vhd_stream_write(vstream, buf, size);
		else
			err = vhd_stream_read(vstream, buf, size);

		if (err)
			goto out;
	}

out:
	if (err)
		VERR(vstream, "error seeking 0x%llx 0x%x: %d\n",
		     off, whence, err);
	return err;
}

static int
vhd_stream_pread(struct vhd_stream *vstream,
		 void *buf, size_t size, off64_t off)
{
	int err;

	if (!vstream->stream) {
		err = -EBADF;
		goto out;
	}

	err = vhd_stream_seek(vstream, off, SEEK_SET);
	if (err)
		goto out;

	err = vhd_stream_read(vstream, buf, size);
	if (err)
		goto out;

out:
	if (err)
		VERR(vstream, "error reading 0x%x bytes at 0x%llx: %d\n",
		     size, off, err);
	return err;
}

static int
vhd_stream_pwrite(struct vhd_stream *vstream,
		  const void *buf, size_t size, off64_t off)
{
	int err;

	if (!vstream->stream) {
		err = -EBADF;
		goto out;
	}

	err = vhd_stream_seek(vstream, off, SEEK_SET);
	if (err)
		goto out;

	err = vhd_stream_write(vstream, buf, size);
	if (err)
		goto out;

out:
	if (err)
		VERR(vstream, "error reading 0x%x bytes at 0x%llx: %d\n",
		     size, off, err);
	return err;
}

static void
vhd_stream_close(struct vhd_stream *vstream)
{
	if (vstream->stream)
		fclose(vstream->stream);
	vstream->vhd.devops = NULL;
	vhd_close(&vstream->vhd);
	free(vstream);
}

static off64_t
vhd_stream_devops_position(vhd_context_t *vhd)
{
	return vhd_stream_position(vhd_to_stream(vhd));
}

static int
vhd_stream_devops_seek(vhd_context_t *vhd, off64_t off, int whence)
{
	return vhd_stream_seek(vhd_to_stream(vhd), off, whence);
}

static int
vhd_stream_devops_read(vhd_context_t *vhd, void *buf, size_t count)
{
	return vhd_stream_read(vhd_to_stream(vhd), buf, count);
}

static int
vhd_stream_devops_pread(vhd_context_t *vhd,
			void *buf, size_t size, off64_t off)
{
	return vhd_stream_pread(vhd_to_stream(vhd), buf, size, off);
}

static int
vhd_stream_devops_write(vhd_context_t *vhd, void *buf, size_t count)
{
	return vhd_stream_write(vhd_to_stream(vhd), buf, count);
}

static int
vhd_stream_devops_pwrite(vhd_context_t *vhd,
			 void *buf, size_t size, off64_t off)
{
	return vhd_stream_pwrite(vhd_to_stream(vhd), buf, size, off);
}

static void
vhd_stream_devops_close(vhd_context_t *vhd)
{
	vhd_stream_close(vhd_to_stream(vhd));
}

vhd_devops_t vhd_stream_devops = {
	.position = vhd_stream_devops_position,
	.seek     = vhd_stream_devops_seek,
	.read     = vhd_stream_devops_read,
	.write    = vhd_stream_devops_write,
	.pread    = vhd_stream_devops_pread,
	.pwrite   = vhd_stream_devops_pwrite,
	.close    = vhd_stream_devops_close,
};

struct vhd_stream *
vhd_stream_allocate(FILE *stream, const char *mode)
{
	int fd, err;
	struct vhd_stream *vstream;

	fd      = -1;
	err     = 0;
	vstream = NULL;

	vstream = calloc(1, sizeof(*vstream));
	if (!vstream) {
		err = -ENOMEM;
		goto out;
	}

	fd = dup(fileno(stream));
	if (fd == -1) {
		err = -errno;
		goto out;
	}

	vstream->stream = fdopen(fd, mode);
	if (!vstream->stream) {
		err = -errno;
		goto out;
	}

	vstream->vhd.devops = &vhd_stream_devops;

out:
	if (err) {
		if (fd != -1)
			close(fd);

		free(vstream);
		vstream = NULL;

		errno = -err;
	}
	return vstream;
}

/**
 * vhd_stream_load(): initialize a vhd_context_t from a stream of data
 *
 * @stream: the stream (fifo, pipe, etc.) containing the vhd
 *
 * This function dups the input stream and attempts to initialize a vhd context
 * from the stream's data.  The resulting vhd_context can be used as any file-
 * based context would be, with the exception that forward seeks may take a
 * long time, backward seeks will fail with ESPIPE, and writes/pwrites will
 * fail with ENOSYS.
 */
vhd_context_t *
vhd_stream_load(FILE *stream)
{
	int err;
	vhd_context_t *vhd;
	struct vhd_stream *vstream;

	vhd = NULL;

	vstream = vhd_stream_allocate(stream, "rb");
	if (!vstream) {
		err = -ENOMEM;
		goto out;
	}

	vhd = &vstream->vhd;
	vhd->oflags = VHD_OPEN_RDONLY;

	err = vhd_read_footer_at(vhd, &vhd->footer, 0);
	if (err)
		goto out;

	err = vhd_read_header(vhd, &vhd->header);
	if (err)
		goto out;

	vhd->spb     = vhd->header.block_size >> VHD_SECTOR_SHIFT;
	vhd->bm_secs = secs_round_up_no_zero(vhd->spb >> 3);

	err = vhd_read_bat(vhd, &vhd->bat);
	if (err)
		goto out;

	if (vhd_has_batmap(vhd)) {
		err = vhd_read_batmap(vhd, &vhd->batmap);
		if (err)
			goto out;
	}

out:
	if (err) {
		vhd = NULL;
		errno = -err;
		ERR("error loading stream: %d\n", err);
		if (vstream)
			vhd_stream_close(vstream);
	}
	return vhd;
}

/**
 * vhd_stream_initialize() - initialze a vhd_stream, possibly cloning @in
 *
 * @stream: the stream to be associated with this vhd
 * @in: (an optional) vhd whose metadata will be used to initialize the stream
 *
 * vhd_stream_initialize prepares a vhd_stream for further use.  If @in is not
 * NULL, its metadata (footer, header, bat, and batmap) will be used to
 * populate the stream metadata.
 */
vhd_context_t *
vhd_stream_initialize(FILE *stream, vhd_context_t *in)
{
	int err;
	char *buf;
	size_t size;
	vhd_context_t *out;
	struct vhd_stream *vstream;

	err     = 0;
	buf     = NULL;
	out     = NULL;
	vstream = NULL;

	vstream = vhd_stream_allocate(stream, "wb");
	if (!vstream) {
		err = -ENOMEM;
		goto out;
	}

	if (!in) {
		out = &vstream->vhd;
		goto out;
	}

	if (!vhd_type_dynamic(in)) {
		err = -EINVAL;
		goto out;
	}

	err = vhd_get_footer(in);
	if (err)
		goto out;

	err = vhd_get_header(in);
	if (err)
		goto out;

	err = vhd_get_bat(in);
	if (err)
		goto out;

	if (vhd_has_batmap(in)) {
		err = vhd_get_batmap(in);
		if (err)
			goto out;
	}

	out = &vstream->vhd;
	vstream->stream = stream;
	out->oflags = VHD_OPEN_RDWR;

	memcpy(&out->footer, &in->footer, sizeof(out->footer));
	memcpy(&out->header, &in->header, sizeof(out->header));

	size = vhd_bytes_padded(in->bat.entries * sizeof(uint32_t));

	err = posix_memalign((void **)&buf, VHD_SECTOR_SIZE, size);
	if (err) {
		buf = NULL;
		err = -err;
		goto out;
	}

	out->spb         = in->spb;
	out->bm_secs     = in->bm_secs;
	out->bat.spb     = in->bat.spb;
	out->bat.entries = in->bat.entries;
	out->bat.bat     = (uint32_t *)buf;
	memcpy(out->bat.bat, in->bat.bat, size);

	if (vhd_has_batmap(in)) {
		size = vhd_bytes_padded(in->footer.curr_size /
					(in->header.block_size * 8));

		err = posix_memalign((void **)&buf, VHD_SECTOR_SIZE, size);
		if (err) {
			buf = NULL;
			err = -err;
			goto out;
		}

		out->batmap.map = buf;
		memcpy(out->batmap.map, in->batmap.map, size);

		memcpy(&out->batmap.header,
		       &in->batmap.header, sizeof(out->batmap.header));
	}

out:
	if (err) {
		free(buf);
		if (out)
			vhd_close(out);
		vstream = NULL;
	}
	return out;
}
