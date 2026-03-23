# CernVM-FS — Native macOS FSKit Backend

This directory contains a native macOS filesystem extension that replaces the
FUSE-based `cvmfs.cc` module with Apple's
[FSKit](https://developer.apple.com/documentation/fskit) API, available since
macOS 15 (Sequoia).

The C++ core (catalog manager, fetcher, cache manager, inode tracker, …) is
reused unchanged via an Objective-C++ bridge (`CvmfsBridge.mm`). Only the
VFS-facing layer is rewritten in Swift.

---

## Contents

```
cvmfs/fskit/
  CvmfsBridge.h          ObjC++ header (the only C++ boundary visible to Swift)
  CvmfsBridge.mm         ObjC++ implementation wrapping MountPoint / FileSystem
  CvmfsErrors.swift      POSIX errno → FSKit Error helpers
  CvmfsItem.swift        FSItem subclass (per-inode state)
  CvmfsFS.swift          FSVolume subclass + all protocol conformances
  CvmfsExtension.swift   Extension entry point (UnaryFileSystemExtension)
  Bridging-Header.h      Imports CvmfsBridge.h for the Swift compiler
  CMakeLists.txt         Build description for the .appex bundle
  README.md              This file
```

---

## Requirements

| Component | Version |
|-----------|---------|
| macOS | **15.0 (Sequoia)** or later |
| Xcode | **16.0** or later (Swift 6, macOS 15 SDK) |
| CMake | 3.28 or later |
| Apple Developer account | Required for code-signing the extension |

Dependencies pulled from the CernVM-FS bundled externals (built automatically
by the parent CMake build): SQLite, libcurl, c-ares, LevelDB, pacparser, zlib,
OpenSSL, protobuf.

---

## Building

### 1. Build the CernVM-FS C++ core as `cvmfs_client`

The FSKit extension links against `libcvmfs_client`. Both can be built in a
single CMake invocation (see step 2), but if you want to build the library
alone first:

```bash
mkdir -p build && cd build

cmake .. \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_CVMFS=no \
  -DBUILD_SERVER=no \
  -DBUILD_LIBCVMFS=yes

make -j$(sysctl -n hw.logicalcpu) cvmfs_client cvmfs_util cvmfs_crypto
```

The resulting library is `build/cvmfs/libcvmfs_client.dylib`.

### 2. Build the FSKit extension bundle

Pass `-DBUILD_FSKIT=yes` alongside `-DBUILD_LIBCVMFS=yes`.
**Always use a fresh build directory** (or delete `CMakeCache.txt`) when
switching from a FUSE build to an FSKit build; a cached `BUILD_CVMFS=ON` is
the most common source of failures.

```bash
mkdir -p build-fskit && cd build-fskit

cmake .. \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_SERVER=no \
  -DBUILD_LIBCVMFS=yes \
  -DBUILD_FSKIT=yes

make -j$(sysctl -n hw.logicalcpu) CvmfsFSKitExtension
```

The CMake configure step automatically:
- Sets `CMAKE_OSX_DEPLOYMENT_TARGET=15.0` if not already set
- Forces `BUILD_CVMFS=OFF` with a `STATUS` message if it was cached from a
  previous run (FSKit and the FUSE module do not share a build tree)

CMake will error out with a clear message if:
- Not building on macOS (`BUILD_FSKIT is only supported on macOS`)
- `BUILD_LIBCVMFS` is not also set (`BUILD_FSKIT requires BUILD_LIBCVMFS=yes`)
- `CMAKE_OSX_DEPLOYMENT_TARGET` is explicitly set below 15.0

The build produces:

```
build/cvmfs/fskit/CvmfsFSKitExtension.appex
```

> **Xcode alternative**: open `FSKitSample/FSKitExp.xcodeproj` (the reference
> sample project) and add the `CvmfsFSKitExtension` target manually, or
> open/create a thin host application project that embeds the extension.
> FSKit extensions **must** be distributed inside a host application bundle.

### 3. Code-sign the extension

FSKit filesystem extensions require the
`com.apple.developer.fskit.fsmodule` entitlement, which is gated behind a
provisioning profile. Request access via
[Apple's developer portal](https://developer.apple.com/contact/request/filesystem-extensions/).

Create `CvmfsFSKitExtension.entitlements`:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>com.apple.security.app-sandbox</key>
  <true/>
  <key>com.apple.developer.fskit.fsmodule</key>
  <true/>
</dict>
</plist>
```

Sign:

```bash
codesign --force --sign "Developer ID Application: Your Name (TEAMID)" \
  --entitlements CvmfsFSKitExtension.entitlements \
  --options runtime \
  build/cvmfs/fskit/CvmfsFSKitExtension.appex
```

---

## Installing

### 1. Wrap the extension in a host application

macOS requires that filesystem extensions live inside a regular `.app` bundle.
Create a minimal host app `CvmfsHost.app`:

```bash
APP=CvmfsHost.app
mkdir -p "$APP/Contents/MacOS"
mkdir -p "$APP/Contents/PlugIns"

# Copy the extension
cp -r build/cvmfs/fskit/CvmfsFSKitExtension.appex \
      "$APP/Contents/PlugIns/"

# Minimal Info.plist for the host app
cat > "$APP/Contents/Info.plist" << 'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleIdentifier</key>
  <string>org.cvmfs.host</string>
  <key>CFBundleExecutable</key>
  <string>CvmfsHost</string>
  <key>CFBundleName</key>
  <string>CvmfsHost</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
  <key>NSExtension</key>
  <dict>
    <key>NSExtensionPointIdentifier</key>
    <string>com.apple.fskit.fsmodule</string>
  </dict>
</dict>
</plist>
EOF

# Minimal stub executable (the host app itself is never launched by users)
printf '#!/bin/sh\nexec true\n' > "$APP/Contents/MacOS/CvmfsHost"
chmod +x "$APP/Contents/MacOS/CvmfsHost"

# Sign the host app
codesign --force --sign "Developer ID Application: Your Name (TEAMID)" \
  --options runtime \
  "$APP"
```

### 2. Install and activate the extension

```bash
# Install the host app somewhere permanent
sudo cp -r CvmfsHost.app /Applications/

# Register the extension with the system extension daemon
# (requires user approval in System Settings on first run)
pluginkit -a /Applications/CvmfsHost.app

# Activate the filesystem personality (makes it available to mount)
pluginkit -e use -i org.cvmfs.fskit.extension
```

On the first activation macOS will show a prompt in
**System Settings → Privacy & Security → System Extensions** asking the user
to approve the extension. Click **Allow**.

### 3. Install CernVM-FS configuration

The FSKit backend uses the same configuration files as the FUSE client.
Install the standard CernVM-FS configuration package or set up manually:

```bash
# Option A: install the cvmfs-config-default package (recommended)
# This provides /etc/cvmfs/domain.d/ and /etc/cvmfs/keys/

# Option B: manual minimal setup
sudo mkdir -p /etc/cvmfs/config.d /etc/cvmfs/domain.d /etc/cvmfs/keys

# Create /etc/cvmfs/default.local with at least:
cat | sudo tee /etc/cvmfs/default.local << 'EOF'
CVMFS_HTTP_PROXY=DIRECT
CVMFS_QUOTA_LIMIT=10000
EOF
```

---

## Mounting

### Manual mount with `mount_fskit`

macOS ships a `mount_fskit` helper that drives FSKit-based filesystems:

```bash
# Create a mountpoint
mkdir -p /cvmfs/atlas.cern.ch

# Mount using the registered cvmfs personality
sudo mount_fskit \
  -t cvmfs \
  -o config=/etc/cvmfs/config.d/atlas.cern.ch.conf \
  cvmfs://atlas.cern.ch \
  /cvmfs/atlas.cern.ch
```

The URL format is `cvmfs://<fqrn>` where `<fqrn>` is the
fully-qualified repository name. The `-o config=<path>` option points to the
repository configuration file; if omitted, the default path
`/etc/cvmfs/repositories.d/<fqrn>/client.conf` is tried, followed by
`/etc/cvmfs/config.d/<fqrn>.conf`.

### Mount options

Options are passed with `-o key=value`:

| Option | Default | Description |
|--------|---------|-------------|
| `config=<path>` | see above | Path to the repository `.conf` file |
| `fqrn=<name>` | derived from URL host | Explicit repository FQRN |

All other CVMFS tuning parameters (`CVMFS_HTTP_PROXY`,
`CVMFS_QUOTA_LIMIT`, `CVMFS_SERVER_URL`, …) are read from the
standard CernVM-FS configuration files — the same files used by the FUSE
backend.

### Unmounting

```bash
sudo umount /cvmfs/atlas.cern.ch
# or
diskutil unmount /cvmfs/atlas.cern.ch
```

### Auto-mount with `autofs`

Add an indirect map entry to `/etc/auto_master`:

```
/cvmfs  auto_cvmfs  --timeout=600
```

Create `/etc/auto_cvmfs`:

```
*  -fstype=cvmfs,config=/etc/cvmfs/config.d/&.conf  cvmfs://&
```

Then reload:

```bash
sudo automount -vc
```

---

## Differences from the FUSE backend

The FSKit backend is functionally equivalent for normal read-only use but
intentionally omits several features present in the FUSE module:

| Feature | FUSE | FSKit |
|---------|------|-------|
| VOMS / authz session | ✅ | ❌ dropped — FSKit gives no caller PID |
| Per-request cancellation | ✅ (`fuse_req_interrupted`) | ❌ dropped (trivial to add via `Task.isCancelled`) |
| Per-entry kernel TTL | ✅ (`attr_timeout`, `entry_timeout`) | ❌ no FSKit equivalent |
| Proactive dentry invalidation | ✅ (`fuse_expire_entry`) | ❌ no FSKit equivalent |
| Chunked / multi-file reads | ✅ | ✅ |
| Magic extended attributes | ✅ | ✅ |
| Catalog TTL / hot remount | ✅ | ✅ (via `FuseRemounter::TryFinish`) |
| Talk socket admin interface | ✅ | ✅ |
| NFS map source | ✅ | ❌ dropped |
| FUSE passthrough (zero-copy) | Linux only | ❌ no FSKit equivalent |
| Hot-reload / `SaveState` | ✅ | ❌ not applicable |

The absence of per-entry TTL means stale catalog entries may linger in the
kernel's VFS cache longer than on Linux. A catalog update will still be picked
up on the next `TryFinish` cycle; only the kernel's view of already-cached
entries may lag behind.

---

## Troubleshooting

### Build fails with `fuse/fuse_lowlevel.h file not found`

This happens when `BUILD_CVMFS=ON` is still set in the CMake cache from a
previous FUSE build. The FSKit build does not need MacFUSE or fuse-t.

**Fix**: use a fresh build directory:

```bash
mkdir -p build-fskit && cd build-fskit
cmake .. -DBUILD_LIBCVMFS=yes -DBUILD_FSKIT=yes
```

If you must reuse an existing build directory, delete the cache first:

```bash
rm CMakeCache.txt && cmake .. -DBUILD_LIBCVMFS=yes -DBUILD_FSKIT=yes
```

CMake will print `BUILD_FSKIT: forcing BUILD_CVMFS=OFF` if it detected and
corrected a stale cached value.

### Extension not listed by `pluginkit`

```bash
pluginkit -m -i org.cvmfs.fskit.extension
```

If the extension is not found, verify the `Info.plist` inside the `.appex`
bundle contains the correct `EXExtensionPointIdentifier`:

```
com.apple.fskit.fsmodule
```

### Mount fails with "Operation not permitted"

The extension entitlement profile may not be activated. Check:

```bash
# System extension status
systemextensionsctl list

# FSKit extension status
pluginkit -m -i org.cvmfs.fskit.extension -A
```

Approve the extension in **System Settings → Privacy & Security →
System Extensions** if it shows as "waiting for user approval".

### Repository configuration not found

The bridge logs to the system log. Inspect with:

```bash
log stream --predicate 'subsystem == "org.cvmfs.fskit"' --level debug
```

Common issues:
- Missing `CVMFS_SERVER_URL` — add to `/etc/cvmfs/config.d/<fqrn>.conf`
- Missing public key — copy `.pub` file to `/etc/cvmfs/keys/<domain>/`
- Proxy not reachable — set `CVMFS_HTTP_PROXY=DIRECT` to skip proxy

### Checking mounted volumes

```bash
mount | grep cvmfs
# or
diskutil list | grep cvmfs
```

### Verbose logging

Set the CVMFS log level in the config file:

```bash
# /etc/cvmfs/default.local
CVMFS_DEBUGLOG=/tmp/cvmfs_fskit.log
```

The FSKit Swift layer logs to the unified system log under subsystem
`org.cvmfs.fskit`, category `CvmfsFS`:

```bash
log stream \
  --predicate 'subsystem == "org.cvmfs.fskit"' \
  --level debug
```

---

## Development notes

### Repository layout

```
cvmfs/fskit/
  CvmfsBridge.h / .mm    C++↔Swift bridge (extend here for new C++ features)
  CvmfsFS.swift          FSKit callbacks (one method per VFS operation)
  CvmfsItem.swift        Per-inode state (add open-file metadata here)
  CvmfsExtension.swift   Entry point + mount-param parsing
```

### Adding a new FSKit capability

1. Declare any new C++ API surface in `CvmfsBridge.h` (ObjC types only).
2. Implement it in `CvmfsBridge.mm`.
3. Conform `CvmfsFS` to the new `FSVolume.*Operations` protocol in
   `CvmfsFS.swift`.

### Running the test suite

The existing C++ unit tests (`make unitests`) continue to work unchanged; they
do not depend on the FSKit layer.

To test the FSKit layer manually:

```bash
# After mounting atlas.cern.ch:
ls   /cvmfs/atlas.cern.ch
cat  /cvmfs/atlas.cern.ch/CVMFS_TEST_FILE 2>/dev/null || echo "(empty repo)"
xattr -l /cvmfs/atlas.cern.ch   # should list user.fqrn, user.revision, …
stat /cvmfs/atlas.cern.ch
```

### Catalog TTL and cache refresh

The catalog TTL is controlled by `CVMFS_MAX_TTL` in the repository config.
The FSKit backend runs the same `FuseRemounter` background thread as the FUSE
module; catalogs are refreshed on the next VFS call after the TTL expires.
Because FSKit does not expose `fuse_expire_entry`, stale kernel-cached dentries
are not actively invalidated — they will time out or be evicted by normal VFS
pressure.
