#!/bin/bash
#
# validate_tarball_roundtrip.sh - Round-trip a container image through CVMFS and
# back to tarballs via "cvmfs_server create-tarball".
#
# Motivation: CVMFS stores container layers in a heavily deduplicated, compressed
# content-addressable object store. Serving a layer as a tarball straight from
# that store (instead of re-reading a flat rootfs through FUSE) is the operation
# a CVMFS-backed container registry would perform on every blob request, so it
# must be both correct and fast.
#
# This script:
#   1. Unpacks an image into CVMFS with cvmfs_ducc (flat image + per-layer trees).
#   2. Exports the flat image and every layer back to a tarball with
#      "cvmfs_server create-tarball" (HTTP path) and "cvmfs_swissknife
#      create-tarball -r <local store>" (local CAS path).
#   3. Validates each exported tarball against a reference tar produced by
#      reading the same subtree through the FUSE mount (tree + content).
#   4. Reports throughput for create-tarball vs. the plain-FUSE-tar baseline.
#
# Usage: validate_tarball_roundtrip.sh [options] <image_ref> [cvmfs_repo]
#
#   image_ref   Container image to convert. Either a registry URL, e.g.
#                 https://registry.hub.docker.com/library/alpine:latest
#               or a local source (avoids re-pulling; pull/save once, test often):
#                 docker-archive:/path/to/image.tar   (from `docker save`)
#                 oci:/path/to/oci-layout-dir
#   cvmfs_repo  CVMFS repository name (default: test-roundtrip.cern.ch)
#
# Options:
#   -c  Compare file contents (sha256 checksums), not just tree structure
#   -L  Also export and validate every individual layer (default: flat image only)
#   -k  Keep temporary files and the CVMFS repo after completion
#   -v  Verbose: print diffs to stdout
#   -h  Show this help message

set -euo pipefail

WORK_DIR=""
KEEP_TEMP=0
VERBOSE=0
COMPARE_CONTENT=0
DO_LAYERS=0
REPO_CREATED=0
CVMFS_REPO=""

# ── helpers ──────────────────────────────────────────────────────────────────

die()      { echo "[ERROR] $*" >&2; exit 1; }
log_info() { echo "[INFO]  $*"; }
log_dbg()  { [ "$VERBOSE" -eq 1 ] && echo "[DEBUG] $*" || true; }

usage() {
    sed -n '2,/^$/{ s/^# \?//; p }' "$0"
    exit 1
}

cleanup() {
    local rc=$?
    if [ "$KEEP_TEMP" -eq 0 ]; then
        [ -n "$WORK_DIR" ] && [ -d "$WORK_DIR" ] && sudo rm -rf "$WORK_DIR"
        if [ "$REPO_CREATED" -eq 1 ] && [ -n "$CVMFS_REPO" ]; then
            log_info "Removing CVMFS repository $CVMFS_REPO"
            sudo cvmfs_server rmfs -f "$CVMFS_REPO" >/dev/null 2>&1 || true
        fi
    fi
    exit $rc
}

# Seconds (float) elapsed running "$@", captured into global REPLY_SECS.
timeit() {
    local t0 t1
    t0=$(date +%s.%N)
    "$@"
    t1=$(date +%s.%N)
    REPLY_SECS=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.3f", b - a }')
}

# https://registry.hub.docker.com/library/alpine:latest  →  registry.hub.docker.com/library/alpine:latest
url_to_cvmfs_image_path() { echo "${1#*://}"; }

# /cvmfs/<repo>/foo/bar  →  foo/bar
to_subpath() { echo "${1#/cvmfs/$CVMFS_REPO/}"; }

