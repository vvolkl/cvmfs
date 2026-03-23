# CvmFS FSKit Volume — Design Specification

## 1. Overview and Goals

Replace `cvmfs/cvmfs.cc` (a FUSE low-level API module) with a native macOS
FSKit filesystem extension written in Swift, while keeping the entire C++ core
(MountPoint, FileSystem, CatalogManager, Fetcher, CacheManager, InodeTracker,
PageCacheTracker, etc.) unchanged.

### In scope
- All read-only VFS operations: lookup, getattr, readlink, opendir/readdir,
  open/read/close, statfs, getxattr/listxattr.
- Catalog TTL / remount (via the existing `FuseRemounter` C++ class, adapted).
- Chunked-file reads.
- Magic extended attributes.

### Explicitly out of scope (dropped)
- VOMS / authz session (`ClientCtxGuard`, `CheckVoms`, `AuthzSessionManager`).
- `FuseInterruptCue` / request cancellation.
- NFS mode (`IsNfsSource()` paths).
- FUSE passthrough (`FUSE_CAP_PASSTHROUGH`, zero-copy backing fd).
- Hot-reload / `SaveState` / `RestoreState` (FUSE loader mechanism).
- Watchdog (`Watchdog::Create`, stack-trace on crash).
- Symlink caching capability flags (no per-reply kernel TTL in FSKit).

---

## 2. High-Level Architecture

```
┌─────────────────────────────────────────────────────┐
│  FSKit Extension process (macOS, Swift)             │
│                                                     │
│  CvmfsFS  (FSVolume subclass, Swift)                │
│    ├─ CvmfsItem  (FSItem subclass, Swift)           │
│    │    └─ holds: inode, openFd, chunkHandle, …     │
│    └─ CvmfsBridge  (C++ wrapper, Swift-callable)    │
│         ├─ MountPoint*       (existing C++)         │
│         ├─ FileSystem*       (existing C++)         │
│         ├─ FuseRemounter*    (existing C++, renamed │
│         │                     RemountController)    │
│         └─ TalkManager*      (existing C++)         │
└─────────────────────────────────────────────────────┘
```

The Swift layer owns the FSKit protocol conformances and translates FSKit
callbacks into calls on a thin **CvmfsBridge** Objective-C++ class that
exposes the relevant C++ subsystems to Swift via safe value types.

---

## 3. File Layout

```
cvmfs/fskit/
  CvmfsFS.swift          – FSVolume subclass + all protocol conformances
  CvmfsItem.swift        – FSItem subclass carrying per-inode state
  CvmfsBridge.h          – Objective-C++ header (Swift-visible)
  CvmfsBridge.mm         – Objective-C++ implementation wrapping C++ core
  CvmfsExtension.swift   – FSFileSystemExtension + FSFileSystem entry point
  CvmfsErrors.swift      – POSIX errno → Swift Error helpers
  Bridging-Header.h      – Includes CvmfsBridge.h for the Swift module
```

All existing C++ files remain unchanged. The extension links them as a static
library (or directly via CMake target).

---

## 4. CvmfsItem — FSItem subclass

`CvmfsItem` is the persistent in-memory object for each inode that FSKit keeps
alive while any user-space or kernel reference exists. On `reclaimItem` it is
released (ARC takes over).

