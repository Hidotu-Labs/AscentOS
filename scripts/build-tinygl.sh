#!/bin/sh
# AscentOS: Build TinyGL for AscentOS
#
# This script cross-compiles TinyGL (software rasterizer) using the musl toolchain.
#
# Usage:
#   ./scripts/build-tinygl.sh          # Build TinyGL
#   ./scripts/build-tinygl.sh clean    # Clean build directory

set -e

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT_DIR/build/tinygl"}
PREFIX=${MUSL_SYSROOT:-"$ROOT_DIR/toolchain/musl-sysroot"}
INSTALL_DIR="$PREFIX/opt/tinygl"
JOBS=$(nproc 2>/dev/null || echo 4)
REPO_URL="https://github.com/C-Chads/tinygl/archive/refs/heads/master.tar.gz"

find_compiler() {
    LOCAL_CC="$ROOT_DIR/toolchain/x86_64-linux-musl/bin/x86_64-linux-musl-gcc"
    
    if [ -x "$LOCAL_CC" ]; then
        CC="$LOCAL_CC"
    elif command -v x86_64-linux-musl-gcc >/dev/null 2>&1; then
        CC="x86_64-linux-musl-gcc"
    elif command -v musl-gcc >/dev/null 2>&1; then
        CC="musl-gcc"
    else
        echo "Error: No musl compiler found. Run scripts/musl-toolchain.sh first." >&2
        exit 1
    fi
    echo "Using CC=$CC"
}

do_clean() {
    echo "Cleaning $BUILD_DIR ..."
    rm -rf "$BUILD_DIR"
    rm -rf "$INSTALL_DIR"
    rm -f "$ROOT_DIR/userland/tglgears.elf"
    echo "Done."
}

build_tinygl() {
    find_compiler
    
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    
    # Download TinyGL source
    if [ ! -f "master.tar.gz" ]; then
        echo "Downloading TinyGL ..."
        curl -L -o "master.tar.gz" "$REPO_URL"
    fi
    
    case "$CC" in
        *-gcc)
            _prefix="${CC%gcc}"
            if command -v "${_prefix}ar" >/dev/null 2>&1; then
                AR="${_prefix}ar"
            elif [ -x "$ROOT_DIR/toolchain/x86_64-linux-musl/bin/x86_64-linux-musl-ar" ]; then
                AR="$ROOT_DIR/toolchain/x86_64-linux-musl/bin/x86_64-linux-musl-ar"
            else
                AR=ar
            fi
            ;;
        *)
            AR=ar
            ;;
    esac

    # Extract
    if [ ! -d "tinygl-main" ] && [ ! -d "tinygl-master" ]; then
        echo "Extracting TinyGL ..."
        tar xf "master.tar.gz"
    fi
    
    if [ -d "tinygl-main" ]; then
        cd "tinygl-main"
    elif [ -d "tinygl-master" ]; then
        cd "tinygl-master"
    else
        echo "Error: Failed to find extracted tinygl directory" >&2
        exit 1
    fi
    
    echo "Configuring TinyGL for AscentOS cross-compilation ..."
    
    # The default config.mk has -march=native and OpenMP enabled, which breaks cross-compiling.
    # We overwrite config.mk to set up our target architectures and paths cleanly.
    cat > config.mk << EOF
CC=$CC
AR=$AR
CFLAGS= -Wall -O3 -std=c99 -DNDEBUG -fno-stack-protector -Wno-unused-function -I$PREFIX/include
CFLAGS_LIB= \$(CFLAGS) -pedantic
LFLAGS= -static -L$PREFIX/lib
EOF

    echo "Building TinyGL ..."
    
    # Build only the library and raw demos (Raw demos only depend on libc, no SDL needed)
    make -j"$JOBS" lib/libTinyGL.a RDMOS
    
    # Install to our opt directory
    echo "Installing TinyGL to $INSTALL_DIR ..."
    mkdir -p "$INSTALL_DIR/lib" "$INSTALL_DIR/include/tinygl"
    cp lib/libTinyGL.a "$INSTALL_DIR/lib/"
    cp -r include/* "$INSTALL_DIR/include/tinygl/"
    
    # Put the built 3D gear raw demo into the userland folder to easily mount and test on AscentOS
    if [ -f "Raw_Demos/gears" ]; then
        echo "Copying gears demo to userland/tglgears.elf ..."
        cp "Raw_Demos/gears" "$ROOT_DIR/userland/tglgears.elf"
    fi
    
    cd "$ROOT_DIR"
    
    echo ""
    echo "========================================"
    echo "TinyGL built successfully for AscentOS!"
    echo "========================================"
    echo "The library is installed at: $INSTALL_DIR"
    echo "You can link against it via: -L$INSTALL_DIR/lib -I$INSTALL_DIR/include -lTinyGL"
    echo ""
    echo "A framebuffer-capable RAW demo has been copied to: userland/tglgears.elf"
    echo ""
}

case "${1:-build}" in
    clean)
        do_clean
        ;;
    build|"")
        build_tinygl
        ;;
    *)
        echo "Usage: $0 [build|clean]" >&2
        exit 1
        ;;
esac
