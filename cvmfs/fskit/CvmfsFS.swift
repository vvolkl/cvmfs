/**
 * CvmfsFS.swift
 *
 * FSVolume subclass implementing all CernVM-FS read-only filesystem operations
 * for the native macOS FSKit layer.
 *
 * Replaces the FUSE low-level callbacks in cvmfs/cvmfs.cc.
 *
 * This file is part of the CernVM File System — FSKit port.
 */

import Foundation
import FSKit
import os

// ---------------------------------------------------------------------------
// CvmfsFS
// ---------------------------------------------------------------------------

final class CvmfsFS: FSVolume {

    let bridge: CvmfsBridge
    private var rootItem: CvmfsItem?
    private let logger = Logger(subsystem: "org.cvmfs.fskit", category: "CvmfsFS")

    init(bridge: CvmfsBridge, volumeID: FSVolume.Identifier, volumeName: FSFileName) {
        self.bridge = bridge
        super.init(volumeID: volumeID, volumeName: volumeName)
    }
}

// ---------------------------------------------------------------------------
// PathConfOperations
// ---------------------------------------------------------------------------

extension CvmfsFS: FSVolume.PathConfOperations {
    var maximumLinkCount: Int { return Int(UInt32.max) }
    var maximumNameLength: Int { return 255 }
    var restrictsOwnershipChanges: Bool { return true }   // read-only
    var truncatesLongNames: Bool { return false }
    var maximumXattrSize: Int { return Int(UInt32.max) }
    var maximumFileSize: UInt64 { return UInt64.max }
}

// ---------------------------------------------------------------------------
// FSVolume.Operations
// ---------------------------------------------------------------------------

extension CvmfsFS: FSVolume.Operations {

    // ── Capabilities ──────────────────────────────────────────────────────

    var supportedVolumeCapabilities: FSVolume.SupportedCapabilities {
        let c = FSVolume.SupportedCapabilities()
        c.supportsSymbolicLinks       = true
        c.supportsPersistentObjectIDs = true
        c.supportsHardLinks           = true
        c.doesNotSupportVolumeSizes   = false
        c.supports64BitObjectIDs      = true
        c.caseFormat                  = .sensitive
        return c
    }

    // ── StatFS ─────────────────────────────────────────────────────────────

    var volumeStatistics: FSStatFSResult {
        let s = bridge.statfs()
        let r = FSStatFSResult(fileSystemTypeName: "cvmfs")
        r.blockSize       = Int(s.blockSize)
        r.ioSize          = Int(s.blockSize)
        r.totalBlocks     = s.totalBlocks
        r.availableBlocks = s.freeBlocks
        r.freeBlocks      = s.freeBlocks
        r.totalFiles      = s.totalFiles
        r.freeFiles       = s.freeFiles
        return r
    }

    // ── Lifecycle ──────────────────────────────────────────────────────────

    func activate(options: FSTaskOptions) async throws -> FSItem {
        logger.debug("activate")
        bridge.tryFinishRemount()

        bridge.enterFence()
        var dirent = CvmfsDirent()
        let found = bridge.getDirentForInode(bridge.rootInode, result: &dirent)
        bridge.leaveFence()

        guard found else {
            logger.error("activate: root inode lookup failed")
            throw dirent.notFoundError
        }

        let root = CvmfsItem.make(from: dirent)
        root.attributes.type     = .directory
        root.attributes.fileID   = .rootDirectory
        root.attributes.parentID = .parentOfRoot
        rootItem = root
        return root
    }

    func deactivate(options: FSDeactivateOptions = []) async throws {
        logger.debug("deactivate")
        bridge.shutdown()
        rootItem = nil
    }

    func mount(options: FSTaskOptions) async throws {
        logger.debug("mount")
    }

    func unmount() async {
        logger.debug("unmount")
    }

    func synchronize(flags: FSSyncFlags) async throws {
        // Read-only filesystem — nothing to sync.
    }

    // ── Attributes ─────────────────────────────────────────────────────────

