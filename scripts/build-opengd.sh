#!/bin/sh
# AscentOS: Build OpenGD for AscentOS
#
# OpenGD is an open-source re-implementation of Geometry Dash, powered by the
# axmol engine (a fork of cocos2d-x 4.0).  The game requires proprietary
# Geometry Dash assets (Resources/) which are NOT fetched here — copy them
# from a legal GD 2.1/2.2 install into userland/opengd/Resources/ after the
# first build.
#
# Graphics : Mesa llvmpipe (software OpenGL) via Alpine rootfs libGL.so
# Windowing: X11 via Alpine rootfs libX11 / libXi / libXcursor / libXrandr
# Audio    : openal-soft OSS backend → /dev/dsp  (no ALSA/PipeWire needed)
# libc     : glibc (Alpine rootfs) — axmol requires C++ exceptions + RTTI
# Compiler : x86_64-buildroot-linux-gnu-g++ from the glibc toolchain
#
# Usage:
#   ./scripts/build-opengd.sh              # clone + build
#   ./scripts/build-opengd.sh build        # build only (sources already present)
#   ./scripts/build-opengd.sh clean        # wipe build tree and output dir

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT_DIR/build/opengd"}
SOURCE_DIR="$BUILD_DIR/OpenGD"
AXMOL_DIR="$SOURCE_DIR/axmol"
OUTPUT_DIR="$ROOT_DIR/userland/opengd"
ALPINE_ROOTFS=${ALPINE_ROOTFS:-"$ROOT_DIR/build/alpine/rootfs"}
GLIBC_TOOLCHAIN_BIN="$ROOT_DIR/toolchain/x86_64-linux-glibc/bin"
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 4)}

OPENGD_REPO_URL=${OPENGD_REPO_URL:-"https://github.com/Open-GD/OpenGD.git"}
AXMOL_REPO_URL=${AXMOL_REPO_URL:-"https://github.com/axmolengine/axmol.git"}

# Pin to the last commit where OpenGD was known to build cleanly.
# Override with OPENGD_COMMIT= to try HEAD.
OPENGD_COMMIT=${OPENGD_COMMIT:-""}
# axmol tag/branch compatible with OpenGD (dev branch tracks closely).
AXMOL_REF=${AXMOL_REF:-"dev"}

# ── Compiler discovery ───────────────────────────────────────────────────────

find_compiler() {
    local_cxx="$GLIBC_TOOLCHAIN_BIN/x86_64-buildroot-linux-gnu-g++"
    local_cc="$GLIBC_TOOLCHAIN_BIN/x86_64-buildroot-linux-gnu-gcc"

    if [ -x "$local_cxx" ]; then
        CXX="$local_cxx"
        CC="$local_cc"
    elif command -v x86_64-buildroot-linux-gnu-g++ >/dev/null 2>&1; then
        CXX=x86_64-buildroot-linux-gnu-g++
        CC=x86_64-buildroot-linux-gnu-gcc
    elif command -v g++ >/dev/null 2>&1; then
        # Last resort: host g++ (works if host is x86-64 Linux with glibc)
        CXX=g++
        CC=gcc
        echo "[!] Warning: glibc cross-compiler not found; falling back to host g++"
        echo "    Run scripts/glibc-toolchain.sh to build the cross-compiler."
    else
        echo "Error: no suitable C++ compiler found." >&2
        echo "       Run scripts/glibc-toolchain.sh first." >&2
        exit 1
    fi
    echo "[*] Using CXX=$CXX"
}

# ── Dependency checks ────────────────────────────────────────────────────────

check_dependencies() {
    echo "[*] Checking Alpine rootfs dependencies..."
    missing=0
    for f in \
        "$ALPINE_ROOTFS/usr/lib/libGL.so" \
        "$ALPINE_ROOTFS/usr/include/GL/gl.h" \
        "$ALPINE_ROOTFS/usr/lib/libX11.so" \
        "$ALPINE_ROOTFS/usr/include/X11/Xlib.h" \
        "$ALPINE_ROOTFS/usr/lib/libfontconfig.so" \
        "$ALPINE_ROOTFS/usr/include/fontconfig/fontconfig.h" \
        "$ALPINE_ROOTFS/usr/lib/libz.so"; do
        # Note: libpthread is merged into libc on Alpine/musl; libpthread.a
        # is a stub archive but there is no separate libpthread.so — skip it.
        if [ ! -e "$f" ]; then
            echo "    Missing: $f" >&2
            missing=$((missing + 1))
        fi
    done
    if [ "$missing" -gt 0 ]; then
        echo "Error: $missing required file(s) missing from Alpine rootfs." >&2
        echo "       Run scripts/setup-alpine.sh first." >&2
        exit 1
    fi

    for tool in git cmake ninja; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            echo "Error: '$tool' not found on PATH; install it before building." >&2
            exit 1
        fi
    done
    echo "[+] Dependencies OK"
}

