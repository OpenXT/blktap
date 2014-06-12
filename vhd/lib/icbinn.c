/*
 * Copyright (c) 2012 Citrix Systems, Inc.
 */

#include <icbinn.h>

static ICBINN *icb_vhd;

static ICBINN *icb_key;

ICBINN *
vhd_icbinn_key (void)
{
  char proto[1024], *ptr, *host;
  int port;

  if (icb_key)
    return icb_key;

  ptr = getenv ("LIBVHD_ICBINN_KEY_SERVER");
  if (!ptr)
    return NULL;

  strncpy (proto, ptr, sizeof (proto));
  proto[sizeof (proto) - 1] = 0;

  host = index (proto, ':');
  if (!host)
    return NULL;
  *(host++) = 0;

  ptr = index (host, ':');
  if (!ptr)
    {
      port = ICBINN_PORT;
    }
  else
    {
      *ptr = 0;
      port = atoi (ptr + 1);
    }

  if (!strcmp (proto, "v4v"))
    {
      icb_key = icbinn_clnt_create_v4v (atoi (host), port);
    }
  else if (!strcmp (proto, "tcp"))
    {
      icb_key = icbinn_clnt_create_tcp (host, port);
    }

  return icb_key;
}

ICBINN *
vhd_icbinn_vhd (void)
{
  char proto[1024], *ptr, *host;
  int port;

  if (icb_vhd)
    return icb_vhd;

  ptr = getenv ("LIBVHD_ICBINN_VHD_SERVER");
  if (!ptr)
    return NULL;

  strncpy (proto, ptr, sizeof (proto));
  proto[sizeof (proto) - 1] = 0;

  host = index (proto, ':');
  if (!host)
    return NULL;
  *(host++) = 0;

  ptr = index (host, ':');
  if (!ptr)
    {
      port = ICBINN_PORT;
    }
  else
    {
      *ptr = 0;
      port = atoi (ptr + 1);
    }

  if (!strcmp (proto, "v4v"))
    {
      icb_vhd = icbinn_clnt_create_v4v (atoi (host), port);
    }
  else if (!strcmp (proto, "tcp"))
    {
      icb_vhd = icbinn_clnt_create_tcp (host, port);
    }

  return icb_vhd;
}

static off64_t
vhd_icbinn_devops_position (vhd_context_t * vhd)
{
  return (off64_t) vhd->offset;
}

static int
vhd_icbinn_devops_seek (vhd_context_t * vhd, off64_t off, int whence)
{
  struct icbinn_stat buf;

  switch (whence)
    {
      //XXX: both of these ought to check for seeks beyond EOF, but they don't
    case SEEK_SET:
      vhd->offset = off;
      break;
    case SEEK_CUR:
      vhd->offset += off;
      break;
    case SEEK_END:

      if (icbinn_stat (icb_vhd, vhd->file, &buf))
        return -1;

      if (buf.type != ICBINN_TYPE_FILE)
        return -1;

      vhd->offset = buf.size + off;
      break;
    default:
      return -1;
    }

  return 0;
}

static int
vhd_icbinn_devops_read (vhd_context_t * vhd, void *buf, size_t count)
{
  size_t ret;

  ret = icbinn_pread (icb_vhd, vhd->fd, buf, count, vhd->offset);

  if (ret > 0)
    vhd->offset += ret;

  return (ret == count) ? 0 : -EIO;
}

static int
vhd_icbinn_devops_pread (vhd_context_t * vhd,
                         void *buf, size_t size, off64_t off)
{
  size_t ret = icbinn_pread (icb_vhd, vhd->fd, buf, size, off);

  return (ret == size) ? 0 : -EIO;
}

static int
vhd_icbinn_devops_write (vhd_context_t * vhd, void *buf, size_t count)
{
  size_t ret;

  ret = icbinn_pwrite (icb_vhd, vhd->fd, buf, count, vhd->offset);

  if (ret > 0)
    vhd->offset += ret;

  return (ret == count) ? 0 : -EIO;
}

static int
vhd_icbinn_devops_pwrite (vhd_context_t * vhd,
                          void *buf, size_t size, off64_t off)
{
  size_t ret = icbinn_pwrite (icb_vhd, vhd->fd, buf, size, off);

  return (ret == size) ? 0 : -EIO;
}

