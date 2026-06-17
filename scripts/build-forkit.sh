#!/usr/bin/env bash
# AscentOS: Build Forkit browser for AscentOS
#
# Forkit is a minimal web browser written in Rust using SDL2.
# This script cross-compiles it targeting x86_64-unknown-linux-musl,
# linking against the SDL2 libraries already installed in the Alpine rootfs
# by setup-alpine.sh.
#
# Prerequisites:
#   - Rust toolchain with the x86_64-unknown-linux-musl target installed
#   - SDL2 dev packages in build/alpine/rootfs (run setup-alpine.sh first)
#
# Usage:
#   ./scripts/build-forkit.sh          # Build Forkit
#   ./scripts/build-forkit.sh clean    # Clean build directory

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/forkit"
ALPINE_ROOTFS="${ROOT_DIR}/build/alpine/rootfs"
INSTALL_DIR="${ROOT_DIR}/userland"
REPO_URL="https://github.com/Hidotu-Labs/Forkit"

RUST_TARGET="x86_64-unknown-linux-musl"

SDL2_INCLUDE="${ALPINE_ROOTFS}/usr/include"
SDL2_LIB="${ALPINE_ROOTFS}/usr/lib"
ALPINE_LIB="${ALPINE_ROOTFS}/lib"   # musl libc lives here in Alpine

do_clean() {
    echo "Cleaning ${BUILD_DIR} ..."
    rm -rf "${BUILD_DIR}"
    rm -f "${INSTALL_DIR}/forkit.elf"
    echo "Done."
}

check_rust() {
    if ! command -v cargo >/dev/null 2>&1; then
        echo "[!] cargo not found. Please install Rust: https://rustup.rs" >&2
        exit 1
    fi
    if ! rustup target list --installed 2>/dev/null | grep -q "${RUST_TARGET}"; then
        echo "[*] Adding Rust target ${RUST_TARGET} ..."
        rustup target add "${RUST_TARGET}"
    fi
}

check_sdl2() {
    if [ ! -f "${SDL2_LIB}/libSDL2.so" ] && [ ! -f "${SDL2_LIB}/libSDL2-2.0.so.0" ]; then
        echo "[!] SDL2 not found in Alpine rootfs (${SDL2_LIB})."
        echo "    Run './scripts/setup-alpine.sh' first." >&2
        exit 1
    fi
}

# The Alpine APK installer creates libSDL2_image.so -> libSDL2_image-2.0.so.0
# (versioned runtime name), but the linker needs libSDL2_image.so -> libSDL2_image-2.0.so.
fix_sdl2_symlinks() {
    local lib="${SDL2_LIB}"
    for name in SDL2_image SDL2_mixer SDL2_ttf SDL2_net; do
        local target
        target=$(find "${lib}" -maxdepth 1 -name "lib${name}-*.so" ! -name "*.so.*" 2>/dev/null | sort | tail -n1)
        if [ -n "${target}" ]; then
            local base
            base=$(basename "${target}")
            echo "[*] Fixing symlink: lib${name}.so -> ${base}"
            ln -sf "${base}" "${lib}/lib${name}.so"
        else
            echo "[!] Warning: could not find unversioned lib${name}-*.so in ${lib}" >&2
        fi
    done
}

