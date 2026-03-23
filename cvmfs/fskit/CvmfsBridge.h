/**
 * CvmfsBridge.h
 *
 * Objective-C++ header — the only C++ boundary visible to Swift.
 * All C++ types are hidden; only Foundation / primitive types cross the line.
 *
 * This file is part of the CernVM File System — FSKit port.
 */

#pragma once

#import <Foundation/Foundation.h>
#include <sys/stat.h>
#include <stdint.h>

NS_ASSUME_NONNULL_BEGIN

// ---------------------------------------------------------------------------
// Value types that cross the Swift/C++ boundary
// ---------------------------------------------------------------------------

/// Packed representation of a catalog::DirectoryEntry.
/// Allocated on the stack / as a value type; no heap ownership.
typedef struct CvmfsDirent {
    uint64_t  inode;
    uint32_t  mode;           ///< S_IF* | permission bits
    uint64_t  size;
    int64_t   mtime;
    uint32_t  uid;
    uint32_t  gid;
    uint32_t  nlink;
    uint8_t   isLink;
    uint8_t   isDirectory;
    uint8_t   isChunkedFile;
    uint8_t   isExternalFile;
    uint8_t   isDirectIo;
    uint8_t   hasXattrs;
    uint8_t   isNegative;     ///< true  → inode not found (ENOENT)
                              ///< false → found or I/O error (EIO)
    char      symlinkTarget[4096];
    /// Raw hash bytes (up to 20 = kMaxDigestSize).
    uint8_t   contentHash[20];
    /// shash::Algorithms enum value: kMd5=0, kSha1=1, kRmd160=2, kShake128=3
    uint8_t   contentHashAlgorithm;
    /// zlib::Algorithms enum value
    uint8_t   zipAlgorithm;
} CvmfsDirent;

/// Per-entry data returned from listDirectory.
typedef struct CvmfsStatEntry {
    char     name[256];       ///< null-terminated filename
    uint64_t inode;
    uint32_t mode;
    uint64_t size;
    int64_t  mtime;
    uint32_t uid;
    uint32_t gid;
    uint32_t nlink;
} CvmfsStatEntry;

/// Mirrors PageCacheTracker::OpenDirectives.
/// Uses C99 bool so Swift imports these as native Bool (not ObjCBool).
typedef struct CvmfsOpenDirectives {
    bool keepCache;
    bool directIo;
} CvmfsOpenDirectives;

/// Mirrors statvfs fields relevant to FSKit.
typedef struct CvmfsStatFS {
    uint64_t blockSize;
    uint64_t totalBlocks;
    uint64_t freeBlocks;
    uint64_t totalFiles;
    uint64_t freeFiles;
} CvmfsStatFS;

// ---------------------------------------------------------------------------
// CvmfsBridge
// ---------------------------------------------------------------------------

@interface CvmfsBridge : NSObject

/// Initialize with a fully-qualified repository name and a config-file path.
/// Internally creates FileSystem, MountPoint, FuseRemounter, TalkManager and
/// calls Spawn() on all background threads.
///
/// Returns nil and sets *outError on failure.
- (nullable instancetype)initWithFQRN:(NSString *)fqrn
                           configFile:(NSString *)configFile
                                error:(NSError *_Nullable *_Nullable)outError;

/// Tear down all C++ objects (reverse of init).  Safe to call multiple times.
- (void)shutdown;

// ── Remount / fence ────────────────────────────────────────────────────────

/// Opportunistically apply a pending catalog update.
/// Equivalent to fuse_remounter_->TryFinish().
/// Non-blocking fast path; may do more work if a reload is pending.
- (void)tryFinishRemount;

/// Enter the remount read-fence.  Must be paired with leaveFence.
/// Callers must NOT suspend (await) between enter and leave.
- (void)enterFence;

/// Leave the remount read-fence.
- (void)leaveFence;

/// Root inode number for this volume (post-annotation).
@property (readonly) uint64_t rootInode;

/// Mangle an inode: corrects catalog-generation offset on reload.
- (uint64_t)mangleInode:(uint64_t)ino;

// ── Inode / path resolution (call while fence is held) ────────────────────

/// Look up a DirectoryEntry by inode number.
/// Fills *dirent on success.  Returns YES if found.
/// If not found: dirent->isNegative == YES → ENOENT; NO → EIO.
- (BOOL)getDirentForInode:(uint64_t)ino
                   result:(CvmfsDirent *)dirent;

/// Look up a DirectoryEntry by full absolute path.
/// Returns 0 = not found, 1 = found, 2 = found but live inode is stale.
/// If return == 0: dirent->isNegative == YES → ENOENT; NO → EIO.
- (uint64_t)getDirentForPath:(NSString *)path
                      result:(CvmfsDirent *)dirent;

