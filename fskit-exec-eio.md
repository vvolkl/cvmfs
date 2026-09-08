# exec() on a macFUSE FSKit mount fails with EIO until the file is read

## Summary

On a filesystem mounted with `-o backend=fskit`, executing a file fails with
**EIO (errno 5)**. Any ordinary read through the mount makes the next `exec()`
of that same, unmodified file succeed — even `tail -c 1`, which reaches the
filesystem as a single whole-file read from offset 0.

The FUSE server never sees a read request during the failing `exec()`. It
receives only `GETATTR`, which it answers correctly with the right size and
mode `0755`. No `OPEN`, no `READ`. `exec()` then fails with EIO without the
filesystem ever having been asked for the file's contents.

This reproduces with a **~220-line standalone FUSE filesystem whose file
contents are in-memory data** — no network, no backing I/O, no
filesystem-level cache, no third-party filesystem involved. It reproduces on
macOS 15.7.5 and on macOS 26.0, i.e. on both the `macfuse-local` and the
generic-URL `macfuse` FSKit modules.

## Environment

| | Host A | Host B |
|---|---|---|
| macOS | 15.7.5 (24G624) | 26.0 (25A354) |
| Arch | arm64 | arm64 |
| macFUSE | 5.3.3 | 5.3.3 |
| FSKit module in use | `io.macfuse.app.fsmodule.macfuse-local` | `io.macfuse.app.fsmodule.macfuse` |
| `mount` source | `/dev/disk4 … (macfuse, local, …, fskit)` | `macfuse://<UUID> … (macfuse, noowners, noatime, fskit)` |

Both hosts show identical behaviour, so this is neither macOS-version-specific
nor specific to one of the two FSKit modules.

## Minimal reproducer

`minfs.c` — a read-only filesystem serving two executable shell scripts whose
contents are in-memory data: one string constant and one buffer generated at
startup. The callbacks relevant to attributes, access checks, xattrs, opens,
reads, releases, directory listing, and statfs are logged to stderr with a
timestamp.

```c
#include <fuse.h>

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef ENOATTR
#ifdef ENODATA
#define ENOATTR ENODATA
#else
#define ENOATTR ENOENT
#endif
#endif

#define SMALL_PATH "/small.sh"
#define BIG_PATH   "/big.sh"

static const char kSmall[] =
  "#!/bin/sh\n"
  "echo \"minfs: small.sh executed ok\"\n";

/* Larger than one 16 KiB arm64 page, so size and page-alignment effects can be
   checked separately from the tiny script. Filled in at startup. */
static char kBig[40960];
static size_t kBigLen;

static void trace(const char *fmt, ...) {
  struct timeval tv;
  va_list ap;
  gettimeofday(&tv, NULL);
  fprintf(stderr, "[%ld.%06d] ", (long)tv.tv_sec, (int)tv.tv_usec);
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

static ino_t ino_for(const char *path) {
  if (strcmp(path, "/") == 0) return 1;
  if (strcmp(path, SMALL_PATH) == 0) return 2;
  if (strcmp(path, BIG_PATH) == 0) return 3;
  return 0;
}

static void fill_common_attrs(const char *path, struct stat *st) {
  st->st_ino = ino_for(path);
  st->st_uid = getuid();
  st->st_gid = getgid();
  st->st_atime = 1;
  st->st_mtime = 1;
  st->st_ctime = 1;
}

static int minfs_getattr(const char *path, struct stat *st) {
  size_t len;
  const char *body;

  memset(st, 0, sizeof(*st));
  if (strcmp(path, "/") == 0) {
    fill_common_attrs(path, st);
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
  fill_common_attrs(path, st);
  st->st_mode = S_IFREG | 0755;   /* executable */
  st->st_nlink = 1;
  st->st_size = (off_t)len;
  st->st_blocks = (blkcnt_t)((len + 511) / 512);
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

static int minfs_access(const char *path, int mask) {
  size_t len;
  if (strcmp(path, "/") != 0 && body_for(path, &len) == NULL) {
    trace("ACCESS %s mask 0x%x -> ENOENT", path, mask);
    return -ENOENT;
  }
  if (mask & W_OK) {
    trace("ACCESS %s mask 0x%x -> EACCES", path, mask);
    return -EACCES;
  }
  trace("ACCESS %s mask 0x%x -> ok", path, mask);
  return 0;
}

static int minfs_open(const char *path, struct fuse_file_info *fi) {
  size_t len;
  if (body_for(path, &len) == NULL) {
    trace("OPEN %s -> ENOENT", path);
    return -ENOENT;
  }
  /* Accept all open flag combinations used by the backend. */
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

#ifdef __APPLE__
static int minfs_getxattr(const char *path, const char *name, char *value,
                          size_t size, uint32_t position) {
  (void)value; (void)size; (void)position;
#else
static int minfs_getxattr(const char *path, const char *name, char *value,
                          size_t size) {
  (void)value; (void)size;
#endif
  size_t len;
  if (strcmp(path, "/") != 0 && body_for(path, &len) == NULL) {
    trace("GETXATTR %s %s -> ENOENT", path, name);
    return -ENOENT;
  }
  trace("GETXATTR %s %s -> ENOATTR", path, name);
  return -ENOATTR;
}

static int minfs_listxattr(const char *path, char *list, size_t size) {
  size_t len;
  (void)list; (void)size;
  if (strcmp(path, "/") != 0 && body_for(path, &len) == NULL) {
    trace("LISTXATTR %s -> ENOENT", path);
    return -ENOENT;
  }
  trace("LISTXATTR %s -> empty", path);
  return 0;
}

static int minfs_statfs(const char *path, struct statvfs *st) {
  memset(st, 0, sizeof(*st));
  st->f_bsize = 4096;
  st->f_frsize = 4096;
  st->f_blocks = 1024;
  st->f_bfree = 1024;
  st->f_bavail = 1024;
  st->f_files = 3;
  st->f_namemax = 255;
  trace("STATFS %s -> ok", path);
  return 0;
}

static struct fuse_operations minfs_ops = {
  .getattr   = minfs_getattr,
  .readdir   = minfs_readdir,
  .access    = minfs_access,
  .open      = minfs_open,
  .read      = minfs_read,
  .release   = minfs_release,
  .getxattr  = minfs_getxattr,
  .listxattr = minfs_listxattr,
  .statfs    = minfs_statfs,
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
```

