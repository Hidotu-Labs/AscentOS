#!/bin/sh
# AvoryOS: bootstrap x86_64-linux-musl (musl-cross-make) if needed, then install
# musl 1.2.5 into a static sysroot under toolchain/musl-sysroot.
#
# Environment (optional):
#   MUSL_TOOLCHAIN_DIR  — where gcc lands (default: $ROOT/toolchain/x86_64-linux-musl)
#   MUSL_SYSROOT        — musl install prefix (default: $ROOT/toolchain/musl-sysroot)
#   MUSL_SKIP_BOOTSTRAP — if set, never run musl-cross-make (fail if no usable CC)
#
# Usage:
#   ./scripts/musl-toolchain.sh          # ensure compiler + build/install sysroot
#   ./scripts/musl-toolchain.sh bootstrap # only musl-cross-make install
#   ./scripts/musl-toolchain.sh sysroot   # only musl libc (needs CC on PATH or under toolchain)

set -e

MUSL_VERSION=1.2.5
MUSL_TARBALL="musl-${MUSL_VERSION}.tar.gz"
MUSL_URL="https://musl.libc.org/releases/${MUSL_TARBALL}"
TARGET=x86_64-linux-musl

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
OUTPUT=${MUSL_TOOLCHAIN_DIR:-"$ROOT_DIR/toolchain/x86_64-linux-musl"}
PREFIX=${MUSL_SYSROOT:-"$ROOT_DIR/toolchain/musl-sysroot"}
BUILD_DIR=${BUILD_DIR:-"$ROOT_DIR/build/musl-${MUSL_VERSION}"}
MUSL_CROSS_MAKE_DIR=${MUSL_CROSS_MAKE_DIR:-"$ROOT_DIR/build/musl-cross-make"}
JOBS=$(nproc 2>/dev/null || echo 4)
LOCAL_CROSS_GCC="$OUTPUT/bin/x86_64-linux-musl-gcc"

bootstrap_toolchain() {
	if [ -n "${MUSL_SKIP_BOOTSTRAP:-}" ]; then
		echo "MUSL_SKIP_BOOTSTRAP is set but no cross compiler found." >&2
		exit 1
	fi
	mkdir -p "$(dirname "$MUSL_CROSS_MAKE_DIR")"
	if [ ! -f "$MUSL_CROSS_MAKE_DIR/Makefile" ]; then
		echo "Cloning musl-cross-make -> $MUSL_CROSS_MAKE_DIR"
		rm -rf "$MUSL_CROSS_MAKE_DIR"
		git clone --depth=1 https://github.com/richfelker/musl-cross-make.git \
			"$MUSL_CROSS_MAKE_DIR"
	fi
	cd "$MUSL_CROSS_MAKE_DIR"

	# Add patch for GCC 16 compatibility with safe-ctype.h
	mkdir -p patches/gcc-13.3.0
	cat > patches/gcc-13.3.0/0010-gcc16-locale-collision.diff << 'EOF'
--- a/gcc/system.h
+++ b/gcc/system.h
@@ -218,10 +218,11 @@
 #ifdef INCLUDE_ARRAY
 # include <array>
 #endif
 #ifdef INCLUDE_FUNCTIONAL
 # include <functional>
 #endif
+# include <locale>
 # include <cstring>
 # include <initializer_list>
 # include <new>
 # include <utility>
 # include <type_traits>
EOF

	# Copy or generate config.mak with C++ support
	CONFIG_MAK="$ROOT_DIR/toolchain/musl-cross-make-config.mak"
	if [ -f "$CONFIG_MAK" ]; then
		cp "$CONFIG_MAK" config.mak
		echo "Using provided config.mak"
	else
		echo "Generating default config.mak with C++ support..."
		cat > config.mak << EOF
TARGET = $TARGET
OUTPUT = $OUTPUT
GCC_VER = 13.3.0
BINUTILS_VER = 2.44
LANGUAGES = c c++
COMMON_CONFIG += --disable-nls
export CFLAGS += -fno-char8_t
export CXXFLAGS += -fno-char8_t -include locale
export CFLAGS_FOR_BUILD += -fno-char8_t
export CXXFLAGS_FOR_BUILD += -fno-char8_t -include locale
GCC_CONFIG += --disable-libquadmath --disable-decimal-float
GCC_CONFIG += --disable-libitm --disable-fixed-point
EOF
	fi
	echo "Building $TARGET -> $OUTPUT (this can take a long time) ..."
	make -j"$JOBS" CC=gcc install
	cd "$ROOT_DIR"
}