/// Resolve inode → full absolute path string.
/// Returns nil if not found.  Call while fence is held.
- (nullable NSString *)getPathForInode:(uint64_t)ino;

// ── InodeTracker VFS refcount (call while fence is held) ──────────────────

/// Register a VfsGet reference for inode at path with given mode bits.
- (void)vfsGetInode:(uint64_t)ino mode:(uint32_t)mode path:(NSString *)path;

/// Register a VfsPut (decrement) for inode by nlookup.
/// Returns YES if the inode was removed from the tracker (trigger page-cache
/// eviction with evictPageCacheEntry:).
- (BOOL)vfsPutInode:(uint64_t)ino nlookup:(uint64_t)count;

// ── PageCacheTracker ───────────────────────────────────────────────────────

/// Inform the tracker a regular file is being opened.  Call outside fence.
- (CvmfsOpenDirectives)pageTrackerOpenInode:(uint64_t)ino
                                       hash:(const uint8_t *)hash
                                  algorithm:(uint8_t)algorithm
                                       stat:(struct stat)st;

/// Direct-I/O open (bypasses page-cache tracking).
- (CvmfsOpenDirectives)pageTrackerOpenDirect;

/// Close a tracked inode.  Call outside fence.
- (void)pageTrackerCloseInode:(uint64_t)ino;

/// Fill hash/algorithm/stat for an open inode.  Returns YES if open.
- (BOOL)pageTrackerGetInfoIfOpen:(uint64_t)ino
                   contentHash:(uint8_t *)outHash   ///< 20-byte output buffer
                      algorithm:(uint8_t *)outAlgorithm
                            stat:(struct stat *)outStat;

/// Returns YES if the dirent content differs from what the tracker recorded.
- (BOOL)pageTrackerIsStaleInode:(uint64_t)ino
                         dirent:(CvmfsDirent)dirent;

/// Evict an inode from the page-cache tracker (call after vfsPut returns YES).
- (void)evictPageCacheInode:(uint64_t)ino;

// ── Directory listing (call while fence is held) ───────────────────────────

/// List the stat entries for a directory at path.
/// Caller is responsible for free()-ing *outEntries.
/// Returns YES on success.
- (BOOL)listDirectory:(NSString *)path
              entries:(CvmfsStatEntry *_Nullable *_Nonnull)outEntries
                count:(NSUInteger *)outCount;

// ── File open / fetch ──────────────────────────────────────────────────────

/// Fetch and open a regular (non-chunked) file.
/// Takes the full dirent so the hash can be reconstructed.
/// Returns an open cache-manager fd >= 0, or -(errno) on failure.
- (int)fetchFileForInode:(uint64_t)ino
                    path:(NSString *)path
                  dirent:(CvmfsDirent)dirent;

/// pread from a cache fd.  Returns bytes read, or -(errno) on failure.
- (int64_t)pread:(int)fd
          buffer:(void *)buf
            size:(uint64_t)size
          offset:(uint64_t)offset;

/// Close a cache fd.  Returns 0 on success, -1 on error.
- (int)closeFd:(int)fd;

// ── Chunked-file lifecycle (call outside fence) ────────────────────────────

/// Open a chunked file; registers it in ChunkTables.
/// Returns a chunk-handle >= 0, or -1 on failure (errno is set).
- (int64_t)openChunkedFileForInode:(uint64_t)ino
                              path:(NSString *)path
                            dirent:(CvmfsDirent)dirent
                    openDirectives:(CvmfsOpenDirectives *)outDirectives;

/// Read from a chunked file handle.
/// Returns bytes read, or -(errno) on failure.
- (int64_t)readChunkedFile:(uint64_t)handle
                    offset:(off_t)offset
                    length:(size_t)length
                    buffer:(void *)buf;

/// Release a chunked file handle and decrement chunk-table refcounts.
- (void)releaseChunkedFile:(uint64_t)handle inode:(uint64_t)ino;

// ── Extended attributes ────────────────────────────────────────────────────

/// Look up a single xattr value.
/// Returns NSData on success (length == 0 is valid), nil on error.
/// *outErrno is set to ENOATTR, ENODATA, EIO, ESTALE on failure.
- (nullable NSData *)getXattrNamed:(NSString *)name
                          forInode:(uint64_t)ino
                            dirent:(CvmfsDirent)dirent
                           errCode:(int *)outErrno;

/// List all xattr names for an inode (magic + catalog merged).
- (NSArray<NSString *> *)listXattrsForInode:(uint64_t)ino
                                     dirent:(CvmfsDirent)dirent;

// ── StatFS ─────────────────────────────────────────────────────────────────

- (CvmfsStatFS)statfs;

@end

NS_ASSUME_NONNULL_END