build_forkit() {
    check_rust
    check_sdl2
    fix_sdl2_symlinks

    # Clone or update the repo
    if [ ! -d "${BUILD_DIR}/.git" ]; then
        echo "[*] Cloning Forkit ..."
        mkdir -p "$(dirname "${BUILD_DIR}")"
        git clone --depth=1 "${REPO_URL}" "${BUILD_DIR}"
    else
        echo "[*] Forkit already cloned, pulling latest ..."
        git -C "${BUILD_DIR}" pull --ff-only
    fi

    cd "${BUILD_DIR}"

    echo "[*] Configuring cross-compilation environment ..."

    # Locate the musl gcc cross-compiler
    MUSL_GCC_BIN="${ROOT_DIR}/toolchain/x86_64-linux-musl/bin/x86_64-linux-musl-gcc"
    if [ -x "${MUSL_GCC_BIN}" ]; then
        export PATH="${ROOT_DIR}/toolchain/x86_64-linux-musl/bin:${PATH}"
        MUSL_GCC_CMD="${MUSL_GCC_BIN}"
    elif command -v x86_64-linux-musl-gcc >/dev/null 2>&1; then
        MUSL_GCC_CMD="x86_64-linux-musl-gcc"
    else
        echo "[!] x86_64-linux-musl-gcc not found. Run scripts/musl-toolchain.sh first." >&2
        exit 1
    fi
    echo "[*] Using musl gcc: ${MUSL_GCC_CMD}"

    # PKG_CONFIG pointing at the Alpine rootfs (no --static so crate emits dylib links)
    export PKG_CONFIG_SYSROOT_DIR="${ALPINE_ROOTFS}"
    export PKG_CONFIG_PATH="${ALPINE_ROOTFS}/usr/lib/pkgconfig:${ALPINE_ROOTFS}/usr/share/pkgconfig"
    export PKG_CONFIG_ALLOW_CROSS=1
    unset PKG_CONFIG_LIBDIR

    # Tell the sdl2 crate to link dynamically
    export SDL2_INCLUDE_PATH="${SDL2_INCLUDE}/SDL2"
    export SDL2_LIB_PATH="${SDL2_LIB}"
    export SDL2_DYNAMIC=1

    # --- Linker wrapper ---
    #
    # The challenge: Rust's musl target passes -Wl,-Bstatic early so every
    # -lSDL2* the sdl2 crate emits needs a static .a.  Alpine only ships .so
    # for SDL2_image/mixer/ttf.  Also, Alpine's SDL2 .so files pull in
    # libc.musl-x86_64.so.1, libfreetype.so, libharfbuzz.so, libglib-2.0.so
    # etc. as transitive deps — our cross ld would normally try to resolve those
    # too, failing because they contain glibc-specific symbol names that don't
    # match our static musl.
    #
    # Solution:
    #   1. Wrap each -lSDL2* in-place with -Wl,-Bdynamic / -Wl,-Bstatic so
    #      the linker accepts the .so files.
    #   2. Add -L for both /usr/lib and /lib in the Alpine rootfs (SDL2 is in
    #      /usr/lib, Alpine's musl libc.so is in /lib).
    #   3. Pass --allow-shlib-undefined to ld so it does NOT try to recursively
    #      resolve symbols from libfreetype/libharfbuzz/libglib/libc that SDL2
    #      pulls in transitively — those will be satisfied at runtime on
    #      AscentOS where the full Alpine rootfs is present.
    #   4. Set rpath=/usr/lib:/lib so the runtime loader finds everything.
    #
    WRAPPER_DIR="${BUILD_DIR}/.linker-wrapper"
    mkdir -p "${WRAPPER_DIR}"

    cat > "${WRAPPER_DIR}/musl-ld-wrapper" << WRAPPER_EOF
#!/usr/bin/env python3
"""
Linker wrapper for building Forkit (Rust/SDL2) against musl targeting AscentOS.

Key transforms applied to the gcc/ld argv:
  - Rewrite rcrt1.o/crt1.o to Scrt1.o so the output uses dynamic-PIE startup
  - Replace -static-pie with -pie and drop full-static flags
  - Inject -L<rootfs/usr/lib> and -L<rootfs/lib> before first -Wl,-Bstatic
  - Inject -Wl,--allow-shlib-undefined before first -Wl,-Bstatic so transitive
    deps of SDL2 .so files (freetype, harfbuzz, glib, musl libc) are not
    required to be resolvable at cross-compile time (they exist at runtime)
  - Keep SDL2, libgcc_s, and libc in dynamic mode so they resolve as .so files
  - End in -Wl,-Bdynamic and append -Wl,-rpath,/usr/lib:/lib for runtime lookup
"""
import sys, subprocess

SDL2_LIBS   = {"-lSDL2", "-lSDL2_ttf", "-lSDL2_image", "-lSDL2_mixer"}
SDL2_LIBDIR = "${SDL2_LIB}"
ALPINE_LIBDIR = "${ALPINE_LIB}"
TOOLCHAIN_LIBDIR = "${ROOT_DIR}/toolchain/x86_64-linux-musl/x86_64-linux-musl/lib"
INTERP      = "/lib/ld-musl-x86_64.so.1"
REAL_GCC    = "${MUSL_GCC_CMD}"

COMMON_LINK_ARGS = [
    "-L" + SDL2_LIBDIR,
    "-L" + ALPINE_LIBDIR,
    "-L" + TOOLCHAIN_LIBDIR,
    "-Wl,-rpath-link," + SDL2_LIBDIR,
    "-Wl,-rpath-link," + ALPINE_LIBDIR,
    "-Wl,-rpath-link," + TOOLCHAIN_LIBDIR,
    "-Wl,--allow-shlib-undefined",
    "-Wl,--dynamic-linker," + INTERP,
]

def rewrite_crt_arg(arg):
    for old, new in (("rcrt1.o", "Scrt1.o"), ("crt1.o", "Scrt1.o")):
        if arg == old or arg.endswith("/" + old):
            return arg[:-len(old)] + new
    return arg

args = sys.argv[1:]
out  = []
injected_prefix = False
pie_added = False

for i, a in enumerate(args):
    a = rewrite_crt_arg(a)

    # Replace -static-pie with -pie so the output is a normal dynamic PIE.
    # Also drop full-static mode and -nodefaultlibs since we need libc and
    # libgcc from the dynamic musl toolchain path.
    if a == "-static-pie":
        if not pie_added:
            out.append("-pie")
            pie_added = True
        continue
    if a in ("-static", "-Wl,-static", "-nodefaultlibs"):
        continue

    # Before the first -Wl,-Bstatic, inject:
    #   -L paths for SDL2 and Alpine musl libc (/lib)
    #   --allow-shlib-undefined so transitive .so deps of SDL2
    #     (freetype, harfbuzz, glib, Alpine musl libc) are not required to
    #     be resolvable at cross-compile time — they exist at runtime
    #   --dynamic-linker so the binary gets a proper PT_INTERP
    if not injected_prefix and a == "-Wl,-Bstatic":
        out += COMMON_LINK_ARGS
        injected_prefix = True

    if a in SDL2_LIBS:
        # SDL2 and the following runtime libs (-lgcc_s/-lc) must stay dynamic.
        prev = out[-1] if out else ""
        if prev != "-Wl,-Bdynamic":
            out.append("-Wl,-Bdynamic")
        out.append(a)
    else:
        out.append(a)

if not injected_prefix:
    out = COMMON_LINK_ARGS + out

# Runtime rpath: SDL2 lives in /usr/lib, Alpine musl libc in /lib. Leave the
# linker in dynamic mode so the default libc/libgcc search does not go static.
out += ["-Wl,-Bdynamic", "-Wl,-rpath,/usr/lib:/lib"]

sys.exit(subprocess.call([REAL_GCC] + out))
WRAPPER_EOF
    chmod +x "${WRAPPER_DIR}/musl-ld-wrapper"

    # Cargo config: use our wrapper as the linker for the musl target
    mkdir -p .cargo
    cat > .cargo/config.toml << EOF
[target.${RUST_TARGET}]
linker = "${WRAPPER_DIR}/musl-ld-wrapper"
rustflags = ["-C", "target-feature=-crt-static"]

[build]
target = "${RUST_TARGET}"
EOF

    echo "[*] Building Forkit (release) for ${RUST_TARGET} ..."
    cargo build --release --target "${RUST_TARGET}"

    BINARY="${BUILD_DIR}/target/${RUST_TARGET}/release/forkit"
    if [ ! -f "${BINARY}" ]; then
        echo "[!] Build succeeded but binary not found at ${BINARY}" >&2
        exit 1
    fi

    echo "[*] Copying binary to ${INSTALL_DIR}/forkit.elf ..."
    cp "${BINARY}" "${INSTALL_DIR}/forkit.elf"
    chmod +x "${INSTALL_DIR}/forkit.elf"

    echo ""
    echo "========================================"
    echo "Forkit built successfully for AscentOS!"
    echo "========================================"
    echo "Binary: ${INSTALL_DIR}/forkit.elf"
    echo "Forkit requires SDL2 at runtime (provided by the Alpine rootfs in disk.img)."
    echo ""
}

case "${1:-build}" in
    clean)
        do_clean
        ;;
    build|"")
        build_forkit
        ;;
    *)
        echo "Usage: $0 [build|clean]" >&2
        exit 1
        ;;
esac
