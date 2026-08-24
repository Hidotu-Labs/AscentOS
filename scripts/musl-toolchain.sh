#!/bin/sh
# AvoryOS: download and setup x86_64 musl toolchain from Bootlin
#
# This script avoids the long build time of musl-cross-make by downloading a 
# pre-built, production-ready toolchain.

set -e

TOOLCHAIN_VERSION="2024.02-1"
TOOLCHAIN_TARBALL="x86-64--musl--stable-${TOOLCHAIN_VERSION}.tar.bz2"
TOOLCHAIN_URL="https://toolchains.bootlin.com/downloads/releases/toolchains/x86-64/tarballs/${TOOLCHAIN_TARBALL}"

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
OUTPUT_DIR="${MUSL_TOOLCHAIN_DIR:-"$ROOT_DIR/toolchain/x86_64-linux-musl"}"
BUILD_DIR="${BUILD_DIR:-"$ROOT_DIR/build/musl-toolchain"}"
SYSROOT_LINK="${MUSL_SYSROOT:-"$ROOT_DIR/toolchain/musl-sysroot"}"

mkdir -p "$BUILD_DIR"
mkdir -p "$ROOT_DIR/toolchain"

if [ ! -d "$OUTPUT_DIR" ] || [ ! -f "$OUTPUT_DIR/bin/x86_64-buildroot-linux-musl-gcc" ]; then
    echo "--- Downloading musl toolchain from Bootlin ---"
    if [ ! -f "$BUILD_DIR/$TOOLCHAIN_TARBALL" ]; then
        curl -L "$TOOLCHAIN_URL" -o "$BUILD_DIR/$TOOLCHAIN_TARBALL"
    fi

    echo "--- Extracting musl toolchain ---"
    mkdir -p "$OUTPUT_DIR"
    tar -xjf "$BUILD_DIR/$TOOLCHAIN_TARBALL" -C "$OUTPUT_DIR" --strip-components=1
    echo "musl toolchain installed to: $OUTPUT_DIR"
else
    echo "musl toolchain already exists at $OUTPUT_DIR. Skipping download."
fi

# Create standard x86_64-linux-musl-* symlinks in bin/
echo "--- Configuring toolchain aliases ---"
cd "$OUTPUT_DIR/bin"
for f in x86_64-buildroot-linux-musl-*; do
    if [ -f "$f" ]; then
        alias_name="x86_64-linux-musl-${f#x86_64-buildroot-linux-musl-}"
        ln -sf "$f" "$alias_name"
    fi
done
# Also ensure musl-gcc / musl-g++ symlinks exist
ln -sf x86_64-buildroot-linux-musl-gcc musl-gcc
ln -sf x86_64-buildroot-linux-musl-g++ musl-g++

# Create symlink to the sysroot
SYSROOT="$OUTPUT_DIR/x86_64-buildroot-linux-musl/sysroot"
if [ -d "$SYSROOT" ]; then
    ln -sfn "$SYSROOT" "$SYSROOT_LINK"
    echo "musl sysroot linked to: $SYSROOT_LINK"
fi

# Also create x86_64-linux-musl symlink inside toolchain dir for compatibility
if [ -d "$OUTPUT_DIR/x86_64-buildroot-linux-musl" ]; then
    ln -sfn "x86_64-buildroot-linux-musl" "$OUTPUT_DIR/x86_64-linux-musl"
fi

# Ensure compatibility paths exist within sysroot
if [ -d "$SYSROOT_LINK/usr/lib" ] && [ ! -e "$SYSROOT_LINK/lib" ]; then
    ln -sfn usr/lib "$SYSROOT_LINK/lib"
fi
if [ -d "$SYSROOT_LINK/usr/include" ] && [ ! -e "$SYSROOT_LINK/include" ]; then
    ln -sfn usr/include "$SYSROOT_LINK/include"
fi

# Create stub linux/vt.h for programs that need it (e.g., nano)
if [ -d "$SYSROOT_LINK/usr/include" ]; then
    mkdir -p "$SYSROOT_LINK/usr/include/linux"
    cat > "$SYSROOT_LINK/usr/include/linux/vt.h" << 'VT_H_EOF'
/* Stub linux/vt.h for AvoryOS */
#ifndef _LINUX_VT_H
#define _LINUX_VT_H

#define VT_GETSTATE 0x5603
#define VT_RELDISP 0x5605
#define VT_ACTIVATE 0x5606
#define VT_WAITACTIVE 0x5607
#define VT_GETMODE 0x5600
#define VT_SETMODE 0x5602
#define VT_DISALLOCATE 0x5608

struct vt_stat {
    unsigned short v_active;
    unsigned short v_signal;
    unsigned short v_state;
};

struct vt_mode {
    char mode;
    char waitv;
    short relsig;
    short acqsig;
    short frsig;
};

#endif /* _LINUX_VT_H */
VT_H_EOF
fi

if [ -d "$SYSROOT_LINK/include" ] && [ ! -f "$SYSROOT_LINK/include/linux/vt.h" ]; then
    mkdir -p "$SYSROOT_LINK/include/linux"
    cp "$SYSROOT_LINK/usr/include/linux/vt.h" "$SYSROOT_LINK/include/linux/vt.h" 2>/dev/null || true
fi

echo "All musl toolchain dependencies have been set up."

