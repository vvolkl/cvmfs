/**
 * CvmfsBridge.mm
 *
 * Objective-C++ implementation.  This is the only translation unit that
 * includes C++ CernVM-FS headers; all other Swift/ObjC files only see the
 * plain-C-compatible CvmfsBridge.h header.
 *
 * This file is part of the CernVM File System — FSKit port.
 */

// Tell duplex_fuse.h to use stub FUSE types instead of real FUSE headers.
// This mirrors what libcvmfs and the unit-test builds do.
#ifndef CVMFS_LIBCVMFS
#define CVMFS_LIBCVMFS
#endif

// Pull in CernVM-FS C++ headers before anything else to avoid conflicts.
// We use paths relative to the repo root (the directory containing cvmfs/).
#include "cvmfs/cache.h"
#include "cvmfs/catalog_mgr_client.h"
#include "cvmfs/compression/compression.h"
#include "cvmfs/crypto/hash.h"
#include "cvmfs/directory_entry.h"
#include "cvmfs/fence.h"
#include "cvmfs/fetch.h"
#include "cvmfs/file_chunk.h"
#include "cvmfs/fuse_inode_gen.h"
#include "cvmfs/fuse_remount.h"
#include "cvmfs/glue_buffer.h"
#include "cvmfs/lru_md.h"
#include "cvmfs/magic_xattr.h"
#include "cvmfs/mountpoint.h"
#include "cvmfs/options.h"
#include "cvmfs/quota.h"
#include "cvmfs/quota_posix.h"
#include "cvmfs/shortstring.h"
#include "cvmfs/statistics.h"
#include "cvmfs/talk.h"
#include "cvmfs/tracer.h"
#include "cvmfs/util/logging.h"
#include "cvmfs/util/mutex.h"
#include "cvmfs/util/pointer.h"
#include "cvmfs/util/posix.h"
#include "cvmfs/util/string.h"
#include "cvmfs/xattr.h"

#import "CvmfsBridge.h"

#include <sys/statvfs.h>
#include <pthread.h>
#include <cerrno>
#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// Internal C++ helper functions (mirroring cvmfs.cc, NFS paths removed)
// ---------------------------------------------------------------------------