    func attributes(
        _ desiredAttributes: FSItem.GetAttributesRequest,
        of item: FSItem
    ) async throws -> FSItem.Attributes {
        guard let cvItem = item as? CvmfsItem else { throw CvmfsError.io }
        let ino = bridge.mangleInode(cvItem.inode)
        logger.debug("getattr inode=\(ino)")

        bridge.enterFence()
        var dirent = CvmfsDirent()
        let found = bridge.getDirentForInode(ino, result: &dirent)
        bridge.leaveFence()

        // If the inode was not found, try serving a stale-but-open file from
        // the page-cache tracker (mirrors the cvmfs_getattr stale-inode path).
        if !found {
            var hashBuf = [UInt8](repeating: 0, count: 20)
            var algByte: UInt8 = 0
            var st = stat()
            let isOpen = bridge.pageTrackerGetInfoIfOpen(
                ino, contentHash: &hashBuf, algorithm: &algByte, stat: &st)
            if isOpen {
                return attributesFromStat(st, fileID: cvItem.attributes.fileID)
            }
            throw dirent.notFoundError
        }

        // For regular files that may be in the page-cache tracker, check for
        // a stale open version and serve that instead.
        if dirent.isDirectory == 0 && dirent.isLink == 0 {
            var hashBuf = [UInt8](repeating: 0, count: 20)
            var algByte: UInt8 = 0
            var st = stat()
            let isOpen = bridge.pageTrackerGetInfoIfOpen(
                ino, contentHash: &hashBuf, algorithm: &algByte, stat: &st)
            if isOpen {
                return attributesFromStat(st, fileID: cvItem.attributes.fileID)
            }
        }

        // Build a fresh attribute set.  Preserve fileID established at
        // lookup time (the root has .rootDirectory; others have their inode).
        let attrs = buildAttributes(from: dirent, fileID: cvItem.attributes.fileID)
        // Keep the item's in-memory attributes up to date.
        cvItem.applyDirent(dirent)
        cvItem.attributes.fileID = attrs.fileID   // preserve original fileID
        return attrs
    }

    func setAttributes(
        _ newAttributes: FSItem.SetAttributesRequest,
        on item: FSItem
    ) async throws -> FSItem.Attributes {
        // CVMFS is read-only.  Accept the call (FSKit may set atime) but only
        // update the in-memory CvmfsItem attributes; never touch the catalog.
        guard let cvItem = item as? CvmfsItem else { throw CvmfsError.io }
        mergeSetAttributes(newAttributes, into: cvItem.attributes)
        return cvItem.attributes
    }

    // ── Lookup ─────────────────────────────────────────────────────────────

    func lookupItem(
        named name: FSFileName,
        inDirectory directory: FSItem
    ) async throws -> (FSItem, FSFileName) {
        guard let parentItem = directory as? CvmfsItem else { throw CvmfsError.io }
        guard let nameStr = name.string, !nameStr.isEmpty else { throw CvmfsError.notFound }

        bridge.tryFinishRemount()
        bridge.enterFence()

        let parentIno = bridge.mangleInode(parentItem.inode)
        logger.debug("lookup '\(nameStr)' in inode=\(parentIno)")

        // Resolve parent path.
        guard let parentPath = bridge.getPathForInode(parentIno) else {
            bridge.leaveFence()
            throw CvmfsError.notFound
        }

        let fullPath = parentPath.isEmpty ? "/\(nameStr)" : "\(parentPath)/\(nameStr)"
        var dirent = CvmfsDirent()
        let liveInode = bridge.getDirentForPath(fullPath, result: &dirent)

        if liveInode == 0 {
            bridge.leaveFence()
            throw dirent.notFoundError
        }

        // If the live inode is stale (open file whose content changed), the
        // bridge's GetDirentForPath already returned the new inode.  We need
        // to update the inode tracker accordingly.
        if liveInode > 1 {
            // Replace the stale live inode with the catalog's current one.
            // vfsGetInode below will register the new inode.
            // The old inode entry will be cleaned up by the next reclaimItem.
        }

        bridge.vfsGetInode(dirent.inode, mode: dirent.mode, path: fullPath)
        bridge.leaveFence()

        let child = CvmfsItem(inode: dirent.inode)
        child.applyDirent(dirent)
        // Use the CVMFS inode as the FSKit item identifier (inodes are >= 256
        // for CVMFS, safely above the reserved FSKit values 0/1/2).
        child.attributes.fileID  = FSItem.Identifier(rawValue: dirent.inode) ?? .invalid
        child.attributes.parentID = FSItem.Identifier(rawValue: parentIno)   ?? .invalid
        return (child, name)
    }