# Deterministic listing: TYPE PERMS UID GID SIZE PATH [-> TARGET].
# Excludes CVMFS bookkeeping files, which create-tarball also drops.
generate_listing() {
    local dir="$1" output="$2"
    ( cd "$dir"
      find . \( -name '.cvmfscatalog' -o -name '.cvmfsdirtab' \
                -o -name '.cvmfsautocatalog' \) -prune -o -print0 \
        | sort -z | while IFS= read -r -d '' e; do
            # Skip the export root itself: create-tarball lays a directory
            # subtree's *contents* at the top level (container-layer layout) and
            # does not emit a "./" member, so the root entry here is just the
            # extraction directory and must not be compared.
            [ "$e" = "." ] && continue
            if   [ -L "$e" ]; then
                printf 'L %s %s -> %s\n' \
                    "$(stat -c '%a %u %g' "$e")" "$e" "$(readlink "$e")"
            elif [ -d "$e" ]; then
                printf 'D %s %s\n' "$(stat -c '%a %u %g' "$e")" "$e"
            elif [ -f "$e" ]; then
                printf 'F %s %s %s\n' "$(stat -c '%a %u %g' "$e")" \
                    "$(stat -c '%s' "$e")" "$e"
            else
                # devices / fifos: record type + rdev so they are compared too
                printf '%s %s\n' "$(stat -c '%F %a %u %g %t:%T' "$e")" "$e"
            fi
        done
    ) > "$output"
}

generate_checksums() {
    local dir="$1" output="$2"
    # Read under sudo: the trees are extracted with `sudo tar`, so members may be
    # root-owned and (for some images) not world-readable.
    sudo sh -c "cd '$dir' && find . -type f \
             ! -name '.cvmfscatalog' ! -name '.cvmfsdirtab' \
             ! -name '.cvmfsautocatalog' -print0 \
        | sort -z | xargs -0 -r sha256sum" > "$output"
}

