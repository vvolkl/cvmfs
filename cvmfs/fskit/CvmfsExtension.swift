/**
 * CvmfsExtension.swift
 *
 * FSKit extension entry point.  CVMFS is not a network filesystem in the FSKit
 * sense — all HTTP transport is handled internally by the C++ core.  From the
 * OS perspective it looks like a pseudo-device filesystem mounted with:
 *
 *   mount -t cvmfs atlas.cern.ch /cvmfs/atlas.cern.ch
 *
 * The FQRN ("atlas.cern.ch") and optional config path arrive through
 * FSTaskOptions.taskOptions, which contains the raw mount arguments as strings.
 *
 * FSResource is essentially a placeholder for this filesystem type and is
 * not inspected beyond what FSKit requires.
 *
 * This file is part of the CernVM File System — FSKit port.
 */

import Foundation
import FSKit
import os

// ---------------------------------------------------------------------------
// Extension entry point
// ---------------------------------------------------------------------------

@main
struct CvmfsExtension: UnaryFileSystemExtension {
    // The associated type must be a concrete class, not a protocol composition.
    var fileSystem: CvmfsFileSystem {
        CvmfsFileSystem()
    }
}

// ---------------------------------------------------------------------------
// CvmfsFileSystem — FSUnaryFileSystem subclass
// ---------------------------------------------------------------------------

final class CvmfsFileSystem: FSUnaryFileSystem, FSUnaryFileSystemOperations {

    private let logger = Logger(subsystem: "org.cvmfs.fskit", category: "CvmfsFileSystem")

    // ── Probe ──────────────────────────────────────────────────────────────
    // CVMFS never needs to inspect a block device; it always recognises its
    // own mount requests. The probe result carries a stable per-FQRN UUID.

    func probeResource(
        resource: FSResource,
        replyHandler: @escaping (FSProbeResult?, (any Error)?) -> Void
    ) {
        logger.debug("probeResource")
        // We cannot read the FQRN at probe time (options are not available
        // here), so return a generic recognised result. The real per-volume
        // identity is established in loadResource.
        let containerID = FSContainerIdentifier(
            uuid: NSUUID(uuidString: "6B43564D-4653-0000-0000-000000000001")! as UUID)
        replyHandler(
            FSProbeResult.usable(name: "cvmfs", containerID: containerID),
            nil)
    }

    // ── Load ───────────────────────────────────────────────────────────────

    func loadResource(
        resource: FSResource,
        options: FSTaskOptions,
        replyHandler: @escaping (FSVolume?, (any Error)?) -> Void
    ) {
        logger.debug("loadResource")

        do {
            let params = try parseMountParams(options: options)
            logger.info("Mounting CVMFS repository '\(params.fqrn)'")

            // CvmfsBridge init is translated by Swift as a throwing initialiser
            // (the NSError** parameter becomes implicit).
            let bridge = try CvmfsBridge(fqrn: params.fqrn,
                                         configFile: params.configFile)

            containerStatus = FSContainerStatus.ready

            replyHandler(
                CvmfsFS(
                    bridge:     bridge,
                    volumeID:   FSVolume.Identifier(
                                    uuid: deterministicUUID(from: params.fqrn)),
                    volumeName: FSFileName(string: params.fqrn)),
                nil)
        } catch {
            logger.error("loadResource failed: \(error, privacy: .public)")
            containerStatus = FSContainerStatus.notReady(status: error as NSError)
            replyHandler(nil, error)
        }
    }

    // ── Unload ─────────────────────────────────────────────────────────────

    func unloadResource(
        resource: FSResource,
        options: FSTaskOptions,
        replyHandler: @escaping ((any Error)?) -> Void
    ) {
        logger.debug("unloadResource")
        replyHandler(nil)
    }

    func didFinishLoading() {
        logger.debug("didFinishLoading")
    }
}

// ---------------------------------------------------------------------------
// Mount parameter parsing
// ---------------------------------------------------------------------------

private struct MountParams {
    let fqrn:       String
    let configFile: String
}

/// Parse mount parameters from FSTaskOptions.
///
/// FSTaskOptions.taskOptions contains the raw mount arguments as an array of
/// strings in the form the mount helper delivers them. Typical layout for:
///
///   mount -t cvmfs atlas.cern.ch /cvmfs/atlas.cern.ch
///   mount -t cvmfs -o config=/etc/cvmfs/atlas.conf atlas.cern.ch /cvmfs/...
///
/// The FQRN is either the first non-option token (no "=" in it, contains ".")
/// or an explicit "fqrn=<name>" entry.  The config is "config=<path>".
private func parseMountParams(options: FSTaskOptions) throws -> MountParams {
    let tokens = options.taskOptions   // [String]

    var fqrn       = ""
    var configFile = ""

    for token in tokens {
        if token.hasPrefix("fqrn=") {
            fqrn = String(token.dropFirst(5))
        } else if token.hasPrefix("config=") {
            configFile = String(token.dropFirst(7))
        } else if !token.contains("=") && token.contains(".") && fqrn.isEmpty {
            // Looks like a bare FQRN (e.g. "atlas.cern.ch").
            fqrn = token
        }
    }

    guard !fqrn.isEmpty else {
        throw fs_errorForPOSIXError(EINVAL)
    }

    if configFile.isEmpty {
        // Standard search path used by the CVMFS configuration package.
        let paths = [
            "/etc/cvmfs/config.d/\(fqrn).conf",
            "/etc/cvmfs/repositories.d/\(fqrn)/client.conf",
        ]
        configFile = paths.first { FileManager.default.fileExists(atPath: $0) }
                  ?? paths[0]   // will produce a clear error from the bridge
    }

    return MountParams(fqrn: fqrn, configFile: configFile)
}

// ---------------------------------------------------------------------------
// Stable UUID from FQRN
// ---------------------------------------------------------------------------

/// Derive a deterministic UUID from a repository FQRN so the same repository
/// always presents the same container identity across remounts.
func deterministicUUID(from name: String) -> UUID {
    // FNV-1a-inspired mix into 16 bytes.
    var h0: UInt64 = 0xcbf2_9ce4_8422_2325
    var h1: UInt64 = 0x1000_0000_0000_0000
    let nsBytes: [UInt8] = [                    // DNS namespace UUID bytes
        0x6b, 0xa7, 0xb8, 0x10, 0x9d, 0xad, 0x11, 0xd1,
        0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8
    ]
    for byte in nsBytes + Array((name.utf8)) {
        h0 ^= UInt64(byte)
        h0  = h0 &* 0x0000_0100_0000_01B3
        h1  = h1 &* 0xFEFE_FEFE_FEFE_FEFE &+ UInt64(byte)
    }

    var out = [UInt8](repeating: 0, count: 16)
    withUnsafeBytes(of: h0.bigEndian) { out.replaceSubrange(0..<8,  with: $0) }
    withUnsafeBytes(of: h1.bigEndian) { out.replaceSubrange(8..<16, with: $0) }
    out[6] = (out[6] & 0x0F) | 0x50   // version 5
    out[8] = (out[8] & 0x3F) | 0x80   // variant 1

    return UUID(uuid: (
        out[0],  out[1],  out[2],  out[3],
        out[4],  out[5],  out[6],  out[7],
        out[8],  out[9],  out[10], out[11],
        out[12], out[13], out[14], out[15]
    ))
}
