#!/bin/sh
# AscentOS: Build GNU Bash 5.3 for AscentOS
#
# This script cross-compiles Bash using the glibc toolchain. Bash's bundled
# allocator remains enabled; glibc provides the sbrk compatibility it needs.
#
# Prerequisites:
#   - Run scripts/glibc-toolchain.sh first
#
# Usage:
#   ./scripts/build-bash.sh          # Build and install Bash
#   ./scripts/build-bash.sh clean    # Clean build directory

set -e

BASH_VERSION_NUM=5.3
BASH_TARBALL="bash-${BASH_VERSION_NUM}.tar.gz"
BASH_URL="https://ftp.gnu.org/gnu/bash/${BASH_TARBALL}"

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT_DIR/build/bash-${BASH_VERSION_NUM}-glibc"}
PREFIX=${GLIBC_SYSROOT:-"$ROOT_DIR/toolchain/glibc-sysroot"}
BASH_INSTALL="${PREFIX}/opt/bash"
JOBS=$(nproc 2>/dev/null || echo 4)

# Find glibc compiler
find_compiler() {
    LOCAL_CC="$ROOT_DIR/toolchain/x86_64-linux-glibc/bin/x86_64-buildroot-linux-gnu-gcc"

    if [ -x "$LOCAL_CC" ]; then
        CC="$LOCAL_CC"
    elif command -v x86_64-buildroot-linux-gnu-gcc >/dev/null 2>&1; then
        CC="x86_64-buildroot-linux-gnu-gcc"
    else
        echo "Error: No glibc compiler found. Run scripts/glibc-toolchain.sh first." >&2
        exit 1
    fi

    echo "Using CC=$CC"
}

# Clean build directory
do_clean() {
    echo "Cleaning $BUILD_DIR ..."
    rm -rf "$BUILD_DIR"
    rm -rf "$BASH_INSTALL"
    echo "Done."
}

# Build Bash
build_bash() {
    find_compiler

    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"

    # Download Bash source
    if [ ! -f "$BASH_TARBALL" ]; then
        CACHED_TARBALL="$ROOT_DIR/build/bash-${BASH_VERSION_NUM}/$BASH_TARBALL"
        if [ -f "$CACHED_TARBALL" ]; then
            cp "$CACHED_TARBALL" "$BASH_TARBALL"
        else
            echo "Downloading $BASH_URL ..."
            curl -L -o "$BASH_TARBALL" "$BASH_URL"
        fi
    fi

    # Extract
    if [ ! -d "bash-${BASH_VERSION_NUM}" ]; then
        echo "Extracting $BASH_TARBALL ..."
        tar xf "$BASH_TARBALL"
    elif [ ! -f "bash-${BASH_VERSION_NUM}/configure" ]; then
        echo "Source tree exists but is incomplete; re-extracting..."
        rm -rf "bash-${BASH_VERSION_NUM}"
        tar xf "$BASH_TARBALL"
    fi

    cd "bash-${BASH_VERSION_NUM}"

    # Set up compiler tools — check if cross-prefixed tools exist
    case "$CC" in
        *-gcc)
            _prefix="${CC%gcc}"
            if command -v "${_prefix}ar" >/dev/null 2>&1; then
                AR="${_prefix}ar"
            else
                AR="ar"
            fi
            if command -v "${_prefix}ranlib" >/dev/null 2>&1; then
                RANLIB="${_prefix}ranlib"
            else
                RANLIB="ranlib"
            fi
            if command -v "${_prefix}strip" >/dev/null 2>&1; then
                STRIP="${_prefix}strip"
            else
                STRIP="strip"
            fi
            ;;
        *)
            AR="ar"
            RANLIB="ranlib"
            STRIP="strip"
            ;;
    esac

    export CC AR RANLIB

    echo "Using CC=$CC AR=$AR RANLIB=$RANLIB"

    echo "Configuring Bash for AscentOS ..."

    # Cross-compile cache variables to avoid configure test failures
    # These tell configure the results of runtime tests that can't run
    # during cross-compilation.
    export bash_cv_func_sigsetjmp=present
    export bash_cv_func_strcoll_broken=no
    export bash_cv_func_ctype_nonascii=no
    export bash_cv_dup2_broken=no
    export bash_cv_pgrp_pipe=no
    export bash_cv_sys_siglist=yes
    export bash_cv_under_sys_siglist=yes
    export bash_cv_opendir_not_robust=no
    export bash_cv_ulimit_maxfds=yes
    export bash_cv_getenv_redef=yes
    export bash_cv_getcwd_malloc=yes
    export bash_cv_func_snprintf=yes
    export bash_cv_printf_a_format=yes
    export bash_cv_unusedvar=yes
    export ac_cv_func_mmap_fixed_mapped=yes
    export ac_cv_func_setvbuf_reversed=no
    export ac_cv_rl_version=8.2
    export bash_cv_job_control_missing=present
    export bash_cv_sys_named_pipes=present
    export bash_cv_wcwidth_broken=no
    export bash_cv_func_lstat=yes

    ./configure \
        --host=x86_64-buildroot-linux-gnu \
        --prefix=/opt/bash \
        --disable-nls \
        --disable-progcomp \
        --disable-net-redirections \
        CFLAGS="-O2 -fno-stack-protector --sysroot=$PREFIX" \
        LDFLAGS="--sysroot=$PREFIX -L$PREFIX/lib -L$PREFIX/usr/lib"

    echo "Building Bash (this may take a while) ..."
    make -j"$JOBS"

    # Install
    echo "Installing Bash to $BASH_INSTALL ..."
    mkdir -p "$BASH_INSTALL/bin"
    cp bash "$BASH_INSTALL/bin/"

    # Strip the binary
    if command -v "$STRIP" >/dev/null 2>&1; then
        echo "Stripping bash binary ..."
        "$STRIP" "$BASH_INSTALL/bin/bash"
    fi

    # Create a default .bashrc with the AscentOS prompt
    mkdir -p "$BASH_INSTALL/etc"
    cat > "$BASH_INSTALL/etc/bashrc" << 'BASHRC_EOF'
# AscentOS default bashrc
export PS1='\033[0;32mRoot@AscentOS\033[0m:\w\$ '
export PATH='/opt/coreutils/bin:/opt/bash/bin:/opt/tcc/bin:/'
BASHRC_EOF

    cd "$ROOT_DIR"

    echo ""
    echo "========================================"
    echo "Bash ${BASH_VERSION_NUM} built successfully!"
    echo "========================================"
    echo ""
    echo "Binary: $BASH_INSTALL/bin/bash"
    echo ""
    echo "To test on AscentOS, run: bash.elf"
    echo ""
}

case "${1:-build}" in
    clean)
        do_clean
        ;;
    build|"")
        build_bash
        ;;
    *)
        echo "Usage: $0 [build|clean]" >&2
        exit 1
        ;;
esac