static void
vhd_icbinn_devops_close (vhd_context_t * vhd)
{
  char *file = vhd->file;
  int fd = vhd->fd;

  vhd->fd = -1;
  __vhd_close (vhd);

  if (file)
    icbinn_close (icb_vhd, fd);
}

static vhd_devops_t vhd_icbinn_devops = {
  .position = vhd_icbinn_devops_position,
  .seek = vhd_icbinn_devops_seek,
  .read = vhd_icbinn_devops_read,
  .write = vhd_icbinn_devops_write,
  .pread = vhd_icbinn_devops_pread,
  .pwrite = vhd_icbinn_devops_pwrite,
  .close = vhd_icbinn_devops_close,
};



/* Shamelessly stolen of glibc 2.9 with modifications */

/* Return the canonical absolute name of file NAME.  A canonical name
   does not contain any `.', `..' components nor any repeated path
   separators ('/') or symlinks.  All path components must exist.  If
   RESOLVED is null, the result is malloc'd; otherwise, if the
   canonical name is PATH_MAX chars or more, returns null with `errno'
   set to ENAMETOOLONG; if the name fits in fewer than PATH_MAX chars,
   returns the name in RESOLVED.  If the name cannot be resolved and
   RESOLVED is non-NULL, it contains the path of the first component
   that cannot be resolved.  If the path can be resolved, RESOLVED
   holds the same value as the value returned.  */

char *
vhd_realpath (const char *name, char *resolved)
{
  char *rpath, *dest;
  const char *start, *end, *rpath_limit;
  long int path_max;

  if (!vhd_icbinn_vhd ())
    return realpath (name, resolved);

  if ((name == NULL) || (name[0] == '\0'))
    return realpath (name, resolved);

#ifdef PATH_MAX
  path_max = PATH_MAX;
#else
  path_max = pathconf (name, _PC_PATH_MAX);
  if (path_max <= 0)
    path_max = 1024;
#endif

  if (resolved == NULL)
    {
      rpath = malloc (path_max);
      if (rpath == NULL)
        return NULL;
    }
  else
    rpath = resolved;
  rpath_limit = rpath + path_max;

  rpath[0] = '/';
  dest = rpath + 1;

  for (start = end = name; *start; start = end)
    {
      struct icbinn_stat st;

      /* Skip sequence of multiple path-separators.  */
      while (*start == '/')
        ++start;

      /* Find end of path component.  */
      for (end = start; *end && *end != '/'; ++end)
        /* Nothing.  */ ;

      if (end - start == 0)
        break;
      else if (end - start == 1 && start[0] == '.')
        /* nothing */ ;
      else if (end - start == 2 && start[0] == '.' && start[1] == '.')
        {
          /* Back up to previous component, ignore if at root already.  */
          if (dest > rpath + 1)
            while ((--dest)[-1] != '/');
        }
      else
        {
          size_t new_size;

          if (dest[-1] != '/')
            *dest++ = '/';

          if (dest + (end - start) >= rpath_limit)
            {
              ptrdiff_t dest_offset = dest - rpath;
              char *new_rpath;

              if (resolved)
                {
                  errno = ENAMETOOLONG;
                  if (dest > rpath + 1)
                    dest--;
                  *dest = '\0';
                  goto error;
                }
              new_size = rpath_limit - rpath;
              if (end - start + 1 > path_max)
                new_size += end - start + 1;
              else
                new_size += path_max;
              new_rpath = (char *) realloc (rpath, new_size);
              if (new_rpath == NULL)
                goto error;
              rpath = new_rpath;
              rpath_limit = rpath + new_size;

              dest = rpath + dest_offset;
            }

          dest = mempcpy (dest, start, end - start);
          *dest = '\0';

          if (icbinn_stat (icb_vhd, rpath, &st) < 0)
            {
              errno = ENOENT;
              goto error;
            }

          if ((st.type != ICBINN_TYPE_DIRECTORY) && (*end != '\0'))
            {
              errno = ENOTDIR;
              goto error;
            }
        }
    }
  if (dest > rpath + 1 && dest[-1] == '/')
    --dest;
  *dest = '\0';

  return rpath;

error:
  if (resolved == NULL)
    free (rpath);
  return NULL;
}
