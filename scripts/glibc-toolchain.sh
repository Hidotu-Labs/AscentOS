#!/bin/sh
# AscentOS: download and setup x86_64 glibc toolchain from Bootlin
#
# This script avoids the long build time of glibc by downloading a 
# pre-built, production-ready toolchain.

set -e

TOOLCHAIN_VERSION="2024.02-1"
TOOLCHAIN_TARBALL="x86-64--glibc--stable-${TOOLCHAIN_VERSION}.tar.bz2"
TOOLCHAIN_URL="https://toolchains.bootlin.com/downloads/releases/toolchains/x86-64/tarballs/${TOOLCHAIN_TARBALL}"

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
OUTPUT_DIR="$ROOT_DIR/toolchain/x86_64-linux-glibc"
BUILD_DIR="$ROOT_DIR/build/glibc-toolchain"

mkdir -p "$BUILD_DIR"
mkdir -p "$ROOT_DIR/toolchain"

if [ ! -d "$OUTPUT_DIR" ]; then
    echo "--- Downloading glibc toolchain from Bootlin ---"
    if [ ! -f "$BUILD_DIR/$TOOLCHAIN_TARBALL" ]; then
        curl -L "$TOOLCHAIN_URL" -o "$BUILD_DIR/$TOOLCHAIN_TARBALL"
    fi

    echo "--- Extracting glibc toolchain ---"
    mkdir -p "$OUTPUT_DIR"
    tar -xjf "$BUILD_DIR/$TOOLCHAIN_TARBALL" -C "$OUTPUT_DIR" --strip-components=1
    echo "glibc toolchain installed to: $OUTPUT_DIR"
else
    echo "glibc toolchain already exists at $OUTPUT_DIR. Skipping."
fi

# Create convenience aliases
if [ -d "$OUTPUT_DIR/bin" ]; then
    for tool in gcc g++ cpp ar as ld nm objcopy objdump ranlib strip; do
        if [ -f "$OUTPUT_DIR/bin/x86_64-buildroot-linux-gnu-$tool" ]; then
            ln -sfn "x86_64-buildroot-linux-gnu-$tool" "$OUTPUT_DIR/bin/x86_64-linux-$tool"
            ln -sfn "x86_64-buildroot-linux-gnu-$tool" "$OUTPUT_DIR/bin/x86_64-linux-gnu-$tool"
        fi
    done
fi

# Create a symlink to the sysroot if needed
SYSROOT="$OUTPUT_DIR/x86_64-buildroot-linux-gnu/sysroot"
if [ -d "$SYSROOT" ]; then
    ln -sfn "$SYSROOT" "$ROOT_DIR/toolchain/glibc-sysroot"
    echo "glibc sysroot linked to: $ROOT_DIR/toolchain/glibc-sysroot"
fi

echo "All glibc toolchain dependencies have been set up."
