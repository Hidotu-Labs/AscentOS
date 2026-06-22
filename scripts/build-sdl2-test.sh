#!/bin/sh
# Build script for test_sdl2.c

ROOT_DIR=$(cd -- "$(dirname "$0")/.." && pwd)
CC="$ROOT_DIR/toolchain/x86_64-linux-musl/bin/x86_64-linux-musl-gcc"
ALPINE_SYSROOT="$ROOT_DIR/build/alpine/rootfs"
MUSL_SYSROOT="$ROOT_DIR/toolchain/musl-sysroot"

if [ ! -x "$CC" ]; then
    echo "Error: Musl compiler not found at $CC"
    exit 1
fi

echo "Compiling userland/test_sdl2.c..."

$CC -static -fno-lto \
    -I"$ALPINE_SYSROOT/usr/include" \
    -I"$MUSL_SYSROOT/include" \
    "$ROOT_DIR/userland/test_sdl2.c" \
    -o "$ROOT_DIR/userland/test_sdl2.elf" \
    -L"$ALPINE_SYSROOT/usr/lib" \
    -L"$MUSL_SYSROOT/lib" \
    -lSDL2 -lm -lpthread -ldl -lX11 -lxcb -lXau -lXdmcp -lmd

if [ $? -eq 0 ]; then
    echo "Success! Output: userland/test_sdl2.elf"
else
    echo "Compilation failed."
    exit 1
fi