    // ── Reclaim ────────────────────────────────────────────────────────────

    func reclaimItem(_ item: FSItem) async throws {
        guard let cvItem = item as? CvmfsItem else { return }
        logger.debug("reclaim inode=\(cvItem.inode)")
        // nlookup = 1: FSKit never batches forgets.
        let removed = bridge.vfsPutInode(cvItem.inode, nlookup: 1)
        if removed {
            bridge.evictPageCacheInode(cvItem.inode)
        }
    }

    // ── Readlink ───────────────────────────────────────────────────────────

    func readSymbolicLink(_ item: FSItem) async throws -> FSFileName {
        guard let cvItem = item as? CvmfsItem else { throw CvmfsError.io }
        let ino = bridge.mangleInode(cvItem.inode)
        logger.debug("readlink inode=\(ino)")

        bridge.enterFence()
        var dirent = CvmfsDirent()
        let found = bridge.getDirentForInode(ino, result: &dirent)
        bridge.leaveFence()

        guard found else { throw dirent.notFoundError }
        guard dirent.isLink != 0 else { throw CvmfsError.notLink }

        let target = withUnsafeBytes(of: dirent.symlinkTarget) { buf -> String in
            let ptr = buf.baseAddress!.assumingMemoryBound(to: CChar.self)
            return String(cString: ptr)
        }
        return FSFileName(string: target)
    }

    // ── Directory enumeration ──────────────────────────────────────────────

    func enumerateDirectory(
        _ directory: FSItem,
        startingAt cookie: FSDirectoryCookie,
        verifier: FSDirectoryVerifier,
        attributes desiredAttrs: FSItem.GetAttributesRequest?,
        packer: FSDirectoryEntryPacker
    ) async throws -> FSDirectoryVerifier {
        guard let dirItem = directory as? CvmfsItem else { throw CvmfsError.io }

        bridge.tryFinishRemount()
        bridge.enterFence()

        let ino = bridge.mangleInode(dirItem.inode)
        logger.debug("readdir inode=\(ino)")

        guard let dirPath = bridge.getPathForInode(ino) else {
            bridge.leaveFence()
            throw CvmfsError.notFound
        }

        var dirDirent = CvmfsDirent()
        guard bridge.getDirentForInode(ino, result: &dirDirent) else {
            bridge.leaveFence()
            throw dirDirent.notFoundError
        }
        guard dirDirent.isDirectory != 0 else {
            bridge.leaveFence()
            throw CvmfsError.notDir
        }

        // Build the listing while holding the fence.
        var entriesPtr: UnsafeMutablePointer<CvmfsStatEntry>? = nil
        var count: UInt = 0
        guard bridge.listDirectory(dirPath,
                                   entries: &entriesPtr,
                                   count: &count) else {
            bridge.leaveFence()
            throw CvmfsError.io
        }
        bridge.leaveFence()

        defer { entriesPtr?.deallocate() }

        let startIdx = Int(cookie.rawValue)
        let n = Int(count)

        for idx in startIdx ..< n {
            let se = entriesPtr![idx]
            let entryName: String = withUnsafeBytes(of: se.name) { buf in
                let ptr = buf.baseAddress!.assumingMemoryBound(to: CChar.self)
                return String(cString: ptr)
            }

            var itemAttrs: FSItem.Attributes? = nil
            if desiredAttrs != nil {
                let a = FSItem.Attributes()
                a.fileID    = FSItem.Identifier(rawValue: se.inode) ?? .invalid
                a.mode      = se.mode
                a.uid       = se.uid
                a.gid       = se.gid
                a.size      = se.size
                a.linkCount = se.nlink
                a.type      = itemTypeFromMode(se.mode)
                var ts = timespec(tv_sec: __darwin_time_t(se.mtime), tv_nsec: 0)
                a.modifyTime = ts
                a.changeTime = ts
                a.accessTime = ts
                itemAttrs = a
            }

            let packed = packer.packEntry(
                name:       FSFileName(string: entryName),
                itemType:   itemTypeFromMode(se.mode),
                itemID:     FSItem.Identifier(rawValue: se.inode) ?? .invalid,
                nextCookie: FSDirectoryCookie(UInt64(idx + 1)),
                attributes: itemAttrs
            )

            if !packed {
                // Buffer full — FSKit will call us again with cookie = idx + 1.
                logger.debug("readdir: packer full at idx=\(idx)")
                break
            }
        }

        return FSDirectoryVerifier(0)
    }

