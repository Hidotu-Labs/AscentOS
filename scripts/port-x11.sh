#!/bin/sh
# AscentOS: X11 (TinyX/Kdrive) Porting Script
#
# This script orchestrates the downloading and building of the X11 stack
# for AscentOS using the musl cross-toolchain.
#
# It targets Kdrive (TinyX) with fbdev and evdev support.

set -e

# ── Environment Setup ────────────────────────────────────────────────────────
ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BUILD_BASE="$ROOT_DIR/build/x11"
SYSROOT=${MUSL_SYSROOT:-"$ROOT_DIR/toolchain/musl-sysroot"}
TOOLCHAIN_BIN="$ROOT_DIR/toolchain/x86_64-linux-musl/bin"
JOBS=$(nproc 2>/dev/null || echo 4)

# Ensure CC and other tools are in PATH
export PATH="$TOOLCHAIN_BIN:$PATH"

# Setup cross-compiler variables
# Setup cross-compiler variables
export CC="x86_64-linux-musl-gcc"
export CXX="x86_64-linux-musl-g++"
# Fallback to standard binutils if prefixed ones don't exist
export AR=$(command -v x86_64-linux-musl-ar || echo "ar")
export STRIP=$(command -v x86_64-linux-musl-strip || echo "strip")
export RANLIB=$(command -v x86_64-linux-musl-ranlib || echo "ranlib")
export PKG_CONFIG="pkg-config"
export PKG_CONFIG_PATH="$SYSROOT/lib/pkgconfig:$SYSROOT/share/pkgconfig"
export PKG_CONFIG_LIBDIR="$SYSROOT/lib/pkgconfig:$SYSROOT/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export HOST_TRIPLET="x86_64-linux-musl"

# Set ACLOCAL_PATH so autoreconf finds our freshly built xorg-macros
export ACLOCAL_PATH="$SYSROOT/share/aclocal"
mkdir -p "$ACLOCAL_PATH"

if ! command -v "$CC" >/dev/null 2>&1; then
    echo "Error: $CC not found. Run scripts/musl-toolchain.sh first."
    exit 1
fi

# ── Helper: Download and Extract ─────────────────────────────────────────────
fetch_pkg() {
    name=$1
    version=$2
    url=$3
    tarball="${name}-${version}.tar.gz"
    
    mkdir -p "$BUILD_BASE"
    cd "$BUILD_BASE"
    
    # Download only if not present or corrupted
    if [ ! -f "$tarball" ] || ! tar tzf "$tarball" >/dev/null 2>&1; then
        echo ">>> Downloading $name $version ..."
        rm -f "$tarball"
        curl -L --connect-timeout 60 --max-time 600 --retry 5 --retry-delay 5 -o "$tarball" "$url"
    fi
    
    # If directory exists but is broken (no build files), remove it
    if [ -d "${name}-${version}" ] && [ ! -f "${name}-${version}/configure.ac" ] && [ ! -f "${name}-${version}/meson.build" ]; then
        echo ">>> Removing broken directory ${name}-${version} ..."
        rm -rf "${name}-${version}"
    fi

    if [ ! -d "${name}-${version}" ]; then
        echo ">>> Extracting $tarball ..."
        mkdir -p "${name}-${version}"
        tar xf "$tarball" -C "${name}-${version}" --strip-components=1
    fi
    
    cd "${name}-${version}"
    
    # If build from git archive (GitLab), we might need to generate configure
    if [ ! -f "configure" ] && [ -f "configure.ac" ]; then
        echo ">>> Generating configure script for $name ..."
        autoreconf -vfi
    fi
}

# ── Helper: Prepare Meson Cross File ─────────────────────────────────────────
prepare_meson_cross() {
    CROSS_FILE="$BUILD_BASE/x86_64-ascentos.meson"
    echo ">>> Generating Meson cross file: $CROSS_FILE"
    sed -e "s|@CC@|$(command -v $CC)|g" \
        -e "s|@CXX@|$(command -v $CXX)|g" \
        -e "s|@AR@|$(command -v $AR)|g" \
        -e "s|@STRIP@|$(command -v $STRIP)|g" \
        -e "s|@SYSROOT@|$SYSROOT|g" \
        "$ROOT_DIR/scripts/x86_64-ascentos.meson.template" > "$CROSS_FILE"
}