```swift
final class CvmfsItem: FSItem {

    // ── Identity ──────────────────────────────────────────────────────────
    /// The numeric inode from the C++ catalog (post-MangleInode).
    let inode: UInt64

    // ── Open-file state (set in openItem, cleared in closeItem) ──────────
    /// Raw cache manager file descriptor for a regular (non-chunked) open.
    /// -1 when no file is open.
    var openFd: Int32 = -1

    /// For chunked files: the handle key in ChunkTables::handle2fd.
    /// nil when not a chunked open.
    var chunkHandle: UInt64? = nil

    /// Whether the open was forced to direct-I/O (no kernel page cache).
    /// Mirrors the PageCacheTracker kBitDirectIo logic from cvmfs.cc.
    var directIo: Bool = false

    /// Whether the kernel page cache entry should be kept across re-opens.
    /// Drives the FSKit equivalent of fi->keep_cache.
    var keepCache: Bool = false

    // ── Populated once at lookup time ─────────────────────────────────────
    /// Cached FSItem.Attributes built from the catalog DirectoryEntry.
    var attributes = FSItem.Attributes()

    // ── Init ──────────────────────────────────────────────────────────────
    init(inode: UInt64, attributes: FSItem.Attributes) {
        self.inode = inode
        super.init()
        self.attributes = attributes
    }
}
```

`CvmfsItem.attributes` is populated once at lookup/activate time and updated
if `setAttributes` is ever called (though CVMFS is read-only, FSKit may still
call setAttributes for atime, etc.).

---

## 5. CvmfsBridge — Objective-C++ interop layer

`CvmfsBridge` is an `NSObject` subclass implemented in `.mm`. It owns the
C++ `FileSystem*` and `MountPoint*` and exposes a value-type Swift-friendly
API. No C++ types are exposed in the header; all results are returned as
Foundation or primitive types.

### 5.1 Value types exposed to Swift

```objc
// CvmfsBridge.h

/// Mirrors catalog::DirectoryEntry fields relevant to FSKit.
typedef struct CvmfsDirent {
    uint64_t  inode;
    uint32_t  mode;       // S_IF* | permission bits
    uint64_t  size;
    int64_t   mtime;
    uint32_t  uid;
    uint32_t  gid;
    uint32_t  nlink;
    BOOL      isLink;
    BOOL      isDirectory;
    BOOL      isChunkedFile;
    BOOL      isExternalFile;
    BOOL      isDirectIo;
    BOOL      hasXattrs;
    // symlink target (only valid when isLink)
    char      symlinkTarget[PATH_MAX];
    // hash for page-cache tracker key (opaque 32 bytes, SHA-1/SHA-256/MD5)
    uint8_t   contentHash[32];
    uint8_t   contentHashType;  // shash::Algorithm enum value
    uint8_t   zipAlgorithm;     // zlib::Algorithms enum value
} CvmfsDirent;

/// Result codes returned to Swift instead of errno integers.
typedef NS_ENUM(int32_t, CvmfsBridgeError) {
    CvmfsBridgeErrorNone      =  0,
    CvmfsBridgeErrorNotFound  = -ENOENT,
    CvmfsBridgeErrorIO        = -EIO,
    CvmfsBridgeErrorNotDir    = -ENOTDIR,
    CvmfsBridgeErrorNotLink   = -EINVAL,
    CvmfsBridgeErrorStale     = -ESTALE,
};
```

### 5.2 Lifecycle methods

```objc
@interface CvmfsBridge : NSObject

/// Initialize with a repository FQRN and a config-file path.
/// Internally calls FileSystem::Create + MountPoint::Create + Spawn().
/// Returns nil and sets *error on failure.
- (nullable instancetype)initWithFQRN:(NSString *)fqrn
                           configFile:(NSString *)configPath
                                error:(NSError **)error;

/// Tear everything down (calls Fini()).
- (void)shutdown;

@end
```

### 5.3 Catalog / remount

```objc
/// Enter the remount fence (equivalent to fuse_remounter_->fence()->Enter()).
/// Must be paired with leaveFence.  Call from every VFS operation that reads
/// catalog data.  NOT reentrant; do not hold across async suspension points.
- (void)enterFence;
- (void)leaveFence;

/// Called at the top of lookup/opendir/open to opportunistically apply a
/// pending catalog update (non-blocking; equivalent to TryFinish()).
- (void)tryFinishRemount;

/// The current kcache timeout in seconds (0.0 = no caching).
@property (readonly) double kcacheTimeoutSec;

/// Root inode number.
@property (readonly) uint64_t rootInode;

/// Mangle an inode (catalog offset correction on reload).
- (uint64_t)mangleInode:(uint64_t)ino;
```

