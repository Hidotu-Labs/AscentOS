#!/bin/sh
# AscentOS: Build ClassiCube for AscentOS
#
# This script cross-compiles ClassiCube (Minecraft Classic Client)
# using the musl toolchain and TinyGL for software rendering.
#
# Usage:
#   ./scripts/build-classicube.sh          # Build ClassiCube
#   ./scripts/build-classicube.sh clean    # Clean build directory

set -e

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT_DIR/build/classicube"}
PREFIX=${MUSL_SYSROOT:-"$ROOT_DIR/toolchain/musl-sysroot"}
INSTALL_DIR="$ROOT_DIR/userland"
TINYGL_DIR="$PREFIX/opt/tinygl"
JOBS=$(nproc 2>/dev/null || echo 4)
REPO_URL="https://github.com/ClassiCube/ClassiCube/archive/refs/heads/master.tar.gz"

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
    rm -f "$INSTALL_DIR/classicube.elf"
    echo "Done."
}

build_classicube() {
    find_compiler
    
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    
    # Download ClassiCube source
    if [ ! -f "master.tar.gz" ]; then
        echo "Downloading ClassiCube ..."
        curl -L -o "master.tar.gz" "$REPO_URL"
    fi
    
    # Extract
    if [ ! -d "ClassiCube-master" ]; then
        echo "Extracting ClassiCube ..."
        tar xf "master.tar.gz"
    fi
    
    cd "ClassiCube-master"
    
    # Patch Core.h to allow overriding CC_BUILD_POSIX and CC_BUILD_LINUX, and disable XInput2
    # We use a temp file and only write if it's different to be cleaner, 
    # but for simplicity here we just make sure we don't double-patch.
    # Note: \x0a is a newline in some sed versions, but we'll use a simpler approach.
    
    # Wrap CC_BUILD_POSIX in #ifndef if not already wrapped
    if ! grep -q "#ifndef CC_BUILD_POSIX" src/Core.h; then
        sed -i 's/#define CC_BUILD_POSIX/#ifndef CC_BUILD_POSIX\n\t#define CC_BUILD_POSIX\n\t#endif/' src/Core.h
    fi
    
    # Wrap CC_BUILD_LINUX in #ifndef if not already wrapped
    if ! grep -q "#ifndef CC_BUILD_LINUX" src/Core.h; then
        sed -i 's/#define CC_BUILD_LINUX/#ifndef CC_BUILD_LINUX\n\t#define CC_BUILD_LINUX\n\t#endif/' src/Core.h
    fi
    
    # Disable CC_BUILD_XINPUT2 if not already disabled
    sed -i 's/^#define CC_BUILD_XINPUT2/\/\/#define CC_BUILD_XINPUT2/' src/Core.h
    
    echo "Building ClassiCube for AscentOS (X11 + Software Renderer) ..."

    # ClassiCube's Makefile is very flexible but we use manual compilation 
    # to ensure all AscentOS-specific flags are handled correctly.
    # We define:
    # - CC_BUILD_X11: Use X11 for windowing/input
    # - CC_BUILD_GL1: Use OpenGL 1.1 for rendering (TinyGL)
    # - CC_BUILD_POSIX: Use POSIX APIs for threading/filesystem
    # - CC_BUILD_CURL: (Disabled for now to simplify, or use wget)
    # - CC_BUILD_NOMUSIC: Disable audio for now to ensure stable first run
    
    CFLAGS="-O2 -static -fno-stack-protector \
            -DCC_BUILD_X11 -DCC_GFX_BACKEND=1 -DCC_BUILD_POSIX \
            -DCC_BUILD_NOMUSIC \
            -I$PREFIX/include"
            
    LDFLAGS="-static -L$PREFIX/lib"
    
    # Libraries needed for X11 on AscentOS
    LIBS="-lX11 -lxcb -lXau -lXdmcp -lmd -lm -lc"

    CORE_SOURCES=$(ls src/*.c | grep -vE "Platform_|Window_|Graphics_|ascentos_stubs")
    BEARSSL_SOURCES=$(ls third_party/bearssl/*.c 2>/dev/null || echo "")
    BACKEND_SOURCES="src/Platform_Posix.c src/Window_X11.c src/Graphics_SoftGPU.c"
    SOURCES="$CORE_SOURCES $BEARSSL_SOURCES $BACKEND_SOURCES"
    
    OBJ_DIR="build-ascentos"
    mkdir -p "$OBJ_DIR"

    echo "Compiling ClassiCube sources incrementally ..."
    
    OBJECTS=""
    for src in $SOURCES; do
        obj="$OBJ_DIR/$(echo $src | tr '/' '_').o"
        OBJECTS="$OBJECTS $obj"
        
        if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
            echo "[CC] $src"
            $CC $CFLAGS -c "$src" -o "$obj"
        fi
    done

    echo "Linking ClassiCube ..."
    $CC $LDFLAGS $OBJECTS -o classicube.elf $LIBS

    echo "ClassiCube built successfully!"
    cp classicube.elf "$INSTALL_DIR/classicube.elf"
    echo "Executable copied to: $INSTALL_DIR/classicube.elf"
    
    cd "$ROOT_DIR"
}

case "${1:-build}" in
    clean)
        do_clean
        ;;
    build|"")
        build_classicube
        ;;
    *)
        echo "Usage: $0 [build|clean]" >&2
        exit 1
    ;;
esac
