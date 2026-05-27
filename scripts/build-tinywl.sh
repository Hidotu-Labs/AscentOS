#!/usr/bin/env bash
# scripts/build-tinywl.sh - Build TinyWL compositor for AscentOS

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SYSROOT="${ROOT_DIR}/build/alpine/rootfs"
CC="${ROOT_DIR}/toolchain/x86_64-linux-musl/bin/x86_64-linux-musl-gcc"

if [ ! -d "$SYSROOT" ]; then
    echo "Error: Alpine rootfs not found. Run scripts/setup-alpine.sh first."
    exit 1
fi

# Fix missing symlinks in sysroot
ln -sf libxcb-ewmh.so "${SYSROOT}/usr/lib/libxcb-ewmh.so.2" 2>/dev/null || true
ln -sf libxcb-icccm.so "${SYSROOT}/usr/lib/libxcb-icccm.so.4" 2>/dev/null || true

# Generate Wayland protocol headers
XDG_SHELL_XML="${SYSROOT}/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml"
if [ ! -f "$XDG_SHELL_XML" ]; then
    echo "Error: xdg-shell.xml not found."
    exit 1
fi

echo "[*] Generating xdg-shell protocol headers..."
wayland-scanner server-header "$XDG_SHELL_XML" "${ROOT_DIR}/userland/xdg-shell-protocol.h"
wayland-scanner private-code "$XDG_SHELL_XML" "${ROOT_DIR}/userland/xdg-shell-protocol.c"

# We need to compile against the libraries in the Alpine rootfs.
INCLUDES=(
    "-I${SYSROOT}/usr/include"
    "-I${SYSROOT}/usr/include/wlroots-0.18"
    "-I${SYSROOT}/usr/include/pixman-1"
    "-I${SYSROOT}/usr/include/libdrm"
    "-I${SYSROOT}/usr/include/libinput"
    "-I${ROOT_DIR}/userland"
)

LIBS=(
    "-L${SYSROOT}/usr/lib"
    "-L${SYSROOT}/lib"
    "-lwlroots-0.18"
    "-lwayland-server"
    "-lxcb"
    "-lxcb-ewmh"
    "-lxcb-icccm"
    "-lxcb-res"
    "-lxcb-render"
    "-lxcb-render-util"
    "-lxkbcommon"
    "-lstdc++"
    "-lm"
)

echo "[*] Compiling userland/tinywl.c ..."
if $CC -O2 -DWLR_USE_UNSTABLE \
    "${ROOT_DIR}/userland/tinywl.c" \
    "${ROOT_DIR}/userland/xdg-shell-protocol.c" \
    "${ROOT_DIR}/userland/cpp_shim.c" \
    -o "${ROOT_DIR}/userland/tinywl.elf" \
    "${INCLUDES[@]}" \
    "${LIBS[@]}" \
    -Wl,-dynamic-linker,/lib/ld-musl-x86_64.so.1 \
    -Wl,-rpath,/usr/lib \
    -Wl,-rpath-link,${SYSROOT}/usr/lib \
    -Wl,-rpath-link,${SYSROOT}/lib; then
    echo "[SUCCESS] Built userland/tinywl.elf"
else
    echo "[FAILURE] Compilation failed"
    exit 1
fi