### 5.4 Inode / path resolution

```objc
/// Looks up a DirectoryEntry by inode number.
/// Returns CvmfsBridgeErrorNone and fills *dirent on success.
/// Returns CvmfsBridgeErrorNotFound if the inode is not in any active catalog.
- (CvmfsBridgeError)getDirentForInode:(uint64_t)ino
                               result:(CvmfsDirent *)dirent;

/// Looks up a DirectoryEntry by path.
/// Returns 0 if not found (negative), 1 if found, >1 if found but inode
/// is stale (open file whose content changed — mirrors GetDirentForPath).
- (uint64_t)getDirentForPath:(NSString *)path
                      result:(CvmfsDirent *)dirent;

/// Resolves inode → full path string.  Returns nil if not found.
- (nullable NSString *)getPathForInode:(uint64_t)ino;

/// Registers a VfsGet (lookup reference) for inode with the InodeTracker.
/// path is the full absolute path for the inode.
- (void)vfsGetInode:(uint64_t)ino mode:(uint32_t)mode path:(NSString *)path;

/// Registers a VfsPut (forget reference) for inode.
/// Returns YES if the inode was removed from the tracker (trigger page-cache
/// eviction).
- (BOOL)vfsPutInode:(uint64_t)ino nlookup:(uint64_t)count;

/// Evicts a page-cache tracker entry for inode (after vfsPut removed it).
- (void)evictPageCacheEntry:(uint64_t)ino;
```

### 5.5 Page-cache tracker

```objc
typedef struct CvmfsOpenDirectives {
    BOOL keepCache;
    BOOL directIo;
} CvmfsOpenDirectives;

/// Inform the page-cache tracker that a regular file is being opened.
- (CvmfsOpenDirectives)pageTrackerOpenInode:(uint64_t)ino
                                       hash:(const uint8_t *)hash   // 32 bytes
                                   hashType:(uint8_t)hashType
                                       stat:(struct stat)st;

/// Direct-I/O open (no page-cache tracking).
- (CvmfsOpenDirectives)pageTrackerOpenDirect;

/// Close a tracked inode in the page-cache tracker.
- (void)pageTrackerClose:(uint64_t)ino;

/// Fill *hash and *st for an inode that is open but whose on-disk content
/// may have changed.  Returns YES if the inode is currently open.
- (BOOL)pageTrackerGetInfoIfOpen:(uint64_t)ino
                     contentHash:(uint8_t *)hash   // 32-byte out buffer
                       hashType:(uint8_t *)hashType
                            stat:(struct stat *)st;

/// Returns YES if the inode's content in the catalog differs from what the
/// page-cache tracker recorded at open time.
- (BOOL)pageTrackerIsStaleInode:(uint64_t)ino
                        direntHash:(const uint8_t *)hash
                        hashType:(uint8_t)hashType;
```

### 5.6 Fetching / reading

```objc
/// Download and cache a regular file; returns an open cache fd ≥ 0 or < 0
/// for -errno.  Equivalent to Fetcher::Fetch().
- (int)fetchInode:(uint64_t)ino
             path:(NSString *)path
             size:(uint64_t)size
     zipAlgorithm:(uint8_t)alg
       isExternal:(BOOL)external;

/// pread from a cache fd.  Returns bytes read or < 0 for -errno.
- (int64_t)pread:(int)fd buffer:(void *)buf size:(uint64_t)size offset:(uint64_t)off;

/// Close a cache fd.
- (int)closeFd:(int)fd;

/// Chunked-file support — mirrors the ChunkTables logic in cvmfs_open/read/
/// release.  Returns a chunk handle ≥ 0 or -1 on failure.
- (int64_t)openChunkedFile:(uint64_t)ino
                      path:(NSString *)path
                   dirent:(CvmfsDirent)dirent;

/// Read from a chunked file handle.  Returns bytes read or < 0 for -errno.
- (int64_t)readChunkedFile:(uint64_t)chunkHandle
                    offset:(off_t)offset
                    length:(size_t)length
                    buffer:(void *)buf;

/// Release a chunked file handle.
- (void)releaseChunkedFile:(uint64_t)chunkHandle inode:(uint64_t)ino;
```

