# CMakeLists.txt — CernVM-FS FSKit Extension
#
# Builds the macOS FSKit filesystem extension (.appex bundle).
# Included by the root CMakeLists.txt when -DBUILD_FSKIT=yes is passed.
#
# Prerequisites (enforced by the root CMakeLists.txt):
#   - macOS host
#   - CMAKE_OSX_DEPLOYMENT_TARGET >= 15.0
#   - BUILD_LIBCVMFS=yes  (provides the cvmfs_client target)
#   - Xcode 16+ toolchain with Swift 6 and the macOS 15 SDK

cmake_minimum_required(VERSION 3.28)

# ---------------------------------------------------------------------------
# Language support
# ---------------------------------------------------------------------------
enable_language(Swift)
enable_language(OBJCXX)

set(CMAKE_OBJCXX_STANDARD 17)
set(CMAKE_SWIFT_LANGUAGE_VERSION 6)

# ---------------------------------------------------------------------------
# Source files
# ---------------------------------------------------------------------------
set(CVMFS_FSKIT_OBJCXX_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/CvmfsBridge.mm"
)

set(CVMFS_FSKIT_SWIFT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/CvmfsErrors.swift"
    "${CMAKE_CURRENT_SOURCE_DIR}/CvmfsItem.swift"
    "${CMAKE_CURRENT_SOURCE_DIR}/CvmfsFS.swift"
    "${CMAKE_CURRENT_SOURCE_DIR}/CvmfsExtension.swift"
)

# ---------------------------------------------------------------------------
# Extension bundle target
# ---------------------------------------------------------------------------
add_executable(CvmfsFSKitExtension
    ${CVMFS_FSKIT_OBJCXX_SOURCES}
    ${CVMFS_FSKIT_SWIFT_SOURCES}
)

# Mark it as a macOS app-extension bundle.
set_target_properties(CvmfsFSKitExtension PROPERTIES
    BUNDLE                     TRUE
    BUNDLE_EXTENSION           "appex"
    MACOSX_BUNDLE_INFO_PLIST   "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"

    # Swift ↔ ObjC++ bridging header.
    # SWIFT_OBJC_BRIDGING_HEADER is Xcode-generator-only; the equivalent for
    # Ninja / make is passed as a Swift compiler flag below.
    SWIFT_OBJC_BRIDGING_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/Bridging-Header.h"

    # Use the libcvmfs stub FUSE types instead of real FUSE headers
    # (mirrors the COMPILE_FLAGS set on the cvmfs_client target).
    COMPILE_FLAGS              "-DCVMFS_LIBCVMFS"

    # Bundle identifier must match the entitlements profile.
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "org.cvmfs.fskit.extension"

    # Hardened runtime is required for notarisation and for FSKit.
    XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME YES

    # Entitlements — must include com.apple.developer.fskit.fsmodule.
    XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS
        "${CMAKE_CURRENT_SOURCE_DIR}/CvmfsFSKitExtension.entitlements"

    # Deployment target: FSKit became a public API in macOS 15.4.
    XCODE_ATTRIBUTE_MACOSX_DEPLOYMENT_TARGET "15.4"
)

# ---------------------------------------------------------------------------
# Include paths
# ---------------------------------------------------------------------------
# The ObjC++ bridge needs C++ headers from the repo root.
target_include_directories(CvmfsFSKitExtension PRIVATE
    "${CMAKE_SOURCE_DIR}"           # #include "cvmfs/mountpoint.h" etc.
    "${CMAKE_SOURCE_DIR}/cvmfs"     # #include "mountpoint.h" etc.
    "${CMAKE_BINARY_DIR}"           # generated headers (e.g. cache.pb.h)
    "${CMAKE_BINARY_DIR}/cvmfs"
)

# Propagate include dirs from the cvmfs_client target (sqlite, curl, …).
target_include_directories(CvmfsFSKitExtension PRIVATE
    $<TARGET_PROPERTY:cvmfs_client,INCLUDE_DIRECTORIES>
)

# ---------------------------------------------------------------------------
# Compile definitions
# ---------------------------------------------------------------------------
target_compile_definitions(CvmfsFSKitExtension PRIVATE
    CVMFS_LIBCVMFS              # stub FUSE types in duplex_fuse.h
    _FILE_OFFSET_BITS=64
)

# ---------------------------------------------------------------------------
# Bridging header for non-Xcode generators (Ninja, make)
# SWIFT_OBJC_BRIDGING_HEADER is only honoured by the Xcode generator;
# for all other generators we pass the flag directly to swiftc.
# ---------------------------------------------------------------------------
if (NOT CMAKE_GENERATOR MATCHES "Xcode")
    target_compile_options(CvmfsFSKitExtension PRIVATE
        $<$<COMPILE_LANGUAGE:Swift>:
            -import-objc-header
            ${CMAKE_CURRENT_SOURCE_DIR}/Bridging-Header.h
        >
    )
endif ()

# ---------------------------------------------------------------------------
# Deployment target
# FSKit is annotated API_AVAILABLE(macos(15.4)) in the public SDK.
# Override whatever the root CMakeLists set for other targets.
# ---------------------------------------------------------------------------
set_target_properties(CvmfsFSKitExtension PROPERTIES
    OSX_DEPLOYMENT_TARGET "15.4"
)
# Also pass it as a compiler flag so both clang and swiftc see it.
target_compile_options(CvmfsFSKitExtension PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-mmacosx-version-min=15.4>
    $<$<COMPILE_LANGUAGE:Swift>:-target;arm64-apple-macosx15.4>
)

# ---------------------------------------------------------------------------
# Link libraries
# ---------------------------------------------------------------------------
# cvmfs_client provides the C++ core (MountPoint, FileSystem, Fetcher, …).
target_link_libraries(CvmfsFSKitExtension PRIVATE cvmfs_client)

# FSKit framework (macOS 15+).
find_library(FSKIT_LIB FSKit REQUIRED)
target_link_libraries(CvmfsFSKitExtension PRIVATE ${FSKIT_LIB})

# Standard system frameworks.
find_library(FOUNDATION_LIB  Foundation  REQUIRED)
find_library(SECURITY_LIB    Security    REQUIRED)
find_library(EXTENSIONFOUNDATION_LIB ExtensionFoundation REQUIRED)
target_link_libraries(CvmfsFSKitExtension PRIVATE
    ${FOUNDATION_LIB}
    ${SECURITY_LIB}
    ${EXTENSIONFOUNDATION_LIB}
)

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
# The .appex is installed into a standard location; it still needs to be
# embedded in a host .app bundle before it can be activated by the OS.
install(
    TARGETS     CvmfsFSKitExtension
    BUNDLE
    DESTINATION "${CMAKE_INSTALL_PREFIX}/libexec/cvmfs"
)
