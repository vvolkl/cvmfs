// main.swift — Minimal host process for the CvmfsFSKitExtension bundle.
//
// macOS requires every filesystem extension (.appex) to live inside a regular
// application bundle (.app).  This binary IS that host application.  It is
// never launched interactively; the OS only uses the bundle as a container for
// discovering and activating the embedded extension through pluginkit / FSKit.
//
// The binary exits immediately and carries no UI or user-visible behaviour.

import Foundation

// Swift's top-level code in main.swift serves as the entry point.
// An empty body is valid; the process starts, runs module initialisers,
// and exits with code 0.
