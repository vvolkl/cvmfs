/**
 * CvmfsErrors.swift
 *
 * POSIX errno ↔ Swift Error helpers for the FSKit layer.
 *
 * This file is part of the CernVM File System — FSKit port.
 */

import Foundation
import FSKit

// ---------------------------------------------------------------------------
// POSIX → FSKit error
// ---------------------------------------------------------------------------

/// Wrap a raw POSIX errno value in an Error that FSKit understands.
@inline(__always)
func cvmfsError(_ posixErrno: Int32) -> Error {
    return fs_errorForPOSIXError(posixErrno)
}

// Convenience aliases for the most common errors in cvmfs callbacks.
enum CvmfsError {
    static var notFound:  Error { cvmfsError(ENOENT)   }
    static var io:        Error { cvmfsError(EIO)       }
    static var notDir:    Error { cvmfsError(ENOTDIR)   }
    static var notLink:   Error { cvmfsError(EINVAL)    }
    static var stale:     Error { cvmfsError(ESTALE)    }
    static var tooManyFd: Error { cvmfsError(EMFILE)    }
    static var noAttr:    Error { cvmfsError(ENOATTR)   }
    static var noData:    Error { cvmfsError(ENODATA)   }
    static var range:     Error { cvmfsError(ERANGE)    }
}

// ---------------------------------------------------------------------------
// CvmfsDirent → Swift error
// ---------------------------------------------------------------------------

extension CvmfsDirent {
    /// Translate a not-found dirent into the appropriate FSKit error.
    /// isNegative==1 → ENOENT, otherwise → EIO.
    var notFoundError: Error {
        return isNegative != 0 ? CvmfsError.notFound : CvmfsError.io
    }
}