    // ── Create / write / remove — stubs (read-only FS) ────────────────────

    func createItem(
        named name: FSFileName,
        type: FSItem.ItemType,
        inDirectory directory: FSItem,
        attributes newAttributes: FSItem.SetAttributesRequest
    ) async throws -> (FSItem, FSFileName) {
        throw cvmfsError(EROFS)
    }

    func createSymbolicLink(
        named name: FSFileName,
        inDirectory directory: FSItem,
        attributes newAttributes: FSItem.SetAttributesRequest,
        linkContents contents: FSFileName
    ) async throws -> (FSItem, FSFileName) {
        throw cvmfsError(EROFS)
    }

    func createLink(
        to item: FSItem,
        named name: FSFileName,
        inDirectory directory: FSItem
    ) async throws -> FSFileName {
        throw cvmfsError(EROFS)
    }

    func removeItem(
        _ item: FSItem,
        named name: FSFileName,
        fromDirectory directory: FSItem
    ) async throws {
        throw cvmfsError(EROFS)
    }

    func renameItem(
        _ item: FSItem,
        inDirectory sourceDirectory: FSItem,
        named sourceName: FSFileName,
        to destinationName: FSFileName,
        inDirectory destinationDirectory: FSItem,
        overItem: FSItem?
    ) async throws -> FSFileName {
        throw cvmfsError(EROFS)
    }
}

// ---------------------------------------------------------------------------
// FSVolume.OpenCloseOperations
// ---------------------------------------------------------------------------

extension CvmfsFS: FSVolume.OpenCloseOperations {

    func openItem(_ item: FSItem, modes: FSVolume.OpenModes) async throws {
        guard let cvItem = item as? CvmfsItem else { throw CvmfsError.io }

        bridge.tryFinishRemount()
        bridge.enterFence()

        let ino = bridge.mangleInode(cvItem.inode)
        logger.debug("open inode=\(ino)")

        guard let path = bridge.getPathForInode(ino) else {
            bridge.leaveFence()
            throw CvmfsError.notFound
        }

        var dirent = CvmfsDirent()
        guard bridge.getDirentForInode(ino, result: &dirent) else {
            bridge.leaveFence()
            throw dirent.notFoundError
        }

        bridge.leaveFence()

        // ── Chunked file ────────────────────────────────────────────────
        if dirent.isChunkedFile != 0 {
            var od = CvmfsOpenDirectives()
            let handle = bridge.openChunkedFile(
                forInode: ino, path: path, dirent: dirent, openDirectives: &od)
            guard handle >= 0 else { throw CvmfsError.io }
            cvItem.chunkHandle = UInt64(bitPattern: handle)
            cvItem.directIo    = od.directIo
            cvItem.keepCache   = od.keepCache
            return
        }

        // ── Regular file ────────────────────────────────────────────────
        // Page-cache tracker.
        let od: CvmfsOpenDirectives
        if dirent.isDirectIo != 0 {
            od = bridge.pageTrackerOpenDirect()
        } else {
            // Extract the C fixed-size array (tuple) into a Swift [UInt8].
            var hashBuf = [UInt8](repeating: 0, count: 20)
            withUnsafeBytes(of: dirent.contentHash) {
                hashBuf = Array($0.prefix(20))
            }
            let st = statFromDirent(dirent)
            od = hashBuf.withUnsafeBytes { ptr in
                bridge.pageTrackerOpenInode(
                    ino,
                    hash: ptr.baseAddress!.assumingMemoryBound(to: UInt8.self),
                    algorithm: dirent.contentHashAlgorithm,
                    stat: st)
            }
        }

        // Fetch file into cache.
        let fd = Int(bridge.fetchFile(forInode: ino, path: path, dirent: dirent))

        if fd < 0 {
            if dirent.isDirectIo == 0 && !od.directIo {
                bridge.pageTrackerCloseInode(ino)
            }
            throw cvmfsError(Int32(-fd))
        }

        cvItem.openFd    = Int32(fd)
        cvItem.directIo  = od.directIo
        cvItem.keepCache = od.keepCache
    }

