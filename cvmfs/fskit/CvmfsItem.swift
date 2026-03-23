/**
 * CvmfsItem.swift
 *
 * FSItem subclass carrying all per-inode state that FUSE stored in fi->fh and
 * scattered across module-level globals.  One CvmfsItem instance is created
 * per lookup reply and kept alive by FSKit's reference counting until
 * reclaimItem() is called.
 *
 * This file is part of the CernVM File System — FSKit port.
 */

import Foundation
import FSKit

final class CvmfsItem: FSItem {

    // ── Identity ─────────────────────────────────────────────────────────
    /// The numeric inode from the C++ catalog (post-MangleInode).
    /// This is stable for the lifetime of the item.
    let inode: UInt64

    // ── FSKit attributes ──────────────────────────────────────────────────
    /// Cached attributes built from the catalog DirectoryEntry at lookup time.
    /// Mutated in memory by setAttributes (CVMFS is read-only so only in-RAM).
    var attributes: FSItem.Attributes = FSItem.Attributes()

    // ── Open-file state ────────────────────────────────────────────────────
    // Set in openItem(_:modes:), cleared in closeItem(_:modes:).

    /// Cache-manager file descriptor for a regular (non-chunked) open file.
    /// -1 when no regular file is open.
    var openFd: Int32 = -1

    /// Handle key in ChunkTables::handle2fd for chunked files.
    /// nil when the file is not chunked-open.
    var chunkHandle: UInt64? = nil

    /// Whether the open was forced to direct I/O (no kernel page cache).
    var directIo: Bool = false

    /// Whether the kernel page-cache entry should be kept across re-opens.
    var keepCache: Bool = false

    // ── Init ──────────────────────────────────────────────────────────────
    init(inode: UInt64) {
        self.inode = inode
        super.init()
    }

    // ── Computed helpers ──────────────────────────────────────────────────
    var isOpen: Bool { openFd >= 0 || chunkHandle != nil }
    var isChunked: Bool { chunkHandle != nil }
}

// ---------------------------------------------------------------------------
// CvmfsDirent → FSItem.Attributes
// ---------------------------------------------------------------------------

extension CvmfsItem {
    /// Populate the item's VFS attributes from a freshly-resolved dirent.
    /// NOTE: Does NOT touch fileID / parentID — those are set by the caller
    /// so that FSKit-level identifiers (.rootDirectory etc.) are preserved.
    func applyDirent(_ d: CvmfsDirent) {
        attributes.mode      = d.mode
        attributes.uid       = d.uid
        attributes.gid       = d.gid
        attributes.size      = d.size
        attributes.allocSize = d.size
        attributes.linkCount = d.nlink

        attributes.type =
            d.isDirectory != 0 ? .directory :
            d.isLink      != 0 ? .symlink :
                                  .file

        let ts = timespec(tv_sec: __darwin_time_t(d.mtime), tv_nsec: 0)
        attributes.modifyTime = ts
        attributes.changeTime = ts
        attributes.accessTime = ts
        attributes.birthTime  = ts
    }
}

// ---------------------------------------------------------------------------
// Static factory
// ---------------------------------------------------------------------------

extension CvmfsItem {
    /// Create a CvmfsItem and immediately apply the dirent's VFS attributes.
    /// The caller is responsible for setting fileID / parentID afterwards.
    static func make(from dirent: CvmfsDirent) -> CvmfsItem {
        let item = CvmfsItem(inode: dirent.inode)
        item.applyDirent(dirent)
        return item
    }
}