namespace {

// Convert a catalog::DirectoryEntry to the opaque CvmfsDirent value type.
static CvmfsDirent DirentFromEntry(const catalog::DirectoryEntry &d) {
    CvmfsDirent out;
    memset(&out, 0, sizeof(out));
    out.inode            = static_cast<uint64_t>(d.inode());
    out.mode             = d.mode();
    out.size             = d.size();
    out.mtime            = static_cast<int64_t>(d.mtime());
    out.uid              = d.uid();
    out.gid              = d.gid();
    // linkcount is stored via GetStatStructure
    struct stat st = d.GetStatStructure();
    out.nlink            = static_cast<uint32_t>(st.st_nlink);
    out.isLink           = d.IsLink()         ? 1 : 0;
    out.isDirectory      = d.IsDirectory()    ? 1 : 0;
    out.isChunkedFile    = d.IsChunkedFile()  ? 1 : 0;
    out.isExternalFile   = d.IsExternalFile() ? 1 : 0;
    out.isDirectIo       = d.IsDirectIo()     ? 1 : 0;
    out.hasXattrs        = d.HasXattrs()      ? 1 : 0;
    out.isNegative       = 0;
    if (d.IsLink()) {
        const LinkString &sym = d.symlink();
        size_t len = std::min(static_cast<size_t>(sym.GetLength()),
                              sizeof(out.symlinkTarget) - 1);
        memcpy(out.symlinkTarget, sym.GetChars(), len);
        out.symlinkTarget[len] = '\0';
    }
    const shash::Any &hash = d.checksum();
    const unsigned digest_size = shash::kDigestSizes[hash.algorithm];
    memcpy(out.contentHash, hash.digest, digest_size);
    out.contentHashAlgorithm = static_cast<uint8_t>(hash.algorithm);
    out.zipAlgorithm         = static_cast<uint8_t>(d.compression_algorithm());
    return out;
}

// Reconstruct a shash::Any from the packed CvmfsDirent fields.
static shash::Any HashFromDirent(const CvmfsDirent &d) {
    shash::Any h(static_cast<shash::Algorithms>(d.contentHashAlgorithm));
    unsigned sz = shash::kDigestSizes[h.algorithm];
    memcpy(h.digest, d.contentHash, sz);
    return h;
}

// ── GetDirentForInode ────────────────────────────────────────────────────
// Call while remount fence is held.
// Returns: true  = found (dirent is valid)
//          false = not found; dirent.GetSpecial()==kDirentNegative → ENOENT
//                             else → EIO
static bool GetDirentForInode(MountPoint *mp,
                               const uint64_t ino,
                               catalog::DirectoryEntry *dirent) {
    // 1. Try the LRU inode cache.
    if (mp->inode_cache()->Lookup(ino, dirent))
        return true;

    *dirent = catalog::DirectoryEntry();

    catalog::ClientCatalogManager *cmgr = mp->catalog_mgr();

    // 2. Root inode special case.
    if (ino == static_cast<uint64_t>(cmgr->GetRootInode())) {
        if (!cmgr->LookupPath(PathString(), catalog::kLookupDefault, dirent))
            return false;
        dirent->set_inode(ino);
        mp->inode_cache()->Insert(ino, *dirent);
        return true;
    }

    // 3. Resolve inode → path via InodeTracker.
    PathString path;
    glue::InodeEx inode_ex(ino, glue::InodeEx::kUnknownType);
    if (!mp->inode_tracker()->FindPath(&inode_ex, &path)) {
        LogCvmfs(kLogCvmfs, kLogDebug,
                 "[FSKit] GetDirentForInode %" PRIu64 " not in tracker", ino);
        const catalog::DirectoryEntry neg =
            catalog::DirectoryEntry(catalog::kDirentNegative);
        *dirent = neg;
        dirent->set_inode(ino);
        return false;
    }

    // 4. Catalog lookup.
    if (cmgr->LookupPath(path, catalog::kLookupDefault, dirent)) {
        dirent->set_inode(ino);
        mp->inode_cache()->Insert(ino, *dirent);
        return true;
    }

    LogCvmfs(kLogCvmfs, kLogDebug,
             "[FSKit] GetDirentForInode path lookup failed for inode %" PRIu64,
             ino);
    return false;
}

// ── GetDirentForPath ─────────────────────────────────────────────────────
// Returns 0 = not found, 1 = found, 2 = found+stale-live-inode.
// Call while remount fence is held.
static uint64_t GetDirentForPath(MountPoint *mp,
                                  const PathString &path,
                                  catalog::DirectoryEntry *dirent) {
    catalog::ClientCatalogManager *cmgr = mp->catalog_mgr();

    // 1. Find the live inode (if any) in the inode tracker.
    const uint64_t live_inode = mp->inode_tracker()->FindInode(path);

    // 2. Try the md5-path cache.
    const shash::Md5 md5path(path.GetChars(), path.GetLength());
    if (mp->md5path_cache()->Lookup(md5path, dirent)) {
        if (dirent->GetSpecial() == catalog::kDirentNegative)
            return 0;
        if (live_inode != 0)
            dirent->set_inode(live_inode);
        return 1;
    }

    // 3. Catalog lookup.
    if (!cmgr->LookupPath(path, catalog::kLookupDefault, dirent)) {
        if (dirent->GetSpecial() == catalog::kDirentNegative)
            mp->md5path_cache()->InsertNegative(md5path);
        return 0;
    }

    if (live_inode != 0)
        dirent->set_inode(live_inode);

    mp->md5path_cache()->Insert(md5path, *dirent);
    return 1;
}

// ── GetPathForInode ──────────────────────────────────────────────────────
// Returns true and fills *path on success.
// Call while remount fence is held.
static bool GetPathForInode(MountPoint *mp,
                             const uint64_t ino,
                             PathString *path) {
    // 1. Try path cache.
    if (mp->path_cache()->Lookup(ino, path))
        return true;

    // 2. Root inode.
    if (ino == static_cast<uint64_t>(mp->catalog_mgr()->GetRootInode()))
        return true;   // empty PathString == "/"

    // 3. InodeTracker.
    glue::InodeEx inode_ex(ino, glue::InodeEx::kUnknownType);
    if (!mp->inode_tracker()->FindPath(&inode_ex, path)) {
        LogCvmfs(kLogCvmfs, kLogDebug,
                 "[FSKit] GetPathForInode: inode %" PRIu64 " not in tracker",
                 ino);
        return false;
    }

    mp->path_cache()->Insert(ino, *path);
    return true;
}

}  // anonymous namespace


// ---------------------------------------------------------------------------
// CvmfsBridge — ObjC implementation
// ---------------------------------------------------------------------------

@implementation CvmfsBridge {
    FileSystem              *_fileSystem;
    MountPoint              *_mountPoint;
    cvmfs::InodeGenerationInfo _inodeGenInfo;
    FuseRemounter           *_remounter;
    TalkManager             *_talkMgr;
    OptionsManager          *_optionsMgr;
}