### Build

```sh
cc -o minfs minfs.c -D_FILE_OFFSET_BITS=64 -DFUSE_USE_VERSION=26 \
   -I/usr/local/include -L/usr/local/lib -lfuse
```

### Mount

```sh
./minfs -f -o backend=fskit,uid=$(id -u),gid=$(id -g) /Volumes/minfs
```

### Trigger

From another shell, without reading the file first:

```sh
/Volumes/minfs/small.sh          # fails
cat /Volumes/minfs/small.sh      # succeeds
/Volumes/minfs/small.sh          # now succeeds
```

`exec()` was invoked directly via `os.execv()` rather than through a shell, so
the errno is unambiguous:

```
== /Volumes/minfs/small.sh
  COLD (no prior read)         execv FAIL errno=5 (EIO)
  read 45 bytes ok
  WARM (after read)            minfs: small.sh executed ok
== /Volumes/minfs/big.sh
  COLD (no prior read)         execv FAIL errno=5 (EIO)
  read 40924 bytes ok
  WARM (after read)            minfs: big.sh ok
```

Both a 45-byte script and a 40924-byte script (larger than one 16 KiB page)
behave the same way on the cold `exec()` failure. The important warm-up is a
normal read that starts at offset 0; the example above uses `cat`, but a test
that reads only from the beginning of the file is the sharper variant if the
question is whether caching the initial page, rather than the whole file, is
sufficient.

## Traces

### Failing exec — fresh mount, single cold `execv()`, complete trace

```
minfs: small.sh 45 bytes, big.sh 40924 bytes
[1784806098.952891] GETATTR / -> dir
[1784806098.953001] GETATTR / -> dir
[1784806098.955195] GETATTR / -> dir
[1784806098.958900] GETATTR / -> dir
[1784806098.959000] GETATTR / -> dir
[1784806098.959604] GETATTR /._. -> ENOENT
[1784806098.960555] GETATTR /DCIM -> ENOENT
[1784806098.960841] GETATTR /.metadata_never_index_unless_rootfs -> ENOENT
[1784806098.961020] GETATTR /.metadata_never_index -> ENOENT
[1784806098.961259] GETATTR /.metadata_direct_scope_only -> ENOENT
[1784806098.961452] GETATTR /.Spotlight-V100 -> ENOENT
[1784806098.963456] GETATTR /.DS_Store -> ENOENT
                        <- everything above is mount-time probing
[1784806110.782197] GETATTR / -> dir
[1784806110.782542] GETATTR /small.sh -> file, size 45, mode 0755
[1784806110.782734] GETATTR /small.sh -> file, size 45, mode 0755
[1784806110.782878] GETATTR /._small.sh -> ENOENT
                        <- execv() returns EIO here
```