# ── Source fetching ──────────────────────────────────────────────────────────

fetch_source() {
    mkdir -p "$BUILD_DIR"

    # Clone OpenGD
    if [ ! -d "$SOURCE_DIR/.git" ]; then
        echo "[*] Cloning OpenGD..."
        git clone --depth=50 "$OPENGD_REPO_URL" "$SOURCE_DIR"
        if [ -n "$OPENGD_COMMIT" ]; then
            git -C "$SOURCE_DIR" checkout "$OPENGD_COMMIT"
        fi
    else
        echo "[*] Using existing OpenGD source at $SOURCE_DIR"
    fi

    # Clone axmol as an embedded subdirectory so OpenGD's CMakeLists finds it
    # at ./axmol without needing AX_ROOT set.
    if [ ! -d "$AXMOL_DIR/.git" ]; then
        echo "[*] Cloning axmol ($AXMOL_REF) into $AXMOL_DIR..."
        git clone --depth=1 --branch "$AXMOL_REF" "$AXMOL_REPO_URL" "$AXMOL_DIR"
    else
        echo "[*] Using existing axmol source at $AXMOL_DIR"
    fi
}

# ── Source patches ───────────────────────────────────────────────────────────

patch_source() {
    echo "[*] Applying AscentOS patches..."

    # ── 1. Patch axmol's CMakeLists to inject the OSS cache-init file ──────
    #   axmol vendors openal-soft under thirdparty/openal/.  We append a
    #   block that pre-loads our cache-init before openal's CMakeLists runs.
    openal_cmake="$AXMOL_DIR/thirdparty/openal/CMakeLists.txt"
    oss_cmake="$ROOT_DIR/scripts/opengd-openal-oss.cmake"

    if [ -f "$openal_cmake" ] && ! grep -q "AscentOS OSS audio" "$openal_cmake"; then
        echo "[*]   Patching axmol openal CMakeLists for OSS backend..."
        # Prepend the include() before the first project() or cmake_minimum_required.
        # We use a sentinel comment and include() at the top of the file.
        tmp="$openal_cmake.ascentos.tmp"
        printf '# AscentOS OSS audio: pre-load backend selection before openal configure\n' > "$tmp"
        printf 'include("%s" OPTIONAL)\n' "$oss_cmake" >> "$tmp"
        cat "$openal_cmake" >> "$tmp"
        mv "$tmp" "$openal_cmake"
    fi

    # ── 2. Axmol looks for <sys/soundcard.h> via its bundled FindOSS.cmake.
    #   The Alpine sysroot has it but cmake's sysroot search may miss it
    #   when CMAKE_SYSROOT is set.  Ensure the include path is present.
    find_oss_cmake="$AXMOL_DIR/thirdparty/openal/cmake/FindOSS.cmake"
    if [ -f "$find_oss_cmake" ] && ! grep -q "ASCENTOS_EXTRA_SOUNDCARD_PATH" "$find_oss_cmake"; then
        echo "[*]   Patching FindOSS.cmake to search Alpine sysroot..."
        sed -i "s|find_path(OSS_INCLUDE_DIRS|find_path(OSS_INCLUDE_DIRS\n    PATHS \"${ALPINE_ROOTFS}/usr/include\" \"${ALPINE_ROOTFS}/include\"|" \
            "$find_oss_cmake" || true
        # Mark patched
        echo "# ASCENTOS_EXTRA_SOUNDCARD_PATH patched" >> "$find_oss_cmake"
    fi

    # ── 3. Disable optional features that need unavailable libs ────────────
    opengd_cmake="$SOURCE_DIR/CMakeLists.txt"
    if ! grep -q "AX_ENABLE_WEBSOCKET.*OFF" "$opengd_cmake"; then
        echo "[*]   Disabling websocket / vlc / webkit in OpenGD CMakeLists..."
        sed -i 's/SET(AX_ENABLE_WEBSOCKET ON)/SET(AX_ENABLE_WEBSOCKET OFF)/' \
            "$opengd_cmake" || true
    fi

    echo "[+] Patches applied"
}

