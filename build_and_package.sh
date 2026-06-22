#!/bin/bash -e

# Configuration
BUILD_DIR="${BUILD_DIR:-/dev/shm/qemu-build}"
STAGING_DIR="${STAGING_DIR:-/dev/shm/qemu-staging}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST_DIR="${DIST_DIR:-$SCRIPT_DIR/dist}"
OUT="${DIST_DIR}/wasabi-qemu-x86_64-linux"

echo "=== Environment ==="
echo "BUILD_DIR (tmpfs):  $BUILD_DIR"
echo "STAGING_DIR:       $STAGING_DIR"
echo "OUT:               $OUT"

# 1) Clean/Create build and staging directories
rm -rf "$BUILD_DIR" "$STAGING_DIR"
mkdir -p "$BUILD_DIR"
mkdir -p "$STAGING_DIR"

# 2) Configure
echo "=== Configuring QEMU ==="
cd "$BUILD_DIR"
"$SCRIPT_DIR/configure" \
    --prefix="$STAGING_DIR" \
    --target-list=x86_64-softmmu \
    --enable-slirp \
    --disable-docs

# 3) Build and Install
echo "=== Building QEMU ==="
ninja

echo "=== Installing QEMU to Staging ==="
ninja install

# 4) Package using package_bundle.sh
echo "=== Packaging Bundle ==="
SRC="$STAGING_DIR" \
OUT="$OUT" \
PATCHELF=patchelf \
"$SCRIPT_DIR/package_bundle.sh"

# 5) Create tarball
echo "=== Creating Tarball ==="
cd "$DIST_DIR"
tar -czf "wasabi-qemu-x86_64-linux.tar.gz" "wasabi-qemu-x86_64-linux"
echo "Bundle created at: $DIST_DIR/wasabi-qemu-x86_64-linux.tar.gz"