- (nullable instancetype)initWithFQRN:(NSString *)fqrn
                           configFile:(NSString *)configFile
                                error:(NSError *_Nullable *_Nullable)outError
{
    self = [super init];
    if (!self) return nil;

    const std::string fqrn_str  = fqrn.UTF8String;
    const std::string cfg_str   = configFile.UTF8String;

    // ── Options manager ────────────────────────────────────────────────────
    _optionsMgr = new SimpleOptionsParser(
        new DefaultOptionsTemplateManager(fqrn_str));
    if (!cfg_str.empty())
        static_cast<SimpleOptionsParser *>(_optionsMgr)->TryParsePath(cfg_str);
    else
        _optionsMgr->ParseDefault(fqrn_str);

    FileSystem::SetupLoggingStandalone(*_optionsMgr, fqrn_str);

    // ── FileSystem ─────────────────────────────────────────────────────────
    FileSystem::FileSystemInfo fs_info;
    fs_info.type        = FileSystem::kFsLibrary;   // no FUSE dependency
    fs_info.name        = fqrn_str;
    fs_info.options_mgr = _optionsMgr;
    fs_info.foreground  = true;

    _fileSystem = FileSystem::Create(fs_info);
    if (!_fileSystem || !_fileSystem->IsValid()) {
        std::string err = _fileSystem ? _fileSystem->boot_error() : "FileSystem::Create returned null";
        if (outError) {
            *outError = [NSError errorWithDomain:@"CvmfsBridgeError"
                                           code:EIO
                                       userInfo:@{NSLocalizedDescriptionKey: @(err.c_str())}];
        }
        [self _cleanup];
        return nil;
    }

    // ── MountPoint ─────────────────────────────────────────────────────────
    _mountPoint = MountPoint::Create(fqrn_str, _fileSystem);
    if (!_mountPoint || !_mountPoint->IsValid()) {
        std::string err = _mountPoint ? _mountPoint->boot_error() : "MountPoint::Create returned null";
        if (outError) {
            *outError = [NSError errorWithDomain:@"CvmfsBridgeError"
                                           code:EIO
                                       userInfo:@{NSLocalizedDescriptionKey: @(err.c_str())}];
        }
        [self _cleanup];
        return nil;
    }

    // ── InodeGenerationInfo ────────────────────────────────────────────────
    _inodeGenInfo.initial_revision =
        _mountPoint->catalog_mgr()->GetRevision();
    _inodeGenInfo.inode_generation =
        _mountPoint->inode_annotation()->GetGeneration();

    // ── FuseRemounter ──────────────────────────────────────────────────────
    // Pass nullptr for FUSE channel — no kernel-side dentry invalidation.
    // The remount fence and TryFinish() still work correctly.
    _remounter = new FuseRemounter(_mountPoint,
                                   &_inodeGenInfo,
                                   nullptr,   // no FUSE channel
                                   false);    // no notify invalidation

    // ── TalkManager ────────────────────────────────────────────────────────
    _talkMgr = TalkManager::Create(_mountPoint->talk_socket_path(),
                                   _mountPoint,
                                   _remounter);
    // TalkManager may be null if the socket path is empty — that's fine.

    // ── Spawn background threads ───────────────────────────────────────────
    _remounter->Spawn();
    _mountPoint->download_mgr()->Spawn();
    _mountPoint->external_download_mgr()->Spawn();
    if (_mountPoint->resolv_conf_watcher())
        _mountPoint->resolv_conf_watcher()->Spawn();
    _fileSystem->cache_mgr()->quota_mgr()->Spawn();
    _mountPoint->tracer()->Spawn();
    if (_talkMgr)
        _talkMgr->Spawn();
    _fileSystem->cache_mgr()->Spawn();

    return self;
}

- (void)_cleanup {
    delete _talkMgr;     _talkMgr    = nullptr;
    delete _remounter;   _remounter  = nullptr;
    delete _mountPoint;  _mountPoint = nullptr;
    delete _fileSystem;  _fileSystem = nullptr;
    delete _optionsMgr;  _optionsMgr = nullptr;
}

- (void)shutdown {
    [self _cleanup];
}

- (void)dealloc {
    [self _cleanup];
}

// ── Remount / fence ────────────────────────────────────────────────────────

- (void)tryFinishRemount {
    _remounter->TryFinish();
}

- (void)enterFence {
    _remounter->fence()->Enter();
}

- (void)leaveFence {
    _remounter->fence()->Leave();
}

- (uint64_t)rootInode {
    return static_cast<uint64_t>(_mountPoint->catalog_mgr()->GetRootInode());
}

- (uint64_t)mangleInode:(uint64_t)ino {
    return _mountPoint->catalog_mgr()->MangleInode(ino);
}