pick_compiler() {
	CC=${CC:-}
	CXX=${CXX:-}
	CC_BIN=""
	
	# If we have a local cross-compiler, prefer it
	if [ -z "$CC" ] && [ -x "$LOCAL_CROSS_GCC" ]; then
		CC="$LOCAL_CROSS_GCC"
	fi
	if [ -z "$CXX" ] && [ -x "${LOCAL_CROSS_GCC%gcc}g++" ]; then
		CXX="${LOCAL_CROSS_GCC%gcc}g++"
	fi

	# Otherwise check for system cross-compiler
	if [ -z "$CC" ] && command -v x86_64-linux-musl-gcc >/dev/null 2>&1; then
		CC=x86_64-linux-musl-gcc
	fi
	if [ -z "$CXX" ] && command -v x86_64-linux-musl-g++ >/dev/null 2>&1; then
		CXX=x86_64-linux-musl-g++
	fi
	
	# Fallback to musl-gcc wrapper (C only, will cause fail later if C++ needed)
	if [ -z "$CC" ] && command -v musl-gcc >/dev/null 2>&1; then
		CC=musl-gcc
	fi

	if [ -z "$CC" ] || [ -z "$CXX" ]; then
		return 1
	fi

	CC_BIN=$(printf '%s\n' "$CC" | awk '{print $1}')
	if [ ! -x "$CC_BIN" ] && ! command -v "$CC_BIN" >/dev/null 2>&1; then
		return 1
	fi
	
	CXX_BIN=$(printf '%s\n' "$CXX" | awk '{print $1}')
	if [ ! -x "$CXX_BIN" ] && ! command -v "$CXX_BIN" >/dev/null 2>&1; then
		return 1
	fi

	return 0
}

ensure_compiler() {
	if pick_compiler; then
		return 0
	fi
	echo "No x86_64-linux-musl-gcc or musl-gcc found; bootstrapping toolchain ..."
	bootstrap_toolchain
	export PATH="$OUTPUT/bin:$PATH"
	CC="$LOCAL_CROSS_GCC"
	CC_BIN="$LOCAL_CROSS_GCC"
	if [ ! -x "$CC_BIN" ]; then
		echo "Bootstrap failed: missing $LOCAL_CROSS_GCC" >&2
		exit 1
	fi
}

build_sysroot() {
	ensure_compiler

	echo "Using CC=$CC"

	HOST_TRIPLET=${HOST_TRIPLET:-}
	if [ -z "$HOST_TRIPLET" ]; then
		HOST_TRIPLET=$($CC_BIN -dumpmachine 2>/dev/null) || true
	fi
	if [ -z "$HOST_TRIPLET" ]; then
		HOST_TRIPLET=x86_64-linux-musl
	fi
	echo "Using --host=$HOST_TRIPLET"

	AR=${AR:-}
	RANLIB=${RANLIB:-}
	case "$CC_BIN" in
	*-gcc)
		_tpref="${CC_BIN%gcc}"
		if [ -z "$AR" ] && command -v "${_tpref}ar" >/dev/null 2>&1; then
			AR=${_tpref}ar
		fi
		if [ -z "$RANLIB" ] && command -v "${_tpref}ranlib" >/dev/null 2>&1; then
			RANLIB=${_tpref}ranlib
		fi
		;;
	esac
	AR=${AR:-ar}
	RANLIB=${RANLIB:-ranlib}
	export AR RANLIB
	echo "Using AR=$AR RANLIB=$RANLIB"

	mkdir -p "$BUILD_DIR"
	cd "$BUILD_DIR"
	if [ ! -f "$MUSL_TARBALL" ]; then
		echo "Downloading $MUSL_URL ..."
		curl -L -o "$MUSL_TARBALL" "$MUSL_URL"
	fi
	rm -rf "musl-${MUSL_VERSION}"
	tar xf "$MUSL_TARBALL"
	cd "musl-${MUSL_VERSION}"

	./configure \
		--prefix="$PREFIX" \
		--enable-shared \
		--enable-static \
		--host="$HOST_TRIPLET" \
		AR="$AR" \
		RANLIB="$RANLIB" \
		CFLAGS="-O2 -fno-PIC -fno-pie"

	make -j"$JOBS"
	make install

	# Create stub linux/vt.h for programs that need it (e.g., nano)
	mkdir -p "$PREFIX/include/linux"
	cat > "$PREFIX/include/linux/vt.h" << 'VT_H_EOF'
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

	cd "$ROOT_DIR"
	echo "musl ${MUSL_VERSION} installed to: $PREFIX"
}

case "${1:-all}" in
bootstrap)
	bootstrap_toolchain
	echo "Toolchain installed under: $OUTPUT"
	echo "Add to PATH: export PATH=\"$OUTPUT/bin:\$PATH\""
	;;
sysroot)
	build_sysroot
	;;
all | "")
	if [ -x "$LOCAL_CROSS_GCC" ]; then
		export PATH="$OUTPUT/bin:$PATH"
	fi
	build_sysroot
	;;
*)
	echo "Usage: $0 [all|bootstrap|sysroot]" >&2
	exit 1
	;;
esac