### 5.7 Directory listing

```objc
typedef struct CvmfsStatEntry {
    char     name[NAME_MAX + 1];
    uint64_t inode;
    uint32_t mode;
    uint64_t size;
    int64_t  mtime;
    uint32_t uid;
    uint32_t gid;
    uint32_t nlink;
} CvmfsStatEntry;

/// List the contents of a directory at path.  Calls
/// catalog_mgr->ListingStat() and resolves each entry's live inode via
/// GetDirentForPath().
/// On success fills *entries (caller must free) and *count.
/// Returns CvmfsBridgeErrorNone or an error code.
- (CvmfsBridgeError)listDirectory:(NSString *)path
                           entries:(CvmfsStatEntry **)entries
                             count:(NSUInteger *)count;
```

### 5.8 Extended attributes

```objc
/// Equivalent to the cvmfs_getxattr logic: resolves magic xattrs first,
/// then falls back to catalog xattrs.
/// Returns NSData on success (length 0 = attribute exists but is empty).
/// Returns nil and sets *errCode to ENOATTR/ENODATA/EIO on failure.
- (nullable NSData *)getXattr:(NSString *)name
                    forInode:(uint64_t)ino
                      dirent:(CvmfsDirent)dirent
                    errCode:(int *)errCode;

/// List all xattr names for inode (magic + catalog xattrs merged).
/// Returns NSArray<NSString*>; empty array = no xattrs.
- (NSArray<NSString *> *)listXattrsForInode:(uint64_t)ino
                                     dirent:(CvmfsDirent)dirent;
```

### 5.9 StatFS

```objc
typedef struct CvmfsStatFS {
    uint64_t blockSize;
    uint64_t totalBlocks;
    uint64_t freeBlocks;
    uint64_t totalFiles;
    uint64_t freeFiles;
} CvmfsStatFS;

- (CvmfsStatFS)statfs;
```

---

## 6. CvmfsFS — FSVolume subclass

`CvmfsFS` is the central Swift class. It holds a `CvmfsBridge` instance and
conforms to the required FSKit protocols.

```swift
final class CvmfsFS: FSVolume,
    FSVolume.Operations,
    FSVolume.OpenCloseOperations,
    FSVolume.ReadWriteOperations,
    FSVolume.XattrOperations,
    FSVolume.PathConfOperations
{
    let bridge: CvmfsBridge
    private let rootItem: CvmfsItem   // created in activate()

    init(bridge: CvmfsBridge) { … }
}
```

### 6.1 `supportedVolumeCapabilities`

```swift
var supportedVolumeCapabilities: FSVolume.SupportedCapabilities {
    let c = FSVolume.SupportedCapabilities()
    c.supportsSymbolicLinks           = true
    c.supportsPersistentObjectIDs     = true
    c.supportsHardLinks               = true
    c.doesNotSupportVolumeSizes       = false
    c.caseFormat                      = .sensitive  // CVMFS is case-sensitive
    c.supports64BitObjectIDs          = true
    // read-only: no create/write/rename/delete capabilities advertised
    return c
}
```

### 6.2 `activate(options:)` → root FSItem

1. Call `bridge.tryFinishRemount()`.
2. Enter fence; call `bridge.getDirentForInode(bridge.rootInode)` to get root
   dirent; leave fence.
3. Construct a `CvmfsItem` for the root, populate `attributes` from the dirent.
4. Retain as `rootItem`.
5. Return `rootItem`.

### 6.3 `mount(options:)` / `unmount()` / `deactivate(options:)`