# Validate one CVMFS subtree against its create-tarball export.
#   $1 label, $2 absolute /cvmfs path of the subtree
# Produces a reference tar via FUSE, an export via create-tarball (HTTP) and via
# swissknife (local CAS), compares all three, and records timing.
HAS_DIFF=0
TIMING_ROWS=()
validate_subtree() {
    local label="$1" cvmfs_path="$2"
    local subpath; subpath="/$(to_subpath "$cvmfs_path")"
    local d="$WORK_DIR/$label"
    mkdir -p "$d"

    log_info "── $label : $subpath"

    # bytes / file count of the source subtree (for throughput numbers)
    local bytes nfiles
    bytes=$(sudo du -sb --apparent-size "$cvmfs_path" 2>/dev/null | cut -f1)
    nfiles=$(sudo find "$cvmfs_path" -type f | wc -l)

    # reference tarball: read the subtree through the FUSE mount
    timeit sudo tar -C "$cvmfs_path" \
        --exclude='.cvmfscatalog' --exclude='.cvmfsautocatalog' \
        --exclude='.cvmfsdirtab' --numeric-owner -cf "$d/reference.tar" .
    local t_fuse="$REPLY_SECS"

    # export via the server wrapper (drives swissknife against CVMFS_STRATUM0,
    # i.e. the HTTP fetch path against the local Apache)
    timeit sudo cvmfs_server create-tarball -p "$subpath" -o "$d/http.tar" \
        "$CVMFS_REPO"
    local t_http="$REPLY_SECS"

    # export directly from the local backend store (LocalPayloadFetcher: decom-
    # press straight from the CAS, no HTTP round-trip)
    local store="/srv/cvmfs/$CVMFS_REPO"
    timeit sudo cvmfs_swissknife create-tarball -r "$store" -p "$subpath" \
        -o "$d/local.tar" -l "$WORK_DIR"
    local t_local="$REPLY_SECS"

    # throughput (MB/s) on the apparent uncompressed size
    local mbps_http mbps_local mbps_fuse
    mbps_http=$(awk -v b="$bytes" -v s="$t_http"  'BEGIN{printf "%.1f", (s>0)?b/1048576/s:0}')
    mbps_local=$(awk -v b="$bytes" -v s="$t_local" 'BEGIN{printf "%.1f", (s>0)?b/1048576/s:0}')
    mbps_fuse=$(awk -v b="$bytes" -v s="$t_fuse"  'BEGIN{printf "%.1f", (s>0)?b/1048576/s:0}')
    TIMING_ROWS+=("$(printf '%-22s %8s %6s %9s %9s %9s %8s %8s %8s' \
        "$label" "$(awk -v b="$bytes" 'BEGIN{printf "%.1fMB", b/1048576}')" \
        "$nfiles" "$t_fuse" "$t_http" "$t_local" \
        "$mbps_fuse" "$mbps_http" "$mbps_local")")

    # listing equality (HTTP and local must be byte-identical to each other)
    if ! cmp -s "$d/http.tar" "$d/local.tar"; then
        # tar byte-equality can legitimately differ (pax headers ordering); fall
        # back to comparing extracted trees below, but flag for visibility
        log_dbg "$label: http.tar and local.tar differ at byte level (checked via extraction)"
    fi

    # extract reference + export, compare the trees
    local ref_dir="$d/ref" ct_dir="$d/ct"
    mkdir -p "$ref_dir" "$ct_dir"
    sudo tar -C "$ref_dir" -xf "$d/reference.tar"
    sudo tar -C "$ct_dir"  -xf "$d/http.tar"

    generate_listing "$ref_dir" "$d/ref.list"
    generate_listing "$ct_dir"  "$d/ct.list"
    if diff -u "$d/ref.list" "$d/ct.list" > "$d/list.diff" 2>&1; then
        log_info "   tree structure : PASS ($nfiles files, $(awk -v b="$bytes" 'BEGIN{printf "%.1f MB", b/1048576}'))"
    else
        HAS_DIFF=1
        log_info "   tree structure : FAIL"
        [ "$VERBOSE" -eq 1 ] && head -40 "$d/list.diff"
    fi

    if [ "$COMPARE_CONTENT" -eq 1 ]; then
        generate_checksums "$ref_dir" "$d/ref.sums"
        generate_checksums "$ct_dir"  "$d/ct.sums"
        if diff -u "$d/ref.sums" "$d/ct.sums" > "$d/sums.diff" 2>&1; then
            log_info "   file contents  : PASS"
        else
            HAS_DIFF=1
            log_info "   file contents  : FAIL"
            [ "$VERBOSE" -eq 1 ] && head -40 "$d/sums.diff"
        fi
    fi

    # also validate the locally-exported tar's tree (catches local-vs-http drift)
    local lc_dir="$d/lc"
    mkdir -p "$lc_dir"
    sudo tar -C "$lc_dir" -xf "$d/local.tar"
    generate_listing "$lc_dir" "$d/lc.list"
    if diff -q "$d/ct.list" "$d/lc.list" >/dev/null 2>&1; then
        log_info "   local == http  : PASS"
    else
        HAS_DIFF=1
        log_info "   local == http  : FAIL"
        [ "$VERBOSE" -eq 1 ] && diff -u "$d/ct.list" "$d/lc.list" | head -40
    fi
}

# ── argument parsing ─────────────────────────────────────────────────────────

while getopts "cLkvh" opt; do
    case $opt in
        c) COMPARE_CONTENT=1 ;;
        L) DO_LAYERS=1 ;;
        k) KEEP_TEMP=1 ;;
        v) VERBOSE=1 ;;
        h) usage ;;
        *) usage ;;
    esac
done
shift $((OPTIND - 1))

IMAGE_URL="${1:-}"
CVMFS_REPO="${2:-test-roundtrip.cern.ch}"
[ -z "$IMAGE_URL" ] && die "Missing required argument: image_url"

command -v cvmfs_ducc       >/dev/null 2>&1 || die "cvmfs_ducc not found in PATH"
command -v cvmfs_server     >/dev/null 2>&1 || die "cvmfs_server not found in PATH"
command -v cvmfs_swissknife >/dev/null 2>&1 || die "cvmfs_swissknife not found in PATH"

trap cleanup EXIT HUP INT TERM