// ── Inode / path resolution ────────────────────────────────────────────────

- (BOOL)getDirentForInode:(uint64_t)ino
                   result:(CvmfsDirent *)dirent {
    catalog::DirectoryEntry d;
    const bool found = GetDirentForInode(_mountPoint, ino, &d);
    if (found) {
        *dirent = DirentFromEntry(d);
        dirent->isNegative = 0;
    } else {
        memset(dirent, 0, sizeof(CvmfsDirent));
        dirent->inode      = ino;
        dirent->isNegative = (d.GetSpecial() == catalog::kDirentNegative) ? 1 : 0;
    }
    return found ? YES : NO;
}

- (uint64_t)getDirentForPath:(NSString *)pathStr
                      result:(CvmfsDirent *)dirent {
    const PathString path(pathStr.UTF8String,
                          static_cast<int>(strlen(pathStr.UTF8String)));
    catalog::DirectoryEntry d;
    const uint64_t rc = GetDirentForPath(_mountPoint, path, &d);
    if (rc > 0) {
        *dirent = DirentFromEntry(d);
        dirent->isNegative = 0;
    } else {
        memset(dirent, 0, sizeof(CvmfsDirent));
        dirent->isNegative = (d.GetSpecial() == catalog::kDirentNegative) ? 1 : 0;
    }
    return rc;
}

- (nullable NSString *)getPathForInode:(uint64_t)ino {
    PathString path;
    if (!GetPathForInode(_mountPoint, ino, &path))
        return nil;
    // Root inode returns an empty PathString which means "/"
    if (path.GetLength() == 0)
        return @"";
    return [NSString stringWithUTF8String:path.c_str()];
}

// ── InodeTracker VFS refcount ──────────────────────────────────────────────

- (void)vfsGetInode:(uint64_t)ino mode:(uint32_t)mode path:(NSString *)pathStr {
    const PathString path(pathStr.UTF8String,
                          static_cast<int>(strlen(pathStr.UTF8String)));
    _mountPoint->inode_tracker()->VfsGet(
        glue::InodeEx(ino, static_cast<uint32_t>(mode)), path);
}

- (BOOL)vfsPutInode:(uint64_t)ino nlookup:(uint64_t)count {
    auto raii = _mountPoint->inode_tracker()->GetVfsPutRaii();
    return raii.VfsPut(ino, static_cast<uint32_t>(count)) ? YES : NO;
}

// ── PageCacheTracker ───────────────────────────────────────────────────────

- (CvmfsOpenDirectives)pageTrackerOpenInode:(uint64_t)ino
                                       hash:(const uint8_t *)hash
                                  algorithm:(uint8_t)algorithm
                                       stat:(struct stat)st {
    shash::Any h(static_cast<shash::Algorithms>(algorithm));
    memcpy(h.digest, hash, shash::kDigestSizes[h.algorithm]);
    const glue::PageCacheTracker::OpenDirectives od =
        _mountPoint->page_cache_tracker()->Open(ino, h, st);
    return CvmfsOpenDirectives{ od.keep_cache ? YES : NO,
                                od.direct_io  ? YES : NO };
}

- (CvmfsOpenDirectives)pageTrackerOpenDirect {
    const glue::PageCacheTracker::OpenDirectives od =
        _mountPoint->page_cache_tracker()->OpenDirect();
    return CvmfsOpenDirectives{ od.keep_cache ? YES : NO,
                                od.direct_io  ? YES : NO };
}

- (void)pageTrackerCloseInode:(uint64_t)ino {
    _mountPoint->page_cache_tracker()->Close(ino);
}

- (BOOL)pageTrackerGetInfoIfOpen:(uint64_t)ino
                     contentHash:(uint8_t *)outHash
                       algorithm:(uint8_t *)outAlgorithm
                            stat:(struct stat *)outStat {
    shash::Any hash;
    struct stat st;
    const bool open =
        _mountPoint->page_cache_tracker()->GetInfoIfOpen(ino, &hash, &st);
    if (open) {
        memcpy(outHash, hash.digest, shash::kDigestSizes[hash.algorithm]);
        *outAlgorithm = static_cast<uint8_t>(hash.algorithm);
        *outStat = st;
    }
    return open ? YES : NO;
}

- (BOOL)pageTrackerIsStaleInode:(uint64_t)ino
                         dirent:(CvmfsDirent)d {
    // Staleness detection is performed by the caller via pageTrackerGetInfoIfOpen
    // followed by a hash comparison.  This method is provided as a convenience
    // but currently always returns NO; use the lower-level API instead.
    (void)ino; (void)d;
    return NO;
}

