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
# Deployment target — FSKit is annotated API_AVAILABLE(macos(15.4)).
# Set it per-target so the global CMAKE_OSX_DEPLOYMENT_TARGET for the rest of
# the build (e.g. cvmfs_client) is left at whatever the project needs.
# ---------------------------------------------------------------------------
# Detect the target architecture for the Swift -target triple.
if (CMAKE_OSX_ARCHITECTURES)
    list(GET CMAKE_OSX_ARCHITECTURES 0 _FSKIT_ARCH)
else ()
    execute_process(
        COMMAND uname -m
        OUTPUT_VARIABLE _FSKIT_ARCH
        OUTPUT_STRIP_TRAILING_WHITESPACE)
endif ()

target_compile_options(CvmfsFSKitExtension PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-mmacosx-version-min=15.4>
    $<$<COMPILE_LANGUAGE:Swift>:-target;${_FSKIT_ARCH}-apple-macos15.4>
)

# ---------------------------------------------------------------------------
# Link libraries
# ---------------------------------------------------------------------------
# cvmfs_client_fskit: static library with -fvisibility=default so all C++
# class symbols (FileSystem, MountPoint, …) are resolvable at link time.
# The global -fvisibility=hidden applied to the rest of the build would
# otherwise hide these symbols inside libcvmfs_client.dylib.
target_link_libraries(CvmfsFSKitExtension PRIVATE cvmfs_client_fskit)

# Runtime dependencies that the C++ core needs at runtime.
target_link_libraries(CvmfsFSKitExtension PRIVATE
    cvmfs_crypto            # libcvmfs_crypto.dylib (OpenSSL wrappers)
    cvmfs_util              # libcvmfs_util.dylib   (platform helpers)
    ${SQLITE3_LIBRARY}
    ${CURL_LIBRARIES}
    ${CARES_LIBRARIES}
    ${CARES_LDFLAGS}
    ${LEVELDB_LIBRARIES}
    ${PACPARSER_LIBRARIES}
    ${ZLIB_LIBRARIES}
    ${PROTOBUF_LITE_LIBRARY}
    ${OPENSSL_LIBRARIES}
)

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
# Host application — minimal macOS .app bundle that carries the extension
# ---------------------------------------------------------------------------
# macOS requires every filesystem extension (.appex) to live inside a regular
# application bundle (.app).  CvmfsHostApp is that container: a background-only
# app with no UI that is never launched interactively.  Its sole purpose is to
# give the OS a well-formed bundle from which pluginkit can discover and
# activate CvmfsFSKitExtension.
# ---------------------------------------------------------------------------
add_executable(CvmfsHostApp
    "${CMAKE_CURRENT_SOURCE_DIR}/app/main.swift"
)

set_target_properties(CvmfsHostApp PROPERTIES
    MACOSX_BUNDLE                TRUE
    OUTPUT_NAME                  "CVMFS"
    # Place the binary at the build-tree root so that the assembled CVMFS.app
    # ends up at <build_dir>/CVMFS.app — easy to find and sign.
    RUNTIME_OUTPUT_DIRECTORY     "${CMAKE_BINARY_DIR}"
    MACOSX_BUNDLE_INFO_PLIST     "${CMAKE_CURRENT_SOURCE_DIR}/app/CVMFS-Info.plist"

    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "org.cvmfs.host"
    XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS
        "${CMAKE_CURRENT_SOURCE_DIR}/app/CVMFS.entitlements"
    XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME   YES
    XCODE_ATTRIBUTE_MACOSX_DEPLOYMENT_TARGET  "15.4"
)

target_compile_options(CvmfsHostApp PRIVATE
    $<$<COMPILE_LANGUAGE:Swift>:-target;${_FSKIT_ARCH}-apple-macos15.4>
)

target_link_libraries(CvmfsHostApp PRIVATE
    ${FOUNDATION_LIB}
)