# ── 0. Build util-macros (M4 Macros) ─────────────────────────────────────────
build_util_macros() {
    fetch_pkg "util-macros" "1.20.1" "https://gitlab.freedesktop.org/xorg/util/macros/-/archive/util-macros-1.20.1/macros-util-macros-1.20.1.tar.gz"
    echo ">>> Building util-macros ..."
    ./configure --prefix="$SYSROOT" --host="$HOST_TRIPLET"
    make install
}

# ── 1. Build xorgproto (Headers) ─────────────────────────────────────────────
build_xorgproto() {
    fetch_pkg "xorgproto" "2024.1" "https://gitlab.freedesktop.org/xorg/proto/xorgproto/-/archive/xorgproto-2024.1/xorgproto-2024.1.tar.gz"
    echo ">>> Building xorgproto ..."
    mkdir -p build && cd build
    meson setup .. \
        --cross-file "$BUILD_BASE/x86_64-ascentos.meson" \
        --prefix="$SYSROOT" \
        --buildtype=release \
        -Dlegacy=true
    ninja install
}

# ── 3. Build zlib (Compression) ──────────────────────────────────────────────
build_zlib() {
    fetch_pkg "zlib" "1.3.1" "https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz"
    echo ">>> Building zlib ..."
    # zlib's configure is a custom script but respects CC
    CC="$CC" CHOST="$HOST_TRIPLET" ./configure --prefix="$SYSROOT" --static
    make install
}

# ── 4. Build libXau (Authorization) ──────────────────────────────────────────
build_libXau() {
    fetch_pkg "libXau" "1.0.11" "https://gitlab.freedesktop.org/xorg/lib/libxau/-/archive/libXau-1.0.11/libxau-libXau-1.0.11.tar.gz"
    echo ">>> Building libXau ..."
    ./configure --prefix="$SYSROOT" --host="$HOST_TRIPLET" --enable-static --disable-shared
    make install
}

# ── 3. Build libXdmcp (Display Manager Control Protocol) ────────────────────
build_libXdmcp() {
    fetch_pkg "libXdmcp" "1.1.5" "https://gitlab.freedesktop.org/xorg/lib/libxdmcp/-/archive/libXdmcp-1.1.5/libxdmcp-libXdmcp-1.1.5.tar.gz"
    echo ">>> Building libXdmcp ..."
    ./configure --prefix="$SYSROOT" --host="$HOST_TRIPLET" --enable-static --disable-shared
    make install
}

# ── 4. Build pixman (Rendering core) ─────────────────────────────────────────
# ── 4. Build xtrans (Transport Layer) ─────────────────────────────────────────
build_xtrans() {
    fetch_pkg "xtrans" "1.5.0" "https://gitlab.freedesktop.org/xorg/lib/libxtrans/-/archive/xtrans-1.5.0/libxtrans-xtrans-1.5.0.tar.gz"
    echo ">>> Building xtrans ..."
    ./configure --prefix="$SYSROOT" --host="$HOST_TRIPLET"
    make install
}

# ── 5. Build libmd (Message Digest) ──────────────────────────────────────────
build_libmd() {
    fetch_pkg "libmd" "1.1.0" "https://gitlab.freedesktop.org/libbsd/libmd/-/archive/1.1.0/libmd-1.1.0.tar.gz"
    echo ">>> Building libmd ..."
    ./configure --prefix="$SYSROOT" --host="$HOST_TRIPLET" --enable-static --disable-shared
    make install
}