That is the entire trace. **No `OPEN`, no `READ`.** The filesystem answered
`GETATTR` with `size 45, mode 0755` and was never asked for the contents.

### Succeeding exec — same mount, after one `read()`

```
[1784806126.057551] GETATTR /small.sh -> file, size 45, mode 0755
[1784806126.064885] OPEN    /small.sh flags 0x0 -> fh 42
[1784806126.065169] READ    /small.sh size 512 offset 0 fh 42
[1784806126.065183] READ    /small.sh -> 45 bytes
[1784806126.065304] GETATTR /small.sh -> file, size 45, mode 0755
[1784806126.065554] RELEASE /small.sh fh 42
                        <- the explicit read() above
[1784806126.066326] GETATTR /small.sh -> file, size 45, mode 0755
[1784806126.074708] OPEN    /small.sh flags 0x0 -> fh 42
[1784806126.074881] RELEASE /small.sh fh 42
                        <- execv() succeeds here, still without any READ
```

The successful `exec()` also issues no `READ`. It succeeds only because the
earlier `read()` populated the relevant cached data.

### Which read unblocks it — a single fresh mount, three steps

Run on one mount instance so the control is unambiguous. `big.sh` is 40924
bytes, i.e. larger than one 16 KiB arm64 page.

```
1. COLD exec (control)      -> EIO       ops reaching the filesystem: none
2. tail -c 1 big.sh         [..] OPEN /big.sh flags 0x0 -> fh 42
                            [..] READ /big.sh size 40960 offset 0 fh 42
3. exec after tail          -> OK        ops reaching the filesystem: OPEN only
```

Two things to note. The cold `exec()` on this very instance failed with EIO and
produced **no** filesystem operations at all, so the file was definitely in the
failing state immediately before step 2. And a request for the **last byte**
arrives at the filesystem as one **whole-file read from offset 0** — so at the
filesystem level there is no distinction between priming with the start of the
file and priming with its tail. Any read appears to do.

Untested: whether this still holds for a file too large to be read in one go.
A 40 KB file is read whole; a 100 MB file will not be, and the answer to "which
part of the file must be resident" may differ there.

## Analysis

On a cold cache, `exec()` does not cause a `read` callback to reach this FUSE
server. It works when the relevant bytes are already cached from an unrelated
earlier read.

XNU's `exec_activate_image()` reads the image header via `vn_rdwr()` after
`namei()`, and the trace shows no preceding `VNOP_OPEN`-equivalent callback to
this FUSE filesystem. For a UBC-backed vnode, that read can be satisfied via
the cache/pagein machinery rather than the normal user read path. The evidence
suggests that the cold-cache exec read path is not being forwarded to the FUSE
server by the FSKit backend, so it fails and `exec()` reports EIO.

Consistent with this, macFUSE 5.3.0's rewrite of the FSKit read path (the new
`MFChannel` API, `FUSE_DARWIN_REPLY_BUF`/`FUSE_DARWIN_PAYLOAD_BUF` zero-copy
reads) did not change the behaviour — the ordinary read path works fine, and
the failing path is a different one that appears never to be entered.

## Ruled out

- **Code signature validation.** The reproducer is an ordinary unsigned
  `#!/bin/sh` script. Unsigned scripts execute normally from other filesystems,
  and this failure happens before the filesystem is asked for file contents.
  `DevToolsSecurity -enable` makes no difference on either host.
- **`noexec`.** That would give EACCES (13), not EIO (5); the mount carries no
  such flag.
- **Anything filesystem-specific.** `minfs` has no filesystem-level cache, no
  network, no backing I/O of any kind — the file contents are in-memory data in
  the process.
- **File size / page alignment.** A 45-byte file and a 40924-byte file
  (> 1 page, not a page multiple) fail identically.
- **Attribute correctness.** `GETATTR` returns the correct size and `0755`
  immediately before the failure.
- **The local-vs-generic FSKit module split.** `FSGenericURLResource` is
  macOS 26.0+, so macOS 15 necessarily uses `macfuse-local` and macOS 26 uses
  the generic-URL `macfuse` module. Both fail identically.