- `mount`: no-op (bridge already initialised by `CvmfsExtension`).
- `unmount`: no-op (bridge shutdown happens in `deactivate`).
- `deactivate`: call `bridge.shutdown()`.

---

## 7. FSKit Callback Implementations

### 7.1 `lookupItem(named:inDirectory:)`

```
INPUT:  name: FSFileName, directory: FSItem (CvmfsItem)
OUTPUT: (FSItem, FSFileName) or throws

1. bridge.tryFinishRemount()
2. bridge.enterFence()
3. let parentIno = bridge.mangleInode((directory as! CvmfsItem).inode)
4. let parentPath = bridge.getPathForInode(parentIno) — throw ENOENT if nil
5. let fullPath = parentPath + "/" + name.string
6. let liveInode = bridge.getDirentForPath(fullPath, &dirent)
   - liveInode == 0 → bridge.leaveFence(); throw ENOENT (or EIO if IO error)
7. If liveInode > 1 (stale open inode):
   - bridge.inodeTracker().replaceInode(live → dirent.inode)   [C++ side]
8. bridge.vfsGetInode(dirent.inode, mode: dirent.mode, path: fullPath)
9. bridge.leaveFence()
10. Build CvmfsItem(inode: dirent.inode, attributes: attributesFrom(dirent))
11. Return (item, name)
```

### 7.2 `reclaimItem(_ item: FSItem)`

```
1. let ino = (item as! CvmfsItem).inode
2. let removed = bridge.vfsPutInode(ino, nlookup: 1)
3. if removed { bridge.evictPageCacheEntry(ino) }
```

*Note:* FSKit calls `reclaimItem` once per previously-returned item, so the
`nlookup` count is always 1 (no multi-forget batching needed).

### 7.3 `attributes(_:of:)`

```
INPUT:  desiredAttributes, item: FSItem (CvmfsItem)

1. let ino = bridge.mangleInode(item.inode)
2. bridge.enterFence()
3. bridge.getDirentForInode(ino, &dirent)
   On not-found:
     a. Try page-cache tracker: bridge.pageTrackerGetInfoIfOpen(ino, …)
        If found → return synthetic attributes from stat
     b. Otherwise throw ENOENT / EIO
   On found:
     a. If MayBeInPageCacheTracker (regular file, ino < rootInode):
        - bridge.pageTrackerGetInfoIfOpen(ino, …) → if open, return that stat
4. bridge.leaveFence()
5. Return attributesFrom(dirent) or attributes from the page-cache stat
```

### 7.4 `readSymbolicLink(_ item: FSItem)`

```
1. let ino = bridge.mangleInode(item.inode)
2. bridge.enterFence()
3. bridge.getDirentForInode(ino, &dirent)
4. bridge.leaveFence()
5. if !dirent.isLink → throw EINVAL
6. return FSFileName(string: String(cString: dirent.symlinkTarget))
```

### 7.5 `enumerateDirectory(_:startingAt:verifier:attributes:packer:)`

This replaces `cvmfs_opendir` + `cvmfs_readdir` + `cvmfs_releasedir` in a
single streaming call. There is no pre-built buffer or handle table.

```
INPUT:  directory: FSItem (CvmfsItem), cookie, verifier, attributes, packer

1. let ino = bridge.mangleInode((directory as! CvmfsItem).inode)
2. bridge.tryFinishRemount()
3. bridge.enterFence()
4. let path = bridge.getPathForInode(ino) — throw ENOENT if nil
5. bridge.getDirentForInode(ino, &dirDirent) — throw ENOENT/EIO if not found
6. if !dirDirent.isDirectory → throw ENOTDIR
7. let (entries, count) = bridge.listDirectory(path)
8. bridge.leaveFence()

9. Enumerate entries (skipping indices < cookie.rawValue for resume):
   for (idx, entry) in entries.enumerated() where idx >= Int(cookie.rawValue) {
       let itemAttrs = attributesFrom(entry)   // if packer wants attrs
       let ok = packer.packEntry(
           name:       FSFileName(string: entry.name),
           itemType:   itemTypeFrom(entry.mode),
           itemID:     FSItem.Identifier(rawValue: entry.inode),
           nextCookie: FSDirectoryCookie(UInt64(idx + 1)),
           attributes: attributes != nil ? itemAttrs : nil
       )
       if !ok { break }   // packer buffer full; resume next time from idx+1
   }

10. return FSDirectoryVerifier(0)
```