# ── 6. Build xcb-proto (XCB Protocol Data) ──────────────────────────────────
build_xcb_proto() {
    fetch_pkg "xcb-proto" "1.16.0" "https://gitlab.freedesktop.org/xorg/proto/xcbproto/-/archive/xcb-proto-1.16.0/xcbproto-xcb-proto-1.16.0.tar.gz"
    echo ">>> Building xcb-proto ..."
    ./configure --prefix="$SYSROOT" --host="$HOST_TRIPLET"
    make install
}

# ── 6. Build libxcb (X C Binding) ───────────────────────────────────────────
build_libxcb() {
    fetch_pkg "libxcb" "1.16" "https://gitlab.freedesktop.org/xorg/lib/libxcb/-/archive/libxcb-1.16/libxcb-libxcb-1.16.tar.gz"
    echo ">>> Building libxcb ..."
    ./configure --prefix="$SYSROOT" --host="$HOST_TRIPLET" --enable-static --disable-shared
    make install
}

# ── 7. Build libX11 (X Library) ──────────────────────────────────────────────
build_libX11() {
    fetch_pkg "libX11" "1.8.7" "https://gitlab.freedesktop.org/xorg/lib/libx11/-/archive/libX11-1.8.7/libx11-libX11-1.8.7.tar.gz"
    echo ">>> Building libX11 ..."
    ./configure --prefix="$SYSROOT" --host="$HOST_TRIPLET" --enable-static --disable-shared
    make install
}

# ── 8. Build libXext (X Extensions) ──────────────────────────────────────────
build_libXext() {
    fetch_pkg "libXext" "1.3.5" "https://gitlab.freedesktop.org/xorg/lib/libxext/-/archive/libXext-1.3.5/libxext-libXext-1.3.5.tar.gz"
    echo ">>> Building libXext ..."
    ./configure --prefix="$SYSROOT" --host="$HOST_TRIPLET" --enable-static --disable-shared
    make install
}

# ── 9. Build pixman (Rendering) ──────────────────────────────────────────────
build_pixman() {
    fetch_pkg "pixman" "0.42.2" "https://gitlab.freedesktop.org/pixman/pixman/-/archive/pixman-0.42.2/pixman-pixman-0.42.2.tar.gz"
    echo ">>> Building pixman ..."
    mkdir -p build-ascentos && cd build-ascentos
    meson setup .. \
        --cross-file "$BUILD_BASE/x86_64-ascentos.meson" \
        --prefix="$SYSROOT" \
        --buildtype=release \
        --default-library=static \
        -Dgtk=disabled \
        -Dlibpng=disabled \
        -Dtests=disabled
    ninja install
}

# ── 10. Build libxcvt (CVT Modeline Generator) ──────────────────────────────
build_libxcvt() {
    fetch_pkg "libxcvt" "0.1.2" "https://gitlab.freedesktop.org/xorg/lib/libxcvt/-/archive/libxcvt-0.1.2/libxcvt-libxcvt-0.1.2.tar.gz"
    echo ">>> Building libxcvt ..."
    mkdir -p build-ascentos && cd build-ascentos
    meson setup .. \
        --cross-file "$BUILD_BASE/x86_64-ascentos.meson" \
        --prefix="$SYSROOT" \
        --buildtype=release \
        --default-library=static
    ninja install
}

# ── 5. Build font-util (Font Macros) ─────────────────────────────────────────
build_font_util() {
    fetch_pkg "font-util" "1.4.1" "https://gitlab.freedesktop.org/xorg/font/util/-/archive/font-util-1.4.1/util-font-util-1.4.1.tar.gz"
    echo ">>> Building font-util ..."
    ./configure --prefix="$SYSROOT" --host="$HOST_TRIPLET"
    make install
}

# ── 6. Build freetype (Font engine) ──────────────────────────────────────────
build_freetype() {
    fetch_pkg "freetype" "2.13.2" "https://download.savannah.gnu.org/releases/freetype/freetype-2.13.2.tar.gz"
    echo ">>> Building freetype ..."
    mkdir -p build-ascentos && cd build-ascentos
    meson setup .. \
        --cross-file "$BUILD_BASE/x86_64-ascentos.meson" \
        --prefix="$SYSROOT" \
        --buildtype=release \
        --default-library=static \
        -Dzlib=disabled \
        -Dbzip2=disabled \
        -Dpng=disabled \
        -Dharfbuzz=disabled \
        -Dbrotli=disabled
    ninja install
}

