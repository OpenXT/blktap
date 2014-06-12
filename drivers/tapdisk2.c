/*
 * Copyright (c) 2008, XenSource Inc.
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
 * Copyright (c) 2010 Citrix Systems, Inc.
 */

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include "tapdisk.h"
#include "tapdisk-utils.h"
#include "tapdisk-server.h"
#include "tapdisk-control.h"

static void
usage(const char *app, int err)
{
	fprintf(stderr, "usage: %s [-D don't daemonize] [-h help]\n", app);
	exit(err);
}

static FILE *
fdup(FILE *stream, const char *mode)
{
	int fd, err;
	FILE *f;

	fd = dup(STDOUT_FILENO);
	if (fd < 0)
		goto fail;

	f = fdopen(fd, mode);
	if (!f)
		goto fail;

	return f;

fail:
	err = -errno;
	if (fd >= 0)
		close(fd);
	errno = -err;

	return NULL;
}

/*
 * put tapdisk pids in non-volatile directory for debugging purposes
 */
#define TAPDISK2_PID_DIRECTORY "/var/log/tapdisk-pids"
static char tapdisk2_pid_file[512] = { 0 };

static int
tapdisk2_create_pid_file(void)
{
	int fd;

	if (access(TAPDISK2_PID_DIRECTORY, X_OK))
		return (errno == ENOENT ? 0 : -errno);

	snprintf(tapdisk2_pid_file, sizeof(tapdisk2_pid_file),
		 "%s/%u", TAPDISK2_PID_DIRECTORY, getpid());

	fd = open(tapdisk2_pid_file, O_CREAT | O_EXCL, 0644);
	if (fd == -1)
		return -errno;

	close(fd);
	return 0;
}

static int
tapdisk2_remove_pid_file(void)
{
	if (!tapdisk2_pid_file[0])
		return -EINVAL;

	return (unlink(tapdisk2_pid_file) == -1 ? -errno : 0);
}

int
main(int argc, char *argv[])
{
	char *control;
	int c, err, nodaemon, fd;
	FILE *out;

	control  = NULL;
	nodaemon = 0;

	while ((c = getopt(argc, argv, "Dh")) != -1) {
		switch (c) {
		case 'D':
			nodaemon = 1;
			break;
		case 'h':
			usage(argv[0], 0);
			break;
		default:
			usage(argv[0], EINVAL);
		}
	}

	if (optind != argc)
		usage(argv[0], EINVAL);

	err = tapdisk_server_init();
	if (err) {
		DPRINTF("failed to initialize server: %d\n", err);
		goto out;
	}

	out = fdup(stdout, "w");
	if (!out) {
		err = -errno;
		DPRINTF("failed to dup stdout: %d\n", err);
		goto out;
	}

	if (!nodaemon) {
		err = daemon(0, 0);
		if (err) {
			DPRINTF("failed to daemonize: %d\n", errno);
			goto out;
		}
	}

	tapdisk_start_logging("tapdisk", NULL);

	err = tapdisk_control_open(&control);
	if (err) {
		DPRINTF("failed to open control socket: %d\n", err);
		goto out;
	}

	err = tapdisk_server_complete();
	if (err) {
		DPRINTF("failed to complete server: %d\n", err);
		goto out;
	}

	fprintf(out, "%s\n", control);
	fclose(out);

	err = tapdisk2_create_pid_file();
	if (err) {
		DPRINTF("failed to create pid file: %d\n", err);
		goto out;
	}

	err = tapdisk_server_run();
	if (!err)
		tapdisk2_remove_pid_file();

out:
	tapdisk_control_close();
	tapdisk_stop_logging();
	return -err;
}