*Rationale:* Instead of pre-building the entire `fuse_listing` buffer in one
shot and slicing it in `cvmfs_readdir`, we let FSKit drive the enumeration. On
buffer-full the packer returns false; FSKit calls us again with an updated
cookie pointing at the next undelivered entry.

### 7.6 `openItem(_ item: FSItem, modes: FSVolume.OpenModes)`

```
1. let ino = bridge.mangleInode(item.inode)
2. bridge.tryFinishRemount()
3. bridge.enterFence()
4. let path = bridge.getPathForInode(ino) — throw ENOENT if nil
5. bridge.getDirentForInode(ino, &dirent) — throw ENOENT/EIO if not found
6. bridge.leaveFence()
7. let cvItem = item as! CvmfsItem

8. if dirent.isChunkedFile:
   a. let handle = bridge.openChunkedFile(ino, path, dirent)
      handle < 0 → throw based on errno
   b. let od = dirent.isDirectIo
                 ? bridge.pageTrackerOpenDirect()
                 : bridge.pageTrackerOpenInode(ino, hash, stat)
   c. cvItem.chunkHandle = UInt64(handle)
   d. cvItem.directIo    = od.directIo
   e. cvItem.keepCache   = od.keepCache
else:
   a. let od = dirent.isDirectIo
                 ? bridge.pageTrackerOpenDirect()
                 : bridge.pageTrackerOpenInode(ino, hash, stat)
   b. let fd = bridge.fetchInode(ino, path, dirent.size,
                                 dirent.zipAlgorithm, dirent.isExternalFile)
      fd < 0 → bridge.pageTrackerClose(ino); throw based on -fd
   c. cvItem.openFd    = Int32(fd)
   d. cvItem.directIo  = od.directIo
   e. cvItem.keepCache = od.keepCache
```

### 7.7 `read(from:at:offset:length:into:)`

```
INPUT:  item: FSItem, offset: off_t, length: Int, buffer: FSMutableFileDataBuffer
OUTPUT: Int (bytes read)

1. let cvItem = item as! CvmfsItem

2. if cvItem.chunkHandle != nil:
   // Chunked read — mirrors the fd < 0 branch in cvmfs_read
   a. buffer.withUnsafeMutableBytes { ptr in
          bridge.readChunkedFile(cvItem.chunkHandle!, offset, length, ptr)
      }
   b. throw on error, return bytesRead

else:
   // Regular read
   a. buffer.withUnsafeMutableBytes { ptr in
          bridge.pread(cvItem.openFd, ptr, length, offset)
      }
   b. throw on error, return bytesRead
```

### 7.8 `closeItem(_ item: FSItem, modes: FSVolume.OpenModes)`

```
1. let cvItem = item as! CvmfsItem
2. let ino = bridge.mangleInode(cvItem.inode)

3. if !cvItem.directIo:
      bridge.pageTrackerClose(ino)

4. if cvItem.chunkHandle != nil:
      bridge.releaseChunkedFile(cvItem.chunkHandle!, inode: ino)
      cvItem.chunkHandle = nil
else if cvItem.openFd >= 0:
      bridge.closeFd(cvItem.openFd)
      cvItem.openFd = -1
```

### 7.9 `volumeStatistics`