- (void)evictPageCacheInode:(uint64_t)ino {
    auto raii = _mountPoint->page_cache_tracker()->GetEvictRaii();
    raii.Evict(ino);
}

// ── Directory listing ──────────────────────────────────────────────────────

- (BOOL)listDirectory:(NSString *)pathStr
              entries:(CvmfsStatEntry **)outEntries
                count:(NSUInteger *)outCount {
    const PathString path(pathStr.UTF8String,
                          static_cast<int>(strlen(pathStr.UTF8String)));

    catalog::StatEntryList raw;
    if (!_mountPoint->catalog_mgr()->ListingStat(path, &raw))
        return NO;

    const NSUInteger n = raw.size();
    CvmfsStatEntry *buf =
        static_cast<CvmfsStatEntry *>(malloc(sizeof(CvmfsStatEntry) * (n + 1)));
    if (!buf) return NO;

    for (NSUInteger i = 0; i < n; ++i) {
        const catalog::StatEntry &se = *raw.AtPtr(static_cast<uint32_t>(i));
        CvmfsStatEntry &out = buf[i];
        memset(&out, 0, sizeof(out));

        // Name
        size_t name_len = std::min(static_cast<size_t>(se.name.GetLength()),
                                   sizeof(out.name) - 1);
        memcpy(out.name, se.name.GetChars(), name_len);
        out.name[name_len] = '\0';

        // Fix live inode via GetDirentForPath
        PathString entry_path = path;
        entry_path.Append("/", 1);
        entry_path.Append(se.name.GetChars(), se.name.GetLength());
        catalog::DirectoryEntry entry_dirent;
        if (GetDirentForPath(_mountPoint, entry_path, &entry_dirent) > 0) {
            out.inode = entry_dirent.inode();
        } else {
            out.inode = static_cast<uint64_t>(se.info.st_ino);
        }
        out.mode  = static_cast<uint32_t>(se.info.st_mode);
        out.size  = static_cast<uint64_t>(se.info.st_size);
        out.mtime = static_cast<int64_t>(se.info.st_mtime);
        out.uid   = static_cast<uint32_t>(se.info.st_uid);
        out.gid   = static_cast<uint32_t>(se.info.st_gid);
        out.nlink = static_cast<uint32_t>(se.info.st_nlink);
    }

    *outEntries = buf;
    *outCount   = n;
    return YES;
}

// ── File fetch / pread / close ─────────────────────────────────────────────

- (int)fetchFileForInode:(uint64_t)ino
                    path:(NSString *)pathStr
                  dirent:(CvmfsDirent)d {
    (void)ino;

    const shash::Any hash = HashFromDirent(d);

    CacheManager::Label label;
    label.path          = pathStr.UTF8String;
    label.size          = d.size;
    label.zip_algorithm = static_cast<zlib::Algorithms>(d.zipAlgorithm);
    if (_mountPoint->catalog_mgr()->volatile_flag())
        label.flags |= CacheManager::kLabelVolatile;
    if (d.isExternalFile)
        label.flags |= CacheManager::kLabelExternal;

    cvmfs::Fetcher *fetcher = d.isExternalFile
        ? _mountPoint->external_fetcher()
        : _mountPoint->fetcher();
    return fetcher->Fetch(CacheManager::LabeledObject(hash, label));
}

- (int64_t)pread:(int)fd
          buffer:(void *)buf
            size:(uint64_t)size
          offset:(uint64_t)offset {
    return _fileSystem->cache_mgr()->Pread(fd, buf, size, offset);
}

- (int)closeFd:(int)fd {
    return _fileSystem->cache_mgr()->Close(fd);
}

// ── Chunked-file lifecycle ─────────────────────────────────────────────────