WORK_DIR=$(mktemp -d /tmp/validate_tarball_roundtrip.XXXXXX)
chmod 0777 "$WORK_DIR"
# A local source (docker-archive:/oci:) lets us pull an image once and convert it
# repeatedly without hitting registry rate limits.
case "$IMAGE_URL" in
    docker-archive:*|oci:*|oci-archive:*) IS_LOCAL=1 ;;
    *) IS_LOCAL=0; CVMFS_IMAGE_PATH=$(url_to_cvmfs_image_path "$IMAGE_URL") ;;
esac

log_info "Image URL  : $IMAGE_URL"
log_info "CVMFS repo : $CVMFS_REPO"
log_info "Work dir   : $WORK_DIR"

# ── Step 1: unpack with cvmfs_ducc ──────────────────────────────────────────

log_info "=== Step 1: Unpacking with cvmfs_ducc ==="
if cvmfs_server list 2>/dev/null | grep -q "^${CVMFS_REPO} "; then
    log_info "Reusing existing CVMFS repository $CVMFS_REPO"
else
    log_info "Creating CVMFS repository $CVMFS_REPO"
    sudo cvmfs_server mkfs -o "$USER" "$CVMFS_REPO" \
        || die "Failed to create CVMFS repository"
    REPO_CREATED=1
fi

# -p skips the podman store (not needed here); layers are unpacked regardless.
cvmfs_ducc convert-single-image -p "$IMAGE_URL" "$CVMFS_REPO" \
    2>&1 | tee "$WORK_DIR/ducc.log" \
    || die "cvmfs_ducc conversion failed (see $WORK_DIR/ducc.log)"

if [ "$IS_LOCAL" -eq 1 ]; then
    # Local images carry a synthetic name; locate the flat image via .flat/.
    FLAT_DIR=$(sudo find "/cvmfs/$CVMFS_REPO/.flat" -mindepth 2 -maxdepth 2 \
        -type d 2>/dev/null | sort | tail -1)
    [ -n "$FLAT_DIR" ] || die "No flat image found under /cvmfs/$CVMFS_REPO/.flat"
else
    FLAT_DIR="/cvmfs/$CVMFS_REPO/$CVMFS_IMAGE_PATH"
    [ -e "$FLAT_DIR" ] || die "Expected flat image not found at $FLAT_DIR"
    FLAT_DIR=$(readlink -f "$FLAT_DIR")
fi
log_info "Flat image resolved to: $FLAT_DIR"

# ── Step 2: round-trip flat image (and optionally each layer) ────────────────

log_info "=== Step 2: Exporting and validating ==="
validate_subtree "flat-image" "$FLAT_DIR"

if [ "$DO_LAYERS" -eq 1 ]; then
    mapfile -t LAYER_DIRS < <(sudo find "/cvmfs/$CVMFS_REPO/.layers" \
        -mindepth 3 -maxdepth 3 -name layerfs -type d 2>/dev/null | sort)
    log_info "Found ${#LAYER_DIRS[@]} layer(s)"
    local_i=0
    for ld in "${LAYER_DIRS[@]}"; do
        validate_subtree "layer-$(printf '%02d' "$local_i")" "$ld"
        local_i=$((local_i + 1))
    done
fi

# ── Summary ──────────────────────────────────────────────────────────────────

echo ""
log_info "=== Timing (seconds; throughput MB/s on apparent size) ==="
printf '%-22s %8s %6s %9s %9s %9s %8s %8s %8s\n' \
    "subtree" "size" "files" "fuse-tar" "ct-http" "ct-local" \
    "fuse" "http" "local"
for row in "${TIMING_ROWS[@]}"; do echo "$row"; done

echo ""
if [ "$HAS_DIFF" -eq 0 ]; then
    log_info "RESULT: PASS — create-tarball round-trips the image faithfully"
    exit 0
else
    log_info "RESULT: FAIL — differences detected (see $WORK_DIR, re-run with -kv)"
    KEEP_TEMP=1
    exit 1
fi