```swift
var volumeStatistics: FSStatFSResult {
    let s = bridge.statfs()
    let r = FSStatFSResult(fileSystemTypeName: "cvmfs")
    r.blockSize       = s.blockSize
    r.ioSize          = s.blockSize
    r.totalBlocks     = s.totalBlocks
    r.freeBlocks      = s.freeBlocks
    r.availableBlocks = s.freeBlocks
    r.totalFiles      = s.totalFiles
    r.freeFiles       = s.freeFiles
    return r
}
```

### 7.10 `xattr(named:of:)`

```
1. let ino = bridge.mangleInode((item as! CvmfsItem).inode)
2. bridge.enterFence()
3. bridge.getDirentForInode(ino, &dirent)
4. bridge.leaveFence()
5. var errCode: Int32 = 0
6. let data = bridge.getXattr(name.string!, forInode: ino,
                               dirent: dirent, errCode: &errCode)
7. if data == nil → throw fs_errorForPOSIXError(errCode)
8. return data!
```

### 7.11 `xattrs(of:)`

```
1. let ino = bridge.mangleInode((item as! CvmfsItem).inode)
2. bridge.enterFence()
3. bridge.getDirentForInode(ino, &dirent)
4. bridge.leaveFence()
5. return bridge.listXattrsForInode(ino, dirent).map { FSFileName(string: $0) }
```

---

## 8. CvmfsExtension — Entry Point

```swift
// CvmfsExtension.swift
@main
final class CvmfsExtension: FSFileSystemExtension {

    override init() {
        super.init()
    }

    override func makeFileSystem() -> FSFileSystem {
        return CvmfsFileSystem()
    }
}

final class CvmfsFileSystem: FSFileSystem {

    override func volume(for resource: FSResource,
                         options: FSTaskOptions) async throws -> FSVolume {
        // resource.url contains mount parameters (FQRN, config path)
        // passed via mount(8) or the FSKit driver descriptor.
        guard let params = parseResource(resource) else {
            throw fs_errorForPOSIXError(EINVAL)
        }
        var err: NSError? = nil
        guard let bridge = CvmfsBridge(fqrn: params.fqrn,
                                       configFile: params.configFile,
                                       error: &err) else {
            throw err!
        }
        return CvmfsFS(
            bridge: bridge,
            volumeID: FSVolume.Identifier(uuid: params.uuid),
            volumeName: FSFileName(string: params.fqrn)
        )
    }
}
```

---

## 9. Concurrency Model

FSKit calls all `FSVolume` methods as Swift `async` functions. The C++ core
is **not** thread-safe for concurrent catalog reads without the fence, so:

- Every FSKit callback that accesses catalog data calls `enterFence` /
  `leaveFence` around the C++ read (fence is a reentrant reader-writer lock
  owned by `FuseRemounter`).
- `CvmfsBridge` methods that take the fence must be called from the same async
  task; do **not** suspend (await) between `enterFence` and `leaveFence`.
- `bridge.tryFinishRemount()` may briefly block if a catalog reload is in
  progress; this is acceptable since it's a very fast check.
- All `CvmfsBridge` methods are annotated `@_silgen_name` or bridged via
  `@objc` and are safe to call from the Swift actor-based async context.
- The chunked-read per-handle lock (`chunk_tables->Handle2Lock`) is acquired
  inside `CvmfsBridge.readChunkedFile` on the C++ side.

---

## 10. Helper: `attributesFrom(CvmfsDirent)` → `FSItem.Attributes`

```swift
func attributesFrom(_ d: CvmfsDirent) -> FSItem.Attributes {
    let a = FSItem.Attributes()
    a.fileID    = FSItem.Identifier(rawValue: d.inode)!
    a.mode      = d.mode
    a.uid       = d.uid
    a.gid       = d.gid
    a.size      = d.size
    a.allocSize = d.size   // CVMFS doesn't track allocated vs logical size
    a.linkCount = d.nlink
    a.type      = d.isDirectory ? .directory
                : d.isLink      ? .symbolicLink
                :                 .file
    var ts = timespec(tv_sec: __darwin_time_t(d.mtime), tv_nsec: 0)
    a.modifyTime = ts
    a.changeTime = ts
    a.accessTime = ts
    a.birthTime  = ts
    return a
}
```