- (int64_t)openChunkedFileForInode:(uint64_t)ino
                              path:(NSString *)pathStr
                            dirent:(CvmfsDirent)d
                    openDirectives:(CvmfsOpenDirectives *)outOD {
    const std::string path_s(pathStr.UTF8String);
    const PathString  path(path_s.c_str(), static_cast<int>(path_s.size()));

    catalog::ClientCatalogManager *cmgr = _mountPoint->catalog_mgr();
    ChunkTables *ct = _mountPoint->chunk_tables();

    // Resolve unique inode from annotated catalog (mirrors cvmfs_open).
    catalog::DirectoryEntry dirent_origin;
    if (!cmgr->LookupPath(path, catalog::kLookupDefault, &dirent_origin)) {
        LogCvmfs(kLogCvmfs, kLogDebug,
                 "[FSKit] openChunkedFile: path %s vanished", path_s.c_str());
        errno = ENOENT;
        return -1;
    }
    const uint64_t unique_inode = dirent_origin.inode();

    // Retrieve or populate the chunk list.
    ct->Lock();
    if (!ct->inode2chunks.Contains(unique_inode)) {
        ct->Unlock();

        UniquePtr<FileChunkList> chunks(new FileChunkList());
        if (!cmgr->ListFileChunks(path,
                                   dirent_origin.hash_algorithm(),
                                   chunks.weak_ref())
            || chunks->IsEmpty()) {
            LogCvmfs(kLogCvmfs, kLogDebug | kLogSyslogErr,
                     "[FSKit] openChunkedFile: no chunks for %s",
                     path_s.c_str());
            errno = EIO;
            return -1;
        }

        ct->Lock();
        if (!ct->inode2chunks.Contains(unique_inode)) {
            ct->inode2chunks.Insert(
                unique_inode,
                FileChunkReflist(chunks.Release(), path,
                                 dirent_origin.compression_algorithm(),
                                 dirent_origin.IsExternalFile()));
            ct->inode2references.Insert(unique_inode, 1);
        } else {
            uint32_t ref;
            ct->inode2references.Lookup(unique_inode, &ref);
            ct->inode2references.Insert(unique_inode, ref + 1);
        }
    } else {
        uint32_t ref;
        ct->inode2references.Lookup(unique_inode, &ref);
        ct->inode2references.Insert(unique_inode, ref + 1);
    }

    // Allocate handle.
    const uint64_t handle = ct->next_handle;
    ct->handle2fd.Insert(handle, ChunkFd());
    ct->handle2uniqino.Insert(handle, unique_inode);

    // Compute combined hash for page-cache tracker.
    FileChunkReflist chunk_reflist;
    ct->inode2chunks.Lookup(unique_inode, &chunk_reflist);

    glue::PageCacheTracker::OpenDirectives od;
    if (d.isDirectIo) {
        od = _mountPoint->page_cache_tracker()->OpenDirect();
    } else {
        const shash::Any combined = chunk_reflist.HashChunkList();
        struct stat st;
        memset(&st, 0, sizeof(st));
        st.st_ino   = static_cast<ino_t>(ino);
        st.st_mode  = static_cast<mode_t>(d.mode);
        st.st_size  = static_cast<off_t>(d.size);
        st.st_mtime = static_cast<time_t>(d.mtime);
        st.st_uid   = static_cast<uid_t>(d.uid);
        st.st_gid   = static_cast<gid_t>(d.gid);
        st.st_nlink = static_cast<nlink_t>(d.nlink);
        od = _mountPoint->page_cache_tracker()->Open(ino, combined, st);
    }

    ++ct->next_handle;
    ct->Unlock();

    if (outOD) {
        outOD->keepCache = od.keep_cache ? YES : NO;
        outOD->directIo  = od.direct_io  ? YES : NO;
    }
    return static_cast<int64_t>(handle);
}

- (int64_t)readChunkedFile:(uint64_t)handle
                    offset:(off_t)offset
                    length:(size_t)length
                    buffer:(void *)buf {
    ChunkTables *ct = _mountPoint->chunk_tables();

    // Resolve unique inode and chunk list.
    ct->Lock();
    uint64_t unique_inode;
    if (!ct->handle2uniqino.Lookup(handle, &unique_inode)) {
        ct->Unlock();
        return -EIO;
    }
    FileChunkReflist chunks;
    if (!ct->inode2chunks.Lookup(unique_inode, &chunks)) {
        ct->Unlock();
        return -EIO;
    }
    ct->Unlock();

    // Per-handle lock.
    pthread_mutex_t *handle_lock = ct->Handle2Lock(handle);
    const MutexLockGuard mlg(handle_lock);

    ct->Lock();
    ChunkFd chunk_fd;
    if (!ct->handle2fd.Lookup(handle, &chunk_fd)) {
        ct->Unlock();
        return -EIO;
    }
    ct->Unlock();

    unsigned chunk_idx = chunks.FindChunkIdx(offset);
    off_t offset_in_chunk = offset - chunks.list->AtPtr(chunk_idx)->offset();
    size_t overall = 0;
    char *dst = static_cast<char *>(buf);

    do {
        // Open chunk fd if needed.
        if (chunk_fd.fd == -1 || chunk_fd.chunk_idx != chunk_idx) {
            if (chunk_fd.fd != -1)
                _fileSystem->cache_mgr()->Close(chunk_fd.fd);

            const FileChunk *fc = chunks.list->AtPtr(chunk_idx);
            CacheManager::Label label;
            label.path = chunks.path.ToString();
            label.size = fc->size();
            label.zip_algorithm = chunks.compression_alg;
            label.flags |= CacheManager::kLabelChunked;
            if (_mountPoint->catalog_mgr()->volatile_flag())
                label.flags |= CacheManager::kLabelVolatile;
            if (chunks.external_data) {
                label.flags |= CacheManager::kLabelExternal;
                label.range_offset = fc->offset();
            }

            cvmfs::Fetcher *fetcher = chunks.external_data
                ? _mountPoint->external_fetcher()
                : _mountPoint->fetcher();
            chunk_fd.fd = fetcher->Fetch(
                CacheManager::LabeledObject(fc->content_hash(), label));

            if (chunk_fd.fd < 0) {
                chunk_fd.fd = -1;
                ct->Lock();
                ct->handle2fd.Insert(handle, chunk_fd);
                ct->Unlock();
                return -EIO;
            }
            chunk_fd.chunk_idx = chunk_idx;
        }

        const FileChunk *fc = chunks.list->AtPtr(chunk_idx);
        const size_t want = std::min(length - overall,
                                     fc->size() - static_cast<size_t>(offset_in_chunk));
        const int64_t got = _fileSystem->cache_mgr()->Pread(
            chunk_fd.fd, dst + overall, want, offset_in_chunk);

        if (got < 0) {
            ct->Lock();
            ct->handle2fd.Insert(handle, chunk_fd);
            ct->Unlock();
            return got;
        }
        overall += static_cast<size_t>(got);
        ++chunk_idx;
        offset_in_chunk = 0;
    } while (overall < length && chunk_idx < chunks.list->size());

    ct->Lock();
    ct->handle2fd.Insert(handle, chunk_fd);
    ct->Unlock();

    return static_cast<int64_t>(overall);
}