# ── axslcc download ──────────────────────────────────────────────────────────

fetch_axslcc() {
    axslcc_bin_dir="$AXMOL_DIR/tools/external/axslcc/bin"
    axslcc_exe="$axslcc_bin_dir/axslcc"

    if [ -x "$axslcc_exe" ]; then
        echo "[*] axslcc already present at $axslcc_exe"
        return 0
    fi

    # Read the required version from axmol's own manifest
    profiles="$AXMOL_DIR/1k/build.profiles"
    if [ ! -f "$profiles" ]; then
        echo "Error: cannot find $profiles to determine axslcc version." >&2
        exit 1
    fi
    axslcc_ver=$(grep '^axslcc=' "$profiles" | cut -d= -f2 | tr -d '[:space:]')
    if [ -z "$axslcc_ver" ]; then
        echo "Error: axslcc version not found in $profiles." >&2
        exit 1
    fi

    pkg="axslcc-${axslcc_ver}-linux-x64.tar.gz"
    url="https://github.com/axmolengine/axslcc/releases/download/v${axslcc_ver}/${pkg}"
    download_dir="$BUILD_DIR/axslcc-dl"
    mkdir -p "$download_dir" "$axslcc_bin_dir"

    if [ ! -f "$download_dir/$pkg" ]; then
        echo "[*] Downloading axslcc ${axslcc_ver} from GitHub..."
        curl -fL --retry 3 -o "$download_dir/$pkg.part" "$url"
        mv "$download_dir/$pkg.part" "$download_dir/$pkg"
    fi

    echo "[*] Extracting axslcc..."
    tar -xzf "$download_dir/$pkg" -C "$download_dir"

    # The tarball layout is flat: axslcc-<ver>-linux-x64/axslcc (binary)
    # Find and install it.
    extracted_bin=$(find "$download_dir" -name "axslcc" -type f ! -name "*.tar.gz" | head -1)
    if [ -z "$extracted_bin" ]; then
        echo "Error: axslcc binary not found after extraction." >&2
        exit 1
    fi
    cp "$extracted_bin" "$axslcc_exe"
    chmod +x "$axslcc_exe"
    echo "[+] axslcc ${axslcc_ver} installed at $axslcc_exe"
}

# ── Build ────────────────────────────────────────────────────────────────────