---

## 11. Error Mapping

```swift
// CvmfsErrors.swift
func cvmfsError(_ posixErrno: Int32) -> Error {
    return fs_errorForPOSIXError(posixErrno)
}

extension CvmfsBridgeError {
    var swiftError: Error { cvmfsError(-self.rawValue) }
}
```

Mapping table:

| Condition | errno | Thrown as |
|-----------|-------|-----------|
| Inode not found in catalog | `ENOENT` | `fs_errorForPOSIXError(ENOENT)` |
| Catalog load / I/O failure | `EIO` | `fs_errorForPOSIXError(EIO)` |
| Inode is not a directory | `ENOTDIR` | `fs_errorForPOSIXError(ENOTDIR)` |
| Inode is not a symlink | `EINVAL` | `fs_errorForPOSIXError(EINVAL)` |
| Stale inode (race) | `ESTALE` | `fs_errorForPOSIXError(ESTALE)` |
| Too many open files | `EMFILE` | `fs_errorForPOSIXError(EMFILE)` |
| Xattr not found | `ENOATTR` | `fs_errorForPOSIXError(ENOATTR)` |

---

## 12. What Is NOT Ported (and Why)

| Feature | Reason |
|---------|--------|
| `fuse_ctx` uid/gid/pid | FSKit gives no caller identity; kernel enforces permissions via attributes |
| `ClientCtxGuard` / VOMS | Depends on `fuse_ctx->pid`; no equivalent path |
| `FuseInterruptCue` | FSKit uses Swift structured concurrency; cancellation not required for v1 |
| `SaveState` / `RestoreState` | FUSE hot-reload mechanism; FSKit has no concept of it |
| Watchdog / crash handler | macOS sandbox + FSKit extension crash policy handles this |
| NFS maps | Not supported on macOS CVMFS client |
| `FUSE_CAP_PASSTHROUGH` | Linux-only zero-copy feature |
| Per-entry kernel cache TTL | FSKit has no per-reply timeout; volume capabilities are static |
| `cvmfs_forget_multi` | FSKit `reclaimItem` is always single-item |
| `fuse_expire_entry` / `InvalidateDentry` | No FSKit API to proactively invalidate a single dentry |
| `dentry_tracker` / `SpawnCleaner` | Depends on fuse_expire_entry |

---

## 13. Open Questions / Future Work

1. **Catalog invalidation**: Without `fuse_expire_entry`, stale entries may
   linger in the kernel's dentry cache across catalog reloads. FSKit may
   provide a volume-level cache flush; needs investigation.

2. **`setAttributes`**: FSKit may call `setAttributes` for timestamps. CVMFS
   is read-only; the implementation should silently accept (updating
   `CvmfsItem.attributes` in memory) without writing to the catalog.

3. **`TalkManager` socket**: The cvmfs talk socket (admin interface) can still
   run; it lives entirely in C++ and doesn't interact with FSKit.

4. **Configuration delivery**: How mount parameters (FQRN, config file path)
   are conveyed from the FSKit driver descriptor / `mount(8)` to
   `CvmfsExtension.volume(for:options:)` needs to be pinned down against the
   final FSKit entitlement and driver descriptor format.

5. **Symlink follow / O_SYMLINK**: On the FUSE macOS port, `O_SYMLINK` caused
   a special `kFileHandleIgnore` path in `cvmfs_open`. In FSKit, symlinks are
   handled at the VFS layer; `openItem` should never be called for a symlink.

6. **`RemountController`**: The existing `FuseRemounter` relies on
   `FuseInvalidator` for dentry invalidation notifications via
   `fuse_lowlevel_notify_inval_entry`. This subsystem needs to be stubbed out
   or replaced; the remount fence and `TryFinish` logic is still needed.