- (void)releaseChunkedFile:(uint64_t)handle inode:(uint64_t)ino {
    ChunkTables *ct = _mountPoint->chunk_tables();

    ct->Lock();
    uint64_t unique_inode;
    if (!ct->handle2uniqino.Lookup(handle, &unique_inode))
        unique_inode = ino;
    else
        ct->handle2uniqino.Erase(handle);

    ChunkFd chunk_fd;
    if (ct->handle2fd.Lookup(handle, &chunk_fd))
        ct->handle2fd.Erase(handle);

    uint32_t ref = 0;
    if (ct->inode2references.Lookup(unique_inode, &ref)) {
        if (--ref == 0) {
            FileChunkReflist to_delete;
            if (ct->inode2chunks.Lookup(unique_inode, &to_delete)) {
                ct->inode2references.Erase(unique_inode);
                ct->inode2chunks.Erase(unique_inode);
                delete to_delete.list;
            }
        } else {
            ct->inode2references.Insert(unique_inode, ref);
        }
    }
    ct->Unlock();

    if (chunk_fd.fd != -1)
        _fileSystem->cache_mgr()->Close(chunk_fd.fd);
}

// ── Extended attributes ────────────────────────────────────────────────────

- (nullable NSData *)getXattrNamed:(NSString *)nameStr
                          forInode:(uint64_t)ino
                            dirent:(CvmfsDirent)d
                           errCode:(int *)outErrno {
    const std::string attr(nameStr.UTF8String);
    *outErrno = 0;

    // Rebuild a DirectoryEntry skeleton for the magic-xattr manager.
    catalog::DirectoryEntry entry;
    // For the GetLocked / PrepareValueFenced path we need the full entry from
    // the catalog.  Get it from the path cache.
    PathString path;
    if (!GetPathForInode(_mountPoint, ino, &path)) {
        *outErrno = ESTALE;
        return nil;
    }

    catalog::ClientCatalogManager *cmgr = _mountPoint->catalog_mgr();
    if (!cmgr->LookupPath(path, catalog::kLookupDefault, &entry)) {
        *outErrno = ESTALE;
        return nil;
    }
    entry.set_inode(ino);

    // Fetch catalog xattrs if the entry has any.
    XattrList xattrs;
    if (entry.HasXattrs()) {
        if (!cmgr->LookupXattrs(path, &xattrs)) {
            *outErrno = ESTALE;
            return nil;
        }
    }

    // Check for raw symlink if it's a symlink (magic xattrs may need it).
    if (entry.IsLink()) {
        catalog::DirectoryEntry raw;
        const catalog::LookupOptions opts =
            static_cast<catalog::LookupOptions>(
                catalog::kLookupDefault | catalog::kLookupRawSymlink);
        if (cmgr->LookupPath(path, opts, &raw))
            entry.set_symlink(raw.symlink());
    }

    // Parse paged-attribute request suffix (@ or ~).
    int32_t attr_req_page = 0;
    bool    attr_req_valid = false;
    MagicXattrMode xattr_mode = kXattrMachineMode;
    std::string base_attr = attr;

    auto parse_suffix = [&](const std::string &sep_str, MagicXattrMode mode) {
        const char sep = sep_str[0];
        size_t pos = attr.rfind(sep);
        if (pos == std::string::npos || pos == 0) return;
        const std::string token = attr.substr(pos + 1);
        if (token == "?") {
            attr_req_page  = -1;
            attr_req_valid = true;
        } else {
            bool all_digits = !token.empty();
            for (char c : token) all_digits = all_digits && (c >= '0' && c <= '9');
            if (all_digits) {
                attr_req_page  = static_cast<int32_t>(String2Uint64(token));
                attr_req_valid = true;
            }
        }
        if (attr_req_valid) {
            xattr_mode = mode;
            base_attr  = attr.substr(0, pos);
        }
    };

    if (attr.find('@') != std::string::npos)
        parse_suffix("@", kXattrHumanMode);
    else if (attr.find('~') != std::string::npos)
        parse_suffix("~", kXattrMachineMode);
    else
        attr_req_valid = true;   // plain name, page 0

    if (!attr_req_valid) {
        *outErrno = ENOATTR;
        return nil;
    }

    // Try magic xattr manager first.
    MagicXattrManager *mgr = _mountPoint->magic_xattr_mgr();
    const MagicXattrRAIIWrapper magic(
        mgr->GetLocked(base_attr, path, &entry));

    if (!magic.IsNull()) {
        // PrepareValueFencedProtected with gid=0 (no VOMS check).
        if (!magic->PrepareValueFencedProtected(0)) {
            *outErrno = ENOATTR;
            return nil;
        }
        const std::pair<bool, std::string> result =
            magic->GetValue(attr_req_page, xattr_mode);
        if (!result.first) {
            *outErrno = ENODATA;
            return nil;
        }
        return [NSData dataWithBytes:result.second.data()
                              length:result.second.size()];
    }

    // Fall back to catalog xattrs.
    std::string val;
    if (!xattrs.Get(base_attr, &val)) {
        *outErrno = ENOATTR;
        return nil;
    }
    return [NSData dataWithBytes:val.data() length:val.size()];
}

