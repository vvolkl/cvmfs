/*
 * minfs -- a minimal read-only FUSE filesystem for reproducing an exec()
 * failure on the macFUSE FSKit backend.  No network, no cache:
 * the file contents are string constants in this file.
 *
 * Every FUSE operation is logged to stderr, so the trace shows exactly what
 * the FSKit backend asks for while exec() is failing.
 *
 * Build (macFUSE 5.x):
 *   cc -o minfs minfs.c -D_FILE_OFFSET_BITS=64 -DFUSE_USE_VERSION=26 \
 *      -I/usr/local/include -L/usr/local/lib -lfuse
 *
 * Run:
 *   mkdir -p /Volumes/minfs
 *   ./minfs -f -o backend=fskit,uid=$(id -u),gid=$(id -g) /Volumes/minfs
 *
 * Test (from another shell, without reading the file first):
 *   /Volumes/minfs/small.sh
 */

#include <fuse.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define SMALL_PATH "/small.sh"
#define BIG_PATH   "/big.sh"

static const char kSmall[] =
  "#!/bin/sh\n"
  "echo \"minfs: small.sh executed ok\"\n";

/* Larger than one 16 KiB arm64 page, so that "the whole file" and "the first
   page" are distinguishable. Filled in at startup. */
static char kBig[40960];
static size_t kBigLen;

static void trace(const char *fmt, ...) {
  struct timeval tv;
  va_list ap;
  gettimeofday(&tv, NULL);
  fprintf(stderr, "[%ld.%06d] ", (long)tv.tv_sec, tv.tv_usec);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  fflush(stderr);
}

static const char *body_for(const char *path, size_t *len) {
  if (strcmp(path, SMALL_PATH) == 0) { *len = sizeof(kSmall) - 1; return kSmall; }
  if (strcmp(path, BIG_PATH) == 0)   { *len = kBigLen;            return kBig;   }
  return NULL;
}

static int minfs_getattr(const char *path, struct stat *st) {
  size_t len;
  const char *body;

  memset(st, 0, sizeof(*st));
  if (strcmp(path, "/") == 0) {
    st->st_mode = S_IFDIR | 0755;
    st->st_nlink = 2;
    trace("GETATTR %s -> dir", path);
    return 0;
  }
  body = body_for(path, &len);
  if (body == NULL) {
    trace("GETATTR %s -> ENOENT", path);
    return -ENOENT;
  }
  st->st_mode = S_IFREG | 0755;   /* executable */
  st->st_nlink = 1;
  st->st_size = (off_t)len;
  trace("GETATTR %s -> file, size %zu, mode 0755", path, len);
  return 0;
}

static int minfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                         off_t offset, struct fuse_file_info *fi) {
  (void)offset; (void)fi;
  if (strcmp(path, "/") != 0) return -ENOENT;
  trace("READDIR %s", path);
  filler(buf, ".", NULL, 0);
  filler(buf, "..", NULL, 0);
  filler(buf, &SMALL_PATH[1], NULL, 0);
  filler(buf, &BIG_PATH[1], NULL, 0);
  return 0;
}

static int minfs_open(const char *path, struct fuse_file_info *fi) {
  size_t len;
  if (body_for(path, &len) == NULL) {
    trace("OPEN %s -> ENOENT", path);
    return -ENOENT;
  }
  /* Do not reject on flags: the FSKit backend always opens read/write. */
  fi->fh = 42;   /* a handle we can recognise in READ */
  trace("OPEN %s flags 0x%x -> fh 42", path, fi->flags);
  return 0;
}

static int minfs_read(const char *path, char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi) {
  size_t len;
  const char *body = body_for(path, &len);

  trace("READ %s size %zu offset %lld fh %llu", path, size, (long long)offset,
        fi ? (unsigned long long)fi->fh : 0ULL);

  if (body == NULL) return -ENOENT;
  if (offset >= (off_t)len) {
    trace("READ %s -> 0 (at/past EOF)", path);
    return 0;                       /* EOF, never an error */
  }
  if (offset + size > len) size = len - offset;
  memcpy(buf, body + offset, size);
  trace("READ %s -> %zu bytes", path, size);
  return (int)size;
}

static int minfs_release(const char *path, struct fuse_file_info *fi) {
  trace("RELEASE %s fh %llu", path, (unsigned long long)fi->fh);
  return 0;
}

static struct fuse_operations minfs_ops = {
  .getattr = minfs_getattr,
  .readdir = minfs_readdir,
  .open    = minfs_open,
  .read    = minfs_read,
  .release = minfs_release,
};

int main(int argc, char *argv[]) {
  int n;
  /* build a >1 page script that prints on the last line */
  n = snprintf(kBig, sizeof(kBig), "#!/bin/sh\n");
  while (n < (int)sizeof(kBig) - 64)
    n += snprintf(kBig + n, sizeof(kBig) - n, "# padding to exceed one page\n");
  n += snprintf(kBig + n, sizeof(kBig) - n, "echo \"minfs: big.sh ok\"\n");
  kBigLen = (size_t)n;
  fprintf(stderr, "minfs: small.sh %zu bytes, big.sh %zu bytes\n",
          sizeof(kSmall) - 1, kBigLen);
  return fuse_main(argc, argv, &minfs_ops, NULL);
}