build_opengd() {
    find_compiler
    check_dependencies
    fetch_source
    patch_source

    # ── Fetch axslcc (axmol's host-side shader compiler) ────────────────────
    # axmol's AXSLCC.cmake does find_program(axslcc) and hard-errors if not
    # found.  setup.ps1 normally downloads it; we do it here instead.
    # Version is read from axmol/1k/build.profiles so it stays in sync with
    # whatever axmol revision was cloned.
    fetch_axslcc

    # ── Ensure OSS header is present in the sysroot ─────────────────────────
    # The Alpine rootfs has libasound.so but no sound headers at all.
    # openal-soft's FindOSS.cmake looks for <sys/soundcard.h>; copy it from
    # the build host so cmake can detect the OSS backend.
    oss_header_dst="$ALPINE_ROOTFS/usr/include/sys/soundcard.h"
    if [ ! -f "$oss_header_dst" ]; then
        for candidate in \
            /usr/include/sys/soundcard.h \
            /usr/include/linux/soundcard.h; do
            if [ -f "$candidate" ]; then
                echo "[*] Installing OSS header from host: $candidate -> $oss_header_dst"
                cp "$candidate" "$oss_header_dst"
                break
            fi
        done
        if [ ! -f "$oss_header_dst" ]; then
            echo "Error: sys/soundcard.h not found on host or in sysroot." >&2
            echo "       Install it with: sudo pacman -S linux-headers  (or equivalent)" >&2
            exit 1
        fi
    fi

    cmake_build_dir="$BUILD_DIR/cmake-build"

    # Wipe the cmake cache if the cache-init file has changed since the last
    # configure.  This prevents a stale CMakeCache.txt from shadowing the new
    # AXSLCC_FIND_PROG_ROOT value on incremental runs.
    cmake_cache="$cmake_build_dir/CMakeCache.txt"
    if [ -f "$cmake_cache" ]; then
        cached_init=$(grep "^CMAKE_CACHE_MAJOR_VERSION\|AXSLCC_FIND_PROG_ROOT" \
            "$cmake_cache" 2>/dev/null | grep "AXSLCC" | head -1 | cut -d= -f2)
        expected_path="$AXMOL_DIR/tools/external/axslcc/bin"
        if [ "$cached_init" != "$expected_path" ]; then
            echo "[*] axslcc path changed in cache — wiping cmake-build for fresh configure"
            rm -rf "$cmake_build_dir"
        fi
    fi
    mkdir -p "$cmake_build_dir"

    # Generate a patched cache-init file with the real axslcc path substituted.
    # The template uses @AXSLCC_BIN_DIR@ as a placeholder so the .cmake file
    # stays path-independent in the repo.
    oss_cmake_template="$ROOT_DIR/scripts/opengd-openal-oss.cmake"
    oss_cmake_patched="$BUILD_DIR/opengd-openal-oss-patched.cmake"
    sed "s|@AXSLCC_BIN_DIR@|$AXMOL_DIR/tools/external/axslcc/bin|g" \
        "$oss_cmake_template" > "$oss_cmake_patched"
    echo "[*] Cache-init: $oss_cmake_patched"
    echo "[*] axslcc path in cache-init: $(grep AXSLCC_FIND_PROG_ROOT "$oss_cmake_patched" | head -1)"

    # ── Sysroot rpath magic ─────────────────────────────────────────────────
    # The binary runs on AscentOS where Alpine libs live at /usr/lib (the
    # Alpine rootfs is merged into / on the disk image).  We set rpath to
    # /usr/lib:/lib so dlopen and direct symbol resolution work at runtime.
    RPATH_FLAGS="-Wl,-rpath,/usr/lib:/lib"
    RPATH_LINK="-Wl,-rpath-link,${ALPINE_ROOTFS}/usr/lib -Wl,-rpath-link,${ALPINE_ROOTFS}/lib"
    LINKER_FLAGS="$RPATH_LINK $RPATH_FLAGS -Wl,--allow-shlib-undefined"

    echo "[*] Configuring OpenGD with CMake (OSS audio, Mesa GL, X11)..."
    cmake -S "$SOURCE_DIR" -B "$cmake_build_dir" \
        -G Ninja \
        -C "$oss_cmake_patched" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER="$CC" \
        -DCMAKE_CXX_COMPILER="$CXX" \
        -DCMAKE_SYSROOT="$ALPINE_ROOTFS" \
        -DCMAKE_FIND_ROOT_PATH="$ALPINE_ROOTFS" \
        -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
        -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=BOTH \
        -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH \
        -DCMAKE_EXE_LINKER_FLAGS="$LINKER_FLAGS" \
        -DCMAKE_SHARED_LINKER_FLAGS="$LINKER_FLAGS" \
        -DCMAKE_PREFIX_PATH="$ALPINE_ROOTFS/usr;$ALPINE_ROOTFS" \
        -DCMAKE_INSTALL_PREFIX="$OUTPUT_DIR" \
        \
        -DAXSLCC_FIND_PROG_ROOT="$AXMOL_DIR/tools/external/axslcc/bin" \
        -DALSOFT_BACKEND_OSS=ON \
        -DALSOFT_BACKEND_ALSA=OFF \
        -DALSOFT_BACKEND_PIPEWIRE=OFF \
        -DALSOFT_BACKEND_PULSEAUDIO=OFF \
        \
        -DAX_ENABLE_VLC_MEDIA=OFF \
        -DAX_ENABLE_MFMEDIA=OFF \
        -DAX_ENABLE_MSEDGE_WEBVIEW2=OFF \
        -DAX_ENABLE_WEBSOCKET=OFF \
        -DAX_ENABLE_3D=OFF \
        -DAX_ENABLE_3D_PHYSICS=OFF \
        -DAX_ENABLE_NAVMESH=OFF \
        \
        -DAX_ENABLE_EXT_IMGUI=ON \
        -DAX_ENABLE_EXT_GUI=ON \
        -DAX_ENABLE_EXT_INSPECTOR=ON \
        -DAX_ENABLE_EXT_ASSETMANAGER=OFF \
        -DAX_ENABLE_EXT_PARTICLE3D=OFF \
        -DAX_ENABLE_EXT_PHYSICS_NODE=OFF \
        -DAX_ENABLE_EXT_SPINE=OFF \
        -DAX_ENABLE_EXT_DRAGONBONES=OFF \
        -DAX_ENABLE_EXT_FAIRYGUI=OFF \
        -DAX_ENABLE_EXT_COCOSTUDIO=OFF \
        -DAX_ENABLE_EXT_LIVE2D=OFF \
        -DAX_ENABLE_EXT_EFFEKSEER=OFF \
        -DAX_ENABLE_EXT_LUA=OFF \
        -DAX_ENABLE_EXT_DRAWNODEEX=OFF \
        -DAX_ENABLE_EXT_SDFGEN=OFF \
        \
        -DCMAKE_C_FLAGS="-fno-stack-protector -O2" \
        -DCMAKE_CXX_FLAGS="-fno-stack-protector -O2 -std=c++20"

    echo "[*] Building OpenGD (this will take a while)..."
    cmake --build "$cmake_build_dir" --parallel "$JOBS"

    install_opengd "$cmake_build_dir"
}