    func closeItem(_ item: FSItem, modes: FSVolume.OpenModes) async throws {
        guard let cvItem = item as? CvmfsItem else { throw CvmfsError.io }
        let ino = bridge.mangleInode(cvItem.inode)
        logger.debug("close inode=\(ino)")

        if !cvItem.directIo {
            bridge.pageTrackerCloseInode(ino)
        }

        if let handle = cvItem.chunkHandle {
            bridge.releaseChunkedFile(handle, inode: ino)
            cvItem.chunkHandle = nil
        } else if cvItem.openFd >= 0 {
            _ = bridge.closeFd(cvItem.openFd)
            cvItem.openFd = -1
        }
    }
}

// ---------------------------------------------------------------------------
// FSVolume.ReadWriteOperations
// ---------------------------------------------------------------------------

extension CvmfsFS: FSVolume.ReadWriteOperations {

    func read(
        from item: FSItem,
        at offset: off_t,
        length: Int,
        into buffer: FSMutableFileDataBuffer
    ) async throws -> Int {
        guard let cvItem = item as? CvmfsItem else { throw CvmfsError.io }
        logger.debug("read inode=\(cvItem.inode) offset=\(offset) len=\(length)")

        let bytesRead: Int64 = buffer.withUnsafeMutableBytes { ptr -> Int64 in
            if let handle = cvItem.chunkHandle {
                return bridge.readChunkedFile(
                    handle,
                    offset: offset,
                    length: length,
                    buffer: ptr.baseAddress!)
            } else {
                guard cvItem.openFd >= 0 else { return Int64(-EIO) }
                return bridge.pread(
                    cvItem.openFd,
                    buffer: ptr.baseAddress!,
                    size: UInt64(length),
                    offset: UInt64(offset))
            }
        }

        if bytesRead < 0 { throw cvmfsError(Int32(-bytesRead)) }
        return Int(bytesRead)
    }

    func write(
        contents: Data,
        to item: FSItem,
        at offset: off_t
    ) async throws -> Int {
        throw cvmfsError(EROFS)
    }
}

// ---------------------------------------------------------------------------
// FSVolume.XattrOperations
// ---------------------------------------------------------------------------

extension CvmfsFS: FSVolume.XattrOperations {

    func xattr(named name: FSFileName, of item: FSItem) async throws -> Data {
        guard let cvItem = item as? CvmfsItem else { throw CvmfsError.io }
        guard let nameStr = name.string else { throw CvmfsError.noAttr }

        let ino = bridge.mangleInode(cvItem.inode)
        logger.debug("getxattr '\(nameStr)' inode=\(ino)")

        bridge.enterFence()
        var dirent = CvmfsDirent()
        guard bridge.getDirentForInode(ino, result: &dirent) else {
            bridge.leaveFence()
            throw dirent.notFoundError
        }
        bridge.leaveFence()

        var errCode: Int32 = 0
        guard let data = bridge.getXattrNamed(
            nameStr, forInode: ino, dirent: dirent, errCode: &errCode) else {
            throw cvmfsError(errCode != 0 ? errCode : ENOATTR)
        }
        return data
    }

    func setXattr(
        named name: FSFileName,
        to value: Data?,
        on item: FSItem,
        policy: FSVolume.SetXattrPolicy
    ) async throws {
        throw cvmfsError(EROFS)
    }

    func xattrs(of item: FSItem) async throws -> [FSFileName] {
        guard let cvItem = item as? CvmfsItem else { throw CvmfsError.io }
        let ino = bridge.mangleInode(cvItem.inode)
        logger.debug("listxattr inode=\(ino)")

        bridge.enterFence()
        var dirent = CvmfsDirent()
        guard bridge.getDirentForInode(ino, result: &dirent) else {
            bridge.leaveFence()
            throw dirent.notFoundError
        }
        bridge.leaveFence()

        let names = bridge.listXattrs(forInode: ino, dirent: dirent)
        return names.map { FSFileName(string: $0) }
    }
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

private extension CvmfsFS {

