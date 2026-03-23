/**
 * CvmfsExtension.swift
 *
 * FSKit extension entry point.
 *
 * Architecture:
 *   - CvmfsExtension (struct) conforms to UnaryFileSystemExtension
 *   - CvmfsFileSystem (class) subclasses FSUnaryFileSystem and conforms to
 *     FSUnaryFileSystemOperations
 *   - loadResource creates a CvmfsBridge + CvmfsFS pair per mount
 *
 * Mount-resource URL format:
 *   cvmfs://<fqrn>?config=<path>
 *
 * Example:
 *   cvmfs://atlas.cern.ch?config=/etc/cvmfs/repositories.d/atlas.cern.ch/client.conf
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
    var fileSystem: CvmfsFileSystem & FSUnaryFileSystemOperations {
        CvmfsFileSystem()
    }
}

// ---------------------------------------------------------------------------
// CvmfsFileSystem — FSUnaryFileSystem subclass
// ---------------------------------------------------------------------------

final class CvmfsFileSystem: FSUnaryFileSystem, FSUnaryFileSystemOperations {

    private let logger = Logger(subsystem: "org.cvmfs.fskit", category: "CvmfsFileSystem")

    // ── Probe ──────────────────────────────────────────────────────────────

    func probeResource(
        resource: FSResource,
        replyHandler: @escaping (FSProbeResult?, (any Error)?) -> Void
    ) {
        logger.debug("probeResource: \(resource, privacy: .public)")

        do {
            let params = try parseMountParams(resource: resource, options: FSTaskOptions())
            let containerID = FSContainerIdentifier(
                uuid: deterministicUUID(from: params.fqrn))
            replyHandler(
                FSProbeResult.usable(name: params.fqrn, containerID: containerID),
                nil)
        } catch {
            // If we cannot parse the resource at all, treat it as unusable.
            replyHandler(FSProbeResult.unusable, nil)
        }
    }

    // ── Load ───────────────────────────────────────────────────────────────

    func loadResource(
        resource: FSResource,
        options: FSTaskOptions,
        replyHandler: @escaping (FSVolume?, (any Error)?) -> Void
    ) {
        logger.debug("loadResource: \(resource, privacy: .public)")

        do {
            let params = try parseMountParams(resource: resource, options: options)
            logger.info("Mounting CVMFS repository '\(params.fqrn)'")

            var bridgeError: NSError? = nil
            guard let bridge = CvmfsBridge(
                fqrn:       params.fqrn,
                configFile: params.configFile,
                error:      &bridgeError)
            else {
                let err = bridgeError ?? NSError(
                    domain: "CvmfsBridgeError", code: Int(EIO),
                    userInfo: [NSLocalizedDescriptionKey: "CvmfsBridge init failed"])
                logger.error("loadResource bridge init failed: \(err, privacy: .public)")
                containerStatus = .notReady(err)
                replyHandler(nil, err)
                return
            }

            containerStatus = .ready
            replyHandler(
                CvmfsFS(
                    bridge:     bridge,
                    volumeID:   FSVolume.Identifier(uuid: deterministicUUID(from: params.fqrn)),
                    volumeName: FSFileName(string: params.fqrn)),
                nil)
        } catch {
            logger.error("loadResource failed: \(error, privacy: .public)")
            containerStatus = .notReady(error)
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

/// Parse mount parameters from the FSResource URL and FSTaskOptions.
///
/// Resolution order:
/// 1. Resource URL host → fqrn, query `config` → configFile
/// 2. Options keys `fqrn` and `config`
private func parseMountParams(
    resource: FSResource,
    options: FSTaskOptions
) throws -> MountParams {
    var fqrn       = ""
    var configFile = ""

    if let url = resource.url {
        if let host = url.host, !host.isEmpty {
            fqrn = host
        }
        if let comps = URLComponents(url: url, resolvingAgainstBaseURL: false),
           let param = comps.queryItems?.first(where: { $0.name == "config" }),
           let val   = param.value, !val.isEmpty {
            configFile = val
        }
    }

    if fqrn.isEmpty,
       let v = options.optionValue(forKey: "fqrn") as? String, !v.isEmpty {
        fqrn = v
    }
    if configFile.isEmpty,
       let v = options.optionValue(forKey: "config") as? String, !v.isEmpty {
        configFile = v
    }

    guard !fqrn.isEmpty else {
        throw fs_errorForPOSIXError(EINVAL)
    }

    if configFile.isEmpty {
        configFile = "/etc/cvmfs/repositories.d/\(fqrn)/client.conf"
    }

    return MountParams(fqrn: fqrn, configFile: configFile)
}

// ---------------------------------------------------------------------------
// Stable UUID from FQRN (version-5-like name-based derivation)
// ---------------------------------------------------------------------------

/// Derive a stable UUID from a repository FQRN so the same repository always
/// presents the same container identity across remounts.
func deterministicUUID(from name: String) -> UUID {
    let nsBytes: [UInt8] = [                   // DNS namespace UUID
        0x6b, 0xa7, 0xb8, 0x10, 0x9d, 0xad, 0x11, 0xd1,
        0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8
    ]
    var data = Data(nsBytes)
    data.append(name.data(using: .utf8) ?? Data())

    // Mix via two FNV-1a-inspired 64-bit accumulators.
    var h0: UInt64 = 0xcbf2_9ce4_8422_2325
    var h1: UInt64 = 0x1000_0000_0000_0000
    for byte in data {
        h0 ^= UInt64(byte)
        h0 = h0 &* 0x0000_0100_0000_01B3
        h1 = h1 &* 0xFEFE_FEFE_FEFE_FEFE &+ UInt64(byte)
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