# ── 6. Build libfontenc ──────────────────────────────────────────────────────
build_libfontenc() {
    fetch_pkg "libfontenc" "1.1.8" "https://gitlab.freedesktop.org/xorg/lib/libfontenc/-/archive/libfontenc-1.1.8/libfontenc-libfontenc-1.1.8.tar.gz"
    echo ">>> Building libfontenc ..."
    ./configure \
        --host=x86_64-linux-musl \
        --prefix="$SYSROOT" \
        --enable-static \
        --disable-shared \
        CFLAGS="-static -O3 -I$SYSROOT/include" \
        LDFLAGS="-static -L$SYSROOT/lib"
    make -j"$JOBS"
    make install
}

# ── 7. Build libXfont2 ───────────────────────────────────────────────────────
build_libXfont2() {
    fetch_pkg "libXfont2" "2.0.6" "https://gitlab.freedesktop.org/xorg/lib/libxfont/-/archive/libXfont2-2.0.6/libxfont-libXfont2-2.0.6.tar.gz"
    echo ">>> Building libXfont2 ..."
    ./configure \
        --host=x86_64-linux-musl \
        --prefix="$SYSROOT" \
        --enable-static \
        --disable-shared \
        --without-fop \
        CFLAGS="-static -O3 -I$SYSROOT/include -I$SYSROOT/include/freetype2" \
        LDFLAGS="-static -L$SYSROOT/lib"
    make -j"$JOBS"
    make install
}

# ── 9. Build libxkbfile (XKB Support) ────────────────────────────────────────
build_libxkbfile() {
    fetch_pkg "libxkbfile" "1.1.2" "https://gitlab.freedesktop.org/xorg/lib/libxkbfile/-/archive/libxkbfile-1.1.2/libxkbfile-libxkbfile-1.1.2.tar.gz"
    echo ">>> Building libxkbfile ..."
    ./configure --prefix="$SYSROOT" --host="$HOST_TRIPLET" --enable-static --disable-shared
    make install
}

# ── 10. Build xkeyboard-config (XKB Keyboard Data) ────────────────────────────
build_xkeyboard_config() {
    fetch_pkg "xkeyboard-config" "2.41" "https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/archive/xkeyboard-config-2.41/xkeyboard-config-xkeyboard-config-2.41.tar.gz"
    echo ">>> Building xkeyboard-config ..."
    mkdir -p build-ascentos && cd build-ascentos
    meson setup .. \
        --cross-file "$BUILD_BASE/x86_64-ascentos.meson" \
        --prefix="$SYSROOT" \
        --buildtype=release
    ninja install
}