# ── Install ──────────────────────────────────────────────────────────────────

install_opengd() {
    cmake_build_dir="$1"
    mkdir -p "$OUTPUT_DIR"

    # Find the produced binary — CMake may name it OpenGD or opengd
    binary=""
    for candidate in \
        "$cmake_build_dir/OpenGD" \
        "$cmake_build_dir/opengd" \
        "$cmake_build_dir/bin/OpenGD" \
        "$cmake_build_dir/bin/opengd"; do
        if [ -x "$candidate" ]; then
            binary="$candidate"
            break
        fi
    done

    if [ -z "$binary" ]; then
        echo "Error: built binary not found under $cmake_build_dir" >&2
        echo "       Contents:" >&2
        find "$cmake_build_dir" -maxdepth 3 -type f -name "OpenGD*" -o -name "opengd*" 2>/dev/null >&2 || true
        exit 1
    fi

    cp "$binary" "$OUTPUT_DIR/OpenGD"
    chmod +x "$OUTPUT_DIR/OpenGD"

    # Copy the Content/ directory (shaders, fonts, UI assets bundled with OpenGD)
    if [ -d "$SOURCE_DIR/Content" ]; then
        echo "[*] Copying Content/ assets..."
        cp -r "$SOURCE_DIR/Content" "$OUTPUT_DIR/Content"
    fi

    # Write a launcher wrapper that sets up the runtime library path and
    # working directory before exec-ing the binary.  This mirrors the pattern
    # used by userland/quake2-launch.sh.
    launcher="$ROOT_DIR/userland/opengd-launch.sh"
    cat > "$launcher" <<'LAUNCHER_EOF'
#!/bin/sh
# OpenGD launcher — sets up library path and working directory.
# Place Geometry Dash Resources/ alongside this script at /opt/opengd/Resources/
export LD_LIBRARY_PATH=/usr/lib:/lib:${LD_LIBRARY_PATH:-}
export LIBGL_ALWAYS_SOFTWARE=1
export GALLIUM_DRIVER=llvmpipe
export GBM_ALWAYS_SOFTWARE=1
# Disable AT-SPI bridge (no D-Bus on base AscentOS)
export NO_AT_BRIDGE=1
export GTK_A11Y=none
cd /opt/opengd
exec ./OpenGD "$@"
LAUNCHER_EOF
    chmod +x "$launcher"

    echo ""
    echo "[+] OpenGD installed to $OUTPUT_DIR"
    echo ""
    echo "    *** ACTION REQUIRED: game assets not included ***"
    echo "    Copy the Resources/ folder from a legal Geometry Dash 2.1/2.2"
    echo "    installation into:"
    echo "        $OUTPUT_DIR/Resources/"
    echo ""
    echo "    On the running AscentOS image the game is at /opt/opengd."
    echo "    Launch via:  /usr/bin/opengd  (or click the desktop launcher)"
    echo ""
}

# ── Clean ────────────────────────────────────────────────────────────────────

do_clean() {
    echo "[*] Cleaning $BUILD_DIR and $OUTPUT_DIR..."
    rm -rf "$BUILD_DIR" "$OUTPUT_DIR"
    rm -f "$ROOT_DIR/userland/opengd-launch.sh"
    echo "[+] Done."
}

# ── Entry point ──────────────────────────────────────────────────────────────

case "${1:-build}" in
    build|"") build_opengd ;;
    clean)    do_clean ;;
    *)
        echo "Usage: $0 [build|clean]" >&2
        exit 1
        ;;
esac