# ---------------------------------------------------------------------------
# Bundle directory assembly for non-Xcode generators (Ninja, make)
# ---------------------------------------------------------------------------
# The Xcode generator creates .appex / .app bundle directories automatically
# during the build.  Ninja and make build flat binaries only; we wrap them in
# the expected bundle layout with explicit post-build steps.
# ---------------------------------------------------------------------------
if (NOT CMAKE_GENERATOR MATCHES "Xcode")

    # Paths used by both the post-build steps and the cvmfs_app target below.
    set(_APPEX_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/CvmfsFSKitExtension.appex")
    set(_APP_BUILD_DIR   "${CMAKE_BINARY_DIR}/CVMFS.app")

    # --- CvmfsFSKitExtension.appex ---
    add_custom_command(TARGET CvmfsFSKitExtension POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${_APPEX_BUILD_DIR}/Contents/MacOS"
        COMMAND ${CMAKE_COMMAND} -E copy
            "$<TARGET_FILE:CvmfsFSKitExtension>"
            "${_APPEX_BUILD_DIR}/Contents/MacOS/$<TARGET_FILE_NAME:CvmfsFSKitExtension>"
        COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"
            "${_APPEX_BUILD_DIR}/Contents/Info.plist"
        COMMENT "Assembling CvmfsFSKitExtension.appex"
        VERBATIM
    )

    # --- CVMFS.app (host binary + Info.plist only; extension added by cvmfs_app) ---
    add_custom_command(TARGET CvmfsHostApp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${_APP_BUILD_DIR}/Contents/MacOS"
        COMMAND ${CMAKE_COMMAND} -E copy
            "$<TARGET_FILE:CvmfsHostApp>"
            "${_APP_BUILD_DIR}/Contents/MacOS/$<TARGET_FILE_NAME:CvmfsHostApp>"
        COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/app/CVMFS-Info.plist"
            "${_APP_BUILD_DIR}/Contents/Info.plist"
        COMMENT "Assembling CVMFS.app host bundle"
        VERBATIM
    )

else ()

    # Under Xcode, TARGET_BUNDLE_DIR gives the real bundle directory.
    set(_APPEX_BUILD_DIR "$<TARGET_BUNDLE_DIR:CvmfsFSKitExtension>")
    set(_APP_BUILD_DIR   "$<TARGET_BUNDLE_DIR:CvmfsHostApp>")

endif ()

# ---------------------------------------------------------------------------
# cvmfs_app — embed the extension into CVMFS.app
# ---------------------------------------------------------------------------
# Building this target produces a self-contained CVMFS.app in the build tree
# that is ready to be code-signed and registered with pluginkit.
#
#   ninja cvmfs_app
#
# The assembled bundle lives at:
#   <build_dir>/CVMFS.app/Contents/
#     Info.plist
#     MacOS/CVMFS
#     PlugIns/CvmfsFSKitExtension.appex/
#       Contents/
#         Info.plist
#         MacOS/CvmfsFSKitExtension
# ---------------------------------------------------------------------------
add_custom_target(cvmfs_app
    DEPENDS CvmfsFSKitExtension CvmfsHostApp
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${_APP_BUILD_DIR}/Contents/PlugIns"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${_APPEX_BUILD_DIR}"
        "${_APP_BUILD_DIR}/Contents/PlugIns/CvmfsFSKitExtension.appex"
    COMMENT "Embedding CvmfsFSKitExtension.appex into CVMFS.app"
    VERBATIM
)

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
# CVMFS_APP_INSTALL_DIR: parent directory for CVMFS.app.  Defaults to
# /Applications regardless of CMAKE_INSTALL_PREFIX because macOS app bundles
# must be in a well-known, user-accessible location for pluginkit to discover
# and register the extension.
# ---------------------------------------------------------------------------
set(CVMFS_APP_INSTALL_DIR "/Applications" CACHE PATH
    "Directory where CVMFS.app is installed (default: /Applications)")

set(_INST_APP    "${CVMFS_APP_INSTALL_DIR}/CVMFS.app")
set(_INST_APPEX  "${_INST_APP}/Contents/PlugIns/CvmfsFSKitExtension.appex")

# Host app binary.
# RUNTIME destination: used by Ninja/make (flat binary).
# BUNDLE  destination: used by Xcode (whole .app directory).
install(TARGETS CvmfsHostApp
    RUNTIME DESTINATION "${_INST_APP}/Contents/MacOS"
    BUNDLE  DESTINATION "${CVMFS_APP_INSTALL_DIR}"
)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/app/CVMFS-Info.plist"
    DESTINATION "${_INST_APP}/Contents"
    RENAME Info.plist
)

# Extension binary (embedded inside the host app's PlugIns directory).
install(TARGETS CvmfsFSKitExtension
    RUNTIME DESTINATION "${_INST_APPEX}/Contents/MacOS"
    BUNDLE  DESTINATION "${_INST_APP}/Contents/PlugIns"
)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"
    DESTINATION "${_INST_APPEX}/Contents"
    RENAME Info.plist
)

# Post-install instructions printed at the end of `ninja install`.
install(CODE "
message(STATUS \"\")
message(STATUS \"╔══════════════════════════════════════════════════════════════╗\")
message(STATUS \"║  CVMFS.app installed to ${CVMFS_APP_INSTALL_DIR}            \")
message(STATUS \"╚══════════════════════════════════════════════════════════════╝\")
message(STATUS \"\")
message(STATUS \"Next steps:\")
message(STATUS \"\")
message(STATUS \"1. Code-sign both bundles (requires Developer ID + fskit entitlement):\")
message(STATUS \"\")
message(STATUS \"   codesign --force --sign 'Developer ID Application: Your Name (TEAMID)' \\\\\" )
message(STATUS \"     --entitlements ${CMAKE_CURRENT_SOURCE_DIR}/CvmfsFSKitExtension.entitlements \\\\\" )
message(STATUS \"     --options runtime \\\\\" )
message(STATUS \"     '${_INST_APPEX}'\")
message(STATUS \"\")
message(STATUS \"   codesign --force --sign 'Developer ID Application: Your Name (TEAMID)' \\\\\" )
message(STATUS \"     --entitlements ${CMAKE_CURRENT_SOURCE_DIR}/app/CVMFS.entitlements \\\\\" )
message(STATUS \"     --options runtime \\\\\" )
message(STATUS \"     '${_INST_APP}'\")
message(STATUS \"\")
message(STATUS \"2. Register with pluginkit (run as the target user, not root):\")
message(STATUS \"   pluginkit -a '${_INST_APP}'\")
message(STATUS \"\")
message(STATUS \"3. Approve in System Settings → Privacy & Security → Extensions\")
message(STATUS \"\")
message(STATUS \"4. Activate the cvmfs filesystem personality:\")
message(STATUS \"   pluginkit -e use -i org.cvmfs.fskit.extension\")
message(STATUS \"\")
message(STATUS \"5. Mount a repository:\")
message(STATUS \"   sudo mount -t cvmfs atlas.cern.ch /cvmfs/atlas.cern.ch\")
message(STATUS \"\")
")