    /// Build FSItem.Attributes from a struct stat (page-cache-tracker path).
    /// Uses the caller-supplied fileID to preserve the FSKit identifier.
    func attributesFromStat(_ st: stat,
                            fileID: FSItem.Identifier = .invalid) -> FSItem.Attributes {
        let a = FSItem.Attributes()
        a.fileID    = fileID != .invalid
                        ? fileID
                        : (FSItem.Identifier(rawValue: UInt64(st.st_ino)) ?? .invalid)
        a.mode      = UInt32(st.st_mode)
        a.uid       = st.st_uid
        a.gid       = st.st_gid
        a.size      = UInt64(st.st_size)
        a.allocSize = UInt64(st.st_blocks) * 512
        a.linkCount = UInt32(st.st_nlink)
        a.type      = itemTypeFromMode(UInt32(st.st_mode))
        a.modifyTime = timespec(tv_sec: st.st_mtimespec.tv_sec,
                                tv_nsec: st.st_mtimespec.tv_nsec)
        a.changeTime = timespec(tv_sec: st.st_ctimespec.tv_sec,
                                tv_nsec: st.st_ctimespec.tv_nsec)
        a.accessTime = timespec(tv_sec: st.st_atimespec.tv_sec,
                                tv_nsec: st.st_atimespec.tv_nsec)
        a.birthTime  = a.modifyTime
        return a
    }

    /// Build FSItem.Attributes from a CvmfsDirent with explicit fileID.
    func buildAttributes(from d: CvmfsDirent,
                         fileID: FSItem.Identifier) -> FSItem.Attributes {
        let a = FSItem.Attributes()
        a.fileID    = fileID
        a.mode      = d.mode
        a.uid       = d.uid
        a.gid       = d.gid
        a.size      = d.size
        a.allocSize = d.size
        a.linkCount = d.nlink
        a.type      = d.isDirectory != 0 ? .directory :
                      d.isLink      != 0 ? .symlink : .file
        let ts = timespec(tv_sec: __darwin_time_t(d.mtime), tv_nsec: 0)
        a.modifyTime = ts
        a.changeTime = ts
        a.accessTime = ts
        a.birthTime  = ts
        return a
    }

    /// Build a struct stat from a CvmfsDirent (for page-cache-tracker calls).
    func statFromDirent(_ d: CvmfsDirent) -> stat {
        var st = stat()
        st.st_ino   = ino_t(d.inode)
        st.st_mode  = mode_t(d.mode)
        st.st_size  = off_t(d.size)
        st.st_nlink = nlink_t(d.nlink)
        st.st_uid   = uid_t(d.uid)
        st.st_gid   = gid_t(d.gid)
        st.st_mtimespec = timespec(tv_sec: __darwin_time_t(d.mtime), tv_nsec: 0)
        return st
    }

    /// Map S_IF* mode bits to FSItem.ItemType.
    func itemTypeFromMode(_ mode: UInt32) -> FSItem.ItemType {
        let fmt = mode & UInt32(S_IFMT)
        switch fmt {
        case UInt32(S_IFDIR): return .directory
        case UInt32(S_IFLNK): return .symlink
        default:              return .file
        }
    }

    /// Merge a SetAttributesRequest into existing Attributes (in-memory only).
    func mergeSetAttributes(
        _ req: FSItem.SetAttributesRequest,
        into attrs: FSItem.Attributes
    ) {
        if req.isValid(FSItem.Attribute.uid)       { attrs.uid       = req.uid }
        if req.isValid(FSItem.Attribute.gid)       { attrs.gid       = req.gid }
        if req.isValid(FSItem.Attribute.mode)      { attrs.mode      = req.mode }
        if req.isValid(FSItem.Attribute.size)      { attrs.size      = req.size }
        if req.isValid(FSItem.Attribute.linkCount) { attrs.linkCount = req.linkCount }
        if req.isValid(FSItem.Attribute.flags)     { attrs.flags     = req.flags }
        if req.isValid(FSItem.Attribute.accessTime) {
            attrs.accessTime = req.accessTime
        }
        if req.isValid(FSItem.Attribute.modifyTime) {
            attrs.modifyTime = req.modifyTime
        }
        if req.isValid(FSItem.Attribute.changeTime) {
            attrs.changeTime = req.changeTime
        }
    }
}