# ── Standardize Kernel Headers ───────────────────────────────────────────────
fix_kernel_headers() {
    echo ">>> Standardizing kernel headers for X11 ..."
    mkdir -p "$SYSROOT/include/linux"
    mkdir -p "$SYSROOT/include/asm"
    mkdir -p "$SYSROOT/include/asm-generic"
    
    # Copy host headers to sysroot to satisfy all transitive kernel dependencies
    cp -r /usr/include/linux/* "$SYSROOT/include/linux/" 2>/dev/null || true
    cp -r /usr/include/asm/* "$SYSROOT/include/asm/" 2>/dev/null || true
    cp -r /usr/include/asm-generic/* "$SYSROOT/include/asm-generic/" 2>/dev/null || true
}

# ── 12. Build X Server (Kdrive/TinyX) ────────────────────────────────────────
build_xserver() {
    fix_kernel_headers
    fetch_pkg "xorg-server" "1.19.7" "https://gitlab.freedesktop.org/xorg/xserver/-/archive/xorg-server-1.19.7/xserver-xorg-server-1.19.7.tar.gz"
    echo ">>> Generating configure script for xorg-server ..."
    
    # Switch to configure for Kdrive support which is missing in Meson 21.1.x
    echo ">>> Building X Server (Kdrive) ..."
    ./configure \
        --host="$HOST_TRIPLET" \
        --prefix="$SYSROOT" \
        --enable-kdrive \
        --enable-kdrive-fbdev \
        --enable-kdrive-evdev \
        --disable-xorg \
        --disable-xnest \
        --disable-xvfb \
        --disable-xwayland \
        --disable-xwin \
        --disable-xquartz \
        --disable-config-udev \
        --disable-config-hal \
        --disable-dri \
        --disable-dri2 \
        --disable-glx \
        --disable-libunwind \
        --disable-input-thread \
        --disable-xshmfence \
        --disable-systemd-logind \
        --with-systemd-daemon=no \
        --with-sha1=libmd \
        --with-xkb-path=/share/X11/xkb \
        --with-xkb-output=/tmp \
        --with-xkb-bin-directory=/bin \
        CFLAGS="-static -O3 -I$SYSROOT/include -DHAVE_CBRT -Wno-incompatible-pointer-types -Wno-array-bounds -D__uid_t=uid_t -D__gid_t=gid_t" \
        LDFLAGS="-static -L$SYSROOT/lib" \
        LIBS="-lpixman-1 -lfreetype -lfontenc -lz"

    make -j"$JOBS"
    make install
}

# ── 13. Build Basic Fonts ──────────────────────────────────────────────────
build_basic_fonts() {
    # Remove broken mkfontscale/mkfontdir that were built with target libs
    rm -f "$BUILD_BASE/host/bin/mkfontscale" "$BUILD_BASE/host/bin/mkfontdir"
    
    # We need at least the fixed font for Xfbdev to work
    fetch_pkg "font-misc-misc" "1.1.3" "https://www.x.org/pub/individual/font/font-misc-misc-1.1.3.tar.gz"
    echo ">>> Building basic fonts ..."
    ./configure --prefix="$SYSROOT" --host="$HOST_TRIPLET" \
        --with-fontdir="$SYSROOT/share/fonts/X11/misc"
    # The install-data-hook tries to run mkfontdir, skip it if broken
    make install || true
    # Run system mkfontdir if available
    if command -v mkfontdir >/dev/null 2>&1; then
        echo ">>> Running system mkfontdir ..."
        mkfontdir "$SYSROOT/share/fonts/X11/misc" 2>/dev/null || true
    fi
    # Create a minimal fonts.dir if missing (X11 can work without it in some cases)
    if [ ! -f "$SYSROOT/share/fonts/X11/misc/fonts.dir" ]; then
        echo ">>> Creating minimal fonts.dir ..."
        echo "2" > "$SYSROOT/share/fonts/X11/misc/fonts.dir"
        echo "fixed-misc.pcf.gz fixed" >> "$SYSROOT/share/fonts/X11/misc/fonts.dir"
        echo "6x13.pcf.gz fixed" >> "$SYSROOT/share/fonts/X11/misc/fonts.dir"
    fi
    # Create fonts.alias for common font aliases
    if [ ! -f "$SYSROOT/share/fonts/X11/misc/fonts.alias" ]; then
        echo ">>> Creating fonts.alias ..."
        cat > "$SYSROOT/share/fonts/X11/misc/fonts.alias" << 'EOF'
! Font aliases for Xfbdev
fixed           "-misc-fixed-medium-r-semicondensed--13-120-75-75-c-60-iso8859-1"
variable        "-misc-fixed-medium-r-normal--13-120-75-75-c-70-iso8859-1"
6x13            "-misc-fixed-medium-r-semicondensed--13-120-75-75-c-60-iso8859-1"
8x13            "-misc-fixed-medium-r-normal--13-120-75-75-c-80-iso8859-1"
9x15            "-misc-fixed-medium-r-normal--15-140-75-75-c-90-iso8859-1"
10x20           "-misc-fixed-medium-r-normal--20-200-75-75-c-100-iso8859-1"
EOF
    fi
}

# Helper: Switch to host compiler for building host tools
_start_host_build() {
    _SAVE_CC="$CC"
    _SAVE_CXX="$CXX"
    _SAVE_PKG_CONFIG_PATH="$PKG_CONFIG_PATH"
    _SAVE_PKG_CONFIG_LIBDIR="$PKG_CONFIG_LIBDIR"
    export CC="gcc"
    export CXX="g++"
    export PKG_CONFIG_PATH="$SYSROOT/lib/pkgconfig:$SYSROOT/share/pkgconfig"
    export PKG_CONFIG_LIBDIR="$SYSROOT/lib/pkgconfig:$SYSROOT/share/pkgconfig"
}

_end_host_build() {
    export CC="$_SAVE_CC"
    export CXX="$_SAVE_CXX"
    export PKG_CONFIG_PATH="$_SAVE_PKG_CONFIG_PATH"
    export PKG_CONFIG_LIBDIR="$_SAVE_PKG_CONFIG_LIBDIR"
}

# ── Host Tools (Early) - bdftopcf, font-util ───────────────────────────────────
build_host_tools_early() {
    echo ">>> Building early host tools (bdftopcf, font-util) ..."
    _start_host_build
    
    # bdftopcf - only needs xproto/fontsproto (already built)
    fetch_pkg "bdftopcf" "1.1.1" "https://www.x.org/pub/individual/util/bdftopcf-1.1.1.tar.gz"
    ./configure --prefix="$BUILD_BASE/host"
    make install
    
    # font-util (provides ucs2any) - no dependencies
    fetch_pkg "font-util" "1.4.1" "https://gitlab.freedesktop.org/xorg/font/util/-/archive/font-util-1.4.1/util-font-util-1.4.1.tar.gz"
    ./configure --prefix="$BUILD_BASE/host"
    make install
    
    _end_host_build
    export PATH="$BUILD_BASE/host/bin:$PATH"
}

# Note: mkfontscale is NOT built because it requires fontenc+freetype2 as HOST
# libraries, but we only have them built for the TARGET. Use system mkfontdir
# instead, or skip fonts.dir generation (not strictly required).

# ── Main ─────────────────────────────────────────────────────────────────────

echo "========================================"
echo "    AscentOS X11 Porting Workspace      "
echo "========================================"

mkdir -p "$BUILD_BASE"
prepare_meson_cross

# Build util-macros and xorgproto first (needed by host tools)
build_util_macros
build_xorgproto

# Build early host tools (bdftopcf, font-util) - only needs proto
build_host_tools_early

# Build dependencies in order
build_zlib
build_xtrans
build_libmd
build_libXau
build_libXdmcp
build_xcb_proto
build_libxcb
build_libX11
build_libXext
build_pixman
build_libxcvt
build_freetype
build_font_util
build_libfontenc

build_libXfont2
build_libxkbfile

# ── Build xkbcomp (Keyboard Compiler) ───────────────────────────────────
build_xkbcomp() {
    fetch_pkg "xkbcomp" "1.4.7" "https://www.x.org/pub/individual/app/xkbcomp-1.4.7.tar.gz"
    echo ">>> Building xkbcomp ..."
    ./configure \
        --host="$HOST_TRIPLET" \
        --prefix="$SYSROOT" \
        --enable-static \
        --disable-shared \
        CFLAGS="-static -O3 -I$SYSROOT/include" \
        LDFLAGS="-static -L$SYSROOT/lib" \
        LIBS="-lxcb -lXau -lXdmcp"
    make -j"$JOBS"
    make install
    # Also copy to userland/xkbcomp.elf as expected by GNUmakefile
    cp xkbcomp "$ROOT_DIR/userland/xkbcomp.elf"
}

# Build XKB keyboard data
build_xkeyboard_config

# Build fonts
build_basic_fonts

# Build xkbcomp
build_xkbcomp

# Finally the server
build_xserver

echo "========================================"
echo "X11 (Xfbdev) built successfully!"
echo "Find it at: $SYSROOT/bin/Xfbdev"
echo "========================================"
