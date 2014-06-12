/*
 * Copyright (c) 2011 Citrix Systems, Inc.
 */

/*
 * gcc -o aio-test aio-test.c -laio
 *
 * This workload was produced by mkntfs on a 100MB image. It's pretty
 * good at exposing a bug in ext4 which was fixed in Linux 2.6.38 by
 * cset e9e3bcecf44c04b9e6b505fd8e2eb9cea58fb94d.
 *
 * Example usage:
 *   for i in {1..100}; do ./aio-test -f /mnt/ext4fs/foo; done
 */

#ifndef _GNU_SOURCE
  #define _GNU_SOURCE
#endif
#define _XOPEN_SOURCE 500
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <libaio.h>
#include <getopt.h>
#include <inttypes.h>
#include <sys/syscall.h>

typedef enum {
    EXT_NOOP,
    EXT_WRITE,
    EXT_FTRUNCATE,
    EXT_FALLOCATE,
} extend_t;

struct iodesc {
    off_t off;
    size_t len;
};

static struct iodesc workload[] = {
    { .off = 0,       .len = 0x1600 },
    { .off = 0x1600,  .len = 0x1600 },
    { .off = 0x2c00,  .len = 0x400  },
    { .off = 0x4000,  .len = 0x1600 },
    { .off = 0x5600,  .len = 0x1600 },
    { .off = 0x6C00,  .len = 0x1600 },
    { .off = 0x8200,  .len = 0x1600 },
    { .off = 0x9800,  .len = 0x1600 },
    { .off = 0xae00,  .len = 0x200  },
};

#define IOCBS (sizeof(workload) / sizeof(workload[0]))

static void
usage(const char *app, int err)
{
    fprintf(stderr, "usage: %s <file> [-v verbose] [-h help]"
            "[(-f fallocate|-t truncate|-w write)]\n", app);
    exit(err);
}

static int
_fallocate(int fd, int mode, off_t offset, off_t length)
{
    return syscall(SYS_fallocate, fd, mode, offset, length);
}

int
main(int argc, char * const argv[])
{
    size_t len;
    extend_t extend;
    io_context_t aio;
    char *buf, *rbuf, *p;
    int i, c, fd, err, verbose;
    struct io_event events[IOCBS], *ep;
    struct iocb iocbs[IOCBS], *piocbs[IOCBS];

    fd = -1;
    aio = 0;
    buf = NULL;
    rbuf = NULL;
    verbose = 0;
    extend = EXT_NOOP;

    while ((c = getopt(argc, argv, "wtfvh")) != -1) {
        switch (c) {
        case 'w':
            extend = EXT_WRITE;
            break;
        case 't':
            extend = EXT_FTRUNCATE;
            break;
        case 'f':
            extend = EXT_FALLOCATE;
            break;
        case 'v':
            verbose = 1;
            break;
        case 'h':
            usage(argv[0], 0);
            break;
        }
    }

    if (argc - optind != 1)
        usage(argv[0], EINVAL);

    for (i = 0, len = 0; i < IOCBS; i++)
        len += workload[i].len;

    err = posix_memalign((void *)&buf, 4096, len);
    if (err) {
        fprintf(stderr, "memalign: %s\n", strerror(err));
        goto out;
    }

    err = io_setup(IOCBS, &aio);
    if (err) {
        perror("io_setup");
        err = errno;
        goto out;
    }

    fd = open(argv[optind], O_RDWR | O_CREAT | O_TRUNC | O_DIRECT, 0644);
    if (fd == -1) {
        perror("open");
        err = errno;
        goto out;
    }

    if (extend != EXT_NOOP) {
        switch (extend) {
        case EXT_WRITE:
            memset(buf, 0, len);
            err = pwrite(fd, buf, len, 0) != len;
            break;
        case EXT_FTRUNCATE:
            err = ftruncate(fd, len);
            break;
        case EXT_FALLOCATE:
            err = _fallocate(fd, 0, 0, len);
            break;
        default:
            break;
        }

        if (err) {
            const char *msg;
            switch (extend) {
            case EXT_WRITE:
                msg = "pwrite";
                break;
            case EXT_FTRUNCATE:
                msg = "ftruncate";
                break;
            case EXT_FALLOCATE:
                msg = "fallocate";
                break;
            default:
                msg = "gcc";
                break;
            }
            perror(msg);
            err = errno;
            goto out;
        }

        err = fdatasync(fd);
        if (err) {
            perror("fdatasync");
            goto out;
        }
    }

    for (i = 0, p = buf; i < IOCBS; i++) {
        struct iocb *io = iocbs + i;
        struct iodesc *pd = workload + i;
        if (verbose)
            printf("io 0x%x off 0x%llx len 0x%x\n",
                   i, (uint64_t)pd->off, pd->len);
        memset(p, i, pd->len);
        io_prep_pwrite(io, fd, p, pd->len, pd->off);
        piocbs[i] = io;
        p += pd->len;
    }

    err = io_submit(aio, IOCBS, piocbs);
    if (err < 0) {
        err = abs(err);
        fprintf(stderr, "io_submit: %s\n", strerror(err));
        goto out;
    }

    err = io_getevents(aio, IOCBS, IOCBS, events, NULL);
    if (err < 0) {
        err = abs(err);
        fprintf(stderr, "io_getevents: %s\n", strerror(err));
        goto out;
    }

    for (i = 0, ep = events; i < IOCBS; i++, ep++) {
        struct iocb *io = ep->obj;
        if (ep->res != io->u.c.nbytes) {
            err = abs(ep->res);
            fprintf(stderr, "io_pwrite: %s\n", strerror(err));
            goto out;
        }
    }

    err = fsync(fd);
    if (err) {
        err = errno;
        perror("fsync");
        goto out;
    }

    for (i = 0, p = buf; i < IOCBS; i++) {
        ssize_t ret;
        struct iodesc *pd = workload + i;

        err = posix_memalign((void *)&rbuf, 4096, pd->len);
        if (err) {
            fprintf(stderr, "memalign: %s\n", strerror(err));
            goto out;
        }

        ret = pread(fd, rbuf, pd->len, pd->off);
        if (ret < 0) {
            err = errno;
            perror("pread");
            goto out;
        } else if (ret != pd->len) {
            err = EIO;
            fprintf(stderr, "pread: %s\n", strerror(err));
            goto out;
        }

        if (memcmp(p, rbuf, pd->len)) {
            err = EIO;
            fprintf(stderr, "data mismatch: io %i\n", i);
            goto out;
        }

        p += pd->len;
        free(rbuf);
        rbuf = NULL;
    }

    err = 0;

 out:
    free(buf);
    free(rbuf);
    if (aio)
        io_destroy(aio);
    if (fd != -1)
        close(fd);
    return err;
}