- (NSArray<NSString *> *)listXattrsForInode:(uint64_t)ino
                                     dirent:(CvmfsDirent)d {
    PathString path;
    if (!GetPathForInode(_mountPoint, ino, &path))
        return @[];

    catalog::ClientCatalogManager *cmgr = _mountPoint->catalog_mgr();
    catalog::DirectoryEntry entry;
    if (!cmgr->LookupPath(path, catalog::kLookupDefault, &entry))
        return @[];
    entry.set_inode(ino);

    XattrList xattrs;
    if (entry.HasXattrs())
        cmgr->LookupXattrs(path, &xattrs);

    // Magic xattr list + catalog xattr list.
    const std::string list_str =
        _mountPoint->magic_xattr_mgr()->GetListString(&entry);
    const std::string full = list_str + xattrs.ListKeysPosix(list_str);

    // full is a null-byte separated list.
    NSMutableArray<NSString *> *result = [NSMutableArray array];
    size_t i = 0;
    while (i < full.size()) {
        const char *start = full.c_str() + i;
        size_t len = strlen(start);
        if (len > 0)
            [result addObject:[NSString stringWithUTF8String:start]];
        i += len + 1;
    }
    return result;
}

// ── StatFS ─────────────────────────────────────────────────────────────────

- (CvmfsStatFS)statfs {
    CvmfsStatFS out;
    memset(&out, 0, sizeof(out));

    QuotaManager *qmgr = _fileSystem->cache_mgr()->quota_mgr();
    out.blockSize = 512;

    if (!qmgr->HasCapability(QuotaManager::kCapIntrospectSize)) {
        // Fall back to MountPoint's cached statvfs.
        const struct statvfs *info = _mountPoint->statfs_cache()->info();
        out.totalBlocks = info->f_blocks;
        out.freeBlocks  = info->f_bfree;
    } else {
        const uint64_t used     = qmgr->GetSize();
        const uint64_t capacity = qmgr->GetCapacity();
        if (capacity == static_cast<uint64_t>(-1)) {
            out.totalBlocks = used / out.blockSize;
        } else {
            out.totalBlocks = capacity / out.blockSize;
            out.freeBlocks  = (capacity - used) / out.blockSize;
        }
    }

    out.totalFiles = _mountPoint->catalog_mgr()->all_inodes();
    out.freeFiles  = out.totalFiles
                   - _mountPoint->catalog_mgr()->loaded_inodes();
    return out;
}

@end
